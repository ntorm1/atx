#include "atx/options/option_adaptive_coordinator.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

#include "atx/core/safe_math.hpp"

namespace atx::options::adaptive {
namespace {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;
using execution::OptionCancelId;
using execution::OptionCancelRequest;
using execution::OptionExecutionFrontierView;
using execution::OptionOrderLifecycleState;
using execution::OptionOrderRequest;

constexpr std::uint64_t kFnvOffset = 14'695'981'039'346'656'037ULL;
constexpr std::uint64_t kFnvPrime = 1'099'511'628'211ULL;

struct CancelCandidate {
  std::size_t order_index{0};
  std::int64_t available_ts_ns{0};
};

[[nodiscard]] Result<std::size_t> checked_add_size(std::size_t left, std::size_t right) {
  if (left > (std::numeric_limits<std::size_t>::max)() - right) {
    return Err(ErrorCode::OutOfRange, "adaptive coordinator size addition overflow");
  }
  return Ok(left + right);
}

[[nodiscard]] Result<std::size_t> checked_mul_size(std::size_t left, std::size_t right) {
  if (left != 0U && right > (std::numeric_limits<std::size_t>::max)() / left) {
    return Err(ErrorCode::OutOfRange, "adaptive coordinator size multiplication overflow");
  }
  return Ok(left * right);
}

[[nodiscard]] std::uint64_t fold_u64(std::uint64_t hash, std::uint64_t value) noexcept {
  for (std::size_t byte = 0U; byte < sizeof(value); ++byte) {
    hash ^= value & 0xFFU;
    hash *= kFnvPrime;
    value >>= 8U;
  }
  return hash;
}

[[nodiscard]] std::uint64_t fold_i64(std::uint64_t hash, std::int64_t value) noexcept {
  return fold_u64(hash, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t fold_double(std::uint64_t hash, double value) noexcept {
  return fold_u64(hash, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] std::uint64_t
fold_identity(std::uint64_t hash, const atx::vol::ArchiveContentIdentity &identity) noexcept {
  hash = fold_u64(hash, identity.file_size);
  hash = fold_u64(hash, identity.created_ts_ns);
  hash = fold_u64(hash, identity.header_crc32c);
  return fold_u64(hash, identity.metadata_crc32c);
}

[[nodiscard]] bool active(OptionOrderLifecycleState state) noexcept {
  switch (state) {
  case OptionOrderLifecycleState::Scheduled:
  case OptionOrderLifecycleState::Working:
  case OptionOrderLifecycleState::PartiallyFilled:
  case OptionOrderLifecycleState::PendingCancel:
    return true;
  case OptionOrderLifecycleState::Filled:
  case OptionOrderLifecycleState::Canceled:
  case OptionOrderLifecycleState::Expired:
  case OptionOrderLifecycleState::NotApplicable:
    return false;
  }
  return false;
}

[[nodiscard]] bool valid_transform(atx::engine::Transform transform) noexcept {
  switch (transform) {
  case atx::engine::Transform::Rank:
  case atx::engine::Transform::ZScore:
  case atx::engine::Transform::Raw:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_scope(OptionReconciliationScope scope) noexcept {
  switch (scope) {
  case OptionReconciliationScope::WholeBasketCancelBarrier:
  case OptionReconciliationScope::IndependentContract:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_missing_signal_policy(OptionMissingSignalPolicy policy) noexcept {
  switch (policy) {
  case OptionMissingSignalPolicy::HoldPositionAndReduceRisk:
  case OptionMissingSignalPolicy::LiquidateToZero:
  case OptionMissingSignalPolicy::RejectDecision:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_time_in_force(execution::OptionTimeInForce time_in_force) noexcept {
  switch (time_in_force) {
  case execution::OptionTimeInForce::FirstFutureQuoteOrCancel:
  case execution::OptionTimeInForce::Day:
  case execution::OptionTimeInForce::GoodTillCanceled:
    return true;
  }
  return false;
}

[[nodiscard]] Result<void> validate_config(const OptionAdaptiveCoordinatorConfig &config) {
  const auto &policy = config.weight_policy;
  const auto &orders = config.orders;
  if (!valid_transform(policy.transform) || !std::isfinite(policy.gross_leverage) ||
      policy.gross_leverage < 0.0 || policy.gross_leverage > 1.0 ||
      !std::isfinite(policy.winsorize_limit) || policy.winsorize_limit < 0.0 ||
      policy.winsorize_limit > 0.5 || !std::isfinite(policy.truncation) ||
      policy.truncation < 0.0 || config.first_cancel_id == 0U || config.cancel_latency_ns <= 0 ||
      !valid_scope(config.reconciliation_scope) ||
      !valid_missing_signal_policy(config.missing_signal_policy) || orders.first_order_id == 0U ||
      orders.strategy_id == 0U || orders.basket_id == 0U || orders.first_priority_sequence == 0U ||
      orders.fee_schedule_key == 0U || orders.arrival_latency_ns <= 0 ||
      !valid_time_in_force(orders.time_in_force) || orders.limit_offset_bps.raw() < 0 ||
      orders.limit_offset_bps >= atx::core::Decimal::from_int(10'000) ||
      orders.limit_price_increment.raw() < 0 ||
      (orders.limit_offset_bps.raw() > 0 && orders.limit_price_increment.raw() == 0) ||
      (orders.time_in_force != execution::OptionTimeInForce::Day && orders.expire_ts_ns != 0)) {
    return Err(ErrorCode::InvalidArgument, "adaptive coordinator configuration is invalid");
  }
  return Ok();
}

[[nodiscard]] std::uint64_t hash_signal(std::span<const double> signal) noexcept {
  std::uint64_t hash = fold_u64(kFnvOffset, signal.size());
  for (double value : signal) {
    hash = fold_double(hash, value);
  }
  return hash;
}

[[nodiscard]] std::uint64_t
hash_input_state(const OptionExecutionFrontierView &frontier,
                 std::span<const std::size_t> active_order_indices) noexcept {
  std::uint64_t hash = fold_i64(kFnvOffset, frontier.frontier_ts_ns);
  for (const auto &exposure : frontier.exposures) {
    hash = fold_u64(hash, exposure.contract_id);
    hash = fold_u64(hash, exposure.engine_id.id);
    hash = fold_i64(hash, exposure.position_contracts);
    hash = fold_i64(hash, exposure.scheduled_contracts);
    hash = fold_i64(hash, exposure.working_contracts);
    hash = fold_i64(hash, exposure.pending_cancel_contracts);
    hash = fold_i64(hash, exposure.projected_contracts);
  }
  for (std::size_t order_index : active_order_indices) {
    const auto &state = frontier.order_states[order_index];
    hash = fold_u64(hash, state.order_id.value);
    hash = fold_i64(hash, state.remaining_contracts);
    hash = fold_u64(hash, static_cast<std::uint64_t>(state.state));
    hash = fold_u64(hash, state.cancel_pending ? 1U : 0U);
  }
  return hash;
}

[[nodiscard]] std::uint64_t hash_targets(const research::OptionTargetBook &targets) noexcept {
  std::uint64_t hash = fold_i64(kFnvOffset, targets.decision_ts_ns);
  for (const auto &target : targets.targets) {
    hash = fold_u64(hash, target.contract_id);
    hash = fold_i64(hash, target.current_contracts);
    hash = fold_i64(hash, target.target_contracts);
  }
  hash = fold_double(hash, targets.requested_gross_exposure);
  hash = fold_double(hash, targets.realized_gross_exposure);
  hash = fold_double(hash, targets.initial_margin);
  return fold_double(hash, targets.maintenance_margin);
}

[[nodiscard]] std::uint64_t
hash_commands(std::span<const OptionOrderRequest> orders,
              std::span<const OptionCancelRequest> cancellations) noexcept {
  std::uint64_t hash = fold_u64(kFnvOffset, orders.size());
  hash = fold_u64(hash, cancellations.size());
  for (const auto &cancel : cancellations) {
    hash = fold_u64(hash, cancel.cancel_id.value);
    hash = fold_u64(hash, cancel.order_id.value);
    hash = fold_i64(hash, cancel.event_ts_ns);
    hash = fold_i64(hash, cancel.available_ts_ns);
    hash = fold_u64(hash, cancel.priority_sequence);
  }
  for (const auto &order : orders) {
    hash = fold_u64(hash, order.order_id.value);
    hash = fold_u64(hash, order.strategy_id);
    hash = fold_u64(hash, order.basket_id);
    hash = fold_u64(hash, order.contract_id);
    hash = fold_u64(hash, order.engine_id.id);
    hash = fold_i64(hash, order.quantity_contracts);
    hash = fold_i64(hash, order.limit_price.raw());
    hash = fold_i64(hash, order.decision_ts_ns);
    hash = fold_i64(hash, order.arrival_ts_ns);
    hash = fold_i64(hash, order.expire_ts_ns);
    hash = fold_u64(hash, order.priority_sequence);
    hash = fold_u64(hash, order.fee_schedule_key);
    hash = fold_u64(hash, static_cast<std::uint64_t>(order.time_in_force));
  }
  return hash;
}

struct UnorderedFingerprint {
  std::uint64_t sum{0};
  std::uint64_t rotated_xor{0};
  std::uint64_t weighted_sum{0};

  void add(std::uint64_t value) noexcept {
    sum += value;
    rotated_xor ^= std::rotl(value, static_cast<int>(value & 63U));
    weighted_sum += value * (value | 1U);
  }

  [[nodiscard]] std::uint64_t finish(std::uint64_t hash, std::size_t count) const noexcept {
    hash = fold_u64(hash, count);
    hash = fold_u64(hash, sum);
    hash = fold_u64(hash, rotated_xor);
    return fold_u64(hash, weighted_sum);
  }
};

[[nodiscard]] std::uint64_t hash_run_definition(
    const research::OptionResearchPanel &panel, const execution::OptionReplayInputs &inputs,
    const execution::OptionReplayConfig &replay_config,
    const OptionAdaptiveCoordinatorConfig &config, const OptionAdaptiveCoordinatorLimits &limits,
    std::span<const std::size_t> replay_contract_indices) noexcept {
  std::uint64_t hash = fold_u64(kFnvOffset, kOptionAdaptiveCoordinatorModelVersion);
  hash = fold_u64(hash, kOptionAdaptiveCoordinatorOrderingVersion);
  hash = fold_u64(hash, replay_config.model_version);
  hash = fold_u64(hash, execution::kOptionExecutionSessionModelVersion);
  hash = fold_u64(hash, execution::kOptionExecutionSessionOrderingVersion);
  hash = fold_identity(hash, replay_config.market_data_identity);
  hash = fold_identity(hash, replay_config.sequence_validation_identity);
  hash = fold_identity(hash, replay_config.calibration_identity);
  hash = fold_u64(hash, static_cast<std::uint64_t>(replay_config.scenario));
  hash = fold_u64(hash, replay_config.sequence_continuity_verified ? 1U : 0U);
  hash = fold_u64(hash, replay_config.allow_locked_market ? 1U : 0U);
  hash = fold_i64(hash, replay_config.displayed_size_fraction.raw());
  hash = fold_i64(hash, replay_config.adverse_price_bps.raw());
  hash = fold_i64(hash, replay_config.max_quote_age_ns);
  hash = fold_i64(hash, replay_config.replay_end_ts_ns);
  hash = fold_u64(hash, static_cast<std::uint64_t>(config.weight_policy.transform));
  hash = fold_double(hash, config.weight_policy.gross_leverage);
  hash = fold_double(hash, config.weight_policy.winsorize_limit);
  hash = fold_double(hash, config.weight_policy.truncation);
  hash = fold_u64(hash, config.weight_policy.industry_neutral ? 1U : 0U);
  hash = fold_u64(hash, config.weight_policy.dollar_neutral ? 1U : 0U);
  hash = fold_u64(hash, static_cast<std::uint64_t>(config.target.basis));
  hash = fold_double(hash, config.target.gross_budget);
  hash = fold_double(hash, config.target.max_position_adv_fraction);
  hash = fold_i64(hash, config.target.max_abs_contracts_per_instrument);
  hash = fold_double(hash, config.target.available_initial_margin);
  hash = fold_u64(hash, static_cast<std::uint64_t>(config.target.margin_policy));
  hash = fold_u64(hash, config.orders.first_order_id);
  hash = fold_u64(hash, config.orders.strategy_id);
  hash = fold_u64(hash, config.orders.basket_id);
  hash = fold_u64(hash, config.orders.first_priority_sequence);
  hash = fold_u64(hash, config.orders.fee_schedule_key);
  hash = fold_i64(hash, config.orders.arrival_latency_ns);
  hash = fold_u64(hash, static_cast<std::uint64_t>(config.orders.time_in_force));
  hash = fold_i64(hash, config.orders.expire_ts_ns);
  hash = fold_i64(hash, config.orders.limit_offset_bps.raw());
  hash = fold_i64(hash, config.orders.limit_price_increment.raw());
  hash = fold_u64(hash, config.first_cancel_id);
  hash = fold_i64(hash, config.cancel_latency_ns);
  hash = fold_u64(hash, static_cast<std::uint64_t>(config.reconciliation_scope));
  hash = fold_u64(hash, static_cast<std::uint64_t>(config.missing_signal_policy));
  hash = fold_u64(hash, limits.max_decisions);
  hash = fold_u64(hash, limits.max_commands_per_decision);
  hash = fold_u64(hash, limits.max_workspace_bytes);
  hash = fold_u64(hash, limits.execution.replay.max_contracts);
  hash = fold_u64(hash, limits.execution.replay.max_quote_events);
  hash = fold_u64(hash, limits.execution.replay.max_orders);
  hash = fold_u64(hash, limits.execution.replay.max_cancellations);
  hash = fold_u64(hash, limits.execution.replay.max_fee_rows);
  hash = fold_u64(hash, limits.execution.replay.max_tick_rows);
  hash = fold_u64(hash, limits.execution.replay.max_fills);
  hash = fold_u64(hash, limits.execution.replay.max_workspace_bytes);
  hash = fold_u64(hash, limits.execution.max_frontiers);
  hash = fold_u64(hash, limits.execution.max_transitions);
  hash = fold_u64(hash, limits.execution.max_workspace_bytes);
  hash = fold_i64(hash, inputs.initial_cash.raw());
  hash = fold_u64(hash, inputs.contracts.size());
  for (std::size_t input_index : replay_contract_indices) {
    const auto &contract = inputs.contracts[input_index];
    hash = fold_u64(hash, contract.contract_id);
    hash = fold_u64(hash, contract.engine_id.id);
    hash = fold_i64(hash, contract.multiplier);
    hash = fold_i64(hash, contract.initial_contracts);
    hash = fold_u64(hash, contract.tick_schedule_key);
    hash = fold_i64(hash, contract.definition_effective_ts_ns);
    hash = fold_i64(hash, contract.definition_available_ts_ns);
    hash = fold_i64(hash, contract.expiry_ts_ns);
    hash = fold_identity(hash, contract.definition_source_identity);
  }
  UnorderedFingerprint quote_fingerprint;
  for (const auto &quote : inputs.quotes) {
    std::uint64_t item = fold_u64(kFnvOffset, quote.contract_id);
    item = fold_u64(item, quote.engine_id.id);
    item = fold_i64(item, quote.quote_event_ts_ns);
    item = fold_i64(item, quote.available_ts_ns);
    item = fold_u64(item, quote.order_key.source_rank);
    item = fold_u64(item, quote.order_key.channel_id);
    item = fold_u64(item, quote.order_key.stream_epoch);
    item = fold_u64(item, quote.order_key.native_sequence);
    item = fold_u64(item, quote.order_key.packet_index);
    item = fold_u64(item, quote.order_key.stable_ingest_ordinal);
    item = fold_i64(item, quote.bid.raw());
    item = fold_i64(item, quote.ask.raw());
    item = fold_i64(item, quote.bid_size_contracts);
    item = fold_i64(item, quote.ask_size_contracts);
    item = fold_u64(item, quote.bid_participant_id);
    item = fold_u64(item, quote.ask_participant_id);
    item = fold_u64(item, quote.bid_updated ? 1U : 0U);
    item = fold_u64(item, quote.ask_updated ? 1U : 0U);
    item = fold_u64(item, static_cast<std::uint64_t>(quote.status));
    item = fold_u64(item, quote.cross_stream_ordering_ambiguous ? 1U : 0U);
    item = fold_identity(item, quote.source_identity);
    quote_fingerprint.add(item);
  }
  hash = quote_fingerprint.finish(hash, inputs.quotes.size());
  UnorderedFingerprint fee_fingerprint;
  for (const auto &fee : inputs.fee_schedules) {
    std::uint64_t item = fold_u64(kFnvOffset, fee.key);
    item = fold_i64(item, fee.available_ts_ns);
    item = fold_i64(item, fee.effective_from_ts_ns);
    item = fold_i64(item, fee.effective_until_ts_ns);
    item = fold_i64(item, fee.exchange_per_contract.raw());
    item = fold_i64(item, fee.clearing_per_contract.raw());
    item = fold_i64(item, fee.regulatory_per_contract.raw());
    item = fold_i64(item, fee.commission_per_contract.raw());
    item = fold_i64(item, fee.commission_per_order.raw());
    item = fold_i64(item, fee.sell_premium_rate.raw());
    item = fold_identity(item, fee.source_identity);
    fee_fingerprint.add(item);
  }
  hash = fee_fingerprint.finish(hash, inputs.fee_schedules.size());
  UnorderedFingerprint tick_fingerprint;
  for (const auto &tick : inputs.tick_schedules) {
    std::uint64_t item = fold_u64(kFnvOffset, tick.key);
    item = fold_i64(item, tick.available_ts_ns);
    item = fold_i64(item, tick.effective_from_ts_ns);
    item = fold_i64(item, tick.effective_until_ts_ns);
    item = fold_i64(item, tick.price_threshold.raw());
    item = fold_i64(item, tick.tick_below_threshold.raw());
    item = fold_i64(item, tick.tick_at_or_above_threshold.raw());
    item = fold_identity(item, tick.source_identity);
    tick_fingerprint.add(item);
  }
  hash = tick_fingerprint.finish(hash, inputs.tick_schedules.size());
  for (const auto &instrument : panel.instruments()) {
    hash = fold_u64(hash, instrument.contract_id);
    hash = fold_u64(hash, instrument.underlier_uid);
    hash = fold_i64(hash, instrument.expiry_ts_ns);
    hash = fold_double(hash, instrument.strike);
    hash = fold_u64(hash, static_cast<std::uint64_t>(instrument.side));
    hash = fold_double(hash, instrument.multiplier);
    hash = fold_u64(hash, instrument.standard_deliverable ? 1U : 0U);
    hash = fold_u64(hash, instrument.engine_id.id);
  }
  for (std::int64_t date : panel.dataset().dates()) {
    hash = fold_i64(hash, date);
  }
  for (std::uint8_t field = 0U;
       field <= static_cast<std::uint8_t>(research::OptionPanelField::Status); ++field) {
    for (double value : panel.column(static_cast<research::OptionPanelField>(field))) {
      hash = fold_double(hash, value);
    }
  }
  for (const auto &audit : panel.decision_audit()) {
    hash = fold_i64(hash, audit.decision_ts_ns);
    hash = fold_u64(hash, audit.contract_id);
    hash = fold_i64(hash, audit.observed_ts_ns);
    hash = fold_i64(hash, audit.feature_available_ts_ns);
    hash = fold_i64(hash, audit.definition_available_ts_ns);
    hash = fold_i64(hash, audit.quote_event_ts_ns);
    hash = fold_i64(hash, audit.quote_available_ts_ns);
    hash = fold_u64(hash, static_cast<std::uint64_t>(audit.status));
    hash = fold_identity(hash, audit.definition_source_identity);
    hash = fold_identity(hash, audit.feature_source_identity);
    hash = fold_identity(hash, audit.execution_source_identity);
  }
  return hash;
}

} // namespace

struct OptionAdaptiveCoordinator::Impl {
  Impl(OptionAdaptiveCoordinatorLimits configured_limits,
       execution::OptionExecutionSession &&configured_session) noexcept
      : limits{configured_limits}, session{std::move(configured_session)} {}

  OptionAdaptiveCoordinatorLimits limits{};
  execution::OptionExecutionSession session;
  OptionAdaptiveCoordinatorState phase{OptionAdaptiveCoordinatorState::Empty};
  atx::engine::WeightPolicyScratch weight_scratch;
  std::vector<atx::u32> group_map;
  std::vector<std::int64_t> current_contracts;
  research::OptionTargetBook target_book;
  research::OptionTargetBook command_book;
  std::vector<std::int64_t> leaf_net;
  std::vector<std::size_t> leaf_count;
  std::vector<std::uint8_t> has_buy;
  std::vector<std::uint8_t> has_sell;
  std::vector<std::uint8_t> has_pending_cancel;
  std::vector<std::uint8_t> unsafe;
  std::vector<CancelCandidate> cancel_candidates;
  std::vector<OptionCancelRequest> cancellations;
  std::vector<OptionOrderRequest> orders;
  std::vector<std::size_t> replay_contract_indices;
  std::vector<std::size_t> replay_quote_indices;
  std::vector<std::size_t> latest_quote_indices;
  std::vector<std::size_t> order_contract_indices;
  std::vector<std::size_t> active_order_indices;
  std::vector<OptionAdaptiveDecisionAudit> decisions;
  std::uint64_t run_definition_hash{0};
  std::uint64_t decision_trace_hash{kFnvOffset};

  [[nodiscard]] Result<void>
  validate_and_index_catalog(const research::OptionResearchPanel &panel,
                             const execution::OptionReplayInputs &inputs) {
    const std::size_t count = panel.instruments().size();
    replay_contract_indices.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
      replay_contract_indices[index] = index;
    }
    std::sort(replay_contract_indices.begin(), replay_contract_indices.end(),
              [&inputs](std::size_t left, std::size_t right) noexcept {
                return inputs.contracts[left].contract_id < inputs.contracts[right].contract_id;
              });

    for (std::size_t index = 0; index < count; ++index) {
      const auto &instrument = panel.instruments()[index];
      const auto &contract = inputs.contracts[replay_contract_indices[index]];
      if (contract.contract_id != instrument.contract_id ||
          contract.engine_id != instrument.engine_id || !instrument.standard_deliverable ||
          instrument.multiplier != static_cast<double>(contract.multiplier) ||
          contract.expiry_ts_ns != instrument.expiry_ts_ns) {
        return Err(ErrorCode::InvalidArgument,
                   "adaptive replay definition does not match the research panel");
      }
    }

    replay_quote_indices.resize(inputs.quotes.size());
    for (std::size_t index = 0; index < inputs.quotes.size(); ++index) {
      replay_quote_indices[index] = index;
    }
    std::sort(
        replay_quote_indices.begin(), replay_quote_indices.end(),
        [&inputs](std::size_t left, std::size_t right) noexcept {
          const auto &left_quote = inputs.quotes[left];
          const auto &right_quote = inputs.quotes[right];
          return std::tie(left_quote.available_ts_ns, left_quote.order_key.source_rank,
                          left_quote.order_key.channel_id, left_quote.order_key.stream_epoch,
                          left_quote.order_key.native_sequence, left_quote.order_key.packet_index,
                          left_quote.order_key.stable_ingest_ordinal) <
                 std::tie(right_quote.available_ts_ns, right_quote.order_key.source_rank,
                          right_quote.order_key.channel_id, right_quote.order_key.stream_epoch,
                          right_quote.order_key.native_sequence, right_quote.order_key.packet_index,
                          right_quote.order_key.stable_ingest_ordinal);
        });

    for (const auto &audit : panel.decision_audit()) {
      const auto found = std::lower_bound(
          panel.instruments().begin(), panel.instruments().end(), audit.contract_id,
          [](const research::OptionInstrument &instrument, std::uint64_t contract_id) noexcept {
            return instrument.contract_id < contract_id;
          });
      if (found == panel.instruments().end() || found->contract_id != audit.contract_id) {
        return Err(ErrorCode::Internal, "adaptive panel audit contract lookup failed");
      }
      const std::size_t panel_index = static_cast<std::size_t>(found - panel.instruments().begin());
      const auto &contract = inputs.contracts[replay_contract_indices[panel_index]];
      if (audit.definition_source_identity != contract.definition_source_identity ||
          contract.definition_available_ts_ns > audit.decision_ts_ns) {
        return Err(ErrorCode::InvalidArgument,
                   "adaptive panel and replay evidence lineage do not match");
      }
    }

    const auto bid = panel.column(research::OptionPanelField::Bid);
    const auto ask = panel.column(research::OptionPanelField::Ask);
    const auto bid_size = panel.column(research::OptionPanelField::BidSizeContracts);
    const auto ask_size = panel.column(research::OptionPanelField::AskSizeContracts);
    constexpr std::size_t kNoQuote = (std::numeric_limits<std::size_t>::max)();
    latest_quote_indices.assign(count, kNoQuote);
    std::size_t quote_cursor = 0U;
    for (std::size_t date_index = 0; date_index < panel.dataset().num_dates(); ++date_index) {
      const std::int64_t decision_ts_ns = panel.dataset().dates()[date_index];
      while (quote_cursor < replay_quote_indices.size() &&
             inputs.quotes[replay_quote_indices[quote_cursor]].available_ts_ns <= decision_ts_ns) {
        const std::size_t quote_index = replay_quote_indices[quote_cursor];
        const auto &quote = inputs.quotes[quote_index];
        const auto instrument = std::lower_bound(
            panel.instruments().begin(), panel.instruments().end(), quote.contract_id,
            [](const research::OptionInstrument &candidate, std::uint64_t contract_id) noexcept {
              return candidate.contract_id < contract_id;
            });
        if (instrument == panel.instruments().end() ||
            instrument->contract_id != quote.contract_id ||
            instrument->engine_id != quote.engine_id) {
          return Err(ErrorCode::InvalidArgument,
                     "adaptive replay quote does not match the contract catalog");
        }
        latest_quote_indices[static_cast<std::size_t>(instrument - panel.instruments().begin())] =
            quote_index;
        ++quote_cursor;
      }

      for (std::size_t instrument_index = 0; instrument_index < count; ++instrument_index) {
        if (!panel.tradable(date_index, instrument_index)) {
          continue;
        }
        const std::uint64_t contract_id = panel.instruments()[instrument_index].contract_id;
        const auto audit =
            std::lower_bound(panel.decision_audit().begin(), panel.decision_audit().end(),
                             std::pair{decision_ts_ns, contract_id},
                             [](const research::OptionDecisionAudit &candidate,
                                const std::pair<std::int64_t, std::uint64_t> &key) noexcept {
                               return std::tie(candidate.decision_ts_ns, candidate.contract_id) <
                                      std::tie(key.first, key.second);
                             });
        if (audit == panel.decision_audit().end() || audit->decision_ts_ns != decision_ts_ns ||
            audit->contract_id != contract_id) {
          return Err(ErrorCode::Internal, "adaptive tradable panel audit row is missing");
        }
        const std::size_t quote_index = latest_quote_indices[instrument_index];
        if (quote_index == kNoQuote) {
          return Err(ErrorCode::InvalidArgument,
                     "adaptive decision has no current replay quote evidence");
        }
        const auto &current_quote = inputs.quotes[quote_index];
        const std::size_t cell = date_index * count + instrument_index;
        ATX_TRY(atx::core::Decimal panel_bid, atx::core::Decimal::from_double(bid[cell]));
        ATX_TRY(atx::core::Decimal panel_ask, atx::core::Decimal::from_double(ask[cell]));
        if (current_quote.quote_event_ts_ns != audit->quote_event_ts_ns ||
            current_quote.available_ts_ns != audit->quote_available_ts_ns ||
            current_quote.bid != panel_bid || current_quote.ask != panel_ask ||
            static_cast<double>(current_quote.bid_size_contracts) != bid_size[cell] ||
            static_cast<double>(current_quote.ask_size_contracts) != ask_size[cell] ||
            current_quote.status != execution::OptionQuoteStatus::Firm ||
            current_quote.source_identity != audit->execution_source_identity) {
          return Err(ErrorCode::InvalidArgument,
                     "adaptive decision BBO is not the current replay state");
        }
      }
    }
    return Ok();
  }

  [[nodiscard]] Result<void> refresh_active_orders(const OptionExecutionFrontierView &frontier) {
    std::size_t write_index = 0U;
    for (std::size_t order_index : active_order_indices) {
      if (order_index >= frontier.order_states.size()) {
        return Err(ErrorCode::Internal, "adaptive active-order index is out of range");
      }
      if (active(frontier.order_states[order_index].state)) {
        active_order_indices[write_index] = order_index;
        ++write_index;
      }
    }
    active_order_indices.resize(write_index);
    return Ok();
  }

  [[nodiscard]] Result<void> align_frontier(const research::OptionResearchPanel &panel,
                                            const OptionExecutionFrontierView &frontier) {
    const std::size_t count = panel.instruments().size();
    if (frontier.exposures.size() != count || frontier.positions.size() != count ||
        frontier.orders.size() != frontier.order_states.size() ||
        frontier.orders.size() != order_contract_indices.size()) {
      return Err(ErrorCode::InvalidArgument,
                 "adaptive frontier does not align with the option catalog");
    }
    current_contracts.resize(count);
    for (std::size_t index = 0; index < count; ++index) {
      const auto &instrument = panel.instruments()[index];
      const auto &exposure = frontier.exposures[index];
      const auto &position = frontier.positions[index];
      if (instrument.contract_id != exposure.contract_id ||
          instrument.engine_id != exposure.engine_id ||
          instrument.contract_id != position.contract_id ||
          instrument.engine_id != position.engine_id ||
          exposure.position_contracts != position.contracts) {
        return Err(ErrorCode::InvalidArgument, "adaptive frontier contract identity is misaligned");
      }
      current_contracts[index] = exposure.position_contracts;
    }
    for (std::size_t index : active_order_indices) {
      if (frontier.orders[index].request.order_id != frontier.order_states[index].order_id ||
          order_contract_indices[index] >= count) {
        return Err(ErrorCode::InvalidArgument, "adaptive frontier order state is misaligned");
      }
      const std::size_t contract_index = order_contract_indices[index];
      if (frontier.order_states[index].contract_id !=
              panel.instruments()[contract_index].contract_id ||
          frontier.order_states[index].engine_id != panel.instruments()[contract_index].engine_id) {
        return Err(ErrorCode::InvalidArgument, "adaptive frontier order contract is misaligned");
      }
    }
    return Ok();
  }

  [[nodiscard]] Result<void> build_cancellations(const OptionExecutionFrontierView &frontier,
                                                 const OptionAdaptiveCoordinatorConfig &config,
                                                 std::uint64_t first_cancel_id,
                                                 std::uint64_t first_priority_sequence) {
    cancellations.clear();
    std::sort(cancel_candidates.begin(), cancel_candidates.end(),
              [&frontier](const CancelCandidate &left, const CancelCandidate &right) {
                return std::tie(left.available_ts_ns,
                                frontier.orders[left.order_index].request.order_id.value) <
                       std::tie(right.available_ts_ns,
                                frontier.orders[right.order_index].request.order_id.value);
              });
    if (cancel_candidates.size() > limits.max_commands_per_decision) {
      return Err(ErrorCode::OutOfRange, "adaptive cancellation capacity is exhausted");
    }
    if (!cancel_candidates.empty()) {
      const std::uint64_t offset = static_cast<std::uint64_t>(cancel_candidates.size() - 1U);
      if (first_cancel_id > (std::numeric_limits<std::uint64_t>::max)() - offset ||
          first_priority_sequence > (std::numeric_limits<std::uint64_t>::max)() - offset) {
        return Err(ErrorCode::OutOfRange, "adaptive cancellation identifiers exceed uint64 range");
      }
    }
    for (std::size_t ordinal = 0; ordinal < cancel_candidates.size(); ++ordinal) {
      const CancelCandidate &candidate = cancel_candidates[ordinal];
      cancellations.push_back(OptionCancelRequest{
          OptionCancelId{first_cancel_id + ordinal},
          frontier.orders[candidate.order_index].request.order_id, frontier.frontier_ts_ns,
          candidate.available_ts_ns, first_priority_sequence + ordinal});
    }
    (void)config;
    return Ok();
  }

  [[nodiscard]] Result<OptionAdaptiveDecisionAudit>
  plan_decision(const research::OptionResearchPanel &panel, std::size_t date_index,
                const OptionExecutionFrontierView &frontier,
                const execution::OptionReplayConfig &replay_config,
                const OptionAdaptiveCoordinatorConfig &config, std::uint64_t next_order_id,
                std::uint64_t next_cancel_id, std::uint64_t next_priority_sequence) {
    ATX_TRY_VOID(refresh_active_orders(frontier));
    ATX_TRY_VOID(align_frontier(panel, frontier));
    const std::size_t count = panel.instruments().size();
    const std::span<const double> signal =
        panel.row(research::OptionPanelField::Signal, date_index);
    const std::span<const atx::u32> groups = config.weight_policy.industry_neutral
                                                 ? std::span<const atx::u32>{group_map}
                                                 : std::span<const atx::u32>{};
    config.weight_policy.to_target_weights(atx::engine::SignalView{signal}, panel.universe(),
                                           weight_scratch, groups);
    ATX_TRY_VOID(research::make_option_target_book_into(
        panel, date_index, weight_scratch.weights, current_contracts, config.target, target_book));

    command_book.decision_ts_ns = target_book.decision_ts_ns;
    command_book.targets.assign(target_book.targets.begin(), target_book.targets.end());
    command_book.requested_gross_exposure = target_book.requested_gross_exposure;
    command_book.realized_gross_exposure = target_book.realized_gross_exposure;
    command_book.initial_margin = target_book.initial_margin;
    command_book.maintenance_margin = target_book.maintenance_margin;
    command_book.margin_clamped = target_book.margin_clamped;

    std::size_t missing_signal_count = 0U;
    for (std::size_t index = 0; index < count; ++index) {
      const bool missing = !std::isfinite(signal[index]) || !panel.tradable(date_index, index);
      if (!missing) {
        continue;
      }
      ++missing_signal_count;
      if (config.missing_signal_policy == OptionMissingSignalPolicy::RejectDecision) {
        return Err(ErrorCode::Unavailable,
                   "adaptive decision contains a missing or non-tradable signal");
      }
      if (config.missing_signal_policy == OptionMissingSignalPolicy::HoldPositionAndReduceRisk) {
        command_book.targets[index].target_contracts = current_contracts[index];
        command_book.targets[index].order_contracts = 0;
      }
    }
    const bool missing_hold_decision =
        missing_signal_count != 0U &&
        config.missing_signal_policy == OptionMissingSignalPolicy::HoldPositionAndReduceRisk;

    leaf_net.assign(count, 0);
    leaf_count.assign(count, 0U);
    has_buy.assign(count, 0U);
    has_sell.assign(count, 0U);
    has_pending_cancel.assign(count, 0U);
    unsafe.assign(count, 0U);
    cancel_candidates.clear();

    std::size_t active_leaf_count = 0U;
    for (std::size_t order_index : active_order_indices) {
      const auto &state = frontier.order_states[order_index];
      const std::size_t contract_index = order_contract_indices[order_index];
      ATX_TRY(leaf_net[contract_index],
              atx::core::checked_add(leaf_net[contract_index], state.remaining_contracts));
      ++leaf_count[contract_index];
      ++active_leaf_count;
      has_buy[contract_index] |= state.remaining_contracts > 0 ? 1U : 0U;
      has_sell[contract_index] |= state.remaining_contracts < 0 ? 1U : 0U;
      has_pending_cancel[contract_index] |= state.cancel_pending ? 1U : 0U;
    }

    bool basket_barrier = missing_hold_decision;
    std::size_t retained_leaf_count = 0U;
    std::size_t deferred_contract_count = 0U;
    for (std::size_t index = 0; index < count; ++index) {
      ATX_TRY(std::int64_t desired,
              atx::core::checked_sub(command_book.targets[index].target_contracts,
                                     current_contracts[index]));
      const bool mixed = has_buy[index] != 0U && has_sell[index] != 0U;
      const bool missing_cell =
          config.missing_signal_policy == OptionMissingSignalPolicy::HoldPositionAndReduceRisk &&
          (!std::isfinite(signal[index]) || !panel.tradable(date_index, index));
      bool safe = !mixed;
      if (missing_hold_decision && current_contracts[index] > 0) {
        safe = safe && has_buy[index] == 0U && leaf_net[index] <= 0 &&
               leaf_net[index] >= -current_contracts[index];
      } else if (missing_hold_decision && current_contracts[index] < 0) {
        ATX_TRY(std::int64_t max_reducing_buy,
                atx::core::checked_sub(std::int64_t{0}, current_contracts[index]));
        safe = safe && has_sell[index] == 0U && leaf_net[index] >= 0 &&
               leaf_net[index] <= max_reducing_buy;
      } else if (missing_hold_decision) {
        safe = safe && leaf_count[index] == 0U;
      } else if (desired > 0) {
        safe = safe && has_sell[index] == 0U && leaf_net[index] >= 0 && leaf_net[index] <= desired;
      } else if (desired < 0) {
        safe = safe && has_buy[index] == 0U && leaf_net[index] <= 0 && leaf_net[index] >= desired;
      } else {
        safe = safe && leaf_count[index] == 0U;
      }
      unsafe[index] = safe ? 0U : 1U;
      if (!safe || has_pending_cancel[index] != 0U || missing_cell) {
        ++deferred_contract_count;
        basket_barrier = true;
      }
      if (safe && has_pending_cancel[index] == 0U) {
        retained_leaf_count += leaf_count[index];
      }
    }

    if (config.reconciliation_scope == OptionReconciliationScope::WholeBasketCancelBarrier &&
        basket_barrier) {
      retained_leaf_count = 0U;
      for (std::size_t index = 0; index < count; ++index) {
        if (leaf_count[index] != 0U && unsafe[index] == 0U) {
          unsafe[index] = 1U;
          ++deferred_contract_count;
        }
      }
    }

    std::int64_t base_cancel_available = 0;
    bool base_cancel_available_computed = false;
    for (std::size_t order_index : active_order_indices) {
      const auto &state = frontier.order_states[order_index];
      if (state.cancel_pending || unsafe[order_contract_indices[order_index]] == 0U) {
        continue;
      }
      if (!base_cancel_available_computed) {
        ATX_TRY(base_cancel_available,
                atx::core::checked_add(frontier.frontier_ts_ns, config.cancel_latency_ns));
        base_cancel_available_computed = true;
      }
      ATX_TRY(std::int64_t after_arrival,
              atx::core::checked_add(frontier.orders[order_index].request.arrival_ts_ns,
                                     std::int64_t{1}));
      const std::int64_t available = (std::max)(base_cancel_available, after_arrival);
      if (available > replay_config.replay_end_ts_ns) {
        return Err(ErrorCode::OutOfRange, "adaptive cancellation falls after replay end");
      }
      if (cancel_candidates.size() >= limits.max_commands_per_decision) {
        return Err(ErrorCode::OutOfRange, "adaptive cancellation capacity is exhausted");
      }
      cancel_candidates.push_back(CancelCandidate{order_index, available});
    }
    ATX_TRY_VOID(build_cancellations(frontier, config, next_cancel_id, next_priority_sequence));

    const bool whole_basket_blocked =
        config.reconciliation_scope == OptionReconciliationScope::WholeBasketCancelBarrier &&
        basket_barrier;
    for (std::size_t index = 0; index < count; ++index) {
      command_book.targets[index].order_contracts = 0;
      const bool contract_blocked = unsafe[index] != 0U || has_pending_cancel[index] != 0U;
      if (missing_hold_decision || whole_basket_blocked || contract_blocked) {
        continue;
      }
      ATX_TRY(std::int64_t desired,
              atx::core::checked_sub(command_book.targets[index].target_contracts,
                                     current_contracts[index]));
      ATX_TRY(command_book.targets[index].order_contracts,
              atx::core::checked_sub(desired, leaf_net[index]));
    }

    if (cancellations.size() >
        (std::numeric_limits<std::uint64_t>::max)() - next_priority_sequence) {
      return Err(ErrorCode::OutOfRange, "adaptive priority sequence exceeds uint64 range");
    }
    const bool has_order_intent =
        std::any_of(command_book.targets.begin(), command_book.targets.end(),
                    [](const research::OptionContractTarget &target) noexcept {
                      return target.order_contracts != 0;
                    });
    orders.clear();
    if (has_order_intent) {
      execution::OptionOrderBatchSpec order_spec = config.orders;
      order_spec.first_order_id = next_order_id;
      order_spec.first_priority_sequence =
          next_priority_sequence + static_cast<std::uint64_t>(cancellations.size());
      if (date_index > (std::numeric_limits<std::uint64_t>::max)() - order_spec.basket_id) {
        return Err(ErrorCode::OutOfRange, "adaptive basket identifier exceeds uint64 range");
      }
      order_spec.basket_id += static_cast<std::uint64_t>(date_index);
      ATX_TRY_VOID(execution::make_option_order_batch_into(panel, date_index, command_book,
                                                           order_spec, orders));
    }
    ATX_TRY(std::size_t command_count, checked_add_size(orders.size(), cancellations.size()));
    if (command_count > limits.max_commands_per_decision) {
      return Err(ErrorCode::OutOfRange, "adaptive total command capacity is exhausted");
    }

    OptionAdaptiveDecisionAudit audit;
    audit.date_index = date_index;
    audit.decision_ts_ns = frontier.frontier_ts_ns;
    audit.signal_hash = hash_signal(signal);
    audit.input_state_hash = hash_input_state(frontier, active_order_indices);
    audit.target_hash = fold_u64(hash_targets(target_book), hash_targets(command_book));
    audit.command_hash = hash_commands(orders, cancellations);
    audit.session_trace_hash_before = frontier.session_summary.command_trace_hash;
    audit.requested_gross_exposure = target_book.requested_gross_exposure;
    audit.realized_gross_exposure = target_book.realized_gross_exposure;
    audit.initial_margin = target_book.initial_margin;
    audit.maintenance_margin = target_book.maintenance_margin;
    audit.active_leaf_count = active_leaf_count;
    audit.retained_leaf_count = retained_leaf_count;
    audit.deferred_contract_count = deferred_contract_count;
    audit.submitted_order_count = orders.size();
    audit.cancellation_count = cancellations.size();
    audit.missing_signal_count = missing_signal_count;
    audit.margin_clamped = target_book.margin_clamped;
    audit.cancel_barrier = basket_barrier;
    return Ok(audit);
  }
};

Result<std::size_t> option_adaptive_coordinator_required_workspace_bytes(
    const OptionAdaptiveCoordinatorLimits &limits) {
  if (limits.execution.replay.max_contracts == 0U || limits.execution.replay.max_orders == 0U ||
      limits.max_decisions == 0U || limits.max_commands_per_decision == 0U) {
    return Err(ErrorCode::InvalidArgument, "adaptive coordinator limits must be positive");
  }
  std::size_t bytes = 0U;
  const auto add_array = [&bytes](std::size_t count, std::size_t element_size) -> Result<void> {
    ATX_TRY(std::size_t block, checked_mul_size(count, element_size));
    ATX_TRY(bytes, checked_add_size(bytes, block));
    return Ok();
  };
  const std::size_t contracts = limits.execution.replay.max_contracts;
  ATX_TRY_VOID(add_array(contracts, sizeof(double) * 4U));
  ATX_TRY_VOID(add_array(contracts, sizeof(std::size_t) * 2U));
  ATX_TRY_VOID(add_array(contracts, sizeof(atx::u32)));
  ATX_TRY_VOID(add_array(contracts, sizeof(atx::u32)));
  ATX_TRY_VOID(add_array(contracts, sizeof(std::int64_t)));
  ATX_TRY_VOID(add_array(contracts, sizeof(research::OptionContractTarget) * 2U));
  ATX_TRY_VOID(add_array(contracts, sizeof(std::int64_t)));
  ATX_TRY_VOID(add_array(contracts, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(contracts, sizeof(std::uint8_t) * 4U));
  ATX_TRY_VOID(add_array(limits.max_commands_per_decision, sizeof(CancelCandidate)));
  ATX_TRY_VOID(add_array(limits.max_commands_per_decision, sizeof(OptionCancelRequest)));
  ATX_TRY_VOID(add_array(limits.max_commands_per_decision, sizeof(OptionOrderRequest)));
  ATX_TRY_VOID(add_array(contracts, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(limits.execution.replay.max_quote_events, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(contracts, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(limits.execution.replay.max_orders, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(limits.execution.replay.max_orders, sizeof(std::size_t)));
  ATX_TRY_VOID(add_array(limits.max_decisions, sizeof(OptionAdaptiveDecisionAudit)));
  return Ok(bytes);
}

Result<OptionAdaptiveCoordinator>
OptionAdaptiveCoordinator::create(OptionAdaptiveCoordinatorLimits limits) {
  ATX_TRY(std::size_t required, option_adaptive_coordinator_required_workspace_bytes(limits));
  if (required > limits.max_workspace_bytes ||
      limits.max_commands_per_decision > limits.execution.replay.max_orders ||
      limits.max_commands_per_decision > limits.execution.replay.max_cancellations ||
      limits.max_decisions > limits.execution.max_frontiers) {
    return Err(ErrorCode::OutOfRange,
               "adaptive coordinator workspace or command limits are invalid");
  }
  ATX_TRY(execution::OptionExecutionSession session,
          execution::OptionExecutionSession::create(limits.execution));
  try {
    auto impl = std::make_unique<Impl>(limits, std::move(session));
    const std::size_t contracts = limits.execution.replay.max_contracts;
    impl->weight_scratch.reserve(contracts);
    impl->group_map.reserve(contracts);
    impl->current_contracts.reserve(contracts);
    impl->target_book.targets.reserve(contracts);
    impl->command_book.targets.reserve(contracts);
    impl->leaf_net.reserve(contracts);
    impl->leaf_count.reserve(contracts);
    impl->has_buy.reserve(contracts);
    impl->has_sell.reserve(contracts);
    impl->has_pending_cancel.reserve(contracts);
    impl->unsafe.reserve(contracts);
    impl->cancel_candidates.reserve(limits.max_commands_per_decision);
    impl->cancellations.reserve(limits.max_commands_per_decision);
    impl->orders.reserve(limits.max_commands_per_decision);
    impl->replay_contract_indices.reserve(contracts);
    impl->replay_quote_indices.reserve(limits.execution.replay.max_quote_events);
    impl->latest_quote_indices.reserve(contracts);
    impl->order_contract_indices.reserve(limits.execution.replay.max_orders);
    impl->active_order_indices.reserve(limits.execution.replay.max_orders);
    impl->decisions.reserve(limits.max_decisions);
    return Ok(OptionAdaptiveCoordinator{std::move(impl)});
  } catch (const std::bad_alloc &) {
    return Err(ErrorCode::OutOfRange, "adaptive coordinator allocation failed");
  } catch (const std::length_error &) {
    return Err(ErrorCode::OutOfRange, "adaptive coordinator capacity exceeds vector limits");
  }
}

OptionAdaptiveCoordinator::OptionAdaptiveCoordinator(std::unique_ptr<Impl> impl) noexcept
    : impl_{std::move(impl)} {}

OptionAdaptiveCoordinator::~OptionAdaptiveCoordinator() = default;
OptionAdaptiveCoordinator::OptionAdaptiveCoordinator(OptionAdaptiveCoordinator &&) noexcept =
    default;
OptionAdaptiveCoordinator &
OptionAdaptiveCoordinator::operator=(OptionAdaptiveCoordinator &&) noexcept = default;

Result<OptionAdaptiveRunView>
OptionAdaptiveCoordinator::run(const research::OptionResearchPanel &panel,
                               const execution::OptionReplayInputs &inputs,
                               const execution::OptionReplayConfig &replay_config,
                               const OptionAdaptiveCoordinatorConfig &config) {
  if (impl_ == nullptr) {
    return Err(ErrorCode::Internal, "adaptive coordinator has no implementation");
  }
  Impl &state = *impl_;
  if (state.phase == OptionAdaptiveCoordinatorState::Running ||
      state.phase == OptionAdaptiveCoordinatorState::Failed) {
    return Err(ErrorCode::InvalidArgument, "adaptive coordinator state does not permit a run");
  }
  ATX_TRY_VOID(validate_config(config));
  const std::size_t contract_count = panel.instruments().size();
  const std::size_t decision_count = panel.dataset().num_dates();
  if (contract_count == 0U || contract_count > state.limits.execution.replay.max_contracts ||
      decision_count == 0U || decision_count > state.limits.max_decisions ||
      inputs.contracts.size() != contract_count || !inputs.orders.empty() ||
      !inputs.cancellations.empty() ||
      inputs.quotes.size() > state.limits.execution.replay.max_quote_events ||
      inputs.fee_schedules.size() > state.limits.execution.replay.max_fee_rows ||
      inputs.tick_schedules.size() > state.limits.execution.replay.max_tick_rows ||
      replay_config.replay_end_ts_ns < panel.dataset().dates().back()) {
    return Err(ErrorCode::InvalidArgument, "adaptive run inputs exceed limits or are misaligned");
  }
  ATX_TRY_VOID(state.validate_and_index_catalog(panel, inputs));

  state.phase = OptionAdaptiveCoordinatorState::Running;
  state.decisions.clear();
  state.order_contract_indices.clear();
  state.active_order_indices.clear();
  state.group_map.clear();
  for (const auto &instrument : panel.instruments()) {
    state.group_map.push_back(instrument.underlier_uid);
  }
  state.run_definition_hash = hash_run_definition(panel, inputs, replay_config, config,
                                                  state.limits, state.replay_contract_indices);
  state.decision_trace_hash = kFnvOffset;

  const auto started = state.session.start(inputs, replay_config);
  if (!started) {
    state.phase = OptionAdaptiveCoordinatorState::Failed;
    return tl::unexpected<atx::core::Error>(started.error());
  }

  std::uint64_t next_order_id = config.orders.first_order_id;
  std::uint64_t next_cancel_id = config.first_cancel_id;
  std::uint64_t next_priority_sequence = config.orders.first_priority_sequence;
  for (std::size_t date_index = 0; date_index < decision_count; ++date_index) {
    const std::int64_t decision_ts_ns = panel.dataset().dates()[date_index];
    auto frontier = state.session.advance_to(decision_ts_ns);
    if (!frontier) {
      state.phase = OptionAdaptiveCoordinatorState::Failed;
      return tl::unexpected<atx::core::Error>(std::move(frontier).error());
    }
    auto audit = state.plan_decision(panel, date_index, *frontier, replay_config, config,
                                     next_order_id, next_cancel_id, next_priority_sequence);
    if (!audit) {
      state.phase = OptionAdaptiveCoordinatorState::Failed;
      return tl::unexpected<atx::core::Error>(std::move(audit).error());
    }
    auto command_count_result = checked_add_size(state.orders.size(), state.cancellations.size());
    if (!command_count_result) {
      state.phase = OptionAdaptiveCoordinatorState::Failed;
      return tl::unexpected<atx::core::Error>(std::move(command_count_result).error());
    }
    const std::size_t command_count = *command_count_result;
    if (state.orders.size() > (std::numeric_limits<std::uint64_t>::max)() - next_order_id ||
        state.cancellations.size() > (std::numeric_limits<std::uint64_t>::max)() - next_cancel_id ||
        command_count > (std::numeric_limits<std::uint64_t>::max)() - next_priority_sequence) {
      state.phase = OptionAdaptiveCoordinatorState::Failed;
      return Err(ErrorCode::OutOfRange, "adaptive identifier cursor exceeds uint64 range");
    }

    const std::size_t old_mapping_size = state.order_contract_indices.size();
    const std::size_t old_active_size = state.active_order_indices.size();
    auto applied = state.session.apply_commands(
        execution::OptionCommandBatch{state.orders, state.cancellations});
    if (!applied) {
      state.phase = OptionAdaptiveCoordinatorState::Failed;
      return tl::unexpected<atx::core::Error>(std::move(applied).error());
    }
    for (const auto &order : state.orders) {
      const auto found = std::lower_bound(
          panel.instruments().begin(), panel.instruments().end(), order.contract_id,
          [](const research::OptionInstrument &instrument, std::uint64_t contract_id) {
            return instrument.contract_id < contract_id;
          });
      if (found == panel.instruments().end() || found->contract_id != order.contract_id) {
        state.order_contract_indices.resize(old_mapping_size);
        state.active_order_indices.resize(old_active_size);
        state.phase = OptionAdaptiveCoordinatorState::Failed;
        return Err(ErrorCode::Internal, "adaptive command contract lookup failed");
      }
      state.order_contract_indices.push_back(
          static_cast<std::size_t>(found - panel.instruments().begin()));
      state.active_order_indices.push_back(state.order_contract_indices.size() - 1U);
    }

    next_order_id += static_cast<std::uint64_t>(state.orders.size());
    next_cancel_id += static_cast<std::uint64_t>(state.cancellations.size());
    next_priority_sequence += static_cast<std::uint64_t>(command_count);
    std::uint64_t decision_hash = fold_u64(kFnvOffset, audit->date_index);
    decision_hash = fold_i64(decision_hash, audit->decision_ts_ns);
    decision_hash = fold_u64(decision_hash, audit->signal_hash);
    decision_hash = fold_u64(decision_hash, audit->input_state_hash);
    decision_hash = fold_u64(decision_hash, audit->target_hash);
    decision_hash = fold_u64(decision_hash, audit->command_hash);
    decision_hash = fold_u64(decision_hash, audit->session_trace_hash_before);
    decision_hash = fold_double(decision_hash, audit->requested_gross_exposure);
    decision_hash = fold_double(decision_hash, audit->realized_gross_exposure);
    decision_hash = fold_double(decision_hash, audit->initial_margin);
    decision_hash = fold_double(decision_hash, audit->maintenance_margin);
    decision_hash = fold_u64(decision_hash, audit->active_leaf_count);
    decision_hash = fold_u64(decision_hash, audit->retained_leaf_count);
    decision_hash = fold_u64(decision_hash, audit->deferred_contract_count);
    decision_hash = fold_u64(decision_hash, audit->submitted_order_count);
    decision_hash = fold_u64(decision_hash, audit->cancellation_count);
    decision_hash = fold_u64(decision_hash, audit->missing_signal_count);
    decision_hash = fold_u64(decision_hash, audit->margin_clamped ? 1U : 0U);
    decision_hash = fold_u64(decision_hash, audit->cancel_barrier ? 1U : 0U);
    state.decision_trace_hash = fold_u64(state.decision_trace_hash, decision_hash);
    state.decisions.push_back(*audit);
  }

  auto finished = state.session.finish();
  if (!finished) {
    state.phase = OptionAdaptiveCoordinatorState::Failed;
    return tl::unexpected<atx::core::Error>(std::move(finished).error());
  }
  state.phase = OptionAdaptiveCoordinatorState::Finished;
  return Ok(OptionAdaptiveRunView{*finished, state.decisions,
                                  kOptionAdaptiveCoordinatorModelVersion,
                                  kOptionAdaptiveCoordinatorOrderingVersion,
                                  state.run_definition_hash, state.decision_trace_hash});
}

OptionAdaptiveCoordinatorState OptionAdaptiveCoordinator::state() const noexcept {
  return impl_ == nullptr ? OptionAdaptiveCoordinatorState::Failed : impl_->phase;
}

} // namespace atx::options::adaptive
