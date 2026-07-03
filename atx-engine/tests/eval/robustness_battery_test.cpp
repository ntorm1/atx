// robustness_battery_test.cpp — p8 Sprint 5 (S5-3): the automated robustness
// battery. RED before this unit: no robustness harness existed in eval/ at all
// (grep atx-engine/include/atx/engine/eval/ found no robustness_battery.hpp) —
// the degenerate `1/price` alpha passed fitness/Sharpe/turnover/corr/DSR/PBO
// because none of those probe whether the edge is a dimensional artifact or
// survives on randomized inputs. GREEN: RobustnessBattery::run flags exactly
// that class via the noise-control negative control, plus three complementary
// survival/stability checks.
//
// Each test drives the battery with a SYNTHETIC Reevaluator that reports a
// controlled edge per ScenarioKind — the battery itself is engine-agnostic (it
// never touches a real Genome/Panel/VM), so a hand-built lambda is the correct,
// direct way to unit-test its orchestration/threshold logic in isolation.

#include <algorithm>
#include <cmath>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/eval/robustness_battery.hpp"

namespace atxtest_robustness_battery {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::eval::BatteryConfig;
using atx::engine::eval::BatteryResult;
using atx::engine::eval::CandidateInputs;
using atx::engine::eval::RobustnessBattery;
using atx::engine::eval::RobustnessScenario;
using atx::engine::eval::ScenarioKind;

// ---------------------------------------------------------------------------
// AllChecksOff_NoOp — the all-false config is a pure no-op: `ran==false`, and
// the Reevaluator is NEVER invoked (a call-counting mock proves it).
// ---------------------------------------------------------------------------
TEST(RobustnessBattery, AllChecksOff_NoOp) {
  int calls = 0;
  auto reeval = [&](const RobustnessScenario &) -> atx::core::Result<f64> {
    ++calls;
    return atx::core::Ok(1.0);
  };
  CandidateInputs inputs;
  inputs.base_edge = 0.8;
  const BatteryResult res = RobustnessBattery::run(BatteryConfig{}, inputs, reeval);
  EXPECT_FALSE(res.ran);
  EXPECT_TRUE(res.overall_pass);
  EXPECT_EQ(calls, 0) << "an all-false config must never call the Reevaluator";
}

// ---------------------------------------------------------------------------
// NoiseControlRejectsArtifact — the central S5-3 claim. A constructed `1/price`-
// style signal: its edge is a DIMENSIONAL artifact, so it SURVIVES on the noise-
// replacement control (a permutation of the input destroys any genuine
// time-series/cross-sectional structure but preserves a purely-dimensional
// relationship this mock stands in for) -> the battery REJECTS it. A genuine
// cross-sectional signal collapses on noise -> PASSES the control.
// ---------------------------------------------------------------------------
TEST(RobustnessBattery, NoiseControlRejectsArtifact) {
  BatteryConfig cfg;
  cfg.noise_control = true;
  cfg.min_survival_ratio = 0.5;
  cfg.seed = 12345;

  std::vector<f64> input_values(64);
  for (usize i = 0; i < input_values.size(); ++i) {
    input_values[i] = 1.0 / (100.0 + static_cast<f64>(i)); // stand-in for a raw-close-derived column
  }

  // Artifact: edge is INDEPENDENT of ORDER (a pure function of the multiset of
  // values — exactly what "dimensional artifact" means: it survives any
  // permutation of the input, including the noise-control permutation).
  CandidateInputs inputs_artifact;
  inputs_artifact.base_edge = 0.90;
  inputs_artifact.input_values = input_values;
  auto reeval_artifact = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    if (sc.kind == ScenarioKind::NoiseControl) {
      return atx::core::Ok(0.88); // survives noise (barely reduced) -> an artifact
    }
    return atx::core::Ok(0.90);
  };
  const BatteryResult art = RobustnessBattery::run(cfg, inputs_artifact, reeval_artifact);
  ASSERT_TRUE(art.ran);
  ASSERT_TRUE(art.noise_control.applicable);
  EXPECT_FALSE(art.noise_control.passed) << "an edge surviving noise must be REJECTED";
  EXPECT_FALSE(art.overall_pass);

  // Genuine: edge depends on the REAL time-series/cross-sectional structure, so
  // permuting the input collapses it toward zero.
  CandidateInputs inputs_genuine;
  inputs_genuine.base_edge = 0.90;
  inputs_genuine.input_values = input_values;
  auto reeval_genuine = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    if (sc.kind == ScenarioKind::NoiseControl) {
      return atx::core::Ok(0.05); // collapses on noise -> genuine
    }
    return atx::core::Ok(0.90);
  };
  const BatteryResult gen = RobustnessBattery::run(cfg, inputs_genuine, reeval_genuine);
  ASSERT_TRUE(gen.ran);
  ASSERT_TRUE(gen.noise_control.applicable);
  EXPECT_TRUE(gen.noise_control.passed) << "an edge that collapses on noise must PASS";
  EXPECT_TRUE(gen.overall_pass);
}

// Inapplicable when no input_values are supplied (graceful degradation, not a
// hard failure): the check is skipped and never drags down overall_pass.
TEST(RobustnessBattery, NoiseControlInapplicableWithoutInputValues) {
  BatteryConfig cfg;
  cfg.noise_control = true;
  CandidateInputs inputs; // input_values left empty
  inputs.base_edge = 0.5;
  int calls = 0;
  auto reeval = [&](const RobustnessScenario &) -> atx::core::Result<f64> {
    ++calls;
    return atx::core::Ok(0.5);
  };
  const BatteryResult res = RobustnessBattery::run(cfg, inputs, reeval);
  EXPECT_TRUE(res.ran);
  EXPECT_FALSE(res.noise_control.applicable);
  EXPECT_TRUE(res.overall_pass) << "an inapplicable check must not fail the battery";
  EXPECT_EQ(calls, 0) << "no Reevaluator call for a check with no usable input";
}

// ---------------------------------------------------------------------------
// SubUniverseCollapseRejected — an edge concentrated in a handful of illiquid
// names falls below min_survival_ratio on the TOP-N (by ADV) universe -> reject;
// a broad edge survives.
// ---------------------------------------------------------------------------
TEST(RobustnessBattery, SubUniverseCollapseRejected) {
  BatteryConfig cfg;
  cfg.sub_universe = true;
  cfg.min_survival_ratio = 0.5;
  cfg.sub_universe_top_n = 4; // restrict to the 4 most liquid of 10 names

  std::vector<f64> adv(10);
  for (usize i = 0; i < adv.size(); ++i) {
    adv[i] = static_cast<f64>(10 - i); // instrument 0 is the most liquid
  }
  CandidateInputs inputs;
  inputs.base_edge = 1.0;
  inputs.adv = adv;

  // Concentrated: the edge lives in illiquid names outside the top-4 -> collapses.
  auto reeval_concentrated = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    if (sc.kind == ScenarioKind::SubUniverse) {
      return atx::core::Ok(0.1); // far below 0.5 * 1.0
    }
    return atx::core::Ok(1.0);
  };
  const BatteryResult concentrated = RobustnessBattery::run(cfg, inputs, reeval_concentrated);
  ASSERT_TRUE(concentrated.sub_universe.applicable);
  EXPECT_FALSE(concentrated.sub_universe.passed);
  EXPECT_FALSE(concentrated.overall_pass);

  // Broad: the edge is roughly uniform across the universe -> survives.
  auto reeval_broad = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    if (sc.kind == ScenarioKind::SubUniverse) {
      return atx::core::Ok(0.95);
    }
    return atx::core::Ok(1.0);
  };
  const BatteryResult broad = RobustnessBattery::run(cfg, inputs, reeval_broad);
  ASSERT_TRUE(broad.sub_universe.applicable);
  EXPECT_TRUE(broad.sub_universe.passed);
  EXPECT_TRUE(broad.overall_pass);
}

// ---------------------------------------------------------------------------
// AltNeutralizationRemovesTilt — a pure sector tilt collapses under the
// alternate group_map; an idiosyncratic signal passes.
// ---------------------------------------------------------------------------
TEST(RobustnessBattery, AltNeutralizationRemovesTilt) {
  BatteryConfig cfg;
  cfg.alt_neutralization = true;
  cfg.min_survival_ratio = 0.5;
  cfg.seed = 777;

  std::vector<u32> group_id = {0, 0, 0, 1, 1, 1, 2, 2, 2, 2};
  CandidateInputs inputs;
  inputs.base_edge = 0.8;
  inputs.group_id = group_id;

  auto reeval_tilt = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    if (sc.kind == ScenarioKind::AltNeutralization) {
      return atx::core::Ok(0.02); // the edge WAS the group tilt -> collapses
    }
    return atx::core::Ok(0.8);
  };
  const BatteryResult tilt = RobustnessBattery::run(cfg, inputs, reeval_tilt);
  ASSERT_TRUE(tilt.alt_neutralization.applicable);
  EXPECT_FALSE(tilt.alt_neutralization.passed);
  EXPECT_FALSE(tilt.overall_pass);

  auto reeval_idio = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    if (sc.kind == ScenarioKind::AltNeutralization) {
      return atx::core::Ok(0.75); // idiosyncratic edge survives group residualization
    }
    return atx::core::Ok(0.8);
  };
  const BatteryResult idio = RobustnessBattery::run(cfg, inputs, reeval_idio);
  ASSERT_TRUE(idio.alt_neutralization.applicable);
  EXPECT_TRUE(idio.alt_neutralization.passed);
  EXPECT_TRUE(idio.overall_pass);
}

// alt_neutralization's scenario carries a PERMUTATION of group_id (same
// multiset of labels, i.e. group SIZES preserved) — not an arbitrary relabeling.
TEST(RobustnessBattery, AltNeutralizationPermutesGroupIdPreservingMultiset) {
  BatteryConfig cfg;
  cfg.alt_neutralization = true;
  cfg.seed = 55;
  std::vector<u32> group_id = {0, 0, 1, 1, 1, 2};
  CandidateInputs inputs;
  inputs.base_edge = 0.5;
  inputs.group_id = group_id;

  std::vector<u32> captured;
  auto reeval = [&](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    if (sc.kind == ScenarioKind::AltNeutralization) {
      captured = sc.alt_group_id;
    }
    return atx::core::Ok(0.5);
  };
  const BatteryResult res = RobustnessBattery::run(cfg, inputs, reeval);
  ASSERT_TRUE(res.ran);
  ASSERT_EQ(captured.size(), group_id.size());
  std::vector<u32> sorted_captured = captured;
  std::vector<u32> sorted_original = group_id;
  std::sort(sorted_captured.begin(), sorted_captured.end());
  std::sort(sorted_original.begin(), sorted_original.end());
  EXPECT_EQ(sorted_captured, sorted_original)
      << "the alternate group map must be a PERMUTATION (same label multiset)";
}

// ---------------------------------------------------------------------------
// ParamPerturbationStable — a knife-edge candidate (edge only at one param
// value) is rejected (high coefficient of variation across jittered draws); a
// stable candidate passes.
// ---------------------------------------------------------------------------
TEST(RobustnessBattery, ParamPerturbationStable) {
  BatteryConfig cfg;
  cfg.param_perturbation = true;
  cfg.param_perturbation_draws = 12;
  cfg.param_perturbation_band = 0.10;
  cfg.param_perturbation_max_cv = 0.25;
  cfg.seed = 2026;

  CandidateInputs inputs;
  inputs.base_edge = 0.7;

  // Knife-edge: edge swings wildly with the jittered scale (a sharp, narrow peak
  // at scale==1.0 that collapses at any perturbation).
  auto reeval_knife_edge = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    const f64 dist = std::abs(sc.param_scale - 1.0);
    return atx::core::Ok(dist < 1e-9 ? 0.9 : 0.9 * std::exp(-400.0 * dist * dist));
  };
  const BatteryResult knife = RobustnessBattery::run(cfg, inputs, reeval_knife_edge);
  ASSERT_TRUE(knife.param_perturbation.applicable);
  EXPECT_FALSE(knife.param_perturbation.passed) << "a knife-edge candidate must be REJECTED";

  // Stable: edge barely moves across the jitter band.
  auto reeval_stable = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    return atx::core::Ok(0.7 + 0.01 * (sc.param_scale - 1.0));
  };
  const BatteryResult stable = RobustnessBattery::run(cfg, inputs, reeval_stable);
  ASSERT_TRUE(stable.param_perturbation.applicable);
  EXPECT_TRUE(stable.param_perturbation.passed) << "a stable candidate must PASS";
}

// ---------------------------------------------------------------------------
// Deterministic_TwiceRun — identical BatteryResult across two runs with the same
// seed (the Reevaluator itself reads the RNG-derived scenario contents, so this
// proves the FULL pipeline — scenario construction + aggregation — is
// reproducible, not just a constant-returning mock). No internal parallel_for
// exists in the battery (every check is a small, fixed sequential reduction —
// see the header's determinism note), so reproducibility across "workers" is
// vacuous by construction; this test is the load-bearing determinism proof.
// ---------------------------------------------------------------------------
TEST(RobustnessBattery, Deterministic_TwiceRun) {
  BatteryConfig cfg;
  cfg.sub_universe = true;
  cfg.alt_neutralization = true;
  cfg.noise_control = true;
  cfg.param_perturbation = true;
  cfg.seed = 4242;
  cfg.param_perturbation_draws = 6;

  std::vector<f64> adv = {5.0, 3.0, 8.0, 1.0, 9.0, 2.0};
  std::vector<u32> group_id = {0, 1, 0, 1, 2, 2};
  std::vector<f64> input_values = {1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0};

  CandidateInputs inputs;
  inputs.base_edge = 0.6;
  inputs.adv = adv;
  inputs.group_id = group_id;
  inputs.input_values = input_values;

  // A Reevaluator whose output is a deterministic function of the scenario's OWN
  // (possibly RNG-derived) contents — a real end-to-end determinism exercise.
  auto reeval = [](const RobustnessScenario &sc) -> atx::core::Result<f64> {
    switch (sc.kind) {
    case ScenarioKind::SubUniverse: {
      f64 s = 0.0;
      for (const auto i : sc.keep_instruments) s += static_cast<f64>(i);
      return atx::core::Ok(0.5 + 0.01 * s);
    }
    case ScenarioKind::AltNeutralization: {
      f64 s = 0.0;
      for (const auto g : sc.alt_group_id) s += static_cast<f64>(g);
      return atx::core::Ok(0.5 + 0.01 * s);
    }
    case ScenarioKind::NoiseControl: {
      f64 s = 0.0;
      for (const auto x : sc.noise_input) s += x;
      return atx::core::Ok(0.001 * s);
    }
    case ScenarioKind::ParamPerturbation:
      return atx::core::Ok(0.6 * sc.param_scale);
    case ScenarioKind::Baseline:
      return atx::core::Ok(0.6);
    }
    return atx::core::Ok(0.0);
  };

  const BatteryResult r1 = RobustnessBattery::run(cfg, inputs, reeval);
  const BatteryResult r2 = RobustnessBattery::run(cfg, inputs, reeval);

  EXPECT_EQ(r1.ran, r2.ran);
  EXPECT_EQ(r1.overall_pass, r2.overall_pass);
  EXPECT_EQ(r1.sub_universe.scenario_edge, r2.sub_universe.scenario_edge);
  EXPECT_EQ(r1.alt_neutralization.scenario_edge, r2.alt_neutralization.scenario_edge);
  EXPECT_EQ(r1.noise_control.scenario_edge, r2.noise_control.scenario_edge);
  EXPECT_EQ(r1.param_perturbation.scenario_edge, r2.param_perturbation.scenario_edge);
}

} // namespace atxtest_robustness_battery
