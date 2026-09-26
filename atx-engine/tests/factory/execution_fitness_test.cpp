// Postimplementation checks for the actual factory/search execution consumer.
#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/parser.hpp"
#include "atx/engine/alpha/registry.hpp"
#include "atx/engine/alpha/typecheck.hpp"
#include "atx/engine/alpha/vm.hpp"
#include "atx/engine/cost/cost_surface.hpp"
#include "atx/engine/factory/fitness.hpp"
#include "atx/engine/factory/pool_view.hpp"
#include "atx/engine/factory/search_driver.hpp"
#include <bit>
#include <cmath>
#include <gtest/gtest.h>
#include <span>
#include <string>
#include <vector>

namespace {
using atx::f64;
using atx::usize;
namespace eng = atx::engine;
namespace fac = eng::factory;
namespace alpha = eng::alpha;
namespace cost = eng::cost;
constexpr usize dates = 16, names = 4;

alpha::Panel panel() {
  std::vector<f64> prices(dates * names);
  for (usize d = 0; d < dates; ++d)
    for (usize i = 0; i < names; ++i)
      prices[d * names + i] = 100.0 + 10.0 * static_cast<f64>(i) +
                              static_cast<f64>((d * (i + 1) * 7) % 13) + 0.2 * static_cast<f64>(d);
  auto result = alpha::Panel::create(dates, names, {"close"}, {std::move(prices)}, {});
  EXPECT_TRUE(result);
  return std::move(result.value());
}
eng::exec::ExecutionSimulator simulator() {
  using namespace eng::exec;
  return ExecutionSimulator{FillCfg{},
                            SlippageCfg{SlippageMode::VolumeShare, 0, 0, 0, 0},
                            ImpactCfg{0, 0.5, 0},
                            CommissionCfg{CommissionMode::PerShare, 0, 0, 1, 0},
                            LatencyCfg{},
                            VolumeCapCfg{1}};
}
struct Fixture {
  alpha::Library library{};
  alpha::Panel data = panel();
  eng::WeightPolicy policy{};
  eng::exec::ExecutionSimulator sim = simulator();
  Fixture() { policy.winsorize_limit = 0; }
  fac::Genome genome() {
    auto ast = alpha::parse_expr("close", library);
    EXPECT_TRUE(ast);
    auto info = alpha::analyze(*ast);
    EXPECT_TRUE(info);
    return fac::Genome{std::move(*ast), std::move(*info), 0};
  }
  atx::core::Result<fac::ExecutionObjectiveContext> context(bool priced = true,
                                                            atx::u64 budget = 16U * 1024U * 1024U) {
    fac::ExecutionObjectiveConfig cfg;
    cfg.rule = fac::ExecutionObjectiveRule::DelayedSurfaceV2;
    cfg.initial_nav = 1000;
    cfg.min_names = 2;
    cfg.max_working_bytes = budget;
    cfg.window_end = dates;
    cfg.maturity_end = dates;
    std::vector<atx::i64> marks(dates), decisions(dates);
    constexpr atx::i64 day = 86'400'000'000'000LL;
    for (usize d = 0; d < dates; ++d) {
      marks[d] = (static_cast<atx::i64>(d) + 1) * day;
      decisions[d] = marks[d] + 1;
    }
    std::vector<atx::u64> ids{10, 11, 12, 13};
    std::vector<cost::CostSurface> surfaces;
    for (usize d = 0; d < dates - 2; ++d) {
      cost::CostSurfaceRecipe recipe;
      recipe.rule = cost::CostSurfaceRule::ModeledInputsV2;
      recipe.impact_y = 0;
      recipe.commission_bps = 2;
      std::vector<cost::CostSurfaceRow> rows(names);
      for (usize i = 0; i < names; ++i) {
        auto &row = rows[i];
        row.instrument_id = ids[i];
        row.state = priced ? cost::CostInputState::Available : cost::CostInputState::Unavailable;
        row.available_at_ns = marks[d];
        row.adv_dollars = 1e8;
        row.daily_vol = .02;
        row.full_spread = .0002;
        row.borrow_state = cost::CostInputState::Available;
        row.borrow_available_at_ns = marks[d];
        row.borrow_annual_fraction = .02;
      }
      cost::CostSurfaceIdentity id{decisions[d], std::string(64, 'a'), "raw-dollar-adv/synthetic",
                                   "modeled-fixture"};
      auto surface = cost::CostSurface::create(recipe, id, rows);
      if (!surface)
        return atx::core::Err(surface.error());
      surfaces.push_back(std::move(*surface));
    }
    std::vector<atx::u32> guard(dates * names, 0);
    return fac::prepare_execution_objective(
        data, policy, cfg, surfaces, marks, decisions, ids,
        {std::string(64, 'b'), "synthetic-train", "total-return-mark-fixture"}, {}, guard);
  }
  fac::SearchDriver driver() {
    return fac::SearchDriver{library, data, policy, sim, {"close", "rank(close)"}, {"close"}};
  }
};
fac::SearchConfig search_config(const fac::ExecutionObjectiveContext &context) {
  fac::SearchConfig cfg;
  cfg.population = 2;
  cfg.generations = 1;
  cfg.elites = 1;
  cfg.k_tournament = 2;
  cfg.n_workers = 1;
  cfg.master_seed = 37;
  cfg.fitness.execution = context.config();
  return cfg;
}

TEST(ExecutionFitnessV2, ReusesSignalAndMatchesMatureNetMomentsWithoutZeroPadding) {
  Fixture f;
  auto context = f.context();
  ASSERT_TRUE(context);
  auto genome = f.genome();
  auto program = alpha::compile(genome.ast, genome.analysis);
  ASSERT_TRUE(program);
  alpha::Engine engine{f.data};
  auto signal = engine.evaluate(*program);
  ASSERT_TRUE(signal);
  auto stream = fac::extract_execution_streams(*signal, *context);
  ASSERT_TRUE(stream);
  fac::FitnessCfg cfg;
  cfg.execution = context->config();
  cfg.execution_context = &*context;
  eng::combine::AlphaStore empty;
  auto result = fac::pool_aware_fitness(genome, empty, f.data, f.policy, f.sim, cfg, nullptr,
                                        &engine, &*signal);
  ASSERT_TRUE(result) << result.error().to_string();
  const auto begin = context->first_realization(), end = context->realization_end();
  f64 mean = 0, square = 0, turnover = 0;
  for (usize t = begin; t < end; ++t) {
    mean += stream->pnl_flat[t];
    turnover += stream->turnover_flat[t];
  }
  mean /= static_cast<f64>(end - begin);
  for (usize t = begin; t < end; ++t)
    square += (stream->pnl_flat[t] - mean) * (stream->pnl_flat[t] - mean);
  const auto expected = std::sqrt(252.0) * mean / std::sqrt(square / static_cast<f64>(end - begin));
  EXPECT_NEAR(result->wq, expected, 1e-12);
  EXPECT_DOUBLE_EQ(result->raw, result->wq);
  EXPECT_NEAR(result->turnover, turnover / static_cast<f64>(end - begin), 1e-14);
  cfg.trial_count = 2000;
  const auto high_trials = fac::pool_aware_fitness(genome, empty, f.data, f.policy, f.sim, cfg,
                                                 nullptr, &engine, &*signal);
  ASSERT_TRUE(high_trials);
  EXPECT_DOUBLE_EQ(high_trials->raw, result->raw);
  EXPECT_LE(high_trials->dsr, result->dsr);
  EXPECT_EQ(result->execution_context_sha256, context->identity_sha256());
  EXPECT_EQ(result->execution_rule, fac::ExecutionObjectiveRule::DelayedSurfaceV2);
  ASSERT_EQ(result->descriptor.size(), dates);
  for (usize t = 0; t < dates; ++t) {
    if (t < begin || t >= end)
      EXPECT_TRUE(std::isnan(result->descriptor[t]));
    else
      EXPECT_DOUBLE_EQ(result->descriptor[t], stream->pnl_flat[t]);
  }
}

TEST(ExecutionFitnessV2, SearchInjectsContextAndIsWorkerInvariant) {
  Fixture f;
  auto context = f.context();
  ASSERT_TRUE(context);
  auto cfg = search_config(*context);
  eng::combine::AlphaStore empty;
  const auto one = f.driver().run(cfg, empty, nullptr, nullptr, nullptr, &*context);
  ASSERT_FALSE(one.execution_invalid) << one.execution_error;
  EXPECT_GT(one.trial_count, 0U);
  EXPECT_EQ(one.execution_context_sha256, context->identity_sha256());
  cfg.n_workers = 2;
  const auto two = f.driver().run(cfg, empty, nullptr, nullptr, nullptr, &*context);
  ASSERT_FALSE(two.execution_invalid) << two.execution_error;
  EXPECT_EQ(one.digest, two.digest);
  EXPECT_EQ(one.best_fitness_per_gen, two.best_fitness_per_gen);
}

TEST(ExecutionFitnessV2, RefusesUnpriceableExecutionInsteadOfZeroFitness) {
  Fixture f;
  auto context = f.context(false);
  ASSERT_TRUE(context);
  auto cfg = search_config(*context);
  eng::combine::AlphaStore empty;
  const auto result = f.driver().run(cfg, empty, nullptr, nullptr, nullptr, &*context);
  EXPECT_TRUE(result.execution_invalid);
  EXPECT_NE(result.execution_error.find("unpriceable"), std::string::npos);
  EXPECT_TRUE(result.admitted_candidates.empty());
}

TEST(ExecutionFitnessV2, RefusesMissingMismatchFidelityUnboundPoolAndWorkerBudget) {
  Fixture f;
  auto context = f.context();
  ASSERT_TRUE(context);
  auto cfg = search_config(*context);
  eng::combine::AlphaStore empty;
  EXPECT_TRUE(f.driver().run(cfg, empty).execution_invalid);
  auto mismatch = cfg;
  ++mismatch.fitness.execution.delay;
  EXPECT_TRUE(
      f.driver().run(mismatch, empty, nullptr, nullptr, nullptr, &*context).execution_invalid);
  auto fidelity = cfg;
  fidelity.fidelity.enabled = true;
  EXPECT_TRUE(
      f.driver().run(fidelity, empty, nullptr, nullptr, nullptr, &*context).execution_invalid);
  auto deflated = cfg;
  deflated.deflate_selection = true;
  const auto dsr_refused = f.driver().run(deflated, empty, nullptr, nullptr, nullptr, &*context);
  EXPECT_TRUE(dsr_refused.execution_invalid);
  EXPECT_NE(dsr_refused.execution_error.find("DSR"), std::string::npos);
  EXPECT_TRUE(dsr_refused.all_scored.empty());
  EXPECT_TRUE(dsr_refused.admitted_candidates.empty());
  eng::combine::AlphaStore pool;
  std::vector<f64> pnl(dates, 0), positions(dates * names, 0);
  ASSERT_TRUE(pool.insert(nullptr, pnl, positions, {}));
  EXPECT_TRUE(f.driver().run(cfg, pool, nullptr, nullptr, nullptr, &*context).execution_invalid);
  auto bounded = f.context(true, context->bytes() + context->per_signal_working_bytes());
  ASSERT_TRUE(bounded);
  auto workers = search_config(*bounded);
  workers.n_workers = 2;
  const auto refused = f.driver().run(workers, empty, nullptr, nullptr, nullptr, &*bounded);
  EXPECT_TRUE(refused.execution_invalid);
  EXPECT_NE(refused.execution_error.find("budget"), std::string::npos);
}

TEST(ExecutionFitnessV2, ExplicitLegacyIgnoresExecutionContextAndInactiveKnobs) {
  Fixture f;
  auto context = f.context();
  ASSERT_TRUE(context);
  auto genome = f.genome();
  eng::combine::AlphaStore empty;
  fac::FitnessCfg legacy;
  auto a = fac::pool_aware_fitness(genome, empty, f.data, f.policy, f.sim, legacy);
  ASSERT_TRUE(a);
  legacy.execution_context = &*context;
  legacy.execution.delay = 999;
  legacy.execution.initial_nav = -1;
  legacy.execution.max_working_bytes = 1;
  auto b = fac::pool_aware_fitness(genome, empty, f.data, f.policy, f.sim, legacy);
  ASSERT_TRUE(b);
  EXPECT_EQ(std::bit_cast<atx::u64>(a->raw), std::bit_cast<atx::u64>(b->raw));
  EXPECT_EQ(std::bit_cast<atx::u64>(a->dsr), std::bit_cast<atx::u64>(b->dsr));
  EXPECT_EQ(a->descriptor, b->descriptor);
  EXPECT_EQ(b->execution_rule, fac::ExecutionObjectiveRule::LegacyStreamsV1);
  EXPECT_TRUE(b->execution_context_sha256.empty());
}
} // namespace
