// atx::engine::cost — Calibration test suite (S6-1).
//
// The calibration entry points (cost/calibration.hpp) recover the impact
// coefficients (Y, δ, γ) from realized observations and emit an exec::ImpactCfg
// plus an auditable FitReport. These tests pin the four load-bearing behaviours:
//
//   C4 (headline recovery)   injected (Y, δ, γ) are recovered within tolerance
//                            from synthetic observations.
//   C1 (honest fit quality)  the FitReport carries a real R², a positive
//                            δ-stderr, and the observation count.
//   C2 (fit/apply firewall)  a trailing-window fit is truncation-invariant:
//                            adding future fills cannot move the coefficients
//                            for a closed window.
//   M8 (NaN-safe rows)       degenerate observations (p<=0) are dropped from the
//                            design, never poisoning it with NaN/Inf.
//
// The γ-bearing recovery drives calibrate_from_obs (perm_shift_frac is an
// explicit observation). The firewall drives calibrate_impact over a real fill
// stream + Market, where perm is unobserved and γ stays at the prior.

#include <gtest/gtest.h>

#include <array>
#include <cmath>   // std::isfinite, std::pow
#include <vector>

#include "atx/core/hash.hpp"   // hash_bytes (C2 truncation-invariance)
#include "atx/core/random.hpp" // Xoshiro256pp (synthetic fixtures only)
#include "atx/core/types.hpp"  // f64, i64, u32, u64, usize

#include "atx/engine/cost/calibration.hpp"   // calibrate_from_obs, calibrate_impact
#include "atx/engine/exec/execution_sim.hpp" // ImpactCfg
#include "atx/engine/exec/payloads.hpp"      // FillPayload
#include "atx/engine/loop/market.hpp"        // Market, InstrumentStats
#include "atx/engine/loop/types.hpp"         // InstrumentId

namespace {

using atx::f64;
using atx::i64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::exec::FillPayload;
using atx::engine::exec::ImpactCfg;
namespace cost = atx::engine::cost;

[[nodiscard]] InstrumentId inst(u32 id) noexcept { return InstrumentId{id}; }

// ---------------------------------------------------------------------------
//  Synthetic observation generator. Each row is built from the EXACT structural
//  laws the calibration inverts:
//    temp = Y · σ · p^δ          (the power law, log-linear in (logY, δ))
//    perm = 0.5 · γ · σ · p      (linear through the structure)
//  with a deterministic per-row participation sweep and a light multiplicative
//  noise so the fit has something non-trivial to recover.
// ---------------------------------------------------------------------------
[[nodiscard]] std::vector<cost::CostObs>
synthetic_obs(f64 Y, f64 delta, f64 gamma, usize n, u64 seed) {
  atx::core::Xoshiro256pp rng{seed};
  std::vector<cost::CostObs> obs;
  obs.reserve(n);
  for (usize i = 0; i < n; ++i) {
    const f64 sigma = 0.02;
    const f64 p = rng.uniform(0.001, 0.10); // participation in (0.1%, 10%)
    const f64 temp = Y * sigma * std::pow(p, delta) * (1.0 + 0.02 * rng.normal());
    const f64 perm = 0.5 * gamma * sigma * p * (1.0 + 0.02 * rng.normal());
    obs.push_back(cost::CostObs{p, sigma, temp, perm});
  }
  return obs;
}

// Like synthetic_obs but a fraction of rows carry participation == 0 (a
// degenerate, log-undefined row that MUST be dropped, not allowed to produce a
// NaN in the design).
[[nodiscard]] std::vector<cost::CostObs>
synthetic_obs_with_zero_participation(f64 Y, f64 delta, f64 gamma, usize n) {
  auto obs = synthetic_obs(Y, delta, gamma, n, 11ULL);
  for (usize i = 0; i < n; i += 5) {
    obs[i].participation = 0.0; // every 5th row is degenerate
  }
  return obs;
}

// Hash the three calibrated impact coefficients (C2 byte-identity over a closed
// window).
[[nodiscard]] u64 hash_impact(const ImpactCfg& c) {
  const std::array<f64, 3> buf{c.Y, c.delta, c.gamma};
  return atx::core::hash_bytes(buf.data(), buf.size() * sizeof(f64));
}

// =============================================================================
//  Suite: Calibration
// =============================================================================

// C4 — the headline. Inject (Y, δ, γ) into synthetic observations, run the fit,
// recover all three within tolerance.
TEST(Calibration, RecoversInjectedSyntheticCoefficients) {
  const f64 Y = 0.8, delta = 0.55, gamma = 0.3;
  auto obs = synthetic_obs(Y, delta, gamma, /*n=*/2000, /*seed=*/5);
  auto cc = cost::calibrate_from_obs(obs, ImpactCfg{});
  EXPECT_NEAR(cc.impact.Y, Y, 0.05);
  EXPECT_NEAR(cc.impact.delta, delta, 0.03);
  EXPECT_NEAR(cc.impact.gamma, gamma, 0.05);
}

// C1 — honest fit quality. The report carries a high R² on a clean fit, a
// strictly positive δ-stderr, and the exact observation count.
TEST(Calibration, ReportsHonestFitQuality) {
  auto obs = synthetic_obs(0.8, 0.55, 0.3, 2000, 5);
  auto cc = cost::calibrate_from_obs(obs, ImpactCfg{});
  EXPECT_GT(cc.report.r2_temp, 0.8);
  EXPECT_GT(cc.report.delta_stderr, 0.0);
  EXPECT_GT(cc.report.Y_stderr, 0.0);
  EXPECT_EQ(cc.report.n_fills, 2000u);
}

// C2 — the fit/apply firewall. A trailing window over a short prefix and over a
// longer prefix that only ADDS future fills must produce identical coefficients
// (future bars cannot change a closed window). calibrate_impact reads
// participation/σ from the Market stats; perm is unobserved so γ stays at prior.
TEST(Calibration, FitOnTrailing_TruncationInvariant) {
  // Universe + per-instrument stats (ADV, σ, spread). One name suffices.
  std::array<InstrumentId, 1> uni{inst(7)};
  std::array<InstrumentStats, 1> stats{InstrumentStats{/*adv=*/1.0e6, /*sigma=*/0.02, /*spread=*/0.05}};
  Market mkt{std::span<const InstrumentId>{uni}, std::span<const InstrumentStats>{stats}};

  // The trailing window is a timestamp cutoff: only fills with t < window enter
  // the design. Build a closed in-window set, then a longer set that appends
  // strictly-later fills (which the cutoff must exclude).
  const i64 window = 1000;
  atx::core::Xoshiro256pp rng{99ULL};
  std::vector<FillPayload> in_window;
  for (i64 k = 0; k < window; k += 1) {
    FillPayload f{};
    f.id = inst(7);
    f.qty = static_cast<i64>(rng.uniform(100.0, 50000.0));
    f.impact = 1.0 * 0.02 * std::pow(static_cast<f64>(f.qty) / 1.0e6, 0.5);
    f.t = atx::core::time::Timestamp::from_unix_nanos(k);
    in_window.push_back(f);
  }
  std::vector<FillPayload> longer = in_window;
  for (i64 k = window; k < window + 500; ++k) {
    FillPayload f{};
    f.id = inst(7);
    f.qty = static_cast<i64>(rng.uniform(100.0, 50000.0));
    f.impact = 1.0 * 0.02 * std::pow(static_cast<f64>(f.qty) / 1.0e6, 0.5);
    f.t = atx::core::time::Timestamp::from_unix_nanos(k); // strictly later
    longer.push_back(f);
  }

  auto a = cost::calibrate_impact(std::span<const FillPayload>{in_window}, mkt,
                                  static_cast<usize>(window), ImpactCfg{});
  auto b = cost::calibrate_impact(std::span<const FillPayload>{longer}, mkt,
                                  static_cast<usize>(window), ImpactCfg{});
  EXPECT_EQ(hash_impact(a.impact), hash_impact(b.impact));
}

// M8 — NaN policy. Degenerate (participation==0) rows are dropped from the
// design; the recovered δ is finite and clamped in-range.
TEST(Calibration, DegenerateRow_NaNSafe_NotInDesign) {
  auto obs = synthetic_obs_with_zero_participation(0.8, 0.55, 0.3, 500);
  auto cc = cost::calibrate_from_obs(obs, ImpactCfg{});
  EXPECT_TRUE(std::isfinite(cc.impact.delta));
  EXPECT_GE(cc.impact.delta, 0.3);
  EXPECT_LE(cc.impact.delta, 0.9);
  EXPECT_TRUE(std::isfinite(cc.impact.Y));
  EXPECT_TRUE(std::isfinite(cc.impact.gamma));
}

// Boundary — an all-degenerate observation set yields the prior unchanged (no
// rows in the design), not a NaN. δ stays at the prior's δ.
TEST(Calibration, AllDegenerate_FallsBackToPrior) {
  std::vector<cost::CostObs> obs(50, cost::CostObs{/*p=*/0.0, /*σ=*/0.02,
                                                   /*temp=*/0.01, /*perm=*/0.0});
  const ImpactCfg prior{};
  auto cc = cost::calibrate_from_obs(obs, prior);
  EXPECT_DOUBLE_EQ(cc.impact.delta, prior.delta);
  EXPECT_DOUBLE_EQ(cc.impact.gamma, prior.gamma);
  EXPECT_EQ(cc.report.n_fills, 0u);
}

} // namespace
