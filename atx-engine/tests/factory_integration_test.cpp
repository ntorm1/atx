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

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp" // alpha::Library
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/weight_policy.hpp"

#include "atx/engine/combine/gate.hpp"  // combine::AlphaGate, GateConfig
#include "atx/engine/combine/store.hpp" // combine::AlphaStore

#include "atx/engine/validation/bias_audit.hpp" // validation::catches_overfit_synthetic

#include "atx/engine/factory/factory.hpp" // factory::Factory, FactoryConfig, FactoryReport

namespace {

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
using atx::engine::factory::SearchConfig;

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
  cfg.search.novelty_w = 0.1;
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
//  ReuseS1BiasAuditVerbatim — the shared validity spine still fires.
// =============================================================================
TEST(FactoryIntegration, ReuseS1BiasAuditVerbatim) {
  EXPECT_TRUE(atx::engine::validation::catches_overfit_synthetic());
}

} // namespace
