#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "atx/core/decimal.hpp"
#include "atx/core/error.hpp"
#include "atx/options/option_execution_replay.hpp"

namespace option_replay_alloc {
extern std::atomic<std::size_t> g_count;
extern std::atomic<bool> g_armed;
} // namespace option_replay_alloc

namespace {

using atx::core::Decimal;
using atx::core::ErrorCode;
using atx::options::execution::option_execution_session_required_workspace_bytes;
using atx::options::execution::OptionCancelDisposition;
using atx::options::execution::OptionCancelId;
using atx::options::execution::OptionCancelRequest;
using atx::options::execution::OptionCommandBatch;
using atx::options::execution::OptionExecutionReplay;
using atx::options::execution::OptionExecutionSession;
using atx::options::execution::OptionExecutionSessionLimits;
using atx::options::execution::OptionExecutionSessionState;
using atx::options::execution::OptionFeeSchedule;
using atx::options::execution::OptionMarketOrderKey;
using atx::options::execution::OptionOrderId;
using atx::options::execution::OptionOrderLifecycleState;
using atx::options::execution::OptionOrderRequest;
using atx::options::execution::OptionOrderTransitionKind;
using atx::options::execution::OptionReplayConfig;
using atx::options::execution::OptionReplayContract;
using atx::options::execution::OptionReplayInputs;
using atx::options::execution::OptionReplayLimits;
using atx::options::execution::OptionReplayScenario;
using atx::options::execution::OptionTickSchedule;
using atx::options::execution::OptionTimeInForce;
using atx::options::execution::OptionTopOfBookEvent;
using atx::vol::ArchiveContentIdentity;

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

[[nodiscard]] OptionExecutionSessionLimits session_limits() {
  OptionExecutionSessionLimits out;
  out.replay.max_contracts = 4U;
  out.replay.max_quote_events = 32U;
  out.replay.max_orders = 32U;
  out.replay.max_cancellations = 16U;
  out.replay.max_fee_rows = 4U;
  out.replay.max_tick_rows = 4U;
  out.replay.max_fills = 64U;
  out.replay.max_workspace_bytes = 4U * 1024U * 1024U;
  out.max_frontiers = 32U;
  out.max_transitions = 256U;
  out.max_workspace_bytes = 8U * 1024U * 1024U;
  return out;
}

[[nodiscard]] OptionReplayConfig config(std::int64_t replay_end_ts_ns = 1'000) {
  OptionReplayConfig out;
  out.market_data_identity = identity(900U);
  out.sequence_validation_identity = identity(901U);
  out.sequence_continuity_verified = true;
  out.scenario = OptionReplayScenario::Calibrated;
  out.calibration_identity = identity(902U);
  out.displayed_size_fraction = Decimal::from_int(1);
  out.max_quote_age_ns = 1'000;
  out.replay_end_ts_ns = replay_end_ts_ns;
  return out;
}

[[nodiscard]] OptionReplayContract contract(std::int64_t initial_contracts = 0) {
  OptionReplayContract out;
  out.contract_id = 10U;
  out.engine_id.id = 1U;
  out.multiplier = 100;
  out.initial_contracts = initial_contracts;
  out.tick_schedule_key = 1U;
  out.definition_effective_ts_ns = 1;
  out.definition_available_ts_ns = 2;
  out.expiry_ts_ns = 2'000;
  out.definition_source_identity = identity(500U);
  return out;
}

[[nodiscard]] OptionTopOfBookEvent quote(std::int64_t available_ts_ns, std::uint64_t sequence,
                                         std::int64_t ask_size = 10, std::int64_t bid_size = 10) {
  OptionTopOfBookEvent out;
  out.contract_id = 10U;
  out.engine_id.id = 1U;
  out.quote_event_ts_ns = available_ts_ns - 1;
  out.available_ts_ns = available_ts_ns;
  out.order_key = OptionMarketOrderKey{1U, 2U, 20'260'726U, sequence, 0U, sequence};
  out.bid = money("9.00");
  out.ask = money("11.00");
  out.bid_size_contracts = bid_size;
  out.ask_size_contracts = ask_size;
  out.bid_participant_id = 7U;
  out.ask_participant_id = 8U;
  out.source_identity = identity(sequence);
  return out;
}

[[nodiscard]] OptionOrderRequest order(std::uint64_t id, std::int64_t quantity,
                                       std::int64_t decision_ts_ns, std::int64_t arrival_ts_ns) {
  OptionOrderRequest out;
  out.order_id = OptionOrderId{id};
  out.strategy_id = 77U;
  out.basket_id = 88U;
  out.contract_id = 10U;
  out.engine_id.id = 1U;
  out.quantity_contracts = quantity;
  out.limit_price = quantity > 0 ? money("11.00") : money("9.00");
  out.decision_ts_ns = decision_ts_ns;
  out.arrival_ts_ns = arrival_ts_ns;
  out.priority_sequence = id;
  out.fee_schedule_key = 1U;
  out.time_in_force = OptionTimeInForce::GoodTillCanceled;
  return out;
}

[[nodiscard]] OptionCancelRequest cancel(std::uint64_t cancel_id, std::uint64_t order_id,
                                         std::int64_t decision_ts_ns,
                                         std::int64_t available_ts_ns) {
  return OptionCancelRequest{OptionCancelId{cancel_id}, OptionOrderId{order_id}, decision_ts_ns,
                             available_ts_ns, cancel_id};
}

[[nodiscard]] OptionFeeSchedule fee() {
  OptionFeeSchedule out;
  out.key = 1U;
  out.effective_from_ts_ns = 0;
  out.effective_until_ts_ns = 2'000;
  out.exchange_per_contract = money("0.01");
  out.commission_per_order = money("0.25");
  out.source_identity = identity(700U);
  return out;
}

[[nodiscard]] OptionTickSchedule tick() {
  OptionTickSchedule out;
  out.key = 1U;
  out.effective_from_ts_ns = 0;
  out.effective_until_ts_ns = 2'000;
  out.tick_below_threshold = money("0.01");
  out.tick_at_or_above_threshold = money("0.01");
  out.source_identity = identity(800U);
  return out;
}

[[nodiscard]] OptionReplayInputs static_inputs(std::span<const OptionReplayContract> contracts,
                                               std::span<const OptionTopOfBookEvent> quotes,
                                               std::span<const OptionFeeSchedule> fees,
                                               std::span<const OptionTickSchedule> ticks,
                                               Decimal initial_cash = Decimal{}) {
  return OptionReplayInputs{contracts, quotes, {}, {}, fees, ticks, initial_cash};
}

TEST(OptionExecutionSession, DecisionOrderCannotConsumeSameTimestampQuote) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U), quote(400, 2U)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, quotes, fees, ticks), config()));

  const auto first = session->advance_to(250);
  ASSERT_TRUE(first) << first.error().to_string();
  EXPECT_TRUE(first->new_fills.empty());
  const std::array commands{order(1U, 3, 250, 300)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{commands, {}}));

  const auto tied = session->advance_to(300);
  ASSERT_TRUE(tied) << tied.error().to_string();
  EXPECT_TRUE(tied->new_fills.empty());
  EXPECT_EQ(tied->replay_summary.quote_events, 1U);
  EXPECT_EQ(tied->replay_summary.firm_quote_events, 1U);
  EXPECT_EQ(tied->replay_summary.open_orders, 1U);
  ASSERT_EQ(tied->order_states.size(), 1U);
  EXPECT_EQ(tied->order_states[0].state, OptionOrderLifecycleState::Working);
  ASSERT_TRUE(session->apply_commands({}));

  const auto later = session->advance_to(400);
  ASSERT_TRUE(later) << later.error().to_string();
  ASSERT_EQ(later->new_fills.size(), 1U);
  EXPECT_EQ(later->replay_summary.quote_events, 2U);
  EXPECT_EQ(later->replay_summary.open_orders, 0U);
  EXPECT_EQ(later->positions[0].contracts, 3);
}

TEST(OptionExecutionSession, PartialFillAndWorkingLeavesCarryAcrossFrontiers) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 6)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, quotes, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  const std::array commands{order(1U, 10, 100, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{commands, {}}));

  const auto filled = session->advance_to(300);
  ASSERT_TRUE(filled) << filled.error().to_string();
  ASSERT_EQ(filled->positions.size(), 1U);
  ASSERT_EQ(filled->exposures.size(), 1U);
  ASSERT_EQ(filled->order_states.size(), 1U);
  EXPECT_EQ(filled->positions[0].contracts, 6);
  EXPECT_EQ(filled->exposures[0].working_contracts, 4);
  EXPECT_EQ(filled->exposures[0].projected_contracts, 10);
  EXPECT_EQ(filled->order_states[0].state, OptionOrderLifecycleState::PartiallyFilled);
  EXPECT_EQ(filled->order_states[0].remaining_contracts, 4);
  const Decimal cash_after_fill = filled->replay_summary.final_cash;
  ASSERT_TRUE(session->apply_commands({}));

  const auto quiet = session->advance_to(350);
  ASSERT_TRUE(quiet) << quiet.error().to_string();
  EXPECT_TRUE(quiet->new_fills.empty());
  EXPECT_EQ(quiet->positions[0].contracts, 6);
  EXPECT_EQ(quiet->exposures[0].working_contracts, 4);
  EXPECT_EQ(quiet->replay_summary.final_cash, cash_after_fill);
}

TEST(OptionExecutionSession, PendingCancelKeepsExposureAndInterveningFill) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 4), quote(375, 2U, 6), quote(450, 3U, 10)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, quotes, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  const std::array commands{order(1U, 10, 100, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{commands, {}}));
  ASSERT_TRUE(session->advance_to(300));
  const std::array cancels{cancel(1U, 1U, 300, 400)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{{}, cancels}));

  const auto pending = session->advance_to(350);
  ASSERT_TRUE(pending) << pending.error().to_string();
  EXPECT_EQ(pending->positions[0].contracts, 4);
  EXPECT_EQ(pending->exposures[0].working_contracts, 6);
  EXPECT_EQ(pending->exposures[0].pending_cancel_contracts, 6);
  EXPECT_EQ(pending->exposures[0].projected_contracts, 10);
  EXPECT_EQ(pending->order_states[0].state, OptionOrderLifecycleState::PendingCancel);
  ASSERT_TRUE(session->apply_commands({}));

  const auto intervening = session->advance_to(375);
  ASSERT_TRUE(intervening) << intervening.error().to_string();
  EXPECT_EQ(intervening->positions[0].contracts, 6);
  EXPECT_EQ(intervening->exposures[0].working_contracts, 4);
  EXPECT_EQ(intervening->exposures[0].pending_cancel_contracts, 4);
  EXPECT_EQ(intervening->exposures[0].projected_contracts, 10);
  ASSERT_TRUE(session->apply_commands({}));

  const auto canceled = session->advance_to(400);
  ASSERT_TRUE(canceled) << canceled.error().to_string();
  EXPECT_EQ(canceled->positions[0].contracts, 6);
  EXPECT_EQ(canceled->exposures[0].working_contracts, 0);
  EXPECT_EQ(canceled->exposures[0].pending_cancel_contracts, 0);
  EXPECT_EQ(canceled->exposures[0].projected_contracts, 6);
  EXPECT_EQ(canceled->order_states[0].state, OptionOrderLifecycleState::Canceled);
}

TEST(OptionExecutionSession, InvalidCommandBasketIsAtomicAndRetryable) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(500, 1U)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, quotes, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));

  std::array commands{order(1U, 1, 100, 200), order(2U, 1, 100, 200)};
  commands[1].limit_price = money("11.005");
  const auto rejected = session->apply_commands(OptionCommandBatch{commands, {}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(session->state(), OptionExecutionSessionState::AtFrontier);

  const std::array corrected{order(1U, 1, 100, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{corrected, {}}));
  const auto next = session->advance_to(200);
  ASSERT_TRUE(next) << next.error().to_string();
  ASSERT_EQ(next->orders.size(), 1U);
  ASSERT_EQ(next->transitions.size(), 2U);
  ASSERT_EQ(next->new_transitions.size(), 2U);
  EXPECT_EQ(next->new_transitions[0].kind, OptionOrderTransitionKind::Scheduled);
  EXPECT_EQ(next->new_transitions[1].kind, OptionOrderTransitionKind::Submitted);
  EXPECT_EQ(next->orders[0].request.order_id.value, 1U);
}

TEST(OptionExecutionSession, StateMachineRejectsDuplicateFrontierWithoutMutation) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(500, 1U)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, quotes, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  ASSERT_TRUE(session->apply_commands({}));

  const auto duplicate = session->advance_to(100);
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(session->state(), OptionExecutionSessionState::ReadyToAdvance);

  const auto next = session->advance_to(101);
  ASSERT_TRUE(next) << next.error().to_string();
  EXPECT_EQ(next->frontier_ts_ns, 101);
  EXPECT_EQ(next->session_summary.frontier_count, 2U);
}

TEST(OptionExecutionSession, FrontierCapacityRejectsPlusOneWithoutMutation) {
  OptionExecutionSessionLimits limits = session_limits();
  limits.max_frontiers = 2U;
  auto session = OptionExecutionSession::create(limits);
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, {}, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  ASSERT_TRUE(session->apply_commands({}));
  ASSERT_TRUE(session->advance_to(200));
  ASSERT_TRUE(session->apply_commands({}));

  const auto rejected = session->advance_to(300);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(session->state(), OptionExecutionSessionState::ReadyToAdvance);
}

TEST(OptionExecutionSession, WorkspaceByteBoundaryIsDeterministic) {
  OptionExecutionSessionLimits limits = session_limits();
  const auto required = option_execution_session_required_workspace_bytes(limits);
  ASSERT_TRUE(required) << required.error().to_string();
  limits.max_workspace_bytes = *required;
  EXPECT_TRUE(OptionExecutionSession::create(limits));
  --limits.max_workspace_bytes;
  const auto rejected = OptionExecutionSession::create(limits);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::OutOfRange);
}

TEST(OptionExecutionSession, FinishMatchesOneShotReplayExactly) {
  auto session = OptionExecutionSession::create(session_limits());
  auto replay = OptionExecutionReplay::create(session_limits().replay);
  ASSERT_TRUE(session) << session.error().to_string();
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 3), quote(400, 2U, 3)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const std::array orders{order(1U, 5, 100, 200)};
  const OptionReplayConfig replay_config = config();

  ASSERT_TRUE(session->start(static_inputs(contracts, quotes, fees, ticks), replay_config));
  ASSERT_TRUE(session->advance_to(100));
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{orders, {}}));
  const auto incremental = session->finish();
  ASSERT_TRUE(incremental) << incremental.error().to_string();

  const OptionReplayInputs batch_inputs{contracts, quotes, orders, {}, fees, ticks, Decimal{}};
  const auto batch = replay->run(batch_inputs, replay_config);
  ASSERT_TRUE(batch) << batch.error().to_string();
  EXPECT_TRUE(std::equal(incremental->replay.fills.begin(), incremental->replay.fills.end(),
                         batch->fills.begin(), batch->fills.end()));
  EXPECT_TRUE(std::equal(incremental->replay.orders.begin(), incremental->replay.orders.end(),
                         batch->orders.begin(), batch->orders.end()));
  EXPECT_TRUE(std::equal(incremental->replay.positions.begin(), incremental->replay.positions.end(),
                         batch->positions.begin(), batch->positions.end()));
  EXPECT_EQ(incremental->replay.summary, batch->summary);
}

TEST(OptionExecutionSession, CancelQuoteSubmitTieCannotBackfillReplacement) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 4), quote(400, 2U, 10), quote(450, 3U, 10)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, quotes, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  const std::array first_order{order(1U, 10, 100, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{first_order, {}}));
  ASSERT_TRUE(session->advance_to(300));

  const std::array replacement{order(2U, 6, 300, 400)};
  const std::array cancel_old{cancel(1U, 1U, 300, 400)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{replacement, cancel_old}));
  const auto tied = session->advance_to(400);
  ASSERT_TRUE(tied) << tied.error().to_string();
  EXPECT_TRUE(tied->new_fills.empty());
  ASSERT_EQ(tied->order_states.size(), 2U);
  EXPECT_EQ(tied->order_states[0].state, OptionOrderLifecycleState::Canceled);
  EXPECT_EQ(tied->order_states[1].state, OptionOrderLifecycleState::Working);
  EXPECT_EQ(tied->positions[0].contracts, 4);
  ASSERT_TRUE(session->apply_commands({}));

  const auto later = session->advance_to(450);
  ASSERT_TRUE(later) << later.error().to_string();
  ASSERT_EQ(later->new_fills.size(), 1U);
  EXPECT_EQ(later->positions[0].contracts, 10);
}

TEST(OptionExecutionSession, CommandPermutationHasCanonicalTraceAndOrderState) {
  auto left = OptionExecutionSession::create(session_limits());
  auto right = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(left) << left.error().to_string();
  ASSERT_TRUE(right) << right.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(500, 1U, 10)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs = static_inputs(contracts, quotes, fees, ticks);
  ASSERT_TRUE(left->start(inputs, config()));
  ASSERT_TRUE(right->start(inputs, config()));
  ASSERT_TRUE(left->advance_to(100));
  ASSERT_TRUE(right->advance_to(100));
  const std::array canonical{order(1U, 2, 100, 200), order(2U, 3, 100, 200)};
  const std::array shuffled{canonical[1], canonical[0]};
  ASSERT_TRUE(left->apply_commands(OptionCommandBatch{canonical, {}}));
  ASSERT_TRUE(right->apply_commands(OptionCommandBatch{shuffled, {}}));
  const auto left_view = left->advance_to(200);
  const auto right_view = right->advance_to(200);
  ASSERT_TRUE(left_view) << left_view.error().to_string();
  ASSERT_TRUE(right_view) << right_view.error().to_string();
  EXPECT_EQ(left_view->session_summary.command_trace_hash,
            right_view->session_summary.command_trace_hash);
  EXPECT_TRUE(std::equal(left_view->orders.begin(), left_view->orders.end(),
                         right_view->orders.begin(), right_view->orders.end()));
  EXPECT_TRUE(std::equal(left_view->transitions.begin(), left_view->transitions.end(),
                         right_view->transitions.begin(), right_view->transitions.end()));
}

TEST(OptionExecutionSession, NonzeroPositionAtExpiryFailsClosed) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  std::array contracts{contract(1)};
  contracts[0].expiry_ts_ns = 500;
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, {}, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(499));
  ASSERT_TRUE(session->apply_commands({}));
  const auto expired = session->advance_to(500);
  ASSERT_FALSE(expired);
  EXPECT_EQ(expired.error().code(), ErrorCode::NotImplemented);
  EXPECT_EQ(session->state(), OptionExecutionSessionState::Failed);

  const std::array reusable_contracts{contract()};
  ASSERT_TRUE(session->start(static_inputs(reusable_contracts, {}, fees, ticks), config()));
  EXPECT_EQ(session->state(), OptionExecutionSessionState::ReadyToAdvance);
}

TEST(OptionExecutionSession, DayOrderExpiresBeforeSameTimestampQuote) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(350, 1U, 10)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, quotes, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  std::array commands{order(1U, 3, 100, 200)};
  commands[0].time_in_force = OptionTimeInForce::Day;
  commands[0].expire_ts_ns = 350;
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{commands, {}}));

  const auto expired = session->advance_to(350);
  ASSERT_TRUE(expired) << expired.error().to_string();
  EXPECT_TRUE(expired->new_fills.empty());
  ASSERT_EQ(expired->order_states.size(), 1U);
  EXPECT_EQ(expired->order_states[0].state, OptionOrderLifecycleState::Expired);
  EXPECT_EQ(expired->exposures[0].projected_contracts, 0);
  EXPECT_EQ(expired->new_transitions.back().kind, OptionOrderTransitionKind::Expired);
}

TEST(OptionExecutionSession, UnknownCancelIsAuditedAtRequestAndAvailability) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, {}, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  const std::array cancels{cancel(1U, 99U, 100, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{{}, cancels}));

  const auto available = session->advance_to(200);
  ASSERT_TRUE(available) << available.error().to_string();
  ASSERT_EQ(available->cancellations.size(), 1U);
  EXPECT_EQ(available->cancellations[0].disposition, OptionCancelDisposition::UnknownOrder);
  ASSERT_EQ(available->transitions.size(), 2U);
  EXPECT_EQ(available->transitions[0].kind, OptionOrderTransitionKind::CancelRequested);
  EXPECT_EQ(available->transitions[0].event_ts_ns, 100);
  EXPECT_EQ(available->transitions[0].available_ts_ns, 100);
  EXPECT_EQ(available->transitions[1].kind, OptionOrderTransitionKind::CancelUnknownOrder);
  EXPECT_EQ(available->transitions[1].event_ts_ns, 100);
  EXPECT_EQ(available->transitions[1].available_ts_ns, 200);
  EXPECT_EQ(available->transitions[1].state_after, OptionOrderLifecycleState::NotApplicable);
}

TEST(OptionExecutionSession, UnknownCancelCannotCaptureFutureOrderWithSameId) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, {}, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  const std::array cancels{cancel(1U, 99U, 100, 300)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{{}, cancels}));
  ASSERT_TRUE(session->advance_to(150));
  const std::array commands{order(99U, 1, 150, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{commands, {}}));

  const auto available = session->advance_to(300);
  ASSERT_TRUE(available) << available.error().to_string();
  ASSERT_EQ(available->cancellations.size(), 1U);
  EXPECT_EQ(available->cancellations[0].disposition, OptionCancelDisposition::UnknownOrder);
  ASSERT_EQ(available->order_states.size(), 1U);
  EXPECT_EQ(available->order_states[0].state, OptionOrderLifecycleState::Working);
  EXPECT_EQ(available->exposures[0].projected_contracts, 1);
}

TEST(OptionExecutionSession, DuplicatePendingCancelBasketRejectsAtomically) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, {}, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  const std::array commands{order(1U, 3, 100, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{commands, {}}));
  ASSERT_TRUE(session->advance_to(200));

  const std::array duplicate_cancels{cancel(1U, 1U, 200, 300), cancel(2U, 1U, 200, 300)};
  const auto rejected = session->apply_commands(OptionCommandBatch{{}, duplicate_cancels});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::AlreadyExists);
  EXPECT_EQ(session->state(), OptionExecutionSessionState::AtFrontier);

  const std::array corrected{duplicate_cancels[0]};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{{}, corrected}));
  const auto pending = session->advance_to(250);
  ASSERT_TRUE(pending) << pending.error().to_string();
  ASSERT_EQ(pending->cancellations.size(), 1U);
  EXPECT_EQ(pending->exposures[0].pending_cancel_contracts, 3);
}

TEST(OptionExecutionSession, AggregatePendingCancelOverflowRejectsAtomically) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, {}, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  constexpr std::int64_t kLargeQuantity = 5'000'000'000'000'000'000LL;
  const std::array commands{order(1U, kLargeQuantity, 100, 200),
                            order(2U, -kLargeQuantity, 100, 200),
                            order(3U, kLargeQuantity, 100, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{commands, {}}));
  ASSERT_TRUE(session->advance_to(200));

  const std::array overflowing{cancel(1U, 1U, 200, 300), cancel(2U, 3U, 200, 300)};
  const auto rejected = session->apply_commands(OptionCommandBatch{{}, overflowing});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(session->state(), OptionExecutionSessionState::AtFrontier);

  const std::array corrected{overflowing[0]};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{{}, corrected}));
  const auto pending = session->advance_to(250);
  ASSERT_TRUE(pending) << pending.error().to_string();
  ASSERT_EQ(pending->cancellations.size(), 1U);
  EXPECT_EQ(pending->exposures[0].pending_cancel_contracts, kLargeQuantity);
  EXPECT_TRUE(pending->order_states[0].cancel_pending);
  EXPECT_FALSE(pending->order_states[2].cancel_pending);
}

TEST(OptionExecutionSession, TransitionCapacityRejectsBeforeCommandMutation) {
  OptionExecutionSessionLimits limits = session_limits();
  limits.max_transitions = 1U;
  auto session = OptionExecutionSession::create(limits);
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, {}, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  const std::array commands{order(1U, 1, 100, 200)};
  const auto rejected = session->apply_commands(OptionCommandBatch{commands, {}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::OutOfRange);
  EXPECT_EQ(session->state(), OptionExecutionSessionState::AtFrontier);
}

TEST(OptionExecutionSession, DynamicIdentifiersMustIncreaseAcrossBatches) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  ASSERT_TRUE(session->start(static_inputs(contracts, {}, fees, ticks), config()));
  ASSERT_TRUE(session->advance_to(100));
  const std::array first{order(2U, 1, 100, 200)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{first, {}}));
  ASSERT_TRUE(session->advance_to(150));

  const std::array regressed{order(1U, 1, 150, 250)};
  const auto rejected = session->apply_commands(OptionCommandBatch{regressed, {}});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(session->state(), OptionExecutionSessionState::AtFrontier);

  const std::array corrected{order(3U, 1, 150, 250)};
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{corrected, {}}));
}

TEST(OptionExecutionSession, WarmLifecyclePerformsNoDynamicAllocation) {
  auto session = OptionExecutionSession::create(session_limits());
  ASSERT_TRUE(session) << session.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 2), quote(400, 2U, 2)};
  const std::array fees{fee()};
  const std::array ticks{tick()};
  const OptionReplayInputs inputs = static_inputs(contracts, quotes, fees, ticks);
  const OptionReplayConfig replay_config = config();
  const std::array commands{order(1U, 4, 100, 200)};
  const std::array cancellations{cancel(1U, 1U, 300, 350)};

  ASSERT_TRUE(session->start(inputs, replay_config));
  ASSERT_TRUE(session->advance_to(100));
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{commands, {}}));
  ASSERT_TRUE(session->advance_to(300));
  ASSERT_TRUE(session->apply_commands(OptionCommandBatch{{}, cancellations}));
  ASSERT_TRUE(session->advance_to(350));
  ASSERT_TRUE(session->apply_commands({}));
  ASSERT_TRUE(session->finish());

  option_replay_alloc::g_count.store(0U, std::memory_order_relaxed);
  option_replay_alloc::g_armed.store(true, std::memory_order_relaxed);
  const auto started = session->start(inputs, replay_config);
  const auto first = started
                         ? session->advance_to(100)
                         : atx::core::Result<atx::options::execution::OptionExecutionFrontierView>{
                               tl::unexpected<atx::core::Error>{started.error()}};
  const auto applied =
      first ? session->apply_commands(OptionCommandBatch{commands, {}})
            : atx::core::Result<void>{tl::unexpected<atx::core::Error>{first.error()}};
  const auto second = applied
                          ? session->advance_to(300)
                          : atx::core::Result<atx::options::execution::OptionExecutionFrontierView>{
                                tl::unexpected<atx::core::Error>{applied.error()}};
  const auto canceled =
      second ? session->apply_commands(OptionCommandBatch{{}, cancellations})
             : atx::core::Result<void>{tl::unexpected<atx::core::Error>{second.error()}};
  const auto third = canceled
                         ? session->advance_to(350)
                         : atx::core::Result<atx::options::execution::OptionExecutionFrontierView>{
                               tl::unexpected<atx::core::Error>{canceled.error()}};
  const auto acknowledged =
      third ? session->apply_commands({})
            : atx::core::Result<void>{tl::unexpected<atx::core::Error>{third.error()}};
  const auto finished =
      acknowledged ? session->finish()
                   : atx::core::Result<atx::options::execution::OptionExecutionSessionResult>{
                         tl::unexpected<atx::core::Error>{acknowledged.error()}};
  option_replay_alloc::g_armed.store(false, std::memory_order_relaxed);

  ASSERT_TRUE(started) << started.error().to_string();
  ASSERT_TRUE(first) << first.error().to_string();
  ASSERT_TRUE(applied) << applied.error().to_string();
  ASSERT_TRUE(second) << second.error().to_string();
  ASSERT_TRUE(canceled) << canceled.error().to_string();
  ASSERT_TRUE(third) << third.error().to_string();
  ASSERT_TRUE(acknowledged) << acknowledged.error().to_string();
  ASSERT_TRUE(finished) << finished.error().to_string();
  EXPECT_EQ(option_replay_alloc::g_count.load(std::memory_order_relaxed), 0U);
}

} // namespace
