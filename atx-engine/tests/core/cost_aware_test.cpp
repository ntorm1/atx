// atx::engine::cost — Cost-aware knobs test suite (S6-4).
//
// S6-4 (C8 — cost is a decision input) maps the calibrated cost into the THREE
// existing decision points so discovery + combination price turnover honestly:
//   * the optimizer's turnover penalty κ (risk::OptimizerConfig::turnover_penalty),
//   * the gate's max_turnover (combine::GateConfig::max_turnover),
//   * a factory fitness cost-floor (CostKnobs::fitness_cost_floor),
// plus a `cost_adjusted_fitness` down-ranking that flips a high-turnover net loser
// below a low-turnover winner.
//
// These tests pin the load-bearing invariants (the constants are latitude, the
// invariants are not):
//
//   DownRanksHighTurnoverNetLoser  NON-VACUOUS: the loser's RAW fitness is >= the
//                                  winner's, so it is the calibrated-cost penalty
//                                  (proportional to turnover) that FLIPS the rank.
//   KappaScalesWithRoundTripCost   MONOTONE: a 10x impact scale yields a larger κ
//                                  (and a larger round-trip cost — the cause).
//   GateTightensWithCost           a costly universe tightens max_turnover below
//                                  the 0.70 default ceiling, but never below 0.
//
// cost_aware.hpp reuses the SIM's functional form (C6): round_trip_cost_bps
// applies the SAME temp = Y·σ·p^δ / perm = 0.5·γ·σ·p coefficients the execution
// simulator charges, evaluated at a representative (participation, sigma). It is
// NOT a second cost model.

#include <gtest/gtest.h>

#include "atx/core/types.hpp" // f64

#include "atx/engine/combine/gate.hpp"       // GateConfig
#include "atx/engine/combine/metrics.hpp"    // AlphaMetrics, kTurnoverFloor
#include "atx/engine/cost/calibration.hpp"   // CalibratedCost, FitReport
#include "atx/engine/cost/cost_aware.hpp"    // round_trip_cost_bps, cost_aware_knobs, ...
#include "atx/engine/exec/execution_sim.hpp" // ImpactCfg, SlippageCfg

namespace atxtest_cost_aware_test {

using atx::f64;
using atx::engine::combine::AlphaMetrics;
using atx::engine::combine::kTurnoverFloor;
namespace cost = atx::engine::cost;

// ---------------------------------------------------------------------------
//  Helpers.
// ---------------------------------------------------------------------------

// Build an AlphaMetrics from the four load-bearing fields; the remaining fields
// (drawdown / margin / holding_days) are set to sane derived values so the POD
// is coherent (margin = returns/floored-turnover; holding ~ 1/floored-turnover).
[[nodiscard]] AlphaMetrics metrics(f64 sharpe, f64 turnover, f64 returns,
                                   f64 fitness) noexcept {
  const f64 t = (turnover > kTurnoverFloor) ? turnover : kTurnoverFloor;
  // AlphaMetrics field order (metrics.hpp:56):
  //   {sharpe, turnover, returns, drawdown, margin, fitness, holding_days}
  return AlphaMetrics{sharpe, turnover, returns, /*drawdown*/ 0.0,
                      /*margin*/ returns / t, fitness, /*holding_days*/ 1.0 / t};
}

// Build a CalibratedCost with the given temporary-impact scale Y; δ and γ stay
// at the simulator defaults (0.5, 0.314). A larger Y is a costlier universe.
[[nodiscard]] cost::CalibratedCost calibrated_cost(f64 Y) noexcept {
  return cost::CalibratedCost{atx::engine::exec::ImpactCfg{Y, 0.5, 0.314},
                              atx::engine::exec::SlippageCfg{}, cost::FitReport{}};
}

// =============================================================================
//  Suite: CostAware
// =============================================================================

// NON-VACUOUS down-rank proof. The loser has the higher RAW fitness (raw favors
// it), so it is ONLY the turnover-proportional cost penalty that can flip the
// ranking. If the assertion below failed, the test would prove nothing.
TEST(CostAware, DownRanksHighTurnoverNetLoser) {
  const auto winner = metrics(/*sharpe*/ 2.0, /*turnover*/ 0.10, /*returns*/ 0.15,
                              /*fitness*/ 2.0); // low turnover
  const auto loser = metrics(/*sharpe*/ 2.5, /*turnover*/ 0.90, /*returns*/ 0.16,
                             /*fitness*/ 2.4); // high turnover, higher RAW fitness
  const f64 rt_cost_bps = 8.0;

  ASSERT_GE(loser.fitness, winner.fitness); // NON-VACUITY: raw favors the loser
  EXPECT_GT(cost::cost_adjusted_fitness(winner, rt_cost_bps),
            cost::cost_adjusted_fitness(loser, rt_cost_bps)); // cost flips it
}

// MONOTONE: a costlier universe (10x temporary-impact scale) derives a larger κ.
// The optional round-trip assertion makes the cause explicit.
TEST(CostAware, KappaScalesWithRoundTripCost) {
  const auto cheap = cost::cost_aware_knobs(calibrated_cost(/*Y*/ 0.5), 0.01, 0.02, 5.0);
  const auto dear = cost::cost_aware_knobs(calibrated_cost(/*Y*/ 5.0), 0.01, 0.02, 5.0);

  EXPECT_GT(dear.kappa, cheap.kappa);
  EXPECT_GT(cost::round_trip_cost_bps(calibrated_cost(5.0), 0.01, 0.02),
            cost::round_trip_cost_bps(calibrated_cost(0.5), 0.01, 0.02));
}

// A costly universe tightens the gate's max_turnover below the 0.70 default
// ceiling, but never below a sane fraction (>= 0).
TEST(CostAware, GateTightensWithCost) {
  const auto dear = cost::cost_aware_knobs(calibrated_cost(/*Y*/ 5.0), 0.01, 0.02, 5.0);

  EXPECT_LT(dear.gate.max_turnover, 0.70);            // tighter than the default ceiling
  EXPECT_GE(dear.gate.max_turnover, cost::kGateFloor); // never below the gate floor (the impl clamp)
}

// The κ knob equals rt_cost_bps / 1e4 (feeds OptimizerConfig::turnover_penalty),
// and the fitness floor equals the round-trip cost in bps. One-line pins so the
// EMITTED knob values stay tied to the documented round-trip composition.
TEST(CostAware, KnobsTieToRoundTripCost) {
  const auto cc = calibrated_cost(/*Y*/ 2.0);
  const f64 rt = cost::round_trip_cost_bps(cc, 0.01, 0.02);
  const auto knobs = cost::cost_aware_knobs(cc, 0.01, 0.02, 5.0);

  EXPECT_DOUBLE_EQ(knobs.kappa, rt / 1e4);
  EXPECT_DOUBLE_EQ(knobs.fitness_cost_floor, rt);
}

// A longer horizon tolerates more turnover: the gate ceiling widens with horizon
// (a slower-rebalancing book pays the round-trip cost less often).
TEST(CostAware, LongerHorizonWidensGate) {
  const auto cc = calibrated_cost(/*Y*/ 5.0);
  const auto fast = cost::cost_aware_knobs(cc, 0.01, 0.02, /*horizon*/ 1.0);
  const auto slow = cost::cost_aware_knobs(cc, 0.01, 0.02, /*horizon*/ 20.0);

  EXPECT_GT(slow.gate.max_turnover, fast.gate.max_turnover);
  EXPECT_LE(slow.gate.max_turnover, 0.70); // never above the default ceiling
}

// S4-3: gate_config_for_cost returns a GateConfig whose max_turnover is derived
// via max_turnover_for and whose OTHER fields keep their documented defaults.

// With a positive cost (10 bps) and a 20-day horizon the ceiling is strictly
// below the unconstrained 0.70 (the denominator > the numerator horizon factor).
TEST(CostAware, GateConfigForCostTightensMaxTurnover) {
  const auto cfg = cost::gate_config_for_cost(/*rt_cost_bps*/ 10.0, /*horizon_days*/ 20.0);

  EXPECT_LT(cfg.max_turnover, 0.70); // cost-tightened below the default ceiling
}

// With zero cost max_turnover_for returns kGateCeiling exactly (no tightening).
TEST(CostAware, GateConfigForCostZeroCostReturnsCeiling) {
  const auto cfg = cost::gate_config_for_cost(/*rt_cost_bps*/ 0.0, /*horizon_days*/ 20.0);

  EXPECT_DOUBLE_EQ(cfg.max_turnover, 0.70); // zero cost => default ceiling exactly
}

// Only max_turnover is touched; all other GateConfig fields keep their defaults.
// This proves the helper is a targeted override, not a factory for a new config.
TEST(CostAware, GateConfigForCostKeepsOtherFieldsAtDefaults) {
  const auto cfg = cost::gate_config_for_cost(/*rt_cost_bps*/ 10.0, /*horizon_days*/ 20.0);
  const atx::engine::combine::GateConfig defaults{};

  EXPECT_DOUBLE_EQ(cfg.min_sharpe,       defaults.min_sharpe);       // 0.25
  EXPECT_DOUBLE_EQ(cfg.min_fitness,      defaults.min_fitness);       // 1.0
  EXPECT_DOUBLE_EQ(cfg.max_pool_corr,    defaults.max_pool_corr);     // 0.7
  EXPECT_DOUBLE_EQ(cfg.rt_cost_bps,      defaults.rt_cost_bps);       // 0.0
  EXPECT_DOUBLE_EQ(cfg.min_holding_days, defaults.min_holding_days);  // 0.0
}


}  // namespace atxtest_cost_aware_test
