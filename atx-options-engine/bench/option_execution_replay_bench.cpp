// Deterministic throughput baselines for the Consolidated-L1 execution replay.
//
// Both cases build and validate immutable inputs before timing, then reuse one
// capacity-bounded replay workspace across iterations. "events_per_second"
// counts order submissions plus quote events; "fills_per_second" counts emitted
// fills. Contract expiries are beyond the replay horizon in these fixtures.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/decimal.hpp"
#include "atx/options/option_execution_replay.hpp"

namespace {

using atx::core::Decimal;
using atx::options::execution::OptionExecutionReplay;
using atx::options::execution::OptionFeeSchedule;
using atx::options::execution::OptionMarketOrderKey;
using atx::options::execution::OptionOrderDisposition;
using atx::options::execution::OptionOrderId;
using atx::options::execution::OptionOrderRequest;
using atx::options::execution::OptionQuoteStatus;
using atx::options::execution::OptionReplayConfig;
using atx::options::execution::OptionReplayContract;
using atx::options::execution::OptionReplayInputs;
using atx::options::execution::OptionReplayLimits;
using atx::options::execution::OptionReplayScenario;
using atx::options::execution::OptionReplayView;
using atx::options::execution::OptionTickSchedule;
using atx::options::execution::OptionTimeInForce;
using atx::options::execution::OptionTopOfBookEvent;
using atx::vol::ArchiveContentIdentity;

constexpr std::size_t kWideContracts = 1'024U;
constexpr std::size_t kPartialContracts = 64U;
constexpr std::size_t kPartialOrdersPerContract = 2U;
constexpr std::size_t kPartialQuoteRounds = 4U;
constexpr std::size_t kConcentratedOrders = 100'000U;
constexpr std::int64_t kDecisionTsNs = 100;
constexpr std::int64_t kArrivalTsNs = 200;
constexpr std::int64_t kFirstQuoteTsNs = 300;
constexpr std::int64_t kReplayEndTsNs = 1'000;
constexpr std::int64_t kContractExpiryTsNs = 2'000;

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) noexcept {
  return ArchiveContentIdentity{1'000U + seed, 2'000U + seed,
                                static_cast<std::uint32_t>(3'000U + seed),
                                static_cast<std::uint32_t>(4'000U + seed)};
}

[[nodiscard]] OptionReplayContract make_contract(std::size_t ordinal) noexcept {
  const auto engine_id = static_cast<std::uint32_t>(ordinal + 1U);
  OptionReplayContract contract;
  contract.contract_id = static_cast<std::uint64_t>(ordinal + 1U);
  contract.engine_id.id = engine_id;
  contract.multiplier = 100;
  contract.tick_schedule_key = 1U;
  contract.definition_effective_ts_ns = 1;
  contract.definition_available_ts_ns = 2;
  contract.expiry_ts_ns = kContractExpiryTsNs;
  contract.definition_source_identity = identity(10'000U + static_cast<std::uint64_t>(ordinal));
  return contract;
}

[[nodiscard]] OptionOrderRequest make_order(std::size_t contract_ordinal, std::uint64_t order_id,
                                            std::int64_t quantity) noexcept {
  OptionOrderRequest order;
  order.order_id = OptionOrderId{order_id};
  order.strategy_id = 1U;
  order.basket_id = 1U;
  order.contract_id = static_cast<std::uint64_t>(contract_ordinal + 1U);
  order.engine_id.id = static_cast<std::uint32_t>(contract_ordinal + 1U);
  order.quantity_contracts = quantity;
  order.limit_price = Decimal::from_int(11);
  order.decision_ts_ns = kDecisionTsNs;
  order.arrival_ts_ns = kArrivalTsNs;
  order.priority_sequence = order_id;
  order.fee_schedule_key = 1U;
  order.time_in_force = OptionTimeInForce::GoodTillCanceled;
  return order;
}

[[nodiscard]] OptionTopOfBookEvent make_quote(std::size_t contract_ordinal,
                                              std::int64_t available_ts_ns, std::uint64_t sequence,
                                              std::int64_t ask_size) noexcept {
  OptionTopOfBookEvent quote;
  quote.contract_id = static_cast<std::uint64_t>(contract_ordinal + 1U);
  quote.engine_id.id = static_cast<std::uint32_t>(contract_ordinal + 1U);
  quote.quote_event_ts_ns = available_ts_ns - 1;
  quote.available_ts_ns = available_ts_ns;
  quote.order_key = OptionMarketOrderKey{1U, 1U, 20'260'726U, sequence, 0U, sequence};
  quote.bid = Decimal::from_int(9);
  quote.ask = Decimal::from_int(11);
  quote.bid_size_contracts = 1'000;
  quote.ask_size_contracts = ask_size;
  quote.bid_participant_id = 1U;
  quote.ask_participant_id = 2U;
  quote.status = OptionQuoteStatus::Firm;
  quote.source_identity = identity(20'000U + sequence);
  return quote;
}

[[nodiscard]] OptionFeeSchedule make_zero_fee_schedule() noexcept {
  OptionFeeSchedule schedule;
  schedule.key = 1U;
  schedule.effective_until_ts_ns = kContractExpiryTsNs;
  schedule.source_identity = identity(30'000U);
  return schedule;
}

[[nodiscard]] OptionTickSchedule make_tick_schedule() noexcept {
  OptionTickSchedule schedule;
  schedule.key = 1U;
  schedule.effective_until_ts_ns = kContractExpiryTsNs;
  schedule.tick_below_threshold = Decimal::from_raw(1'000'000);
  schedule.tick_at_or_above_threshold = Decimal::from_raw(1'000'000);
  schedule.source_identity = identity(35'000U);
  return schedule;
}

[[nodiscard]] OptionReplayConfig make_config() noexcept {
  OptionReplayConfig config;
  config.scenario = OptionReplayScenario::Calibrated;
  config.market_data_identity = identity(40'000U);
  config.sequence_validation_identity = identity(40'002U);
  config.sequence_continuity_verified = true;
  config.calibration_identity = identity(40'001U);
  config.displayed_size_fraction = Decimal::from_int(1);
  config.max_quote_age_ns = 1'000;
  config.replay_end_ts_ns = kReplayEndTsNs;
  return config;
}

struct ReplayFixture {
  std::vector<OptionReplayContract> contracts;
  std::vector<OptionTopOfBookEvent> quotes;
  std::vector<OptionOrderRequest> orders;
  std::vector<OptionFeeSchedule> fees;
  std::vector<OptionTickSchedule> ticks;
  OptionReplayConfig config;
  OptionReplayLimits limits;
  std::size_t expected_fills{0U};
  std::size_t expected_filled_contracts{0U};
  std::int64_t expected_fill_quantity{0};
  std::int64_t expected_position_per_contract{0};
  bool expect_reset_pool_per_fill{true};

  [[nodiscard]] OptionReplayInputs inputs() const noexcept {
    return OptionReplayInputs{std::span<const OptionReplayContract>{contracts},
                              std::span<const OptionTopOfBookEvent>{quotes},
                              std::span<const OptionOrderRequest>{orders},
                              {},
                              std::span<const OptionFeeSchedule>{fees},
                              std::span<const OptionTickSchedule>{ticks},
                              Decimal{}};
  }

  [[nodiscard]] std::size_t event_count() const noexcept { return quotes.size() + orders.size(); }
};

[[nodiscard]] ReplayFixture make_wide_fixture() {
  ReplayFixture fixture;
  fixture.contracts.reserve(kWideContracts);
  fixture.quotes.reserve(kWideContracts);
  fixture.orders.reserve(kWideContracts);
  for (std::size_t i = 0U; i < kWideContracts; ++i) {
    fixture.contracts.push_back(make_contract(i));
    fixture.orders.push_back(make_order(i, static_cast<std::uint64_t>(i + 1U), 1));
    fixture.quotes.push_back(make_quote(i, kFirstQuoteTsNs, static_cast<std::uint64_t>(i + 1U), 1));
  }
  fixture.fees.push_back(make_zero_fee_schedule());
  fixture.ticks.push_back(make_tick_schedule());
  fixture.config = make_config();
  fixture.limits.max_contracts = kWideContracts;
  fixture.limits.max_quote_events = kWideContracts;
  fixture.limits.max_orders = kWideContracts;
  fixture.limits.max_cancellations = 1U;
  fixture.limits.max_fee_rows = 1U;
  fixture.limits.max_tick_rows = 1U;
  fixture.limits.max_fills = kWideContracts;
  fixture.limits.max_workspace_bytes = 64U * 1024U * 1024U;
  fixture.expected_fills = kWideContracts;
  fixture.expected_filled_contracts = kWideContracts;
  fixture.expected_fill_quantity = 1;
  fixture.expected_position_per_contract = 1;
  return fixture;
}

[[nodiscard]] ReplayFixture make_partial_fixture() {
  ReplayFixture fixture;
  const std::size_t order_count = kPartialContracts * kPartialOrdersPerContract;
  const std::size_t quote_count = kPartialContracts * kPartialQuoteRounds;
  fixture.contracts.reserve(kPartialContracts);
  fixture.orders.reserve(order_count);
  fixture.quotes.reserve(quote_count);

  for (std::size_t contract = 0U; contract < kPartialContracts; ++contract) {
    fixture.contracts.push_back(make_contract(contract));
    for (std::size_t order = 0U; order < kPartialOrdersPerContract; ++order) {
      const std::size_t ordinal = contract * kPartialOrdersPerContract + order;
      fixture.orders.push_back(make_order(contract, static_cast<std::uint64_t>(ordinal + 1U), 8));
    }
  }
  for (std::size_t round = 0U; round < kPartialQuoteRounds; ++round) {
    for (std::size_t contract = 0U; contract < kPartialContracts; ++contract) {
      const std::size_t ordinal = round * kPartialContracts + contract;
      fixture.quotes.push_back(make_quote(
          contract, kFirstQuoteTsNs + static_cast<std::int64_t>(round),
          static_cast<std::uint64_t>(ordinal + 1U), static_cast<std::int64_t>((round + 1U) * 4U)));
    }
  }

  fixture.fees.push_back(make_zero_fee_schedule());
  fixture.ticks.push_back(make_tick_schedule());
  fixture.config = make_config();
  fixture.limits.max_contracts = kPartialContracts;
  fixture.limits.max_quote_events = quote_count;
  fixture.limits.max_orders = order_count;
  fixture.limits.max_cancellations = 1U;
  fixture.limits.max_fee_rows = 1U;
  fixture.limits.max_tick_rows = 1U;
  fixture.limits.max_fills = quote_count;
  fixture.limits.max_workspace_bytes = 32U * 1024U * 1024U;
  fixture.expected_fills = quote_count;
  fixture.expected_filled_contracts = kPartialContracts * kPartialOrdersPerContract * 8U;
  fixture.expected_fill_quantity = 4;
  fixture.expected_position_per_contract = 16;
  return fixture;
}

[[nodiscard]] ReplayFixture make_concentrated_fixture() {
  ReplayFixture fixture;
  fixture.contracts.push_back(make_contract(0U));
  fixture.orders.reserve(kConcentratedOrders);
  for (std::size_t i = 0U; i < kConcentratedOrders; ++i) {
    fixture.orders.push_back(make_order(0U, static_cast<std::uint64_t>(i + 1U), 1));
  }
  fixture.quotes.push_back(
      make_quote(0U, kFirstQuoteTsNs, 1U, static_cast<std::int64_t>(kConcentratedOrders)));
  fixture.fees.push_back(make_zero_fee_schedule());
  fixture.ticks.push_back(make_tick_schedule());
  fixture.config = make_config();
  fixture.limits.max_contracts = 1U;
  fixture.limits.max_quote_events = 1U;
  fixture.limits.max_orders = kConcentratedOrders;
  fixture.limits.max_cancellations = 1U;
  fixture.limits.max_fee_rows = 1U;
  fixture.limits.max_tick_rows = 1U;
  fixture.limits.max_fills = kConcentratedOrders;
  fixture.limits.max_workspace_bytes = 256U * 1024U * 1024U;
  fixture.expected_fills = kConcentratedOrders;
  fixture.expected_filled_contracts = kConcentratedOrders;
  fixture.expected_fill_quantity = 1;
  fixture.expected_position_per_contract = static_cast<std::int64_t>(kConcentratedOrders);
  fixture.expect_reset_pool_per_fill = false;
  return fixture;
}

[[nodiscard]] bool verify_result(const OptionReplayView &view,
                                 const ReplayFixture &fixture) noexcept {
  if (view.fills.size() != fixture.expected_fills || view.orders.size() != fixture.orders.size() ||
      view.positions.size() != fixture.contracts.size() ||
      view.summary.quote_events != fixture.quotes.size() ||
      view.summary.filled_contracts != fixture.expected_filled_contracts) {
    return false;
  }
  const bool all_orders_filled =
      std::all_of(view.orders.begin(), view.orders.end(), [](const auto &audit) {
        return audit.disposition == OptionOrderDisposition::Filled &&
               audit.remaining_contracts == 0;
      });
  const bool fill_quantities_match =
      std::all_of(view.fills.begin(), view.fills.end(), [&fixture](const auto &fill) {
        return fill.quantity_contracts == fixture.expected_fill_quantity &&
               (!fixture.expect_reset_pool_per_fill ||
                (fill.displayed_size_before_contracts == fixture.expected_fill_quantity &&
                 fill.displayed_size_after_contracts == 0));
      });
  const bool positions_match =
      std::all_of(view.positions.begin(), view.positions.end(), [&fixture](const auto &position) {
        return position.contracts == fixture.expected_position_per_contract;
      });
  const bool shared_pool_match =
      fixture.expect_reset_pool_per_fill ||
      (!view.fills.empty() &&
       view.fills.front().displayed_size_before_contracts ==
           static_cast<std::int64_t>(fixture.expected_filled_contracts) &&
       view.fills.back().displayed_size_after_contracts == 0);
  return all_orders_filled && fill_quantities_match && positions_match && shared_pool_match;
}

void run_replay_benchmark(benchmark::State &state, const ReplayFixture &fixture) {
  auto workspace_result = OptionExecutionReplay::create(fixture.limits);
  if (!workspace_result) {
    const std::string message = workspace_result.error().to_string();
    state.SkipWithError(message.c_str());
    return;
  }
  OptionExecutionReplay workspace = std::move(workspace_result.value());

  const OptionReplayInputs inputs = fixture.inputs();
  const auto validation = workspace.run(inputs, fixture.config);
  if (!validation || !verify_result(validation.value(), fixture)) {
    const std::string message = validation ? "option replay benchmark fixture verification failed"
                                           : validation.error().to_string();
    state.SkipWithError(message.c_str());
    return;
  }

  for (auto _ : state) {
    auto result = workspace.run(inputs, fixture.config);
    if (!result) {
      const std::string message = result.error().to_string();
      state.SkipWithError(message.c_str());
      break;
    }
    benchmark::DoNotOptimize(result->fills.data());
    benchmark::DoNotOptimize(result->summary.filled_contracts);
    benchmark::ClobberMemory();
  }

  const double iterations = static_cast<double>(state.iterations());
  state.counters["events_per_second"] = benchmark::Counter(
      iterations * static_cast<double>(fixture.event_count()), benchmark::Counter::kIsRate);
  state.counters["fills_per_second"] = benchmark::Counter(
      iterations * static_cast<double>(fixture.expected_fills), benchmark::Counter::kIsRate);
}

void BM_ReplayWideFullFill(benchmark::State &state) {
  const ReplayFixture fixture = make_wide_fixture();
  run_replay_benchmark(state, fixture);
}
BENCHMARK(BM_ReplayWideFullFill);

void BM_ReplayRepeatedPartialFills(benchmark::State &state) {
  const ReplayFixture fixture = make_partial_fixture();
  run_replay_benchmark(state, fixture);
}
BENCHMARK(BM_ReplayRepeatedPartialFills);

void BM_ReplayConcentratedBook100k(benchmark::State &state) {
  const ReplayFixture fixture = make_concentrated_fixture();
  run_replay_benchmark(state, fixture);
}
BENCHMARK(BM_ReplayConcentratedBook100k);

} // namespace
