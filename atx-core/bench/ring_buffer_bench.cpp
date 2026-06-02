// ring_buffer_bench.cpp — micro-benchmarks for atx::core::container::RingBuffer
//
// Benchmarks:
//   BM_RingBufferPushPop    — push until full, pop all, repeat (throughput).
//   BM_RingBufferOverwrite  — push_overwrite steady-state (always full, always drops).
//   BM_RingBufferWindowRead — sequential oldest-to-newest indexed reads.

#include <benchmark/benchmark.h>

#include <atx/core/container/ring_buffer.hpp>

using atx::core::container::RingBuffer;

// ---------------------------------------------------------------------------
// BM_RingBufferPushPop
//
// Measures the combined push+pop throughput on a capacity-64 ring buffer.
// Each iteration: fill the buffer then drain it completely.  This exercises
// the masking path, placement-new, and explicit destructor call.
// ---------------------------------------------------------------------------
static void BM_RingBufferPushPop(benchmark::State& state) {
    RingBuffer<int, 64> rb;
    int counter = 0;
    for (auto _ : state) {
        // Fill to capacity.
        while (!rb.full()) {
            (void)rb.push(counter++);
        }
        // Drain completely.
        while (!rb.empty()) {
            benchmark::DoNotOptimize(rb.pop());
        }
    }
    state.SetItemsProcessed(
        state.iterations() * static_cast<int64_t>(RingBuffer<int, 64>::capacity()) * 2LL
    );
}
BENCHMARK(BM_RingBufferPushPop);

// ---------------------------------------------------------------------------
// BM_RingBufferOverwrite
//
// Measures push_overwrite in steady-state (buffer already full on every call).
// Each call destroys the oldest element and constructs a new one.
// ---------------------------------------------------------------------------
static void BM_RingBufferOverwrite(benchmark::State& state) {
    RingBuffer<int, 64> rb;
    // Pre-fill.
    for (int i = 0; i < static_cast<int>(rb.capacity()); ++i) {
        (void)rb.push(i);
    }
    int counter = static_cast<int>(rb.capacity());
    for (auto _ : state) {
        rb.push_overwrite(counter++);
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_RingBufferOverwrite);

// ---------------------------------------------------------------------------
// BM_RingBufferWindowRead
//
// Measures sequential window reads (operator[]) over a full buffer.
// Models the hot path in rolling-window statistics.
// ---------------------------------------------------------------------------
static void BM_RingBufferWindowRead(benchmark::State& state) {
    constexpr atx::usize kCap = 64U;
    RingBuffer<double, kCap> rb;
    for (atx::usize i = 0U; i < kCap; ++i) {
        (void)rb.push(static_cast<double>(i));
    }
    double sum = 0.0;
    for (auto _ : state) {
        for (atx::usize i = 0U; i < kCap; ++i) {
            sum += rb[i];
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(kCap));
}
BENCHMARK(BM_RingBufferWindowRead);
