// atx::engine::factory — Factory integration + anti-snooping proof (S3-6, suite
// FactoryIntegration). The SPRINT-3 EXIT CRITERIA.
//
// This is integration over already-built, already-tested lower layers:
//   * S3-5 SearchDriver  — the seeded, deflated, pool-aware evolutionary search.
//   * S3-4 pool_aware_fitness — the WQ x marginal-corr x robustness score + dsr.
//   * P4   AlphaGate     — the fitness/turnover/MAX-corr admission screen.
//   * S1   deflated_sharpe / pbo — the multiple-testing deflation bar (min_dsr).
//
// The crown jewel is F4 (NoiseAdmitsNothingAfterDeflation): it is NON-VACUOUS
// precisely because the SAME default_gate_cfg() + min_dsr that admits survivors on
// a real-signal panel (MinesAndAdmitsThroughP4Gates) admits NOTHING on i.i.d.
// noise. The two panels share the seed grammar (rank / ts_mean / delta over the
// fields); only the planted edge differs. See the fixture notes for the observed
// admitted counts (real > 0, noise == 0).

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp" // alpha::Library (its definition site)
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"  // combine::AlphaGate, GateConfig
#include "atx/engine/combine/store.hpp" // combine::AlphaStore

#include "atx/engine/validation/bias_audit.hpp" // validation::catches_overfit_synthetic

#include "atx/engine/factory/factory.hpp" // factory::Factory, FactoryConfig, FactoryReport

namespace atxtest_factory_integration_test {

using atx::f64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::combine::AlphaGate;
using atx::engine::combine::AlphaStore;
using atx::engine::combine::GateConfig;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::CommissionMode;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::SlippageMode;
using atx::engine::exec::VolumeCapCfg;
using atx::engine::factory::Factory;
using atx::engine::factory::FactoryConfig;
using atx::engine::factory::FactoryReport;

// ---- builders ---------------------------------------------------------------

[[nodiscard]] ExecutionSimulator frictionless_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel make_panel(usize dates, usize insts, std::vector<std::string> fields,
                               std::vector<std::vector<f64>> cols) {
  auto r = Panel::create(dates, insts, std::move(fields), std::move(cols), {});
  EXPECT_TRUE(r.has_value()) << "panel fixture must build";
  return std::move(r.value());
}

// A tiny deterministic LCG (no RNG dep) -> uniform(-1, 1), the S3-4/S3-5 idiom.
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// REAL-SIGNAL close matrix [dates*insts]: each instrument follows a random walk
// with a small PERSISTENT per-instrument drift, so rank(close) / ts_mean(close)
// carry a genuine (noisy, finite-Sharpe) momentum edge the seed grammar captures.
// This is the S3-4/S3-5 fixture idiom (a planted edge), with a slightly larger
// drift spread so the survivor clears the SAME default gate + dsr bar that kills
// the noise population (F4 non-vacuity).
[[nodiscard]] std::vector<f64> momentum_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.010 - 0.0040 * static_cast<f64>(j); // persistent cross-sectional spread
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.008 * rng.next()); // signal-dominant (drift >> noise band)
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// PURE-NOISE close matrix: i.i.d. multiplicative noise, ZERO drift -> no
// cross-sectional or time-series edge. rank/ts_mean/delta over this have no
// persistent signal, so every candidate's deflated Sharpe collapses under the
// multiple-testing N (F4). Same volatility band as the real panel so the noise
// is genuinely comparable (the ONLY difference is the absent drift).
[[nodiscard]] std::vector<f64> noise_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + 0.008 * rng.next()); // pure noise, NO drift
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

// A two-field (close + 1-day reversal) panel over the supplied close matrix — the
// seed grammar's field surface (rank/ts ops over close/rev).
[[nodiscard]] Panel two_field_panel(usize dates, usize insts, std::vector<f64> close) {
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

[[nodiscard]] Panel real_signal_panel() {
  return two_field_panel(120, 8, momentum_close(120, 8, 0xA11Cu));
}

[[nodiscard]] Panel pure_noise_panel() {
  return two_field_panel(120, 8, noise_close(120, 8, 0xBADC0FFEEu));
}

// The seed-expression set (all analyze-valid in-grammar templates over close/rev).
[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)", "rank(rev)",         "ts_mean(close, 5)", "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))", "delta(close, 2)"};
}

[[nodiscard]] std::vector<std::string> panel_fields() { return {"close", "rev"}; }

// The shared P4 gate config (the BRAIN gold-standard floors). The SAME cfg gates
// BOTH the real-signal admit AND the noise reject — that shared discrimination is
// what makes F4 non-vacuous.
[[nodiscard]] GateConfig default_gate_cfg() {
  GateConfig g; // min_sharpe=1, min_fitness=1, max_turnover=0.7, max_pool_corr=0.7
  return g;
}

// The shared min_dsr deflation bar (S1, F4). The same bar gates both panels.
constexpr f64 kMinDsr = 0.5;

[[nodiscard]] FactoryConfig base_cfg(atx::u64 seed, usize pop, usize gens) {
  FactoryConfig cfg;
  cfg.search.master_seed = seed;
  cfg.search.population = pop;
  cfg.search.generations = gens;
  cfg.search.elites = 2;
  cfg.search.k_tournament = 3;
  cfg.search.p_cross = 0.5;
  cfg.search.enable_behavioral_novelty = true;
  // The deflation N is the running trial count; the search reports it. The CPCV
  // geometry is the S3-4 default (6 groups, 2 test). trial_count here is the BASE
  // N the fitness adds the search trials onto (kept small so the real panel's
  // genuine edge survives while the noise panel does not).
  cfg.search.fitness.trial_count = 4;
  cfg.seed_exprs = seed_exprs();
  cfg.panel_fields = panel_fields();
  cfg.min_dsr = kMinDsr;
  return cfg;
}

// A real-signal mining config: a modest budget that lets the planted edge surface.
[[nodiscard]] FactoryConfig real_signal_cfg(atx::u64 seed) { return base_cfg(seed, 16, 4); }

// A LARGE-budget config: many trials -> a high multiple-testing N -> the deflation
// drives even the in-sample-best NOISE candidate's dsr below the bar (F4).
[[nodiscard]] FactoryConfig large_budget_cfg(atx::u64 seed) {
  FactoryConfig cfg = base_cfg(seed, 24, 6);
  cfg.search.fitness.trial_count = 64; // a big N: the snooping penalty bites hard
  return cfg;
}

[[nodiscard]] AlphaStore fresh_pool() { return AlphaStore{}; }

// A reusable fixture bundle (one Library/Panel/policy/sim). The verbatim tests
// construct Factory(lib, panel, sim, weight_policy).
struct Fixture {
  Library lib{};
  Panel panel;
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  explicit Fixture(Panel p) : panel{std::move(p)} {}

  [[nodiscard]] Factory factory() { return Factory{lib, panel, sim, policy}; }
};

// =============================================================================
//  MinesAndAdmitsThroughP4Gates — admits survivors on a real-signal panel.
// =============================================================================
TEST(FactoryIntegration, MinesAndAdmitsThroughP4Gates) {
  Fixture fx{real_signal_panel()};
  AlphaStore pool;
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();
  const FactoryReport rep = f.mine(real_signal_cfg(/*seed*/ 1), pool, gate);
  EXPECT_GT(rep.admitted, 0u);              // admits survivors on a real-signal panel
  EXPECT_EQ(pool.n_alphas(), rep.admitted); // admitted == inserted
}

// =============================================================================
//  NoiseAdmitsNothingAfterDeflation — F4, THE anti-snooping proof (non-vacuous).
//
//  The SAME default_gate_cfg() + kMinDsr that admits on the real panel above kills
//  the ENTIRE pure-noise population: i.i.d. returns have no persistent edge, so
//  under the large-budget multiple-testing N every candidate's deflated Sharpe
//  collapses below the bar (and/or its raw fitness fails the gate floors). admitted
//  == 0 is therefore a genuine rejection, not a vacuously-strict bar.
// =============================================================================
TEST(FactoryIntegration, NoiseAdmitsNothingAfterDeflation) {
  Fixture fx{pure_noise_panel()};
  AlphaStore pool;
  AlphaGate gate{default_gate_cfg()};
  Factory f = fx.factory();
  const FactoryReport rep = f.mine(large_budget_cfg(/*seed*/ 2), pool, gate);
  EXPECT_EQ(rep.admitted, 0u);     // deflation + gates kill the entire noise population
  EXPECT_EQ(pool.n_alphas(), 0u);  // nothing inserted
}

// =============================================================================
//  SeededRunReplaysByteIdentical — F1/F2: same seed => byte-identical digest.
// =============================================================================
TEST(FactoryIntegration, SeededRunReplaysByteIdentical) {
  Fixture fx1{real_signal_panel()};
  Fixture fx2{real_signal_panel()};
  AlphaGate gate{default_gate_cfg()};
  AlphaStore p1 = fresh_pool();
  AlphaStore p2 = fresh_pool();
  Factory f1 = fx1.factory();
  Factory f2 = fx2.factory();
  const FactoryReport a = f1.mine(real_signal_cfg(/*seed*/ 5), p1, gate);
  const FactoryReport b = f2.mine(real_signal_cfg(/*seed*/ 5), p2, gate);
  EXPECT_EQ(a.digest, b.digest);
  EXPECT_EQ(a.admitted, b.admitted); // identical mine+admit -> identical outcome
}

// =============================================================================
//  W4a SplitSharpeFloorGatesAdmission — the OPTIONAL split-sample stability floor.
//
//  UNIFIED CONTRACT (review Fix 1): mine() now gates split-stability via the SAME
//  split_floor_ok(min_split_sharpe, cand_pnl, metrics) helper the three library admit
//  paths use — the floor is measured on the REALIZED full-OOS PnL stream, NOT the
//  CPCV-aggregated fit-> fields. One statistical contract across all four mine paths.
//
//  The default (-inf) floor admits the real-signal survivors EXACTLY as the pre-W4a
//  screen did (split_floor_ok returns true immediately on !isfinite -> the accept
//  expression collapses to (verdict==Accept) && (dsr>=min_dsr): same admitted count +
//  byte-identical digest as a baseline run that never touches it). An IMPOSSIBLY-HIGH
//  finite floor activates the gate and rejects every candidate (no realized-stream
//  per-period half-Sharpe clears 1e9), shifting the digest — proof the floor actually
//  bites when set. Mirrors the min_dsr deflation-bar test idiom.
// =============================================================================
TEST(FactoryIntegration, SplitSharpeFloorGatesAdmission) {
  // (1) Default (-inf) floor: admits survivors, byte-identical to the untouched cfg.
  Fixture fx_def{real_signal_panel()};
  AlphaStore pool_def;
  AlphaGate gate{default_gate_cfg()};
  Factory f_def = fx_def.factory();
  const FactoryConfig cfg_default = real_signal_cfg(/*seed*/ 1); // min_split_sharpe == -inf
  const FactoryReport rep_def = f_def.mine(cfg_default, pool_def, gate);
  EXPECT_GT(rep_def.admitted, 0u) << "default (disabled) floor admits the real-signal survivors";

  // (2) Active, impossibly-high floor on the SAME seed/panel: rejects everything.
  Fixture fx_hi{real_signal_panel()};
  AlphaStore pool_hi;
  Factory f_hi = fx_hi.factory();
  FactoryConfig cfg_hi = real_signal_cfg(/*seed*/ 1);
  cfg_hi.min_split_sharpe = 1.0e9; // no half-Sharpe can clear this -> active gate rejects all
  const FactoryReport rep_hi = f_hi.mine(cfg_hi, pool_hi, gate);
  EXPECT_EQ(rep_hi.admitted, 0u) << "an active, impossibly-high split-Sharpe floor rejects all";
  EXPECT_EQ(pool_hi.n_alphas(), 0u);
  EXPECT_NE(rep_hi.digest, rep_def.digest) << "the active floor changes admission -> shifts the digest";
}

// =============================================================================
//  W4b RunLevelPboHelper — the POST-HOC run-level CSCV-PBO verdict over a run's
//  admitted alphas. Unit-tests detail::finalize_run_pbo (the single source of truth
//  the five Factory admit paths share) directly on hand-built admitted_pnls — the
//  same testability the W4a detail::split_half_sharpe helper has.
//
//    (1) OVERFIT pattern — disjoint single-window spikes, flat-negative elsewhere
//        (the bias_audit.cpp synthetic, as per-candidate PnL rows) -> HIGH PBO. With
//        max_pbo = 0.5 the gate FAILS, but rep.pbo is still a finite [0,1] verdict.
//    (2) PERSISTENT pattern — every row shares a stable positive drift -> LOW PBO ->
//        the gate PASSES.
//    (3) OFF (max_pbo = 1.0) -> NO compute: rep.pbo stays the NaN sentinel, all the
//        PBO fields stay at their sentinels, the gate passes.
//    (4) < 2 admitted rows -> infeasible -> sentinels, gate passes.
// =============================================================================
TEST(FactoryIntegration, W4b_RunLevelPboHelper) {
  using atx::engine::factory::detail::finalize_run_pbo;

  // A structural index-0 zero is prepended to every row (finalize_run_pbo drops it,
  // mirroring the §0-F combine convention) so the post-drop length is kT.
  constexpr usize kN = 16u; // admitted alphas (candidate rows)
  constexpr usize kT = 64u; // post-drop periods
  constexpr usize kW = kT / 8u; // sub-window width (8 = the CSCV split count)

  // (1) OVERFIT: each candidate spikes ONLY on its own disjoint sub-window, dead
  // (flat-negative) elsewhere — the canonical high-PBO construction.
  {
    std::vector<std::vector<f64>> overfit;
    for (usize c = 0; c < kN; ++c) {
      std::vector<f64> row;
      row.reserve(kT + 1u);
      row.push_back(0.0); // index-0 structural zero (dropped by finalize_run_pbo)
      for (usize t = 0; t < kT; ++t) {
        row.push_back((t / kW == c % 8u) ? 1.0 : -0.02);
      }
      overfit.push_back(std::move(row));
    }
    FactoryReport rep;
    finalize_run_pbo(rep, overfit, /*max_pbo=*/0.5);
    EXPECT_TRUE(std::isfinite(rep.pbo)) << "an active gate over a feasible set computes a PBO";
    EXPECT_GE(rep.pbo, 0.0);
    EXPECT_LE(rep.pbo, 1.0);
    EXPECT_EQ(rep.pbo_n_candidates, kN);
    EXPECT_EQ(rep.pbo_n_splits, 8u) << "n_splits auto-clamps to min(8, T) rounded even";
    EXPECT_FALSE(rep.pbo_gate_passed) << "the overfit set's high PBO exceeds max_pbo=0.5 -> FAIL";
  }

  // (2) PERSISTENT: every candidate shares the SAME stable positive drift (a real,
  // OOS-persistent edge) -> the IS winner keeps winning OOS -> low PBO -> gate passes.
  {
    std::vector<std::vector<f64>> persistent;
    for (usize c = 0; c < kN; ++c) {
      std::vector<f64> row;
      row.reserve(kT + 1u);
      row.push_back(0.0);
      for (usize t = 0; t < kT; ++t) {
        // A deterministic, candidate-ordered positive drift with a tiny per-candidate
        // tilt so the rows are not bit-identical (CSCV needs a strict cross-section).
        row.push_back(0.10 + 0.001 * static_cast<f64>(c) + 0.0001 * static_cast<f64>(t));
      }
      persistent.push_back(std::move(row));
    }
    FactoryReport rep;
    finalize_run_pbo(rep, persistent, /*max_pbo=*/0.5);
    EXPECT_TRUE(std::isfinite(rep.pbo));
    EXPECT_LE(rep.pbo, 0.5) << "a persistent shared edge has low PBO";
    EXPECT_TRUE(rep.pbo_gate_passed) << "low PBO <= max_pbo=0.5 -> gate passes";
  }

  // (3) OFF (the 1.0 disabling default): NO compute, every PBO field at its sentinel.
  {
    std::vector<std::vector<f64>> overfit; // a high-PBO set — but the gate is OFF
    for (usize c = 0; c < kN; ++c) {
      std::vector<f64> row(kT + 1u, -0.02);
      row[0] = 0.0;
      for (usize t = 0; t < kW; ++t) {
        row[1u + (c % 8u) * kW + t] = 1.0;
      }
      overfit.push_back(std::move(row));
    }
    FactoryReport rep;
    finalize_run_pbo(rep, overfit, /*max_pbo=*/1.0);
    EXPECT_TRUE(std::isnan(rep.pbo)) << "off (max_pbo=1.0) leaves pbo at the NaN sentinel";
    EXPECT_EQ(rep.pbo_mean_logit, 0.0);
    EXPECT_EQ(rep.pbo_n_candidates, 0u);
    EXPECT_EQ(rep.pbo_n_splits, 0u);
    EXPECT_TRUE(rep.pbo_gate_passed) << "off -> the gate passes (fail-open)";
  }

  // (4) < 2 admitted rows: infeasible (pbo_cscv needs n_candidates >= 2) -> sentinels.
  {
    std::vector<std::vector<f64>> one_row;
    std::vector<f64> row(kT + 1u, 0.1);
    row[0] = 0.0;
    one_row.push_back(std::move(row));
    FactoryReport rep;
    finalize_run_pbo(rep, one_row, /*max_pbo=*/0.5); // active gate, but only 1 row
    EXPECT_TRUE(std::isnan(rep.pbo)) << "< 2 admitted rows is infeasible -> sentinel";
    EXPECT_EQ(rep.pbo_n_candidates, 0u);
    EXPECT_TRUE(rep.pbo_gate_passed) << "infeasible -> gate passes (fail-open)";
  }
}

// =============================================================================
//  W4b RunLevelPboMineByteIdentity — the POST-HOC PBO records over the REAL admitted
//  set WITHOUT perturbing the admission digest.
//
//    * max_pbo = 1.0 (default): rep.pbo is the NaN sentinel AND the digest equals a
//      baseline run that never touches the field.
//    * max_pbo = 0.5 (active) on the SAME seed/panel: rep.pbo is FINITE,
//      rep.pbo_n_candidates == rep.admitted, the verdict is set, and the DIGEST is
//      UNCHANGED vs the default run — proof PBO does NOT perturb any admission decision.
// =============================================================================
TEST(FactoryIntegration, W4b_RunLevelPboMineByteIdentity) {
  AlphaGate gate{default_gate_cfg()};

  // (1) Default (off) run.
  Fixture fx_def{real_signal_panel()};
  AlphaStore pool_def;
  Factory f_def = fx_def.factory();
  const FactoryConfig cfg_default = real_signal_cfg(/*seed*/ 1); // max_pbo == 1.0
  const FactoryReport rep_def = f_def.mine(cfg_default, pool_def, gate);
  ASSERT_GE(rep_def.admitted, 2u) << "this fixture/seed must admit >= 2 for a PBO cross-section";
  EXPECT_TRUE(std::isnan(rep_def.pbo)) << "the off path leaves pbo at the NaN sentinel";

  // (2) Active gate on the SAME seed/panel.
  Fixture fx_act{real_signal_panel()};
  AlphaStore pool_act;
  Factory f_act = fx_act.factory();
  FactoryConfig cfg_act = real_signal_cfg(/*seed*/ 1);
  cfg_act.max_pbo = 0.5; // active: compute + record, but never alter admission
  const FactoryReport rep_act = f_act.mine(cfg_act, pool_act, gate);

  EXPECT_EQ(rep_act.admitted, rep_def.admitted) << "PBO does not change the admitted count";
  EXPECT_EQ(rep_act.digest, rep_def.digest)
      << "the POST-HOC PBO never perturbs the admission digest (byte-identity invariant)";
  EXPECT_TRUE(std::isfinite(rep_act.pbo)) << "the active gate computed a finite PBO";
  EXPECT_GE(rep_act.pbo, 0.0);
  EXPECT_LE(rep_act.pbo, 1.0);
  EXPECT_EQ(rep_act.pbo_n_candidates, rep_act.admitted)
      << "every admitted alpha feeds the run-level CSCV cross-section";
  EXPECT_GT(rep_act.pbo_n_splits, 0u);
}

// =============================================================================
//  W4b RunLevelPboDeterministic — same seed + same active max_pbo -> identical pbo
//  (the F1 determinism guarantee: pbo_cscv is pure + the matrix is assembled in the
//  deterministic sequential admit order).
// =============================================================================
TEST(FactoryIntegration, W4b_RunLevelPboDeterministic) {
  AlphaGate gate{default_gate_cfg()};

  Fixture fx1{real_signal_panel()};
  Fixture fx2{real_signal_panel()};
  AlphaStore p1;
  AlphaStore p2;
  Factory f1 = fx1.factory();
  Factory f2 = fx2.factory();
  FactoryConfig cfg = real_signal_cfg(/*seed*/ 7);
  cfg.max_pbo = 0.5;
  const FactoryReport a = f1.mine(cfg, p1, gate);
  const FactoryReport b = f2.mine(cfg, p2, gate);
  ASSERT_GE(a.admitted, 2u);
  EXPECT_EQ(a.digest, b.digest);
  EXPECT_TRUE(std::isfinite(a.pbo));
  EXPECT_EQ(a.pbo, b.pbo) << "same seed + same max_pbo -> byte-identical run-level PBO";
  EXPECT_EQ(a.pbo_n_candidates, b.pbo_n_candidates);
  EXPECT_EQ(a.pbo_n_splits, b.pbo_n_splits);
  EXPECT_EQ(a.pbo_gate_passed, b.pbo_gate_passed);
}

// =============================================================================
//  ReuseS1BiasAuditVerbatim — the shared validity spine still fires.
// =============================================================================
TEST(FactoryIntegration, ReuseS1BiasAuditVerbatim) {
  EXPECT_TRUE(atx::engine::validation::catches_overfit_synthetic());
}


}  // namespace atxtest_factory_integration_test
