#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

#include "atx/options/option_pretrade_risk.hpp"

namespace option_replay_alloc {
extern std::atomic<std::size_t> g_count;
extern std::atomic<bool> g_armed;
} // namespace option_replay_alloc

namespace {

using atx::options::research::OptionInstrument;
using atx::options::risk::option_pretrade_risk_required_workspace_bytes;
using atx::options::risk::OptionPreTradeRiskEngine;
using atx::options::risk::OptionRiskBreach;
using atx::options::risk::OptionRiskContentDigest;
using atx::options::risk::OptionRiskContractRow;
using atx::options::risk::OptionRiskDisposition;
using atx::options::risk::OptionRiskEngineLimits;
using atx::options::risk::OptionRiskHardLimits;
using atx::options::risk::OptionRiskLeaf;
using atx::options::risk::OptionRiskPanel;
using atx::options::risk::OptionRiskPanelLimits;
using atx::options::risk::OptionRiskPanelProvenance;
using atx::options::risk::OptionRiskRowStatus;
using atx::options::risk::OptionRiskScenario;
using atx::options::risk::OptionRiskScenarioPnlRow;
using atx::vol::ArchiveContentIdentity;
using atx::vol::ExerciseStyle;

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) noexcept {
  return ArchiveContentIdentity{10'000U + seed, 20'000U + seed,
                                static_cast<std::uint32_t>(30'000U + seed),
                                static_cast<std::uint32_t>(40'000U + seed)};
}

[[nodiscard]] OptionRiskContentDigest digest(std::uint8_t seed) noexcept {
  OptionRiskContentDigest out;
  for (std::size_t index = 0; index < out.bytes.size(); ++index) {
    out.bytes[index] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
  }
  return out;
}

[[nodiscard]] OptionRiskPanelProvenance provenance() noexcept {
  return OptionRiskPanelProvenance{11U, 12U, digest(1U), digest(65U)};
}

[[nodiscard]] OptionRiskContractRow contract_row(std::int64_t date, std::uint64_t contract_id,
                                                 std::uint32_t underlier_uid) noexcept {
  OptionRiskContractRow out;
  out.decision_ts_ns = date;
  out.contract_id = contract_id;
  out.engine_id.id = static_cast<std::uint32_t>(contract_id);
  out.underlier_uid = underlier_uid;
  out.observed_ts_ns = date - 20;
  out.available_ts_ns = date - 10;
  out.market_observed_ts_ns = date - 5;
  out.market_available_ts_ns = date;
  out.definition_available_ts_ns = date - 10;
  out.expiry_ts_ns = 10'000;
  out.strike = 100.0;
  out.side = atx::vol::Side::Call;
  out.multiplier = 100.0;
  out.standard_deliverable = true;
  out.definition_source_identity = identity(contract_id + 600U);
  out.spot_delta_cash_per_contract = contract_id == 10U ? 1.0 : -1.0;
  out.spot_gamma_cash_per_contract = 2.0;
  out.vega_cash_per_vol_point_per_contract = contract_id == 10U ? 3.0 : -3.0;
  out.theta_cash_per_day_per_contract = -1.0;
  out.vanna_cash_per_return_vol_point_per_contract = contract_id == 10U ? 0.5 : -0.5;
  out.volga_cash_per_vol_point_squared_per_contract = 0.25;
  out.premium_cash_notional_per_contract = 100.0;
  out.risk_source_identity = identity(contract_id + static_cast<std::uint64_t>(date));
  out.surface_source_identity = identity(contract_id + 500U);
  out.market_source_identity = identity(contract_id + 700U);
  return out;
}

[[nodiscard]] OptionRiskScenarioPnlRow scenario_row(std::int64_t date, std::uint64_t contract_id,
                                                    std::uint64_t scenario_id,
                                                    double pnl) noexcept {
  return OptionRiskScenarioPnlRow{date,
                                  contract_id,
                                  scenario_id,
                                  date - 20,
                                  date - 10,
                                  pnl,
                                  identity(contract_id + scenario_id + 900U)};
}

[[nodiscard]] OptionInstrument instrument(std::uint64_t contract_id,
                                          std::uint32_t underlier_uid) noexcept {
  OptionInstrument out;
  out.contract_id = contract_id;
  out.engine_id.id = static_cast<std::uint32_t>(contract_id);
  out.underlier_uid = underlier_uid;
  out.expiry_ts_ns = 10'000;
  out.strike = 100.0;
  out.multiplier = 100.0;
  return out;
}

[[nodiscard]] OptionRiskPanel make_panel(std::span<const std::int64_t> dates,
                                         std::span<const std::uint64_t> contract_ids,
                                         std::span<const std::uint32_t> underlier_uids,
                                         std::span<const std::uint64_t> scenario_ids,
                                         std::span<const double> pnl) {
  std::vector<OptionRiskContractRow> rows;
  rows.reserve(dates.size() * contract_ids.size());
  for (std::int64_t date : dates) {
    for (std::size_t contract_index = 0; contract_index < contract_ids.size(); ++contract_index) {
      rows.push_back(
          contract_row(date, contract_ids[contract_index], underlier_uids[contract_index]));
    }
  }
  std::vector<OptionRiskScenario> scenarios;
  scenarios.reserve(scenario_ids.size());
  for (std::uint64_t scenario_id : scenario_ids) {
    scenarios.push_back(OptionRiskScenario{scenario_id, identity(scenario_id + 700U)});
  }
  std::vector<OptionRiskScenarioPnlRow> pnl_rows;
  pnl_rows.reserve(pnl.size());
  std::size_t offset = 0U;
  for (std::int64_t date : dates) {
    for (std::uint64_t scenario_id : scenario_ids) {
      for (std::uint64_t contract_id : contract_ids) {
        pnl_rows.push_back(scenario_row(date, contract_id, scenario_id, pnl[offset]));
        ++offset;
      }
    }
  }
  const auto created =
      OptionRiskPanel::create(rows, scenarios, pnl_rows, provenance(), OptionRiskPanelLimits{});
  EXPECT_TRUE(created) << created.error().to_string();
  return std::move(*created);
}

[[nodiscard]] OptionRiskEngineLimits engine_limits() noexcept {
  OptionRiskEngineLimits out;
  out.max_contracts = 8U;
  out.max_live_leaves = 16U;
  out.max_candidate_leaves = 16U;
  out.max_scenarios = 8U;
  out.max_underliers = 8U;
  out.max_workspace_bytes = 1U * 1024U * 1024U;
  return out;
}

[[nodiscard]] std::uint32_t bit(OptionRiskBreach value) noexcept {
  return static_cast<std::uint32_t>(value);
}

TEST(OptionRiskPanel, InputPermutationHasCanonicalDefinitionAndValues) {
  const std::array<std::int64_t, 2> dates{100, 200};
  const std::array<std::uint64_t, 2> contracts{10U, 20U};
  const std::array<std::uint32_t, 2> underliers{1U, 2U};
  const std::array<std::uint64_t, 2> scenarios{5U, 2U};
  const std::array<double, 8> pnl{-10.0, 20.0, 11.0, -21.0, -12.0, 22.0, 13.0, -23.0};

  const OptionRiskPanel first = make_panel(dates, contracts, underliers, scenarios, pnl);

  std::vector<OptionRiskContractRow> rows;
  std::vector<OptionRiskScenarioPnlRow> pnl_rows;
  for (std::int64_t date : dates) {
    rows.push_back(contract_row(date, 20U, 2U));
    rows.push_back(contract_row(date, 10U, 1U));
  }
  const std::array<OptionRiskScenario, 2> scenario_rows{
      OptionRiskScenario{2U, identity(702U)},
      OptionRiskScenario{5U, identity(705U)},
  };
  // Values must follow the same keys as make_panel, not its caller ordering.
  pnl_rows.push_back(scenario_row(200, 20U, 2U, -23.0));
  pnl_rows.push_back(scenario_row(100, 10U, 5U, -10.0));
  pnl_rows.push_back(scenario_row(200, 10U, 5U, -12.0));
  pnl_rows.push_back(scenario_row(100, 20U, 2U, -21.0));
  pnl_rows.push_back(scenario_row(100, 20U, 5U, 20.0));
  pnl_rows.push_back(scenario_row(200, 10U, 2U, 13.0));
  pnl_rows.push_back(scenario_row(100, 10U, 2U, 11.0));
  pnl_rows.push_back(scenario_row(200, 20U, 5U, 22.0));

  const auto second =
      OptionRiskPanel::create(rows, scenario_rows, pnl_rows, provenance(), OptionRiskPanelLimits{});

  ASSERT_TRUE(second) << second.error().to_string();
  EXPECT_EQ(first.definition_hash(), second->definition_hash());
  EXPECT_TRUE(std::ranges::equal(first.dates(), second->dates()));
  EXPECT_TRUE(std::ranges::equal(first.contract_ids(), second->contract_ids()));
  EXPECT_TRUE(std::ranges::equal(first.scenario_ids(), second->scenario_ids()));
  for (std::size_t date_index = 0; date_index < dates.size(); ++date_index) {
    for (std::size_t scenario_index = 0; scenario_index < scenarios.size(); ++scenario_index) {
      for (std::size_t contract_index = 0; contract_index < contracts.size(); ++contract_index) {
        EXPECT_EQ(first.scenario_pnl(date_index, scenario_index, contract_index),
                  second->scenario_pnl(date_index, scenario_index, contract_index));
      }
    }
  }
}

TEST(OptionRiskPanel, MissingScenarioCoverageAndZeroDigestFailClosed) {
  const std::array rows{contract_row(100, 10U, 1U)};
  const std::array scenarios{OptionRiskScenario{1U, identity(701U)}};
  const std::array<OptionRiskScenarioPnlRow, 0> empty_pnl{};

  const auto missing =
      OptionRiskPanel::create(rows, scenarios, empty_pnl, provenance(), OptionRiskPanelLimits{});
  EXPECT_FALSE(missing);

  OptionRiskPanelProvenance bad = provenance();
  bad.risk_snapshot_digest = {};
  const std::array pnl_rows{scenario_row(100, 10U, 1U, -10.0)};
  const auto zero_digest =
      OptionRiskPanel::create(rows, scenarios, pnl_rows, bad, OptionRiskPanelLimits{});
  EXPECT_FALSE(zero_digest);
}

TEST(OptionRiskPanel, DefinitionHashCoversScenarioAndPnlRowProvenance) {
  const std::array rows{contract_row(100, 10U, 1U)};
  const std::array scenarios{OptionRiskScenario{1U, identity(701U)}};
  const std::array pnl_rows{scenario_row(100, 10U, 1U, -10.0)};
  const auto baseline =
      OptionRiskPanel::create(rows, scenarios, pnl_rows, provenance(), OptionRiskPanelLimits{});
  ASSERT_TRUE(baseline) << baseline.error().to_string();

  auto changed_scenarios = scenarios;
  changed_scenarios[0].source_identity = identity(702U);
  const auto changed_scenario = OptionRiskPanel::create(rows, changed_scenarios, pnl_rows,
                                                        provenance(), OptionRiskPanelLimits{});
  ASSERT_TRUE(changed_scenario) << changed_scenario.error().to_string();

  auto changed_pnl_rows = pnl_rows;
  changed_pnl_rows[0].available_ts_ns = 99;
  changed_pnl_rows[0].source_identity = identity(1'999U);
  const auto changed_pnl = OptionRiskPanel::create(rows, scenarios, changed_pnl_rows, provenance(),
                                                   OptionRiskPanelLimits{});
  ASSERT_TRUE(changed_pnl) << changed_pnl.error().to_string();

  EXPECT_NE(baseline->definition_hash(), changed_scenario->definition_hash());
  EXPECT_NE(baseline->definition_hash(), changed_pnl->definition_hash());
}

TEST(OptionRiskPanel, MarketMarkLineageIsValidatedAndCoveredByDefinitionHash) {
  const std::array scenarios{OptionRiskScenario{1U, identity(701U)}};
  const std::array pnl_rows{scenario_row(100, 10U, 1U, -10.0)};
  const std::array baseline_rows{contract_row(100, 10U, 1U)};
  const auto baseline = OptionRiskPanel::create(baseline_rows, scenarios, pnl_rows, provenance());
  ASSERT_TRUE(baseline) << baseline.error().to_string();

  auto changed_rows = baseline_rows;
  changed_rows[0].market_observed_ts_ns -= 1;
  changed_rows[0].definition_available_ts_ns -= 1;
  changed_rows[0].market_source_identity = identity(9'001U);
  const auto changed = OptionRiskPanel::create(changed_rows, scenarios, pnl_rows, provenance());
  ASSERT_TRUE(changed) << changed.error().to_string();
  EXPECT_NE(baseline->definition_hash(), changed->definition_hash());

  auto future_rows = baseline_rows;
  future_rows[0].market_available_ts_ns = future_rows[0].decision_ts_ns + 1;
  const auto future = OptionRiskPanel::create(future_rows, scenarios, pnl_rows, provenance());
  ASSERT_FALSE(future);
  EXPECT_EQ(future.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto future_definition_rows = baseline_rows;
  future_definition_rows[0].definition_available_ts_ns =
      future_definition_rows[0].market_observed_ts_ns + 1;
  const auto future_definition =
      OptionRiskPanel::create(future_definition_rows, scenarios, pnl_rows, provenance());
  ASSERT_FALSE(future_definition);
  EXPECT_EQ(future_definition.error().code(), atx::core::ErrorCode::InvalidArgument);

  auto missing_source_rows = baseline_rows;
  missing_source_rows[0].market_source_identity = {};
  const auto missing_source =
      OptionRiskPanel::create(missing_source_rows, scenarios, pnl_rows, provenance());
  ASSERT_FALSE(missing_source);
  EXPECT_EQ(missing_source.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionRiskPanel, ExerciseStyleIsValidatedAndCoveredByDefinitionHash) {
  const std::array scenarios{OptionRiskScenario{1U, identity(701U)}};
  const std::array pnl_rows{scenario_row(100, 10U, 1U, -10.0)};
  const std::array american_rows{contract_row(100, 10U, 1U)};
  auto european_rows = american_rows;
  european_rows.front().exercise_style = ExerciseStyle::European;

  const auto american = OptionRiskPanel::create(american_rows, scenarios, pnl_rows, provenance(),
                                                OptionRiskPanelLimits{});
  const auto european = OptionRiskPanel::create(european_rows, scenarios, pnl_rows, provenance(),
                                                OptionRiskPanelLimits{});

  ASSERT_TRUE(american) << american.error().to_string();
  ASSERT_TRUE(european) << european.error().to_string();
  EXPECT_NE(american->definition_hash(), european->definition_hash());

  european_rows.front().exercise_style = static_cast<ExerciseStyle>(255U);
  const auto invalid = OptionRiskPanel::create(european_rows, scenarios, pnl_rows, provenance(),
                                               OptionRiskPanelLimits{});
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionRiskPanel, ExerciseStyleCannotChangeAcrossDates) {
  std::array rows{contract_row(100, 10U, 1U), contract_row(200, 10U, 1U)};
  rows.back().exercise_style = ExerciseStyle::European;
  const std::array scenarios{OptionRiskScenario{1U, identity(701U)}};
  const std::array pnl_rows{
      scenario_row(100, 10U, 1U, -10.0),
      scenario_row(200, 10U, 1U, -11.0),
  };

  const auto result =
      OptionRiskPanel::create(rows, scenarios, pnl_rows, provenance(), OptionRiskPanelLimits{});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionRiskPanel, ScenarioManifestIsBoundedBeforeCanonicalization) {
  const std::array rows{contract_row(100, 10U, 1U)};
  const std::array scenarios{
      OptionRiskScenario{1U, identity(701U)},
      OptionRiskScenario{2U, identity(702U)},
  };
  const std::array pnl_rows{
      scenario_row(100, 10U, 1U, -10.0),
      scenario_row(100, 10U, 2U, 10.0),
  };
  OptionRiskPanelLimits limits;
  limits.max_scenarios = 1U;

  const auto result = OptionRiskPanel::create(rows, scenarios, pnl_rows, provenance(), limits);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionRiskPanel, RetainedLowerBoundIsCheckedBeforeCanonicalization) {
  const std::array rows{contract_row(100, 10U, 1U)};
  const std::array scenarios{OptionRiskScenario{1U, identity(701U)}};
  const std::array pnl_rows{scenario_row(100, 10U, 1U, -10.0)};
  OptionRiskPanelLimits limits;
  limits.max_workspace_bytes =
      sizeof(OptionRiskContractRow) + sizeof(std::uint64_t) + sizeof(double) - 1U;

  const auto result = OptionRiskPanel::create(rows, scenarios, pnl_rows, provenance(), limits);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(OptionPreTradeRisk, OpposingLeavesCanBeProjectedSafeButWorstSubsetUnsafe) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 2> contracts{10U, 20U};
  const std::array<std::uint32_t, 2> underliers{1U, 1U};
  const std::array<std::uint64_t, 1> scenarios{7U};
  const std::array<double, 2> pnl{-10.0, 10.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U), instrument(20U, 1U)};
  const std::array<std::int64_t, 2> filled{0, 0};
  const std::array<OptionRiskLeaf, 0> live{};
  const std::array candidates{OptionRiskLeaf{0U, 1}, OptionRiskLeaf{1U, 1}};
  OptionRiskHardLimits limits;
  limits.max_scenario_loss = 5.0;
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine) << engine.error().to_string();

  const auto result = engine->evaluate(panel, 0U, catalog, filled, live, candidates, limits);

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_DOUBLE_EQ(result->candidate_projected.scenario_loss, 0.0);
  EXPECT_DOUBLE_EQ(result->candidate_worst_fill.scenario_loss, 10.0);
  EXPECT_EQ(result->candidate_worst_fill.worst_scenario_id, 7U);
  EXPECT_EQ(result->disposition, OptionRiskDisposition::RejectNewOrders);
  EXPECT_NE(result->candidate_breach_mask & bit(OptionRiskBreach::ScenarioLoss), 0U);
}

TEST(OptionPreTradeRisk, LiveLeafRemainsInBaselineAndCandidateEnvelope) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 1> contracts{10U};
  const std::array<std::uint32_t, 1> underliers{1U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 1> pnl{-10.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U)};
  const std::array<std::int64_t, 1> filled{4};
  const std::array live{OptionRiskLeaf{0U, 6}};
  const std::array<OptionRiskLeaf, 0> candidates{};
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine) << engine.error().to_string();

  const auto result =
      engine->evaluate(panel, 0U, catalog, filled, live, candidates, OptionRiskHardLimits{});

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_DOUBLE_EQ(result->filled.scenario_loss, 40.0);
  EXPECT_DOUBLE_EQ(result->baseline_projected.scenario_loss, 100.0);
  EXPECT_DOUBLE_EQ(result->baseline_worst_fill.scenario_loss, 100.0);
  EXPECT_EQ(result->baseline_worst_fill.open_order_contracts, 6U);
}

TEST(OptionPreTradeRisk, ExistingBreachAllowsOnlyNonWorseningProjectedReduction) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 1> contracts{10U};
  const std::array<std::uint32_t, 1> underliers{1U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 1> pnl{-10.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U)};
  const std::array<std::int64_t, 1> filled{10};
  const std::array<OptionRiskLeaf, 0> live{};
  const std::array reducing{OptionRiskLeaf{0U, -5}};
  const std::array increasing{OptionRiskLeaf{0U, 5}};
  OptionRiskHardLimits limits;
  limits.max_scenario_loss = 80.0;
  auto first = OptionPreTradeRiskEngine::create(engine_limits());
  auto second = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  const auto reduced = first->evaluate(panel, 0U, catalog, filled, live, reducing, limits);
  const auto worsened = second->evaluate(panel, 0U, catalog, filled, live, increasing, limits);

  ASSERT_TRUE(reduced) << reduced.error().to_string();
  ASSERT_TRUE(worsened) << worsened.error().to_string();
  EXPECT_DOUBLE_EQ(reduced->baseline_worst_fill.scenario_loss, 100.0);
  EXPECT_DOUBLE_EQ(reduced->candidate_worst_fill.scenario_loss, 100.0);
  EXPECT_DOUBLE_EQ(reduced->candidate_projected.scenario_loss, 50.0);
  EXPECT_EQ(reduced->disposition, OptionRiskDisposition::ReduceOnlyAccept);
  EXPECT_EQ(worsened->disposition, OptionRiskDisposition::CancelOnly);
}

TEST(OptionPreTradeRisk, LimitBoundaryIsInclusiveAndOneUlpBeyondFails) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 1> contracts{10U};
  const std::array<std::uint32_t, 1> underliers{1U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 1> pnl{-10.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U)};
  const std::array<std::int64_t, 1> filled{1};
  const std::array<OptionRiskLeaf, 0> leaves{};
  OptionRiskHardLimits exact;
  exact.max_scenario_loss = 10.0;
  OptionRiskHardLimits below = exact;
  below.max_scenario_loss = std::nextafter(10.0, 0.0);
  auto first = OptionPreTradeRiskEngine::create(engine_limits());
  auto second = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  const auto passing = first->evaluate(panel, 0U, catalog, filled, leaves, leaves, exact);
  const auto failing = second->evaluate(panel, 0U, catalog, filled, leaves, leaves, below);

  ASSERT_TRUE(passing);
  ASSERT_TRUE(failing);
  EXPECT_EQ(passing->disposition, OptionRiskDisposition::Accept);
  EXPECT_EQ(failing->disposition, OptionRiskDisposition::CancelOnly);
}

TEST(OptionPreTradeRisk, ZeroCrossUnderlierCreditSumsRootLosses) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 2> contracts{10U, 20U};
  const std::array<std::uint32_t, 2> underliers{1U, 2U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 2> pnl{-10.0, 10.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U), instrument(20U, 2U)};
  const std::array<std::int64_t, 2> filled{1, 1};
  const std::array<OptionRiskLeaf, 0> leaves{};
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine);

  const auto result =
      engine->evaluate(panel, 0U, catalog, filled, leaves, leaves, OptionRiskHardLimits{});

  ASSERT_TRUE(result);
  EXPECT_DOUBLE_EQ(result->filled.scenario_loss, 10.0);
  EXPECT_DOUBLE_EQ(result->filled.max_single_underlier_scenario_loss, 10.0);
  EXPECT_EQ(result->filled.worst_underlier_uid, 1U);
}

TEST(OptionPreTradeRisk, SingleUnderlierLossIsMaximizedIndependentlyOfAccountScenario) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 2> contracts{10U, 20U};
  const std::array<std::uint32_t, 2> underliers{1U, 2U};
  const std::array<std::uint64_t, 2> scenarios{1U, 2U};
  const std::array<double, 4> pnl{-10.0, -10.0, -15.0, 100.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U), instrument(20U, 2U)};
  const std::array<std::int64_t, 2> filled{1, 1};
  const std::array<OptionRiskLeaf, 0> leaves{};
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine);

  const auto result =
      engine->evaluate(panel, 0U, catalog, filled, leaves, leaves, OptionRiskHardLimits{});

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_DOUBLE_EQ(result->filled.scenario_loss, 20.0);
  EXPECT_EQ(result->filled.worst_scenario_id, 1U);
  EXPECT_DOUBLE_EQ(result->filled.max_single_underlier_scenario_loss, 15.0);
  EXPECT_EQ(result->filled.worst_underlier_scenario_id, 2U);
  EXPECT_EQ(result->filled.worst_underlier_uid, 1U);
  EXPECT_DOUBLE_EQ(result->baseline_worst_fill.max_single_underlier_scenario_loss, 15.0);
  EXPECT_EQ(result->baseline_worst_fill.worst_underlier_scenario_id, 2U);
}

TEST(OptionPreTradeRisk, InputHashCoversHardLimits) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 1> contracts{10U};
  const std::array<std::uint32_t, 1> underliers{1U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 1> pnl{-10.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U)};
  const std::array<std::int64_t, 1> filled{1};
  const std::array<OptionRiskLeaf, 0> leaves{};
  OptionRiskHardLimits first_limits;
  first_limits.max_scenario_loss = 100.0;
  OptionRiskHardLimits second_limits = first_limits;
  second_limits.max_scenario_loss = 101.0;
  auto first = OptionPreTradeRiskEngine::create(engine_limits());
  auto second = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  const auto first_result =
      first->evaluate(panel, 0U, catalog, filled, leaves, leaves, first_limits);
  const auto second_result =
      second->evaluate(panel, 0U, catalog, filled, leaves, leaves, second_limits);

  ASSERT_TRUE(first_result);
  ASSERT_TRUE(second_result);
  EXPECT_NE(first_result->input_hash, second_result->input_hash);
}

TEST(OptionPreTradeRisk, InputHashIsInvariantToLeafPermutation) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 2> contracts{10U, 20U};
  const std::array<std::uint32_t, 2> underliers{1U, 2U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 2> pnl{-10.0, 8.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U), instrument(20U, 2U)};
  const std::array<std::int64_t, 2> filled{2, -1};
  const std::array first_live{OptionRiskLeaf{0U, 3}, OptionRiskLeaf{1U, -2}};
  const std::array second_live{OptionRiskLeaf{1U, -2}, OptionRiskLeaf{0U, 3}};
  const std::array first_candidates{OptionRiskLeaf{0U, -1}, OptionRiskLeaf{1U, 1}};
  const std::array second_candidates{OptionRiskLeaf{1U, 1}, OptionRiskLeaf{0U, -1}};
  auto first = OptionPreTradeRiskEngine::create(engine_limits());
  auto second = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);

  const auto first_result = first->evaluate(panel, 0U, catalog, filled, first_live,
                                            first_candidates, OptionRiskHardLimits{});
  const auto second_result = second->evaluate(panel, 0U, catalog, filled, second_live,
                                              second_candidates, OptionRiskHardLimits{});

  ASSERT_TRUE(first_result);
  ASSERT_TRUE(second_result);
  EXPECT_EQ(first_result->input_hash, second_result->input_hash);
  EXPECT_EQ(first_result->candidate_worst_fill, second_result->candidate_worst_fill);
  EXPECT_EQ(first_result->disposition, second_result->disposition);
}

TEST(OptionPreTradeRisk, ContractDefinitionMismatchFailsClosed) {
  const std::array rows{contract_row(100, 10U, 1U)};
  const std::array scenarios{OptionRiskScenario{1U, identity(701U)}};
  const std::array pnl_rows{scenario_row(100, 10U, 1U, -10.0)};
  auto mismatched_rows = rows;
  mismatched_rows[0].multiplier = 50.0;
  const auto panel = OptionRiskPanel::create(mismatched_rows, scenarios, pnl_rows, provenance());
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array catalog{instrument(10U, 1U)};
  const std::array<std::int64_t, 1> filled{1};
  const std::array<OptionRiskLeaf, 0> leaves{};
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine);

  const auto result =
      engine->evaluate(*panel, 0U, catalog, filled, leaves, leaves, OptionRiskHardLimits{});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionPreTradeRisk, NonstandardDeliverableCannotBeTradableRiskEvidence) {
  auto row = contract_row(100, 10U, 1U);
  row.standard_deliverable = false;
  const std::array scenarios{OptionRiskScenario{1U, identity(701U)}};
  const std::array pnl_rows{scenario_row(100, 10U, 1U, -10.0)};

  const std::array invalid_rows{row};
  const auto invalid = OptionRiskPanel::create(invalid_rows, scenarios, pnl_rows, provenance());
  EXPECT_FALSE(invalid);

  row.status = OptionRiskRowStatus::UnsupportedContract;
  const std::array unsupported_rows{row};
  const auto panel = OptionRiskPanel::create(unsupported_rows, scenarios, pnl_rows, provenance());
  ASSERT_TRUE(panel) << panel.error().to_string();
  auto nonstandard_catalog = std::array{instrument(10U, 1U)};
  nonstandard_catalog[0].standard_deliverable = false;
  const std::array<std::int64_t, 1> filled{1};
  const std::array<OptionRiskLeaf, 0> leaves{};
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine);

  const auto result = engine->evaluate(*panel, 0U, nonstandard_catalog, filled, leaves, leaves,
                                       OptionRiskHardLimits{});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionPreTradeRisk, InexactIntegerQuantityFailsClosed) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 1> contracts{10U};
  const std::array<std::uint32_t, 1> underliers{1U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 1> pnl{-1.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U)};
  constexpr std::int64_t kFirstInexactInteger = (std::int64_t{1} << 53U) + 1;
  const std::array<std::int64_t, 1> filled{kFirstInexactInteger};
  const std::array<OptionRiskLeaf, 0> leaves{};
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine);

  const auto result =
      engine->evaluate(panel, 0U, catalog, filled, leaves, leaves, OptionRiskHardLimits{});

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(OptionPreTradeRisk, OpenContractNonWorseningUsesExactUint64Comparison) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 1> contracts{10U};
  const std::array<std::uint32_t, 1> underliers{1U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 1> pnl{-1.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U)};
  const std::array<std::int64_t, 1> filled{10};
  constexpr std::int64_t kExactLargeLeaf = std::int64_t{1} << 52U;
  const std::array live{
      OptionRiskLeaf{0U, kExactLargeLeaf},
      OptionRiskLeaf{0U, -kExactLargeLeaf},
  };
  const std::array candidates{OptionRiskLeaf{0U, -1}};
  OptionRiskHardLimits limits;
  limits.max_open_order_contracts = (std::uint64_t{1} << 53U) - 1U;
  limits.max_scenario_loss = 5.0;
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine);

  const auto result = engine->evaluate(panel, 0U, catalog, filled, live, candidates, limits);

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_EQ(result->baseline_worst_fill.open_order_contracts, std::uint64_t{1} << 53U);
  EXPECT_EQ(result->candidate_worst_fill.open_order_contracts, (std::uint64_t{1} << 53U) + 1U);
  EXPECT_LT(result->candidate_projected.scenario_loss, result->baseline_projected.scenario_loss);
  EXPECT_EQ(result->disposition, OptionRiskDisposition::CancelOnly);
}

TEST(OptionPreTradeRisk, WorstFillMetricsMatchExhaustiveSubsetOracle) {
  constexpr std::int64_t kDate = 100;
  const std::array<std::uint64_t, 4> contract_ids{10U, 20U, 30U, 40U};
  const std::array<std::uint32_t, 4> underlier_uids{1U, 1U, 2U, 3U};
  const std::array<std::uint64_t, 3> scenario_ids{3U, 7U, 11U};
  const std::array catalog{
      instrument(10U, 1U),
      instrument(20U, 1U),
      instrument(30U, 2U),
      instrument(40U, 3U),
  };
  std::mt19937_64 random{0xA7C0'5EEDULL};
  std::uniform_int_distribution<int> coefficient{-5, 5};
  std::uniform_int_distribution<int> premium{50, 150};
  std::uniform_int_distribution<int> scenario_pnl{-12, 12};
  std::uniform_int_distribution<int> position{-2, 2};
  std::uniform_int_distribution<int> contract_index{0, 3};
  std::uniform_int_distribution<int> leaf_quantity{-3, 3};

  for (std::size_t trial = 0; trial < 16U; ++trial) {
    std::vector<OptionRiskContractRow> rows;
    rows.reserve(contract_ids.size());
    for (std::size_t index = 0; index < contract_ids.size(); ++index) {
      OptionRiskContractRow row = contract_row(kDate, contract_ids[index], underlier_uids[index]);
      row.spot_delta_cash_per_contract = static_cast<double>(coefficient(random));
      row.spot_gamma_cash_per_contract = static_cast<double>(coefficient(random));
      row.vega_cash_per_vol_point_per_contract = static_cast<double>(coefficient(random));
      row.theta_cash_per_day_per_contract = static_cast<double>(coefficient(random));
      row.vanna_cash_per_return_vol_point_per_contract =
          static_cast<double>(coefficient(random)) * 0.5;
      row.volga_cash_per_vol_point_squared_per_contract =
          static_cast<double>(coefficient(random)) * 0.25;
      row.premium_cash_notional_per_contract = static_cast<double>(premium(random));
      rows.push_back(row);
    }
    std::vector<OptionRiskScenario> scenarios;
    std::vector<OptionRiskScenarioPnlRow> pnl_rows;
    scenarios.reserve(scenario_ids.size());
    pnl_rows.reserve(scenario_ids.size() * contract_ids.size());
    for (std::uint64_t scenario_id : scenario_ids) {
      scenarios.push_back(OptionRiskScenario{scenario_id, identity(scenario_id + 700U)});
      for (std::uint64_t contract_id : contract_ids) {
        pnl_rows.push_back(scenario_row(kDate, contract_id, scenario_id,
                                        static_cast<double>(scenario_pnl(random))));
      }
    }
    const auto panel = OptionRiskPanel::create(rows, scenarios, pnl_rows, provenance());
    ASSERT_TRUE(panel) << panel.error().to_string();

    std::array<std::int64_t, 4> filled{};
    for (std::int64_t &quantity : filled) {
      quantity = position(random);
    }
    std::array<OptionRiskLeaf, 6> leaves{};
    for (OptionRiskLeaf &leaf : leaves) {
      leaf.contract_index = static_cast<std::size_t>(contract_index(random));
      do {
        leaf.remaining_contracts = leaf_quantity(random);
      } while (leaf.remaining_contracts == 0);
    }
    const std::span<const OptionRiskLeaf> live{leaves.data(), 2U};
    const std::span<const OptionRiskLeaf> candidates{leaves.data() + 2U, 4U};
    auto engine = OptionPreTradeRiskEngine::create(engine_limits());
    ASSERT_TRUE(engine);
    const auto evaluated =
        engine->evaluate(*panel, 0U, catalog, filled, live, candidates, OptionRiskHardLimits{});
    ASSERT_TRUE(evaluated) << evaluated.error().to_string();

    atx::options::risk::OptionRiskWorstFillMetrics expected;
    expected.worst_scenario_id = scenario_ids.front();
    expected.worst_underlier_scenario_id = scenario_ids.front();
    expected.worst_underlier_uid = 1U;
    for (const OptionRiskLeaf &leaf : leaves) {
      expected.open_order_contracts +=
          static_cast<std::uint64_t>(std::abs(leaf.remaining_contracts));
    }
    const std::uint64_t subsets = std::uint64_t{1} << leaves.size();
    for (std::uint64_t subset = 0U; subset < subsets; ++subset) {
      std::array<std::int64_t, 4> quantities = filled;
      for (std::size_t leaf_index = 0; leaf_index < leaves.size(); ++leaf_index) {
        if ((subset & (std::uint64_t{1} << leaf_index)) != 0U) {
          quantities[leaves[leaf_index].contract_index] += leaves[leaf_index].remaining_contracts;
        }
      }
      double delta = 0.0;
      double gamma = 0.0;
      double vega = 0.0;
      double theta = 0.0;
      double vanna = 0.0;
      double volga = 0.0;
      double gross_gamma = 0.0;
      double gross_vega = 0.0;
      double gross_vanna = 0.0;
      double gross_volga = 0.0;
      double gross_premium = 0.0;
      for (std::size_t contract = 0; contract < quantities.size(); ++contract) {
        const double quantity = static_cast<double>(quantities[contract]);
        const OptionRiskContractRow &row = panel->contract_row(0U, contract);
        delta += quantity * row.spot_delta_cash_per_contract;
        gamma += quantity * row.spot_gamma_cash_per_contract;
        vega += quantity * row.vega_cash_per_vol_point_per_contract;
        theta += quantity * row.theta_cash_per_day_per_contract;
        vanna += quantity * row.vanna_cash_per_return_vol_point_per_contract;
        volga += quantity * row.volga_cash_per_vol_point_squared_per_contract;
        gross_gamma += std::abs(quantity * row.spot_gamma_cash_per_contract);
        gross_vega += std::abs(quantity * row.vega_cash_per_vol_point_per_contract);
        gross_vanna += std::abs(quantity * row.vanna_cash_per_return_vol_point_per_contract);
        gross_volga += std::abs(quantity * row.volga_cash_per_vol_point_squared_per_contract);
        gross_premium += std::abs(quantity) * row.premium_cash_notional_per_contract;
      }
      expected.max_abs_spot_delta_cash =
          (std::max)(expected.max_abs_spot_delta_cash, std::abs(delta));
      expected.max_abs_spot_gamma_cash =
          (std::max)(expected.max_abs_spot_gamma_cash, std::abs(gamma));
      expected.max_abs_vega_cash_per_vol_point =
          (std::max)(expected.max_abs_vega_cash_per_vol_point, std::abs(vega));
      expected.max_abs_theta_cash_per_day =
          (std::max)(expected.max_abs_theta_cash_per_day, std::abs(theta));
      expected.max_abs_vanna_cash_per_return_vol_point =
          (std::max)(expected.max_abs_vanna_cash_per_return_vol_point, std::abs(vanna));
      expected.max_abs_volga_cash_per_vol_point_squared =
          (std::max)(expected.max_abs_volga_cash_per_vol_point_squared, std::abs(volga));
      expected.max_gross_spot_gamma_cash =
          (std::max)(expected.max_gross_spot_gamma_cash, gross_gamma);
      expected.max_gross_vega_cash_per_vol_point =
          (std::max)(expected.max_gross_vega_cash_per_vol_point, gross_vega);
      expected.max_gross_vanna_cash_per_return_vol_point =
          (std::max)(expected.max_gross_vanna_cash_per_return_vol_point, gross_vanna);
      expected.max_gross_volga_cash_per_vol_point_squared =
          (std::max)(expected.max_gross_volga_cash_per_vol_point_squared, gross_volga);
      expected.max_gross_premium_cash_notional =
          (std::max)(expected.max_gross_premium_cash_notional, gross_premium);

      for (std::size_t scenario = 0; scenario < scenario_ids.size(); ++scenario) {
        std::array<double, 3> root_pnl{};
        for (std::size_t contract = 0; contract < quantities.size(); ++contract) {
          const std::size_t root = underlier_uids[contract] == 1U
                                       ? 0U
                                       : static_cast<std::size_t>(underlier_uids[contract] - 1U);
          root_pnl[root] += static_cast<double>(quantities[contract]) *
                            panel->scenario_pnl(0U, scenario, contract);
        }
        double loss = 0.0;
        for (std::size_t root = 0; root < root_pnl.size(); ++root) {
          const double root_loss = (std::max)(0.0, -root_pnl[root]);
          loss += root_loss;
          const std::uint32_t root_uid = static_cast<std::uint32_t>(root + 1U);
          if (root_loss > expected.max_single_underlier_scenario_loss ||
              (root_loss == expected.max_single_underlier_scenario_loss &&
               (scenario_ids[scenario] < expected.worst_underlier_scenario_id ||
                (scenario_ids[scenario] == expected.worst_underlier_scenario_id &&
                 root_uid < expected.worst_underlier_uid)))) {
            expected.max_single_underlier_scenario_loss = root_loss;
            expected.worst_underlier_scenario_id = scenario_ids[scenario];
            expected.worst_underlier_uid = root_uid;
          }
        }
        if (loss > expected.scenario_loss ||
            (loss == expected.scenario_loss &&
             scenario_ids[scenario] < expected.worst_scenario_id)) {
          expected.scenario_loss = loss;
          expected.worst_scenario_id = scenario_ids[scenario];
        }
      }
    }
    EXPECT_EQ(evaluated->candidate_worst_fill, expected) << "trial " << trial;
  }
}

TEST(OptionPreTradeRisk, WorkspaceBoundaryIsExact) {
  OptionRiskEngineLimits limits = engine_limits();
  const auto required = option_pretrade_risk_required_workspace_bytes(limits);
  ASSERT_TRUE(required) << required.error().to_string();
  limits.max_workspace_bytes = *required;
  EXPECT_TRUE(OptionPreTradeRiskEngine::create(limits));
  limits.max_workspace_bytes = *required - 1U;
  EXPECT_FALSE(OptionPreTradeRiskEngine::create(limits));
}

TEST(OptionPreTradeRisk, SuccessfulEvaluationAllocatesOnlyAtCreate) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 2> contracts{10U, 20U};
  const std::array<std::uint32_t, 2> underliers{1U, 1U};
  const std::array<std::uint64_t, 2> scenarios{1U, 2U};
  const std::array<double, 4> pnl{-10.0, 8.0, 7.0, -9.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U), instrument(20U, 1U)};
  const std::array<std::int64_t, 2> filled{2, -1};
  const std::array live{OptionRiskLeaf{0U, 3}, OptionRiskLeaf{1U, -2}};
  const std::array candidates{OptionRiskLeaf{0U, -1}, OptionRiskLeaf{1U, 1}};
  auto engine = OptionPreTradeRiskEngine::create(engine_limits());
  ASSERT_TRUE(engine);

  option_replay_alloc::g_count.store(0U, std::memory_order_relaxed);
  option_replay_alloc::g_armed.store(true, std::memory_order_relaxed);
  const auto result =
      engine->evaluate(panel, 0U, catalog, filled, live, candidates, OptionRiskHardLimits{});
  option_replay_alloc::g_armed.store(false, std::memory_order_relaxed);
  const std::size_t allocations = option_replay_alloc::g_count.load(std::memory_order_relaxed);

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_EQ(allocations, 0U);
}

TEST(OptionPreTradeRisk, RepeatedUnderliersDoNotAllocateBeyondConfiguredRootCapacity) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 2> contracts{10U, 20U};
  const std::array<std::uint32_t, 2> underliers{1U, 1U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 2> pnl{-10.0, 8.0};
  const OptionRiskPanel panel = make_panel(dates, contracts, underliers, scenarios, pnl);
  const std::array catalog{instrument(10U, 1U), instrument(20U, 1U)};
  const std::array<std::int64_t, 2> filled{2, -1};
  const std::array<OptionRiskLeaf, 0> leaves{};
  OptionRiskEngineLimits limits = engine_limits();
  limits.max_underliers = 1U;
  auto engine = OptionPreTradeRiskEngine::create(limits);
  ASSERT_TRUE(engine);

  option_replay_alloc::g_count.store(0U, std::memory_order_relaxed);
  option_replay_alloc::g_armed.store(true, std::memory_order_relaxed);
  const auto result =
      engine->evaluate(panel, 0U, catalog, filled, leaves, leaves, OptionRiskHardLimits{});
  option_replay_alloc::g_armed.store(false, std::memory_order_relaxed);
  const std::size_t allocations = option_replay_alloc::g_count.load(std::memory_order_relaxed);

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_EQ(allocations, 0U);
}

TEST(OptionPreTradeRisk, RootCacheRecoversAfterOverCapacityPanel) {
  const std::array<std::int64_t, 1> dates{100};
  const std::array<std::uint64_t, 2> contracts{10U, 20U};
  const std::array<std::uint32_t, 2> valid_underliers{1U, 1U};
  const std::array<std::uint32_t, 2> excess_underliers{1U, 2U};
  const std::array<std::uint64_t, 1> scenarios{1U};
  const std::array<double, 2> pnl{-10.0, 8.0};
  const OptionRiskPanel valid_panel =
      make_panel(dates, contracts, valid_underliers, scenarios, pnl);
  const OptionRiskPanel excess_panel =
      make_panel(dates, contracts, excess_underliers, scenarios, pnl);
  const std::array valid_catalog{instrument(10U, 1U), instrument(20U, 1U)};
  const std::array excess_catalog{instrument(10U, 1U), instrument(20U, 2U)};
  const std::array<std::int64_t, 2> filled{2, -1};
  const std::array<OptionRiskLeaf, 0> leaves{};
  OptionRiskEngineLimits limits = engine_limits();
  limits.max_underliers = 1U;
  auto engine = OptionPreTradeRiskEngine::create(limits);
  ASSERT_TRUE(engine);

  const auto first = engine->evaluate(valid_panel, 0U, valid_catalog, filled, leaves, leaves,
                                      OptionRiskHardLimits{});
  const auto rejected = engine->evaluate(excess_panel, 0U, excess_catalog, filled, leaves, leaves,
                                         OptionRiskHardLimits{});
  option_replay_alloc::g_count.store(0U, std::memory_order_relaxed);
  option_replay_alloc::g_armed.store(true, std::memory_order_relaxed);
  const auto recovered = engine->evaluate(valid_panel, 0U, valid_catalog, filled, leaves, leaves,
                                          OptionRiskHardLimits{});
  option_replay_alloc::g_armed.store(false, std::memory_order_relaxed);
  const std::size_t allocations = option_replay_alloc::g_count.load(std::memory_order_relaxed);

  ASSERT_TRUE(first) << first.error().to_string();
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::OutOfRange);
  ASSERT_TRUE(recovered) << recovered.error().to_string();
  EXPECT_EQ(*recovered, *first);
  EXPECT_EQ(allocations, 0U);
}

TEST(OptionRiskPanel, GeneratedCanonicalMatchesExpandedPanelAcrossDates) {
  constexpr std::array<std::int64_t, 2> dates{100, 200};
  constexpr std::array<std::uint64_t, 2> contract_ids{10U, 20U};
  constexpr std::array<std::uint32_t, 2> underlier_uids{1U, 2U};
  constexpr std::array<std::uint64_t, 2> scenario_ids{7U, 9U};
  constexpr std::array<double, 8> pnl{-11.0, 12.0, 13.0, -14.0, 21.0, -22.0, -23.0, 24.0};

  std::vector<OptionRiskContractRow> contract_rows;
  contract_rows.reserve(dates.size() * contract_ids.size());
  for (const std::int64_t date : dates) {
    for (std::size_t contract = 0U; contract < contract_ids.size(); ++contract) {
      contract_rows.push_back(contract_row(date, contract_ids[contract], underlier_uids[contract]));
    }
  }

  std::vector<OptionRiskScenario> scenarios;
  scenarios.reserve(scenario_ids.size());
  for (const std::uint64_t scenario_id : scenario_ids) {
    scenarios.push_back(OptionRiskScenario{scenario_id, identity(scenario_id + 700U)});
  }

  std::vector<OptionRiskScenarioPnlRow> expanded_pnl;
  std::vector<atx::options::risk::OptionRiskGeneratedPnlLineage> generated_lineage;
  expanded_pnl.reserve(pnl.size());
  generated_lineage.reserve(dates.size() * scenario_ids.size());
  std::size_t cell = 0U;
  for (const std::int64_t date : dates) {
    for (const std::uint64_t scenario_id : scenario_ids) {
      const std::int64_t observed_ts_ns = date - 30;
      const std::int64_t available_ts_ns = date - 15;
      const ArchiveContentIdentity source_identity =
          identity(static_cast<std::uint64_t>(date) + scenario_id + 5'000U);
      generated_lineage.push_back(
          {date, scenario_id, observed_ts_ns, available_ts_ns, source_identity});
      for (const std::uint64_t contract_id : contract_ids) {
        expanded_pnl.push_back({date, contract_id, scenario_id, observed_ts_ns, available_ts_ns,
                                pnl[cell++], source_identity});
      }
    }
  }

  const auto expanded =
      OptionRiskPanel::create(contract_rows, scenarios, expanded_pnl, provenance());
  const auto generated = OptionRiskPanel::create_generated_canonical(
      contract_rows, scenarios, std::vector<double>{pnl.begin(), pnl.end()},
      std::move(generated_lineage), provenance());
  ASSERT_TRUE(expanded) << expanded.error().to_string();
  ASSERT_TRUE(generated) << generated.error().to_string();
  EXPECT_EQ(generated->definition_hash(), expanded->definition_hash());
  EXPECT_EQ(generated->provenance(), expanded->provenance());
  ASSERT_EQ(generated->dates().size(), expanded->dates().size());
  ASSERT_EQ(generated->contract_count(), expanded->contract_count());
  ASSERT_EQ(generated->scenario_count(), expanded->scenario_count());
  for (std::size_t date = 0U; date < expanded->dates().size(); ++date) {
    EXPECT_EQ(generated->dates()[date], expanded->dates()[date]);
    for (std::size_t contract = 0U; contract < expanded->contract_count(); ++contract) {
      EXPECT_EQ(generated->contract_row(date, contract), expanded->contract_row(date, contract));
    }
    for (std::size_t scenario = 0U; scenario < expanded->scenario_count(); ++scenario) {
      EXPECT_EQ(generated->scenario_ids()[scenario], expanded->scenario_ids()[scenario]);
      for (std::size_t contract = 0U; contract < expanded->contract_count(); ++contract) {
        EXPECT_DOUBLE_EQ(generated->scenario_pnl(date, scenario, contract),
                         expanded->scenario_pnl(date, scenario, contract));
      }
    }
  }
}

TEST(OptionRiskPanel, GeneratedCanonicalRejectsDuplicateContractRows) {
  const OptionRiskContractRow row = contract_row(100, 10U, 1U);
  std::vector<OptionRiskContractRow> rows{row, row};
  std::vector<OptionRiskScenario> scenarios{OptionRiskScenario{7U, identity(707U)}};
  std::vector<double> pnl{-1.0, -1.0};
  std::vector<atx::options::risk::OptionRiskGeneratedPnlLineage> lineage{
      {100, 7U, 70, 85, identity(5'107U)}};

  const auto result = OptionRiskPanel::create_generated_canonical(
      std::move(rows), std::move(scenarios), std::move(pnl), std::move(lineage), provenance());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

TEST(OptionRiskPanel, GeneratedCanonicalRejectsDuplicateScenarios) {
  std::vector<OptionRiskContractRow> rows{contract_row(100, 10U, 1U)};
  const OptionRiskScenario scenario{7U, identity(707U)};
  std::vector<OptionRiskScenario> scenarios{scenario, scenario};
  std::vector<double> pnl{-1.0, -1.0};
  std::vector<atx::options::risk::OptionRiskGeneratedPnlLineage> lineage{
      {100, 7U, 70, 85, identity(5'107U)}, {100, 7U, 70, 85, identity(5'107U)}};

  const auto result = OptionRiskPanel::create_generated_canonical(
      std::move(rows), std::move(scenarios), std::move(pnl), std::move(lineage), provenance());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
}

} // namespace
