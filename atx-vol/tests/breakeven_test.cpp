#include "atx/vol/breakeven.hpp"

#include <cmath>
#include <random>
#include <vector>

#include <gtest/gtest.h>

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

} // namespace
} // namespace atx::vol
