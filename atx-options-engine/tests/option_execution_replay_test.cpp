#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <span>
#include <vector>

#include "atx/core/decimal.hpp"
#include "atx/core/error.hpp"
#include "atx/options/option_execution_replay.hpp"
#include "atx/options/option_research_panel.hpp"

namespace option_replay_alloc {
std::atomic<std::size_t> g_count{0U};
std::atomic<bool> g_armed{false};
} // namespace option_replay_alloc

void *operator new(std::size_t size) {
  if (option_replay_alloc::g_armed.load(std::memory_order_relaxed)) {
    option_replay_alloc::g_count.fetch_add(1U, std::memory_order_relaxed);
  }
  void *memory = std::malloc(size == 0U ? 1U : size);
  if (memory == nullptr) {
    throw std::bad_alloc{};
  }
  return memory;
}

void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept { std::free(memory); }

namespace {

using atx::core::Decimal;
using atx::core::ErrorCode;
using atx::options::execution::make_option_order_batch;
using atx::options::execution::option_replay_required_workspace_bytes;
using atx::options::execution::OptionCancelDisposition;
using atx::options::execution::OptionCancelId;
using atx::options::execution::OptionCancelRequest;
using atx::options::execution::OptionExecutionReplay;
using atx::options::execution::OptionFeeSchedule;
using atx::options::execution::OptionFill;
using atx::options::execution::OptionMarketOrderKey;
using atx::options::execution::OptionOrderAudit;
using atx::options::execution::OptionOrderBatchSpec;
using atx::options::execution::OptionOrderDisposition;
using atx::options::execution::OptionOrderId;
using atx::options::execution::OptionOrderRequest;
using atx::options::execution::OptionPositionSnapshot;
using atx::options::execution::OptionQuoteStatus;
using atx::options::execution::OptionReplayConfig;
using atx::options::execution::OptionReplayContract;
using atx::options::execution::OptionReplayInputs;
using atx::options::execution::OptionReplayLimits;
using atx::options::execution::OptionReplayScenario;
using atx::options::execution::OptionTickSchedule;
using atx::options::execution::OptionTimeInForce;
using atx::options::execution::OptionTopOfBookEvent;
using atx::options::research::make_option_target_book;
using atx::options::research::OptionPanelRow;
using atx::options::research::OptionResearchPanel;
using atx::options::research::OptionSizingBasis;
using atx::options::research::OptionTargetSpec;
using atx::vol::ArchiveContentIdentity;

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) {
  return ArchiveContentIdentity{1'000U + seed, 2'000U + seed,
                                static_cast<std::uint32_t>(3'000U + seed),
                                static_cast<std::uint32_t>(4'000U + seed)};
}

[[nodiscard]] Decimal money(const char *text) {
  const auto value = Decimal::from_string(text);
  EXPECT_TRUE(value) << value.error().to_string();
  return value.value_or(Decimal{});
}

[[nodiscard]] OptionReplayLimits limits() {
  OptionReplayLimits out;
  out.max_contracts = 8U;
  out.max_quote_events = 32U;
  out.max_orders = 16U;
  out.max_cancellations = 8U;
  out.max_fee_rows = 8U;
  out.max_tick_rows = 8U;
  out.max_fills = 32U;
  out.max_workspace_bytes = 4U * 1024U * 1024U;
  return out;
}

[[nodiscard]] OptionReplayConfig config(std::int64_t replay_end_ts_ns = 1'000) {
  OptionReplayConfig out;
  out.market_data_identity = identity(900U);
  out.sequence_validation_identity = identity(902U);
  out.sequence_continuity_verified = true;
  out.scenario = OptionReplayScenario::Calibrated;
  out.calibration_identity = identity(901U);
  out.displayed_size_fraction = Decimal::from_int(1);
  out.max_quote_age_ns = 1'000;
  out.replay_end_ts_ns = replay_end_ts_ns;
  return out;
}

[[nodiscard]] OptionReplayContract contract(std::uint64_t contract_id = 10U,
                                            std::uint32_t engine_id = 1U,
                                            std::int64_t initial_contracts = 0) {
  OptionReplayContract out;
  out.contract_id = contract_id;
  out.engine_id.id = engine_id;
  out.multiplier = 100;
  out.initial_contracts = initial_contracts;
  out.tick_schedule_key = 1U;
  out.definition_effective_ts_ns = 1;
  out.definition_available_ts_ns = 2;
  out.expiry_ts_ns = 10'000;
  out.definition_source_identity = identity(contract_id + 500U);
  return out;
}

[[nodiscard]] OptionTopOfBookEvent quote(std::int64_t available_ts_ns, std::uint64_t sequence,
                                         std::int64_t bid_size_contracts = 10,
                                         std::int64_t ask_size_contracts = 10) {
  OptionTopOfBookEvent out;
  out.contract_id = 10U;
  out.engine_id.id = 1U;
  out.quote_event_ts_ns = available_ts_ns - 1;
  out.available_ts_ns = available_ts_ns;
  out.order_key = OptionMarketOrderKey{1U, 2U, 20'260'726U, sequence, 0U, sequence};
  out.bid = money("9.0");
  out.ask = money("11.0");
  out.bid_size_contracts = bid_size_contracts;
  out.ask_size_contracts = ask_size_contracts;
  out.bid_participant_id = 7U;
  out.ask_participant_id = 8U;
  out.source_identity = identity(sequence);
  return out;
}

[[nodiscard]] OptionOrderRequest order(std::uint64_t order_id, std::int64_t quantity_contracts,
                                       std::int64_t arrival_ts_ns = 200,
                                       const char *limit_price = "11.0") {
  OptionOrderRequest out;
  out.order_id = OptionOrderId{order_id};
  out.strategy_id = 77U;
  out.basket_id = 88U;
  out.contract_id = 10U;
  out.engine_id.id = 1U;
  out.quantity_contracts = quantity_contracts;
  out.limit_price = money(limit_price);
  out.decision_ts_ns = arrival_ts_ns - 100;
  out.arrival_ts_ns = arrival_ts_ns;
  out.priority_sequence = order_id;
  out.fee_schedule_key = 1U;
  out.time_in_force = OptionTimeInForce::GoodTillCanceled;
  return out;
}

[[nodiscard]] OptionFeeSchedule fee(std::int64_t effective_from_ts_ns = 0,
                                    std::int64_t effective_until_ts_ns = 10'000) {
  OptionFeeSchedule out;
  out.key = 1U;
  out.effective_from_ts_ns = effective_from_ts_ns;
  out.effective_until_ts_ns = effective_until_ts_ns;
  out.source_identity = identity(static_cast<std::uint64_t>(effective_from_ts_ns) + 700U);
  return out;
}

[[nodiscard]] OptionTickSchedule tick(std::int64_t effective_from_ts_ns = 0,
                                      std::int64_t effective_until_ts_ns = 10'000,
                                      const char *tick_size = "0.01") {
  OptionTickSchedule out;
  out.key = 1U;
  out.effective_from_ts_ns = effective_from_ts_ns;
  out.effective_until_ts_ns = effective_until_ts_ns;
  out.tick_below_threshold = money(tick_size);
  out.tick_at_or_above_threshold = money(tick_size);
  out.source_identity = identity(static_cast<std::uint64_t>(effective_from_ts_ns) + 800U);
  return out;
}

[[nodiscard]] std::span<const OptionTickSchedule> default_ticks() {
  static const std::array schedules{tick()};
  return schedules;
}

[[nodiscard]] OptionReplayInputs replay_inputs(std::span<const OptionReplayContract> contracts,
                                               std::span<const OptionTopOfBookEvent> quotes,
                                               std::span<const OptionOrderRequest> orders,
                                               std::span<const OptionCancelRequest> cancellations,
                                               std::span<const OptionFeeSchedule> fees,
                                               Decimal initial_cash = Decimal{}) {
  return OptionReplayInputs{contracts, quotes,          orders,      cancellations,
                            fees,      default_ticks(), initial_cash};
}

[[nodiscard]] const OptionOrderAudit &find_order(std::span<const OptionOrderAudit> orders,
                                                 std::uint64_t order_id) {
  const auto found = std::find_if(orders.begin(), orders.end(), [order_id](const auto &audit) {
    return audit.request.order_id.value == order_id;
  });
  EXPECT_NE(found, orders.end());
  return found == orders.end() ? orders.front() : *found;
}

TEST(OptionExecutionReplay, SharedSelectedParticipantSizeProducesPartialFill) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 10, 6)};
  const std::array orders{order(1U, 4), order(2U, 4)};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 2U);
  EXPECT_EQ(result->fills[0].order_id.value, 1U);
  EXPECT_EQ(result->fills[0].quantity_contracts, 4);
  EXPECT_EQ(result->fills[1].order_id.value, 2U);
  EXPECT_EQ(result->fills[1].quantity_contracts, 2);
  EXPECT_EQ(result->fills[1].displayed_size_before_contracts, 2);
  EXPECT_EQ(result->fills[1].displayed_size_after_contracts, 0);
  EXPECT_EQ(find_order(result->orders, 1U).disposition, OptionOrderDisposition::Filled);
  EXPECT_EQ(find_order(result->orders, 2U).remaining_contracts, 2);
  EXPECT_EQ(find_order(result->orders, 2U).disposition, OptionOrderDisposition::OpenAtEnd);
  EXPECT_EQ(result->summary.filled_contracts, 6U);
}

TEST(OptionExecutionReplay, BetterLimitHasPricePriorityThenSequenceBreaksTies) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 10, 1)};
  OptionOrderRequest lower = order(1U, 1, 200, "11.0");
  lower.priority_sequence = 1U;
  OptionOrderRequest better = order(2U, 1, 200, "12.0");
  better.priority_sequence = 2U;
  const std::array orders{lower, better};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 1U);
  EXPECT_EQ(result->fills.front().order_id.value, 2U);

  OptionTopOfBookEvent later = quote(400, 2U, 10, 2);
  const std::array tie_quotes{quotes.front(), later};
  better.limit_price = money("11.0");
  better.priority_sequence = 2U;
  const std::array tied_orders{lower, better};
  const auto tied =
      replay->run(replay_inputs(contracts, tie_quotes, tied_orders, {}, fees), config());
  ASSERT_TRUE(tied) << tied.error().to_string();
  ASSERT_EQ(tied->fills.size(), 2U);
  EXPECT_EQ(tied->fills[0].order_id.value, 1U);
  EXPECT_EQ(tied->fills[1].order_id.value, 2U);
}

TEST(OptionExecutionReplay, UnchangedParticipantDisplayIsNotReplenished) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent first = quote(300, 1U, 10, 4);
  OptionTopOfBookEvent unchanged = quote(400, 2U, 10, 4);
  OptionTopOfBookEvent increased = quote(500, 3U, 10, 6);
  const std::array quotes{first, unchanged, increased};
  const std::array orders{order(1U, 10)};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 2U);
  EXPECT_EQ(result->fills[0].fill_ts_ns, 300);
  EXPECT_EQ(result->fills[0].quantity_contracts, 4);
  EXPECT_EQ(result->fills[1].fill_ts_ns, 500);
  EXPECT_EQ(result->fills[1].quantity_contracts, 2);
  EXPECT_EQ(result->summary.filled_contracts, 6U);
  EXPECT_EQ(result->orders.front().remaining_contracts, 4);
}

TEST(OptionExecutionReplay, SelectedParticipantChangeStartsNewDisplayedPool) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent first = quote(300, 1U, 10, 2);
  OptionTopOfBookEvent next = quote(400, 2U, 10, 2);
  next.ask_participant_id = 9U;
  const std::array quotes{first, next};
  const std::array orders{order(1U, 4)};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 2U);
  EXPECT_EQ(result->fills[0].selected_participant_id, 8U);
  EXPECT_EQ(result->fills[1].selected_participant_id, 9U);
  EXPECT_EQ(result->orders.front().disposition, OptionOrderDisposition::Filled);
}

TEST(OptionExecutionReplay, BidOnlyUpdateCannotReplenishConsumedAsk) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent first = quote(300, 1U, 10, 4);
  OptionTopOfBookEvent bid_only = quote(400, 2U, 11, 4);
  bid_only.bid_updated = true;
  bid_only.ask_updated = false;
  OptionTopOfBookEvent ask_increase = quote(500, 3U, 11, 6);
  ask_increase.bid_updated = false;
  ask_increase.ask_updated = true;
  const std::array quotes{first, bid_only, ask_increase};
  const std::array orders{order(1U, 10)};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 2U);
  EXPECT_EQ(result->fills[0].fill_ts_ns, 300);
  EXPECT_EQ(result->fills[0].quantity_contracts, 4);
  EXPECT_EQ(result->fills[1].fill_ts_ns, 500);
  EXPECT_EQ(result->fills[1].quantity_contracts, 2);
  EXPECT_EQ(result->orders.front().remaining_contracts, 4);
}

TEST(OptionExecutionReplay, OneSidedContextCannotRefreshOrTriggerOppositeSide) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent before_arrival = quote(150, 1U, 10, 10);
  OptionTopOfBookEvent bid_only = quote(300, 2U, 11, 10);
  bid_only.ask_updated = false;
  OptionTopOfBookEvent ask_update = quote(400, 3U, 11, 10);
  ask_update.bid_updated = false;
  const std::array quotes{before_arrival, bid_only, ask_update};
  const std::array orders{order(1U, 1)};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 1U);
  EXPECT_EQ(result->fills.front().fill_ts_ns, 400);
  EXPECT_EQ(result->fills.front().quote_order_key.native_sequence, 3U);
}

TEST(OptionExecutionReplay, ContradictoryOneSidedContextFailsClosed) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent first = quote(300, 1U, 10, 10);
  OptionTopOfBookEvent contradictory = quote(400, 2U, 11, 1);
  contradictory.ask_updated = false;
  const std::array quotes{first, contradictory};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, {}, {}, fees), config());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionExecutionReplay, QuoteAtArrivalCannotFillOrder) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(200, 1U, 10, 10), quote(201, 2U, 10, 10)};
  const std::array orders{order(1U, 1, 200)};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 1U);
  EXPECT_EQ(result->fills.front().fill_ts_ns, 201);
  EXPECT_GT(result->fills.front().fill_ts_ns, result->fills.front().arrival_ts_ns);
}

TEST(OptionExecutionReplay, FirstFutureQuoteOrCancelCancelsLeavesAtFirstQuote) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent stale = quote(300, 1U, 10, 10);
  stale.quote_event_ts_ns = 100;
  const std::array quotes{stale, quote(400, 2U, 10, 10)};
  OptionOrderRequest request = order(1U, 3);
  request.time_in_force = OptionTimeInForce::FirstFutureQuoteOrCancel;
  const std::array orders{request};
  const std::array fees{fee()};
  OptionReplayConfig cfg = config();
  cfg.max_quote_age_ns = 50;

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), cfg);

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_TRUE(result->fills.empty());
  ASSERT_EQ(result->orders.size(), 1U);
  EXPECT_EQ(result->orders.front().disposition, OptionOrderDisposition::Canceled);
  EXPECT_EQ(result->orders.front().remaining_contracts, 3);
  EXPECT_EQ(result->summary.stale_match_attempts, 1U);
}

TEST(OptionExecutionReplay, NonExecutableQuotesFailClosedUntilFirmEvidence) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent missing = quote(300, 1U);
  missing.status = OptionQuoteStatus::MissingSize;
  missing.bid_size_contracts = 0;
  missing.ask_size_contracts = 0;
  OptionTopOfBookEvent crossed = quote(400, 2U);
  crossed.status = OptionQuoteStatus::Crossed;
  crossed.bid = money("12.0");
  crossed.ask = money("11.0");
  const std::array quotes{missing, crossed, quote(500, 3U, 10, 1)};
  const std::array orders{order(1U, 1)};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 1U);
  EXPECT_EQ(result->fills.front().fill_ts_ns, 500);
  EXPECT_EQ(result->summary.non_executable_quote_events, 2U);
}

TEST(OptionExecutionReplay, AdverseModeledPriceBeyondLimitDoesNotFill) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U)};
  const std::array orders{order(1U, 1)};
  const std::array fees{fee()};
  OptionReplayConfig cfg = config();
  cfg.adverse_price_bps = Decimal::from_int(100);

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), cfg);

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_TRUE(result->fills.empty());
  EXPECT_EQ(result->orders.front().disposition, OptionOrderDisposition::OpenAtEnd);
}

TEST(OptionExecutionReplay, ExactAdversePriceRoundsOutwardToEffectiveTick) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(301, 1U)};
  const std::array orders{order(1U, 1, 200, "11.05"), order(2U, -1, 200, "8.95")};
  const std::array fees{fee()};
  const std::array ticks{tick(0, 300, "0.01"), tick(300, 10'000, "0.05")};
  OptionReplayConfig cfg = config();
  cfg.adverse_price_bps = Decimal::from_int(1);

  const auto result =
      replay->run(OptionReplayInputs{contracts, quotes, orders, {}, fees, ticks, Decimal{}}, cfg);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 2U);
  EXPECT_EQ(result->fills[0].fill_price, money("11.05"));
  EXPECT_EQ(result->fills[1].fill_price, money("8.95"));
  EXPECT_EQ(result->fills[0].tick_source_identity, ticks[1].source_identity);
}

TEST(OptionExecutionReplay, SubNanoAdverseRemainderStillRoundsToNextBuyTick) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent low_price = quote(300, 1U);
  low_price.bid = money("0.01");
  low_price.ask = money("0.02");
  const std::array quotes{low_price};
  const std::array orders{order(1U, 1, 200, "0.03")};
  const std::array fees{fee()};
  OptionReplayConfig cfg = config();
  cfg.adverse_price_bps = Decimal::from_raw(1);

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), cfg);

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 1U);
  EXPECT_EQ(result->fills.front().fill_price, money("0.03"));
}

TEST(OptionExecutionReplay, OffTickQuotesAndLimitsFailAtBoundary) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent bad_quote = quote(300, 1U);
  bad_quote.ask = money("11.01");
  const std::array quotes{bad_quote};
  const std::array orders{order(1U, 1, 200, "11.00")};
  const std::array fees{fee()};
  const std::array ticks{tick(0, 10'000, "0.05")};

  const auto quote_result = replay->run(
      OptionReplayInputs{contracts, quotes, orders, {}, fees, ticks, Decimal{}}, config());
  ASSERT_FALSE(quote_result);
  EXPECT_EQ(quote_result.error().code(), ErrorCode::InvalidArgument);

  const std::array good_quotes{quote(300, 1U)};
  const std::array bad_orders{order(1U, 1, 200, "11.01")};
  const auto order_result = replay->run(
      OptionReplayInputs{contracts, good_quotes, bad_orders, {}, fees, ticks, Decimal{}}, config());
  ASSERT_FALSE(order_result);
  EXPECT_EQ(order_result.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionExecutionReplay, TickRuleMustBeKnownAtDecisionAndCannotChangeInFlight) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  OptionOrderRequest future_rule_order = order(1U, 1, 120);
  future_rule_order.decision_ts_ns = 100;
  const std::array orders{future_rule_order};
  const std::array quotes{quote(130, 1U)};
  OptionTickSchedule future_rule = tick(110, 10'000, "0.01");
  future_rule.available_ts_ns = 105;
  const std::array future_ticks{future_rule};

  const auto leaked = replay->run(
      OptionReplayInputs{contracts, quotes, orders, {}, fees, future_ticks, Decimal{}}, config());
  ASSERT_FALSE(leaked);
  EXPECT_EQ(leaked.error().code(), ErrorCode::InvalidArgument);

  OptionTopOfBookEvent straddled = quote(300, 1U);
  straddled.quote_event_ts_ns = 299;
  const std::array straddled_quotes{straddled};
  const std::array transition_ticks{tick(0, 300, "0.01"), tick(300, 10'000, "0.05")};
  const auto transition = replay->run(
      OptionReplayInputs{contracts, straddled_quotes, {}, {}, fees, transition_ticks, Decimal{}},
      config());
  ASSERT_FALSE(transition);
  EXPECT_EQ(transition.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionExecutionReplay, ExactMultiplierCashAndComponentFeesReconcile) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 10, 10)};
  const std::array orders{order(1U, -2, 200, "9.0")};
  OptionFeeSchedule schedule = fee();
  schedule.exchange_per_contract = money("0.1");
  schedule.clearing_per_contract = money("0.025");
  schedule.regulatory_per_contract = money("0.01");
  schedule.commission_per_contract = money("0.05");
  schedule.commission_per_order = money("0.5");
  schedule.sell_premium_rate = money("0.00002");
  const std::array fees{schedule};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 1U);
  const OptionFill &fill = result->fills.front();
  EXPECT_EQ(fill.premium_notional, money("1800.0"));
  EXPECT_EQ(fill.fees.exchange, money("0.2"));
  EXPECT_EQ(fill.fees.clearing, money("0.05"));
  EXPECT_EQ(fill.fees.regulatory, money("0.02"));
  EXPECT_EQ(fill.fees.commission, money("0.6"));
  EXPECT_EQ(fill.fees.sales_value, money("0.036"));
  EXPECT_EQ(fill.fees.total, money("0.906"));
  EXPECT_EQ(fill.cash_delta, money("1799.094"));
  EXPECT_EQ(result->summary.final_cash, money("1799.094"));
  EXPECT_EQ(result->summary.gross_premium_turnover, money("1800.0"));
  EXPECT_EQ(result->summary.total_fees, money("0.906"));
  ASSERT_EQ(result->positions.size(), 1U);
  EXPECT_EQ(result->positions.front().contracts, -2);
}

TEST(OptionExecutionReplay, EffectiveDatedFeeRowIsSelectedAtFillTime) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(600, 1U)};
  const std::array orders{order(1U, 1)};
  OptionFeeSchedule early = fee(0, 500);
  early.exchange_per_contract = money("0.1");
  OptionFeeSchedule late = fee(500, 1'000);
  late.exchange_per_contract = money("0.3");
  const std::array fees{late, early};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 1U);
  EXPECT_EQ(result->fills.front().fees.exchange, money("0.3"));
  EXPECT_EQ(result->fills.front().fee_source_identity, late.source_identity);
}

TEST(OptionExecutionReplay, FeeIntervalBoundarySelectsNewRowExactly) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(500, 1U)};
  const std::array orders{order(1U, 1)};
  OptionFeeSchedule early = fee(0, 500);
  early.exchange_per_contract = money("0.1");
  OptionFeeSchedule late = fee(500, 1'000);
  late.exchange_per_contract = money("0.4");
  const std::array fees{early, late};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 1U);
  EXPECT_EQ(result->fills.front().fees.exchange, money("0.4"));
  EXPECT_EQ(result->fills.front().fee_source_identity, late.source_identity);
}

TEST(OptionExecutionReplay, RebateAndSellOnlySalesChargePreserveExactSigns) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 1, 1)};
  const std::array orders{order(1U, 1), order(2U, -1, 200, "9.0")};
  OptionFeeSchedule schedule = fee();
  schedule.exchange_per_contract = money("-0.2");
  schedule.sell_premium_rate = money("0.001");
  const std::array fees{schedule};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 2U);
  EXPECT_EQ(result->fills[0].fees.sales_value, Decimal{});
  EXPECT_EQ(result->fills[0].fees.total, money("-0.2"));
  EXPECT_EQ(result->fills[0].cash_delta, money("-1099.8"));
  EXPECT_EQ(result->fills[1].fees.sales_value, money("0.9"));
  EXPECT_EQ(result->fills[1].fees.total, money("0.7"));
  EXPECT_EQ(result->fills[1].cash_delta, money("899.3"));
}

TEST(OptionExecutionReplay, FeeKnowledgeClockAndArithmeticOverflowFailClosed) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U)};
  const std::array orders{order(1U, 2)};
  OptionFeeSchedule future_known = fee();
  future_known.available_ts_ns = 1;
  const std::array future_fees{future_known};

  const auto knowledge =
      replay->run(replay_inputs(contracts, quotes, orders, {}, future_fees), config());
  ASSERT_FALSE(knowledge);
  EXPECT_EQ(knowledge.error().code(), ErrorCode::InvalidArgument);

  OptionFeeSchedule huge = fee();
  huge.exchange_per_contract = Decimal::from_raw((std::numeric_limits<std::int64_t>::max)());
  const std::array huge_fees{huge};
  const auto overflow =
      replay->run(replay_inputs(contracts, quotes, orders, {}, huge_fees), config());
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().code(), ErrorCode::OutOfRange);
}

TEST(OptionExecutionReplay, PerOrderCommissionIsChargedOnceAcrossPartialFills) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 10, 1), quote(400, 2U, 10, 2), quote(500, 3U, 10, 3)};
  const std::array orders{order(1U, 3)};
  OptionFeeSchedule schedule = fee();
  schedule.commission_per_contract = money("0.1");
  schedule.commission_per_order = money("0.5");
  const std::array fees{schedule};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->fills.size(), 3U);
  EXPECT_EQ(result->fills[0].fees.commission, money("0.6"));
  EXPECT_EQ(result->fills[1].fees.commission, money("0.1"));
  EXPECT_EQ(result->fills[2].fees.commission, money("0.1"));
  EXPECT_EQ(result->orders.front().total_fees, money("0.8"));
  EXPECT_EQ(result->summary.total_fees, money("0.8"));
}

TEST(OptionExecutionReplay, OverlapAndMissingFeeCoverageFailClosed) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U)};
  const std::array orders{order(1U, 1)};
  const std::array overlapping{fee(0, 400), fee(300, 500)};

  const auto overlap =
      replay->run(replay_inputs(contracts, quotes, orders, {}, overlapping), config());
  ASSERT_FALSE(overlap);
  EXPECT_EQ(overlap.error().code(), ErrorCode::AlreadyExists);

  const std::array gapped{fee(0, 250)};
  const auto gap = replay->run(replay_inputs(contracts, quotes, orders, {}, gapped), config());
  ASSERT_FALSE(gap);
  EXPECT_EQ(gap.error().code(), ErrorCode::NotFound);
}

TEST(OptionExecutionReplay, CancelBeforeSameTimestampQuoteAndReplacementLosesPriority) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 10, 1), quote(400, 2U, 10, 1)};
  OptionOrderRequest original = order(1U, 1);
  OptionOrderRequest replacement = order(2U, 1, 300);
  const std::array orders{original, replacement};
  const std::array cancels{
      OptionCancelRequest{OptionCancelId{1U}, OptionOrderId{1U}, 299, 300, 1U}};
  const std::array fees{fee()};

  const auto result =
      replay->run(replay_inputs(contracts, quotes, orders, cancels, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->cancellations.size(), 1U);
  EXPECT_EQ(result->cancellations.front().disposition, OptionCancelDisposition::Applied);
  EXPECT_EQ(find_order(result->orders, 1U).disposition, OptionOrderDisposition::Canceled);
  ASSERT_EQ(result->fills.size(), 1U);
  EXPECT_EQ(result->fills.front().order_id.value, 2U);
  EXPECT_EQ(result->fills.front().fill_ts_ns, 400);
}

TEST(OptionExecutionReplay, DayOrderExpiresBeforeLaterQuote) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(400, 1U)};
  OptionOrderRequest request = order(1U, 1);
  request.time_in_force = OptionTimeInForce::Day;
  request.expire_ts_ns = 350;
  const std::array orders{request};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_TRUE(result->fills.empty());
  EXPECT_EQ(result->orders.front().disposition, OptionOrderDisposition::Expired);
  EXPECT_EQ(result->summary.expired_orders, 1U);
}

TEST(OptionExecutionReplay, QuoteUnavailableUntilExpiryIsRejected) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  OptionReplayContract expiring = contract();
  expiring.expiry_ts_ns = 300;
  const std::array contracts{expiring};
  OptionTopOfBookEvent at_expiry = quote(300, 1U);
  at_expiry.quote_event_ts_ns = 299;
  const std::array quotes{at_expiry};
  const std::array orders{order(1U, 1)};
  const std::array fees{fee()};

  const auto result = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionExecutionReplay, UnknownCancelIsAuditedSeparately) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array cancels{
      OptionCancelRequest{OptionCancelId{1U}, OptionOrderId{999U}, 250, 300, 1U}};

  const auto result = replay->run(replay_inputs(contracts, {}, {}, cancels, {}), config());

  ASSERT_TRUE(result) << result.error().to_string();
  ASSERT_EQ(result->cancellations.size(), 1U);
  EXPECT_EQ(result->cancellations.front().disposition, OptionCancelDisposition::UnknownOrder);
}

TEST(OptionExecutionReplay, NonAdjacentDuplicateCancelIdsAreRejected) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array cancels{
      OptionCancelRequest{OptionCancelId{1U}, OptionOrderId{900U}, 250, 300, 1U},
      OptionCancelRequest{OptionCancelId{2U}, OptionOrderId{901U}, 350, 400, 2U},
      OptionCancelRequest{OptionCancelId{1U}, OptionOrderId{902U}, 450, 500, 3U}};

  const auto result = replay->run(replay_inputs(contracts, {}, {}, cancels, {}), config());

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::AlreadyExists);
}

TEST(OptionExecutionReplay, LockedMarketRequiresExplicitPolicy) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionTopOfBookEvent locked = quote(300, 1U);
  locked.status = OptionQuoteStatus::Locked;
  locked.bid = money("10.0");
  locked.ask = money("10.0");
  const std::array quotes{locked};
  const std::array orders{order(1U, 1)};
  const std::array fees{fee()};

  const auto conservative =
      replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());
  ASSERT_TRUE(conservative) << conservative.error().to_string();
  EXPECT_TRUE(conservative->fills.empty());

  OptionReplayConfig allowed = config();
  allowed.allow_locked_market = true;
  const auto executable = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), allowed);
  ASSERT_TRUE(executable) << executable.error().to_string();
  ASSERT_EQ(executable->fills.size(), 1U);
  EXPECT_EQ(executable->fills.front().fill_price, money("10.0"));
}

TEST(OptionExecutionReplay, ShuffledInputsProduceIdenticalCanonicalOutput) {
  auto first_replay = OptionExecutionReplay::create(limits());
  auto second_replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(first_replay) << first_replay.error().to_string();
  ASSERT_TRUE(second_replay) << second_replay.error().to_string();
  const std::array contracts{contract(20U, 2U), contract(10U, 1U)};
  OptionTopOfBookEvent q10 = quote(300, 1U, 10, 2);
  OptionTopOfBookEvent q20 = quote(300, 2U, 10, 2);
  q20.contract_id = 20U;
  q20.engine_id.id = 2U;
  const std::array quotes{q20, q10};
  OptionOrderRequest o10 = order(10U, 1);
  OptionOrderRequest o20 = order(20U, -1, 200, "9.0");
  o20.contract_id = 20U;
  o20.engine_id.id = 2U;
  const std::array orders{o20, o10};
  const std::array fees{fee()};

  const auto first =
      first_replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());
  ASSERT_TRUE(first) << first.error().to_string();
  std::vector<OptionFill> first_fills(first->fills.begin(), first->fills.end());
  std::vector<OptionOrderAudit> first_orders(first->orders.begin(), first->orders.end());
  std::vector<OptionPositionSnapshot> first_positions(first->positions.begin(),
                                                      first->positions.end());
  const auto first_summary = first->summary;

  const std::array reversed_contracts{contracts[1], contracts[0]};
  const std::array reversed_quotes{quotes[1], quotes[0]};
  const std::array reversed_orders{orders[1], orders[0]};
  const auto second = second_replay->run(
      replay_inputs(reversed_contracts, reversed_quotes, reversed_orders, {}, fees), config());

  ASSERT_TRUE(second) << second.error().to_string();
  EXPECT_TRUE(std::equal(first_fills.begin(), first_fills.end(), second->fills.begin(),
                         second->fills.end()));
  EXPECT_TRUE(std::equal(first_orders.begin(), first_orders.end(), second->orders.begin(),
                         second->orders.end()));
  EXPECT_TRUE(std::equal(first_positions.begin(), first_positions.end(), second->positions.begin(),
                         second->positions.end()));
  EXPECT_EQ(first_summary, second->summary);
}

TEST(OptionExecutionReplay, NativeStreamDuplicatesAndAvailabilityRegressionAreRejected) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};

  OptionTopOfBookEvent duplicate = quote(400, 1U);
  duplicate.order_key.stable_ingest_ordinal = 2U;
  const std::array duplicate_quotes{quote(300, 1U), duplicate};
  const auto duplicate_result =
      replay->run(replay_inputs(contracts, duplicate_quotes, {}, {}, fees), config());
  ASSERT_FALSE(duplicate_result);
  EXPECT_EQ(duplicate_result.error().code(), ErrorCode::AlreadyExists);

  const std::array regressed_quotes{quote(300, 2U), quote(400, 1U)};
  const auto regressed_result =
      replay->run(replay_inputs(contracts, regressed_quotes, {}, {}, fees), config());
  ASSERT_FALSE(regressed_result);
  EXPECT_EQ(regressed_result.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionExecutionReplay, NativeSequenceCanResetOnlyAcrossMonotoneEpochs) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  OptionTopOfBookEvent first_session = quote(300, 100U);
  first_session.order_key.stream_epoch = 1U;
  OptionTopOfBookEvent second_session = quote(400, 1U);
  second_session.order_key.stream_epoch = 2U;
  const std::array valid_quotes{second_session, first_session};

  const auto valid = replay->run(replay_inputs(contracts, valid_quotes, {}, {}, fees), config());
  ASSERT_TRUE(valid) << valid.error().to_string();

  OptionTopOfBookEvent reentered = quote(500, 101U);
  reentered.order_key.stream_epoch = 1U;
  const std::array invalid_quotes{first_session, second_session, reentered};
  const auto invalid =
      replay->run(replay_inputs(contracts, invalid_quotes, {}, {}, fees), config());
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionExecutionReplay, InvalidClocksDuplicatesAndMissingLineageFailAtBoundary) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  OptionTopOfBookEvent invalid_quote = quote(300, 1U);
  invalid_quote.quote_event_ts_ns = 301;
  const std::array bad_quotes{invalid_quote};
  const std::array orders{order(1U, 1)};
  const auto bad_clock =
      replay->run(replay_inputs(contracts, bad_quotes, orders, {}, fees), config());
  ASSERT_FALSE(bad_clock);
  EXPECT_EQ(bad_clock.error().code(), ErrorCode::InvalidArgument);

  const std::array duplicate_orders{order(1U, 1), order(1U, 2)};
  const std::array quotes{quote(300, 1U)};
  const auto duplicate =
      replay->run(replay_inputs(contracts, quotes, duplicate_orders, {}, fees), config());
  ASSERT_FALSE(duplicate);
  EXPECT_EQ(duplicate.error().code(), ErrorCode::AlreadyExists);

  OptionTopOfBookEvent no_lineage = quote(300, 1U);
  no_lineage.source_identity = {};
  const std::array no_lineage_quotes{no_lineage};
  const auto lineage =
      replay->run(replay_inputs(contracts, no_lineage_quotes, orders, {}, fees), config());
  ASSERT_FALSE(lineage);
  EXPECT_EQ(lineage.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionExecutionReplay, ArithmeticOverflowFailsAndWorkspaceRemainsReusable) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  OptionReplayContract huge = contract();
  huge.multiplier = 1'000'000'000;
  const std::array huge_contracts{huge};
  OptionTopOfBookEvent huge_quote = quote(300, 1U);
  huge_quote.ask = Decimal::from_raw((std::numeric_limits<std::int64_t>::max)());
  const std::array huge_quotes{huge_quote};
  OptionOrderRequest huge_order = order(1U, 2);
  huge_order.limit_price = Decimal::from_raw((std::numeric_limits<std::int64_t>::max)());
  const std::array huge_orders{huge_order};
  const std::array fees{fee()};
  const std::array nano_ticks{tick(0, 10'000, "0.000000001")};

  const auto overflow = replay->run(
      OptionReplayInputs{huge_contracts, huge_quotes, huge_orders, {}, fees, nano_ticks, Decimal{}},
      config());
  ASSERT_FALSE(overflow);
  EXPECT_EQ(overflow.error().code(), ErrorCode::OutOfRange) << overflow.error().to_string();

  const std::array contracts{contract()};
  const std::array quotes{quote(300, 2U)};
  const std::array orders{order(2U, 1)};
  const auto recovered = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), config());
  ASSERT_TRUE(recovered) << recovered.error().to_string();
  ASSERT_EQ(recovered->fills.size(), 1U);
}

TEST(OptionExecutionReplay, WorkspaceLimitsRejectBeforeReplay) {
  OptionReplayLimits invalid = limits();
  invalid.max_workspace_bytes = 1U;

  const auto replay = OptionExecutionReplay::create(invalid);

  ASSERT_FALSE(replay);
  EXPECT_EQ(replay.error().code(), ErrorCode::OutOfRange);
}

TEST(OptionExecutionReplay, WorkspaceByteBoundaryIsDeterministic) {
  OptionReplayLimits exact = limits();
  const auto required = option_replay_required_workspace_bytes(exact);
  ASSERT_TRUE(required) << required.error().to_string();
  ASSERT_GT(*required, 0U);
  exact.max_workspace_bytes = *required;
  EXPECT_TRUE(OptionExecutionReplay::create(exact));

  exact.max_workspace_bytes = *required - 1U;
  const auto too_small = OptionExecutionReplay::create(exact);
  ASSERT_FALSE(too_small);
  EXPECT_EQ(too_small.error().code(), ErrorCode::OutOfRange);
}

TEST(OptionExecutionReplay, SuccessfulWarmRunPerformsNoDynamicAllocation) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U, 10, 2)};
  const std::array orders{order(1U, 2)};
  const std::array fees{fee()};
  const OptionReplayInputs inputs = replay_inputs(contracts, quotes, orders, {}, fees);
  const OptionReplayConfig cfg = config();
  ASSERT_TRUE(replay->run(inputs, cfg));

  option_replay_alloc::g_count.store(0U, std::memory_order_relaxed);
  option_replay_alloc::g_armed.store(true, std::memory_order_relaxed);
  const auto result = replay->run(inputs, cfg);
  option_replay_alloc::g_armed.store(false, std::memory_order_relaxed);

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_EQ(option_replay_alloc::g_count.load(std::memory_order_relaxed), 0U);
}

[[nodiscard]] OptionPanelRow panel_row() {
  OptionPanelRow out;
  out.observation.uid = 1U;
  out.observation.observed_ts_ns = 80;
  out.observation.available_ts_ns = 90;
  out.observation.decision_ts_ns = 100;
  out.observation.execution_ts_ns = 101;
  out.observation.label_end_ts_ns = 200;
  out.observation.signal = 1.0;
  out.observation.forward_pnl = 1.0;
  out.observation.lagged_capital = 1'000.0;
  out.observation.source_identity = identity(1U);
  out.contract_id = 10U;
  out.engine_id.id = 1U;
  out.definition_available_ts_ns = 70;
  out.quote_event_ts_ns = 95;
  out.quote_available_ts_ns = 99;
  out.expiry_ts_ns = 10'000;
  out.strike = 100.0;
  out.multiplier = 100.0;
  out.mark = 10.0;
  out.bid = 9.0;
  out.ask = 11.0;
  out.bid_size_contracts = 10.0;
  out.ask_size_contracts = 10.0;
  out.interval_volume_contracts = 100.0;
  out.lagged_open_interest_contracts = 1'000.0;
  out.adv_contracts = 10'000.0;
  out.return_sigma = 0.2;
  out.vega_per_contract = 10.0;
  out.initial_margin_per_contract = 100.0;
  out.maintenance_margin_per_contract = 80.0;
  out.definition_source_identity = identity(2U);
  out.feature_source_identity = identity(3U);
  out.execution_source_identity = identity(4U);
  return out;
}

TEST(OptionOrderBatch, TargetBookBecomesStrictlyLaterMarketableLimitOrders) {
  const std::array rows{panel_row()};
  const auto panel = OptionResearchPanel::create(rows);
  ASSERT_TRUE(panel) << panel.error().to_string();
  const std::array weights{1.0};
  const std::array<std::int64_t, 1> current{};
  OptionTargetSpec target_spec;
  target_spec.basis = OptionSizingBasis::Vega;
  target_spec.gross_budget = 20.0;
  target_spec.max_position_adv_fraction = 1.0;
  target_spec.available_initial_margin = 10'000.0;
  const auto targets = make_option_target_book(*panel, 0U, weights, current, target_spec);
  ASSERT_TRUE(targets) << targets.error().to_string();
  OptionOrderBatchSpec order_spec;
  order_spec.arrival_latency_ns = 5;
  order_spec.time_in_force = OptionTimeInForce::Day;
  order_spec.expire_ts_ns = 500;

  const auto orders = make_option_order_batch(*panel, 0U, *targets, order_spec);

  ASSERT_TRUE(orders) << orders.error().to_string();
  ASSERT_EQ(orders->size(), 1U);
  EXPECT_EQ(orders->front().quantity_contracts, 2);
  EXPECT_EQ(orders->front().limit_price, money("11.0"));
  EXPECT_EQ(orders->front().decision_ts_ns, 100);
  EXPECT_EQ(orders->front().arrival_ts_ns, 105);
  EXPECT_EQ(orders->front().expire_ts_ns, 500);

  order_spec.limit_offset_bps = Decimal::from_int(1);
  order_spec.limit_price_increment = money("0.05");
  const auto widened = make_option_order_batch(*panel, 0U, *targets, order_spec);
  ASSERT_TRUE(widened) << widened.error().to_string();
  EXPECT_EQ(widened->front().limit_price, money("11.05"));
}

TEST(OptionExecutionReplay, CalibratedScenarioRequiresFrozenCalibrationIdentity) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array quotes{quote(300, 1U)};
  const std::array orders{order(1U, 1)};
  const std::array fees{fee()};
  OptionReplayConfig cfg = config();
  cfg.scenario = OptionReplayScenario::Calibrated;
  cfg.calibration_identity = {};

  const auto missing = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), cfg);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);

  cfg.calibration_identity = identity(999U);
  const auto frozen = replay->run(replay_inputs(contracts, quotes, orders, {}, fees), cfg);
  ASSERT_TRUE(frozen) << frozen.error().to_string();
}

TEST(OptionExecutionReplay, RunManifestBindsImplementationAndMaterialParameters) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  const std::array fees{fee()};
  OptionReplayConfig spoofed = config();
  ++spoofed.model_version;

  const auto rejected = replay->run(replay_inputs(contracts, {}, {}, {}, fees), spoofed);
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);

  OptionReplayConfig cfg = config();
  cfg.allow_locked_market = true;
  cfg.adverse_price_bps = money("1.25");
  cfg.max_quote_age_ns = 777;
  const auto result = replay->run(replay_inputs(contracts, {}, {}, {}, fees), cfg);

  ASSERT_TRUE(result) << result.error().to_string();
  EXPECT_EQ(result->summary.model_version,
            atx::options::execution::kOptionExecutionReplayModelVersion);
  EXPECT_EQ(result->summary.ordering_version,
            atx::options::execution::kOptionExecutionReplayOrderingVersion);
  EXPECT_EQ(result->summary.sequence_validation_identity, cfg.sequence_validation_identity);
  EXPECT_EQ(result->summary.calibration_identity, cfg.calibration_identity);
  EXPECT_EQ(result->summary.displayed_size_fraction, cfg.displayed_size_fraction);
  EXPECT_EQ(result->summary.adverse_price_bps, cfg.adverse_price_bps);
  EXPECT_EQ(result->summary.max_quote_age_ns, cfg.max_quote_age_ns);
  EXPECT_EQ(result->summary.replay_end_ts_ns, cfg.replay_end_ts_ns);
  EXPECT_EQ(result->fee_schedules.size(), 1U);
  EXPECT_EQ(result->tick_schedules.size(), 1U);
}

TEST(OptionExecutionReplay, ConsolidatedL1RequiresFrozenSequenceValidation) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionReplayConfig cfg = config();
  cfg.sequence_continuity_verified = false;

  const auto unverified = replay->run(replay_inputs(contracts, {}, {}, {}, {}), cfg);
  ASSERT_FALSE(unverified);
  EXPECT_EQ(unverified.error().code(), ErrorCode::InvalidArgument);

  cfg.sequence_continuity_verified = true;
  cfg.sequence_validation_identity = {};
  const auto unidentified = replay->run(replay_inputs(contracts, {}, {}, {}, {}), cfg);
  ASSERT_FALSE(unidentified);
  EXPECT_EQ(unidentified.error().code(), ErrorCode::InvalidArgument);
}

TEST(OptionExecutionReplay, StrictScenarioCapsDisplayedParticipation) {
  auto replay = OptionExecutionReplay::create(limits());
  ASSERT_TRUE(replay) << replay.error().to_string();
  const std::array contracts{contract()};
  OptionReplayConfig cfg = config();
  cfg.scenario = OptionReplayScenario::Strict;
  cfg.calibration_identity = {};
  cfg.displayed_size_fraction = money("0.2500001");

  const auto result = replay->run(replay_inputs(contracts, {}, {}, {}, {}), cfg);

  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
