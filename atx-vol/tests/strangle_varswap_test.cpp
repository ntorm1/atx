// atx-vol XOM strangle-vs-varswap comparison backtest — Task 1 gates.
//
// The OPTIONS leg of the comparison: a FIXED-EXPIRY, DAILY-RESTRIKE strangle.
// One cycle picks a single expiry off the run's session grid and holds it; every
// step inside the cycle closes both wings and reopens them at freshly resolved
// +/-40 delta strikes on that step's surface, at the SAME expiry. The engine
// settles the cycle at its expiry session and the next cycle opens on that same
// step.
//
// Gates:
//   1. OpensFortyDeltaStrangleAtSnappedFixedExpiry — the inception pair sits at
//      +/-40 delta on the hand-computed snapped session.
//   2. RestrikesDailyAtFixedExpiry               — strikes move every step, the
//      expiry does not, and the book never accumulates lots.
//   3. RollsIntoNewCycleAtExpiry                 — a new pair with a strictly
//      LATER fixed expiry exists after the expiry session.
//   4. HedgeSpecIsDeltaToZeroDaily               — the engine-owned hedge request.
//   5. KeepsStrikesWhenSurfaceCannotServeDelta   — an unusable surface leaves the
//      live strikes untouched (no fabricated strike, no reopen churn).
//   6. FixesTheLastSessionWhenTheTenorOutrunsTheGrid — the short final cycle, and
//      the exhausted grid that opens nothing at all.
//
// Fixture plumbing (synthetic eSSVI surfaces written as one-symbol archives per
// date) mirrors backtest_swap_test.cpp. Per-step book state is read off
// `run_backtest_incremental`'s checkpoint over a PREFIX of the clock, so every
// assertion is on a book the engine itself validated and priced — never on
// strategy internals.

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"           // Err
#include "atx/vol/american.hpp"         // al_fast_opts, AmericanMethod
#include "atx/vol/backtest.hpp"         // Clock, run_backtest_incremental, RunConfig
#include "atx/vol/corpus.hpp"           // CorpusManifest, CorpusEntry, CorpusFitStatus
#include "atx/vol/portfolio_pricer.hpp" // kNsPerYear, SurfaceRef
#include "atx/vol/priced_surface.hpp"   // PricedSurface, PricingContext
#include "atx/vol/strangle_varswap.hpp" // StrangleVarswapConfig, StrangleVsVarswapStrategy
#include "atx/vol/strategy.hpp"         // HedgeSpec
#include "atx/vol/surface_archive.hpp"  // write_surface_archive_v2_file
#include "atx/vol/surface_parity.hpp"   // SliceContext
#include "atx/vol/types.hpp"            // Side, Result, Status
#include "atx/vol/vol_curve.hpp"        // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"      // EssviParams

using namespace atx::vol;
namespace fs = std::filesystem;

namespace {

constexpr double kR = 0.043;
constexpr std::int64_t kBaseNow = 1700000000000000000LL;
constexpr std::int64_t kDayNs = 86400LL * 1000000000LL;
// 30 calendar days between sessions. Coarse on purpose: it keeps the fixture to
// seven archives while every residual tenor the strategy resolves on stays
// inside the synthetic surface's fitted pillar range [0.05, 1.0].
constexpr std::int64_t kStepNs = 30LL * kDayNs;
constexpr std::uint32_t kUid = 11;
constexpr std::uint32_t kDarkUid = 4243; // written only on the "board went dark" date
constexpr std::size_t kSessions = 7;
constexpr double kTargetDelta = 0.40;
constexpr double kContracts = 100.0;
constexpr const char *kSymbol = "XOM";

// A synthetic eSSVI PricedSurface (flat forward, genuine American premium via
// q_eff=0.02), slices T in [0.05, 1.0]. Mirrors backtest_swap_test's make_surface.
[[nodiscard]] PricedSurface make_surface(std::uint32_t uid, double S, std::int64_t now_ts) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  const double Ts[] = {0.05, 0.10, 0.20, 0.35, 0.50, 0.75, 1.00};
  int i = 0;
  for (const double T : Ts) {
    const double term_forward = S * std::exp((kR - 0.02) * T);
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
  const fs::path dir = fs::temp_directory_path() / (std::string("atx-strvs-") + tag);
  std::error_code ec;
  fs::remove_all(dir, ec);
  return dir;
}

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

[[nodiscard]] CorpusManifest
make_manifest(const std::vector<std::pair<std::string, std::string>> &date_paths) {
  CorpusManifest m;
  for (const auto &[date, path] : date_paths) {
    m.dates.push_back(date);
    CorpusEntry e;
    e.date = date;
    e.symbol = kSymbol;
    e.status = CorpusFitStatus::Ok;
    e.archive_path = path;
    m.entries.push_back(std::move(e));
  }
  return m;
}

struct Corpus {
  CorpusManifest manifest;
  std::vector<std::pair<std::string, std::string>> dp; // (date, path), ascending
  std::vector<std::int64_t> sessions;                  // the run's session grid
};

// Seven sessions kStepNs apart on an EXPLICIT drifting spot path, so each step's
// 40-delta strikes are genuinely different numbers. `dark_at` names the one date
// whose archive is written under a different symbol/uid — the name's board is
// simply absent that session.
[[nodiscard]] Corpus make_corpus(const fs::path &dir,
                                 std::size_t dark_at = static_cast<std::size_t>(-1)) {
  const double spots[kSessions] = {100.0, 106.0, 96.0, 103.0, 111.0, 94.0, 101.0};
  Corpus c;
  for (std::size_t d = 0; d < kSessions; ++d) {
    const std::int64_t now = kBaseNow + static_cast<std::int64_t>(d) * kStepNs;
    const bool dark = d == dark_at;
    const PricedSurface s = make_surface(dark ? kDarkUid : kUid, spots[d], now);
    char buf[16];
    std::snprintf(buf, sizeof buf, "2026-08-%02d", static_cast<int>(d) + 1);
    const std::string date = buf;
    c.dp.emplace_back(date, write_one(dir, date, dark ? "OTHER" : kSymbol, s));
    c.sessions.push_back(now);
  }
  c.manifest = make_manifest(c.dp);
  return c;
}

[[nodiscard]] StrangleVarswapConfig make_config(const Corpus &c, double tenor_years) {
  StrangleVarswapConfig cfg;
  cfg.symbol = kSymbol;
  cfg.target_abs_delta = kTargetDelta;
  cfg.tenor_years = tenor_years;
  cfg.contracts = kContracts;
  cfg.session_ts = c.sessions;
  return cfg;
}

// Run `strat` over refs[0..last_index] and hand back the engine's rows plus the
// post-step book at that ref. The strategy carries per-cycle state, so every
// prefix gets its own instance; the run is deterministic, so a prefix reproduces
// the corresponding steps of the full run exactly.
[[nodiscard]] Result<BacktestContinuation> run_prefix(const Clock &clock, const Corpus &c,
                                                      StrangleVsVarswapStrategy &strat,
                                                      std::size_t last_index,
                                                      const RunConfig &rcfg) {
  Result<Clock> sub = clock.between(c.dp.front().first, c.dp[last_index].first);
  if (!sub) {
    return atx::core::Err(sub.error());
  }
  return run_backtest_incremental(*sub, strat, rcfg, nullptr);
}

[[nodiscard]] const Lot *find_side(const PortfolioState &book, Side side) noexcept {
  for (const Lot &lot : book.lots) {
    if (lot.contract.side == side) {
      return &lot;
    }
  }
  return nullptr;
}

// INDEPENDENT delta oracle: reload the date's archive and ask the very surface
// the engine priced against what this lot's delta is. Nothing here reads the
// strategy.
[[nodiscard]] double lot_delta(const std::string &archive_path, const Lot &lot) {
  Result<MarketSnapshot> snap = MarketSnapshot::load(archive_path);
  EXPECT_TRUE(snap.has_value()) << (snap.has_value() ? std::string{} : snap.error().to_string());
  if (!snap) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const SurfaceRef s = snap->find(lot.contract.uid);
  EXPECT_NE(s, nullptr);
  if (s == nullptr) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const Result<double> d = s->delta(lot.contract.K, lot.contract.T, lot.contract.side);
  EXPECT_TRUE(d.has_value()) << (d.has_value() ? std::string{} : d.error().to_string());
  return d.has_value() ? *d : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

// ── 1. Inception: a 40-delta pair on the hand-computed snapped expiry ────────
TEST(StrangleVarswap, OpensFortyDeltaStrangleAtSnappedFixedExpiry) {
  const fs::path dir = fresh_dir("open");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();

  // 0.25y == 91.3125 calendar days on the library's 365.25-day year. The session
  // grid is 30 calendar days apart from kBaseNow, so the FIRST session at or
  // after the raw anchor is day 120 — ref index 4. Hand-computed, not derived
  // from the strategy.
  const std::int64_t expected_expiry = kBaseNow + 4LL * kStepNs;

  StrangleVsVarswapStrategy strat{make_config(c, 0.25)};
  auto out = run_prefix(*clock, c, strat, /*last_index=*/0u, RunConfig{});
  ASSERT_TRUE(out.has_value()) << out.error().to_string();
  EXPECT_EQ(strat.unresolved_strike_steps(), 0u);
  EXPECT_EQ(strat.cycle_expiry_ts_ns(), expected_expiry);
  const PortfolioState &book = out->checkpoint.portfolio;
  ASSERT_EQ(book.lots.size(), 2u);
  EXPECT_TRUE(book.swap_lots.empty()); // Task 1 is the options leg only

  const Lot *call = find_side(book, Side::Call);
  const Lot *put = find_side(book, Side::Put);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(put, nullptr);

  EXPECT_EQ(call->expiry_ts_ns, expected_expiry);
  EXPECT_EQ(put->expiry_ts_ns, expected_expiry);
  EXPECT_EQ(call->qty, kContracts);
  EXPECT_EQ(put->qty, kContracts);
  // OTM wings straddle the spot: the call strike is above the put strike.
  EXPECT_GT(call->contract.K, put->contract.K);

  EXPECT_NEAR(lot_delta(c.dp[0].second, *call), kTargetDelta, 0.02);
  EXPECT_NEAR(lot_delta(c.dp[0].second, *put), -kTargetDelta, 0.02);
}

// ── 2. Daily restrike at a FIXED expiry, with no lot accumulation ────────────
TEST(StrangleVarswap, RestrikesDailyAtFixedExpiry) {
  const fs::path dir = fresh_dir("restrike");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrangleVarswapConfig scfg = make_config(c, 0.25);
  const std::int64_t expected_expiry = kBaseNow + 4LL * kStepNs;

  double prev_call_K = 0.0;
  double prev_put_K = 0.0;
  for (std::size_t step = 0; step < 3u; ++step) {
    StrangleVsVarswapStrategy strat{scfg};
    auto out = run_prefix(*clock, c, strat, step, RunConfig{});
    ASSERT_TRUE(out.has_value()) << "step " << step << ": " << out.error().to_string();
    EXPECT_EQ(strat.unresolved_strike_steps(), 0u) << "step " << step;
    const PortfolioState &book = out->checkpoint.portfolio;
    // Exactly two option lots every step: the prior pair was CLOSED, not kept.
    ASSERT_EQ(book.lots.size(), 2u) << "step " << step;
    // Two fresh lot ids per step, so no id was reused and none was retained.
    EXPECT_EQ(out->checkpoint.next_lot_id, 1u + 2u * (step + 1u)) << "step " << step;

    const Lot *call = find_side(book, Side::Call);
    const Lot *put = find_side(book, Side::Put);
    ASSERT_NE(call, nullptr) << "step " << step;
    ASSERT_NE(put, nullptr) << "step " << step;

    // The expiry is FIXED for the whole cycle; only the strikes move.
    EXPECT_EQ(call->expiry_ts_ns, expected_expiry) << "step " << step;
    EXPECT_EQ(put->expiry_ts_ns, expected_expiry) << "step " << step;
    // Each restrike re-hits the target on THAT step's surface.
    EXPECT_NEAR(lot_delta(c.dp[step].second, *call), kTargetDelta, 0.02) << "step " << step;
    EXPECT_NEAR(lot_delta(c.dp[step].second, *put), -kTargetDelta, 0.02) << "step " << step;

    if (step > 0u) {
      EXPECT_GT(std::fabs(call->contract.K - prev_call_K), 1.0e-6) << "step " << step;
      EXPECT_GT(std::fabs(put->contract.K - prev_put_K), 1.0e-6) << "step " << step;
    }
    prev_call_K = call->contract.K;
    prev_put_K = put->contract.K;
  }
}

// ── 3. The cycle settles at its expiry and the next one opens the same step ──
TEST(StrangleVarswap, RollsIntoNewCycleAtExpiry) {
  const fs::path dir = fresh_dir("roll");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  // 50 calendar days: the raw anchor from ref 0 lands between session 1 (day 30)
  // and session 2 (day 60), so cycle 1 expires on ref 2. From ref 2 the anchor is
  // day 110, between session 3 (day 90) and session 4 (day 120) — cycle 2 fixes
  // on ref 4.
  const StrangleVarswapConfig scfg = make_config(c, 50.0 / 365.25);
  const std::int64_t cycle1_expiry = kBaseNow + 2LL * kStepNs;
  const std::int64_t cycle2_expiry = kBaseNow + 4LL * kStepNs;

  StrangleVsVarswapStrategy before_strat{scfg};
  auto before = run_prefix(*clock, c, before_strat, /*last_index=*/1u, RunConfig{});
  ASSERT_TRUE(before.has_value()) << before.error().to_string();
  ASSERT_EQ(before->checkpoint.portfolio.lots.size(), 2u);
  for (const Lot &lot : before->checkpoint.portfolio.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, cycle1_expiry);
  }
  const std::uint64_t ids_before = before->checkpoint.next_lot_id;

  StrangleVsVarswapStrategy after_strat{scfg};
  auto after = run_prefix(*clock, c, after_strat, /*last_index=*/2u, RunConfig{});
  ASSERT_TRUE(after.has_value()) << after.error().to_string();
  const PortfolioState &book = after->checkpoint.portfolio;
  // The expired pair settled (engine-owned) and a NEW pair replaced it.
  ASSERT_EQ(book.lots.size(), 2u);
  const Lot *call = find_side(book, Side::Call);
  const Lot *put = find_side(book, Side::Put);
  ASSERT_NE(call, nullptr);
  ASSERT_NE(put, nullptr);
  EXPECT_EQ(call->expiry_ts_ns, cycle2_expiry);
  EXPECT_EQ(put->expiry_ts_ns, cycle2_expiry);
  EXPECT_GT(cycle2_expiry, cycle1_expiry);
  // Fresh ids: the new pair is not the settled one carried forward.
  EXPECT_GE(call->id, ids_before);
  EXPECT_GE(put->id, ids_before);
  // The 40-delta target is honored on the new cycle's tenor too.
  EXPECT_NEAR(lot_delta(c.dp[2].second, *call), kTargetDelta, 0.02);
  EXPECT_NEAR(lot_delta(c.dp[2].second, *put), -kTargetDelta, 0.02);
}

// ── 4. The engine-owned hedge request ───────────────────────────────────────
TEST(StrangleVarswap, HedgeSpecIsDeltaToZeroDaily) {
  StrangleVsVarswapStrategy strat{StrangleVarswapConfig{}};
  const HedgeSpec hedge = strat.hedge_spec();
  EXPECT_EQ(hedge.kind, HedgeSpec::Kind::DeltaToZero);
  EXPECT_EQ(hedge.cadence, HedgeSpec::Cadence::Daily);
  EXPECT_EQ(hedge.band, 0.0);
}

// ── 5. An unusable surface keeps the live strikes, and never fabricates one ──
TEST(StrangleVarswap, KeepsStrikesWhenSurfaceCannotServeDelta) {
  const fs::path dir = fresh_dir("nodelta");
  // Ref 2's archive carries a different name entirely: there is no surface to
  // resolve a 40-delta strike on that session.
  const Corpus c = make_corpus(dir, /*dark_at=*/2u);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrangleVarswapConfig scfg = make_config(c, 0.25);
  RunConfig rcfg;
  // The held pair cannot be valued on the dark session either; that is the
  // documented ExcludeAndReport lane, and it is what keeps the run going so the
  // strategy's own behaviour is observable.
  rcfg.unpriced = UnpricedLotPolicy::ExcludeAndReport;

  StrangleVsVarswapStrategy live_strat{scfg};
  auto live = run_prefix(*clock, c, live_strat, /*last_index=*/1u, rcfg);
  ASSERT_TRUE(live.has_value()) << live.error().to_string();
  EXPECT_EQ(live_strat.unresolved_strike_steps(), 0u);
  ASSERT_EQ(live->checkpoint.portfolio.lots.size(), 2u);
  const Lot *live_call = find_side(live->checkpoint.portfolio, Side::Call);
  const Lot *live_put = find_side(live->checkpoint.portfolio, Side::Put);
  ASSERT_NE(live_call, nullptr);
  ASSERT_NE(live_put, nullptr);
  const double held_call_K = live_call->contract.K;
  const double held_put_K = live_put->contract.K;
  const std::uint64_t ids_before_dark = live->checkpoint.next_lot_id;

  // The dark session: the run CONTINUES and the book is untouched.
  StrangleVsVarswapStrategy dark_strat{scfg};
  auto dark = run_prefix(*clock, c, dark_strat, /*last_index=*/2u, rcfg);
  ASSERT_TRUE(dark.has_value()) << dark.error().to_string();
  // Counted, never silent: exactly the one session whose board was missing.
  EXPECT_EQ(dark_strat.unresolved_strike_steps(), 1u);
  const PortfolioState &dark_book = dark->checkpoint.portfolio;
  ASSERT_EQ(dark_book.lots.size(), 2u);
  const Lot *dark_call = find_side(dark_book, Side::Call);
  const Lot *dark_put = find_side(dark_book, Side::Put);
  ASSERT_NE(dark_call, nullptr);
  ASSERT_NE(dark_put, nullptr);
  // EXACT, not near: the strikes are the previous step's, never re-derived.
  EXPECT_EQ(dark_call->contract.K, held_call_K);
  EXPECT_EQ(dark_put->contract.K, held_put_K);
  EXPECT_EQ(dark_call->id, live_call->id);
  EXPECT_EQ(dark_put->id, live_put->id);
  // No reopen churn: the id watermark did not move on the dark step.
  EXPECT_EQ(dark->checkpoint.next_lot_id, ids_before_dark);
  // The dark row really was unvaluable — the fixture is not vacuously green.
  ASSERT_EQ(dark->rows.size(), 3u);
  EXPECT_GT(dark->rows.n_unpriced_greeks.back(), 0.0);

  // The board comes back on ref 3 and the restrike resumes.
  StrangleVsVarswapStrategy back_strat{scfg};
  auto back = run_prefix(*clock, c, back_strat, /*last_index=*/3u, rcfg);
  ASSERT_TRUE(back.has_value()) << back.error().to_string();
  EXPECT_EQ(back_strat.unresolved_strike_steps(), 1u); // still only the dark session
  ASSERT_EQ(back->checkpoint.portfolio.lots.size(), 2u);
  const Lot *back_call = find_side(back->checkpoint.portfolio, Side::Call);
  const Lot *back_put = find_side(back->checkpoint.portfolio, Side::Put);
  ASSERT_NE(back_call, nullptr);
  ASSERT_NE(back_put, nullptr);
  EXPECT_GT(std::fabs(back_call->contract.K - held_call_K), 1.0e-6);
  EXPECT_GT(std::fabs(back_put->contract.K - held_put_K), 1.0e-6);
  EXPECT_GT(back->checkpoint.next_lot_id, ids_before_dark);
}

// ── 6. The grid boundary: a short final cycle, then nothing ─────────────────
TEST(StrangleVarswap, FixesTheLastSessionWhenTheTenorOutrunsTheGrid) {
  const fs::path dir = fresh_dir("gridend");
  const Corpus c = make_corpus(dir);
  auto clock = Clock::from_manifest(c.manifest);
  ASSERT_TRUE(clock.has_value()) << clock.error().to_string();
  const StrangleVarswapConfig scfg = make_config(c, 0.25);

  // Cycle 1 expires on ref 4 (day 120). From there the raw anchor is day 211.3,
  // past the last session (day 180) — so cycle 2 takes the LAST session rather
  // than an expiry the corpus never reaches.
  StrangleVsVarswapStrategy tail_strat{scfg};
  auto tail = run_prefix(*clock, c, tail_strat, /*last_index=*/4u, RunConfig{});
  ASSERT_TRUE(tail.has_value()) << tail.error().to_string();
  ASSERT_EQ(tail->checkpoint.portfolio.lots.size(), 2u);
  for (const Lot &lot : tail->checkpoint.portfolio.lots) {
    EXPECT_EQ(lot.expiry_ts_ns, kBaseNow + 6LL * kStepNs);
  }

  // On the last session itself there is no session left to expire into, so the
  // settled cycle is not replaced — the book goes flat rather than opening a lot
  // the run can never settle.
  StrangleVsVarswapStrategy end_strat{scfg};
  auto end = run_prefix(*clock, c, end_strat, /*last_index=*/kSessions - 1u, RunConfig{});
  ASSERT_TRUE(end.has_value()) << end.error().to_string();
  EXPECT_TRUE(end->checkpoint.portfolio.lots.empty());
  EXPECT_EQ(end_strat.cycle_expiry_ts_ns(), 0);
  EXPECT_EQ(end_strat.unresolved_strike_steps(), 0u);
}
