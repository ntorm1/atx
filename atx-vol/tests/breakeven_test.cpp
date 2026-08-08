#include "atx/vol/breakeven.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/backtest.hpp"    // Clock, MarketSnapshot
#include "atx/vol/corpus.hpp"      // CorpusBoard, build_corpus, CorpusManifest, read_manifest_file
#include "atx/vol/data.hpp"        // iso_to_ns
#include "atx/vol/market_env.hpp"  // MarketEnv
#include "atx/vol/panel.hpp"       // make_synthetic_american_panel, SynthPanelSpec
#include "atx/vol/spy_fixture.hpp" // make_spy_synthetic_spec
#include "atx/vol/vol_curve.hpp"   // CurveConfig, VolCurveKind
#include "support/cached_artifacts.hpp" // cached_corpus

namespace atx::vol {
namespace {

constexpr std::int64_t kDayNs = 86'400'000'000'000LL;

std::vector<BevDayState> synth_gbm_path(double sigma, std::size_t n_days, std::uint32_t seed,
                                        double s0 = 100.0, double r = 0.0, double q = 0.0) {
  std::mt19937 rng(seed);
  std::normal_distribution<double> z(0.0, 1.0);
  // The variance step must match the calendar-day timestamp grid priced at
  // ACT/365.25: bev_replay_pnl prices/hedges on T = (expiry-ts)/365.25 days,
  // and this generator timestamps each step exactly one CALENDAR day apart
  // (kDayNs below), so the per-step variance has to be sigma^2/365.25 per
  // step for the realized path to actually realize `sigma` under that
  // convention. The brief's original Step 1 text used a 1/252 TRADING-year dt
  // against that same 1-calendar-day timestamp grid, which silently injects
  // ~45% extra variance per unit of priced time (365.25/252) and biased
  // HedgedPnlAtTrueVolIsSmallOnAverage's mean to ~4x its own noise band —
  // confirmed via an independent closed-form Black-Scholes replication with
  // no AL pricer or replay code involved at all (mean=1.26 vs band=0.29,
  // matching the observed bev_replay_pnl failure order-of-magnitude).
  // Day-count fix to the test's synthetic generator only, confirmed with the
  // controller and folded into the plan for later tasks (root-find/batch/
  // loader) that reuse this helper; bev_replay_pnl's ACT/365.25 contract is
  // unchanged.
  const double dt = 1.0 / 365.25, sq = sigma * std::sqrt(dt);
  std::vector<BevDayState> p;
  p.reserve(n_days + 1);
  double s = s0;
  for (std::size_t i = 0; i <= n_days; ++i) {
    p.push_back(BevDayState{static_cast<std::int64_t>(i) * kDayNs, s, r, q});
    s *= std::exp((r - q - 0.5 * sigma * sigma) * dt + sq * z(rng));
  }
  return p;
}

BevSpec atm_call_expiring_at(const std::vector<BevDayState> &p) {
  return BevSpec{p.front().s, p.back().ts_ns, Side::Call};
}

TEST(Breakeven, ReplayFailsClosedWhenExpiryNotLastObservation) {
  auto p = synth_gbm_path(0.2, 60, 5u);
  BevSpec spec = atm_call_expiring_at(p);
  spec.expiry_ns += kDayNs; // one day past the path
  EXPECT_FALSE(bev_replay_pnl(p, spec, 0.2, {}, {}).has_value());
}

TEST(Breakeven, HedgedPnlAtTrueVolIsSmallOnAverage) {
  // 40 paths, 126 days: mean PnL at sigma_true within the Derman-Kamal noise band.
  double sum = 0.0;
  int n_ok = 0;
  double vega0 = 0.0;
  for (std::uint32_t seed = 0; seed < 40; ++seed) {
    const auto p = synth_gbm_path(0.25, 126, 100u + seed);
    const auto r = bev_replay_pnl(p, atm_call_expiring_at(p), 0.25, {}, {});
    ASSERT_TRUE(r.has_value()) << seed;
    sum += r->pnl;
    vega0 = r->vega_entry;
    ++n_ok;
  }
  const double mean = sum / n_ok;
  // std(P&L) ~ sqrt(pi/4)*vega*sigma/sqrt(N); mean-of-40 shrinks by sqrt(40).
  const double band =
      std::sqrt(3.14159265 / 4.0) * vega0 * 0.25 / std::sqrt(126.0) / std::sqrt(40.0) * 4.0;
  EXPECT_LT(std::abs(mean), band);
}

TEST(Breakeven, PnlIsMonotoneDecreasingInEntrySigma) {
  const auto p = synth_gbm_path(0.25, 126, 77u);
  const BevSpec spec = atm_call_expiring_at(p);
  double prev = 1e300;
  for (double sig : {0.10, 0.20, 0.30, 0.45, 0.70}) {
    const auto r = bev_replay_pnl(p, spec, sig, {}, {});
    ASSERT_TRUE(r.has_value());
    EXPECT_LT(r->pnl, prev) << sig;
    prev = r->pnl;
  }
}

TEST(Breakeven, LongCheapGammaPathIsProfitable) {
  const auto p = synth_gbm_path(0.40, 126, 11u);                           // realizes 40 vol
  const auto r = bev_replay_pnl(p, atm_call_expiring_at(p), 0.20, {}, {}); // paid 20
  ASSERT_TRUE(r.has_value());
  EXPECT_GT(r->pnl, 0.0);
}

TEST(Breakeven, DeepItmCallExercisesBeforeLargeDividend) {
  auto p = synth_gbm_path(0.15, 60, 3u, /*s0=*/100.0, /*r=*/0.01);
  BevSpec spec{60.0, p.back().ts_ns, Side::Call};         // deep ITM
  const DividendEvent div{p[30].ts_ns + kDayNs / 2, 5.0}; // huge dividend mid-path
  const auto r = bev_replay_pnl(p, spec, 0.15, std::span(&div, 1), {});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->exercised_early);
  EXPECT_LT(r->exercise_ts_ns, div.ex_date_ns);
}

TEST(Breakeven, DeepItmPutAtHighRateExercisesNearExpiry) {
  // Deep ITM (strike 200 vs. spot ~100) plus a high rate (10%) makes the
  // one-step forgone-interest threshold exceed the shrinking remaining time
  // value well before the true expiry -- the put must exercise early.
  auto p = synth_gbm_path(0.15, 20, 9u, /*s0=*/100.0, /*r=*/0.10);
  const BevSpec spec{200.0, p.back().ts_ns, Side::Put};
  const auto r = bev_replay_pnl(p, spec, 0.15, {}, {});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->exercised_early);
}

TEST(Breakeven, ItmPutWithTimeValueDoesNotFalseTriggerOnDayOne) {
  // The reviewer's concrete failure case: r=5%, ~126d remaining, a put with
  // real (~O(1)) time value. The pre-fix full-remaining-life threshold
  // (~strike * (1 - exp(-r*T)), ~1.70 for strike~100) exceeded that time
  // value and forced exercise on the very first step; the corrected
  // one-step threshold (~strike * (1 - exp(-r*dt_next)), ~0.014) must not.
  const auto p = synth_gbm_path(0.25, 126, 55u, /*s0=*/100.0, /*r=*/0.05);
  const BevSpec spec{105.0, p.back().ts_ns, Side::Put}; // 5 pts ITM at entry
  const auto r = bev_replay_pnl(p, spec, 0.25, {}, {});
  ASSERT_TRUE(r.has_value());
  // Robust either way: never exercises, or if it eventually does (as time
  // value genuinely erodes later in the path), it is not the day-1 false
  // trigger the bug produced (n_days == 1 there). Observed with this fix:
  // exercises on n_days=30 -- comfortably clear of the day-1 boundary.
  EXPECT_TRUE(!r->exercised_early || r->n_days > 1)
      << "exercised_early=" << r->exercised_early << " n_days=" << r->n_days;
}

TEST(Breakeven, SlippageChargedOnEntryAndExitEvenWithNoIntermediateRebalances) {
  // hedge_band huge enough that NO intermediate rebalance ever fires, so the
  // entry hedge trade and the expiry-liquidation trade are the ONLY
  // slippage-eligible trades in this run -- isolates exactly the two sites
  // the fix adds (entry ~:4082, expiry liquidation ~:4107).
  const auto p = synth_gbm_path(0.25, 126, 77u);
  const BevSpec spec = atm_call_expiring_at(p);
  const auto r0 = bev_replay_pnl(p, spec, 0.25, {}, BevReplayConfig{.hedge_band = 1.0e9});
  ASSERT_TRUE(r0.has_value());
  const auto r1 = bev_replay_pnl(p, spec, 0.25, {},
                                 BevReplayConfig{.hedge_band = 1.0e9, .hedge_slippage_bps = 10.0});
  ASSERT_TRUE(r1.has_value());
  EXPECT_LT(r1->pnl, r0->pnl);
  // Loose lower bound: a real 10bps charge on two round-trip legs near
  // spot ~100 is order 0.1-0.2 (|delta| in [0,1], applied to ~S0 and
  // ~S_expiry each); this floor only needs to rule out the pre-fix no-op
  // (both entry and exit contributed exactly 0). Observed with this fix:
  // diff ~= 0.106, a 5x margin over this floor.
  EXPECT_GT(r0->pnl - r1->pnl, 0.02);
}

TEST(Breakeven, GoldenPathPnlIsPinned) {
  const auto p = synth_gbm_path(0.25, 126, 1234u);
  const auto r = bev_replay_pnl(p, atm_call_expiring_at(p), 0.22, {}, {});
  ASSERT_TRUE(r.has_value());
  // Pinned from the first green implementation (determinism gate).
  EXPECT_DOUBLE_EQ(r->pnl, 1.3832639438393373);
}

// ---- THEO-3: solve_breakeven_vol (bisection root-find over bev_replay_pnl) ----

TEST(Breakeven, SolveRecoversTrueVolOnGbmWithinNoiseBand) {
  // sigma_be estimates gamma-weighted realized vol; across seeds it centers on 0.25.
  double sum = 0.0;
  for (std::uint32_t seed = 0; seed < 20; ++seed) {
    const auto p = synth_gbm_path(0.25, 126, 500u + seed);
    const auto lab = solve_breakeven_vol(p, atm_call_expiring_at(p), {}, {});
    ASSERT_TRUE(lab.has_value());
    ASSERT_EQ(lab->flag, BevFlag::Ok);
    sum += lab->sigma_be;
  }
  EXPECT_NEAR(sum / 20.0, 0.25, 0.02);
}

TEST(Breakeven, SolveResidualIsWithinVegaScaledTolerance) {
  const auto p = synth_gbm_path(0.30, 126, 900u);
  const auto lab = solve_breakeven_vol(p, atm_call_expiring_at(p), {}, {});
  ASSERT_TRUE(lab.has_value());
  EXPECT_LT(std::abs(lab->pnl_residual), lab->vega_at_be * 2e-4 + 1e-8);
}

TEST(Breakeven, FarOtmWingReturnsNoBracketNotError) {
  const auto p = synth_gbm_path(0.10, 21, 8u);
  BevSpec spec{p.front().s * 3.0, p.back().ts_ns, Side::Call}; // absurd wing
  const auto lab = solve_breakeven_vol(p, spec, {}, {});
  ASSERT_TRUE(lab.has_value());
  EXPECT_EQ(lab->flag, BevFlag::NoBracket);
}

// ---- THEO-4: solve_breakeven_batch (deterministic parallel fan-out) ----

// Build kN independent (path, spec) pairs from seeds [seed0, seed0+kN) plus
// the BevJob span-list over them. `paths`/`specs` are returned by value so
// the caller keeps them alive for the lifetime of every BevJob's spans (the
// jobs themselves are non-owning, per BevJob's contract).
struct BevBatchFixture {
  std::vector<std::vector<BevDayState>> paths;
  std::vector<BevSpec> specs;
  std::vector<BevJob> jobs;
};

BevBatchFixture make_batch_fixture(std::size_t kN, std::uint32_t seed0) {
  BevBatchFixture fx;
  fx.paths.reserve(kN);
  fx.specs.reserve(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    fx.paths.push_back(synth_gbm_path(0.25, 126, seed0 + static_cast<std::uint32_t>(i)));
    fx.specs.push_back(atm_call_expiring_at(fx.paths.back()));
  }
  fx.jobs.reserve(kN);
  for (std::size_t i = 0; i < kN; ++i) {
    fx.jobs.push_back(BevJob{.path = fx.paths[i], .spec = fx.specs[i], .dividends = {}});
  }
  return fx;
}

TEST(Breakeven, BatchMatchesSerialFieldForField) {
  constexpr std::size_t kN = 32;
  const auto fx = make_batch_fixture(kN, /*seed0=*/0u);

  const auto batch = solve_breakeven_batch(fx.jobs, {}, /*n_threads=*/1);
  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->sigma_be.size(), kN);
  ASSERT_EQ(batch->premium_at_be.size(), kN);
  ASSERT_EQ(batch->vega_at_be.size(), kN);
  ASSERT_EQ(batch->pnl_residual.size(), kN);
  ASSERT_EQ(batch->n_days.size(), kN);
  ASSERT_EQ(batch->iters.size(), kN);
  ASSERT_EQ(batch->flag.size(), kN);
  ASSERT_EQ(batch->status_ok.size(), kN);

  for (std::size_t i = 0; i < kN; ++i) {
    const auto serial = solve_breakeven_vol(fx.paths[i], fx.specs[i], {}, {});
    ASSERT_TRUE(serial.has_value()) << i;
    EXPECT_EQ(batch->status_ok[i], 1u) << i;
    EXPECT_EQ(batch->sigma_be[i], serial->sigma_be) << i;
    EXPECT_EQ(batch->premium_at_be[i], serial->premium_at_be) << i;
    EXPECT_EQ(batch->vega_at_be[i], serial->vega_at_be) << i;
    EXPECT_EQ(batch->pnl_residual[i], serial->pnl_residual) << i;
    EXPECT_EQ(batch->n_days[i], serial->n_days) << i;
    EXPECT_EQ(batch->iters[i], serial->iters) << i;
    EXPECT_EQ(batch->flag[i], static_cast<std::uint8_t>(serial->flag)) << i;
  }
}

TEST(Breakeven, BatchIsBitIdenticalAcrossThreadCounts) {
  constexpr std::size_t kN = 32;
  const auto fx = make_batch_fixture(kN, /*seed0=*/2000u);

  const auto t1 = solve_breakeven_batch(fx.jobs, {}, /*n_threads=*/1);
  const auto t4 = solve_breakeven_batch(fx.jobs, {}, /*n_threads=*/4);
  ASSERT_TRUE(t1.has_value());
  ASSERT_TRUE(t4.has_value());
  ASSERT_EQ(t1->sigma_be.size(), kN);
  ASSERT_EQ(t4->sigma_be.size(), kN);

  for (std::size_t i = 0; i < kN; ++i) {
    EXPECT_EQ(t1->sigma_be[i], t4->sigma_be[i]) << i;
    EXPECT_EQ(t1->premium_at_be[i], t4->premium_at_be[i]) << i;
    EXPECT_EQ(t1->vega_at_be[i], t4->vega_at_be[i]) << i;
    EXPECT_EQ(t1->pnl_residual[i], t4->pnl_residual[i]) << i;
    EXPECT_EQ(t1->n_days[i], t4->n_days[i]) << i;
    EXPECT_EQ(t1->iters[i], t4->iters[i]) << i;
    EXPECT_EQ(t1->flag[i], t4->flag[i]) << i;
    EXPECT_EQ(t1->status_ok[i], t4->status_ok[i]) << i;
  }
}

TEST(Breakeven, PoisonedJobDoesNotSinkBatch) {
  constexpr std::size_t kN = 16;
  auto fx = make_batch_fixture(kN, /*seed0=*/3000u);
  fx.jobs[7].path = {}; // poisoned: empty path, rejected by bev_replay_pnl

  const auto batch = solve_breakeven_batch(fx.jobs, {}, /*n_threads=*/1);
  ASSERT_TRUE(batch.has_value());
  ASSERT_EQ(batch->status_ok.size(), kN);

  EXPECT_EQ(batch->status_ok[7], 0u);
  EXPECT_EQ(batch->sigma_be[7], 0.0);
  EXPECT_EQ(batch->premium_at_be[7], 0.0);
  EXPECT_EQ(batch->vega_at_be[7], 0.0);
  EXPECT_EQ(batch->pnl_residual[7], 0.0);
  EXPECT_EQ(batch->n_days[7], 0u);
  EXPECT_EQ(batch->iters[7], 0u);
  EXPECT_EQ(batch->flag[7], 0u);

  for (std::size_t i = 0; i < kN; ++i) {
    if (i == 7) {
      continue;
    }
    EXPECT_EQ(batch->status_ok[i], 1u) << i;
  }
}

// ---- Task 5: load_bev_path (real surfaces -> BevDayState) ----
//
// Synthetic-only, like MultinamePipeline/Corpus (no OPRA pull, no GTEST_SKIP):
// a small daily corpus built from spy_fixture.hpp's known-truth SPY panel,
// with the spot deliberately walked day-to-day (fixed multipliers, not
// random) so the delta-hedged replay realizes genuine P&L and
// solve_breakeven_vol has a real bracket to find in test (d). Fit once per
// build tree via `cached_corpus` (the established convention for
// fit-bound/slow suites -- see multiname_pipeline_test.cpp).

namespace fs = std::filesystem;

constexpr int kBevPathNDays = 6;
constexpr const char *kBevPathDates[kBevPathNDays] = {"2026-06-01", "2026-06-02", "2026-06-03",
                                                      "2026-06-04", "2026-06-05", "2026-06-06"};
// Deterministic, reviewable daily moves (not GBM-random): a few percent per
// day, enough realized movement for solve_breakeven_vol to find a genuine
// sign-changing bracket.
constexpr double kBevPathSpotMul[kBevPathNDays] = {1.000, 1.022, 0.985, 1.031, 0.978, 1.015};
// Floor applied to each session's remaining tenor before probing q_eff_at
// (Task 5's tenor_probe_years) -- about one calendar day.
constexpr double kBevPathProbeFloor = 1.0 / 365.25;

[[nodiscard]] std::int64_t bev_path_entry_ts() { return iso_to_ns(kBevPathDates[0]); }
[[nodiscard]] std::int64_t bev_path_expiry_ts() {
  return iso_to_ns(kBevPathDates[kBevPathNDays - 1]);
}

// One day's board: the canonical SPY fixture rescaled to `spot` (mirrors
// multiname_pipeline_test.cpp's make_index_spec) so the strike ladder tracks
// the walked spot while the fitted term structure/skew stay the fixture's.
[[nodiscard]] SynthPanelSpec bev_path_day_spec(const std::string &date, double spot) {
  SynthPanelSpec s = make_spy_synthetic_spec(date);
  const double scale = spot / s.spot;
  s.spot = spot;
  for (double &k : s.strikes) {
    k *= scale;
  }
  return s;
}

[[nodiscard]] CorpusBoard bev_path_board(const std::string &date, double spot) {
  const SynthPanelSpec spec = bev_path_day_spec(date, spot);
  auto panel = make_synthetic_american_panel(spec);
  EXPECT_TRUE(panel.has_value()) << (panel.has_value() ? "" : panel.error().to_string());
  CorpusBoard b;
  b.date = date;
  b.symbol = "SPY";
  if (panel.has_value()) {
    b.frame = panel->frame;
  }
  b.env = MarketEnv::flat(spec.spot, spec.r, iso_to_ns(spec.snapshot_iso), spec.cash_divs);
  b.curve = CurveConfig{VolCurveKind::ConvexDense}; // penny-dense index recipe (corpus.hpp)
  return b;
}

[[nodiscard]] std::vector<CorpusBoard> make_bev_path_boards() {
  std::vector<CorpusBoard> boards;
  boards.reserve(kBevPathNDays);
  double spot = 600.0;
  for (int i = 0; i < kBevPathNDays; ++i) {
    spot *= kBevPathSpotMul[i];
    boards.push_back(bev_path_board(kBevPathDates[i], spot));
  }
  return boards;
}

class BevPathLoader : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    const std::vector<CorpusBoard> boards = make_bev_path_boards();
    const fs::path out =
        atx::vol::test::cached_corpus("bev-path-loader-spy-6d", [&boards] { return boards; });
    auto man = read_manifest_file((out / "manifest.tsv").string());
    ASSERT_TRUE(man.has_value()) << man.error().to_string();
    ASSERT_EQ(man->n_boards, static_cast<std::uint32_t>(kBevPathNDays));
    ASSERT_EQ(man->n_ok, static_cast<std::uint32_t>(kBevPathNDays))
        << "every synthetic day must fit Ok for this fixture to be meaningful";
    auto clk = Clock::from_manifest(*man);
    ASSERT_TRUE(clk.has_value()) << clk.error().to_string();
    clock_ = std::move(*clk);
  }

  static Clock clock_;
};

Clock BevPathLoader::clock_{};

// (a) one BevDayState per session between entry and expiry, strictly
// increasing ts_ns, positive spots.
TEST_F(BevPathLoader, LoadedPathHasOneEntryPerSessionIncreasingTsPositiveSpots) {
  const auto path =
      load_bev_path(clock_, "SPY", bev_path_entry_ts(), bev_path_expiry_ts(), kBevPathProbeFloor);
  ASSERT_TRUE(path.has_value()) << path.error().to_string();
  EXPECT_FALSE(path->snapped);
  EXPECT_EQ(path->settle_ts_ns, bev_path_expiry_ts());
  ASSERT_EQ(path->days.size(), static_cast<std::size_t>(kBevPathNDays));
  for (std::size_t i = 0; i < path->days.size(); ++i) {
    EXPECT_GT(path->days[i].s, 0.0) << i;
    if (i > 0) {
      EXPECT_GT(path->days[i].ts_ns, path->days[i - 1].ts_ns) << i;
    }
  }
  EXPECT_EQ(path->days.back().ts_ns, path->settle_ts_ns);
}

// (b) Exact snap on a non-session expiry fails closed.
TEST_F(BevPathLoader, ExactSnapOnNonSessionExpiryFailsClosed) {
  const std::int64_t mid_session_ts = iso_to_ns(kBevPathDates[2]) + kDayNs / 2;
  const auto path = load_bev_path(clock_, "SPY", bev_path_entry_ts(), mid_session_ts,
                                  kBevPathProbeFloor, BevExpirySnap::Exact);
  EXPECT_FALSE(path.has_value());
}

// (c) LastSessionAtOrBefore snaps, snapped=true, settle < expiry.
TEST_F(BevPathLoader, LastSessionAtOrBeforeSnapsAndSettlesBeforeExpiry) {
  const std::int64_t mid_session_ts = iso_to_ns(kBevPathDates[2]) + kDayNs / 2;
  const auto path = load_bev_path(clock_, "SPY", bev_path_entry_ts(), mid_session_ts,
                                  kBevPathProbeFloor, BevExpirySnap::LastSessionAtOrBefore);
  ASSERT_TRUE(path.has_value()) << path.error().to_string();
  EXPECT_TRUE(path->snapped);
  EXPECT_LT(path->settle_ts_ns, mid_session_ts);
  EXPECT_EQ(path->settle_ts_ns, iso_to_ns(kBevPathDates[2]));
  ASSERT_FALSE(path->days.empty());
  EXPECT_EQ(path->days.back().ts_ns, path->settle_ts_ns);
}

// (d) end-to-end: load_bev_path + solve_breakeven_vol on one SPY contract.
TEST_F(BevPathLoader, EndToEndLoadAndSolveProducesOkBreakevenVol) {
  const auto path =
      load_bev_path(clock_, "SPY", bev_path_entry_ts(), bev_path_expiry_ts(), kBevPathProbeFloor);
  ASSERT_TRUE(path.has_value()) << path.error().to_string();
  ASSERT_GE(path->days.size(), 2u);

  const BevSpec spec = atm_call_expiring_at(path->days);
  const auto lab = solve_breakeven_vol(path->days, spec, {}, {});
  ASSERT_TRUE(lab.has_value()) << lab.error().to_string();
  EXPECT_EQ(lab->flag, BevFlag::Ok);
  EXPECT_GE(lab->sigma_be, 0.05);
  EXPECT_LE(lab->sigma_be, 1.0);
}

} // namespace
} // namespace atx::vol
