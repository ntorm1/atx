// Incremental command/frontier throughput for the persistent options session.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/decimal.hpp"
#include "atx/core/error.hpp"
#include "atx/options/option_execution_replay.hpp"

namespace {

using atx::core::Decimal;
using atx::options::execution::OptionCommandBatch;
using atx::options::execution::OptionExecutionFrontierView;
using atx::options::execution::OptionExecutionSession;
using atx::options::execution::OptionExecutionSessionLimits;
using atx::options::execution::OptionExecutionSessionResult;
using atx::options::execution::OptionFeeSchedule;
using atx::options::execution::OptionMarketOrderKey;
using atx::options::execution::OptionOrderId;
using atx::options::execution::OptionOrderRequest;
using atx::options::execution::OptionReplayConfig;
using atx::options::execution::OptionReplayContract;
using atx::options::execution::OptionReplayInputs;
using atx::options::execution::OptionReplayScenario;
using atx::options::execution::OptionTickSchedule;
using atx::options::execution::OptionTimeInForce;
using atx::options::execution::OptionTopOfBookEvent;
using atx::vol::ArchiveContentIdentity;

constexpr std::size_t kDynamicOrders = 10'000U;
constexpr std::size_t kWideContracts = 10'000U;
constexpr std::size_t kWideFrontiers = 10'000U;
constexpr std::int64_t kFirstDecisionTsNs = 100;

[[nodiscard]] ArchiveContentIdentity identity(std::uint64_t seed) noexcept {
  return ArchiveContentIdentity{100'000U + seed, 200'000U + seed,
                                static_cast<std::uint32_t>(300'000U + seed),
                                static_cast<std::uint32_t>(400'000U + seed)};
}

[[nodiscard]] Decimal money(const char *text) {
  const auto value = Decimal::from_string(text);
  return value.value_or(Decimal{});
}

struct SessionFixture {
  OptionExecutionSessionLimits limits{};
  OptionReplayConfig config{};
  std::vector<OptionReplayContract> contracts;
  std::vector<OptionTopOfBookEvent> quotes;
  std::vector<OptionOrderRequest> orders;
  std::vector<OptionFeeSchedule> fees;
  std::vector<OptionTickSchedule> ticks;

  SessionFixture() {
    limits.replay.max_contracts = 1U;
    limits.replay.max_quote_events = kDynamicOrders;
    limits.replay.max_orders = kDynamicOrders;
    limits.replay.max_cancellations = 1U;
    limits.replay.max_fee_rows = 1U;
    limits.replay.max_tick_rows = 1U;
    limits.replay.max_fills = kDynamicOrders;
    limits.replay.max_workspace_bytes = 256U * 1024U * 1024U;
    limits.max_frontiers = kDynamicOrders;
    limits.max_transitions = kDynamicOrders * 3U;
    limits.max_workspace_bytes = 512U * 1024U * 1024U;

    config.scenario = OptionReplayScenario::Calibrated;
    config.market_data_identity = identity(1U);
    config.sequence_validation_identity = identity(2U);
    config.calibration_identity = identity(3U);
    config.sequence_continuity_verified = true;
    config.displayed_size_fraction = Decimal::from_int(1);
    config.max_quote_age_ns = 1'000'000;
    config.replay_end_ts_ns =
        kFirstDecisionTsNs + static_cast<std::int64_t>(kDynamicOrders * 2U) + 10;

    contracts.push_back(OptionReplayContract{
        1U, {1U}, 100, 0, 1U, 1, 2, config.replay_end_ts_ns + 1'000, identity(4U)});
    fees.push_back(OptionFeeSchedule{
        1U, 0, 0, config.replay_end_ts_ns + 1'000, {}, {}, {}, {}, {}, {}, identity(5U)});
    ticks.push_back(OptionTickSchedule{
        1U, 0, 0, config.replay_end_ts_ns + 1'000, {}, money("0.01"), money("0.01"), identity(6U)});

    quotes.reserve(kDynamicOrders);
    orders.reserve(kDynamicOrders);
    for (std::size_t i = 0U; i < kDynamicOrders; ++i) {
      const std::int64_t decision_ts = kFirstDecisionTsNs + static_cast<std::int64_t>(i * 2U);
      const std::int64_t quote_ts = decision_ts + 2;
      const std::uint64_t sequence = static_cast<std::uint64_t>(i + 1U);
      quotes.push_back(
          OptionTopOfBookEvent{1U,
                               {1U},
                               quote_ts - 1,
                               quote_ts,
                               OptionMarketOrderKey{1U, 1U, 20'260'726U, sequence, 0U, sequence},
                               money("9.00"),
                               money("11.00"),
                               static_cast<std::int64_t>(i + 1U),
                               static_cast<std::int64_t>(i + 1U),
                               7U,
                               8U,
                               true,
                               true,
                               atx::options::execution::OptionQuoteStatus::Firm,
                               false,
                               identity(10U + sequence)});
      orders.push_back(OptionOrderRequest{OptionOrderId{sequence},
                                          1U,
                                          sequence,
                                          1U,
                                          {1U},
                                          1,
                                          money("11.00"),
                                          decision_ts,
                                          decision_ts + 1,
                                          0,
                                          sequence,
                                          1U,
                                          OptionTimeInForce::GoodTillCanceled});
    }
  }

  [[nodiscard]] OptionReplayInputs inputs() const noexcept {
    return OptionReplayInputs{contracts, quotes, {}, {}, fees, ticks, Decimal{}};
  }
};

struct WideSessionFixture {
  OptionExecutionSessionLimits limits{};
  OptionReplayConfig config{};
  std::vector<OptionReplayContract> contracts;
  std::vector<OptionFeeSchedule> fees;
  std::vector<OptionTickSchedule> ticks;

  WideSessionFixture() {
    limits.replay.max_contracts = kWideContracts;
    limits.replay.max_quote_events = 1U;
    limits.replay.max_orders = 1U;
    limits.replay.max_cancellations = 1U;
    limits.replay.max_fee_rows = 1U;
    limits.replay.max_tick_rows = 1U;
    limits.replay.max_fills = 1U;
    limits.replay.max_workspace_bytes = 256U * 1024U * 1024U;
    limits.max_frontiers = kWideFrontiers;
    limits.max_transitions = 1U;
    limits.max_workspace_bytes = 512U * 1024U * 1024U;

    config.market_data_identity = identity(50U);
    config.sequence_validation_identity = identity(51U);
    config.sequence_continuity_verified = true;
    config.max_quote_age_ns = 1'000'000;
    config.replay_end_ts_ns = kFirstDecisionTsNs + static_cast<std::int64_t>(kWideFrontiers) + 10;

    contracts.reserve(kWideContracts);
    for (std::size_t i = 0U; i < kWideContracts; ++i) {
      const std::uint64_t id = static_cast<std::uint64_t>(i + 1U);
      contracts.push_back(OptionReplayContract{id,
                                               {static_cast<std::uint32_t>(id)},
                                               100,
                                               0,
                                               1U,
                                               1,
                                               2,
                                               config.replay_end_ts_ns + 1'000,
                                               identity(100'000U + id)});
    }
    fees.push_back(OptionFeeSchedule{
        1U, 0, 0, config.replay_end_ts_ns + 1'000, {}, {}, {}, {}, {}, {}, identity(52U)});
    ticks.push_back(OptionTickSchedule{1U,
                                       0,
                                       0,
                                       config.replay_end_ts_ns + 1'000,
                                       {},
                                       money("0.01"),
                                       money("0.01"),
                                       identity(53U)});
  }

  [[nodiscard]] OptionReplayInputs inputs() const noexcept {
    return OptionReplayInputs{contracts, {}, {}, {}, fees, ticks, Decimal{}};
  }
};

void BM_ExecutionSessionDynamic10k(benchmark::State &state) {
  const SessionFixture fixture;
  auto created = OptionExecutionSession::create(fixture.limits);
  if (!created) {
    state.SkipWithError(created.error().to_string().c_str());
    return;
  }
  OptionExecutionSession session = std::move(*created);
  const auto run_once = [&]() -> atx::core::Result<OptionExecutionSessionResult> {
    auto status = session.start(fixture.inputs(), fixture.config);
    for (std::size_t i = 0U; status && i < kDynamicOrders; ++i) {
      const std::int64_t frontier = kFirstDecisionTsNs + static_cast<std::int64_t>(i * 2U);
      const auto observed = session.advance_to(frontier);
      if (!observed) {
        status = tl::unexpected<atx::core::Error>{observed.error()};
        break;
      }
      const OptionOrderRequest &order = fixture.orders[i];
      status = session.apply_commands(
          OptionCommandBatch{std::span<const OptionOrderRequest>{&order, 1U}, {}});
    }
    if (!status) {
      return tl::unexpected<atx::core::Error>{status.error()};
    }
    return session.finish();
  };

  const auto verified = run_once();
  if (!verified || verified->replay.fills.size() != kDynamicOrders ||
      verified->transitions.size() != kDynamicOrders * 3U ||
      verified->replay.positions.size() != 1U ||
      verified->replay.positions[0].contracts != static_cast<std::int64_t>(kDynamicOrders)) {
    const std::string error =
        verified ? "session benchmark fixture did not exercise the expected lifecycle"
                 : verified.error().to_string();
    state.SkipWithError(error.c_str());
    return;
  }

  for (auto _ : state) {
    static_cast<void>(_);
    const auto finished = run_once();
    if (!finished) {
      state.SkipWithError(finished.error().to_string().c_str());
      break;
    }
    std::uint64_t trace = finished->session_summary.command_trace_hash;
    benchmark::DoNotOptimize(trace);
  }
  const double completed_commands =
      static_cast<double>(state.iterations()) * static_cast<double>(kDynamicOrders);
  state.counters["commands_per_second"] =
      benchmark::Counter(completed_commands, benchmark::Counter::kIsRate);
  state.counters["frontiers_per_second"] =
      benchmark::Counter(completed_commands, benchmark::Counter::kIsRate);
}

BENCHMARK(BM_ExecutionSessionDynamic10k);

void BM_ExecutionSessionWideCatalogNoop10k(benchmark::State &state) {
  const WideSessionFixture fixture;
  auto created = OptionExecutionSession::create(fixture.limits);
  if (!created) {
    state.SkipWithError(created.error().to_string().c_str());
    return;
  }
  OptionExecutionSession session = std::move(*created);
  const auto run_once = [&]() -> atx::core::Result<OptionExecutionSessionResult> {
    ATX_TRY_VOID(session.start(fixture.inputs(), fixture.config));
    for (std::size_t i = 0U; i < kWideFrontiers; ++i) {
      const std::int64_t frontier = kFirstDecisionTsNs + static_cast<std::int64_t>(i);
      ATX_TRY(OptionExecutionFrontierView observed, session.advance_to(frontier));
      benchmark::DoNotOptimize(observed.frontier_ts_ns);
      ATX_TRY_VOID(session.apply_commands({}));
    }
    return session.finish();
  };

  const auto verified = run_once();
  if (!verified || verified->session_summary.frontier_count != kWideFrontiers ||
      !verified->transitions.empty()) {
    const std::string error =
        verified ? "wide session benchmark fixture did not exercise the expected lifecycle"
                 : verified.error().to_string();
    state.SkipWithError(error.c_str());
    return;
  }

  for (auto _ : state) {
    static_cast<void>(_);
    const auto finished = run_once();
    if (!finished) {
      state.SkipWithError(finished.error().to_string().c_str());
      break;
    }
    std::uint64_t trace = finished->session_summary.command_trace_hash;
    benchmark::DoNotOptimize(trace);
  }
  const double completed_frontiers =
      static_cast<double>(state.iterations()) * static_cast<double>(kWideFrontiers);
  state.counters["frontiers_per_second"] =
      benchmark::Counter(completed_frontiers, benchmark::Counter::kIsRate);
}

BENCHMARK(BM_ExecutionSessionWideCatalogNoop10k);

} // namespace
