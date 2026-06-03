#include <benchmark/benchmark.h>

// Entry point for the atx-engine micro-benchmark suite. Per-unit benchmark
// translation units (e.g. event_bus_bench.cpp) register their cases via the
// BENCHMARK() macro and are linked into this single executable.
BENCHMARK_MAIN();
