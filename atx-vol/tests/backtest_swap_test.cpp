// atx-vol backtest SWAP LANE gates (Task 10).
//
// The engine's vol-derivative lane is ADDITIVE: a `PortfolioState::swap_lots`
// entry accrues its own realized-variance fixings off the daily snapshot spot,
// marks through `deriv_price` against the shifted surface, and cash-settles from
// the accrual at an exactly-observed expiry — all without touching a single
// number the option lane produces.
//
// Gates:
//   1. VarSwapAccruesAndSettlesExactly       — hand-computed accrual + payoff.
//   1b. DailySwapMarksMatchIndependentDerivPriceOracle — each LIVE daily mark
//                                              against a hand-built contract.
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

#include <bit> // Task F-8: the NAV gate compares on the bits, not near-equal
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"                  // atx::core::Ok, ErrorCode
#include "atx/vol/american.hpp"                // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"                // Clock, run_backtest, SwapLot, RunConfig
#include "atx/vol/backtest_db.hpp"             // append_backtest_results
#include "atx/vol/corpus.hpp"                  // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/derivatives.hpp"             // DerivKind, DerivContract, DerivConfig, DerivQuote
#include "atx/vol/detail/deriv_ref_bridge.hpp" // detail::deriv_price_on_ref
#include "atx/vol/portfolio_pricer.hpp"        // kNsPerYear, SurfaceRef
#include "atx/vol/priced_surface.hpp"          // PricedSurface, PricingContext
#include "atx/vol/strategy.hpp"                // IStrategy, DeclarativeStrategy, StrategySpec
#include "atx/vol/surface_archive.hpp"         // write_surface_archive_v2_file
#include "atx/vol/surface_parity.hpp"          // SliceContext
#include "atx/vol/types.hpp"                   // Side, Result, Status
#include "atx/vol/vol_curve.hpp"               // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"             // EssviParams

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
//
// `dark_from` is the first date index whose archive is written under a DIFFERENT
// uid/symbol, i.e. the date the swap's name goes dark. Defaults past the end.
[[nodiscard]] Corpus make_spot_corpus(const fs::path &dir, const std::string &symbol,
                                      const std::vector<double> &spots,
                                      std::size_t dark_from = static_cast<std::size_t>(-1)) {
  std::vector<std::pair<std::string, std::string>> dp;
  for (std::size_t d = 0; d < spots.size(); ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kStepNs;
    const bool dark = d >= dark_from;
    const PricedSurface s = make_surface(dark ? kMissingUid : kUid, spots[d], spots[d], now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(d) + 1);
    const std::string date = buf;
    dp.emplace_back(date, write_one(dir, date, dark ? "OTHER" : symbol, s));
  }
  Corpus c;
  c.dp = std::move(dp);
  c.manifest = make_manifest(c.dp, symbol);
  return c;
}

// Task F-8 fix round 2 (C-1): the same corpus on an IRREGULAR timeline.
//
// `make_spot_corpus` places every snapshot `kStepNs` apart, so `dt_this /
// dt_prev == 1` on every step of every fixture built from it. That single
// uniformity is what hid C-1 — a carry column mis-scaled by exactly that ratio —
// through five explain tests, a NAV gate, and a review. A real trading calendar
// is not uniform: a Friday-to-Monday step follows a one-day step, which is the
// 3x case.
//
// `gaps[d]` is the spacing in DAYS from snapshot d-1 to snapshot d (the first
// entry is ignored). Dates are still emitted in ascending order and still one
// archive per spot, so everything else about the fixture is unchanged and any
// difference between the two corpora is attributable to step length alone.
[[nodiscard]] Corpus make_irregular_spot_corpus(const fs::path &dir, const std::string &symbol,
                                                const std::vector<double> &spots,
                                                const std::vector<int> &gaps) {
  EXPECT_EQ(spots.size(), gaps.size());
  std::vector<std::pair<std::string, std::string>> dp;
  std::int64_t now = kBaseNow;
  int day_of_month = 1;
  for (std::size_t d = 0; d < spots.size(); ++d) {
    if (d > 0) {
      now += static_cast<std::int64_t>(gaps[d]) * kDayNs;
      day_of_month += gaps[d];
    }
    const PricedSurface s = make_surface(kUid, spots[d], spots[d], now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", day_of_month);
    dp.emplace_back(std::string(buf), write_one(dir, buf, symbol, s));
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

// Opens one swap lot at inception, then MISBEHAVES at `bad_step` in one of the
// ways the transition check must reject. Each mode is a separate gate.
class MisbehavingSwapStrategy : public IStrategy {
public:
  enum class Mode { EraseLot, MutateStrike, IdBelowWatermark };

  MisbehavingSwapStrategy(SwapLot proto, Mode mode, std::size_t bad_step) noexcept
      : proto_{proto}, mode_{mode}, bad_step_{bad_step} {}

  Status on_step(const MarketSnapshot &base, std::size_t step_index, PortfolioState &book,
                 std::uint64_t &next_lot_id) override {
    if (step_index == 0) {
      SwapLot lot = proto_;
      lot.id = next_lot_id++;
      lot.start_ts_ns = base.ts_ns();
      book.swap_lots.push_back(lot);
      return atx::core::Ok();
    }
    if (step_index != bad_step_) {
      return atx::core::Ok();
    }
    switch (mode_) {
    case Mode::EraseLot:
      book.swap_lots.clear(); // an "early close" the engine has no price for
      break;
    case Mode::MutateStrike:
      if (!book.swap_lots.empty()) {
        book.swap_lots.front().strike_dec += 0.01; // restrike in place
      }
      break;
    case Mode::IdBelowWatermark: {
      SwapLot lot = proto_;
      lot.id = 1u; // already issued at inception — a reused id
      lot.start_ts_ns = base.ts_ns();
      book.swap_lots.push_back(lot);
      break;
    }
    }
    return atx::core::Ok();
  }

private:
  SwapLot proto_;
  Mode mode_;
  std::size_t bad_step_;
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

// A minimally-valid hand-built BacktestResult covering rows
// [first_index, first_index + n_rows): every mandatory series column sized to
// the row count, with strictly ascending dates AND timestamps derived from the
// absolute row index so two results concatenate in order (which is exactly what
// `append_backtest_results` validates). Optionally carries the swap lane filled
// with `swap_value`.
[[nodiscard]] BacktestResult make_result(std::size_t first_index, std::size_t n_rows, bool swap,
                                         double swap_value = 0.0) {
  BacktestResult r;
  const std::size_t n = n_rows;
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t idx = first_index + i;
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(idx) + 1);
    r.date.emplace_back(buf);
    r.ts_ns.push_back(kBaseNow + static_cast<std::int64_t>(idx) * kDayNs);
  }
  // The 25 mandatory series columns (kBacktestSeriesColumns order). Spelled with
  // NON-const member pointers so the fill needs no const_cast; a column added to
  // the frozen registry without being added here shows up immediately as
  // `append_backtest_results` rejecting the fixture on a row-count mismatch.
  std::vector<double> BacktestResult::*const mandatory[] = {&BacktestResult::pnl_total,
                                                            &BacktestResult::pnl_delta,
                                                            &BacktestResult::pnl_gamma,
                                                            &BacktestResult::pnl_vega,
                                                            &BacktestResult::pnl_vanna,
                                                            &BacktestResult::pnl_volga,
                                                            &BacktestResult::pnl_theta,
                                                            &BacktestResult::pnl_rho,
                                                            &BacktestResult::pnl_charm,
                                                            &BacktestResult::pnl_unexplained,
                                                            &BacktestResult::pnl_settlement,
                                                            &BacktestResult::pnl_shares,
                                                            &BacktestResult::financing,
                                                            &BacktestResult::cost,
                                                            &BacktestResult::nav,
                                                            &BacktestResult::cash,
                                                            &BacktestResult::gross_delta,
                                                            &BacktestResult::gross_gamma,
                                                            &BacktestResult::gross_vega,
                                                            &BacktestResult::gross_theta,
                                                            &BacktestResult::turnover_notional,
                                                            &BacktestResult::turnover_vega,
                                                            &BacktestResult::n_open_lots,
                                                            &BacktestResult::n_unpriced_lots,
                                                            &BacktestResult::n_unpriced_greeks};
  for (std::vector<double> BacktestResult::*const member : mandatory) {
    (r.*member).assign(n, 0.0);
  }
  if (swap) {
    r.swap_pv.assign(n, swap_value);
    r.swap_pnl.assign(n, swap_value);
  }
  return r;
}

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

// The INDEPENDENT oracle for one live daily swap mark.
//
// Built from the engine's DOCUMENTED conventions, never from `step_swap_lots`:
//   * residual tenor = (expiry_ts_ns - snapshot_ts_ns) / kNsPerYear, in YEARS
//     (portfolio_pricer.hpp's Julian-year constant, the one every residual-T
//     site in the engine divides by);
//   * `rv_spec` = the accrual state AFTER this snapshot's own fixing, per
//     SwapAccrual's transcribed RealizedTracker arithmetic:
//     rv_done_dec = annualization * Sigma r^2 / n_done (0 before any return);
//   * every other field is the lot's own term, unscaled.
//
// It is priced through the SAME entry the mark lane prices through, and the
// mark is qty-scaled the same way, so the ONLY thing this can disagree with the
// engine about is how a `SwapLot` becomes a `DerivContract` — which is exactly
// the divergence a telescoping-sum gate is blind to.
[[nodiscard]] double reference_swap_mark(const PricedSurface &surface, std::int64_t ts_ns,
                                         const SwapLot &lot, std::uint32_t n_obs_done,
                                         double sum_sq_log_returns_done) {
  RealizedVarianceSpec rv{};
  rv.annualization = lot.annualization;
  rv.n_obs_total = lot.n_obs_total;
  rv.n_obs_done = n_obs_done;
  rv.sum_sq_log_returns_done = sum_sq_log_returns_done;
  rv.rv_done_dec = n_obs_done == 0u ? 0.0
                                    : lot.annualization * sum_sq_log_returns_done /
                                          static_cast<double>(n_obs_done);

  DerivContract contract;
  contract.kind = lot.kind;
  contract.maturity_t = static_cast<double>(lot.expiry_ts_ns - ts_ns) / kNsPerYear;
  contract.strike_dec = lot.strike_dec;
  contract.cap_dec = lot.cap_dec;
  contract.notional = lot.notional;
  contract.rv_spec = rv;

  const Result<DerivQuote> quote =
      detail::deriv_price_on_ref(SurfaceRef{&surface}, contract, DerivConfig{});
  EXPECT_TRUE(quote.has_value()) << (quote.has_value() ? std::string{} : quote.error().to_string());
  if (!quote.has_value()) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return lot.qty * quote->pv;
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

// ── 1b. Every LIVE daily mark is the independently-priced value ─────────────
//
// Gate 1's telescoping identity (Sigma swap_pnl == settlement payoff) is BLIND
// to a wrong daily mark: consecutive marks cancel by construction, so a units
// slip in the residual tenor, a stale/mis-staged `rv_spec` snapshot, or a
// mis-wired `DerivContract` field would leave that sum — and the settlement —
// exactly right while distorting every intermediate NAV row. This pins the
// marks themselves against a contract rebuilt from first principles.
TEST(BacktestSwap, DailySwapMarksMatchIndependentDerivPriceOracle) {
  const fs::path dir = fresh_dir("markoracle");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 3LL * kStepNs; // exactly the last snapshot
  const SwapLot proto = var_swap_proto(kUid, expiry, /*n_obs_total=*/2u);
  SwapOnlyStrategy strat{proto};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.size(), 4u);
  ASSERT_EQ(r.swap_pv.size(), r.size());

  // Row 1 is the SEED step — the swap pass first sees the lot here, so its
  // fixing series is seeded at spots[1] with NOTHING accrued (n_done = 0,
  // Sigma r^2 = 0). The mark is the pure future leg over a 60-day residual.
  const std::int64_t ts1 = kBaseNow + 1LL * kStepNs;
  const PricedSurface surface1 = make_surface(kUid, spots[1], spots[1], ts1);
  const double ref1 = reference_swap_mark(surface1, ts1, proto, /*n_obs_done=*/0u,
                                          /*sum_sq_log_returns_done=*/0.0);

  // Row 2 carries ONE accrued return, so it exercises the aged blend: the mark
  // must read the accrual as of THIS snapshot's own fixing — not the previous
  // step's (stale) and not the terminal one's (look-ahead) — over a 30-day
  // residual, i.e. half of row 1's. A units error in either quantity moves the
  // two rows differently and cannot hide.
  const std::int64_t ts2 = kBaseNow + 2LL * kStepNs;
  const double ra = std::log(spots[2] / spots[1]);
  const PricedSurface surface2 = make_surface(kUid, spots[2], spots[2], ts2);
  const double ref2 = reference_swap_mark(surface2, ts2, proto, /*n_obs_done=*/1u,
                                          /*sum_sq_log_returns_done=*/ra * ra);

  // A zero reference would make the comparison vacuous; both legs must price.
  ASSERT_NE(ref1, 0.0);
  ASSERT_NE(ref2, 0.0);
  ASSERT_EQ(ref1, ref1); // not NaN: the oracle's own deriv_price call succeeded
  ASSERT_EQ(ref2, ref2);

  // Both sides run the identical strip/quadrature on the identical surface, so
  // this is an EXACT double comparison, not a tolerance.
  EXPECT_EQ(r.swap_pv[1], ref1);
  EXPECT_EQ(r.swap_pv[2], ref2);
  // The first mark carries the whole entry PV, so swap_pnl[1] is pinned too;
  // row 2's increment is then pinned by both marks being pinned.
  EXPECT_EQ(r.swap_pnl[1], ref1);
  EXPECT_EQ(r.swap_pnl[2], ref2 - ref1);
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
  EXPECT_NE(result.error().message().find("no surface for held swap lot"), std::string::npos)
      << result.error().to_string();
}

// ── 5b. The EXPIRY-DAY surface is required too: the close is the last fixing ──
TEST(BacktestSwap, MissingSurfaceOnExpiryDayErrors) {
  const fs::path dir = fresh_dir("nosurface-expiry");
  // The name goes dark on the LAST date — which is this lot's expiry. The lot
  // marked fine on every earlier step, so only the terminal fixing is missing;
  // settling anyway would divide a short Sigma r^2 by a short denominator.
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots, /*dark_from=*/3u);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 3LL * kStepNs; // the dark date
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/2u)};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
  EXPECT_NE(result.error().message().find("no surface for settling swap lot"), std::string::npos)
      << result.error().to_string();
}

// ── 6. A lot that reaches expiry with no observed return must not settle ─────
TEST(BacktestSwap, ZeroObservationSettlementErrors) {
  const fs::path dir = fresh_dir("zeroobs");
  const std::vector<double> spots = {100.0, 101.0, 99.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // Expiry on refs[1] — the very step that first SEES the lot, so its fixing
  // series is seeded and immediately terminal with zero accrued returns. The
  // estimator would be a fabricated 0.0 paying away the whole strike.
  const std::int64_t expiry = kBaseNow + 1LL * kStepNs;
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/1u)};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::NotFound);
  EXPECT_NE(result.error().message().find("no observed return"), std::string::npos)
      << result.error().to_string();
}

// ── 7. Transition guards: a strategy may not erase or mutate a swap lot ──────
TEST(BacktestSwap, StrategyErasingSwapLotErrors) {
  const fs::path dir = fresh_dir("erase");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs; // survives the corpus
  MisbehavingSwapStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/8u),
                                MisbehavingSwapStrategy::Mode::EraseLot, /*bad_step=*/2u};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("erased by strategy"), std::string::npos)
      << result.error().to_string();
}

TEST(BacktestSwap, StrategyMutatingSurvivingSwapLotErrors) {
  const fs::path dir = fresh_dir("mutate");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs;
  MisbehavingSwapStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/8u),
                                MisbehavingSwapStrategy::Mode::MutateStrike, /*bad_step=*/2u};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("mutated surviving swap lot"), std::string::npos)
      << result.error().to_string();
}

TEST(BacktestSwap, ReusedSwapLotIdErrors) {
  const fs::path dir = fresh_dir("reuseid");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 9LL * kStepNs;
  MisbehavingSwapStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/8u),
                                MisbehavingSwapStrategy::Mode::IdBelowWatermark, /*bad_step=*/2u};

  RunConfig cfg;
  auto result = run_backtest(*clock, strat, cfg);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().message().find("duplicate swap lot id"), std::string::npos)
      << result.error().to_string();
}

// ── 8. append_backtest_results must not launder the swap lane away ───────────
TEST(BacktestSwap, AppendRefusesToLaunderSwapLaneAway) {
  // The DB never persists the swap lane, so `append_backtest_results` sees a
  // decoded prefix (lane ABSENT) meeting a fresh continuation (lane PRESENT).
  // Collapsing that to "absent" is only safe when neither side has real swap
  // data — otherwise it would both destroy the history and disarm the store
  // guard, which only ever inspects the combined result.
  const BacktestResult empty_lane_a = make_result(/*first_index=*/0u, /*n_rows=*/2u,
                                                  /*swap=*/false);
  const BacktestResult empty_lane_b = make_result(2u, 1u, /*swap=*/false);
  const BacktestResult zero_lane_b = make_result(2u, 1u, /*swap=*/true, /*swap_value=*/0.0);
  const BacktestResult live_lane_b = make_result(2u, 1u, /*swap=*/true, /*swap_value=*/12.5);

  // (1) Both sides lack the lane => plain append, lane stays absent.
  BacktestResult both_empty = empty_lane_a;
  ASSERT_TRUE(append_backtest_results(both_empty, empty_lane_b).has_value());
  EXPECT_EQ(both_empty.size(), 3u);
  EXPECT_TRUE(both_empty.swap_pv.empty());

  // (2) Shapes differ but the populated side is all-zero (the real DB-extension
  //     case) => collapse to absent, and stay row-consistent.
  BacktestResult mixed_zero = empty_lane_a;
  ASSERT_TRUE(append_backtest_results(mixed_zero, zero_lane_b).has_value());
  EXPECT_EQ(mixed_zero.size(), 3u);
  EXPECT_TRUE(mixed_zero.swap_pv.empty());
  EXPECT_TRUE(mixed_zero.swap_pnl.empty());

  // (3) Shapes differ AND one side carries real swap PnL => refuse.
  BacktestResult mixed_live = empty_lane_a;
  const Status refused = append_backtest_results(mixed_live, live_lane_b);
  ASSERT_FALSE(refused.has_value());
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(refused.error().message().find("swap-lane shape change"), std::string::npos)
      << refused.error().to_string();

  // (4) Both sides carry the lane => genuine concatenation, nothing dropped.
  BacktestResult both_live = make_result(0u, 1u, /*swap=*/true, /*swap_value=*/3.0);
  ASSERT_TRUE(append_backtest_results(both_live, live_lane_b).has_value());
  ASSERT_EQ(both_live.swap_pnl.size(), 2u);
  EXPECT_EQ(both_live.swap_pnl[0], 3.0);
  EXPECT_EQ(both_live.swap_pnl[1], 12.5);
}

// ── Task F-8 S4: the swap lane's P&L explain ────────────────────────────────
//
// `RunConfig::swap_pnl_explain` adds seven attribution columns and an
// unattributed counter beside `swap_pnl`. The gates below are, in order of what
// they protect: the sprint's NAV invariant, the identity the columns exist to
// satisfy, its survival under downsampling, and the "empty, not zero" contract.

// THE NAV GATE. The sprint treats an unexplained backtest NAV move as
// stop-the-sprint, so this is the first thing the feature has to prove -- and it
// proves it with the flag ON, not merely off. An opt-in diagnostic that
// perturbed the run when enabled would be worse than no diagnostic: it would
// make every measurement taken with it untrustworthy.
//
// Asserted on the BITS of every NAV-lane column, not near-equal. The explain
// only ever READS (one extra `deriv_greeks_on_ref` and three surface reads per
// live lot per step); if any of it ever fed back into a mark, this is where that
// shows up.
TEST(BacktestSwapExplain, NavIsUnmovedByTheExplain) {
  const fs::path dir = fresh_dir("explain_nav");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0, 101.5, 103.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 5LL * kStepNs;
  SwapOnlyStrategy off_strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/4u)};
  SwapOnlyStrategy on_strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/4u)};

  RunConfig off_cfg;
  const auto off = run_backtest(*clock, off_strat, off_cfg);
  ASSERT_TRUE(off.has_value()) << off.error().to_string();

  RunConfig on_cfg;
  on_cfg.swap_pnl_explain = true;
  const auto on = run_backtest(*clock, on_strat, on_cfg);
  ASSERT_TRUE(on.has_value()) << on.error().to_string();

  ASSERT_EQ(off->size(), on->size());
  ASSERT_GT(off->size(), 1u);
  const auto bits = [](double v) { return std::bit_cast<std::uint64_t>(v); };
  for (std::size_t i = 0; i < off->size(); ++i) {
    EXPECT_EQ(bits(off->nav[i]), bits(on->nav[i])) << "nav row " << i;
    EXPECT_EQ(bits(off->cash[i]), bits(on->cash[i])) << "cash row " << i;
    EXPECT_EQ(bits(off->pnl_total[i]), bits(on->pnl_total[i])) << "pnl_total row " << i;
    EXPECT_EQ(bits(off->swap_pv[i]), bits(on->swap_pv[i])) << "swap_pv row " << i;
    EXPECT_EQ(bits(off->swap_pnl[i]), bits(on->swap_pnl[i])) << "swap_pnl row " << i;
    EXPECT_EQ(bits(off->pnl_settlement[i]), bits(on->pnl_settlement[i]))
        << "pnl_settlement row " << i;
  }
  // And the flag really did something, or the comparison above is vacuous.
  ASSERT_EQ(on->swap_explain_residual.size(), on->size());
  EXPECT_TRUE(off->swap_explain_residual.empty());
}

// THE IDENTITY, row by row. `swap_pnl` is the number being explained, so the
// seven components must sum to it exactly -- to floating-point rounding on the
// per-lot sums, not to a modelling tolerance, because `residual` is defined as
// whatever is left over and therefore absorbs every real modelling gap.
//
// A test that only checked "the columns are finite" would pass on an explain
// that attributed nothing; the `unattributed` column is asserted too, so a run
// that silently gave up on every lot cannot masquerade as an explained one.
TEST(BacktestSwapExplain, ComponentsSumToSwapPnlOnEveryRow) {
  const fs::path dir = fresh_dir("explain_identity");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0, 101.5, 103.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 5LL * kStepNs;
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/4u)};

  RunConfig cfg;
  cfg.swap_pnl_explain = true;
  const auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;

  ASSERT_EQ(r.swap_explain_carry.size(), r.size());
  ASSERT_EQ(r.swap_explain_unattributed.size(), r.size());

  double total_unattributed = 0.0;
  for (std::size_t i = 0; i < r.size(); ++i) {
    const double summed = r.swap_explain_carry[i] + r.swap_explain_realized[i] +
                          r.swap_explain_vol_level[i] + r.swap_explain_skew[i] +
                          r.swap_explain_convexity[i] + r.swap_explain_discount[i] +
                          r.swap_explain_residual[i];
    EXPECT_TRUE(std::isfinite(summed)) << "row " << i;
    // The scale is the row's own move; an absolute floor keeps a row whose move
    // is legitimately ~0 from being compared relatively against itself.
    const double tol = 1.0e-9 * std::abs(r.swap_pnl[i]) + 1.0e-6;
    EXPECT_NEAR(summed, r.swap_pnl[i], tol)
        << "row " << i << " components=" << summed << " swap_pnl=" << r.swap_pnl[i];
    total_unattributed += r.swap_explain_unattributed[i];
  }

  // Rows 0 (inception) and 1 (the lot's first mark, no prior state) are
  // unattributed by construction, and the expiry row settles. Everything in
  // between must actually attribute, or this feature explains nothing.
  EXPECT_GT(r.size(), 3u);
  bool any_attributed = false;
  for (std::size_t i = 2; i + 1 < r.size(); ++i) {
    any_attributed = any_attributed || (r.swap_explain_unattributed[i] == 0.0);
  }
  EXPECT_TRUE(any_attributed) << "every step was unattributed; total=" << total_unattributed;

  // Carry is the deterministic "nothing happened" leg and is the largest named
  // component on a swap that is not moving much -- a sign flip here would still
  // satisfy the identity above, because the residual would absorb it.
  bool any_carry = false;
  for (std::size_t i = 0; i < r.size(); ++i) {
    any_carry = any_carry || (r.swap_explain_carry[i] != 0.0);
  }
  EXPECT_TRUE(any_carry) << "carry was zero on every row";
}

// The identity has to survive DOWNSAMPLING, which is the whole reason every
// component is a flow. At `record_every_n > 1` each recorded row is a block sum;
// if any component were accumulated as state (like `swap_pv` legitimately is)
// its column would report a level against six summed flows and the identity
// would silently stop closing.
TEST(BacktestSwapExplain, IdentitySurvivesRecordEveryN) {
  const fs::path dir = fresh_dir("explain_stride");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0, 101.5, 103.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 6LL * kStepNs;
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/5u)};

  RunConfig cfg;
  cfg.swap_pnl_explain = true;
  cfg.record_every_n = 2;
  const auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.swap_explain_residual.size(), r.size());

  for (std::size_t i = 0; i < r.size(); ++i) {
    const double summed = r.swap_explain_carry[i] + r.swap_explain_realized[i] +
                          r.swap_explain_vol_level[i] + r.swap_explain_skew[i] +
                          r.swap_explain_convexity[i] + r.swap_explain_discount[i] +
                          r.swap_explain_residual[i];
    const double tol = 1.0e-9 * std::abs(r.swap_pnl[i]) + 1.0e-6;
    EXPECT_NEAR(summed, r.swap_pnl[i], tol) << "downsampled row " << i;
  }
}

// EMPTY, not zero-filled, when the flag is off -- the same distinction
// `nav_liquidation` makes, and the one that lets a reader tell "not measured"
// from "measured as flat". `validate()` accepts both shapes and rejects a
// partially-filled one.
TEST(BacktestSwapExplain, ColumnsAreEmptyRatherThanZeroFilledWhenOff) {
  const fs::path dir = fresh_dir("explain_off");
  const std::vector<double> spots = {100.0, 101.0, 99.0, 102.0};
  const Corpus c = make_spot_corpus(dir, "SPX", spots);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 3LL * kStepNs;
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/2u)};

  RunConfig cfg;  // flag OFF (the default)
  const auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  EXPECT_TRUE(result->swap_explain_carry.empty());
  EXPECT_TRUE(result->swap_explain_realized.empty());
  EXPECT_TRUE(result->swap_explain_vol_level.empty());
  EXPECT_TRUE(result->swap_explain_skew.empty());
  EXPECT_TRUE(result->swap_explain_convexity.empty());
  EXPECT_TRUE(result->swap_explain_discount.empty());
  EXPECT_TRUE(result->swap_explain_residual.empty());
  EXPECT_TRUE(result->swap_explain_unattributed.empty());
  // But the run is still a valid, shape-checked result.
  EXPECT_TRUE(result->validate().has_value());

  // A hand-built result with a half-length explain column is refused, which is
  // what registering them in `validate()` buys.
  BacktestResult broken = *result;
  broken.swap_explain_carry.assign(broken.size() - 1u, 0.0);
  EXPECT_FALSE(broken.validate().has_value());
}

// The explain columns concatenate or clear on THEIR OWN shape test, not the
// swap lane's -- they are opt-in, so a stored prefix without them meeting a
// continuation with them is an ordinary shape change.
//
// FIX ROUND 2 (I-3): the collapse is legal ONLY when neither side has
// attribution to lose. Round 1 implemented the collapse without that condition,
// where the sibling swap-lane rule thirty lines above refuses exactly the same
// thing -- and this test pinned the loss as if it were intended, asserting that
// a column carrying 1.0 came back empty. A test that pins a defect is worse than
// no test, because it converts the next person's correct fix into a red suite.
// It now pins the refusal, and the collapse is checked on the all-zero case
// where it is genuinely lossless.
TEST(BacktestSwapExplain, AppendRefusesToDiscardExplainAcrossAShapeChange) {
  BacktestResult a = make_result(/*first_index=*/0u, /*n_rows=*/2u, /*swap=*/true,
                                 /*swap_value=*/0.0);
  BacktestResult b = make_result(2u, 1u, /*swap=*/true, /*swap_value=*/0.0);
  a.swap_explain_carry.assign(a.size(), 1.0);
  a.swap_explain_realized.assign(a.size(), 0.0);
  a.swap_explain_vol_level.assign(a.size(), 0.0);
  a.swap_explain_skew.assign(a.size(), 0.0);
  a.swap_explain_convexity.assign(a.size(), 0.0);
  a.swap_explain_discount.assign(a.size(), 0.0);
  a.swap_explain_residual.assign(a.size(), 0.0);
  a.swap_explain_unattributed.assign(a.size(), 0.0);
  // `b` carries no explain columns: one side has them, the other does not.

  // `a` carries REAL attribution (a 1.0) and `b` carries none. Collapsing would
  // discard it silently, so this must be refused rather than performed.
  BacktestResult combined = a;
  const Status refused = append_backtest_results(combined, b);
  ASSERT_FALSE(refused.has_value())
      << "appending across an explain shape change while one side carries attribution "
         "would discard it; the swap lane refuses the same shape thirty lines up";
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(refused.error().message().find("swap-explain shape change"), std::string::npos)
      << refused.error().to_string();

  // The lossless case: same shape change, but no side carries attribution. That
  // IS the DB-extension case (a decoded prefix reports the explain absent, a
  // fresh continuation reports it present and all-zero) and collapsing to absent
  // loses nothing.
  BacktestResult zeroed = a;
  for (const BacktestExplainColumn &column : swap_explain_columns()) {
    (zeroed.*(column.member)).assign(zeroed.size(), 0.0);
  }
  BacktestResult collapsed = zeroed;
  ASSERT_TRUE(append_backtest_results(collapsed, b).has_value());
  EXPECT_EQ(collapsed.size(), 3u);
  EXPECT_TRUE(collapsed.swap_explain_carry.empty())
      << "a half-present all-zero explain collapses to absent, not to a ragged column";
  EXPECT_TRUE(collapsed.swap_explain_residual.empty());
  EXPECT_TRUE(collapsed.validate().has_value());

  // Both sides present => genuine concatenation, nothing dropped.
  BacktestResult b_full = b;
  b_full.swap_explain_carry.assign(b_full.size(), 2.0);
  b_full.swap_explain_realized.assign(b_full.size(), 0.0);
  b_full.swap_explain_vol_level.assign(b_full.size(), 0.0);
  b_full.swap_explain_skew.assign(b_full.size(), 0.0);
  b_full.swap_explain_convexity.assign(b_full.size(), 0.0);
  b_full.swap_explain_discount.assign(b_full.size(), 0.0);
  b_full.swap_explain_residual.assign(b_full.size(), 0.0);
  b_full.swap_explain_unattributed.assign(b_full.size(), 0.0);

  BacktestResult both = a;
  ASSERT_TRUE(append_backtest_results(both, b_full).has_value());
  ASSERT_EQ(both.swap_explain_carry.size(), 3u);
  EXPECT_EQ(both.swap_explain_carry[0], 1.0);
  EXPECT_EQ(both.swap_explain_carry[2], 2.0);
  EXPECT_TRUE(both.validate().has_value());
}


// ── C-1: carry on an irregular calendar ─────────────────────────────────────
//
// `theta_zero_fixing` is a RATE from a roll of `bumps.time_years` carrying
// exactly ONE injected fixing, so `theta_zero_fixing * h` is the deterministic
// move over `h` INCLUDING that fixing. Round 1 resolved the greeks one step
// early and multiplied them by the NEXT step's `dt`, scaling the fixing leg by
// `dt_this / dt_prev`. Every committed fixture used `make_spot_corpus`, whose
// snapshots are uniformly `kStepNs` apart, so that ratio was 1.000 everywhere
// and nothing could see it. The residual absorbed the error, the identity
// closed, and NAV never moved.
//
// The fix removes the coupling rather than correcting for it: start-of-step
// sensitivities now resolve against `base`, so the interval the greek is bumped
// over and the interval it is multiplied by are the same number by construction.
//
// This fixture is the other half. A 1/3/1/7/1-day gap sequence puts
// `dt_this/dt_prev` at 3, 1/3, 7 and 1/7 on consecutive steps, which is the
// shape a real calendar produces around weekends and holidays.

// The identity must hold on an irregular calendar exactly as it does on a
// uniform one -- necessary but NOT sufficient, since the residual absorbs a
// mis-scaled carry and closes the identity anyway. That is precisely why round 1
// passed. The real assertion is the next test.
TEST(BacktestSwapExplain, IdentityHoldsOnAnIrregularCalendar) {
  const fs::path dir = fresh_dir("explain_irregular");
  const std::vector<double> spots = {100.0, 101.0, 99.5, 102.0, 101.0, 103.5};
  const std::vector<int> gaps = {0, 1, 3, 1, 7, 1};
  const Corpus c = make_irregular_spot_corpus(dir, "SPX", spots, gaps);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 13LL * kDayNs; // the last snapshot
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/4u)};

  RunConfig cfg;
  cfg.swap_pnl_explain = true;
  const auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.swap_explain_carry.size(), r.size());

  for (std::size_t i = 0; i < r.size(); ++i) {
    const double summed = r.swap_explain_carry[i] + r.swap_explain_realized[i] +
                          r.swap_explain_vol_level[i] + r.swap_explain_skew[i] +
                          r.swap_explain_convexity[i] + r.swap_explain_discount[i] +
                          r.swap_explain_residual[i];
    const double tol = 1.0e-9 * std::abs(r.swap_pnl[i]) + 1.0e-6;
    EXPECT_NEAR(summed, r.swap_pnl[i], tol) << "irregular row " << i;
  }
}

// THE TEST THAT WOULD HAVE CAUGHT C-1.
//
// `carry` is `theta_zero_fixing * dt`, and `theta_zero_fixing` is a rate whose
// fixing leg does NOT scale with the interval -- one fixing lands per step
// regardless of how long the step is. So carry is NOT proportional to `dt`: it
// is (one fixing) + (a calendar roll proportional to dt). On a variance swap the
// fixing leg dominates by orders of magnitude, so carry should be roughly
// CONSTANT per step across wildly different step lengths.
//
// Round 1's bug made carry scale by `dt_this / dt_prev`, which on this fixture's
// 1/3/1/7/1-day sequence would swing consecutive carry values by 3x, 1/3, 7x and
// 1/7. That is the signature this test refuses.
//
// Stated as a ratio rather than a value so it pins the SHAPE and not a number
// that would need re-deriving whenever the fixture moves.
TEST(BacktestSwapExplain, CarryDoesNotScaleWithStepLengthOnAnIrregularCalendar) {
  const fs::path dir = fresh_dir("explain_carry_scale");
  // Flat spots: every step realizes exactly zero variance, so `realized` is 0
  // and carry is the only live component. That isolates the column under test.
  const std::vector<double> spots = {100.0, 100.0, 100.0, 100.0, 100.0, 100.0};
  const std::vector<int> gaps = {0, 1, 3, 1, 7, 1};
  const Corpus c = make_irregular_spot_corpus(dir, "SPX", spots, gaps);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  const std::int64_t expiry = kBaseNow + 13LL * kDayNs;
  SwapOnlyStrategy strat{var_swap_proto(kUid, expiry, /*n_obs_total=*/4u)};

  RunConfig cfg;
  cfg.swap_pnl_explain = true;
  const auto result = run_backtest(*clock, strat, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().to_string();
  const BacktestResult &r = *result;
  ASSERT_EQ(r.swap_explain_carry.size(), r.size());

  // The attributed rows: skip inception (row 0) and the lot's first mark (row 1,
  // which seeds the fixing series and lands no fixing), and skip the expiry row,
  // which settles rather than marks.
  std::vector<double> carries;
  for (std::size_t i = 2; i + 1 < r.size(); ++i) {
    if (r.swap_explain_unattributed[i] == 0.0) {
      carries.push_back(r.swap_explain_carry[i]);
    }
  }
  ASSERT_GE(carries.size(), 3u) << "need several attributed steps of differing length";

  double lo = std::abs(carries.front());
  double hi = lo;
  for (const double v : carries) {
    lo = std::min(lo, std::abs(v));
    hi = std::max(hi, std::abs(v));
  }
  ASSERT_GT(lo, 0.0) << "carry must be non-zero, or this test measures nothing";

  // The step lengths here span 1 to 7 days -- a 7x range. A carry that tracked
  // `dt_this/dt_prev` would spread by at least 3x across these rows. One fixing
  // per step plus a small dt-proportional roll spreads far less; the bound is
  // deliberately loose because the roll leg IS allowed to vary with dt.
  EXPECT_LT(hi / lo, 2.0)
      << "carry spread " << lo << " .. " << hi << " across steps of 1, 3, 7 and 1 days. "
      << "carry is one fixing (independent of step length) plus a dt-proportional roll, "
      << "so it must not track the step-length ratio. A spread near 3x or 7x means "
      << "`theta_zero_fixing` is being multiplied by an interval other than the one it "
      << "was bumped over -- which is C-1.";
}


// ── The partially-populated explain set (fix round 6) ───────────────────────
//
// The explain columns are written together or not at all -- `push_row` fills all
// eight under one `if`. Nothing enforced that. Both shape validators checked
// every column INDEPENDENTLY (`empty || row-parallel`), so eight individually
// legal columns could form an illegal SET, and `append_backtest_results` decided
// the set's shape by reading `swap_explain_columns().front()` -- one column
// sampled for a property of all eight -- justified by a comment asserting the
// validators had already rejected a partial set.
//
// MEASURED before the fix: a result with `swap_explain_carry` populated and the
// other seven empty passed `validate()`, passed the append, and emerged with
// `carry` at 3 rows and `skew` at 1. `combined.validate()` refused it only
// afterwards, by which point the ragged result had already escaped.
//
// The fix is `swap_explain_shape`, which cannot answer from fewer than all eight
// columns, plus both validators rejecting `Mixed`. These tests pin each half.

TEST(BacktestSwapExplain, APartiallyPopulatedExplainSetIsMalformed) {
  BacktestResult r = make_result(/*first_index=*/0u, /*n_rows=*/2u, /*swap=*/true,
                                 /*swap_value=*/0.0);
  EXPECT_EQ(swap_explain_shape(r), SwapExplainShape::Absent);
  EXPECT_TRUE(r.validate().has_value());

  // All eight -> Present, and legal.
  for (const BacktestExplainColumn &column : swap_explain_columns()) {
    (r.*(column.member)).assign(r.size(), 0.0);
  }
  EXPECT_EQ(swap_explain_shape(r), SwapExplainShape::Present);
  EXPECT_TRUE(r.validate().has_value());

  // Exactly one emptied -> Mixed, and refused. Every column is individually
  // legal here (empty is always allowed per column); only the SET is wrong,
  // which is precisely the question the per-column loop cannot ask.
  r.swap_explain_skew.clear();
  EXPECT_EQ(swap_explain_shape(r), SwapExplainShape::Mixed);
  const Status refused = r.validate();
  ASSERT_FALSE(refused.has_value())
      << "a set with seven populated columns and one empty must be refused; before fix "
         "round 6 this returned OK and the ragged result escaped the append";
  EXPECT_NE(refused.error().message().find("partially populated"), std::string::npos)
      << refused.error().to_string();

  // And the mirror case: exactly one populated.
  BacktestResult one = make_result(0u, 2u, /*swap=*/true, /*swap_value=*/0.0);
  one.swap_explain_carry.assign(one.size(), 0.0);
  EXPECT_EQ(swap_explain_shape(one), SwapExplainShape::Mixed);
  EXPECT_FALSE(one.validate().has_value());
}

// The escape itself, end to end. This is the reproduction from the round-6
// audit, asserted as a refusal.
TEST(BacktestSwapExplain, AppendRefusesAPartiallyPopulatedExplainRatherThanRaggedIt) {
  // dst: carry populated but ALL ZERO, so `result_has_explain_data` is false and
  // the round-2 I-3 refusal does NOT fire. That is what made this reachable.
  BacktestResult dst = make_result(0u, 2u, /*swap=*/true, /*swap_value=*/0.0);
  dst.swap_explain_carry.assign(dst.size(), 0.0);

  BacktestResult src = make_result(2u, 1u, /*swap=*/true, /*swap_value=*/0.0);
  for (const BacktestExplainColumn &column : swap_explain_columns()) {
    (src.*(column.member)).assign(src.size(), 0.0);
  }

  const Status refused = append_backtest_results(dst, src);
  ASSERT_FALSE(refused.has_value())
      << "before fix round 6 this returned OK and produced carry=3, skew=1 -- a ragged "
         "result that only the NEXT validate() would have caught";
  EXPECT_EQ(refused.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(refused.error().message().find("partially populated"), std::string::npos)
      << refused.error().to_string();

  // The append refused rather than half-applied: `dst` still has its own rows.
  EXPECT_EQ(dst.size(), 2u);
}

// The property the whole round rests on: no accessor answers the set question
// from one column. A single populated column must NOT read as Present, which is
// exactly what the deleted `.front()` sample did whenever `carry` was the
// populated one.
TEST(BacktestSwapExplain, TheShapeAccessorReadsEveryColumnNotTheFirst) {
  const std::span<const BacktestExplainColumn> roster = swap_explain_columns();
  ASSERT_GT(roster.size(), 1u);

  for (std::size_t i = 0; i < roster.size(); ++i) {
    BacktestResult only_one = make_result(0u, 2u, /*swap=*/true, /*swap_value=*/0.0);
    (only_one.*(roster[i].member)).assign(only_one.size(), 0.0);
    EXPECT_EQ(swap_explain_shape(only_one), SwapExplainShape::Mixed)
        << "column " << roster[i].name
        << " populated alone must read as Mixed; a first-column sample would have called "
           "index 0 Present and every other index Absent";

    BacktestResult all_but_one = make_result(0u, 2u, /*swap=*/true, /*swap_value=*/0.0);
    for (std::size_t j = 0; j < roster.size(); ++j) {
      if (j != i) {
        (all_but_one.*(roster[j].member)).assign(all_but_one.size(), 0.0);
      }
    }
    EXPECT_EQ(swap_explain_shape(all_but_one), SwapExplainShape::Mixed)
        << "column " << roster[i].name
        << " empty alone must read as Mixed; a first-column sample would have called this "
           "Present for every index except 0";
  }
}


// ── `MarketSnapshot::load` verifies what it LOADED (fix rounds 7-8) ─────────
//
// The rule, and it is one rule rather than a branch-by-branch habit: a load
// checks `now_ts_ns` across exactly the surfaces it loaded, and promises nothing
// about records it never read.
//
//   * whole board          -> every record loaded, every record checked
//   * subset that MATCHED  -> checks the surfaces it loaded, not the ones it
//                             skipped, and does not claim to
//   * subset matching NONE -> owns zero surfaces (`subset_missed` is
//                             `subset_requested && !loaded_subset`), so it
//                             verifies the empty set and reads ONE record purely
//                             to date an empty snapshot
//
// Round 7 made the last case verify the entire directory. Round 8 reverted that:
// it contradicted the B1 cheap-miss argument in `MarketSnapshot::load` (measured
// +71% on the miss path, taking it to 91% of a whole-board load at 512 surfaces)
// AND it made the path returning NO data enforce more than the path returning
// one surface, which is backwards.
//
// So these tests pin the rule including its deliberate limit: a mixed archive is
// refused by every path that loads from it, and accepted by the path that loads
// nothing. That asymmetry is a consequence of the rule rather than an exception
// to it, and it is pinned so it cannot drift back into being silent.

namespace {

// Two surfaces in ONE archive, with caller-chosen timestamps so the disagreeing
// case is constructible through the ordinary writer rather than by corrupting
// bytes.
[[nodiscard]] std::string write_two(const fs::path &dir, const std::string &date,
                                    std::int64_t now_a, std::int64_t now_b) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  const std::string path = (dir / (date + ".atxvsa")).string();
  const PricedSurface a = make_surface(kUid, 100.0, 100.0, now_a);
  const PricedSurface b = make_surface(kUid + 1u, 105.0, 105.0, now_b);
  const SurfaceArchiveItem items[] = {SurfaceArchiveItem{"AAA", &a},
                                      SurfaceArchiveItem{"BBB", &b}};
  const Status st = write_surface_archive_v2_file(path, std::span<const SurfaceArchiveItem>{items});
  EXPECT_TRUE(st.has_value()) << (st.has_value() ? std::string{} : st.error().to_string());
  return path;
}

} // namespace

// THIS PINS BEHAVIOUR THAT ALREADY HELD; it does not change any. The
// verification loop these cases exercise is byte-identical at 8454a32, c1771a6,
// c8bf271 and ec7d3ae -- a subset naming both uids fell into the same `else` as
// a whole-board load at every one of them, and refused at every one. F-8 r8
// reported this as "previously loaded, now refuses" and a CHANGELOG migration
// entry was written from that report before anyone read the pre-image. Recorded
// here so the next reader does not re-derive the same false entry from this
// test's existence: the gain is that an UNPINNED behaviour is now pinned, which
// is worth a test and is not worth a migration note.
TEST(BacktestSwapExplain, EveryLoadThatReadsAMixedArchiveRefusesIt) {
  const fs::path dir = fresh_dir("r8-ts-disagree");
  // The two surfaces are stamped a full day apart -- a corrupt or mixed archive.
  const std::string path = write_two(dir, "2026-08-01", kBaseNow, kBaseNow + kDayNs);

  // Whole board: loads both, so it sees the disagreement.
  const auto whole = MarketSnapshot::load(path);
  ASSERT_FALSE(whole.has_value()) << "a whole-board load reads both records and must refuse";
  EXPECT_NE(whole.error().message().find("disagree on now_ts_ns"), std::string::npos)
      << whole.error().to_string();

  // A subset naming BOTH uids also loads both, so it must refuse identically --
  // the rule is about what was loaded, not about which entry point was used.
  const std::uint32_t both[] = {kUid, kUid + 1u};
  const auto pair = MarketSnapshot::load(path, QueryPricingTier::LegacyCompatible,
                                         std::span<const std::uint32_t>{both});
  ASSERT_FALSE(pair.has_value()) << "a subset that loads both records must refuse them too";
  EXPECT_NE(pair.error().message().find("disagree on now_ts_ns"), std::string::npos)
      << pair.error().to_string();
}

// THE DELIBERATE LIMIT, pinned so it cannot become silent again. A load that
// reads ONE record, or none, cannot see a disagreement -- and does not claim to.
// This is the documented consequence of "verify what you loaded", not a gap in
// it; round 7 tried to close it by walking the whole directory and paid 91% of a
// whole-board load on the path that exists to be cheap.
TEST(BacktestSwapExplain, ALoadThatReadsOneRecordOrNoneCannotSeeAMixedArchive) {
  const fs::path dir = fresh_dir("r8-ts-partial");
  const std::string path = write_two(dir, "2026-08-01", kBaseNow, kBaseNow + kDayNs);

  // Zero-surface: verifies the empty set, dates the snapshot from one record.
  const std::uint32_t absent[] = {kUid + 9000u};
  const auto missed = MarketSnapshot::load(path, QueryPricingTier::LegacyCompatible,
                                           std::span<const std::uint32_t>{absent});
  ASSERT_TRUE(missed.has_value()) << missed.error().to_string();
  EXPECT_EQ(missed->n_surfaces(), 0u);

  // Exactly one surface: verifies that one against itself, trivially agrees.
  const std::uint32_t one[] = {kUid + 1u};
  const auto single = MarketSnapshot::load(path, QueryPricingTier::LegacyCompatible,
                                           std::span<const std::uint32_t>{one});
  ASSERT_TRUE(single.has_value()) << single.error().to_string();
  EXPECT_EQ(single->n_surfaces(), 1u);
  EXPECT_EQ(single->ts_ns(), kBaseNow + kDayNs)
      << "a one-surface load reports ITS surface's date, not the archive's first record's";
}

TEST(BacktestSwapExplain, AZeroSurfaceSnapshotStillLoadsWhenTheArchiveAgrees) {
  const fs::path dir = fresh_dir("r8-ts-agree");
  const std::string path = write_two(dir, "2026-08-01", kBaseNow, kBaseNow);

  // The positive control for the test above: same shape, same zero-surface path,
  // agreeing timestamps. Without this, "refuses everything" would pass.
  const std::uint32_t absent[] = {kUid + 9000u};
  const auto missed = MarketSnapshot::load(path, QueryPricingTier::LegacyCompatible,
                                           std::span<const std::uint32_t>{absent});
  ASSERT_TRUE(missed.has_value()) << missed.error().to_string();
  EXPECT_EQ(missed->n_surfaces(), 0u) << "a subset matching no uid owns no surface, by design";
  EXPECT_EQ(missed->ts_ns(), kBaseNow) << "and still reports the date it was loaded for";

  // A subset that DOES match goes through the other branch and is verified there.
  const std::uint32_t present[] = {kUid + 1u};
  const auto hit = MarketSnapshot::load(path, QueryPricingTier::LegacyCompatible,
                                        std::span<const std::uint32_t>{present});
  ASSERT_TRUE(hit.has_value()) << hit.error().to_string();
  EXPECT_EQ(hit->n_surfaces(), 1u);
  EXPECT_EQ(hit->ts_ns(), kBaseNow);
}
