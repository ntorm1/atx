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
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <unordered_set>
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

namespace atxtest_deflate_selection_running_n {
namespace factory = atx::engine::factory;

struct SelectionSink : factory::SearchProgressSink {
  std::vector<factory::GenerationSnapshot> seen;
  atx::core::Status on_generation(const factory::GenerationSnapshot &snapshot) override {
    seen.push_back(snapshot);
    return atx::core::Ok();
  }
};

factory::SearchResumeState resume_from(const factory::GenerationSnapshot &cp) {
  factory::SearchResumeState out;
  out.start_generation = cp.generation; out.population = cp.population;
  out.canon_blob = cp.canon_blob; out.cache_blob = cp.cache_blob;
  out.archive_blob = cp.archive_blob; out.best_per_gen_blob = cp.best_per_gen_blob;
  out.digest = cp.digest; out.candidates_generated = cp.candidates_generated;
  return out;
}

std::vector<atx::u64> candidate_order(const SearchResult &result) {
  std::vector<atx::u64> out;
  for (const auto &g : result.admitted_candidates) out.push_back(g.canon_hash);
  return out;
}

// Public, fresh full-fitness evaluations form the oracle. Search must reach the
// same scores/order using cached statistics, without those repeated backtests.
void verify_selection_oracle(Fixture &fx, const SearchConfig &cfg,
                             const SelectionSink &sink, const SearchResult &result) {
  ASSERT_FALSE(sink.seen.empty());
  usize reused = 0;
  std::vector<atx::u64> expected_order;
  for (const auto &snapshot : sink.seen) {
    factory::FitnessCfg fit = cfg.fitness;
    fit.trial_count = cfg.prior_trial_count + snapshot.n_evaluated;
    std::vector<atx::u64> cache_keys;
    std::vector<factory::CachedScore> cache;
    bool versioned = false;
    ASSERT_TRUE(factory::deserialize_cache(snapshot.cache_blob, cache_keys, cache,
                                           nullptr, nullptr, &versioned));
    ASSERT_TRUE(versioned);
    std::vector<atx::u64> hashes;
    std::vector<f64> scores, flat;
    constexpr usize width = factory::kObjDeflation + 1;
    f64 best = -std::numeric_limits<f64>::infinity();
    for (const auto &source : snapshot.population) {
      auto ast = atx::engine::alpha::parse_expr(source, fx.lib);
      ASSERT_TRUE(ast);
      auto g = factory::analyze_into(std::move(*ast));
      ASSERT_TRUE(g);
      g->canon_hash = factory::canonical_hash(*g, cfg.canon);
      auto report = factory::pool_aware_fitness(*g, AlphaStore{}, fx.panel, fx.policy, fx.sim, fit);
      ASSERT_TRUE(report);
      ASSERT_TRUE(std::isfinite(report->dsr));
      const f64 score = std::min(report->raw, report->raw * report->dsr);
      best = std::max(best, score);
      hashes.push_back(g->canon_hash); scores.push_back(score);
      auto objectives = report->objectives;
      objectives[factory::kObjDeflation] = report->dsr;
      flat.insert(flat.end(), objectives.begin(), objectives.begin() + width);
      const auto hit = std::find(cache_keys.begin(), cache_keys.end(), g->canon_hash);
      if (hit != cache_keys.end()) {
        ++reused;
        const auto &cached = cache[static_cast<usize>(hit - cache_keys.begin())];
        EXPECT_DOUBLE_EQ(cached.raw, report->raw);
        EXPECT_EQ(cached.objectives[factory::kObjDeflation], 0.0);
        EXPECT_TRUE(cached.dsr_sample.available);
        EXPECT_EQ(cached.dsr_sample.observations, report->dsr_sample.observations);
        EXPECT_DOUBLE_EQ(cached.dsr_sample.per_period_sharpe, report->dsr_sample.per_period_sharpe);
        EXPECT_DOUBLE_EQ(cached.dsr_sample.skewness, report->dsr_sample.skewness);
        EXPECT_DOUBLE_EQ(cached.dsr_sample.excess_kurtosis, report->dsr_sample.excess_kurtosis);
      }
    }
    EXPECT_DOUBLE_EQ(snapshot.best_fitness, best) << "generation " << snapshot.generation;
    std::vector<usize> order(hashes.size());
    std::iota(order.begin(), order.end(), 0U); // snapshot population is canonical order
    if (cfg.objective_mode == ObjectiveMode::ScalarRaw) {
      std::sort(order.begin(), order.end(), [&](usize a, usize b) {
        return scores[a] != scores[b] ? scores[a] > scores[b] : hashes[a] < hashes[b];
      });
    } else {
      const factory::ObjMatrix matrix{flat, hashes.size(), width};
      const auto ranks = factory::fast_nondominated_sort(matrix, order);
      std::vector<f64> crowding(hashes.size());
      for (atx::u16 front = 0; front <= *std::max_element(ranks.begin(), ranks.end()); ++front) {
        std::vector<usize> members;
        for (const auto i : order) if (ranks[i] == front) members.push_back(i);
        const auto distances = factory::crowding_distance(matrix, members, order);
        for (const auto i : members) crowding[i] = distances[i];
      }
      std::sort(order.begin(), order.end(), [&](usize a, usize b) {
        if (ranks[a] != ranks[b]) return ranks[a] < ranks[b];
        return crowding[a] != crowding[b] ? crowding[a] > crowding[b] : hashes[a] < hashes[b];
      });
    }
    expected_order.clear();
    for (const auto i : order) expected_order.push_back(hashes[i]);
  }
  EXPECT_GT(reused, 0U); // must actually exercise elites/cache hits
  EXPECT_EQ(candidate_order(result), expected_order);
}

TEST(DeflateSelection, CurrentTrialsRefreshEveryEliteAndRankedCandidate) {
  for (const auto mode : {ObjectiveMode::ScalarRaw, ObjectiveMode::MultiObjective}) {
    Fixture fx;
    auto cfg = deflate_cfg(42, 12, 5, 2);
    cfg.objective_mode = mode;
    cfg.prior_trial_count = 29;
    SelectionSink sink;
    const auto result = fx.driver().run(cfg, AlphaStore{}, &sink);
    ASSERT_EQ(sink.seen.size(), cfg.generations);
    EXPECT_GT(sink.seen.back().n_evaluated, sink.seen.front().n_evaluated);
    EXPECT_EQ(result.full_fitness_evaluations, result.trial_count);
    verify_selection_oracle(fx, cfg, sink, result);
  }
}

TEST(DeflateSelection, VersionedCheckpointRefreshesWithoutRepeatingBacktests) {
  Fixture fx;
  auto cfg = deflate_cfg(99, 12, 5, 1);
  cfg.objective_mode = ObjectiveMode::MultiObjective;
  cfg.prior_trial_count = 57;
  SelectionSink full_sink;
  const auto full = fx.driver().run(cfg, AlphaStore{}, &full_sink);
  ASSERT_EQ(full_sink.seen.size(), cfg.generations);
  auto resume = resume_from(full_sink.seen[2]);
  SelectionSink resumed_sink;
  cfg.n_workers = 4;
  const auto resumed = fx.driver().run(cfg, AlphaStore{}, &resumed_sink, &resume);
  ASSERT_FALSE(resumed.fitness_cache_resume_mismatch);
  EXPECT_EQ(full.digest, resumed.digest);
  EXPECT_EQ(full.best_fitness_per_gen, resumed.best_fitness_per_gen);
  EXPECT_EQ(candidate_order(full), candidate_order(resumed));
  const auto restored = factory::deserialize_canon(resume.canon_blob);
  ASSERT_TRUE(restored);
  EXPECT_EQ(resumed.full_fitness_evaluations, resumed.trial_count - restored->size());
  verify_selection_oracle(fx, cfg, resumed_sink, resumed);

  // An increased cross-run history also refreshes restored elites immediately.
  cfg.prior_trial_count = 10000;
  SelectionSink later_sink;
  const auto later = fx.driver().run(cfg, AlphaStore{}, &later_sink, &resume);
  EXPECT_EQ(later.full_fitness_evaluations, later.trial_count - restored->size());
  verify_selection_oracle(fx, cfg, later_sink, later);
}

TEST(DeflateSelection, LegacyAndMalformedScoreCheckpointsAreRefused) {
  Fixture fx;
  const auto cfg = deflate_cfg(7, 8, 3, 1);
  SelectionSink sink;
  static_cast<void>(fx.driver().run(cfg, AlphaStore{}, &sink));
  ASSERT_EQ(sink.seen.size(), cfg.generations);
  auto resume = resume_from(sink.seen[1]);
  // Recreate the old layout: its cached raw may already contain a haircut and
  // descriptor bins cannot recover the original sample moments.
  std::vector<atx::u64> keys;
  std::vector<factory::CachedScore> cache;
  ASSERT_TRUE(factory::deserialize_cache(resume.cache_blob, keys, cache));
  ASSERT_FALSE(keys.empty());
  std::string legacy;
  for (usize i = 0; i < keys.size(); ++i) {
    if (i != 0) legacy += '\n';
    legacy += factory::u64_to_hex(keys[i]) + ' ' + factory::f64_to_hex(cache[i].raw);
    legacy += ' ' + factory::u64_to_hex(cache[i].n_objectives);
    for (const auto value : cache[i].objectives) legacy += ' ' + factory::f64_to_hex(value);
    legacy += ' ' + factory::u64_to_hex(cache[i].descriptor.size());
    for (const auto value : cache[i].descriptor) legacy += ' ' + factory::f64_to_hex(value);
  }
  for (const auto &blob : {legacy, std::string{}, sink.seen[1].cache_blob + " garbage"}) {
    resume.cache_blob = blob;
    const auto rejected = fx.driver().run(cfg, AlphaStore{}, nullptr, &resume);
    EXPECT_TRUE(rejected.fitness_cache_resume_mismatch);
    EXPECT_TRUE(rejected.admitted_candidates.empty());
    EXPECT_EQ(rejected.full_fitness_evaluations, 0U);
  }
  auto disabled = cfg;
  disabled.deflate_selection = false;
  resume.cache_blob = legacy;
  EXPECT_TRUE(fx.driver().run(disabled, AlphaStore{}, nullptr, &resume).fitness_cache_resume_mismatch);
}
} // namespace atxtest_deflate_selection_running_n
