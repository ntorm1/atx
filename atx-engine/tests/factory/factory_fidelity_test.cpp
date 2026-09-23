// atx::engine::factory — multi-fidelity racing tests (L3, suite FactoryFidelity).
//
// Generic race() semantics (promotion, NaN rejection, trial accounting,
// canon-hash tie-break, worker invariance), strided_panel, and the SearchDriver
// wiring: fidelity off is byte-identical; fidelity on is worker-count invariant
// and counts rung-rejected candidates as trials.

#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/fidelity.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/parallel/det_pool.hpp"

namespace atx_test_l3_search_fidelity {

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
using atx::engine::factory::FidelityCfg;
using atx::engine::factory::Genome;
using atx::engine::factory::GenomeId;
using atx::engine::factory::promote_count;
using atx::engine::factory::race;
using atx::engine::factory::RaceResult;
using atx::engine::factory::Rung;
using atx::engine::factory::SearchConfig;
using atx::engine::factory::SearchDriver;
using atx::engine::factory::SearchResult;
using atx::engine::factory::strided_panel;
using atx::engine::parallel::DetPool;

[[nodiscard]] std::vector<Genome> dummy_genomes(usize n) {
  std::vector<Genome> out(n);
  for (usize i = 0; i < n; ++i) {
    out[i].canon_hash = 1000U + (i * 7919U) % 97U; // distinct, non-monotone in i
  }
  return out;
}

// Score = a fixed per-candidate quality + a rung-dependent perturbation.
[[nodiscard]] f64 quality(const Genome &g) { return static_cast<f64>(g.canon_hash % 41U); }

TEST(FactoryFidelity, PromoteCountHonoursEtaAndFloor) {
  FidelityCfg cfg{};
  EXPECT_EQ(promote_count(9, cfg), 3U);
  EXPECT_EQ(promote_count(10, cfg), 4U); // ceil
  EXPECT_EQ(promote_count(3, cfg), 2U);  // min_keep
  EXPECT_EQ(promote_count(1, cfg), 1U);
  EXPECT_EQ(promote_count(0, cfg), 0U);
}

TEST(FactoryFidelity, RacePromotesBestAndCountsEveryEvalAsATrial) {
  FidelityCfg cfg{};
  cfg.enabled = true;
  const auto cands = dummy_genomes(27);
  const auto eval = [](const Genome &g, usize r, const Rung &, usize) {
    return quality(g) + 0.001 * static_cast<f64>(r);
  };
  const RaceResult rr = race(cands, cfg, eval);
  EXPECT_EQ(rr.evals_per_rung[0], 27U);
  EXPECT_EQ(rr.evals_per_rung[1], 9U);
  EXPECT_EQ(rr.evals_per_rung[2], 3U);
  EXPECT_EQ(rr.n_evals, 39U);
  ASSERT_EQ(rr.survivors.size(), 3U);
  EXPECT_EQ(rr.n_rejected, 24U);
  // Survivors are the three highest-quality candidates.
  std::vector<f64> q;
  for (const Genome &g : cands) {
    q.push_back(quality(g));
  }
  std::vector<f64> sorted = q;
  std::sort(sorted.begin(), sorted.end(), std::greater<>());
  for (const GenomeId id : rr.survivors) {
    EXPECT_GE(q[id], sorted[2]);
    EXPECT_EQ(rr.last_rung[id], 2U);
  }
}

TEST(FactoryFidelity, NaNIsRejectionAndTiesBreakByCanonHash) {
  FidelityCfg cfg{};
  cfg.min_keep = 1;
  cfg.eta = 0.25;
  auto cands = dummy_genomes(8);
  const auto eval = [](const Genome &g, usize, const Rung &, usize) {
    return (g.canon_hash % 2U == 0U) ? std::numeric_limits<f64>::quiet_NaN() : 1.0;
  };
  const RaceResult rr = race(cands, cfg, eval, 2);
  // Only odd hashes are live; all tie at 1.0 -> the smallest canon_hash wins.
  u64 best = ~0ULL;
  for (const Genome &g : cands) {
    if (g.canon_hash % 2U == 1U) {
      best = std::min(best, g.canon_hash);
    }
  }
  ASSERT_EQ(rr.survivors.size(), 1U);
  EXPECT_EQ(cands[rr.survivors.front()].canon_hash, best);
}

TEST(FactoryFidelity, RaceIsWorkerCountInvariant) {
  FidelityCfg cfg{};
  const auto cands = dummy_genomes(50);
  const auto eval = [](const Genome &g, usize r, const Rung &rung, usize) {
    return std::sin(static_cast<f64>(g.canon_hash) * (1.0 + static_cast<f64>(r))) +
           0.01 * static_cast<f64>(rung.date_stride);
  };
  const RaceResult serial = race(cands, cfg, eval);
  for (const usize w : {1U, 2U, 4U}) {
    DetPool pool{w};
    const RaceResult par = race(cands, cfg, eval, 3, &pool);
    EXPECT_EQ(par.survivors, serial.survivors);
    EXPECT_EQ(par.last_score, serial.last_score);
    EXPECT_EQ(par.n_evals, serial.n_evals);
  }
}

TEST(FactoryFidelity, StridedPanelSubsamplesFieldsAndUniverse) {
  std::vector<f64> x(6 * 4);
  std::vector<std::uint8_t> uni(6 * 4, 1);
  for (usize i = 0; i < x.size(); ++i) {
    x[i] = static_cast<f64>(i);
  }
  uni[2 * 4 + 2] = 0;
  auto p = Panel::create(6, 4, {"x"}, {x}, uni);
  ASSERT_TRUE(p.has_value());
  auto s = strided_panel(*p, 2, 2);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->dates(), 3U);
  EXPECT_EQ(s->instruments(), 2U);
  const auto col = s->field_all(0);
  EXPECT_EQ(col[0], 0.0);
  EXPECT_EQ(col[1], 2.0);
  EXPECT_EQ(col[2], 8.0);
  EXPECT_EQ(col[3], 10.0);
  EXPECT_FALSE(s->in_universe(1, 1)); // (2,2) in the source
  EXPECT_TRUE(s->in_universe(1, 0));
}

// ---- driver wiring -----------------------------------------------------------

[[nodiscard]] ExecutionSimulator frictionless() {
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0.0, 0.0, 0.0, 0.0},
                            ImpactCfg{0.0, 0.5, 0.0},
                            CommissionCfg{CommissionMode::PerShare, 0.0, 0.0, 1.0, 0.0},
                            LatencyCfg{},
                            VolumeCapCfg{1.0}};
}

[[nodiscard]] Panel momentum_panel(usize dates, usize insts) {
  std::vector<f64> close(dates * insts);
  std::vector<f64> rev(dates * insts, 0.0);
  std::vector<f64> px(insts, 100.0);
  std::uint64_t s = 0xA11CULL;
  for (usize t = 0; t < dates; ++t) {
    for (usize j = 0; j < insts; ++j) {
      s = s * 6364136223846793005ULL + 1442695040888963407ULL;
      const f64 u = 2.0 * (static_cast<f64>(s >> 11U) / static_cast<f64>(1ULL << 53U)) - 1.0;
      px[j] *= 1.0 + (0.004 - 0.0008 * static_cast<f64>(j)) + 0.01 * u;
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

struct DriverFixture {
  Library lib{};
  Panel panel = momentum_panel(160, 12);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless();

  [[nodiscard]] SearchDriver driver() {
    return SearchDriver{lib, panel, policy, sim,
                        {"rank(close)", "rank(rev)", "ts_mean(close, 5)", "ts_mean(rev, 3)",
                         "rank(ts_mean(close, 10))", "delta(close, 2)"},
                        {"close", "rev"}};
  }
};

[[nodiscard]] SearchConfig cfg_of(u64 seed, usize workers) {
  SearchConfig cfg;
  cfg.master_seed = seed;
  cfg.population = 18;
  cfg.generations = 4;
  cfg.n_workers = workers;
  cfg.stagnation_patience = 0;
  return cfg;
}

TEST(FactoryFidelity, DisabledIsByteIdenticalToDefault) {
  DriverFixture fx;
  const AlphaStore pool{};
  const SearchResult base = fx.driver().run(cfg_of(11, 1), pool);
  SearchConfig off = cfg_of(11, 1);
  off.fidelity.enabled = false;
  off.fidelity.eta = 0.1; // knobs are inert while disabled
  off.fidelity.rungs[0] = Rung{8, 4, 2};
  const SearchResult r = fx.driver().run(off, pool);
  EXPECT_EQ(r.digest, base.digest);
  EXPECT_EQ(r.trial_count, base.trial_count);
  EXPECT_EQ(r.best_fitness_per_gen, base.best_fitness_per_gen);
  EXPECT_EQ(r.fidelity_evals, 0U);
  EXPECT_EQ(r.fidelity_rejected, 0U);
}

TEST(FactoryFidelity, EnabledIsDeterministicAcrossWorkerCounts) {
  DriverFixture fx;
  const AlphaStore pool{};
  SearchConfig c1 = cfg_of(23, 1);
  c1.fidelity.enabled = true;
  SearchConfig c4 = c1;
  c4.n_workers = 4;
  const SearchResult r1 = fx.driver().run(c1, pool);
  const SearchResult r4 = fx.driver().run(c4, pool);
  EXPECT_EQ(r1.digest, r4.digest);
  EXPECT_EQ(r1.trial_count, r4.trial_count);
  EXPECT_EQ(r1.best_fitness_per_gen, r4.best_fitness_per_gen);
  EXPECT_EQ(r1.fidelity_evals, r4.fidelity_evals);
  EXPECT_EQ(r1.fidelity_rejected, r4.fidelity_rejected);
}

TEST(FactoryFidelity, TrialCountIncludesRungRejections) {
  DriverFixture fx;
  const AlphaStore pool{};
  SearchConfig cfg = cfg_of(5, 2);
  cfg.fidelity.enabled = true;
  const SearchResult r = fx.driver().run(cfg, pool);
  EXPECT_GT(r.fidelity_rejected, 0U);
  EXPECT_GE(r.fidelity_evals, r.fidelity_rejected);
  // Every distinct candidate the race looked at is a trial, rejected or not.
  EXPECT_EQ(r.trial_count, r.all_scored.size());
  EXPECT_GE(r.trial_count, r.fidelity_rejected);
}

} // namespace atx_test_l3_search_fidelity
