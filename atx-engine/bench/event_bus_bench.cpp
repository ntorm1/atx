// event_bus_bench.cpp — micro-benchmarks for atx::engine::EventBus (P1-3).
//
// Measures ns/op for the engine event-spine hot paths, all SINGLE-THREADED (the
// backtest cadence) so the numbers isolate the facade's per-event cost over the
// already-benchmarked atx-core ring (see atx-core/bench/disruptor_bench.cpp for
// the cross-thread hand-off throughput):
//   - publish        : claim_slot + fill-in-place + publish, draining as we go so
//                      the ring never wraps into a stalled consumer (steady state).
//   - drain dispatch : drain a pre-published batch through drain_in_order (SC).
//   - drain SP->2C   : the same drain fanned out to two registered consumers.
//
// Host/build context (CPU, cores, build type) is recorded by Google Benchmark's
// own header and echoed in the agent report. Build is Debug / clang-cl, so these
// are upper-bound latencies, not the optimised figure.

#include <atx/engine/bus/event_bus.hpp>
#include <atx/engine/event/event.hpp>

#include <memory>

#include <benchmark/benchmark.h>

using atx::i64;
using atx::usize;
using atx::engine::EventBus;
using atx::engine::event::Event;
using atx::engine::event::EventType;

namespace {

constexpr usize kCap = 1024; // ring capacity used across cases

// Fill a freshly-claimed slot with a cheap, representative event so the measured
// work includes the in-place payload write (the producer's real per-event cost).
ATX_FORCE_INLINE void fill(Event &e, i64 i) {
  e.event_ts = atx::core::time::Timestamp::from_unix_nanos(i);
  e.type = EventType::Market;
  // Two i64 stores into the payload buffer — stands in for a MarketPayload write.
  struct Probe {
    i64 a;
    i64 b;
  };
  e.store_payload(Probe{.a = i, .b = i ^ 0x5a5a5a5a});
}

// --- publish: claim_slot + fill + publish, draining to keep the ring hot -------
void BM_EventBus_Publish(benchmark::State &state) {
  auto bus = std::make_unique<EventBus<kCap, 1>>();
  auto consumer = bus->add_consumer(0);
  i64 i = 0;
  i64 next = 0; // consumer cursor (drain just enough to avoid wrap-stall)
  for (auto _ : state) {
    i64 seq = 0;
    Event &slot = bus->claim_slot(seq);
    fill(slot, i);
    bus->publish(seq);
    // Drain in lock-step so the single producer never laps consumer 0.
    const i64 hi = consumer.wait_for(next);
    i64 sink = 0;
    for (i64 s = next; s <= hi; ++s) {
      sink += bus->at(s).event_ts.unix_nanos();
    }
    benchmark::DoNotOptimize(sink);
    consumer.consumed(hi);
    next = hi + 1;
    ++i;
  }
  state.SetItemsProcessed(state.iterations());
}

// --- drain dispatch: drain a pre-published batch (SC) --------------------------
void BM_EventBus_Drain_SC(benchmark::State &state) {
  const i64 batch = state.range(0);
  auto bus = std::make_unique<EventBus<kCap, 1>>();
  (void)bus->add_consumer(0); // drain_in_order drives consumer 0; no handle needed
  i64 base = 0;
  for (auto _ : state) {
    state.PauseTiming();
    // Publish a fresh batch (<= capacity) and rewind the consumer each iteration
    // so every timed drain processes exactly `batch` events from a hot ring.
    for (i64 j = 0; j < batch; ++j) {
      i64 seq = 0;
      Event &slot = bus->claim_slot(seq);
      fill(slot, base + j);
      bus->publish(seq);
    }
    state.ResumeTiming();

    i64 sink = 0;
    // drain_in_order advances consumer 0's gate, so the next iteration's publish
    // batch (<= capacity) never wrap-stalls.
    bus->drain_in_order([&](usize, const Event &e) { sink += e.event_ts.unix_nanos(); });
    benchmark::DoNotOptimize(sink);

    state.PauseTiming();
    base += batch; // keep event tags monotone across iterations
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * batch);
}

// --- drain SP -> 2 consumers ---------------------------------------------------
void BM_EventBus_Drain_2C(benchmark::State &state) {
  const i64 batch = state.range(0);
  auto bus = std::make_unique<EventBus<kCap, 2>>();
  (void)bus->add_consumer(0);
  (void)bus->add_consumer(1);
  i64 base = 0;
  for (auto _ : state) {
    state.PauseTiming();
    for (i64 j = 0; j < batch; ++j) {
      i64 seq = 0;
      Event &slot = bus->claim_slot(seq);
      fill(slot, base + j);
      bus->publish(seq);
    }
    state.ResumeTiming();

    i64 sink = 0;
    bus->drain_in_order(
        [&](usize c, const Event &e) { sink += static_cast<i64>(c) + e.event_ts.unix_nanos(); });
    benchmark::DoNotOptimize(sink);

    state.PauseTiming();
    base += batch;
    state.ResumeTiming();
  }
  state.SetItemsProcessed(state.iterations() * batch * 2);
}

} // namespace

BENCHMARK(BM_EventBus_Publish)->UseRealTime();
BENCHMARK(BM_EventBus_Drain_SC)->Arg(256)->UseRealTime();
BENCHMARK(BM_EventBus_Drain_2C)->Arg(256)->UseRealTime();
