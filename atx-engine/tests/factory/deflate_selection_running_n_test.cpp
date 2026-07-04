// deflate_selection_running_n_test.cpp — p8 Sprint 5 (S5-2, sub-seam 1): feed the
// CROSS-RUN cumulative trial count into the search's own NSGA/ScalarRaw deflation
// column (SearchConfig::prior_trial_count), so the "dsr" selection signal
// (kObjDeflation / the ScalarRaw raw*=dsr haircut, active only when
// deflate_selection is set) reflects the ACTUAL number of trials a multi-run
// --library-dir sweep has accumulated, not just this run's own local canon.size().
//
// Distinct from the cascade_trial_count_test.cpp sub-seam (the cascade SKIP-BOUND,
// which LOOSENS with N and is already reconciled on main) — this file proves the
// OTHER sub-seam: the SELECTION column gets STRICTER (lower deflated raw fitness)
// as the effective N grows, because a higher N raises the expected-max-Sharpe
// benchmark SR*_N (eval::expected_max_sharpe), which lowers PSR/dsr for a fixed
// observed Sharpe.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/combine/store.hpp"
#include "atx/engine/eval/deflated_sharpe.hpp" // eval::expected_max_sharpe (the quantified claim)
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atxtest_deflate_selection_running_n {

using atx::f64;
using atx::usize;
using atx::engine::WeightPolicy;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
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
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

namespace eval = atx::engine::eval;

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

struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

[[nodiscard]] std::vector<f64> noisy_close(usize dates, usize insts, std::uint64_t seed) {
  std::vector<f64> drift(insts);
  for (usize j = 0; j < insts; ++j) {
    drift[j] = 0.006 - 0.0024 * static_cast<f64>(j);
  }
  std::vector<f64> close(dates * insts);
  std::vector<f64> px(insts, 100.0);
  Lcg rng{seed};
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      px[j] *= (1.0 + drift[j] + 0.010 * rng.next());
      close[t * insts + j] = px[j];
    }
  }
  return close;
}

[[nodiscard]] Panel fixture_panel(usize dates, usize insts) {
  const std::vector<f64> close = noisy_close(dates, insts, 0xA11Cu);
  std::vector<f64> rev(dates * insts, 0.0);
  for (usize t = 1; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      const f64 prev = close[(t - 1) * insts + j];
      rev[t * insts + j] = -(close[t * insts + j] / prev - 1.0);
    }
  }
  return make_panel(dates, insts, {"close", "rev"}, {close, rev});
}

[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)", "rank(rev)",         "ts_mean(close, 5)", "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))", "delta(close, 2)"};
}

struct Fixture {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  [[nodiscard]] SearchDriver driver() {
    return SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
  }
};

// A deterministic ScalarRaw+deflate_selection config (mirrors ElitismKeepsBest's
// pinned knobs, plus deflate_selection=true so the dsr haircut is live).
[[nodiscard]] SearchConfig deflate_cfg(atx::u64 seed, usize pop, usize gens, usize workers) {
  SearchConfig cfg;
  cfg.master_seed = seed;
  cfg.population = pop;
  cfg.generations = gens;
  cfg.elites = 2;
  cfg.k_tournament = 3;
  cfg.p_cross = 0.5;
  cfg.objective_mode = ObjectiveMode::ScalarRaw;
  cfg.enable_behavioral_novelty = false; // ScalarRaw pin requires novelty off
  cfg.seed_from_grammar = false;
  cfg.n_immigrants = 0;
  cfg.stagnation_patience = 0; // run the full budget (no early-stop shortcut)
  cfg.adaptive_operators = false;
  cfg.jitter_anneal = false;
  cfg.deflate_selection = true;
  cfg.n_workers = workers;
  return cfg;
}

// ---------------------------------------------------------------------------
// PriorTrialCountDeflatesSearchSelection — the concrete quantified S5-2 claim:
// SAME seed/panel/generations, deflate_selection=true, differing ONLY in
// prior_trial_count. The larger cross-run N raises SR*_N (eval::expected_max_
// sharpe), which lowers PSR/dsr for the SAME observed Sharpe, so the dsr-haircut
// raw fitness the search selects on is LOWER at large N — RED before the S5-2
// wire (prior_trial_count did not exist; SearchConfig had no way to express a
// cross-run N at all), GREEN after.
// ---------------------------------------------------------------------------
TEST(DeflateSelection, PriorTrialCountDeflatesSearchSelection) {
  // The quantified building block: SR*_N is strictly higher at N=100000 than at
  // the tiny in-run N a 16-genome/6-generation search reaches locally.
  const f64 sr_star_small = eval::expected_max_sharpe(20, 1.0 / 252.0);
  const f64 sr_star_large = eval::expected_max_sharpe(100000, 1.0 / 252.0);
  ASSERT_GT(sr_star_large, sr_star_small)
      << "the selection benchmark must rise with N (else this test is vacuous)";

  Fixture fx_n0;
  const SearchResult r_n0 =
      fx_n0.driver().run(deflate_cfg(/*seed*/ 42, /*pop*/ 16, /*gens*/ 6, /*workers*/ 1), AlphaStore{});
  ASSERT_FALSE(r_n0.best_fitness_per_gen.empty());

  Fixture fx_big;
  SearchConfig cfg_big = deflate_cfg(/*seed*/ 42, /*pop*/ 16, /*gens*/ 6, /*workers*/ 1);
  cfg_big.prior_trial_count = 100000;
  const SearchResult r_big = fx_big.driver().run(cfg_big, AlphaStore{});
  ASSERT_FALSE(r_big.best_fitness_per_gen.empty());

  // Compare ONLY generation 0: gen-0's population is drawn purely from
  // (master_seed, grammar seeding) — identical between the two runs regardless of
  // prior_trial_count (fitness is computed AFTER the population exists, never
  // influences which genomes are generated). So gen 0's candidate SET is
  // apples-to-apples; each candidate's raw*=dsr(N) shrinks pointwise as N grows
  // (dsr is monotone non-increasing in N for a fixed observed Sharpe/T/skew/kurt),
  // so the MAX over that identical set is also non-increasing. Generations 1+ are
  // NOT comparable this way — tournament/elitism selection reads the (now
  // different) raw values, so the two runs' populations genuinely diverge from
  // gen 1 onward and nothing orders their best-fitness trajectories.
  ASSERT_FALSE(r_n0.best_fitness_per_gen.empty());
  ASSERT_FALSE(r_big.best_fitness_per_gen.empty());
  EXPECT_LT(r_big.best_fitness_per_gen.front(), r_n0.best_fitness_per_gen.front())
      << "prior_trial_count=100000 must STRICTLY deflate gen-0's best fitness vs "
         "prior_trial_count=0 (identical gen-0 population; only the dsr haircut differs)";
  // The two runs must not be byte-identical overall (the running-N wire changed
  // the selection signal from gen 0 onward, hence generally the digest too).
  EXPECT_NE(r_n0.digest, r_big.digest);
}

// ---------------------------------------------------------------------------
// OffPathByteIdentical — prior_trial_count is IGNORED when deflate_selection is
// false (the field is read only inside that opt-in branch): a nonzero
// prior_trial_count with deflate_selection=false replays byte-identically to
// prior_trial_count=0.
// ---------------------------------------------------------------------------
TEST(DeflateSelection, OffPathByteIdenticalWhenDeflateSelectionOff) {
  auto cfg_a = deflate_cfg(/*seed*/ 7, /*pop*/ 12, /*gens*/ 4, /*workers*/ 1);
  cfg_a.deflate_selection = false;
  cfg_a.prior_trial_count = 0;

  auto cfg_b = cfg_a;
  cfg_b.prior_trial_count = 999999; // large, but deflate_selection is OFF -> must be ignored

  Fixture fx_a;
  Fixture fx_b;
  const SearchResult r_a = fx_a.driver().run(cfg_a, AlphaStore{});
  const SearchResult r_b = fx_b.driver().run(cfg_b, AlphaStore{});
  EXPECT_EQ(r_a.digest, r_b.digest);
  EXPECT_EQ(r_a.best_fitness_per_gen, r_b.best_fitness_per_gen);
}

// ---------------------------------------------------------------------------
// SeqEqualsParallel — the running-N deflation column is identical --workers 1
// vs --workers N (prior_trial_count is a per-run scalar captured serially before
// the parallel_for, exactly like canon.size() already was — the deflate_selection
// precedent).
// ---------------------------------------------------------------------------
TEST(DeflateSelection, SeqEqualsParallel) {
  SearchConfig cfg_seq = deflate_cfg(/*seed*/ 99, /*pop*/ 16, /*gens*/ 5, /*workers*/ 1);
  cfg_seq.prior_trial_count = 500;
  SearchConfig cfg_par = cfg_seq;
  cfg_par.n_workers = 4;

  Fixture fx_seq;
  Fixture fx_par;
  const SearchResult r_seq = fx_seq.driver().run(cfg_seq, AlphaStore{});
  const SearchResult r_par = fx_par.driver().run(cfg_par, AlphaStore{});

  EXPECT_EQ(r_seq.digest, r_par.digest);
  EXPECT_EQ(r_seq.best_fitness_per_gen, r_par.best_fitness_per_gen);
  EXPECT_EQ(r_seq.trial_count, r_par.trial_count);
}

} // namespace atxtest_deflate_selection_running_n
