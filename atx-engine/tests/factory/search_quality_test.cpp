// atx-engine/tests/factory/search_quality_test.cpp
#include <algorithm>
#include <gtest/gtest.h>
#include <vector>
#include <string>
#include "atx/core/types.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/factory/search_progress.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atx::engine::factory {
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::WeightPolicy;
using atx::engine::combine::AlphaStore;

// --- Fixture (mirrors factory_search_driver_test.cpp) ---------------------
namespace {
ExecutionSimulator frictionless_sim() {
  using namespace atx::engine::exec;
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{}, VolumeCapCfg{1.0}};
}
struct Lcg { std::uint64_t s;
  double next() noexcept { s = s*6364136223846793005ULL + 1442695040888963407ULL;
    return 2.0*(static_cast<double>(s>>11U)/static_cast<double>(1ULL<<53U)) - 1.0; } };
Panel fixture_panel(atx::usize dates, atx::usize insts) {
  std::vector<double> drift(insts);
  for (atx::usize j=0;j<insts;++j) drift[j]=0.006-0.0024*static_cast<double>(j);
  std::vector<double> close(dates*insts), px(insts,100.0); Lcg rng{0xA11Cu};
  for (atx::usize t=0;t<dates;++t) for (atx::usize j=0;j<insts;++j){
    px[j]*=(1.0+drift[j]+0.010*rng.next()); close[t*insts+j]=px[j]; }
  std::vector<double> rev(dates*insts,0.0);
  for (atx::usize t=1;t<dates;++t) for (atx::usize j=0;j<insts;++j){
    rev[t*insts+j] = -(close[t*insts+j]/close[(t-1)*insts+j]-1.0); }
  auto r = Panel::create(dates, insts, {"close","rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value()); return std::move(r.value());
}
std::vector<std::string> seed_exprs() {
  return {"rank(close)","rank(rev)","ts_mean(close, 5)","ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))","delta(close, 2)"}; }
struct Fixture {
  Library lib{}; Panel panel = fixture_panel(96,6); WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver driver() { return SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close","rev"}}; }
};
SearchConfig base_cfg(atx::u64 seed) {
  SearchConfig c; c.master_seed=seed; c.population=16; c.generations=4;
  c.elites=2; c.k_tournament=3; c.p_cross=0.5; return c; }
} // namespace

// --- CountingSink — captures gen-0 distinct population size via progress sink --
namespace {
struct CountingSink : SearchProgressSink {
  std::vector<atx::usize> distinct_per_gen;
  [[nodiscard]] atx::core::Status on_generation(const GenerationSnapshot &s) override {
    std::vector<std::string> pop = s.population;
    std::sort(pop.begin(), pop.end());
    pop.erase(std::unique(pop.begin(), pop.end()), pop.end());
    distinct_per_gen.push_back(pop.size());
    return atx::core::Ok();
  }
};
} // namespace

// Step 1 (RED): with today's cycle-fill, gen-0 has only ~6 distinct structures
// (seed count). After ramped init (Task 3), grammar fill diversifies slots above
// the seed floor and distinct >= population/2 == 12.
TEST(RampedInit, GenZeroIsMostlyDistinct) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pool{};
  SearchConfig cfg = base_cfg(31337);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.population = 24; cfg.generations = 1;
  cfg.seed_from_grammar = true;                 // ramped grammar fill (the new default)
  CountingSink sink;
  (void)d.run(cfg, pool, &sink);
  ASSERT_FALSE(sink.distinct_per_gen.empty());
  // Gen-0 should be far more diverse than the seed count (6 seeds): expect the
  // grammar fill to push distinct structures well above the seed floor.
  EXPECT_GE(sink.distinct_per_gen.front(), cfg.population / 2);
}

// Behavioral novelty now toggles via enable_behavioral_novelty (NOT novelty_w).
TEST(SearchNoveltyKnob, BehavioralNoveltyTogglesDigest) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pool_on{}, pool_off{};
  SearchConfig on = base_cfg(777);  on.objective_mode = ObjectiveMode::MultiObjective;
  on.enable_behavioral_novelty = true;
  SearchConfig off = on; off.enable_behavioral_novelty = false;
  const auto r_on  = d.run(on,  pool_on);
  const auto r_off = d.run(off, pool_off);
  // Behavioral novelty is a live 4th objective when ON -> different survivor set/digest.
  EXPECT_NE(r_on.digest, r_off.digest);
}

// Parsimony pressure: with it ON, the final survivors are no LARGER (total node
// count) than with it OFF, on the same seed/panel — and strictly smaller on at
// least one run where bloat occurs. Asserted as: mean survivor node-count(ON) <=
// mean survivor node-count(OFF).
TEST(Parsimony, ShrinksOrTiesSurvivors) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pool_on{}, pool_off{};
  SearchConfig on = base_cfg(4242);
  on.objective_mode = ObjectiveMode::MultiObjective;
  on.generations = 6; on.enable_parsimony = true;
  SearchConfig off = on; off.enable_parsimony = false;
  const auto r_on  = d.run(on,  pool_on);
  const auto r_off = d.run(off, pool_off);
  auto mean_nodes = [](const std::vector<Genome>& gs) {
    if (gs.empty()) return 0.0; atx::usize tot=0;
    for (auto& g : gs) tot += g.ast.nodes().size();
    return static_cast<double>(tot)/static_cast<double>(gs.size()); };
  EXPECT_LE(mean_nodes(r_on.admitted_candidates), mean_nodes(r_off.admitted_candidates) + 1e-9);
}

// Immigrants change the trajectory vs none (same seed) -> different digest.
TEST(Immigrant, ChangesTrajectory) {
  Fixture fx; auto d = fx.driver();
  AlphaStore p0{}, p1{};
  SearchConfig none = base_cfg(909); none.objective_mode = ObjectiveMode::MultiObjective;
  none.generations = 5; none.n_immigrants = 0; none.stagnation_patience = 0;
  SearchConfig some = none; some.n_immigrants = 3;
  const auto r0 = d.run(none, p0);
  const auto r1 = d.run(some, p1);
  EXPECT_NE(r0.digest, r1.digest);
}

// Stagnation stop: with patience small, a run on a trivially-converging config
// records FEWER generations than the budget. Use 1 seed so the population
// collapses fast, patience=2, generations=20.
TEST(StagnationStop, StopsEarly) {
  Library lib; Panel panel = fixture_panel(96,6); WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();
  SearchDriver d{lib, panel, policy, sim, {"rank(close)"}, {"close","rev"}};
  AlphaStore pool{};
  SearchConfig cfg = base_cfg(5);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.seed_from_grammar = false;   // single seed -> immediate convergence
  cfg.n_immigrants = 0;
  cfg.generations = 20; cfg.stagnation_patience = 2;
  const auto r = d.run(cfg, pool);
  EXPECT_LT(r.best_fitness_per_gen.size(), cfg.generations);
}

// Stagnation stop with patience=0 (disabled) must run the full budget.
// This validates the "0 disables" contract and that the dual-metric check
// doesn't fire when patience=0 is the guard.
TEST(StagnationStop, DisabledRunsFullBudget) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pool{};
  SearchConfig cfg = base_cfg(42);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.seed_from_grammar = true;
  cfg.n_immigrants = 4;
  cfg.generations = 8;
  cfg.stagnation_patience = 0;  // disabled
  const auto r = d.run(cfg, pool);
  EXPECT_EQ(r.best_fitness_per_gen.size(), cfg.generations);
}

// Stagnation stop: when population is single-seed (collapses immediately),
// patience=2 must fire before the full budget. A collapsed population has
// BOTH best_raw AND mean_raw flat, satisfying the new dual-metric criterion.
TEST(StagnationStop, CollapsedPopulationStopsEarlyWithDualMetric) {
  Library lib; Panel panel = fixture_panel(96,6); WeightPolicy policy;
  ExecutionSimulator sim = frictionless_sim();
  // Single seed expression + no grammar fill + no immigrants -> immediate
  // population collapse (all genomes identical -> mean_raw == best_raw == flat).
  SearchDriver d{lib, panel, policy, sim, {"rank(close)"}, {"close","rev"}};
  AlphaStore pool{};
  SearchConfig cfg = base_cfg(7);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.seed_from_grammar = false;
  cfg.n_immigrants = 0;
  cfg.generations = 20;
  cfg.stagnation_patience = 2;
  const auto r = d.run(cfg, pool);
  // Both metrics flat -> stagnation fires -> fewer gens than the budget.
  EXPECT_LT(r.best_fitness_per_gen.size(), cfg.generations);
}

// Adaptive operators are deterministic (replay) AND change the trajectory vs the
// fixed-uniform draw on the same seed.
TEST(AdaptiveOperator, DeterministicAndDistinct) {
  Fixture fx; auto d = fx.driver();
  AlphaStore pa{}, pb{}, pf{};
  SearchConfig adapt = base_cfg(2024);
  adapt.objective_mode = ObjectiveMode::MultiObjective;
  adapt.generations = 6; adapt.n_immigrants = 0; adapt.stagnation_patience = 0;
  adapt.adaptive_operators = true; adapt.jitter_anneal = true;
  SearchConfig fixed = adapt; fixed.adaptive_operators = false; fixed.jitter_anneal = false;
  const auto a1 = d.run(adapt, pa);
  const auto a2 = d.run(adapt, pb);
  const auto f  = d.run(fixed, pf);
  EXPECT_EQ(a1.digest, a2.digest);   // F1 replay holds with adaptation on
  EXPECT_NE(a1.digest, f.digest);    // adaptation actually changes the search
}
} // namespace atx::engine::factory
