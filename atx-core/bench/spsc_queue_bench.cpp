// spsc_queue_bench.cpp — micro-benchmarks for the wait-free SPSC ring buffer.
//
// Benchmarks:
//   BM_SpscQueueRoundTrip — single-thread push+pop steady state (no contention,
//                           measures the bare op cost: mask, placement-new,
//                           release store).
//   BM_SpscQueueThroughput — producer thread + consumer (the benchmark thread),
//                            measuring cross-core handoff throughput.

#include <benchmark/benchmark.h>

#include <thread>

#include <atx/core/concurrent/spsc_queue.hpp>

using atx::core::concurrent::SpscQueue;

// ---------------------------------------------------------------------------
// BM_SpscQueueRoundTrip — uncontended single-thread push then pop.
// ---------------------------------------------------------------------------
static void BM_SpscQueueRoundTrip(benchmark::State& state) {
    SpscQueue<int, 1024> q;
    int counter = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(q.try_push(counter++));
        int v = 0;
        benchmark::DoNotOptimize(q.try_pop(v));
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SpscQueueRoundTrip);

// ---------------------------------------------------------------------------
// BM_SpscQueueThroughput — one producer thread feeds the consumer (this thread).
// Reports items/sec of genuine cross-thread handoff.
// ---------------------------------------------------------------------------
static void BM_SpscQueueThroughput(benchmark::State& state) {
    SpscQueue<int, 1024> q;
    for (auto _ : state) {
        constexpr int kItems = 100'000;
        std::thread prod([&] {
            for (int i = 0; i < kItems;) {
                if (q.try_push(i)) {
                    ++i;
                }
            }
        });
        int received = 0;
        while (received < kItems) {
            int v = 0;
            if (q.try_pop(v)) {
                ++received;
            }
        }
        prod.join();
        state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kItems));
    }
}
BENCHMARK(BM_SpscQueueThroughput)->UseRealTime();
