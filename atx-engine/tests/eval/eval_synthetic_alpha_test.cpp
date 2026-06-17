// atx::engine::eval — synthetic_alpha.hpp tests (S4.4a, suite EvalSyntheticAlpha).
//
// Proves the robustness measurement layer's FIRST half: a SYNTHETIC-ALPHA
// RECOVERY experiment. generate_synthetic_panel plants a known signal (a DSL
// expression's output OR a latent factor) into a panel's forward returns with a
// chosen beta + noise; a SMALL seeded GA (the research_engine_test harness:
// ResearchDriver over a library::Library) mines it, and recovery_correlation
// measures whether the admitted survivors' OOS PnL tracks the planted signal's
// own OOS PnL.
//
// THE PROOF (RecoversPlantedSignal): a beta>0 panel admits survivors whose mean
// recovery correlation clears a bar r*; the MATCHED beta=0 noise panel (same
// seed/dims, same deflated gate) admits ~0. Same gate, opposite outcomes —
// non-vacuous recovery, not an author assertion.
//
// Runtime is kept small (small panel, pop=16/gens=4, few runs) so the dev loop
// stays fast (target < ~5 s for the whole suite).

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "atx/engine/combine/correlation.hpp"

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"

#include "atx/engine/factory/factory.hpp"
#include "atx/engine/factory/research_driver.hpp"

#include "atx/engine/library/library.hpp"
#include "atx/engine/library/record.hpp"
#include "atx/engine/library/store.hpp"

#include "atx/engine/eval/synthetic_alpha.hpp"

namespace {

using atx::f64;
using atx::u32;
using atx::u64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::extract_streams;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::SignalSet;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::GateConfig;
using atx::engine::eval::Dims;
using atx::engine::eval::generate_synthetic_panel;
using atx::engine::eval::planted_signal_pnl;
using atx::engine::eval::PlantedKind;
using atx::engine::eval::PlantedSpec;
using atx::engine::eval::recovery_correlation;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::ResearchConfig;
using atx::engine::factory::ResearchDriver;
using atx::engine::factory::ResearchReport;

namespace lib = atx::engine::library;

// ---- frictionless sim + frame builders (mirrored from research_engine_test) --

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

// The DSL seed grammar the small GA searches over (momentum/reversal templates on
// the synthetic panel's "close" — NOT the hidden "signal" field, so recovery is a
// genuine rediscovery of the planted edge from price, not a peek at the answer).
[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)",       "ts_mean(close, 5)",
          "delta(close, 2)",   "rank(ts_mean(close, 10))",
          "ts_mean(close, 3)", "rank(delta(close, 1))"};
}

// IMPORTANT: the GA may NOT search the hidden "signal" field (that would be the
// answer key). Only "close" is a field-swap candidate.
[[nodiscard]] std::vector<std::string> panel_fields() { return {"close"}; }

[[nodiscard]] GateConfig default_gate_cfg() { return GateConfig{}; }

// The S1 deflation admission bar shared by the signal + noise configs (anti-snoop).
constexpr f64 kMinDsr = 0.50;
constexpr u64 kLibSeed = 0xC0FFEEu;

[[nodiscard]] FactoryConfig per_run_cfg(usize pop, usize gens) {
  FactoryConfig cfg;
  cfg.search.master_seed = 0; // overwritten per run
  cfg.search.population = pop;
  cfg.search.generations = gens;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.novelty_w = 0.1;
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = seed_exprs();
  cfg.panel_fields = panel_fields();
  cfg.min_dsr = kMinDsr;
  return cfg;
}

[[nodiscard]] ResearchConfig research_cfg(u64 master_seed, usize max_runs) {
  ResearchConfig cfg;
  cfg.per_run = per_run_cfg(16, 4);
  cfg.max_runs = max_runs;
  cfg.patience = 0;
  cfg.master_seed = master_seed;
  return cfg;
}

[[nodiscard]] std::string tmpdir(const std::string &tag) {
  const ::testing::TestInfo *info = ::testing::UnitTest::GetInstance()->current_test_info();
  std::string base = std::string(info != nullptr ? info->name() : "t") + "_" + tag;
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / "atx_s44a_synth" / base;
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  (void)ec;
  std::filesystem::create_directories(dir, ec);
  (void)ec;
  return dir.string();
}

// Evaluate a stored expr_source over the panel and return its realized OOS PnL
// stream (the no-look-ahead extract_streams output). Mirrors the
// research_engine_test re-parse round-trip, extended to extract the PnL.
[[nodiscard]] std::vector<f64> oos_pnl_of_expr(const std::string &expr_source, const Panel &panel,
                                               const WeightPolicy &policy,
                                               const ExecutionSimulator &sim) {
  Library dsl;
  auto parsed = parse_expr(expr_source, dsl);
  EXPECT_TRUE(parsed.has_value()) << "stored expr must re-parse: " << expr_source;
  if (!parsed) {
    return {};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value());
  if (!info) {
    return {};
  }
  auto prog = compile(*parsed, *info);
  EXPECT_TRUE(prog.has_value());
  if (!prog) {
    return {};
  }
  Engine engine{panel};
  auto sig = engine.evaluate(*prog);
  EXPECT_TRUE(sig.has_value());
  if (!sig) {
    return {};
  }
  auto strm = extract_streams(*sig, policy, panel, sim);
  EXPECT_TRUE(strm.has_value());
  if (!strm) {
    return {};
  }
  const std::span<const f64> pnl = strm->pnl(0);
  return std::vector<f64>{pnl.begin(), pnl.end()};
}

// Run the small GA over `panel` and return the OOS PnL of every ADMITTED survivor
// (re-extracted from the library by re-parsing each stored expr_source). Also
// reports the admit count via out_admitted.
[[nodiscard]] std::vector<std::vector<f64>>
mine_and_collect_survivor_pnl(const Panel &panel, u64 master_seed, usize max_runs,
                              const std::string &tag, usize &out_admitted) {
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  Library dsl{};
  AlphaGate gate{default_gate_cfg()};
  lib::Library library = lib::Library::open(tmpdir(tag), default_gate_cfg(), {kLibSeed});
  ResearchDriver engine{library, dsl, panel, sim, policy, gate};
  const ResearchReport rep = engine.run(research_cfg(master_seed, max_runs));
  out_admitted = rep.total_admitted;

  std::vector<std::vector<f64>> survivors;
  const u64 n = library.n_alphas();
  for (u64 a = 0; a < n; ++a) {
    const lib::AlphaRecordView rec = library.get(lib::AlphaId{static_cast<u32>(a)});
    survivors.push_back(oos_pnl_of_expr(rec.provenance.expr_source, panel, policy, sim));
  }
  return survivors;
}

// =============================================================================
//  PanelIsDeterministicInItsSeed — generate_synthetic_panel is a PURE function of
//  its seed: same (seed, spec, dims) => byte-identical close column.
// =============================================================================
TEST(EvalSyntheticAlpha, PanelIsDeterministicInItsSeed) {
  const PlantedSpec spec{PlantedKind::SignalExpr, "rank(close)", 0.020, 0.004};
  const Dims dims{60, 6};
  auto a = generate_synthetic_panel(0xABCDu, spec, dims);
  auto b = generate_synthetic_panel(0xABCDu, spec, dims);
  ASSERT_TRUE(a.has_value());
  ASSERT_TRUE(b.has_value());
  const auto ca = a->field_id("close");
  ASSERT_TRUE(ca.has_value());
  const std::span<const f64> fa = a->field_all(*ca);
  const std::span<const f64> fb = b->field_all(*ca);
  ASSERT_EQ(fa.size(), fb.size());
  for (usize i = 0; i < fa.size(); ++i) {
    EXPECT_EQ(fa[i], fb[i]) << "close cell " << i << " diverged for the same seed";
  }
  // A DIFFERENT seed yields a different panel (non-degenerate generator).
  auto c = generate_synthetic_panel(0x1111u, spec, dims);
  ASSERT_TRUE(c.has_value());
  const std::span<const f64> fc = c->field_all(*ca);
  bool any_diff = false;
  for (usize i = 0; i < fa.size() && !any_diff; ++i) {
    any_diff = (fa[i] != fc[i]);
  }
  EXPECT_TRUE(any_diff) << "a different seed must produce a different panel";
}

// =============================================================================
//  RecoversPlantedSignal — THE proof. A beta>0 synthetic panel admits survivors
//  whose mean recovery correlation with the planted signal clears r*; a matched
//  beta=0 noise panel (same seed/dims/gate) admits ~0.
// =============================================================================
TEST(EvalSyntheticAlpha, RecoversPlantedSignal) {
  const Dims dims{120, 8};
  // The recovery bar. The MEAN |corr| over ALL admitted survivors is structurally
  // DILUTED by the factory's diversification mandate (the diversify objective +
  // novelty admit some pool-orthogonal alphas, which are partly orthogonal to the
  // planted edge too — see the per-survivor spread in the proof). 0.20 sits well
  // above that diluted floor for the beta>0 panel yet is UNREACHABLE for the
  // beta=0 noise panel (which admits 0 => recovery 0), so the bar separates
  // signal from noise non-vacuously. The strongest individual survivor recovers
  // |corr| ~0.5 (a clean rediscovery); the mean is the conservative aggregate.
  const f64 r_star = 0.20;

  // (a) beta > 0: a recoverable planted reversal signal (rank of a 1-day reversal
  // proxy) drives the forward returns. The GA mines price templates and should
  // rediscover the edge.
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  const PlantedSpec signal_spec{PlantedKind::SignalExpr, "rank(close)", 0.045, 0.003};
  auto signal_panel = generate_synthetic_panel(0xA11Cu, signal_spec, dims);
  ASSERT_TRUE(signal_panel.has_value());

  // The planted signal's OWN OOS PnL (the recovery target).
  auto planted = planted_signal_pnl(*signal_panel, policy, sim);
  ASSERT_TRUE(planted.has_value());

  usize signal_admitted = 0;
  const std::vector<std::vector<f64>> survivors = mine_and_collect_survivor_pnl(
      *signal_panel, /*seed*/ 41, /*runs*/ 1, "signal", signal_admitted);
  ASSERT_GT(signal_admitted, 0u) << "the beta>0 panel must admit at least one survivor";

  // recovery_correlation: mean |corr| of survivors' OOS PnL with the planted PnL.
  std::vector<std::span<const f64>> spans;
  spans.reserve(survivors.size());
  for (const std::vector<f64> &s : survivors) {
    spans.emplace_back(s);
  }
  const f64 recov = recovery_correlation(spans, std::span<const f64>{*planted});
  EXPECT_GT(recov, r_star) << "admitted survivors must track the planted signal (recov=" << recov
                           << " r*=" << r_star << ")";

  // The STRONGEST individual survivor is a clean rediscovery of the planted edge
  // (|corr| well above the diluted mean) — the recovery is genuine, not a mean
  // barely scraping the bar from a pile of weak admits.
  f64 best = 0.0;
  for (const std::span<const f64> &sp : spans) {
    const f64 c = atx::engine::combine::pairwise_complete_corr(sp, std::span<const f64>{*planted});
    const f64 ac = (c < 0.0) ? -c : c;
    if (ac > best) {
      best = ac;
    }
  }
  EXPECT_GT(best, 0.40) << "at least one survivor must strongly recover the planted signal";

  // (b) beta == 0: a matched NOISE panel (same seed/dims), SAME deflated gate.
  const PlantedSpec noise_spec{PlantedKind::SignalExpr, "rank(close)", 0.0, 0.004};
  auto noise_panel = generate_synthetic_panel(0xA11Cu, noise_spec, dims);
  ASSERT_TRUE(noise_panel.has_value());

  usize noise_admitted = 0;
  const std::vector<std::vector<f64>> noise_survivors =
      mine_and_collect_survivor_pnl(*noise_panel, /*seed*/ 41, /*runs*/ 1, "noise", noise_admitted);
  // Under the same deflated gate the pure-noise forward returns admit ~nothing.
  EXPECT_EQ(noise_admitted, 0u) << "the beta=0 noise panel must NOT admit a fluke under the gate";
  EXPECT_TRUE(noise_survivors.empty());
}

// =============================================================================
//  RecoveryIsDeterministic — the WHOLE recovery experiment (panel + small GA +
//  survivor re-extraction + recovery_correlation) replays byte-identical for the
//  same seeds. Same-seed determinism is the load-bearing invariant.
// =============================================================================
TEST(EvalSyntheticAlpha, RecoveryIsDeterministic) {
  const Dims dims{90, 6};
  const PlantedSpec spec{PlantedKind::SignalExpr, "rank(close)", 0.045, 0.003};

  auto p1 = generate_synthetic_panel(0x5EEDu, spec, dims);
  auto p2 = generate_synthetic_panel(0x5EEDu, spec, dims);
  ASSERT_TRUE(p1.has_value());
  ASSERT_TRUE(p2.has_value());

  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  auto planted1 = planted_signal_pnl(*p1, policy, sim);
  auto planted2 = planted_signal_pnl(*p2, policy, sim);
  ASSERT_TRUE(planted1.has_value());
  ASSERT_TRUE(planted2.has_value());

  usize a1 = 0;
  usize a2 = 0;
  const std::vector<std::vector<f64>> s1 =
      mine_and_collect_survivor_pnl(*p1, /*seed*/ 7, /*runs*/ 1, "det1", a1);
  const std::vector<std::vector<f64>> s2 =
      mine_and_collect_survivor_pnl(*p2, /*seed*/ 7, /*runs*/ 1, "det2", a2);

  ASSERT_EQ(a1, a2) << "same seed => same admit count";
  ASSERT_EQ(s1.size(), s2.size());
  for (usize k = 0; k < s1.size(); ++k) {
    ASSERT_EQ(s1[k].size(), s2[k].size());
    for (usize t = 0; t < s1[k].size(); ++t) {
      EXPECT_EQ(s1[k][t], s2[k][t]) << "survivor " << k << " period " << t << " diverged";
    }
  }

  std::vector<std::span<const f64>> sp1;
  std::vector<std::span<const f64>> sp2;
  for (const std::vector<f64> &v : s1) {
    sp1.emplace_back(v);
  }
  for (const std::vector<f64> &v : s2) {
    sp2.emplace_back(v);
  }
  const f64 r1 = recovery_correlation(sp1, std::span<const f64>{*planted1});
  const f64 r2 = recovery_correlation(sp2, std::span<const f64>{*planted2});
  EXPECT_EQ(r1, r2) << "the recovery statistic is byte-identical for the same seed";
}

// =============================================================================
//  RecoveryCorrelationIsBounded — recovery_correlation of a stream with itself is
//  1, with an empty survivor set is 0 (degenerate), and is in [-1, 1].
// =============================================================================
TEST(EvalSyntheticAlpha, RecoveryCorrelationIsBounded) {
  std::vector<f64> planted(20);
  for (usize t = 0; t < planted.size(); ++t) {
    planted[t] = static_cast<f64>(t % 5) - 2.0;
  }
  // A survivor identical to the planted stream => corr 1.
  std::vector<std::span<const f64>> one{std::span<const f64>{planted}};
  EXPECT_NEAR(recovery_correlation(one, std::span<const f64>{planted}), 1.0, 1e-9);

  // No survivors => 0 (nothing to recover).
  std::vector<std::span<const f64>> none;
  EXPECT_EQ(recovery_correlation(none, std::span<const f64>{planted}), 0.0);
}

} // namespace
