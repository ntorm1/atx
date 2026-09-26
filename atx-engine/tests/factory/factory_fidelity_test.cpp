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
#include "atx/engine/factory/factory.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/parallel/det_pool.hpp"
#include "atx/engine/combine/store.hpp"

namespace atx::engine::factory {
// Friend of SearchDriver (search_driver.hpp): exposes the private ranking /
// finalize seams so the L3 score-provenance contract can be pinned directly.
struct L3ScoreTestAccess {
  static void rank(std::vector<Scored> &scored, const SearchConfig &cfg) {
    std::vector<atx::usize> canon(scored.size());
    for (atx::usize i = 0; i < canon.size(); ++i) {
      canon[i] = i;
    }
    SearchDriver::assign_pareto_ranks(scored, canon, cfg);
  }
  [[nodiscard]] static std::vector<atx::usize> order(const std::vector<Scored> &scored) {
    return SearchDriver::pareto_ordered_indices(scored);
  }
  static void finalize(const SearchDriver &d, const std::vector<Scored> &scored,
                       SearchResult &res) {
    const CanonSet canon{};
    d.finalize(scored, canon, res);
  }
  [[nodiscard]] static std::vector<Genome> genomes(const SearchDriver &d,
                                                   const std::vector<std::string> &exprs) {
    auto r = d.deserialize_population(exprs);
    EXPECT_TRUE(r.has_value());
    return r.has_value() ? std::move(r.value()) : std::vector<Genome>{};
  }
};
} // namespace atx::engine::factory

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
using atx::engine::Transform;
using atx::engine::factory::CachedScore;
using atx::engine::factory::FidelityCfg;
using atx::engine::factory::kObjParsimony;
using atx::engine::factory::L3ScoreTestAccess;
using atx::engine::factory::ObjectiveMode;
using atx::engine::factory::rejected_score;
using atx::engine::factory::Scored;
using atx::engine::factory::ScoreOrigin;
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

// ---- review fixes (L3 re-review) ----------------------------------------------

// A one-member pool whose PnL stream spans the FULL panel (the shape every real
// caller's pool has). Low rungs run on date-strided sub-panels, so they must not
// correlate against it (unequal lengths -> ATX_ASSERT abort in Debug).
[[nodiscard]] AlphaStore full_length_pool(usize dates, usize insts) {
  AlphaStore pool;
  std::vector<f64> pnl(dates);
  for (usize t = 0; t < dates; ++t) {
    pnl[t] = 0.001 * std::sin(0.37 * static_cast<f64>(t)) + 0.0002;
  }
  const std::vector<f64> pos(dates * insts, 0.0);
  const auto id = pool.insert(nullptr, pnl, pos, atx::engine::combine::AlphaMetrics{});
  EXPECT_TRUE(id.has_value());
  return pool;
}

TEST(FactoryFidelity, NonEmptyPoolDoesNotMisalignLowRungs) {
  DriverFixture fx;
  const AlphaStore pool = full_length_pool(fx.panel.dates(), fx.panel.instruments());
  ASSERT_EQ(pool.n_alphas(), 1U);
  // Fidelity off: the full-length pool is well-formed for the full pass.
  const SearchResult off = fx.driver().run(cfg_of(29, 1), pool);
  EXPECT_FALSE(off.admitted_candidates.empty());
  // Fidelity on: low rungs score pool-free; must not abort and must stay
  // worker-count invariant with a live pool.
  SearchConfig c1 = cfg_of(29, 1);
  c1.fidelity.enabled = true;
  SearchConfig c4 = c1;
  c4.n_workers = 4;
  const SearchResult r1 = fx.driver().run(c1, pool);
  const SearchResult r4 = fx.driver().run(c4, pool);
  EXPECT_GT(r1.fidelity_rejected, 0U);
  EXPECT_EQ(r1.digest, r4.digest);
  EXPECT_EQ(r1.best_fitness_per_gen, r4.best_fitness_per_gen);
  EXPECT_FALSE(r1.admitted_candidates.empty());
}

[[nodiscard]] Scored scored_of(const Genome &g, const CachedScore &cs) {
  Scored s{g.clone(), cs.raw, cs.raw};
  s.objectives = cs.objectives;
  s.n_objectives = cs.n_objectives;
  s.origin = cs.origin;
  s.genome.canon_hash = g.canon_hash;
  return s;
}

TEST(FactoryFidelity, RejectedRanksBelowNegativeRawInScalarMode) {
  DriverFixture fx;
  const SearchDriver d = fx.driver();
  const std::vector<Genome> g =
      L3ScoreTestAccess::genomes(d, {"rank(close)", "rank(rev)", "ts_mean(close, 5)"});
  ASSERT_EQ(g.size(), 3U);
  CachedScore neg_a{};
  neg_a.raw = -0.5;
  CachedScore neg_b{};
  neg_b.raw = -1.25;
  std::vector<Scored> scored;
  scored.push_back(scored_of(g[0], rejected_score()));
  scored.push_back(scored_of(g[1], neg_a));
  scored.push_back(scored_of(g[2], neg_b));
  SearchConfig cfg = cfg_of(1, 1);
  cfg.objective_mode = ObjectiveMode::ScalarRaw;
  L3ScoreTestAccess::rank(scored, cfg);
  const std::vector<usize> ord = L3ScoreTestAccess::order(scored);
  ASSERT_EQ(ord.size(), 3U);
  EXPECT_EQ(ord[0], 1U);
  EXPECT_EQ(ord[1], 2U);
  EXPECT_EQ(ord[2], 0U) << "a never-evaluated rejected candidate must rank last";
  SearchResult res;
  L3ScoreTestAccess::finalize(d, scored, res);
  ASSERT_EQ(res.admitted_candidates.size(), 2U);
  for (const Genome &a : res.admitted_candidates) {
    EXPECT_NE(a.canon_hash, g[0].canon_hash) << "rejected candidate must not be emitted";
  }
}

TEST(FactoryFidelity, RejectedStaysOffFrontZeroWithParsimony) {
  DriverFixture fx;
  const SearchDriver d = fx.driver();
  const std::vector<Genome> g = L3ScoreTestAccess::genomes(
      d, {"rank(close)", "rank(rev)", "ts_mean(close, 5)", "delta(close, 2)"});
  ASSERT_EQ(g.size(), 4U);
  // Two evaluated rows with negative wq and a (negative) parsimony column: an
  // all-zero row would be non-dominated and land on front 0. The sentinel must not.
  auto eval = [](f64 wq, f64 div, f64 nodes) {
    CachedScore cs{};
    cs.raw = wq;
    cs.objectives[0] = wq;
    cs.objectives[1] = div;
    cs.objectives[2] = 0.5;
    cs.objectives[kObjParsimony] = -nodes;
    cs.n_objectives = static_cast<atx::u8>(kObjParsimony + 1U);
    return cs;
  };
  std::vector<Scored> scored;
  scored.push_back(scored_of(g[0], eval(-0.2, 0.3, 3.0)));
  scored.push_back(scored_of(g[1], rejected_score()));
  scored.push_back(scored_of(g[2], eval(-0.4, 0.6, 5.0)));
  scored.push_back(scored_of(g[3], rejected_score()));
  SearchConfig cfg = cfg_of(1, 1);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.enable_parsimony = true;
  L3ScoreTestAccess::rank(scored, cfg);
  EXPECT_EQ(scored[0].rank, 0U);
  EXPECT_EQ(scored[2].rank, 0U); // mutually non-dominated evaluated pair
  EXPECT_EQ(scored[1].rank, 1U); // trailing front, after every live front
  EXPECT_EQ(scored[3].rank, 1U);
  const std::vector<usize> ord = L3ScoreTestAccess::order(scored);
  ASSERT_EQ(ord.size(), 4U);
  EXPECT_NE(scored[ord[0]].origin, ScoreOrigin::FidelityRejected);
  EXPECT_NE(scored[ord[1]].origin, ScoreOrigin::FidelityRejected);
  SearchResult res;
  L3ScoreTestAccess::finalize(d, scored, res);
  EXPECT_EQ(res.admitted_candidates.size(), 2U);
}

TEST(FactoryFidelity, BorrowedFingerprintScoresAreNotEmitted) {
  DriverFixture fx;
  const SearchDriver d = fx.driver();
  const std::vector<Genome> g = L3ScoreTestAccess::genomes(d, {"rank(close)", "rank(rev)"});
  ASSERT_EQ(g.size(), 2U);
  CachedScore full{};
  full.raw = 0.1;
  CachedScore borrowed = full;
  borrowed.raw = 0.2;
  borrowed.origin = ScoreOrigin::FingerprintBorrowed;
  std::vector<Scored> scored;
  scored.push_back(scored_of(g[0], full));
  scored.push_back(scored_of(g[1], borrowed));
  L3ScoreTestAccess::rank(scored, cfg_of(1, 1));
  SearchResult res;
  L3ScoreTestAccess::finalize(d, scored, res);
  ASSERT_EQ(res.admitted_candidates.size(), 1U);
  EXPECT_EQ(res.admitted_candidates[0].canon_hash, g[0].canon_hash);
}

TEST(FactoryFidelity, MultiObjectiveParsimonyRunIsDeterministic) {
  DriverFixture fx;
  const AlphaStore pool{};
  SearchConfig c1 = cfg_of(31, 1);
  c1.fidelity.enabled = true;
  c1.objective_mode = ObjectiveMode::MultiObjective;
  c1.enable_parsimony = true;
  SearchConfig c4 = c1;
  c4.n_workers = 4;
  const SearchResult r1 = fx.driver().run(c1, pool);
  const SearchResult r4 = fx.driver().run(c4, pool);
  EXPECT_GT(r1.fidelity_rejected, 0U);
  EXPECT_EQ(r1.digest, r4.digest);
  EXPECT_EQ(r1.trial_count, r4.trial_count);
  EXPECT_FALSE(r1.admitted_candidates.empty());
}

// output_dedup's fingerprint is rank-quantized (monotone-invariant): it is only
// meaningful under the Rank weight transform. Under ZScore it must be inert.
TEST(FactoryFidelity, OutputDedupIsInertUnderZScoreTransform) {
  DriverFixture fx;
  fx.policy.transform = Transform::ZScore;
  const AlphaStore pool{};
  SearchConfig off = cfg_of(17, 1);
  off.generations = 6;
  SearchConfig on = off;
  on.output_dedup = true;
  const SearchResult r_off = fx.driver().run(off, pool);
  const SearchResult r_on = fx.driver().run(on, pool);
  EXPECT_EQ(r_on.fingerprint_hits, 0U);
  EXPECT_EQ(r_on.digest, r_off.digest);
  EXPECT_EQ(r_on.best_fitness_per_gen, r_off.best_fitness_per_gen);
}

TEST(FactoryIcScreenIntegration, DisabledIgnoresAllKnobsAndPreservesSearch) {
  DriverFixture fx;
  const AlphaStore pool;
  SearchConfig cfg = cfg_of(47, 1);
  cfg.generations = 2;
  const SearchResult baseline = fx.driver().run(cfg, pool);
  cfg.ic_screen.horizons = {0U, 0U, 0U, 0U};
  cfg.ic_screen.window_end = std::numeric_limits<usize>::max();
  cfg.ic_screen.max_cache_bytes = 0;
  const SearchResult disabled = fx.driver().run(cfg, pool);
  EXPECT_EQ(disabled.digest, baseline.digest);
  EXPECT_EQ(disabled.trial_count, baseline.trial_count);
  EXPECT_EQ(disabled.best_fitness_per_gen, baseline.best_fitness_per_gen);
  EXPECT_TRUE(disabled.ic_rejected_hashes.empty());
  EXPECT_EQ(disabled.ic_screen_evaluations, 0U);
  EXPECT_EQ(disabled.ic_screen_unavailable, 0U);
  EXPECT_EQ(disabled.ic_prepass_vm_evaluations, 0U);
}

TEST(FactoryIcScreenIntegration, InsufficientEvidenceKeepsSinglePassAndWorkerIdentity) {
  DriverFixture fx;
  const AlphaStore pool;
  SearchConfig cfg = cfg_of(49, 1);
  cfg.generations = 2;
  const SearchResult baseline = fx.driver().run(cfg, pool);
  cfg.ic_screen.rule = atx::engine::factory::IcScreenRule::ConservativeV2;
  // 160 dates/12 names cannot establish the default 126-session, 20-name screen.
  const SearchResult screened = fx.driver().run(cfg, pool);
  cfg.n_workers = 4;
  const SearchResult parallel = fx.driver().run(cfg, pool);
  EXPECT_EQ(screened.digest, baseline.digest);
  EXPECT_EQ(screened.best_fitness_per_gen, baseline.best_fitness_per_gen);
  EXPECT_EQ(screened.trial_count, baseline.trial_count);
  EXPECT_GT(screened.ic_screen_evaluations, 0U);
  EXPECT_EQ(screened.ic_screen_unavailable, 0U);
  EXPECT_EQ(screened.ic_prepass_vm_evaluations, 0U);
  EXPECT_TRUE(screened.ic_rejected_hashes.empty());
  EXPECT_EQ(parallel.digest, screened.digest);
  EXPECT_EQ(parallel.best_fitness_per_gen, screened.best_fitness_per_gen);
  EXPECT_EQ(parallel.ic_screen_evaluations, screened.ic_screen_evaluations);
}

TEST(FactoryIcScreenIntegration, RejectedOriginIsNeverRankedOrEmittedAsFull) {
  DriverFixture fx;
  const SearchDriver driver = fx.driver();
  const auto genomes = L3ScoreTestAccess::genomes(driver, {"rank(close)", "rank(rev)"});
  ASSERT_EQ(genomes.size(), 2U);
  CachedScore full;
  full.raw = -1.0;
  full.objectives[0] = -1.0;
  full.objectives[kObjParsimony] = -5.0;
  full.n_objectives = static_cast<atx::u8>(kObjParsimony + 1U);
  std::vector<Scored> scored;
  scored.push_back(scored_of(genomes[0], atx::engine::factory::ic_rejected_score()));
  scored.push_back(scored_of(genomes[1], full));
  SearchConfig cfg = cfg_of(1, 1);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.enable_parsimony = true;
  L3ScoreTestAccess::rank(scored, cfg);
  EXPECT_GT(scored[0].rank, scored[1].rank);
  SearchResult result;
  L3ScoreTestAccess::finalize(driver, scored, result);
  ASSERT_EQ(result.admitted_candidates.size(), 1U);
  EXPECT_EQ(result.admitted_candidates.front().canon_hash, genomes[1].canon_hash);
}

// A deliberately alternating weak correlation, not a fitted noise threshold:
// cyclic rank shift 7 has rho=-27/1023, and reversing every other date cancels
// its temporal mean while retaining nonzero inference variance.
[[nodiscard]] Panel alternating_null_panel() {
  constexpr usize dates = 384;
  constexpr usize names = 32;
  std::vector<f64> close(dates * names);
  std::vector<f64> noise(dates * names);
  for (usize t = 0; t < dates; ++t) {
    for (usize i = 0; i < names; ++i) {
      close[t * names + i] = std::exp(0.0001 * static_cast<f64>(t) *
                                      (static_cast<f64>(i) - 15.5));
      const f64 x = static_cast<f64>((i + 7U) % names) - 15.5;
      noise[t * names + i] = t % 2U == 0U ? x : -x;
    }
  }
  auto panel = Panel::create(dates, names, {"close", "noise"},
                            {std::move(close), std::move(noise)}, {});
  EXPECT_TRUE(panel);
  return std::move(panel.value());
}

[[nodiscard]] std::vector<std::string> null_seeds() {
  return {"noise", "noise * 2", "noise + 1", "-1 * noise", "rank(noise)", "noise / 2"};
}

TEST(FactoryIcScreenIntegration, RejectsBeforeEitherBacktestPathAndPreservesTrials) {
  const Library lib;
  const Panel panel = alternating_null_panel();
  const WeightPolicy policy;
  const ExecutionSimulator sim = frictionless();
  const AlphaStore pool;
  SearchConfig cfg;
  cfg.population = 6;
  cfg.generations = 1;
  cfg.n_workers = 1;
  cfg.ic_screen.rule = atx::engine::factory::IcScreenRule::ConservativeV2;
  const auto run = [&](const SearchConfig &config) {
    SearchDriver driver{lib, panel, policy, sim, null_seeds(), {"close", "noise"}};
    return driver.run(config, pool);
  };
  const SearchResult once = run(cfg);
  ASSERT_EQ(once.ic_rejected_hashes.size(), 6U);
  EXPECT_EQ(once.trial_count, 6U);
  EXPECT_EQ(once.all_scored.size(), 6U);
  EXPECT_TRUE(once.admitted_candidates.empty());
  EXPECT_EQ(once.ic_screen_evaluations, 6U);
  EXPECT_EQ(once.ic_prepass_vm_evaluations, 0U);
  cfg.fidelity.enabled = true;
  const SearchResult both = run(cfg);
  cfg.n_workers = 4;
  const SearchResult parallel = run(cfg);
  EXPECT_EQ(both.ic_rejected_hashes, once.ic_rejected_hashes);
  EXPECT_EQ(both.digest, once.digest);
  EXPECT_EQ(both.trial_count, 6U);
  EXPECT_EQ(both.all_scored.size(), 6U);
  EXPECT_EQ(both.ic_screen_evaluations, 6U);
  EXPECT_EQ(both.ic_prepass_vm_evaluations, 6U);
  EXPECT_EQ(both.fidelity_evals, 0U); // IC rejects never enter even the lowest rung
  EXPECT_EQ(parallel.digest, both.digest);
  EXPECT_EQ(parallel.ic_rejected_hashes, both.ic_rejected_hashes);
  EXPECT_EQ(parallel.ic_screen_evaluations, both.ic_screen_evaluations);
  EXPECT_TRUE(parallel.admitted_candidates.empty());
}

TEST(FactoryIcScreenIntegration, FactoryKeepsTrialLedgerButExcludesRejectedAdmission) {
  const Library lib;
  const Panel panel = alternating_null_panel();
  const WeightPolicy policy;
  const ExecutionSimulator sim = frictionless();
  atx::engine::factory::Factory factory{lib, panel, sim, policy};
  atx::engine::factory::FactoryConfig cfg;
  cfg.search.population = 6;
  cfg.search.generations = 1;
  cfg.search.n_workers = 1;
  cfg.search.ic_screen.rule = atx::engine::factory::IcScreenRule::ConservativeV2;
  cfg.seed_exprs = null_seeds();
  cfg.panel_fields = {"close", "noise"};
  AlphaStore pool;
  const atx::engine::combine::AlphaGate gate;
  const auto report = factory.mine(cfg, pool, gate);
  EXPECT_EQ(report.trials, 6U);
  EXPECT_EQ(report.evaluated, 6U);
  EXPECT_EQ(report.scored_canon_hashes.size(), 6U);
  EXPECT_EQ(report.ic_rejected, 6U);
  EXPECT_EQ(report.ic_screen_evaluations, 6U);
  EXPECT_EQ(report.admitted, 0U);
}

} // namespace atx_test_l3_search_fidelity
