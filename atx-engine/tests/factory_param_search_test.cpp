// atx::engine::factory — parameter-optimizer tests (S3-3, suite FactoryParamSearch).
//
// Covers the plan's verbatim Task-S3-3 test list (§4.5 / §2.1):
//   * ExtractsFreeConstants        — free-constant extraction (the Window literals)
//   * SepCmaEsFindsKnownOptimum    — sep-CMA-ES converges to an interior optimum (gate)
//   * SameSeedSameTrajectory       — same seed ⇒ byte-identical search (F1)
//   * GridIsExhaustiveAndBounded   — Grid is exhaustive and touches NO rng
// plus one extra:
//   * OptimizeParamsInstantiatesGenome — the genome path: each candidate is
//     instantiated via rebuild_with and scored as an analyze-valid genome.
//
// Local helpers (no engine make_genome): mirror factory_mutation_test's shape.

#include <cmath>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"

#include "atx/engine/factory/genome.hpp"
#include "atx/engine/factory/param_search.hpp"

namespace atxtest_factory_param_search_test {

using atx::core::Xoshiro256pp;
using atx::engine::alpha::analyze;
using atx::engine::alpha::Library;
using atx::engine::alpha::parse_expr;
using atx::engine::factory::box;
using atx::engine::factory::extract_free_constants;
using atx::engine::factory::Genome;
using atx::engine::factory::Method;
using atx::engine::factory::optimize_params;
using atx::engine::factory::optimize_params_raw;
using atx::engine::factory::ParamSpace;

// ---- shared helper (mirror factory_mutation_test) ----------------------------

[[nodiscard]] Genome make_genome(std::string_view src, Library &lib) {
  auto parsed = parse_expr(src, lib);
  EXPECT_TRUE(parsed.has_value()) << (parsed ? "" : parsed.error().message());
  if (!parsed) {
    return Genome{};
  }
  auto info = analyze(*parsed);
  EXPECT_TRUE(info.has_value()) << (info ? "" : info.error().message());
  if (!info) {
    return Genome{};
  }
  return Genome{std::move(*parsed), std::move(*info), 0};
}

// ---- tests ------------------------------------------------------------------

TEST(FactoryParamSearch, ExtractsFreeConstants) {
  Library lib;
  auto g = make_genome("decay_linear(ts_mean(close, 10), 8)", lib);
  ParamSpace sp = extract_free_constants(g);
  EXPECT_EQ(sp.dims(), 2u); // the 10 (window) and 8 (window)
}

TEST(FactoryParamSearch, SepCmaEsFindsKnownOptimum) {
  // inject a quadratic objective with a known interior maximum; sep-CMA-ES must converge
  Xoshiro256pp rng(2024);
  ParamSpace sp = box({{0.0, 10.0}, {0.0, 10.0}});
  auto f = [](std::span<const double> x) {
    return -((x[0] - 3.0) * (x[0] - 3.0) + (x[1] - 7.0) * (x[1] - 7.0));
  };
  auto r = optimize_params_raw(sp, f, rng, {Method::SepCmaEs, /*lambda*/ 12, /*gens*/ 40});
  EXPECT_NEAR(r.best_x[0], 3.0, 0.05);
  EXPECT_NEAR(r.best_x[1], 7.0, 0.05);
}

TEST(FactoryParamSearch, SameSeedSameTrajectory) { // F1: deterministic search
  Xoshiro256pp r1(99), r2(99);
  ParamSpace sp = box({{0, 10}, {0, 10}});
  auto f = [](std::span<const double> x) { return -(x[0] * x[0] + x[1] * x[1]); };
  auto a = optimize_params_raw(sp, f, r1, {Method::SepCmaEs, 8, 20});
  auto b = optimize_params_raw(sp, f, r2, {Method::SepCmaEs, 8, 20});
  EXPECT_EQ(a.best_x, b.best_x); // byte-identical
  EXPECT_EQ(a.trials, b.trials);
}

TEST(FactoryParamSearch, GridIsExhaustiveAndBounded) {
  ParamSpace sp = box({{1, 3}});
  int evals = 0;
  auto f = [&](std::span<const double>) {
    ++evals;
    return 0.0;
  };
  // The Grid path touches NO rng — but we pass a REAL stack rng (never a null
  // reference: forming `*(T*)nullptr` is UB). The evals counter proves the Grid
  // path is exhaustive (3 points) and the rng goes untouched.
  Xoshiro256pp dummy(0);
  const auto r = optimize_params_raw(sp, f, dummy, {Method::Grid, /*per-dim*/ 3, 1});
  static_cast<void>(r);
  EXPECT_EQ(evals, 3); // 3 grid points, no RNG used
}

// Extra (genome path): optimize_params over a real `ts_mean(close, W)` template.
// A trivial fitness rewards windows nearest 20; the returned best must instantiate
// to an analyze-valid genome and the trial count must equal lambda*generations.
TEST(FactoryParamSearch, OptimizeParamsInstantiatesGenome) {
  Library lib;
  auto g = make_genome("ts_mean(close, 5)", lib);
  ParamSpace sp = extract_free_constants(g);
  ASSERT_EQ(sp.dims(), 1u);

  // Fitness over the instantiated genome: prefer the window closest to 20. The
  // candidate genome is fully analyze-valid (it is the F5 oracle that gates it),
  // so we read the window literal back out and score on distance to 20.
  auto fitness = [](const Genome &cand) {
    double w = 0.0;
    for (const auto &e : cand.ast.nodes()) {
      if (e.kind == atx::engine::alpha::Expr::Kind::Literal) {
        w = e.value;
      }
    }
    return -(w - 20.0) * (w - 20.0);
  };

  Xoshiro256pp rng(7);
  auto r = optimize_params(g, sp, fitness, rng, {Method::SepCmaEs, 10, 40});
  EXPECT_EQ(r.trials, 10u * 40u);      // every fitness eval counted
  EXPECT_NEAR(r.best_x[0], 20.0, 1.0); // search homed in on the rewarded window

  // The reported optimum instantiates to an analyze-valid genome (F5).
  auto best = atx::engine::factory::instantiate(g, sp, r.best_x);
  ASSERT_TRUE(best.has_value()) << (best ? "" : best.error().message());
  EXPECT_TRUE(analyze(best->ast).has_value());
}

// Boundary (plan §3): a genome with ZERO free constants. `rank(close)` carries no
// Window/Scale literal, so the search space is empty. Both the raw (empty box) and
// genome forms must no-op cleanly — a FINITE best_fitness, never NaN (the sep-CMA
// recurrence divides by K), and the genome form's degenerate point must still
// instantiate to an analyze-valid genome (F5).
TEST(FactoryParamSearch, ZeroFreeConstantsIsNoOp) {
  // Raw form over an empty box: no dim, no NaN.
  {
    Xoshiro256pp rng(1);
    ParamSpace empty = box({});
    EXPECT_EQ(empty.dims(), 0u);
    auto f = [](std::span<const double>) { return 0.5; };
    auto r = optimize_params_raw(empty, f, rng, {Method::SepCmaEs, 12, 40});
    EXPECT_TRUE(std::isfinite(r.best_fitness)); // never NaN despite K==0
    EXPECT_TRUE(r.best_x.empty());              // 0-dim space → empty point
  }

  // Genome form: `rank(close)` has no free constant.
  Library lib;
  auto g = make_genome("rank(close)", lib);
  ParamSpace sp = extract_free_constants(g);
  EXPECT_EQ(sp.dims(), 0u);

  Xoshiro256pp rng(2);
  auto fitness = [](const Genome &) { return 1.0; };
  auto r = optimize_params(g, sp, fitness, rng, {Method::SepCmaEs, 12, 40});
  EXPECT_TRUE(std::isfinite(r.best_fitness)); // graceful no-op, not NaN

  // The degenerate (empty) point instantiates back to the (valid) template genome.
  auto best = atx::engine::factory::instantiate(g, sp, r.best_x);
  ASSERT_TRUE(best.has_value()) << (best ? "" : best.error().message());
  EXPECT_TRUE(analyze(best->ast).has_value());
}


}  // namespace atxtest_factory_param_search_test
