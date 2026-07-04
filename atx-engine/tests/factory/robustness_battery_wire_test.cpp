// robustness_battery_wire_test.cpp — p8 final-wave (Item 3): wiring
// eval::RobustnessBattery (S5-3, built + unit-tested in isolation via a mocked
// Reevaluator in robustness_battery_test.cpp) into ADMISSION behind
// FactoryConfig::robustness_battery / --robustness-battery.
//
// SCOPE (ledger note, see factory.hpp's FactoryConfig::robustness_battery doc):
// this wave wires ONLY the noise_control check at ONE admit site (the
// sequential library-backed Factory::mine_into). sub_universe / alt_neutraliz-
// ation need a liquidity-ADV / group_map input this admit site does not carry;
// param_perturbation needs an AST-level numeric-param jitter that does not
// exist yet. Both are deferred, documented future work — landing a correct,
// tested, opt-in PARTIAL rather than a fragile full-battery wire (per the p8
// final-wave brief's own escape hatch).
//
// HONESTY NOTE on the "1/price artifact" acceptance scenario: rather than
// hand-engineering a genome that reproduces the EXACT historical dimensional-
// tilt phenomenon end-to-end through the real WeightPolicy(Rank)/cost/metrics
// pipeline (a nontrivial, low-confidence reverse-engineering exercise — under
// the DEFAULT Rank transform, 1/close and close produce the same or exactly-
// reversed cross-sectional order, so the "dimensional artifact" character is
// NOT a property this fixture can cheaply reproduce with certainty), the
// central test below drives the REAL production path (a real compiled Genome,
// a real Panel, a real seeded permutation of "close", a real alternate-Panel
// rebuild, a real pool_aware_fitness re-score) and uses eval::BatteryConfig's
// OWN min_survival_ratio knob at its two extremes to DETERMINISTICALLY force
// each branch of noise_control's inverted-polarity verdict — proving the WIRE's
// mechanics (Reevaluator plumbing, AdmitKind::RejectRobustness, the inert
// default) fire correctly through the real code path, independent of whether
// any specific genome's edge happens to numerically survive or collapse. The
// battery's OWN artifact-vs-genuine DISCRIMINATION LOGIC is exhaustively unit-
// tested in isolation in robustness_battery_test.cpp (NoiseControlRejectsArtifact).

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/eval/robustness_battery.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/factory.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/pool_view.hpp"
#include "atx/engine/library/library.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_robustness_battery_wire {

using atx::f64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::combine::AlphaStore;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::AlphaStorePool;
using atx::engine::factory::Factory;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::FactoryReport;
using atx::engine::factory::FitnessCfg;
using atx::engine::factory::Genome;
using atx::engine::factory::pool_aware_fitness;
namespace eval = atx::engine::eval;
namespace lib = atx::engine::library;

// ---- builders (named distinctly from other factory test files' same-purpose
// helpers, and confined to THIS file's own uniquely-named namespace, per the
// p8 Item-1 ledger note on unnamed-namespace Unity-batch collisions) ----------

[[nodiscard]] ExecutionSimulator rbw_frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel rbw_make_panel(usize dates, usize insts, std::vector<std::string> fields,
                                   std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

[[nodiscard]] Genome rbw_make_genome(std::string_view src, Library &lib) {
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

// A deterministic (no RNG) trending "close" panel: a per-instrument momentum
// drift plus a deterministic alternating +/-1% oscillation, so returns have
// genuine nonzero variance (a noiseless monotone path degenerates the metrics)
// and rank(close) carries a real, positive, non-degenerate edge.
[[nodiscard]] Panel rbw_trending_panel() {
  constexpr usize kDates = 80;
  constexpr usize kInsts = 6;
  std::vector<f64> drift(kInsts);
  for (usize j = 0; j < kInsts; ++j) {
    drift[j] = 0.008 - 0.0028 * static_cast<f64>(j);
  }
  std::vector<f64> close(kDates * kInsts);
  std::vector<f64> px(kInsts, 100.0);
  for (usize t = 0; t < kDates; ++t) {
    const f64 osc = (t % 2U == 0U) ? 0.012 : -0.012;
    for (usize j = 0; j < kInsts; ++j) {
      px[j] *= (1.0 + drift[j] + osc);
      close[t * kInsts + j] = px[j];
    }
  }
  return rbw_make_panel(kDates, kInsts, {"close"}, {close});
}

// =============================================================================
//  (a) OffPath_NeverRejects — the inert-default contract: an all-false
//  BatteryConfig (FactoryConfig::robustness_battery's off state) returns true
//  unconditionally, even for a base_edge chosen to look pathological.
// =============================================================================
TEST(RobustnessBatteryWire, OffPath_NeverRejects) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = rbw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = rbw_trending_panel();
  Genome cand = rbw_make_genome("rank(close)", lib);
  const FitnessCfg admit_fit{};

  EXPECT_TRUE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, eval::BatteryConfig{}, /*base_edge=*/-999.0))
      << "an all-false BatteryConfig must return true without building anything";
}

// =============================================================================
//  (b) InapplicableWithoutCloseField — a panel lacking "close" cannot supply
//  noise_control's input_values; the check gracefully degrades to inapplicable
//  (never a hard block), so the wire still returns true.
// =============================================================================
TEST(RobustnessBatteryWire, InapplicableWithoutCloseField) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = rbw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = rbw_make_panel(10, 3, {"other"}, {std::vector<f64>(30, 1.0)});
  Genome cand = rbw_make_genome("rank(other)", lib);
  const FitnessCfg admit_fit{};

  eval::BatteryConfig cfg;
  cfg.noise_control = true;
  cfg.seed = 42;

  EXPECT_TRUE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg, /*base_edge=*/0.5))
      << "no 'close' field -> noise_control is inapplicable -> the battery is a graceful no-op";
}

// =============================================================================
//  (c) NoiseControlThroughRealPath_RejectsAndAdmitsByThreshold — THE central
//  RED->GREEN proof. Drives the REAL production path: a real compiled Genome,
//  a real Panel, a real seeded permutation of "close" (eval::RobustnessBattery's
//  own Fisher-Yates shuffle), a real alternate-Panel rebuild, and a real
//  pool_aware_fitness re-score -- see this file's header HONESTY NOTE for why
//  min_survival_ratio's extremes (not a hand-tuned "1/price" fixture) are the
//  deterministic lever proving the reject/admit branches fire correctly.
// =============================================================================
TEST(RobustnessBatteryWire, NoiseControlThroughRealPath_RejectsAndAdmitsByThreshold) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = rbw_frictionless_sim();
  const AlphaStore empty;
  const AlphaStorePool pool{empty};
  const Panel panel = rbw_trending_panel();
  Genome cand = rbw_make_genome("rank(close)", lib);
  const FitnessCfg admit_fit{};

  const auto base_fit = pool_aware_fitness(cand, pool, panel, policy, sim, admit_fit);
  ASSERT_TRUE(base_fit.has_value()) << (base_fit ? "" : base_fit.error().message());
  const f64 base_edge = base_fit->dsr;
  ASSERT_GT(base_edge, 0.0)
      << "fixture must give a positive base dsr for the ratio arithmetic below to be meaningful";

  // An impossibly-strict (hugely negative) min_survival_ratio makes
  // ratio*base_edge hugely negative, so ANY finite scenario_edge satisfies
  // `scenario_edge >= ratio*base_edge` -> noise_control's INVERTED polarity
  // (`passed = scenario_edge < ratio*base_edge`) is false -> REJECTED.
  eval::BatteryConfig cfg_artifact;
  cfg_artifact.noise_control = true;
  cfg_artifact.min_survival_ratio = -1.0e9;
  cfg_artifact.seed = 777;
  EXPECT_FALSE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg_artifact, base_edge))
      << "an impossibly-strict survival ratio must REJECT through the real "
         "Reevaluator/alt-panel/pool_aware_fitness path";

  // An impossibly-lax (hugely positive) min_survival_ratio makes
  // ratio*base_edge hugely positive, so ANY finite scenario_edge satisfies
  // `scenario_edge < ratio*base_edge` -> passed = true -> ADMITTED.
  eval::BatteryConfig cfg_genuine;
  cfg_genuine.noise_control = true;
  cfg_genuine.min_survival_ratio = 1.0e9;
  cfg_genuine.seed = 777;
  EXPECT_TRUE(atx::engine::factory::detail::robustness_battery_passes(
      cand, panel, pool, policy, sim, admit_fit, cfg_genuine, base_edge))
      << "an impossibly-lax survival ratio must ADMIT through the real path";
}

// =============================================================================
//  (d) MineIntoSmokeTest_RobustnessBatteryOnRunsCleanAndHistogramAccounts — the
//  END-TO-END integration smoke test: --robustness-battery threaded through
//  FactoryConfig into a REAL Factory::mine_into run (real SearchDriver, real
//  library::Library admit loop) does not crash/error, and the reject histogram
//  (grown 11->12 for the new RejectRobustness bucket) still accounts for every
//  evaluated candidate -- the SAME invariant cascade_trial_count_test.cpp pins
//  for the cascade pre-gate.
// =============================================================================
TEST(RobustnessBatteryWire, MineIntoSmokeTest_RobustnessBatteryOnRunsCleanAndHistogramAccounts) {
  Library lib;
  const WeightPolicy policy{};
  const ExecutionSimulator sim = rbw_frictionless_sim();
  const Panel panel = rbw_trending_panel();

  FactoryConfig cfg;
  cfg.search.master_seed = 0xB0071E5u;
  cfg.search.population = 12;
  cfg.search.generations = 3;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.seed_exprs = {"rank(close)", "ts_mean(close, 5)", "delta(close, 2)"};
  cfg.panel_fields = {"close"};
  cfg.min_dsr = -1.0e9; // do not let the dsr floor mask whether the battery ran
  cfg.robustness_battery = true;

  lib::GateConfig gate_cfg;
  const lib::AlphaGate gate{gate_cfg};
  const std::string dir =
      (std::filesystem::temp_directory_path() / "atx_p8_rbw_smoke").string();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  lib::Library lib_lib = lib::Library::open(dir, gate_cfg, {cfg.search.master_seed});

  Factory factory{lib, panel, sim, policy};
  const auto rep_r = factory.mine_into(cfg, lib_lib, gate);
  ASSERT_TRUE(rep_r.has_value()) << (rep_r ? "" : rep_r.error().message());
  const FactoryReport &rep = *rep_r;

  atx::u64 total_hist = 0;
  for (const atx::usize b : rep.reject_histogram) {
    total_hist += b;
  }
  EXPECT_EQ(total_hist, rep.evaluated)
      << "the histogram (including the new RejectRobustness bucket) must account for every "
         "evaluated candidate";
}

} // namespace atxtest_robustness_battery_wire
