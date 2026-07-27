#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "atx/core/decimal.hpp"
#include "atx/options/option_adaptive_coordinator.hpp"

namespace option_replay_alloc {
extern std::atomic<std::size_t> g_count;
extern std::atomic<bool> g_armed;
} // namespace option_replay_alloc

namespace {

using atx::core::Decimal;
using atx::options::adaptive::option_adaptive_coordinator_required_workspace_bytes;
using atx::options::adaptive::OptionAdaptiveCoordinator;
using atx::options::adaptive::OptionAdaptiveCoordinatorConfig;
using atx::options::adaptive::OptionAdaptiveCoordinatorLimits;
using atx::options::adaptive::OptionMissingSignalPolicy;
using atx::options::adaptive::OptionReconciliationScope;
using atx::options::execution::OptionFeeSchedule;
using atx::options::execution::OptionMarketOrderKey;
using atx::options::execution::OptionReplayConfig;
using atx::options::execution::OptionReplayContract;
using atx::options::execution::OptionReplayInputs;
using atx::options::execution::OptionReplayScenario;
using atx::options::execution::OptionTickSchedule;
using atx::options::execution::OptionTimeInForce;
using atx::options::execution::OptionTopOfBookEvent;
using atx::options::research::OptionPanelRow;
using atx::options::research::OptionPanelStatus;
using atx::options::research::OptionResearchPanel;
using atx::options::research::OptionSizingBasis;
using atx::options::risk::OptionRiskContentDigest;
using atx::options::risk::OptionRiskContractRow;
using atx::options::risk::OptionRiskDisposition;
using atx::options::risk::OptionRiskPanel;
using atx::options::risk::OptionRiskPanelProvenance;
using atx::options::risk::OptionRiskScenario;
using atx::options::risk::OptionRiskScenarioPnlRow;
using atx::vol::ArchiveContentIdentity;
using atx::vol::ExerciseStyle;

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) noexcept {
  return ArchiveContentIdentity{10'000U + seed, 20'000U + seed,
                                static_cast<std::uint32_t>(30'000U + seed),
                                static_cast<std::uint32_t>(40'000U + seed)};
}

[[nodiscard]] Decimal money(const char *text) {
  const auto value = Decimal::from_string(text);
  EXPECT_TRUE(value) << value.error().to_string();
  return value.value_or(Decimal{});
}

[[nodiscard]] OptionPanelRow panel_row(std::uint64_t contract_id, std::int64_t decision_ts_ns,
                                       double signal) {
  OptionPanelRow out;
  out.observation.uid = 1U;
  out.observation.observed_ts_ns = decision_ts_ns - 20;
  out.observation.available_ts_ns = decision_ts_ns - 10;
  out.observation.decision_ts_ns = decision_ts_ns;
  out.observation.execution_ts_ns = decision_ts_ns + 1;
  out.observation.label_end_ts_ns = decision_ts_ns + 100;
  out.observation.signal = signal;
  out.observation.forward_pnl = 1.0;
  out.observation.lagged_capital = 1'000.0;
  out.observation.source_identity = identity(contract_id + 1U);
  out.contract_id = contract_id;
  out.engine_id.id = static_cast<std::uint32_t>(contract_id);
  out.definition_available_ts_ns = decision_ts_ns - 30;
  out.quote_event_ts_ns = decision_ts_ns - 1;
  out.quote_available_ts_ns = decision_ts_ns;
  out.expiry_ts_ns = 10'000;
  out.strike = 100.0 + static_cast<double>(contract_id);
  out.multiplier = 100.0;
  out.mark = 10.0;
  out.bid = 9.0;
  out.ask = 11.0;
  out.bid_size_contracts = 50.0;
  out.ask_size_contracts = 50.0;
  out.interval_volume_contracts = 100.0;
  out.lagged_open_interest_contracts = 1'000.0;
  out.adv_contracts = 10'000.0;
  out.return_sigma = 0.2;
  out.vega_per_contract = 10.0;
  out.initial_margin_per_contract = 100.0;
  out.maintenance_margin_per_contract = 80.0;
  out.definition_source_identity = identity(contract_id + 500U);
  out.feature_source_identity = identity(contract_id + 3U);
  out.execution_source_identity = identity(900U);
  return out;
}

[[nodiscard]] OptionReplayContract contract(std::uint64_t contract_id) {
  OptionReplayContract out;
  out.contract_id = contract_id;
  out.engine_id.id = static_cast<std::uint32_t>(contract_id);
  out.multiplier = 100;
  out.tick_schedule_key = 1U;
  out.definition_effective_ts_ns = 1;
  out.definition_available_ts_ns = 2;
  out.expiry_ts_ns = 10'000;
  out.definition_source_identity = identity(contract_id + 500U);
  return out;
}

[[nodiscard]] OptionTopOfBookEvent quote(std::uint64_t contract_id, std::int64_t available_ts_ns,
                                         std::uint64_t sequence, std::int64_t size_contracts) {
  OptionTopOfBookEvent out;
  out.contract_id = contract_id;
  out.engine_id.id = static_cast<std::uint32_t>(contract_id);
  out.quote_event_ts_ns = available_ts_ns - 1;
  out.available_ts_ns = available_ts_ns;
  out.order_key = OptionMarketOrderKey{1U, 2U, 20'260'726U, sequence, 0U, sequence};
  out.bid = money("9.00");
  out.ask = money("11.00");
  out.bid_size_contracts = size_contracts;
  out.ask_size_contracts = size_contracts;
  out.bid_participant_id = 7U;
  out.ask_participant_id = 8U;
  out.source_identity = identity(sequence);
  return out;
}

[[nodiscard]] OptionFeeSchedule fee() {
  OptionFeeSchedule out;
  out.key = 1U;
  out.effective_from_ts_ns = 0;
  out.effective_until_ts_ns = 10'000;
  out.source_identity = identity(700U);
  return out;
}

[[nodiscard]] OptionTickSchedule tick() {
  OptionTickSchedule out;
  out.key = 1U;
  out.effective_from_ts_ns = 0;
  out.effective_until_ts_ns = 10'000;
  out.tick_below_threshold = money("0.01");
  out.tick_at_or_above_threshold = money("0.01");
  out.source_identity = identity(800U);
  return out;
}

[[nodiscard]] OptionReplayConfig replay_config() {
  OptionReplayConfig out;
  out.market_data_identity = identity(900U);
  out.sequence_validation_identity = identity(901U);
  out.calibration_identity = identity(902U);
  out.sequence_continuity_verified = true;
  out.scenario = OptionReplayScenario::Calibrated;
  out.displayed_size_fraction = Decimal::from_int(1);
  out.max_quote_age_ns = 1'000;
  out.replay_end_ts_ns = 800;
  return out;
}

[[nodiscard]] OptionAdaptiveCoordinatorLimits coordinator_limits() {
  OptionAdaptiveCoordinatorLimits out;
  out.execution.replay.max_contracts = 4U;
  out.execution.replay.max_quote_events = 32U;
  out.execution.replay.max_orders = 32U;
  out.execution.replay.max_cancellations = 32U;
  out.execution.replay.max_fee_rows = 4U;
  out.execution.replay.max_tick_rows = 4U;
  out.execution.replay.max_fills = 64U;
  out.execution.replay.max_workspace_bytes = 4U * 1024U * 1024U;
  out.execution.max_frontiers = 16U;
  out.execution.max_transitions = 256U;
  out.execution.max_workspace_bytes = 8U * 1024U * 1024U;
  out.pretrade_risk.max_contracts = 4U;
  out.pretrade_risk.max_scenarios = 4U;
  out.pretrade_risk.max_underliers = 4U;
  out.pretrade_risk.max_live_leaves = 32U;
  out.pretrade_risk.max_candidate_leaves = 16U;
  out.pretrade_risk.max_workspace_bytes = 1U * 1024U * 1024U;
  out.max_decisions = 8U;
  out.max_commands_per_decision = 16U;
  out.max_workspace_bytes = 1U * 1024U * 1024U;
  return out;
}

[[nodiscard]] OptionAdaptiveCoordinatorConfig coordinator_config() {
  OptionAdaptiveCoordinatorConfig out;
  out.weight_policy.transform = atx::engine::Transform::Rank;
  out.weight_policy.winsorize_limit = 0.025;
  out.weight_policy.dollar_neutral = true;
  out.weight_policy.gross_leverage = 1.0;
  out.target.basis = OptionSizingBasis::Vega;
  out.target.gross_budget = 200.0;
  out.target.max_position_adv_fraction = 1.0;
  out.target.available_initial_margin = 1'000'000.0;
  out.orders.first_order_id = 10U;
  out.orders.strategy_id = 77U;
  out.orders.basket_id = 100U;
  out.orders.first_priority_sequence = 1'000U;
  out.orders.fee_schedule_key = 1U;
  out.orders.arrival_latency_ns = 100;
  out.orders.time_in_force = OptionTimeInForce::GoodTillCanceled;
  out.first_cancel_id = 20U;
  out.cancel_latency_ns = 50;
  return out;
}

[[nodiscard]] OptionResearchPanel make_panel(std::span<const std::int64_t> dates,
                                             bool flip_after_first) {
  std::vector<OptionPanelRow> rows;
  rows.reserve(dates.size() * 2U);
  for (std::size_t i = 0; i < dates.size(); ++i) {
    const bool flipped = flip_after_first && i > 0U;
    OptionPanelRow first = panel_row(10U, dates[i], flipped ? 1.0 : -1.0);
    OptionPanelRow second = panel_row(20U, dates[i], flipped ? -1.0 : 1.0);
    if (i > 0U) {
      first.bid_size_contracts = 1.0;
      first.ask_size_contracts = 1.0;
      second.bid_size_contracts = 1.0;
      second.ask_size_contracts = 1.0;
    }
    rows.push_back(first);
    rows.push_back(second);
  }
  auto panel = OptionResearchPanel::create(rows);
  EXPECT_TRUE(panel) << panel.error().to_string();
  return std::move(*panel);
}

[[nodiscard]] std::vector<OptionTopOfBookEvent>
replay_quotes(const OptionResearchPanel &panel,
              std::span<const OptionTopOfBookEvent> additional = {}) {
  std::vector<OptionTopOfBookEvent> out;
  out.reserve(panel.dataset().num_dates() * panel.instruments().size() + additional.size());
  const auto bid = panel.column(atx::options::research::OptionPanelField::Bid);
  const auto ask = panel.column(atx::options::research::OptionPanelField::Ask);
  const auto bid_size = panel.column(atx::options::research::OptionPanelField::BidSizeContracts);
  const auto ask_size = panel.column(atx::options::research::OptionPanelField::AskSizeContracts);
  std::uint64_t sequence = 1U;
  for (std::size_t date_index = 0; date_index < panel.dataset().num_dates(); ++date_index) {
    for (std::size_t instrument_index = 0; instrument_index < panel.instruments().size();
         ++instrument_index) {
      if (!panel.tradable(date_index, instrument_index)) {
        continue;
      }
      const std::int64_t decision_ts_ns = panel.dataset().dates()[date_index];
      const std::uint64_t contract_id = panel.instruments()[instrument_index].contract_id;
      const auto audit = std::find_if(panel.decision_audit().begin(), panel.decision_audit().end(),
                                      [decision_ts_ns, contract_id](const auto &candidate) {
                                        return candidate.decision_ts_ns == decision_ts_ns &&
                                               candidate.contract_id == contract_id;
                                      });
      if (audit == panel.decision_audit().end()) {
        ADD_FAILURE() << "tradable panel row has no audit evidence";
        return {};
      }
      const std::size_t cell = date_index * panel.instruments().size() + instrument_index;
      OptionTopOfBookEvent event;
      event.contract_id = contract_id;
      event.engine_id = panel.instruments()[instrument_index].engine_id;
      event.quote_event_ts_ns = audit->quote_event_ts_ns;
      event.available_ts_ns = audit->quote_available_ts_ns;
      event.order_key = OptionMarketOrderKey{1U, 3U, 20'260'726U, sequence, 0U, sequence};
      event.bid = Decimal::from_double(bid[cell]).value_or(Decimal{});
      event.ask = Decimal::from_double(ask[cell]).value_or(Decimal{});
      event.bid_size_contracts = static_cast<std::int64_t>(bid_size[cell]);
      event.ask_size_contracts = static_cast<std::int64_t>(ask_size[cell]);
      event.bid_participant_id = 7U;
      event.ask_participant_id = 8U;
      event.bid_updated = true;
      event.ask_updated = true;
      event.source_identity = audit->execution_source_identity;
      out.push_back(event);
      ++sequence;
    }
  }
  out.insert(out.end(), additional.begin(), additional.end());
  return out;
}

[[nodiscard]] OptionRiskContentDigest risk_digest(std::uint8_t seed) noexcept {
  OptionRiskContentDigest out;
  for (std::size_t index = 0; index < out.bytes.size(); ++index) {
    out.bytes[index] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
  }
  return out;
}

[[nodiscard]] OptionRiskPanel
risk_panel(const OptionResearchPanel &panel, std::span<const double> dense_scenario_pnl = {},
           std::optional<ExerciseStyle> exercise_style = std::nullopt) {
  EXPECT_TRUE(dense_scenario_pnl.empty() ||
              dense_scenario_pnl.size() ==
                  panel.dataset().num_dates() * panel.instruments().size());
  std::vector<OptionRiskContractRow> rows;
  rows.reserve(panel.dataset().num_dates() * panel.instruments().size());
  std::vector<OptionRiskScenarioPnlRow> scenario_pnl;
  scenario_pnl.reserve(rows.capacity());
  constexpr std::uint64_t kScenarioId = 1U;
  std::size_t cell = 0U;
  for (std::int64_t date : panel.dataset().dates()) {
    for (const auto &instrument : panel.instruments()) {
      OptionRiskContractRow row;
      row.decision_ts_ns = date;
      row.contract_id = instrument.contract_id;
      row.engine_id = instrument.engine_id;
      row.underlier_uid = instrument.underlier_uid;
      row.observed_ts_ns = date - 20;
      row.available_ts_ns = date - 10;
      row.expiry_ts_ns = instrument.expiry_ts_ns;
      row.strike = instrument.strike;
      row.side = instrument.side;
      row.exercise_style = exercise_style.value_or(instrument.exercise_style);
      row.multiplier = instrument.multiplier;
      row.standard_deliverable = instrument.standard_deliverable;
      row.definition_source_identity = panel.decision_audit()[cell].definition_source_identity;
      row.spot_delta_cash_per_contract = instrument.contract_id == 10U ? 1.0 : -1.0;
      row.spot_gamma_cash_per_contract = 2.0;
      row.vega_cash_per_vol_point_per_contract = instrument.contract_id == 10U ? 3.0 : -3.0;
      row.theta_cash_per_day_per_contract = -1.0;
      row.vanna_cash_per_return_vol_point_per_contract = instrument.contract_id == 10U ? 0.5 : -0.5;
      row.volga_cash_per_vol_point_squared_per_contract = 0.25;
      row.premium_cash_notional_per_contract = 1'000.0;
      row.risk_source_identity = identity(instrument.contract_id + 1'000U);
      row.surface_source_identity = identity(instrument.contract_id + 2'000U);
      rows.push_back(row);
      scenario_pnl.push_back(OptionRiskScenarioPnlRow{
          date,
          instrument.contract_id,
          kScenarioId,
          date - 20,
          date - 10,
          dense_scenario_pnl.empty() ? (instrument.contract_id == 10U ? -10.0 : 10.0)
                                     : dense_scenario_pnl[cell],
          identity(instrument.contract_id + 3'000U),
      });
      ++cell;
    }
  }
  const std::array scenarios{OptionRiskScenario{kScenarioId, identity(4'000U)}};
  const OptionRiskPanelProvenance provenance{1U, 1U, risk_digest(1U), risk_digest(65U)};
  const auto created = OptionRiskPanel::create(rows, scenarios, scenario_pnl, provenance);
  EXPECT_TRUE(created) << created.error().to_string();
  return std::move(*created);
}

[[nodiscard]] atx::core::Result<atx::options::adaptive::OptionAdaptiveRunView>
run_coordinator(OptionAdaptiveCoordinator &coordinator, const OptionResearchPanel &panel,
                const OptionReplayInputs &inputs, const OptionReplayConfig &replay,
                const OptionAdaptiveCoordinatorConfig &config) {
  const OptionRiskPanel risk = risk_panel(panel);
  return coordinator.run(panel, risk, inputs, replay, config);
}

TEST(OptionAdaptiveCoordinator, CompatibleWorkingLeavesAreNotSubmittedTwice) {
  const std::array<std::int64_t, 2> dates{100, 400};
  const OptionResearchPanel panel = make_panel(dates, false);
  const std::array contracts{contract(10U), contract(20U)};
  const std::array additional_quotes{
      quote(10U, 300, 1U, 4),
      quote(20U, 300, 2U, 4),
      quote(10U, 600, 3U, 20),
      quote(20U, 600, 4U, 20),
  };
  const auto quotes = replay_quotes(panel, additional_quotes);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();

  const auto result =
      run_coordinator(*coordinator, panel, inputs, replay_config(), coordinator_config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 2U);
  EXPECT_EQ(result->decisions[0].submitted_order_count, 2U);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
  EXPECT_EQ(result->decisions[1].retained_leaf_count, 2U);
  EXPECT_FALSE(result->decisions[1].cancel_barrier);
  ASSERT_EQ(result->execution.replay.orders.size(), 2U);
  ASSERT_EQ(result->execution.replay.positions.size(), 2U);
  EXPECT_EQ(result->execution.replay.positions[0].contracts, -10);
  EXPECT_EQ(result->execution.replay.positions[1].contracts, 10);
}

TEST(OptionAdaptiveCoordinator, TargetFlipCancelsFirstAndReplansAfterBarrier) {
  const std::array<std::int64_t, 3> dates{100, 400, 500};
  const OptionResearchPanel panel = make_panel(dates, true);
  const std::array contracts{contract(10U), contract(20U)};
  const std::array additional_quotes{
      quote(10U, 300, 1U, 4),
      quote(20U, 300, 2U, 4),
      quote(10U, 700, 3U, 20),
      quote(20U, 700, 4U, 20),
  };
  const auto quotes = replay_quotes(panel, additional_quotes);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();

  const auto result =
      run_coordinator(*coordinator, panel, inputs, replay_config(), coordinator_config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 3U);
  EXPECT_TRUE(result->decisions[1].cancel_barrier);
  EXPECT_EQ(result->decisions[1].cancellation_count, 2U);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
  EXPECT_EQ(result->decisions[2].submitted_order_count, 2U);
  ASSERT_EQ(result->execution.replay.orders.size(), 4U);
  ASSERT_EQ(result->execution.replay.cancellations.size(), 2U);
  EXPECT_EQ(result->execution.replay.orders[2].request.quantity_contracts, 14);
  EXPECT_EQ(result->execution.replay.orders[3].request.quantity_contracts, -14);
  ASSERT_EQ(result->execution.replay.positions.size(), 2U);
  EXPECT_EQ(result->execution.replay.positions[0].contracts, 10);
  EXPECT_EQ(result->execution.replay.positions[1].contracts, -10);
}

TEST(OptionAdaptiveCoordinator, PendingCancelsAreNotDuplicatedOrTreatedAsTerminal) {
  const std::array<std::int64_t, 4> dates{100, 400, 425, 500};
  const OptionResearchPanel panel = make_panel(dates, true);
  const std::array contracts{contract(10U), contract(20U)};
  const std::array additional_quotes{
      quote(10U, 300, 1U, 4),
      quote(20U, 300, 2U, 4),
      quote(10U, 700, 3U, 20),
      quote(20U, 700, 4U, 20),
  };
  const auto quotes = replay_quotes(panel, additional_quotes);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();

  const auto result =
      run_coordinator(*coordinator, panel, inputs, replay_config(), coordinator_config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 4U);
  EXPECT_EQ(result->decisions[1].cancellation_count, 2U);
  EXPECT_EQ(result->decisions[2].cancellation_count, 0U);
  EXPECT_EQ(result->decisions[2].submitted_order_count, 0U);
  EXPECT_TRUE(result->decisions[2].cancel_barrier);
  EXPECT_EQ(result->decisions[3].submitted_order_count, 2U);
  EXPECT_EQ(result->execution.replay.cancellations.size(), 2U);
  EXPECT_EQ(result->execution.replay.orders.size(), 4U);
}

TEST(OptionAdaptiveCoordinator, ScheduledCancelWaitsUntilStrictlyAfterArrival) {
  const std::array<std::int64_t, 3> dates{100, 200, 500};
  const OptionResearchPanel panel = make_panel(dates, true);
  const std::array contracts{contract(10U), contract(20U)};
  const auto quotes = replay_quotes(panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.orders.arrival_latency_ns = 300;

  const auto result = run_coordinator(*coordinator, panel, inputs, replay_config(), config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->execution.replay.cancellations.size(), 2U);
  EXPECT_EQ(result->execution.replay.cancellations[0].request.available_ts_ns, 401);
  EXPECT_EQ(result->execution.replay.cancellations[1].request.available_ts_ns, 401);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
  EXPECT_EQ(result->decisions[2].submitted_order_count, 2U);
}

TEST(OptionAdaptiveCoordinator, WorkspaceByteBoundaryIsDeterministic) {
  OptionAdaptiveCoordinatorLimits limits = coordinator_limits();
  const auto required = option_adaptive_coordinator_required_workspace_bytes(limits);
  ASSERT_TRUE(required) << required.error().to_string();
  limits.max_workspace_bytes = *required;
  EXPECT_TRUE(OptionAdaptiveCoordinator::create(limits));
  --limits.max_workspace_bytes;
  const auto rejected = OptionAdaptiveCoordinator::create(limits);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::OutOfRange);
}

TEST(OptionAdaptiveCoordinator, SuccessfulRunAllocatesOnlyAtCreate) {
  const std::array<std::int64_t, 2> dates{100, 400};
  const OptionResearchPanel panel = make_panel(dates, false);
  const std::array contracts{contract(10U), contract(20U)};
  const std::array additional_quotes{
      quote(10U, 300, 1U, 4),
      quote(20U, 300, 2U, 4),
      quote(10U, 600, 3U, 20),
      quote(20U, 600, 4U, 20),
  };
  const auto quotes = replay_quotes(panel, additional_quotes);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.weight_policy.industry_neutral = true;
  const OptionRiskPanel risk = risk_panel(panel);
  option_replay_alloc::g_count.store(0U, std::memory_order_relaxed);
  option_replay_alloc::g_armed.store(true, std::memory_order_relaxed);

  const auto result = coordinator->run(panel, risk, inputs, replay_config(), config);

  option_replay_alloc::g_armed.store(false, std::memory_order_relaxed);
  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_EQ(option_replay_alloc::g_count.load(std::memory_order_relaxed), 0U);
}

TEST(OptionAdaptiveCoordinator, RejectsProjectedSafeBasketWithUnsafeFillSubset) {
  const std::array<std::int64_t, 1> dates{100};
  const OptionResearchPanel panel = make_panel(dates, false);
  const std::array contracts{contract(10U), contract(20U)};
  const auto quotes = replay_quotes(panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  const std::array<double, 2> pnl{10.0, 10.0};
  const OptionRiskPanel risk = risk_panel(panel, pnl);
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.pretrade_risk_limits.max_scenario_loss = 50.0;

  const auto result = coordinator->run(panel, risk, inputs, replay_config(), config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 1U);
  const auto &decision = result->decisions.front();
  EXPECT_DOUBLE_EQ(decision.pretrade_risk.candidate_projected.scenario_loss, 0.0);
  EXPECT_DOUBLE_EQ(decision.pretrade_risk.candidate_worst_fill.scenario_loss, 100.0);
  EXPECT_EQ(decision.pretrade_risk.disposition, OptionRiskDisposition::RejectNewOrders);
  EXPECT_EQ(decision.submitted_order_count, 0U);
  EXPECT_EQ(decision.cancellation_count, 0U);
  EXPECT_TRUE(result->execution.replay.orders.empty());
}

TEST(OptionAdaptiveCoordinator, BreachedPendingCancelsRemainInRiskEnvelope) {
  const std::array<std::int64_t, 3> dates{100, 400, 425};
  const OptionResearchPanel panel = make_panel(dates, false);
  const std::array contracts{contract(10U), contract(20U)};
  const std::array additional_quotes{
      quote(10U, 300, 1U, 4),
      quote(20U, 300, 2U, 4),
  };
  const auto quotes = replay_quotes(panel, additional_quotes);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  const std::array<double, 6> pnl{1.0, 1.0, 10.0, 10.0, 10.0, 10.0};
  const OptionRiskPanel risk = risk_panel(panel, pnl);
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.pretrade_risk_limits.max_scenario_loss = 20.0;

  const auto result = coordinator->run(panel, risk, inputs, replay_config(), config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 3U);
  EXPECT_EQ(result->decisions[0].pretrade_risk.disposition, OptionRiskDisposition::Accept);
  EXPECT_EQ(result->decisions[0].submitted_order_count, 2U);
  EXPECT_EQ(result->decisions[1].pretrade_risk.disposition, OptionRiskDisposition::CancelOnly);
  EXPECT_EQ(result->decisions[1].cancellation_count, 2U);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
  EXPECT_EQ(result->decisions[2].pretrade_risk.disposition, OptionRiskDisposition::CancelOnly);
  EXPECT_GT(result->decisions[2].pretrade_risk.baseline_worst_fill.open_order_contracts, 0U);
  EXPECT_GT(result->decisions[2].pretrade_risk.baseline_worst_fill.scenario_loss, 20.0);
  EXPECT_EQ(result->decisions[2].cancellation_count, 0U);
  EXPECT_EQ(result->execution.replay.cancellations.size(), 2U);
}

TEST(OptionAdaptiveCoordinator, FailedReductionCancelsBreachingLiveEnvelope) {
  const std::array<std::int64_t, 2> dates{100, 400};
  std::vector<OptionPanelRow> rows{
      panel_row(10U, dates[0], -1.0),
      panel_row(20U, dates[0], 1.0),
      panel_row(10U, dates[1], -1.0),
      panel_row(20U, dates[1], 1.0),
  };
  rows[2].vega_per_contract = 5.0;
  rows[2].bid_size_contracts = 1.0;
  rows[2].ask_size_contracts = 1.0;
  rows[3].bid_size_contracts = 1.0;
  rows[3].ask_size_contracts = 1.0;
  const auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array contracts{contract(10U), contract(20U)};
  const std::array additional_quotes{
      quote(10U, 300, 1U, 20),
      quote(20U, 300, 2U, 4),
  };
  const auto quotes = replay_quotes(*panel, additional_quotes);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  const std::array<double, 4> pnl{-1.0, 1.0, 10.0, 10.0};
  const OptionRiskPanel risk = risk_panel(*panel, pnl);
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.pretrade_risk_limits.max_scenario_loss = 20.0;

  const auto result = coordinator->run(*panel, risk, inputs, replay_config(), config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 2U);
  EXPECT_EQ(result->decisions[0].submitted_order_count, 2U);
  EXPECT_EQ(result->decisions[1].pretrade_risk.disposition, OptionRiskDisposition::CancelOnly);
  EXPECT_GT(result->decisions[1].pretrade_risk.baseline_worst_fill.scenario_loss, 20.0);
  EXPECT_GT(result->decisions[1].pretrade_risk.candidate_projected.scenario_loss,
            result->decisions[1].pretrade_risk.baseline_projected.scenario_loss);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
  EXPECT_EQ(result->decisions[1].cancellation_count, 1U);
  EXPECT_EQ(result->execution.replay.cancellations.size(), 1U);
}

TEST(OptionAdaptiveCoordinator, RiskLimitChangesRunDefinitionHash) {
  const std::array<std::int64_t, 1> dates{100};
  const OptionResearchPanel panel = make_panel(dates, false);
  const std::array contracts{contract(10U), contract(20U)};
  const auto quotes = replay_quotes(panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  const OptionRiskPanel risk = risk_panel(panel);
  auto first = OptionAdaptiveCoordinator::create(coordinator_limits());
  auto second = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  OptionAdaptiveCoordinatorConfig first_config = coordinator_config();
  first_config.pretrade_risk_limits.max_scenario_loss = 100.0;
  OptionAdaptiveCoordinatorConfig second_config = first_config;
  second_config.pretrade_risk_limits.max_scenario_loss = 101.0;

  const auto first_result = first->run(panel, risk, inputs, replay_config(), first_config);
  const auto second_result = second->run(panel, risk, inputs, replay_config(), second_config);

  ASSERT_TRUE(first_result) << first_result.error().to_string();
  ASSERT_TRUE(second_result) << second_result.error().to_string();
  EXPECT_NE(first_result->run_definition_hash, second_result->run_definition_hash);
}

TEST(OptionAdaptiveCoordinator, MisalignedRiskCatalogFailsBeforeSessionMutation) {
  const std::array<std::int64_t, 1> dates{100};
  const OptionResearchPanel panel = make_panel(dates, false);
  std::vector<OptionPanelRow> other_rows{
      panel_row(10U, dates[0], -1.0),
      panel_row(30U, dates[0], 1.0),
  };
  const auto other_panel = OptionResearchPanel::create(other_rows);
  ASSERT_TRUE(other_panel) << other_panel.error().to_string();
  const OptionRiskPanel risk = risk_panel(*other_panel);
  const std::array contracts{contract(10U), contract(20U)};
  const auto quotes = replay_quotes(panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator);

  const auto result = coordinator->run(panel, risk, inputs, replay_config(), coordinator_config());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(coordinator->state(), atx::options::adaptive::OptionAdaptiveCoordinatorState::Empty);
}

TEST(OptionAdaptiveCoordinator, MismatchedRiskExerciseStyleFailsBeforeSessionMutation) {
  const std::array<std::int64_t, 1> dates{100};
  const OptionResearchPanel panel = make_panel(dates, false);
  const OptionRiskPanel risk = risk_panel(panel, {}, ExerciseStyle::European);
  const std::array contracts{contract(10U), contract(20U)};
  const auto quotes = replay_quotes(panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator);

  const auto result = coordinator->run(panel, risk, inputs, replay_config(), coordinator_config());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(coordinator->state(), atx::options::adaptive::OptionAdaptiveCoordinatorState::Empty);
}

TEST(OptionAdaptiveCoordinator, InvalidRiskLimitFailsBeforeSessionMutation) {
  const std::array<std::int64_t, 1> dates{100};
  const OptionResearchPanel panel = make_panel(dates, false);
  const OptionRiskPanel risk = risk_panel(panel);
  const std::array contracts{contract(10U), contract(20U)};
  const auto quotes = replay_quotes(panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator);
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.pretrade_risk_limits.max_scenario_loss = -1.0;

  const auto result = coordinator->run(panel, risk, inputs, replay_config(), config);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(coordinator->state(), atx::options::adaptive::OptionAdaptiveCoordinatorState::Empty);
}

TEST(OptionAdaptiveCoordinator, ShuffledReplayInputsRetainCanonicalRunFingerprint) {
  const std::array<std::int64_t, 2> dates{100, 400};
  const OptionResearchPanel panel = make_panel(dates, false);
  const std::array canonical_contracts{contract(10U), contract(20U)};
  const std::array shuffled_contracts{contract(20U), contract(10U)};
  const std::array additional_quotes{
      quote(10U, 300, 1U, 4),
      quote(20U, 300, 2U, 4),
      quote(10U, 600, 3U, 20),
      quote(20U, 600, 4U, 20),
  };
  const auto canonical_quotes = replay_quotes(panel, additional_quotes);
  auto shuffled_quotes = canonical_quotes;
  std::reverse(shuffled_quotes.begin(), shuffled_quotes.end());
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs canonical_inputs{
      canonical_contracts, canonical_quotes, {}, {}, fees, ticks, Decimal{}};
  const OptionReplayInputs shuffled_inputs{shuffled_contracts, shuffled_quotes, {}, {}, fees, ticks,
                                           Decimal{}};
  auto first = OptionAdaptiveCoordinator::create(coordinator_limits());
  auto second = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(first) << first.error().to_string();
  ASSERT_TRUE(second) << second.error().to_string();

  const auto canonical =
      run_coordinator(*first, panel, canonical_inputs, replay_config(), coordinator_config());
  const auto shuffled =
      run_coordinator(*second, panel, shuffled_inputs, replay_config(), coordinator_config());

  ASSERT_TRUE(canonical) << canonical.error().to_string();
  ASSERT_TRUE(shuffled) << shuffled.error().to_string();
  EXPECT_EQ(canonical->run_definition_hash, shuffled->run_definition_hash);
  EXPECT_EQ(canonical->decision_trace_hash, shuffled->decision_trace_hash);
  EXPECT_EQ(canonical->execution.replay.positions[0].contracts,
            shuffled->execution.replay.positions[0].contracts);
  EXPECT_EQ(canonical->execution.replay.positions[1].contracts,
            shuffled->execution.replay.positions[1].contracts);
}

TEST(OptionAdaptiveCoordinator, DecisionQuoteMismatchFailsClosedBeforeReplay) {
  const std::array<std::int64_t, 1> dates{100};
  const OptionResearchPanel panel = make_panel(dates, false);
  const std::array contracts{contract(10U), contract(20U)};
  auto quotes = replay_quotes(panel);
  ASSERT_FALSE(quotes.empty());
  quotes.front().ask = money("11.01");
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();

  const auto result =
      run_coordinator(*coordinator, panel, inputs, replay_config(), coordinator_config());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_EQ(coordinator->state(), atx::options::adaptive::OptionAdaptiveCoordinatorState::Empty);
}

TEST(OptionAdaptiveCoordinator, SupersededDecisionQuoteFailsClosed) {
  const std::array<std::int64_t, 1> dates{100};
  const OptionResearchPanel panel = make_panel(dates, false);
  const std::array contracts{contract(10U), contract(20U)};
  auto quotes = replay_quotes(panel);
  ASSERT_GE(quotes.size(), 2U);
  OptionTopOfBookEvent newer = quotes.front();
  newer.order_key.channel_id = 4U;
  newer.order_key.native_sequence = 100U;
  newer.order_key.stable_ingest_ordinal = 100U;
  newer.bid = money("10.00");
  newer.ask = money("12.00");
  newer.source_identity = identity(9'999U);
  quotes.push_back(newer);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();

  const auto result =
      run_coordinator(*coordinator, panel, inputs, replay_config(), coordinator_config());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_NE(result.error().to_string().find("current replay state"), std::string::npos);
  EXPECT_EQ(coordinator->state(), atx::options::adaptive::OptionAdaptiveCoordinatorState::Empty);
}

TEST(OptionAdaptiveCoordinator, MissingSignalHoldsPositionAndCancelsIncreasingLeaf) {
  const std::array<std::int64_t, 2> dates{100, 200};
  std::vector<OptionPanelRow> rows{
      panel_row(10U, dates[0], 1.0),
      panel_row(20U, dates[0], -1.0),
      panel_row(10U, dates[1], 1.0),
      panel_row(20U, dates[1], -1.0),
  };
  rows[2].status = OptionPanelStatus::MissingQuote;
  const auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel) << panel.error().to_string();
  std::array contracts{contract(10U), contract(20U)};
  contracts[0].initial_contracts = 5;
  const auto quotes = replay_quotes(*panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.orders.arrival_latency_ns = 300;
  config.missing_signal_policy = OptionMissingSignalPolicy::HoldPositionAndReduceRisk;

  const auto result = run_coordinator(*coordinator, *panel, inputs, replay_config(), config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 2U);
  EXPECT_EQ(result->decisions[1].missing_signal_count, 1U);
  EXPECT_TRUE(result->decisions[1].cancel_barrier);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
  EXPECT_GE(result->decisions[1].cancellation_count, 1U);
  const auto canceled_increasing_leaf = std::any_of(
      result->execution.replay.cancellations.begin(), result->execution.replay.cancellations.end(),
      [&result](const auto &cancel_audit) {
        const auto order_id = cancel_audit.request.order_id;
        const auto found = std::find_if(result->execution.replay.orders.begin(),
                                        result->execution.replay.orders.end(),
                                        [order_id](const auto &order_audit) {
                                          return order_audit.request.order_id == order_id;
                                        });
        return found != result->execution.replay.orders.end() && found->request.contract_id == 10U;
      });
  EXPECT_TRUE(canceled_increasing_leaf);
}

TEST(OptionAdaptiveCoordinator, MissingSignalNeverOffsetsRetainedReducingLeaf) {
  const std::array<std::int64_t, 2> dates{100, 200};
  std::vector<OptionPanelRow> rows{
      panel_row(10U, dates[0], 1.0),
      panel_row(20U, dates[0], -1.0),
      panel_row(10U, dates[1], 1.0),
      panel_row(20U, dates[1], -1.0),
  };
  rows[2].status = OptionPanelStatus::MissingQuote;
  const auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel) << panel.error().to_string();
  std::array contracts{contract(10U), contract(20U)};
  contracts[0].initial_contracts = 10;
  const auto quotes = replay_quotes(*panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.target.gross_budget = 100.0;
  config.orders.arrival_latency_ns = 300;
  config.reconciliation_scope = OptionReconciliationScope::IndependentContract;

  const auto result = run_coordinator(*coordinator, *panel, inputs, replay_config(), config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 2U);
  EXPECT_EQ(result->decisions[1].missing_signal_count, 1U);
  EXPECT_EQ(result->decisions[1].retained_leaf_count, 1U);
  EXPECT_EQ(result->decisions[1].cancellation_count, 1U);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
  ASSERT_EQ(result->execution.replay.orders.size(), 2U);
  const auto reducing_leaf =
      std::find_if(result->execution.replay.orders.begin(), result->execution.replay.orders.end(),
                   [](const auto &order) { return order.request.contract_id == 10U; });
  ASSERT_NE(reducing_leaf, result->execution.replay.orders.end());
  EXPECT_EQ(reducing_leaf->request.quantity_contracts, -5);
}

TEST(OptionAdaptiveCoordinator, MissingSignalCancelsExposureIncreasingLeavesFromFlat) {
  const std::array<std::int64_t, 2> dates{100, 200};
  std::vector<OptionPanelRow> rows{
      panel_row(10U, dates[0], 1.0),
      panel_row(20U, dates[0], -1.0),
      panel_row(10U, dates[1], 1.0),
      panel_row(20U, dates[1], -1.0),
  };
  rows[2].status = OptionPanelStatus::MissingQuote;
  const auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array contracts{contract(10U), contract(20U)};
  const auto quotes = replay_quotes(*panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.orders.arrival_latency_ns = 300;
  config.reconciliation_scope = OptionReconciliationScope::IndependentContract;

  const auto result = run_coordinator(*coordinator, *panel, inputs, replay_config(), config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 2U);
  EXPECT_EQ(result->decisions[1].missing_signal_count, 1U);
  EXPECT_EQ(result->decisions[1].active_leaf_count, 2U);
  EXPECT_EQ(result->decisions[1].retained_leaf_count, 0U);
  EXPECT_EQ(result->decisions[1].cancellation_count, 2U);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
}

TEST(OptionAdaptiveCoordinator, WholeBasketBarrierCancelsCompatibleLeavesToo) {
  const std::array<std::int64_t, 2> dates{100, 200};
  const std::array<std::uint64_t, 4> ids{10U, 20U, 30U, 40U};
  const std::array<double, 4> first_signals{-2.0, -1.0, 1.0, 2.0};
  const std::array<double, 4> second_signals{-2.0, -1.0, 2.0, 1.0};
  std::vector<OptionPanelRow> rows;
  rows.reserve(8U);
  for (std::size_t index = 0; index < ids.size(); ++index) {
    rows.push_back(panel_row(ids[index], dates[0], first_signals[index]));
  }
  for (std::size_t index = 0; index < ids.size(); ++index) {
    rows.push_back(panel_row(ids[index], dates[1], second_signals[index]));
  }
  const auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array contracts{
      contract(10U),
      contract(20U),
      contract(30U),
      contract(40U),
  };
  const auto quotes = replay_quotes(*panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.target.gross_budget = 400.0;
  config.orders.arrival_latency_ns = 300;
  config.reconciliation_scope = OptionReconciliationScope::WholeBasketCancelBarrier;

  const auto result = run_coordinator(*coordinator, *panel, inputs, replay_config(), config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 2U);
  EXPECT_TRUE(result->decisions[1].cancel_barrier);
  EXPECT_EQ(result->decisions[1].active_leaf_count, 4U);
  EXPECT_EQ(result->decisions[1].retained_leaf_count, 0U);
  EXPECT_EQ(result->decisions[1].cancellation_count, 4U);
  EXPECT_EQ(result->decisions[1].submitted_order_count, 0U);
}

TEST(OptionAdaptiveCoordinator, CommandLimitBoundsOrdersAndCancellationsTogether) {
  const std::array<std::int64_t, 2> dates{100, 200};
  std::vector<OptionPanelRow> rows{
      panel_row(10U, dates[0], -2.0), panel_row(20U, dates[0], -1.0),
      panel_row(30U, dates[0], 1.0),  panel_row(40U, dates[0], 2.0),
      panel_row(50U, dates[0], 0.0),  panel_row(10U, dates[1], 2.0),
      panel_row(20U, dates[1], 1.0),  panel_row(30U, dates[1], -1.0),
      panel_row(40U, dates[1], -2.0), panel_row(50U, dates[1], 3.0),
  };
  rows[4].status = OptionPanelStatus::MissingQuote;
  const auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array contracts{
      contract(10U), contract(20U), contract(30U), contract(40U), contract(50U),
  };
  const auto quotes = replay_quotes(*panel);
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  OptionAdaptiveCoordinatorLimits limits = coordinator_limits();
  limits.execution.replay.max_contracts = 5U;
  limits.pretrade_risk.max_contracts = 5U;
  limits.max_commands_per_decision = 4U;
  auto coordinator = OptionAdaptiveCoordinator::create(limits);
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.target.gross_budget = 400.0;
  config.orders.arrival_latency_ns = 300;
  config.reconciliation_scope = OptionReconciliationScope::IndependentContract;
  config.missing_signal_policy = OptionMissingSignalPolicy::LiquidateToZero;

  const auto result = run_coordinator(*coordinator, *panel, inputs, replay_config(), config);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), atx::core::ErrorCode::OutOfRange);
  EXPECT_NE(result.error().to_string().find("total command"), std::string::npos);
  EXPECT_EQ(coordinator->state(), atx::options::adaptive::OptionAdaptiveCoordinatorState::Failed);
}

TEST(OptionAdaptiveCoordinator, EmptyDecisionDoesNotEvaluateOverflowingLatencies) {
  constexpr std::int64_t kDecision = (std::numeric_limits<std::int64_t>::max)() - 100;
  std::vector<OptionPanelRow> rows{
      panel_row(10U, kDecision, 1.0),
      panel_row(20U, kDecision, 1.0),
  };
  for (auto &row : rows) {
    row.expiry_ts_ns = (std::numeric_limits<std::int64_t>::max)();
  }
  const auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel) << panel.error().to_string();
  std::array contracts{contract(10U), contract(20U)};
  for (auto &item : contracts) {
    item.expiry_ts_ns = (std::numeric_limits<std::int64_t>::max)();
  }
  const auto quotes = replay_quotes(*panel);
  std::array fees{fee()};
  fees[0].effective_until_ts_ns = (std::numeric_limits<std::int64_t>::max)();
  std::array ticks{tick()};
  ticks[0].effective_until_ts_ns = (std::numeric_limits<std::int64_t>::max)();
  const OptionReplayInputs inputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  auto coordinator = OptionAdaptiveCoordinator::create(coordinator_limits());
  ASSERT_TRUE(coordinator) << coordinator.error().to_string();
  OptionAdaptiveCoordinatorConfig config = coordinator_config();
  config.weight_policy.gross_leverage = 0.0;
  config.orders.arrival_latency_ns = 200;
  config.cancel_latency_ns = 200;
  OptionReplayConfig replay = replay_config();
  replay.replay_end_ts_ns = kDecision;

  const auto result = run_coordinator(*coordinator, *panel, inputs, replay, config);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->decisions.size(), 1U);
  EXPECT_EQ(result->decisions[0].submitted_order_count, 0U);
  EXPECT_EQ(result->decisions[0].cancellation_count, 0U);
}

TEST(WeightPolicyScratch, RankWinsorAndGroupDemeanAllocateOnlyAtReserve) {
  constexpr std::size_t kCount = 128U;
  std::vector<double> signal(kCount);
  std::vector<atx::engine::InstrumentId> universe(kCount);
  std::vector<atx::u32> groups(kCount);
  for (std::size_t index = 0; index < kCount; ++index) {
    signal[index] = static_cast<double>((index * 37U) % kCount);
    universe[index].id = static_cast<std::uint32_t>(index + 1U);
    groups[index] = static_cast<atx::u32>((index % 8U) + 1U);
  }
  atx::engine::WeightPolicy policy;
  policy.transform = atx::engine::Transform::Rank;
  policy.industry_neutral = true;
  policy.winsorize_limit = 0.025;
  atx::engine::WeightPolicyScratch scratch;
  scratch.reserve(kCount);
  option_replay_alloc::g_count.store(0U, std::memory_order_relaxed);
  option_replay_alloc::g_armed.store(true, std::memory_order_relaxed);

  policy.to_target_weights(atx::engine::SignalView{signal}, universe, scratch, groups);

  option_replay_alloc::g_armed.store(false, std::memory_order_relaxed);
  EXPECT_EQ(scratch.weights.size(), kCount);
  EXPECT_EQ(option_replay_alloc::g_count.load(std::memory_order_relaxed), 0U);
}

} // namespace
