// disruptor_bench.cpp — throughput micro-benchmarks for the LMAX-pattern ring.
//
// Measures items/s and ns/op for:
//   - SP -> SC   : single producer, single consumer (the hot path).
//   - SP -> 2C   : single producer fanned out to two independent consumers.
//   - MP(2) -> SC: two producers (CAS claim) into one consumer.
//   - Baseline   : std::mutex + std::queue SPSC, to quantify the lock-free win.
//
// Each case runs the producer on a separate thread and drains on the benchmark
// thread, so the reported rate is end-to-end hand-off throughput. We report a
// per-item rate via benchmark::Counter so the numbers are directly comparable.
//
// Host/build context is recorded by Google Benchmark's own header (CPU, cores,
// build type) and echoed in the agent report.

#include <atx/core/concurrent/disruptor.hpp>

#include <array>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>

#include <benchmark/benchmark.h>

using atx::i64;
using atx::usize;
using namespace atx::core::concurrent; // NOLINT(google-build-using-namespace)

namespace {

constexpr usize kCap = 1024; // ring capacity used across cases

// --- SP -> SC ---------------------------------------------------------------
void BM_Disruptor_SPSC(benchmark::State& state) {
    const i64 n = state.range(0);
    for (auto _ : state) {
        Disruptor<i64, kCap> d;
        std::thread prod([&] {
            for (i64 i = 0; i < n; ++i) {
                const i64 s = d.claim();
                d.at(s)    = i;
                d.publish(s);
            }
        });
        i64 sink = 0;
        i64 next = 0;
        while (next < n) {
            const i64 hi = d.wait_for(next);
            for (i64 s = next; s <= hi; ++s) {
                sink += d.at(s);
            }
            d.consumed(hi);
            next = hi + 1;
        }
        benchmark::DoNotOptimize(sink);
        prod.join();
    }
    state.SetItemsProcessed(state.iterations() * n);
    state.counters["items_per_s"] =
        benchmark::Counter(static_cast<double>(n), benchmark::Counter::kIsIterationInvariantRate);
}

// --- SP -> 2 consumers ------------------------------------------------------
void BM_Disruptor_SP_2C(benchmark::State& state) {
    const i64 n = state.range(0);
    for (auto _ : state) {
        Disruptor<i64, kCap, ProducerKind::Single, 2> d;
        std::thread prod([&] {
            for (i64 i = 0; i < n; ++i) {
                const i64 s = d.claim();
                d.at(s)    = i;
                d.publish(s);
            }
        });
        // Consumer 1 runs on its own thread; consumer 0 drains on this thread.
        std::thread c1([&] {
            i64 sink = 0;
            i64 next = 0;
            while (next < n) {
                const i64 hi = d.wait_for(next, 1);
                for (i64 s = next; s <= hi; ++s) {
                    sink += d.at(s);
                }
                d.consumed(hi, 1);
                next = hi + 1;
            }
            benchmark::DoNotOptimize(sink);
        });
        i64 sink = 0;
        i64 next = 0;
        while (next < n) {
            const i64 hi = d.wait_for(next, 0);
            for (i64 s = next; s <= hi; ++s) {
                sink += d.at(s);
            }
            d.consumed(hi, 0);
            next = hi + 1;
        }
        benchmark::DoNotOptimize(sink);
        prod.join();
        c1.join();
    }
    state.SetItemsProcessed(state.iterations() * n);
}

// --- MP(2) -> SC ------------------------------------------------------------
void BM_Disruptor_MP2_SC(benchmark::State& state) {
    const i64 n        = state.range(0);
    const i64 per_prod = n / 2;
    const i64 total    = per_prod * 2;
    for (auto _ : state) {
        Disruptor<i64, kCap, ProducerKind::Multi> d;
        std::array<std::thread, 2> prods;
        for (usize p = 0; p < 2; ++p) {
            prods[p] = std::thread([&, p] {
                const i64 base = static_cast<i64>(p) * per_prod;
                for (i64 i = 0; i < per_prod; ++i) {
                    const i64 s = d.claim();
                    d.at(s)    = base + i;
                    d.publish(s);
                }
            });
        }
        i64 sink = 0;
        for (i64 i = 0; i < total; ++i) {
            d.wait_for(i);
            sink += d.at(i);
            d.consumed(i);
        }
        benchmark::DoNotOptimize(sink);
        for (auto& t : prods) {
            t.join();
        }
    }
    state.SetItemsProcessed(state.iterations() * total);
}

// --- Baseline: std::mutex + std::queue SPSC ---------------------------------
void BM_MutexQueue_SPSC(benchmark::State& state) {
    const i64 n = state.range(0);
    for (auto _ : state) {
        std::mutex          m;
        std::queue<i64>     q;
        std::thread prod([&] {
            for (i64 i = 0; i < n; ++i) {
                std::lock_guard<std::mutex> lk(m);
                q.push(i);
            }
        });
        i64 sink  = 0;
        i64 count = 0;
        while (count < n) {
            i64  v;
            bool got = false;
            {
                std::lock_guard<std::mutex> lk(m);
                if (!q.empty()) {
                    v   = q.front();
                    q.pop();
                    got = true;
                }
            }
            if (got) {
                sink += v;
                ++count;
            }
        }
        benchmark::DoNotOptimize(sink);
        prod.join();
    }
    state.SetItemsProcessed(state.iterations() * n);
}

constexpr i64 kN = 1 << 20; // ~1M items per run

} // namespace

BENCHMARK(BM_Disruptor_SPSC)->Arg(kN)->UseRealTime();
BENCHMARK(BM_Disruptor_SP_2C)->Arg(kN)->UseRealTime();
BENCHMARK(BM_Disruptor_MP2_SC)->Arg(kN)->UseRealTime();
BENCHMARK(BM_MutexQueue_SPSC)->Arg(kN)->UseRealTime();
