// mpmc_queue_bench.cpp — micro-benchmarks for the Vyukov bounded MPMC queue.
//
// Benchmarks:
//   BM_MpmcQueueRoundTrip — single-thread push+pop steady state (bare CAS path).
//   BM_MpmcQueueContended — N producers + N consumers, cross-core throughput.

#include <benchmark/benchmark.h>

#include <atomic>
#include <thread>
#include <vector>

#include <atx/core/concurrent/mpmc_queue.hpp>

using atx::core::concurrent::MpmcQueue;

// ---------------------------------------------------------------------------
// BM_MpmcQueueRoundTrip — uncontended single-thread push then pop.
// ---------------------------------------------------------------------------
static void BM_MpmcQueueRoundTrip(benchmark::State& state) {
    MpmcQueue<int, 1024> q;
    int counter = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(q.try_push(counter++));
        int v = 0;
        benchmark::DoNotOptimize(q.try_pop(v));
        benchmark::DoNotOptimize(v);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MpmcQueueRoundTrip);

// ---------------------------------------------------------------------------
// BM_MpmcQueueContended — `threads` producers and `threads` consumers move a
// fixed number of items through the queue. Reports aggregate items/sec.
// ---------------------------------------------------------------------------
static void BM_MpmcQueueContended(benchmark::State& state) {
    const int threads = static_cast<int>(state.range(0));
    constexpr int kPerProducer = 50'000;
    for (auto _ : state) {
        MpmcQueue<int, 1024> q;
        std::atomic<int> consumed{0};
        const int total = threads * kPerProducer;
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(threads) * 2U);
        for (int p = 0; p < threads; ++p) {
            pool.emplace_back([&, p] {
                for (int i = 0; i < kPerProducer;) {
                    if (q.try_push(p * kPerProducer + i)) {
                        ++i;
                    }
                }
            });
        }
        for (int c = 0; c < threads; ++c) {
            pool.emplace_back([&] {
                int v = 0;
                while (consumed.load(std::memory_order_relaxed) < total) {
                    if (q.try_pop(v)) {
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }
        for (auto& t : pool) {
            t.join();
        }
        state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(total));
    }
}
BENCHMARK(BM_MpmcQueueContended)->Arg(2)->Arg(4)->UseRealTime();
