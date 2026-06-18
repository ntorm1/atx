// atx::engine::factory — deterministic evolutionary search driver tests
// (S3-5, suite FactorySearchDriver).
//
// Covers the plan's verbatim Task-S3-5 list (§4.7 + F1 / F2 / F5 / F6):
//   * SameSeedReplaysByteIdentical      — F1: the load-bearing determinism test
//   * DedupSkipsEquivalentExpressions   — F6: canonical dedup skips re-scoring
//   * ElitismKeepsBest                  — best-per-gen monotone non-decreasing
//   * AllCandidatesAreValidCausalPrograms — F5: every scored genome analyze-valid
//   * EvalDigestIsWorkerInvariantAndMatchesSingleThread
//                                       — F2: parallel_evaluate's SignalSet digest
//                                         is worker-invariant AND (when the warm-up
//                                         does not perturb it) equals the single-
//                                         thread fresh-Engine reference digest the
//                                         driver actually uses.
//
// The fixture builds a noisy momentum panel + a seed-expression set DIRECTLY
// (public members, the S2/S3-4 precedent), so the search has real finite-Sharpe
// candidates to score and the determinism/dedup properties hold by construction.

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/alpha/bytecode.hpp" // alpha::compile, alpha::Program
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/parser.hpp"   // alpha::parse_expr, alpha::Library
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"       // alpha::Engine (single-thread reference eval)
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/parallel/batch_eval.hpp" // parallel::parallel_evaluate
#include "atx/engine/parallel/det_pool.hpp"   // parallel::DetPool
#include "atx/engine/parallel/digest.hpp"     // parallel::signal_set_digest

namespace atxtest_factory_search_driver_test {

using atx::f64;
using atx::usize;
using atx::engine::alpha::analyze;
using atx::engine::alpha::compile;
using atx::engine::alpha::Engine;
using atx::engine::alpha::Library;
using atx::engine::alpha::Panel;
using atx::engine::alpha::parse_expr;
using atx::engine::alpha::Program;
using atx::engine::parallel::DetPool;
using atx::engine::parallel::parallel_evaluate;
using atx::engine::parallel::signal_set_digest;
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
using atx::engine::WeightPolicy;

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

// A tiny deterministic LCG (no RNG dep) -> uniform(-1, 1), the S3-4 fixture idiom.
struct Lcg {
  std::uint64_t s;
  [[nodiscard]] f64 next() noexcept {
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const std::uint64_t hi = s >> 11U;
    const f64 u = static_cast<f64>(hi) / static_cast<f64>(1ULL << 53U);
    return 2.0 * u - 1.0;
  }
};

// Build a noisy close matrix [dates*insts]: each instrument follows a random walk
// with a small per-instrument persistent drift so rank(close) has a genuine (but
// noisy) momentum edge — a realistic, finite-Sharpe alpha for the search to score.
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

// A two-field momentum + reversal panel: "close" (momentum) and "rev" (1-day
// reversal). Seed expressions over these fields give the search a small valid
// grammar surface (rank/ts ops over close/rev) to mutate and cross.
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

// The seed-expression set (all analyze-valid in-grammar templates).
[[nodiscard]] std::vector<std::string> seed_exprs() {
  return {"rank(close)", "rank(rev)",         "ts_mean(close, 5)", "ts_mean(rev, 3)",
          "rank(ts_mean(close, 10))", "delta(close, 2)"};
}

// A small search config; the verbatim test helper from the plan, parameterized.
[[nodiscard]] SearchConfig small_search_cfg(atx::u64 seed, usize pop, usize gens) {
  SearchConfig cfg;
  cfg.master_seed = seed;
  cfg.population = pop;
  cfg.generations = gens;
  cfg.elites = 2;
  cfg.k_tournament = 3;
  cfg.p_cross = 0.5;
  cfg.novelty_w = 0.1;
  return cfg;
}

[[nodiscard]] AlphaStore empty_pool() { return AlphaStore{}; }

// A reusable driver over the shared fixture (one Library/Panel/policy/sim per
// driver; the verbatim tests construct `SearchDriver(lib, panel)` fresh each run).
struct Fixture {
  Library lib{};
  Panel panel = fixture_panel(96, 6);
  WeightPolicy policy{};
  ExecutionSimulator sim = frictionless_sim();

  [[nodiscard]] SearchDriver driver() {
    return SearchDriver{lib, panel, policy, sim, seed_exprs(), {"close", "rev"}};
  }
};

// =============================================================================
//  SameSeedReplaysByteIdentical — F1 (the load-bearing determinism test).
// =============================================================================
TEST(FactorySearchDriver, SameSeedReplaysByteIdentical) {
  Fixture fx;
  const auto cfg = small_search_cfg(/*seed*/ 777, /*pop*/ 16, /*gens*/ 5);
  const SearchResult r1 = fx.driver().run(cfg, empty_pool());
  const SearchResult r2 = fx.driver().run(cfg, empty_pool());
  EXPECT_EQ(r1.digest, r2.digest);             // byte-identical search
  EXPECT_EQ(r1.trial_count, r2.trial_count);   // identical distinct-candidate count
}

// =============================================================================
//  DedupSkipsEquivalentExpressions — F6 throughput.
// =============================================================================
TEST(FactorySearchDriver, DedupSkipsEquivalentExpressions) {
  Fixture fx;
  // Multi-generation run with a modest population: re-scored structural duplicates
  // (an elite carried forward, a mutation reproducing a prior structure) make the
  // distinct-candidate trial count strictly less than the total generated.
  const auto cfg = small_search_cfg(/*seed*/ 8, /*pop*/ 12, /*gens*/ 4);
  const SearchResult r = fx.driver().run(cfg, empty_pool());
  EXPECT_LT(r.trial_count, r.candidates_generated); // structural duplicates skipped
  EXPECT_GT(r.dedup_pct, 0.0);
}

// =============================================================================
//  ElitismKeepsBest — best-per-gen monotone non-decreasing under elitism.
// =============================================================================
TEST(FactorySearchDriver, ElitismKeepsBest) {
  Fixture fx;
  auto cfg = small_search_cfg(/*seed*/ 42, /*pop*/ 16, /*gens*/ 6);
  cfg.elites = 2;
  const SearchResult r = fx.driver().run(cfg, empty_pool());
  ASSERT_FALSE(r.best_fitness_per_gen.empty());
  EXPECT_GE(r.best_fitness_per_gen.back(), r.best_fitness_per_gen.front());
}

// =============================================================================
//  AllCandidatesAreValidCausalPrograms — F5.
// =============================================================================
TEST(FactorySearchDriver, AllCandidatesAreValidCausalPrograms) {
  Fixture fx;
  const SearchResult r = fx.driver().run(small_search_cfg(1, 16, 5), empty_pool());
  ASSERT_FALSE(r.all_scored.empty());
  for (const auto &g : r.all_scored) {
    EXPECT_TRUE(analyze(g.ast).has_value());
  }
}

// =============================================================================
//  EvalDigestIsWorkerInvariantAndMatchesSingleThread — F2 (the spec's line-348
//  contract): parallel_evaluate's SignalSet digest is invariant across worker
//  counts AND equals the single-thread fresh-Engine reference digest the driver
//  actually uses. This exercises the real S2 cross-path contract on the SAFE seed
//  grammar (the driver itself stays on the single-thread fallback — this test does
//  not change that). Varying only cfg.n_workers through the driver would be
//  vacuous (worker count never enters the single-thread driver math), so we call
//  parallel_evaluate directly here on the compiled seed programs.
// =============================================================================
TEST(FactorySearchDriver, EvalDigestIsWorkerInvariantAndMatchesSingleThread) {
  using atx::engine::alpha::SignalSet;
  Fixture fx;

  // Parse + analyze + compile each seed expression into a Program (the SAFE seed
  // grammar: rank / ts_mean / delta over close/rev — the same set the driver seeds).
  std::vector<Program> progs;
  for (const std::string &src : seed_exprs()) {
    auto ast = parse_expr(src, fx.lib);
    ASSERT_TRUE(ast.has_value()) << "seed must parse: " << src;
    auto info = analyze(*ast);
    ASSERT_TRUE(info.has_value()) << "seed must analyze: " << src;
    auto prog = compile(*ast, *info);
    ASSERT_TRUE(prog.has_value()) << "seed must compile: " << src;
    progs.push_back(std::move(*prog));
  }
  ASSERT_FALSE(progs.empty());

  // SINGLE-THREAD reference: assemble exactly what parallel_evaluate assembles —
  // one SignalSet whose alphas are every program's roots concatenated in (program
  // order, then root order within a program) — but using a FRESH Engine per program
  // (never reused, no warm-up: the driver's actual eval path). Then digest it ONCE.
  // This is genuinely comparable to parallel_evaluate's single assembled SignalSet
  // (NOT a per-program digest fold), so the equality below is the real cross-path
  // contract the spec asks for (line 348).
  SignalSet single_thread;
  single_thread.dates = fx.panel.dates();
  single_thread.instruments = fx.panel.instruments();
  for (const Program &p : progs) {
    Engine engine{fx.panel}; // fresh Engine per program (the driver's fallback)
    auto ss = engine.evaluate(p);
    ASSERT_TRUE(ss.has_value()) << "single-thread eval must succeed on the seed grammar";
    for (auto &a : ss->alphas) {
      single_thread.alphas.push_back(std::move(a));
    }
  }
  const atx::u64 single_thread_digest = signal_set_digest(single_thread);

  // PARALLEL path: parallel_evaluate the whole batch at 1, 2, and 4 workers; digest
  // each assembled SignalSet.
  auto parallel_batch_digest = [&](usize workers) -> atx::u64 {
    DetPool pool{workers};
    auto ss = parallel_evaluate(std::span<const Program>{progs}, fx.panel, pool);
    EXPECT_TRUE(ss.has_value()) << "parallel_evaluate must succeed on the seed grammar";
    return ss.has_value() ? signal_set_digest(*ss) : atx::u64{0};
  };
  const atx::u64 p1 = parallel_batch_digest(1);
  const atx::u64 p2 = parallel_batch_digest(2);
  const atx::u64 p4 = parallel_batch_digest(4);

  // (1) Worker-invariance — the load-bearing S2 contract: parallel_evaluate's
  // digest must NOT depend on the worker count.
  EXPECT_EQ(p1, p2) << "parallel digest must be worker-count-invariant (1 vs 2)";
  EXPECT_EQ(p1, p4) << "parallel digest must be worker-count-invariant (1 vs 4)";

  // (2) Cross-path equality — the spec's line-348 contract. If the per-worker
  // warm-up + reuse genuinely perturbs the assembled result, this fails and the
  // driver's single-thread fallback is justified by PERTURBATION; if it holds, the
  // fallback is a conservative choice and the seed grammar is parallel-clean. We
  // assert it; report which branch held.
  EXPECT_EQ(p1, single_thread_digest)
      << "parallel_evaluate digest must equal the single-thread fresh-Engine digest "
         "on the seed grammar (the driver uses single-thread precisely because S2's "
         "warm-up+reuse is unverified for arbitrary EVOLVED candidates)";
}


// =============================================================================
//  DriverIsWorkerCountInvariant — byte-identity across n_workers in {1, 2, 4}.
//
//  NOTE ON TDD FRAMING (agent.md §0 explicit deviation): this is a behavior-
//  preserving refactor, so classic red→green does not apply cleanly. The
//  discipline is: confirm this test is VACUOUS-GREEN on the pre-change sequential
//  code (worker count never entered the math), then IMPLEMENT the parallel
//  refactor, then confirm it STAYS GREEN (now a real race/ordering guard). The
//  full-suite golden digests (FactorySearchDriver.*, FactoryMineInto.*, etc.) are
//  the byte-identity proof. This mirrors the rationale in
//  EvalDigestIsWorkerInvariantAndMatchesSingleThread above.
// =============================================================================
TEST(FactorySearchDriver, DriverIsWorkerCountInvariant) {
  Fixture fx;

  // Population=32, generations=4 gives ample parallelism surface once the
  // parallel refactor lands (≥32 fresh compiles + fitness evals per generation).
  auto cfg = small_search_cfg(/*seed*/ 0xDEADBEEF42ULL, /*pop*/ 32, /*gens*/ 4);
  cfg.elites = 2;
  cfg.k_tournament = 3;

  // Run at three worker counts — reset the driver each time so there is no
  // shared state between runs (the driver is stateless across run() calls).
  cfg.n_workers = 1;
  const SearchResult r1 = fx.driver().run(cfg, empty_pool());

  cfg.n_workers = 2;
  const SearchResult r2 = fx.driver().run(cfg, empty_pool());

  cfg.n_workers = 4;
  const SearchResult r4 = fx.driver().run(cfg, empty_pool());

  // Digest — the load-bearing byte-identity check.
  EXPECT_EQ(r1.digest, r2.digest) << "digest must be worker-count-invariant (1 vs 2)";
  EXPECT_EQ(r1.digest, r4.digest) << "digest must be worker-count-invariant (1 vs 4)";

  // Trial count — distinct structures scored (CanonSet size) must be identical.
  EXPECT_EQ(r1.trial_count, r2.trial_count) << "trial_count must be invariant (1 vs 2)";
  EXPECT_EQ(r1.trial_count, r4.trial_count) << "trial_count must be invariant (1 vs 4)";

  // all_scored size + per-index canon_hash sequence — order-identity guard.
  ASSERT_EQ(r1.all_scored.size(), r2.all_scored.size())
      << "all_scored size must be invariant (1 vs 2)";
  ASSERT_EQ(r1.all_scored.size(), r4.all_scored.size())
      << "all_scored size must be invariant (1 vs 4)";
  for (atx::usize i = 0; i < r1.all_scored.size(); ++i) {
    EXPECT_EQ(r1.all_scored[i].canon_hash, r2.all_scored[i].canon_hash)
        << "all_scored[" << i << "].canon_hash mismatch (1 vs 2)";
    EXPECT_EQ(r1.all_scored[i].canon_hash, r4.all_scored[i].canon_hash)
        << "all_scored[" << i << "].canon_hash mismatch (1 vs 4)";
  }

  // best_fitness_per_gen vector — full sequence must be equal.
  EXPECT_EQ(r1.best_fitness_per_gen, r2.best_fitness_per_gen)
      << "best_fitness_per_gen must be invariant (1 vs 2)";
  EXPECT_EQ(r1.best_fitness_per_gen, r4.best_fitness_per_gen)
      << "best_fitness_per_gen must be invariant (1 vs 4)";
}

}  // namespace atxtest_factory_search_driver_test
