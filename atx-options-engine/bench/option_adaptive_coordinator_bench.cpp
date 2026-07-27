// Dense date-major signal -> target -> leaf reconciliation -> replay benchmark.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/decimal.hpp"
#include "atx/options/option_adaptive_coordinator.hpp"

namespace {

using atx::core::Decimal;
using atx::options::adaptive::OptionAdaptiveCoordinator;
using atx::options::adaptive::OptionAdaptiveCoordinatorConfig;
using atx::options::adaptive::OptionAdaptiveCoordinatorLimits;
using atx::options::execution::OptionFeeSchedule;
using atx::options::execution::OptionMarketOrderKey;
using atx::options::execution::OptionReplayConfig;
using atx::options::execution::OptionReplayContract;
using atx::options::execution::OptionReplayInputs;
using atx::options::execution::OptionTickSchedule;
using atx::options::execution::OptionTimeInForce;
using atx::options::execution::OptionTopOfBookEvent;
using atx::options::research::OptionPanelRow;
using atx::options::research::OptionResearchPanel;
using atx::options::research::OptionSizingBasis;
using atx::options::risk::OptionRiskContentDigest;
using atx::options::risk::OptionRiskContractRow;
using atx::options::risk::OptionRiskPanel;
using atx::options::risk::OptionRiskPanelLimits;
using atx::options::risk::OptionRiskPanelProvenance;
using atx::options::risk::OptionRiskRowStatus;
using atx::options::risk::OptionRiskScenario;
using atx::options::risk::OptionRiskScenarioPnlRow;
using atx::vol::ArchiveContentIdentity;

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) noexcept {
  return ArchiveContentIdentity{100'000U + seed, 200'000U + seed,
                                static_cast<std::uint32_t>(300'000U + seed),
                                static_cast<std::uint32_t>(400'000U + seed)};
}

[[nodiscard]] OptionRiskContentDigest digest(std::uint8_t seed) noexcept {
  OptionRiskContentDigest out;
  for (std::size_t index = 0; index < out.bytes.size(); ++index) {
    out.bytes[index] = static_cast<std::uint8_t>(seed + static_cast<std::uint8_t>(index));
  }
  return out;
}

[[nodiscard]] Decimal money(const char *text) {
  return Decimal::from_string(text).value_or(Decimal{});
}

[[nodiscard]] OptionPanelRow panel_row(std::uint64_t contract_id, std::int64_t decision_ts_ns,
                                       double signal) {
  OptionPanelRow out;
  out.observation.uid = static_cast<std::uint32_t>(((contract_id - 1U) % 16U) + 1U);
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
  out.expiry_ts_ns = 1'000'000;
  out.strike = 100.0;
  out.multiplier = 100.0;
  out.mark = 10.0;
  out.bid = 9.0;
  out.ask = 11.0;
  out.bid_size_contracts = 100.0;
  out.ask_size_contracts = 100.0;
  out.interval_volume_contracts = 1'000.0;
  out.lagged_open_interest_contracts = 10'000.0;
  out.adv_contracts = 100'000.0;
  out.return_sigma = 0.2;
  out.vega_per_contract = 10.0;
  out.initial_margin_per_contract = 100.0;
  out.maintenance_margin_per_contract = 80.0;
  out.definition_source_identity = identity(10'000U + contract_id);
  out.feature_source_identity = identity(contract_id + 3U);
  out.execution_source_identity = identity(900U);
  return out;
}

struct CoordinatorFixture {
  std::size_t contract_count{0};
  std::size_t decision_count{0};
  OptionResearchPanel panel;
  OptionRiskPanel risk_panel;
  OptionAdaptiveCoordinatorLimits limits{};
  OptionAdaptiveCoordinatorConfig coordinator_config{};
  OptionReplayConfig replay_config{};
  std::vector<OptionReplayContract> contracts;
  std::vector<OptionTopOfBookEvent> quotes;
  std::vector<OptionFeeSchedule> fees;
  std::vector<OptionTickSchedule> ticks;

  CoordinatorFixture(std::size_t configured_contracts, std::size_t configured_decisions,
                     bool churn = false)
      : contract_count{configured_contracts}, decision_count{configured_decisions},
        panel{make_panel(configured_contracts, configured_decisions, churn)},
        risk_panel{make_risk_panel(panel)} {
    const std::size_t order_capacity =
        churn ? configured_contracts * ((configured_decisions + 1U) / 2U)
              : configured_contracts + 1U;
    const std::size_t cancel_capacity =
        churn ? configured_contracts * (configured_decisions / 2U) : configured_contracts + 1U;
    const std::size_t command_capacity = configured_contracts + 1U;
    limits.execution.replay.max_contracts = configured_contracts;
    limits.execution.replay.max_quote_events = configured_contracts * configured_decisions;
    limits.execution.replay.max_orders = order_capacity;
    limits.execution.replay.max_cancellations = cancel_capacity;
    limits.execution.replay.max_fee_rows = 1U;
    limits.execution.replay.max_tick_rows = 1U;
    limits.execution.replay.max_fills = configured_contracts * configured_decisions;
    limits.execution.replay.max_workspace_bytes = 256U * 1024U * 1024U;
    limits.execution.max_frontiers = configured_decisions;
    limits.execution.max_transitions =
        (order_capacity + cancel_capacity) * 3U + limits.execution.replay.max_fills;
    limits.execution.max_workspace_bytes = 512U * 1024U * 1024U;
    limits.pretrade_risk.max_contracts = configured_contracts;
    limits.pretrade_risk.max_live_leaves = order_capacity;
    limits.pretrade_risk.max_candidate_leaves = command_capacity;
    limits.pretrade_risk.max_scenarios = 1U;
    limits.pretrade_risk.max_underliers = (std::min)(configured_contracts, std::size_t{16U});
    limits.pretrade_risk.max_workspace_bytes = 256U * 1024U * 1024U;
    limits.max_decisions = configured_decisions;
    limits.max_commands_per_decision = command_capacity;
    limits.max_workspace_bytes = 256U * 1024U * 1024U;

    coordinator_config.weight_policy.transform = atx::engine::Transform::Rank;
    coordinator_config.weight_policy.winsorize_limit = 0.025;
    coordinator_config.weight_policy.dollar_neutral = true;
    coordinator_config.weight_policy.gross_leverage = 1.0;
    coordinator_config.target.basis = OptionSizingBasis::Vega;
    coordinator_config.target.gross_budget = static_cast<double>(configured_contracts) * 100.0;
    coordinator_config.target.max_position_adv_fraction = 1.0;
    coordinator_config.target.available_initial_margin = 1.0e15;
    coordinator_config.orders.first_order_id = 1U;
    coordinator_config.orders.strategy_id = 1U;
    coordinator_config.orders.basket_id = 1U;
    coordinator_config.orders.first_priority_sequence = 1U;
    coordinator_config.orders.fee_schedule_key = 1U;
    coordinator_config.orders.arrival_latency_ns = 1;
    coordinator_config.orders.time_in_force = OptionTimeInForce::GoodTillCanceled;
    coordinator_config.first_cancel_id = 1U;
    coordinator_config.cancel_latency_ns = 1;

    const std::int64_t replay_end = 100 + static_cast<std::int64_t>(configured_decisions) + 10;
    replay_config.market_data_identity = identity(900U);
    replay_config.sequence_validation_identity = identity(901U);
    replay_config.sequence_continuity_verified = true;
    replay_config.replay_end_ts_ns = replay_end;

    contracts.reserve(configured_contracts);
    for (std::size_t index = 0; index < configured_contracts; ++index) {
      const std::uint64_t id = static_cast<std::uint64_t>(index + 1U);
      contracts.push_back(OptionReplayContract{id,
                                               {static_cast<std::uint32_t>(id)},
                                               100,
                                               0,
                                               1U,
                                               1,
                                               2,
                                               1'000'000,
                                               identity(10'000U + id)});
    }
    quotes.reserve(configured_contracts * configured_decisions);
    const auto bid = panel.column(atx::options::research::OptionPanelField::Bid);
    const auto ask = panel.column(atx::options::research::OptionPanelField::Ask);
    const auto bid_size = panel.column(atx::options::research::OptionPanelField::BidSizeContracts);
    const auto ask_size = panel.column(atx::options::research::OptionPanelField::AskSizeContracts);
    for (std::size_t index = 0; index < panel.decision_audit().size(); ++index) {
      const auto &audit = panel.decision_audit()[index];
      const auto instrument = std::lower_bound(
          panel.instruments().begin(), panel.instruments().end(), audit.contract_id,
          [](const atx::options::research::OptionInstrument &candidate, std::uint64_t contract_id) {
            return candidate.contract_id < contract_id;
          });
      if (instrument == panel.instruments().end() || instrument->contract_id != audit.contract_id) {
        throw std::runtime_error{"adaptive benchmark audit/catalog mismatch"};
      }
      OptionTopOfBookEvent event;
      event.contract_id = audit.contract_id;
      event.engine_id = instrument->engine_id;
      event.quote_event_ts_ns = audit.quote_event_ts_ns;
      event.available_ts_ns = audit.quote_available_ts_ns;
      const std::uint64_t sequence = static_cast<std::uint64_t>(index + 1U);
      event.order_key = OptionMarketOrderKey{1U, 3U, 20'260'726U, sequence, 0U, sequence};
      event.bid = Decimal::from_double(bid[index]).value_or(Decimal{});
      event.ask = Decimal::from_double(ask[index]).value_or(Decimal{});
      event.bid_size_contracts = static_cast<std::int64_t>(bid_size[index]);
      event.ask_size_contracts = static_cast<std::int64_t>(ask_size[index]);
      event.bid_participant_id = 7U;
      event.ask_participant_id = 8U;
      event.bid_updated = true;
      event.ask_updated = true;
      event.source_identity = audit.execution_source_identity;
      quotes.push_back(event);
    }
    fees.push_back(
        OptionFeeSchedule{1U, 0, 0, replay_end + 1'000, {}, {}, {}, {}, {}, {}, identity(902U)});
    ticks.push_back(OptionTickSchedule{
        1U, 0, 0, replay_end + 1'000, {}, money("0.01"), money("0.01"), identity(903U)});
  }

  [[nodiscard]] static OptionResearchPanel make_panel(std::size_t contract_count,
                                                      std::size_t decision_count, bool churn) {
    std::vector<OptionPanelRow> rows;
    rows.reserve(contract_count * decision_count);
    for (std::size_t date = 0; date < decision_count; ++date) {
      const std::int64_t timestamp = 100 + static_cast<std::int64_t>(date);
      const bool reverse = churn && (((date + 1U) / 2U) % 2U != 0U);
      for (std::size_t contract_index = 0; contract_index < contract_count; ++contract_index) {
        const std::uint64_t id = static_cast<std::uint64_t>(contract_index + 1U);
        const std::size_t signal_rank =
            reverse ? contract_count - 1U - contract_index : contract_index;
        rows.push_back(panel_row(id, timestamp, static_cast<double>(signal_rank)));
      }
    }
    auto created = OptionResearchPanel::create(rows);
    if (!created) {
      std::fprintf(stderr, "adaptive benchmark panel error: %s\n",
                   created.error().to_string().c_str());
      throw std::runtime_error{created.error().to_string()};
    }
    return std::move(*created);
  }

  [[nodiscard]] static OptionRiskPanel make_risk_panel(const OptionResearchPanel &research_panel) {
    const std::size_t contract_rows = research_panel.decision_audit().size();
    std::vector<OptionRiskContractRow> rows;
    std::vector<OptionRiskScenarioPnlRow> scenario_pnl;
    rows.reserve(contract_rows);
    scenario_pnl.reserve(contract_rows);
    constexpr std::uint64_t scenario_id = 1'001U;
    for (std::size_t date_index = 0; date_index < research_panel.dataset().dates().size();
         ++date_index) {
      const std::int64_t decision_ts_ns = research_panel.dataset().dates()[date_index];
      for (std::size_t contract_index = 0; contract_index < research_panel.instruments().size();
           ++contract_index) {
        const auto &instrument = research_panel.instruments()[contract_index];
        const std::size_t cell = date_index * research_panel.instruments().size() + contract_index;
        OptionRiskContractRow row;
        row.decision_ts_ns = decision_ts_ns;
        row.contract_id = instrument.contract_id;
        row.engine_id = instrument.engine_id;
        row.underlier_uid = instrument.underlier_uid;
        row.observed_ts_ns = decision_ts_ns - 20;
        row.available_ts_ns = decision_ts_ns - 10;
        row.market_observed_ts_ns = research_panel.decision_audit()[cell].quote_event_ts_ns;
        row.market_available_ts_ns = research_panel.decision_audit()[cell].quote_available_ts_ns;
        row.definition_available_ts_ns =
            research_panel.decision_audit()[cell].definition_available_ts_ns;
        row.expiry_ts_ns = instrument.expiry_ts_ns;
        row.strike = instrument.strike;
        row.side = instrument.side;
        row.multiplier = instrument.multiplier;
        row.standard_deliverable = instrument.standard_deliverable;
        row.definition_source_identity =
            research_panel.decision_audit()[cell].definition_source_identity;
        row.spot_delta_cash_per_contract = contract_index % 2U == 0U ? 1.0 : -1.0;
        row.spot_gamma_cash_per_contract = 2.0;
        row.vega_cash_per_vol_point_per_contract = 10.0;
        row.theta_cash_per_day_per_contract = -1.0;
        row.vanna_cash_per_return_vol_point_per_contract = contract_index % 2U == 0U ? -1.0 : 1.0;
        row.volga_cash_per_vol_point_squared_per_contract = 1.0;
        row.premium_cash_notional_per_contract = 1'000.0;
        row.status = OptionRiskRowStatus::Ok;
        row.risk_source_identity = identity(40'000U + instrument.contract_id);
        row.surface_source_identity = identity(50'000U + instrument.contract_id);
        row.market_source_identity =
            research_panel.decision_audit()[cell].execution_source_identity;
        rows.push_back(row);

        const double pnl = contract_index % 3U == 0U ? -5.0 : 2.0;
        scenario_pnl.push_back(OptionRiskScenarioPnlRow{
            decision_ts_ns, instrument.contract_id, scenario_id, decision_ts_ns - 20,
            decision_ts_ns - 10, pnl, identity(60'000U + instrument.contract_id)});
      }
    }

    const std::array scenarios{
        OptionRiskScenario{scenario_id, identity(70'000U)},
    };
    OptionRiskPanelProvenance provenance;
    provenance.pricer_model_version = 1U;
    provenance.greek_convention_version = 1U;
    provenance.risk_snapshot_digest = digest(1U);
    provenance.scenario_manifest_digest = digest(101U);
    OptionRiskPanelLimits limits;
    limits.max_contract_rows = contract_rows;
    limits.max_scenarios = scenarios.size();
    limits.max_scenario_rows = contract_rows;
    limits.max_workspace_bytes = 256U * 1024U * 1024U;
    auto created = OptionRiskPanel::create(rows, scenarios, scenario_pnl, provenance, limits);
    if (!created) {
      throw std::runtime_error{created.error().to_string()};
    }
    return std::move(*created);
  }

  [[nodiscard]] OptionReplayInputs inputs() const noexcept {
    return OptionReplayInputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  }
};

void BM_AdaptiveCoordinatorDateMajor(benchmark::State &state) {
  const std::size_t contracts = static_cast<std::size_t>(state.range(0));
  const std::size_t decisions = static_cast<std::size_t>(state.range(1));
  const CoordinatorFixture fixture{contracts, decisions};
  auto created = OptionAdaptiveCoordinator::create(fixture.limits);
  if (!created) {
    state.SkipWithError(created.error().to_string().c_str());
    return;
  }
  OptionAdaptiveCoordinator coordinator = std::move(*created);
  const auto run_once = [&]() {
    return coordinator.run(fixture.panel, fixture.risk_panel, fixture.inputs(),
                           fixture.replay_config, fixture.coordinator_config);
  };
  const auto verified = run_once();
  if (!verified || verified->decisions.size() != decisions ||
      verified->execution.replay.orders.empty()) {
    const std::string error =
        verified ? "adaptive coordinator fixture produced no orders" : verified.error().to_string();
    state.SkipWithError(error.c_str());
    return;
  }

  for (auto _ : state) {
    static_cast<void>(_);
    const auto result = run_once();
    if (!result) {
      state.SkipWithError(result.error().to_string().c_str());
      break;
    }
    std::uint64_t trace = result->decision_trace_hash;
    benchmark::DoNotOptimize(trace);
  }
  const double completed_decisions =
      static_cast<double>(state.iterations()) * static_cast<double>(decisions);
  state.counters["decisions_per_second"] =
      benchmark::Counter(completed_decisions, benchmark::Counter::kIsRate);
  state.counters["contract_decisions_per_second"] = benchmark::Counter(
      completed_decisions * static_cast<double>(contracts), benchmark::Counter::kIsRate);
}

BENCHMARK(BM_AdaptiveCoordinatorDateMajor)->Args({128, 256})->Args({1'024, 64});

void BM_AdaptiveCoordinatorCancelReplaceChurn(benchmark::State &state) {
  const std::size_t contracts = static_cast<std::size_t>(state.range(0));
  const std::size_t decisions = static_cast<std::size_t>(state.range(1));
  const CoordinatorFixture fixture{contracts, decisions, true};
  auto created = OptionAdaptiveCoordinator::create(fixture.limits);
  if (!created) {
    state.SkipWithError(created.error().to_string().c_str());
    return;
  }
  OptionAdaptiveCoordinator coordinator = std::move(*created);
  const auto run_once = [&]() {
    return coordinator.run(fixture.panel, fixture.risk_panel, fixture.inputs(),
                           fixture.replay_config, fixture.coordinator_config);
  };
  const auto verified = run_once();
  if (!verified || verified->execution.replay.cancellations.empty()) {
    const std::string error = verified ? "adaptive churn fixture produced no cancellations"
                                       : verified.error().to_string();
    state.SkipWithError(error.c_str());
    return;
  }

  for (auto _ : state) {
    static_cast<void>(_);
    const auto result = run_once();
    if (!result) {
      state.SkipWithError(result.error().to_string().c_str());
      break;
    }
    std::uint64_t trace = result->decision_trace_hash;
    benchmark::DoNotOptimize(trace);
  }
  const double completed_decisions =
      static_cast<double>(state.iterations()) * static_cast<double>(decisions);
  state.counters["decisions_per_second"] =
      benchmark::Counter(completed_decisions, benchmark::Counter::kIsRate);
  state.counters["contract_decisions_per_second"] = benchmark::Counter(
      completed_decisions * static_cast<double>(contracts), benchmark::Counter::kIsRate);
}

BENCHMARK(BM_AdaptiveCoordinatorCancelReplaceChurn)->Args({64, 128});

} // namespace
