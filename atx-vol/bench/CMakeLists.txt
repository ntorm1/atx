# atx-vol Google Benchmark suite (P0.1). Built only when ATX_BUILD_BENCH=ON
# (the shared, atx-wide option — NOT a new one). Mirrors atx-core/bench and
# atx-engine/bench: each target links atx::vol + benchmark::benchmark + the
# strict-warnings interface, and a BENCHMARK_MAIN() entry TU. Google Benchmark
# is a SYSTEM include (declared in the root CMakeLists FetchContent), so its
# headers never trip atx-vol's /W4 /WX gate.
#
# Two targets (one *_bench.cpp each, kept separate so a matrix change in one does
# not relink the other):
#   atx-vol-american-bench   <- american_pricing_bench.cpp
#   atx-vol-portfolio-bench  <- portfolio_throughput_bench.cpp
#
# Shared, header-only helpers live beside them:
#   bench_util.hpp           — common per-registration knobs (warm-up, repetitions,
#                              p95 + CV custom statistics) and the opt-in counter dump
#   support/synth_book.hpp   — the data-free synthetic surfaces + 64-underlying book
#
# When ATX_VOL_COUNTERS=ON the atx-vol PUBLIC compile definition flows here, so
# bench_util.hpp's counter dump lights up and both targets emit cnt_* columns.

find_package(Threads REQUIRED)

add_executable(atx-vol-american-bench
    bench_main.cpp
    american_pricing_bench.cpp
)
target_link_libraries(atx-vol-american-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

add_executable(atx-vol-portfolio-bench
    bench_main.cpp
    portfolio_throughput_bench.cpp
)
target_link_libraries(atx-vol-portfolio-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# SIMD/AVX2 vectorized-kernel throughput: scalar per-contract loop vs AVX2 SoA
# batch. Proof harness for the vectorization work (fitting/pricing/greeks/pnl).
# Any bench/simd_*bench.cpp auto-joins (subagents add per-kernel benches here).
file(GLOB ATX_VOL_SIMD_BENCH CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/simd_*bench.cpp)
add_executable(atx-vol-simd-bench
    bench_main.cpp
    ${ATX_VOL_SIMD_BENCH}
)
target_link_libraries(atx-vol-simd-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)
