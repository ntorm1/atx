// atx-vol backtest SWAP LANE gates (Task 10).
//
// The engine's vol-derivative lane is ADDITIVE: a `PortfolioState::swap_lots`
// entry accrues its own realized-variance fixings off the daily snapshot spot,
// marks through `deriv_price` against the shifted surface, and cash-settles from
// the accrual at an exactly-observed expiry — all without touching a single
// number the option lane produces.
//
// Five gates:
//   1. VarSwapAccruesAndSettlesExactly       — hand-computed accrual + payoff.
//   2. OptionOnlyBookHasZeroSwapColumns      — zero-swap books: columns exactly
//                                              0.0 and NAV == Sigma pnl_total.
//   3. CheckpointResumeReproducesSwapMarks   — split run == one-shot run, bitwise.
//   4. DuplicateTimestampRefusedNotDoubleCounted — a replayed snapshot ts errors.
//   5. MissingSurfaceForSwapLotErrors        — a live swap lot fails closed.
//
// Fixture plumbing (synthetic eSSVI surfaces written as one-symbol archives per
// date) mirrors backtest_exec_test.cpp; the spot path is EXPLICIT here so the
// realized-variance arithmetic can be hand-computed exactly as
// `RealizedTracker.ObserveBatch_HandComputedThreeReturns` does.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"          // atx::core::Ok, ErrorCode
#include "atx/vol/american.hpp"        // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"        // Clock, run_backtest, SwapLot, RunConfig
#include "atx/vol/corpus.hpp"          // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/derivatives.hpp"     // DerivKind
#include "atx/vol/priced_surface.hpp"  // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"        // IStrategy, DeclarativeStrategy, StrategySpec
#include "atx/vol/surface_archive.hpp" // write_surface_archive_v2_file
#include "atx/vol/surface_parity.hpp"  // SliceContext
#include "atx/vol/types.hpp"           // Side, Result, Status
#include "atx/vol/vol_curve.hpp"       // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"     // EssviParams

using namespace atx::vol;
using atx::core::ErrorCode;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
// 30 calendar days between snapshots: the swap's residual T stays inside the
// synthetic surface's fitted pillar range [0.05, 1.0] on every marked step, so
// the strip prices real carry rather than a flat extrapolation.
constexpr std::int64_t kStepNs = 30LL * kDayNs;
constexpr std::uint32_t kUid = 7;
constexpr std::uint32_t kMissingUid = 4242; // never written into any archive

[[nodiscard]] bool bits_equal(double a, double b) noexcept {
  std::uint64_t ba = 0;
  std::uint64_t bb = 0;
  std::memcpy(&ba, &a, sizeof ba);
  std::memcpy(&bb, &b, sizeof bb);
  return ba == bb;
}

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors backtest_exec_test's make_surface.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, double fwd,
                                         std::int64_t now_ts) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    const double term_forward = fwd * std::exp((kR - 0.02) * T);
    EssviParams e{};
    e.theta = 0.04 + 0.005 * static_cast<double>(i);
    e.phi = 1.5 - 0.05 * static_cast<double>(i);
    e.rho = -0.4 + 0.02 * static_cast<double>(i);
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = term_forward;
    e.expiry_id = static_cast<std::uint16_t>(i);
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-kR * T)));
    ctx.push_back(SliceContext{T, term_forward, 0.0, 0.02, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = S;
  pc.r = kR;
  pc.now_ts_ns = now_ts;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  auto ps = PricedSurface::create(std::move(cs), std::move(ctx), pc);
  EXPECT_TRUE(ps.has_value()) << (ps.has_value() ? std::string{} : ps.error().to_string());
  return std::move(*ps);
}

[[nodiscard]] fs::path fresh_dir(const char *tag) {
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-btswap-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

// Write one surface as this date's archive; return its path (creating `dir`).
[[nodiscard]] std::string write_one(const fs::path &dir, const std::string &date,
                                    const std::string &symbol, const PricedSurface &s) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const SurfaceArchiveItem item{symbol, &s};
  const std::span<const SurfaceArchiveItem> items(&item, 1);
  const Status st = write_surface_archive_v2_file(path, items);
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

// Hand-build an Ok-only manifest over (date, archive_path) rows (one entry/date).
[[nodiscard]] CorpusManifest
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths,
              const std::string &symbol) {
  CorpusManifest m;
  for (const auto &[date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = symbol;
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

struct Corpus {
  CorpusManifest manifest;
  std::vector<std::pair<std::string, std::string>> dp; // (date, path), ascending
};

// One archive per entry of `spots`, snapshots kStepNs apart. The spot path is
// the caller's so the realized-variance accrual is hand-computable.
[[nodiscard]] Corpus make_spot_corpus(const fs::path &dir, const std::string &symbol,
                                      const std::vector<double> &spots) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < spots.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kStepNs;
    const PricedSurface s = make_surface(kUid, spots[d], spots[d], now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(d) + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_one(dir, date, symbol, s));
  }
  Corpus c;
  c.dp = std::move(dp);
  c.manifest = make_manifest(c.dp, symbol);
  return c;
}

// Opens exactly ONE swap lot at inception and never trades options. The lot's
// id is drawn from the engine's monotonic watermark, exactly as an option lot's
// would be.
class SwapOnlyStrategy : public IStrategy {
public:
  explicit SwapOnlyStrategy(SwapLot proto) noexcept : proto_{proto} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0) {
      SwapLot lot = proto_;
      lot.id = next_lot_id++;
      lot.start_ts_ns = base.ts_ns();
      book.swap_lots.push_back(lot);
    }
    return atx::core::Ok();
  }

private:
  SwapLot proto_;
};

// A single-clip option strategy: opens ONE structure at inception and holds it.
[[nodiscard]] StrategySpec single_clip(std::uint32_t uid, double target_T, Side side) {
  StrategySpec spec;
  spec.name = "single-clip";
  LegSpec leg;
  leg.uid = uid;
  leg.tenor.target_T = target_T;
  leg.structure.kind = StructureSpec::Kind::Single;
  leg.structure.single_side = side;
  leg.strike = StrikeSelector{StrikeSelector::Kind::AtmForward, 0.0};
  leg.size = SizeSpec{SizeSpec::Kind::FixedContracts, 1.0, +1.0};
  spec.legs.push_back(leg);
  spec.lifecycle.entry = LifecycleSpec::Entry::EveryNDays;
  spec.lifecycle.holding = LifecycleSpec::Holding::HoldToExpiry;
  spec.lifecycle.entry_every_n = 100000; // >> corpus => opens once, at inception
  return spec;
}

// The var-swap fixture shared by gates 1 and 3: spots [100, 101, 99, 102] with
// the lot opened at inception. The engine SEEDS the fixing series on the FIRST
// STEP's snapshot (the swap pass runs only inside the step loop), so the seed is
// spots[1] and the accrued returns are ln(spots[2]/spots[1]) and
// ln(spots[3]/spots[2]) — two returns, hence n_obs_total = 2.
constexpr double kAnnualization = 252.0;
constexpr double kStrikeDec = 0.04;
constexpr double kNotional = 1000.0;
constexpr double kQty = 2.0;

[[nodiscard]] SwapLot var_swap_proto(std::uint32_t uid, std::int64_t expiry_ts_ns,
                                     std::uint32_t n_obs_total) noexcept {
  SwapLot lot{};
  lot.uid = uid;
  lot.kind = DerivKind::VarSwap;
  lot.strike_dec = kStrikeDec;
  lot.cap_dec = 0.0;
  lot.notional = kNotional;
  lot.qty = kQty;
  lot.expiry_ts_ns = expiry_ts_ns;
  lot.n_obs_total = n_obs_total;
  lot.annualization = kAnnualization;
  return lot;
}

} // namespace

// ── 1. Accrual + settlement are exactly the hand computation ────────────────
TEST(BacktestSwap, VarSwapAccruesAndSettlesExactly) {
  const fs::path dir = fresh_dir("accrue");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 3LL * kStepNs; // exactly the last snapshot
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/2u)};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.size(), 4u);
  ASSERT_EQ(r.swap_pv.size(), r.size());
  ASSERT_EQ(r.swap_pnl.size(), r.size());

  // Hand computation over the two accrued returns.
  const double ra = std::log(spots[2] / spots[1]);
  const double rb = std::log(spots[3] / spots[2]);
  const double sum_sq = ra * ra + rb * rb;
  const double rv_dec = kAnnualization * sum_sq / 2.0;
  const double payoff = kQty * kNotional * (rv_dec - kStrikeDec);

  // Inception books no swap economics at all (swaps open at zero cost).
  EXPECT_TRUE(bits_equal(r.swap_pv[0], 0.0));
  EXPECT_TRUE(bits_equal(r.swap_pnl[0], 0.0));
  // The first mark carries the whole entry PV (prev_pv starts at zero).
  EXPECT_TRUE(bits_equal(r.swap_pnl[1], r.swap_pv[1]));
  EXPECT_NE(r.swap_pv[1], 0.0);
  EXPECT_NE(r.swap_pv[2], 0.0);
  // The lot settled and was erased: no live mark remains.
  EXPECT_TRUE(bits_equal(r.swap_pv[3], 0.0));

  // Settlement pays qty*notional*(rv_done - K) into the cash ledger, and nothing
  // else in this book touches cash (no option lots, no frictions).
  EXPECT_NEAR(r.cash.back(), payoff, 1.0e-9);
  // swap_pnl telescopes over the marks to exactly the settlement payoff.
  double swap_pnl_sum = 0.0;
  for (const double v : r.swap_pnl) {
    swap_pnl_sum += v;
  }
  EXPECT_NEAR(swap_pnl_sum, payoff, 1.0e-9);
  EXPECT_NEAR(r.nav.back(), payoff, 1.0e-9);

  // The accrual itself, read straight off a checkpoint after the seed + one
  // return: this is the RealizedTracker arithmetic, hand-computed.
  auto sub = clock->between(c.dp[0].first, c.dp[2].first);
  ASSERT_TRUE(sub.has_value()) << sub.error().to_string();
  SwapOnlyStrategy strat_sub{var_swap_proto(kUid, expiry, /*n_obs_total=*/2u)};
  auto cont = run_backtest_incremental(*sub, strat_sub, cfg, nullptr);
  ASSERT_TRUE(cont.has_value()) << cont.error().to_string();
  ASSERT_EQ(cont->checkpoint.swap_accruals.size(), 1u);
  ASSERT_EQ(cont->checkpoint.portfolio.swap_lots.size(), 1u);
  const SwapAccrual &acc = cont->checkpoint.swap_accruals.front();
  EXPECT_EQ(acc.lot_id, cont->checkpoint.portfolio.swap_lots.front().id);
  EXPECT_TRUE(acc.have_prev);
  EXPECT_EQ(acc.prev_ts_ns, kBaseNow + 2LL * kStepNs);
  EXPECT_EQ(acc.prev_spot, spots[2]);
  EXPECT_EQ(acc.rv.n_obs_done, 1u);
  EXPECT_EQ(acc.rv.n_obs_total, 2u);
  EXPECT_LT(std::fabs(acc.rv.sum_sq_log_returns_done - ra * ra), 1.0e-15);
  EXPECT_LT(std::fabs(acc.rv.rv_done_dec - kAnnualization * ra * ra), 1.0e-13);
}

// ── 2. Zero-swap books are untouched ───────────────────────────────────────
TEST(BacktestSwap, OptionOnlyBookHasZeroSwapColumns) {
  const fs::path dir = fresh_dir("optiononly");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0, 103.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  DeclarativeStrategy strat{single_clip(kUid, 0.75, Side::Put)};
  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.size(), 5u);
  ASSERT_EQ(r.swap_pv.size(), r.size());
  ASSERT_EQ(r.swap_pnl.size(), r.size());
  ASSERT_GT(r.n_open_lots.back(), 0.0); // the option lane really did run

  // The swap pass is skipped entirely on an empty swap book: both columns are
  // exactly +0.0 on every row, never a rounded-to-zero mark.
  for (std::size_t i = 0; i < r.size(); ++i) {
    EXPECT_TRUE(bits_equal(r.swap_pv[i], 0.0)) << i;
    EXPECT_TRUE(bits_equal(r.swap_pnl[i], 0.0)) << i;
  }

  // NAV identity: with a zero swap lane, `nav` is still exactly the running sum
  // of the per-step totals (step_total = pnl_total + settlement + shares +
  // swap + financing - cost, and `pnl_total` IS that sum at stride 1). Exact
  // double equality, not a tolerance — the only latitude is the sign of zero
  // (row 0 books `nav = -cost`, i.e. -0.0 in a frictionless run).
  double running = 0.0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    running += r.pnl_total[i];
    EXPECT_EQ(r.nav[i], running) << i;
  }
  EXPECT_EQ(r.nav.back(), running);
}

// ── 3. Checkpoint resume reproduces the swap marks bit-for-bit ─────────────
TEST(BacktestSwap, CheckpointResumeReproducesSwapMarks) {
  const fs::path dir = fresh_dir("resume");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0, 98.0, 104.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Seed at step 1, four accrued returns (steps 2..5), settling on the last one.
  const std::int64_t expiry = kBaseNow + 5LL * kStepNs;
  const SwapLot proto = var_swap_proto(kUid, expiry, /*n_obs_total=*/4u);
  RunConfig cfg;

  SwapOnlyStrategy one_shot_strat{proto};
  auto one_shot = run_backtest_incremental(*clock, one_shot_strat, cfg, nullptr);
  ASSERT_TRUE(one_shot.has_value()) << one_shot.error().to_string();
  const BacktestResult &full = one_shot->rows;
  ASSERT_EQ(full.size(), 6u);

  // Split: [d0..d3] then resume over [d3..d5].
  auto head_clock = clock->between(c.dp[0].first, c.dp[3].first);
  ASSERT_TRUE(head_clock.has_value()) << head_clock.error().to_string();
  auto tail_clock = clock->between(c.dp[3].first, c.dp[5].first);
  ASSERT_TRUE(tail_clock.has_value()) << tail_clock.error().to_string();

  SwapOnlyStrategy head_strat{proto};
  auto head = run_backtest_incremental(*head_clock, head_strat, cfg, nullptr);
  ASSERT_TRUE(head.has_value()) << head.error().to_string();
  ASSERT_EQ(head->rows.size(), 4u);
  ASSERT_EQ(head->checkpoint.swap_accruals.size(), 1u);

  SwapOnlyStrategy tail_strat{proto};
  auto tail = run_backtest_incremental(*tail_clock, tail_strat, cfg, &head->checkpoint);
  ASSERT_TRUE(tail.has_value()) << tail.error().to_string();
  ASSERT_EQ(tail->rows.size(), 2u); // resumed runs omit the anchor row

  // Head rows reproduce the one-shot prefix; the resumed rows reproduce the
  // suffix — swap marks, swap PnL and NAV all bitwise.
  for (std::size_t i = 0; i < head->rows.size(); ++i) {
    EXPECT_TRUE(bits_equal(head->rows.swap_pv[i], full.swap_pv[i])) << i;
    EXPECT_TRUE(bits_equal(head->rows.swap_pnl[i], full.swap_pnl[i])) << i;
    EXPECT_TRUE(bits_equal(head->rows.nav[i], full.nav[i])) << i;
  }
  for (std::size_t i = 0; i < tail->rows.size(); ++i) {
    const std::size_t j = i + 4u;
    EXPECT_TRUE(bits_equal(tail->rows.swap_pv[i], full.swap_pv[j])) << i;
    EXPECT_TRUE(bits_equal(tail->rows.swap_pnl[i], full.swap_pnl[j])) << i;
    EXPECT_TRUE(bits_equal(tail->rows.nav[i], full.nav[j])) << i;
  }
  EXPECT_TRUE(bits_equal(tail->checkpoint.nav, one_shot->checkpoint.nav));
  EXPECT_TRUE(bits_equal(tail->checkpoint.cash, one_shot->checkpoint.cash));
  // The lot settled on the final step in both runs.
  EXPECT_TRUE(tail->checkpoint.portfolio.swap_lots.empty());
  EXPECT_TRUE(tail->checkpoint.swap_accruals.empty());
  EXPECT_TRUE(one_shot->checkpoint.portfolio.swap_lots.empty());
  EXPECT_TRUE(bits_equal(full.swap_pv.back(), 0.0));
}

// ── 4. A replayed snapshot timestamp is refused, never double-counted ──────
TEST(BacktestSwap, DuplicateTimestampRefusedNotDoubleCounted) {
  const fs::path dir = fresh_dir("duplicate");
  const std::vector<double> spots = {100.0, 101.0};
  Corpus c = make_spot_corpus(dir, "SPX", spots);
  // A third clock ref pointing at the SECOND date's archive: a distinct clock
  // date replaying an already-consumed fixing timestamp.
  c.dp.emplace_back("2026-08-03", c.dp[1].second);
  c.manifest = make_manifest(c.dp, "SPX");
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  ASSERT_EQ(clock->size(), 3u);

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs; // far beyond the corpus
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/8u)};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
  // Right reason: the refusal is the swap lane's fixing guard, and it aborts the
  // run rather than accruing the replayed date a second time.
  EXPECT_NE(result.error().message().find("duplicate/backdated swap fixing"), std::string::npos)
      << result.error().to_string();
}

// ── 5. A live swap lot with no surface fails closed ────────────────────────
TEST(BacktestSwap, MissingSurfaceForSwapLotErrors) {
  const fs::path dir = fresh_dir("nosurface");
  const std::vector<double> spots = {100.0, 101.0, 99.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs; // far beyond the corpus
  SwapOnlyStrategy strat{var_swap_proto(kMissingUid, expiry, /*n_obs_total=*/8u)};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
  EXPECT_NE(result.error().message().find("no surface for swap lot"), std::string::npos)
      << result.error().to_string();
}
