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
  // Timestamp grid is calendar==trading here; expiry is the last node.
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
  const auto p = synth_gbm_path(0.40, 126, 11u); // realizes 40 vol
  const auto r = bev_replay_pnl(p, atm_call_expiring_at(p), 0.20, {}, {}); // paid 20
  ASSERT_TRUE(r.has_value());
  EXPECT_GT(r->pnl, 0.0);
}

TEST(Breakeven, DeepItmCallExercisesBeforeLargeDividend) {
  auto p = synth_gbm_path(0.15, 60, 3u, /*s0=*/100.0, /*r=*/0.01);
  BevSpec spec{60.0, p.back().ts_ns, Side::Call}; // deep ITM
  const DividendEvent div{p[30].ts_ns + kDayNs / 2, 5.0}; // huge dividend mid-path
  const auto r = bev_replay_pnl(p, spec, 0.15, std::span(&div, 1), {});
  ASSERT_TRUE(r.has_value());
  EXPECT_TRUE(r->exercised_early);
  EXPECT_LT(r->exercise_ts_ns, div.ex_date_ns);
}

TEST(Breakeven, GoldenPathPnlIsPinned) {
  const auto p = synth_gbm_path(0.25, 126, 1234u);
  const auto r = bev_replay_pnl(p, atm_call_expiring_at(p), 0.22, {}, {});
  ASSERT_TRUE(r.has_value());
  // Pinned from the first green implementation (determinism gate).
  EXPECT_DOUBLE_EQ(r->pnl, 1.3832639438393373);
}

} // namespace
} // namespace atx::vol
