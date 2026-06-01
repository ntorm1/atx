#include <benchmark/benchmark.h>

// Entry point for the atx-core micro-benchmark suite. Per-module benchmark
// translation units (e.g. ring_buffer_bench.cpp) register their cases via the
// BENCHMARK() macro and are linked into this single executable.
BENCHMARK_MAIN();
