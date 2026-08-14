#include "backtest/quant_pipeline.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace atx::vol {
namespace {

[[nodiscard]] std::filesystem::path pipeline_test_root() {
  static std::atomic<std::uint64_t> sequence{0u};
  const auto root = std::filesystem::temp_directory_path() /
                    ("atx_quant_pipeline_" + std::to_string(sequence.fetch_add(1u)));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  return root;
}

[[nodiscard]] BacktestSeriesInfo series_info(std::size_t rows) {
  BacktestSeriesInfo info;
  info.template_id = "three-month-strangle";
  info.template_fingerprint = 0x111u;
  info.symbol = "SPY";
  info.uid = 101u;
  info.row_count = rows;
  info.run_identity_hash = 0xabcdefu;
  info.partition_filename = "partition.atxrun";
  info.partition_identity = ArchiveContentIdentity{4096u, 123u, 456u, 789u};
  return info;
}

[[nodiscard]] BacktestSeriesData series_data(std::size_t rows) {
  BacktestSeriesData data;
  data.backtest.ts_ns.reserve(rows);
  data.backtest.date.reserve(rows);
  data.backtest.pnl_total.reserve(rows);
  std::vector<double> signal;
  signal.reserve(rows);
  for (std::size_t i = 0; i < rows; ++i) {
    data.backtest.ts_ns.push_back(static_cast<std::int64_t>(i + 1u) * 100);
    data.backtest.date.push_back("2026-01-" + std::to_string(10u + i));
    data.backtest.pnl_total.push_back(i == 0u ? 0.0 : (i % 3u == 0u ? -2.0 : 3.0));
    signal.push_back(static_cast<double>(static_cast<int>(i % 5u) - 2));
  }
  data.backtest.signals.emplace_back("implied_correlation", std::move(signal));
  return data;
}

[[nodiscard]] BacktestSignalResearchSpec research_spec() {
  BacktestSignalResearchSpec spec;
  spec.signal_name = "implied_correlation";
  spec.lagged_capital = 1'000.0;
  spec.validation.min_train_groups = 8u;
  spec.validation.test_groups = 4u;
  spec.validation.step_groups = 4u;
  spec.newey_west_lag = 1u;
  spec.candidates = {
      ResearchSignalCandidate{"level-long", ResearchSignalTransform::Identity, 0u, 0u,
                              ResearchSignalDirection::LongHigh},
      ResearchSignalCandidate{"change-long", ResearchSignalTransform::Difference, 0u, 1u,
                              ResearchSignalDirection::LongHigh},
  };
  spec.family = ResearchTrialFamily{true, spec.candidates.size()};
  return spec;
}

TEST(QuantPipeline, MapsCloseSignalToNextObservationAndFollowingOutcome) {
  const BacktestSeriesData data = series_data(18u);
  const BacktestSeriesInfo info = series_info(data.backtest.size());
  const auto result = mine_backtest_signal_series(info, data, research_spec());
  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->observations.size(), data.backtest.size() - 2u);
  const ResearchObservation &first = result->observations.front();
  EXPECT_EQ(first.decision_ts_ns, data.backtest.ts_ns[0]);
  EXPECT_EQ(first.execution_ts_ns, data.backtest.ts_ns[1]);
  EXPECT_EQ(first.label_end_ts_ns, data.backtest.ts_ns[2]);
  EXPECT_DOUBLE_EQ(first.forward_pnl, data.backtest.pnl_total[2]);
  EXPECT_EQ(first.source_identity, info.partition_identity);
  EXPECT_FALSE(result->validation.folds.empty());
  EXPECT_FALSE(result->mining.selected_candidate_id.empty());
}

TEST(QuantPipeline, RejectsUnknownOrNonParallelSignalAndUnsealedFamily) {
  BacktestSeriesData data = series_data(18u);
  const BacktestSeriesInfo info = series_info(data.backtest.size());
  BacktestSignalResearchSpec spec = research_spec();
  spec.signal_name = "unknown";
  EXPECT_FALSE(mine_backtest_signal_series(info, data, spec));

  spec = research_spec();
  data.backtest.signals.front().second.pop_back();
  EXPECT_FALSE(mine_backtest_signal_series(info, data, spec));

  data = series_data(18u);
  spec.family.sealed = false;
  EXPECT_FALSE(mine_backtest_signal_series(info, data, spec));
}

TEST(QuantPipeline, PublishesImmutableOosTrialWithExactBacktestDependency) {
  const BacktestSeriesData data = series_data(18u);
  const BacktestSeriesInfo info = series_info(data.backtest.size());
  const BacktestSignalResearchSpec spec = research_spec();
  auto result = mine_backtest_signal_series(info, data, spec);
  ASSERT_TRUE(result) << result.error().to_string();

  const std::filesystem::path root = pipeline_test_root();
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db) << db.error().to_string();
  auto published = publish_backtest_signal_trial(
      *db, info, spec, *result, ResearchTrialPublishSpec{"trials/spy/implied-correlation", {}});
  ASSERT_TRUE(published) << published.error().to_string();
  EXPECT_EQ(published->kind, ResearchArtifactKind::Trial);

  auto dependencies = db->load_dependencies(published->artifact_id);
  ASSERT_TRUE(dependencies) << dependencies.error().to_string();
  ASSERT_EQ(dependencies->size(), 1u);
  EXPECT_EQ(dependencies->front().archive_identity, info.partition_identity);

  auto returns = db->map_section(published->artifact_id, "oos_returns");
  ASSERT_TRUE(returns) << returns.error().to_string();
  EXPECT_GT(returns->view.n_rows(), 0u);
  EXPECT_EQ(returns->view.f64_col("value").size(), returns->view.n_rows());
  returns->view = {};
  returns->archive.reset();
  std::filesystem::remove_all(root);
}

TEST(QuantPipeline, PublicationRevalidatesLineageFoldsFamilyAndEvidence) {
  const BacktestSeriesData data = series_data(18u);
  const BacktestSeriesInfo info = series_info(data.backtest.size());
  const BacktestSignalResearchSpec spec = research_spec();
  auto valid = mine_backtest_signal_series(info, data, spec);
  ASSERT_TRUE(valid) << valid.error().to_string();

  const std::filesystem::path root = pipeline_test_root();
  auto db = ResearchDb::create(root.string());
  ASSERT_TRUE(db) << db.error().to_string();
  const ResearchTrialPublishSpec publication{"trials/spy/audit", {}};

  BacktestSignalResearchResult mutated = *valid;
  mutated.observations.front().source_identity.header_crc32c += 1u;
  EXPECT_FALSE(publish_backtest_signal_trial(*db, info, spec, mutated, publication));

  mutated = *valid;
  mutated.validation.folds.front().train_indices.push_back(
      mutated.validation.folds.front().test_indices.front());
  EXPECT_FALSE(publish_backtest_signal_trial(*db, info, spec, mutated, publication));

  mutated = *valid;
  mutated.mining.evaluations.front().oos_returns.front().value += 1.0;
  EXPECT_FALSE(publish_backtest_signal_trial(*db, info, spec, mutated, publication));

  BacktestSignalResearchSpec unsealed = spec;
  unsealed.family.sealed = false;
  EXPECT_FALSE(publish_backtest_signal_trial(*db, info, unsealed, *valid, publication));
  EXPECT_TRUE(db->artifacts().empty());
  std::filesystem::remove_all(root);
}

TEST(QuantPipeline, MapsAuthoritativeDispersionBookPositionsToSymbols) {
  DispersionBook book;
  book.index_leg.symbol = "SPX";
  book.index_leg.uid = 1u;
  book.index_leg.straddle_qty = -2.0;
  DispersionLeg name;
  name.symbol = "AAPL";
  name.uid = 2u;
  name.straddle_qty = 3.0;
  book.name_legs.push_back(name);
  book.positions = {
      Position{1u, OptionContract{1u, 100.0, 0.25, Side::Call}, -2.0, 100.0},
      Position{2u, OptionContract{1u, 100.0, 0.25, Side::Put}, -2.0, 100.0},
      Position{3u, OptionContract{2u, 50.0, 0.25, Side::Call}, 3.0, 100.0},
      Position{4u, OptionContract{2u, 50.0, 0.25, Side::Put}, 3.0, 100.0},
  };
  auto mapped = dispersion_named_positions(book);
  ASSERT_TRUE(mapped) << mapped.error().to_string();
  ASSERT_EQ(mapped->size(), 4u);
  EXPECT_EQ((*mapped)[0].symbol, "SPX");
  EXPECT_EQ((*mapped)[1].symbol, "SPX");
  EXPECT_EQ((*mapped)[2].symbol, "AAPL");
  EXPECT_EQ((*mapped)[3].symbol, "AAPL");

  book.positions.back().qty = 4.0;
  EXPECT_FALSE(dispersion_named_positions(book));
}

TEST(QuantPipeline, ComposesTargetRiskScenarioClampAndResearchIntent) {
  const std::vector<NamedPosition> target = {
      NamedPosition{"SPX", Position{1u, OptionContract{1u, 100.0, 0.25, Side::Put}, 2.0, 100.0}},
      NamedPosition{"AAPL", Position{2u, OptionContract{2u, 50.0, 0.25, Side::Call}, -1.0, 100.0}},
  };
  AmericanGreeks index_greeks;
  index_greeks.price = 5.0;
  index_greeks.delta = 0.5;
  index_greeks.gamma = 0.01;
  index_greeks.vega = 2.0;
  index_greeks.theta = -0.1;
  AmericanGreeks name_greeks = index_greeks;
  name_greeks.price = 3.0;
  name_greeks.delta = -0.5;

  const std::vector<PositionRiskInput> risk = {
      PositionRiskInput{target[0].position, index_greeks},
      PositionRiskInput{target[1].position, name_greeks},
  };
  const std::vector<ScenarioRiskInput> scenarios = {
      ScenarioRiskInput{target[0].position, 100.0, index_greeks},
      ScenarioRiskInput{target[1].position, 50.0, name_greeks},
  };
  StrategyImplementationSpec spec;
  spec.strategy_fingerprint = 0x1234u;
  spec.decision_ts_ns = 1'000;
  spec.risk_limits.max_abs_delta = 75.0;
  ConditionalComponentScenario scenario;
  scenario.index_uid = 1u;
  scenario.index_spot_pct = -0.05;
  scenario.index_vol_bump = 0.02;
  scenario.components = {ComponentShockModel{2u, 1.2, 0.0, 1.0, 0.0}};
  spec.scenarios.push_back(scenario);
  spec.hedge_targets.push_back(HedgeTarget{1u, 0.0, 100.0});

  auto plan = build_strategy_implementation_plan(target, {}, risk, scenarios, spec);
  ASSERT_TRUE(plan) << plan.error().to_string();
  EXPECT_DOUBLE_EQ(plan->unconstrained_risk.delta, 150.0);
  EXPECT_DOUBLE_EQ(plan->risk_overlay.scale, 0.5);
  ASSERT_EQ(plan->risk_adjusted_target.size(), 2u);
  EXPECT_DOUBLE_EQ(plan->risk_adjusted_target[0].position.qty, 1.0);
  EXPECT_DOUBLE_EQ(plan->risk_adjusted_target[1].position.qty, -0.5);
  ASSERT_EQ(plan->scenario_results.size(), 1u);
  EXPECT_EQ(plan->intent.disposition, IntentDisposition::ResearchOnly);
  ASSERT_EQ(plan->intent.option_orders.size(), 2u);
  ASSERT_EQ(plan->intent.hedges.size(), 1u);
  EXPECT_DOUBLE_EQ(plan->intent.hedges.front().target_shares, 50.0);
}

TEST(QuantPipeline, ImplementationPlanRequiresScenarioEvidence) {
  const NamedPosition target{"SPX",
                             Position{1u, OptionContract{1u, 100.0, 0.25, Side::Put}, 2.0, 100.0}};
  AmericanGreeks greeks;
  greeks.price = 5.0;
  const std::vector<PositionRiskInput> risk = {
      PositionRiskInput{target.position, greeks},
  };

  StrategyImplementationSpec spec;
  spec.strategy_fingerprint = 0x1234u;
  spec.decision_ts_ns = 1'000;
  EXPECT_FALSE(build_strategy_implementation_plan(std::span{&target, 1u}, {}, risk, {}, spec));

  const std::vector<ScenarioRiskInput> orphaned_scenario_input = {
      ScenarioRiskInput{target.position, 100.0, greeks},
  };
  EXPECT_FALSE(build_strategy_implementation_plan(std::span{&target, 1u}, {}, risk,
                                                  orphaned_scenario_input, spec));
}

TEST(QuantPipeline, AlignsImplementationInputsByStableIdAndGreekSnapshot) {
  const std::vector<NamedPosition> target = {
      NamedPosition{"SPX", Position{1u, OptionContract{1u, 100.0, 0.25, Side::Put}, 2.0, 100.0}},
      NamedPosition{"AAPL", Position{2u, OptionContract{2u, 50.0, 0.25, Side::Call}, -1.0, 100.0}},
  };
  AmericanGreeks index_greeks;
  index_greeks.price = 5.0;
  index_greeks.delta = 0.5;
  AmericanGreeks name_greeks;
  name_greeks.price = 3.0;
  name_greeks.delta = -0.5;

  const std::vector<PositionRiskInput> reversed_risk = {
      PositionRiskInput{target[1].position, name_greeks},
      PositionRiskInput{target[0].position, index_greeks},
  };
  std::vector<ScenarioRiskInput> reversed_scenario_inputs = {
      ScenarioRiskInput{target[1].position, 50.0, name_greeks},
      ScenarioRiskInput{target[0].position, 100.0, index_greeks},
  };
  StrategyImplementationSpec spec;
  spec.strategy_fingerprint = 0x1234u;
  spec.decision_ts_ns = 1'000;
  ConditionalComponentScenario scenario;
  scenario.index_uid = 1u;
  scenario.index_spot_pct = -0.05;
  scenario.index_vol_bump = 0.02;
  scenario.components = {ComponentShockModel{2u, 1.2, 0.0, 1.0, 0.0}};
  spec.scenarios.push_back(scenario);

  EXPECT_TRUE(build_strategy_implementation_plan(target, {}, reversed_risk,
                                                 reversed_scenario_inputs, spec));

  reversed_scenario_inputs[0].greeks_per_share.delta = -0.4;
  EXPECT_FALSE(build_strategy_implementation_plan(target, {}, reversed_risk,
                                                  reversed_scenario_inputs, spec));

  reversed_scenario_inputs[0].greeks_per_share = name_greeks;
  reversed_scenario_inputs[0].position.qty = -2.0;
  EXPECT_FALSE(build_strategy_implementation_plan(target, {}, reversed_risk,
                                                  reversed_scenario_inputs, spec));
}

} // namespace
} // namespace atx::vol
