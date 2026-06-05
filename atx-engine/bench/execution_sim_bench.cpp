// execution_sim_bench.cpp — micro-benchmarks for atx::engine::exec::ExecutionSimulator (P2-6).
//
// Measures the per-slice settle throughput (orders/s) and per-order latency
// (ns/order) on a synthetic open set, exercising the full cost pipeline
// (eligibility -> volume cap -> slippage -> √-impact -> commission -> emit). Two
// shapes:
//   - settle_full_cost : every order fills fully in one slice (the common case);
//     the cost is the pricing pipeline + the zero-alloc fill emission.
//   - queue_then_settle : the queue() append + a single settle, to size the
//     end-to-end per-order cost as the loop would drive it each bar.
//
// Build is Debug / clang-cl (the project default), so these are UPPER-BOUND
// latencies, not the optimised figure. Host/build context (CPU, cores, build
// type) is recorded by Google Benchmark's own header and echoed in the report.

#include <array>
#include <span>
#include <vector>

#include <benchmark/benchmark.h>

#include "atx/core/datetime.hpp"
#include "atx/core/decimal.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/exec/execution_sim.hpp"
#include "atx/engine/exec/payloads.hpp"
#include "atx/engine/loop/market.hpp"
#include "atx/engine/loop/panel_types.hpp"
#include "atx/engine/loop/types.hpp"

namespace {

using atx::core::Decimal;
using atx::core::domain::Bar;
using atx::core::domain::Price;
using atx::core::domain::Quantity;
using atx::core::time::Timestamp;
using atx::engine::InstrumentId;
using atx::engine::InstrumentStats;
using atx::engine::Market;
using atx::engine::MarketSlice;
using atx::engine::SliceRow;
using atx::engine::exec::CommissionCfg;
using atx::engine::exec::ExecutionSimulator;
using atx::engine::exec::FillCfg;
using atx::engine::exec::ImpactCfg;
using atx::engine::exec::LatencyCfg;
using atx::engine::exec::OrderPayload;
using atx::engine::exec::OrderType;
using atx::engine::exec::SlippageCfg;
using atx::engine::exec::VolumeCapCfg;

constexpr atx::usize kOrders = 256; // synthetic open-set size per slice

// A book over `kOrders` distinct instruments, each priced at 100 with ample ADV
// and bar volume so the default volume cap never partials the synthetic orders.
struct Fixture {
  std::vector<InstrumentId> universe;
  std::vector<InstrumentStats> stats;
  std::vector<SliceRow> rows;
  Market market;

  Fixture()
      : universe(make_universe()), stats(make_stats()), rows(make_rows()),
        market{std::span<const InstrumentId>{universe}, std::span<const InstrumentStats>{stats}} {
    market.update_prices(
        MarketSlice{Timestamp::from_unix_nanos(100), std::span<const SliceRow>{rows}});
  }

  static std::vector<InstrumentId> make_universe() {
    std::vector<InstrumentId> u;
    u.reserve(kOrders);
    for (atx::u32 i = 0; i < kOrders; ++i) {
      u.push_back(InstrumentId{i + 1U});
    }
    return u;
  }
  static std::vector<InstrumentStats> make_stats() {
    return std::vector<InstrumentStats>(kOrders, InstrumentStats{/*adv=*/1e7, /*sigma=*/0.02,
                                                                 /*spread=*/0.01});
  }
  static std::vector<SliceRow> make_rows() {
    std::vector<SliceRow> r;
    r.reserve(kOrders);
    for (atx::u32 i = 0; i < kOrders; ++i) {
      const Bar b{Timestamp::from_unix_nanos(100),
                  Price::from_int(100),
                  Price::from_int(100),
                  Price::from_int(100),
                  Price::from_int(100),
                  Quantity::from_int(1'000'000)};
      r.push_back(SliceRow{InstrumentId{i + 1U}, b, false});
    }
    return r;
  }
};

[[nodiscard]] std::vector<OrderPayload> make_orders() {
  std::vector<OrderPayload> o;
  o.reserve(kOrders);
  for (atx::u32 i = 0; i < kOrders; ++i) {
    o.push_back(OrderPayload{InstrumentId{i + 1U}, /*qty=*/1'000, OrderType::Market, Decimal{},
                             Timestamp::from_unix_nanos(1000)});
  }
  return o;
}

[[nodiscard]] ExecutionSimulator make_sim() {
  return ExecutionSimulator{FillCfg{},       SlippageCfg{}, ImpactCfg{},
                            CommissionCfg{}, LatencyCfg{},  VolumeCapCfg{/*volume_limit=*/0.5}};
}

// settle_full_cost: re-queue + settle each iteration so steady state is measured;
// the open set is drained fully each settle (orders fill in one slice).
void BM_SettleFullCost(benchmark::State &state) {
  Fixture fx{};
  ExecutionSimulator sim = make_sim();
  const std::vector<OrderPayload> orders = make_orders();
  const Timestamp settle_at = Timestamp::from_unix_nanos(2000);

  for (auto _ : state) {
    sim.queue(std::span<const OrderPayload>{orders}, Timestamp::from_unix_nanos(1000));
    const auto fills = sim.settle_pending(settle_at, fx.market);
    benchmark::DoNotOptimize(fills.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOrders));
}
BENCHMARK(BM_SettleFullCost);

// queue_then_settle: same end-to-end work but reported per order to expose the
// ns/order figure the loop sees each bar.
void BM_QueueThenSettlePerOrder(benchmark::State &state) {
  Fixture fx{};
  ExecutionSimulator sim = make_sim();
  const std::vector<OrderPayload> orders = make_orders();
  const Timestamp settle_at = Timestamp::from_unix_nanos(2000);

  for (auto _ : state) {
    sim.queue(std::span<const OrderPayload>{orders}, Timestamp::from_unix_nanos(1000));
    const auto fills = sim.settle_pending(settle_at, fx.market);
    benchmark::DoNotOptimize(fills.data());
  }
  state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kOrders));
}
BENCHMARK(BM_QueueThenSettlePerOrder);

} // namespace
