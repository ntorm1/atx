// backtest_loop_bench.cpp — end-to-end BacktestLoop throughput (P2-8).
//
// Measures the full per-slice crank — drain -> mark -> settle -> panel -> signal
// -> weights -> reconcile -> queue -> sample — over a synthetic multi-symbol feed
// with a fully-engaged cost stack and a rotating signal so EVERY slice rebalances
// and settles (the active, representative case, not a warm-up-then-idle book).
//
// Reported: items/s as bars·symbols/s (the data rate the loop sustains) plus a
// slices/s counter. Each iteration reconstructs the consumable spine (the feed and
// signal source are single-use), so setup is INSIDE the timed region — honest for
// an "end-to-end" figure and amortised by the feed size. Build is Debug / clang-cl
// (the project default): UPPER-BOUND latencies, not the optimised number. Host /
// build context is recorded by Google Benchmark's own header.

#include <memory>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/decimal.hpp"
#include "atx/core/domain/domain.hpp"
#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/loop/backtest_loop.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/rolling_panel.hpp"
#include "atx/engine/loop/signal_source.hpp"
#include "atx/engine/loop/types.hpp"
#include "atx/engine/loop/weight_policy.hpp"
#include "atx/engine/portfolio/portfolio.hpp"

namespace {

using atx::f64;
using atx::i64;
using atx::usize;
using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::domain::Symbol;
using atx::engine::BacktestLoop;
using atx::engine::EventBus;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::Portfolio;
using atx::engine::RollingPanel;
using atx::engine::Schedule;
using atx::engine::ScriptedSignalSource;
using atx::engine::SimClock;
using atx::engine::Universe;
using atx::engine::WeightPolicy;
using atx::engine::data::BarRow;
using atx::engine::data::InMemoryBarFeed;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::VolumeCapCfg;
using Bus = EventBus<>;
using Timestamp = atx::core::time::Timestamp;

constexpr usize kCap = 8; // panel ring (>= max_lookback=1)
constexpr atx::u32 kSymbols = 50;
constexpr int kBars = 252; // one trading year

// Per-symbol constant-price sources (one knowledge_ts-sorted vector each).
struct Feed {
  std::vector<InstrumentId> universe;
  std::vector<std::vector<BarRow>> per_symbol;
  std::vector<std::span<const BarRow>> spans;

  Feed() {
    universe.reserve(kSymbols);
    per_symbol.resize(kSymbols);
    for (atx::u32 s = 0; s < kSymbols; ++s) {
      universe.push_back(InstrumentId{s + 1U});
      auto &rows = per_symbol[s];
      rows.reserve(kBars);
      for (int k = 1; k <= kBars; ++k) {
        Bar bar{};
        bar.ts = Timestamp::from_unix_nanos(k);
        const i64 px = 100 + static_cast<i64>(s); // distinct per symbol
        bar.open = Price::from_int(px);
        bar.high = Price::from_int(px);
        bar.low = Price::from_int(px);
        bar.close = Price::from_int(px);
        bar.volume = Quantity::from_int(10'000'000);
        rows.push_back(BarRow{Symbol{s + 1U}, bar, Timestamp::from_unix_nanos(k), false});
      }
    }
    spans.reserve(kSymbols);
    for (const auto &rows : per_symbol) {
      spans.push_back(std::span<const BarRow>{rows});
    }
  }
};

// A schedule whose per-rebalance vector ROTATES the scores so the cross-sectional
// ranking shifts every slice -> the policy retargets and the exec sim settles each
// slice (the loop is never idle). Built once; copied into the source per run.
[[nodiscard]] std::vector<std::vector<f64>> rotating_schedule() {
  std::vector<std::vector<f64>> sched;
  sched.reserve(static_cast<usize>(kBars));
  for (int t = 0; t < kBars; ++t) {
    std::vector<f64> v(kSymbols);
    for (atx::u32 i = 0; i < kSymbols; ++i) {
      v[i] = static_cast<f64>((i + static_cast<atx::u32>(t)) % kSymbols);
    }
    sched.push_back(std::move(v));
  }
  return sched;
}

[[nodiscard]] ExecutionSimulator make_sim() {
  return ExecutionSimulator{FillCfg{},       SlippageCfg{}, ImpactCfg{},
                            CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{/*volume_limit=*/0.5}};
}

void BM_BacktestLoop_EndToEnd(benchmark::State &state) {
  const Feed feed_data{};
  const std::vector<std::vector<f64>> schedule = rotating_schedule();
  const std::span<const std::span<const BarRow>> sources{feed_data.spans};
  const std::vector<InstrumentStats> stats(
      kSymbols, InstrumentStats{/*adv=*/1.0e6, /*sigma=*/0.02, /*spread=*/0.05});

  for (auto _ : state) {
    auto bus = std::make_unique<Bus>();
    SimClock clock;
    InMemoryBarFeed feed{sources, clock, *bus};
    RollingPanel<kCap> panel{std::span<const InstrumentId>{feed_data.universe}, 1};
    ScriptedSignalSource src{schedule, kSymbols, 1};
    const WeightPolicy policy{};
    ExecutionSimulator sim = make_sim();
    Portfolio pf{Decimal::from_int(10'000'000), std::span<const InstrumentId>{feed_data.universe}};
    Market market{std::span<const InstrumentId>{feed_data.universe},
                  std::span<const InstrumentStats>{stats}};
    BacktestLoop<kCap> loop{feed,       clock, *bus, panel,  src,
                            policy,     sim,   pf,   market, Universe{feed_data.universe},
                            Schedule{1}};
    auto result = loop.run();
    benchmark::DoNotOptimize(result); // non-const lvalue: the mutable-ref overload
    benchmark::ClobberMemory();
  }

  // bars·symbols processed per second (the data rate the crank sustains).
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kBars) *
                          static_cast<int64_t>(kSymbols));
  state.counters["slices_per_s"] = benchmark::Counter(
      static_cast<double>(state.iterations()) * kBars, benchmark::Counter::kIsRate);
}
BENCHMARK(BM_BacktestLoop_EndToEnd);

} // namespace
