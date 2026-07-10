#include <benchmark/benchmark.h>

// Entry point for an atx-vol Google Benchmark target. The per-suite translation
// unit (american_pricing_bench.cpp / portfolio_throughput_bench.cpp) registers
// its cases via BENCHMARK()/Apply and is linked into this single executable.
BENCHMARK_MAIN();
