// fitness_turnover_test.cpp — p7-S4-4: the tradeable-profile turnover helpers.
//
// Two opt-in, header-only free functions in factory/fitness.hpp:
//   * tradeable_fitness_cfg()        — a FitnessCfg with the recommended tradeable
//                                      defaults (turnover_penalty_slope=2.0,
//                                      max_turnover_target=0.20); all other fields
//                                      the inert FitnessCfg{} defaults.
//   * turnover_target_from_gate(L)   — derive a max_turnover_target from a gate
//                                      cost_max_turnover threshold L (L for L>0,
//                                      +inf for L<=0) so the search penalty is
//                                      coherent with admission.
// Both are OPT-IN: the default FitnessCfg{} is unchanged, so every existing golden
// digest stays byte-identical (the boundary pin). This unit also pins the penalty
// multiplier at three turnover levels against the formula in finish_report
// (fitness.cpp), driven by the tradeable defaults.
//
// Suite: FitnessTurnover
//   (a) DefaultCfgUnchanged_OffPathPin — FitnessCfg{} defaults still inert (0.0 /
//       +inf) so no existing digest moves; the reviewer gate (empty fitness.cpp
//       diff) + the factory byte-identity slice complete class (a).
//   (b) TradeableCfgAndPenaltyFormula  — the cfg carries the named constants AND
//       finish_report's mult matches the formula at turnover {0.10, 0.30, 0.60};
//       turnover_target_from_gate(0.25)==0.25 and (0.0)==+inf.
//   (c) TwiceRunBitIdentical            — tradeable_fitness_cfg() is pure (two calls
//       bit-identical).
//   (d) ThreadSafe                      — no shared mutable state; concurrent calls
//       are identical.

#include <cmath>   // std::isinf
#include <cstring> // std::memcmp (bitwise identity)
#include <limits>  // std::numeric_limits
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/factory/fitness.hpp"

namespace atxtest_fitness_turnover_test {

using atx::f64;
using atx::engine::factory::FitnessCfg;
using atx::engine::factory::FitnessReport;
using atx::engine::factory::kTradeableMaxTurnover;
using atx::engine::factory::kTradeableTurnoverSlope;
using atx::engine::factory::tradeable_fitness_cfg;
using atx::engine::factory::turnover_target_from_gate;
using atx::engine::factory::detail::finish_report;
using atx::engine::factory::detail::FitnessCore;

// A FitnessCore wired so finish_report's raw == the turnover-penalty multiplier:
// wq=1, robust=1, and (with redundancy=0 -> diversify=1) raw = 1*1*1*mult = mult.
// Only `turnover` varies; every other field is inert for this isolation.
[[nodiscard]] FitnessCore core_with_turnover(f64 turnover) {
  FitnessCore c{};
  c.oos_pnl = {};
  c.wq = 1.0;
  c.robust = 1.0;
  c.dsr = 0.0;
  c.haircut_sharpe = 0.0;
  c.cost_bps = 0.0;
  c.turnover = turnover;
  c.sharpe_h1 = 0.0;
  c.sharpe_h2 = 0.0;
  c.split_stable = false;
  return c;
}

// ===========================================================================
//  (a) Off-path pin: the DEFAULT FitnessCfg{} is unchanged — slope 0.0, target
//      +inf — so the penalty branch is never entered and no existing digest moves.
//      (Full byte-identity is the factory *Oracle*:*Golden*:*Digest* slice +
//      the reviewer's empty src/factory/fitness.cpp diff.)
// ===========================================================================
TEST(FitnessTurnover, DefaultCfgUnchanged_OffPathPin) {
  const FitnessCfg def{};
  EXPECT_EQ(def.turnover_penalty_slope, 0.0) << "default slope must stay inert (0.0)";
  EXPECT_TRUE(std::isinf(def.max_turnover_target) && def.max_turnover_target > 0.0)
      << "default target must stay +inf (no excess ever)";

  // With the default cfg, finish_report applies NO penalty (slope==0 -> branch
  // skipped): raw == wq*diversify*robust == 1.0 even at a huge turnover.
  const FitnessReport rep = finish_report(core_with_turnover(5.0), /*redundancy=*/0.0,
                                          /*cost_active=*/false, def);
  EXPECT_EQ(rep.raw, 1.0) << "default cfg must leave raw unpenalised (boundary pin)";
}

// ===========================================================================
//  (b) tradeable_fitness_cfg() carries the named constants, the penalty multiplier
//      matches the formula at three turnover levels, and the gate->target helper is
//      coherent.
//
//  Formula (finish_report / fitness.cpp): with target=0.20, slope=2.0,
//    excess = max(0, turnover - 0.20); slack = max(0.20*2.0, 1e-12) = 0.40;
//    mult   = clamp(1 - excess/slack, 0.0, 1.0).
//    turnover=0.10 -> excess 0    -> mult 1.00
//    turnover=0.30 -> excess 0.10 -> mult 1 - 0.10/0.40 = 0.75
//    turnover=0.60 -> excess 0.40 -> mult 1 - 0.40/0.40 = 0.00 (kFloor)
// ===========================================================================
TEST(FitnessTurnover, TradeableCfgAndPenaltyFormula) {
  const FitnessCfg cfg = tradeable_fitness_cfg();

  // The named tradeable constants (not magic numbers).
  EXPECT_EQ(kTradeableTurnoverSlope, 2.0);
  EXPECT_EQ(kTradeableMaxTurnover, 0.20);
  EXPECT_EQ(cfg.turnover_penalty_slope, kTradeableTurnoverSlope);
  EXPECT_EQ(cfg.max_turnover_target, kTradeableMaxTurnover);
  // Every OTHER field is the FitnessCfg{} default (opt-in: only the two knobs move).
  const FitnessCfg def{};
  EXPECT_EQ(cfg.trial_count, def.trial_count);
  EXPECT_EQ(cfg.book_size, def.book_size);
  EXPECT_EQ(cfg.target_aum, def.target_aum);

  // The penalty multiplier at the three plan-specified turnover levels (raw == mult
  // because wq=robust=diversify=1). Exact to 1e-12 against the closed form.
  const FitnessReport r_lo = finish_report(core_with_turnover(0.10), 0.0, false, cfg);
  const FitnessReport r_mid = finish_report(core_with_turnover(0.30), 0.0, false, cfg);
  const FitnessReport r_hi = finish_report(core_with_turnover(0.60), 0.0, false, cfg);
  EXPECT_NEAR(r_lo.raw, 1.00, 1e-12) << "turnover below target -> no penalty";
  EXPECT_NEAR(r_mid.raw, 0.75, 1e-12) << "turnover=target+0.10 -> 25% haircut";
  EXPECT_NEAR(r_hi.raw, 0.00, 1e-12) << "turnover=target+0.40 -> floored at kFloor";

  // Gate->target coherence: a positive gate limit passes through; a non-positive
  // limit means "no gate" -> +inf (the inert no-target value).
  EXPECT_EQ(turnover_target_from_gate(0.25), 0.25);
  EXPECT_TRUE(std::isinf(turnover_target_from_gate(0.0)) && turnover_target_from_gate(0.0) > 0.0);
  EXPECT_TRUE(std::isinf(turnover_target_from_gate(-1.0)) && turnover_target_from_gate(-1.0) > 0.0)
      << "a negative gate limit is also 'no gate' -> +inf";
}

// ===========================================================================
//  (c) tradeable_fitness_cfg() is a pure function — two calls are bit-identical.
//      Compare the penalty-relevant fields bitwise (the struct has non-trivial
//      members like CalibratedCost, so we compare the two scalar knobs that the
//      helper sets, plus the defaults it must preserve).
// ===========================================================================
TEST(FitnessTurnover, TwiceRunBitIdentical) {
  const FitnessCfg a = tradeable_fitness_cfg();
  const FitnessCfg b = tradeable_fitness_cfg();
  const f64 a_knobs[2] = {a.turnover_penalty_slope, a.max_turnover_target};
  const f64 b_knobs[2] = {b.turnover_penalty_slope, b.max_turnover_target};
  EXPECT_EQ(std::memcmp(a_knobs, b_knobs, sizeof(a_knobs)), 0)
      << "tradeable_fitness_cfg() must be bit-deterministic";
  EXPECT_EQ(a.target_aum, b.target_aum);
  EXPECT_EQ(a.book_size, b.book_size);
}

// ===========================================================================
//  (d) Thread-safe: no shared mutable state. Concurrent calls produce identical
//      knob values from both threads.
// ===========================================================================
TEST(FitnessTurnover, ThreadSafe) {
  f64 slope_a = 0.0, target_a = 0.0, slope_b = 0.0, target_b = 0.0;
  std::thread t1([&] {
    const FitnessCfg c = tradeable_fitness_cfg();
    slope_a = c.turnover_penalty_slope;
    target_a = c.max_turnover_target;
  });
  std::thread t2([&] {
    const FitnessCfg c = tradeable_fitness_cfg();
    slope_b = c.turnover_penalty_slope;
    target_b = c.max_turnover_target;
  });
  t1.join();
  t2.join();
  EXPECT_EQ(slope_a, slope_b);
  EXPECT_EQ(target_a, target_b);
  EXPECT_EQ(slope_a, kTradeableTurnoverSlope);
  EXPECT_EQ(target_a, kTradeableMaxTurnover);
}

} // namespace atxtest_fitness_turnover_test
