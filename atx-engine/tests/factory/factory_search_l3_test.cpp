// atx::engine::factory — L3 SearchDriver wiring tests (suite FactorySearchL3):
// semantic canonical dedup and output-fingerprint dedup. Both flags default OFF
// and must then be byte-identical; ON they must stay worker-count invariant.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace atx_test_l3_search_driver_l3 {

using atx::f64;
using atx::u64;
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
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;

[[nodiscard]] ExecutionSimulator zero_cost_sim() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel walk_panel(usize dates, usize insts) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> rev(dates * insts, 0.0);
  std::vector<f64> px(insts, 100.0);
  std::uint64_t s = 0xBEEFULL;
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      const f64 u = 2.0 * (static_cast<f64>(s >> 11U) / static_cast<f64>(1ULL << 53U)) - 1.0;
      px[j] *= 1.0 + (0.003 - 0.0006 * static_cast<f64>(j)) + 0.01 * u;
      close[t * insts + j] = px[j];
      if (t > 0) {
        rev[t * insts + j] = -(px[j] / close[(t - 1) * insts + j] - 1.0);
      }
    }
  }
  auto r = Panel::create(dates, insts, {"close", "rev"}, {close, rev}, {});
  EXPECT_TRUE(r.has_value());
  return std::move(r.value());
}

struct Fx {
  Library lib{};
  Panel panel = walk_panel(128, 10);
  WeightPolicy policy{};
  ExecutionSimulator sim = zero_cost_sim();

  [[nodiscard]] SearchDriver driver(std::vector<std::string> seeds) {
    return SearchDriver{lib, panel, policy, sim, std::move(seeds), {"close", "rev"}};
  }
  [[nodiscard]] SearchDriver driver() {
    return driver({"rank(close)", "rank(rev)", "ts_mean(close, 5)", "ts_mean(rev, 3)",
                   "rank(ts_mean(close, 10))", "delta(close, 2)"});
  }
};

[[nodiscard]] SearchConfig base_cfg(u64 seed, usize workers) {
  SearchConfig cfg;
  cfg.master_seed = seed;
  cfg.population = 16;
  cfg.generations = 5;
  cfg.n_workers = workers;
  cfg.stagnation_patience = 0;
  return cfg;
}

TEST(FactorySearchL3, SemanticCanonMergesEquivalentSeeds) {
  Fx fx;
  const AlphaStore pool{};
  SearchConfig cfg = base_cfg(3, 1);
  cfg.population = 3;
  cfg.generations = 1;
  cfg.seed_from_grammar = false;
  const std::vector<std::string> seeds{"rank(close)", "rank(rank(close))", "rank(2 * close)"};
  const SearchResult structural = fx.driver(seeds).run(cfg, pool);
  cfg.canon.semantic = true;
  const SearchResult semantic = fx.driver(seeds).run(cfg, pool);
  EXPECT_EQ(structural.trial_count, 3U);
  EXPECT_EQ(semantic.trial_count, 1U); // one signal, one trial
}

TEST(FactorySearchL3, FlagsOffAreByteIdentical) {
  Fx fx;
  const AlphaStore pool{};
  const SearchResult base = fx.driver().run(base_cfg(41, 1), pool);
  SearchConfig off = base_cfg(41, 1);
  off.canon.semantic = false;
  off.output_dedup = false;
  off.fingerprint_rows = 7; // inert while output_dedup is off
  const SearchResult r = fx.driver().run(off, pool);
  EXPECT_EQ(r.digest, base.digest);
  EXPECT_EQ(r.trial_count, base.trial_count);
  EXPECT_EQ(r.best_fitness_per_gen, base.best_fitness_per_gen);
  EXPECT_EQ(r.fingerprint_hits, 0U);
}

TEST(FactorySearchL3, SemanticCanonIsWorkerCountInvariant) {
  Fx fx;
  const AlphaStore pool{};
  SearchConfig c1 = base_cfg(9, 1);
  c1.canon.semantic = true;
  SearchConfig c4 = c1;
  c4.n_workers = 4;
  const SearchResult r1 = fx.driver().run(c1, pool);
  const SearchResult r4 = fx.driver().run(c4, pool);
  EXPECT_EQ(r1.digest, r4.digest);
  EXPECT_EQ(r1.trial_count, r4.trial_count);
}

TEST(FactorySearchL3, OutputDedupReusesScoresDeterministically) {
  Fx fx;
  const AlphaStore pool{};
  SearchConfig c1 = base_cfg(17, 1);
  c1.output_dedup = true;
  c1.generations = 6;
  SearchConfig c4 = c1;
  c4.n_workers = 4;
  const SearchResult r1 = fx.driver().run(c1, pool);
  const SearchResult r4 = fx.driver().run(c4, pool);
  EXPECT_GT(r1.fingerprint_hits, 0U) << "GP children routinely re-emit a known signal";
  EXPECT_EQ(r1.fingerprint_hits, r4.fingerprint_hits);
  EXPECT_EQ(r1.digest, r4.digest);
  EXPECT_EQ(r1.best_fitness_per_gen, r4.best_fitness_per_gen);
}

} // namespace atx_test_l3_search_driver_l3
