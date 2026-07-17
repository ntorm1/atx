# atx-vol Google Benchmark suite (P0.1). Built only when ATX_BUILD_BENCH=ON
# (the shared, atx-wide option — NOT a new one). Mirrors atx-core/bench and
# atx-engine/bench: each target links atx::vol + benchmark::benchmark + the
# strict-warnings interface, and a BENCHMARK_MAIN() entry TU. Google Benchmark
# is a SYSTEM include (declared in the root CMakeLists FetchContent), so its
# headers never trip atx-vol's /W4 /WX gate.
#
# Targets, kept separate so a matrix change in one does not relink the others:
#   atx-vol-american-bench   <- american_pricing_bench.cpp
#   atx-vol-portfolio-bench  <- portfolio_throughput_bench.cpp
#   atx-vol-projection-bench <- contract_projection_bench.cpp
#   atx-vol-simd-bench       <- simd_*bench.cpp
#   atx-vol-fitting-bench    <- fitting_throughput_bench.cpp + corpus_build_bench.cpp
#   atx-vol-e2e-hotpath-bench <- canonical real OPRA fit/value/backtest pipeline
#                                (C0.3: calibration throughput as a first-class number)
#
# Shared, header-only helpers live beside them:
#   bench_util.hpp           — common per-registration knobs (warm-up, repetitions,
#                              p95 + CV custom statistics) and the opt-in counter dump
#   support/synth_book.hpp   — the data-free synthetic surfaces + 64-underlying book
#
# When ATX_VOL_COUNTERS=ON the atx-vol PUBLIC compile definition flows here, so
# bench_util.hpp's counter dump lights up and all targets emit cnt_* columns.

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

add_executable(atx-vol-projection-bench
    bench_main.cpp
    contract_projection_bench.cpp
)
target_link_libraries(atx-vol-projection-bench
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

# Throughput/perf cases RELOCATED out of the test suite (they measured timing,
# not correctness, so they do not belong in the fast test gate). Each is a proper
# Google Benchmark now:
#   backtest_throughput_bench.cpp     <- tests/backtest_bench_test.cpp (whole file)
#   american_greeks_reuse_bench.cpp   <- tests/american_test.cpp :: DISABLED_FdBoundaryReuse_Speedup
#   strangle_solver_bench.cpp         <- tests/spy_strangle_backtest_test.cpp :: DISABLED_SolverBreakdown
# (corpus_build_bench.cpp moved to atx-vol-fitting-bench below — C0.3: it is a
# fit-throughput case and belongs with the rest of the calibration gate.)
add_executable(atx-vol-reloc-bench
    bench_main.cpp
    backtest_throughput_bench.cpp
    american_greeks_reuse_bench.cpp
    strangle_solver_bench.cpp
)
target_link_libraries(atx-vol-reloc-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# Fitting/calibration throughput (C0.3): calibrations/s as a first-class
# number, directly comparable to the published 16.5k American-calibrations/s
# anchor (fit/american_iv/*), legacy whole-surface and single-slice eSSVI
# cost (fit/surface_cold/*, fit/slice_*), canonical real-OPRA facade admission
# rows (fit/facade/*), plus the relocated corpus build-throughput case.
add_executable(atx-vol-fitting-bench
    bench_main.cpp
    fitting_throughput_bench.cpp
    corpus_build_bench.cpp
)
target_link_libraries(atx-vol-fitting-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)
# fitting_throughput_bench.cpp's real-OPRA case (C0.2) reaches the testkit
# fixture loaders via `#include "support/spy_fit_fixture.hpp"` — the same
# quoted path every tests/*.cpp uses, resolved there via same-directory
# lookup (tests/support/ sits beside the including tests/*.cpp). bench/ has
# no such sibling, so add ../tests as an extra quoted-include root, PRIVATE
# so it does not leak into any other bench target.
target_include_directories(atx-vol-fitting-bench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests
)

add_executable(atx-vol-e2e-hotpath-bench
    bench_main.cpp
    e2e_hotpath_bench.cpp
)
target_link_libraries(atx-vol-e2e-hotpath-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)
target_include_directories(atx-vol-e2e-hotpath-bench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests
)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME atx-vol-e2e-benchmark-name-coverage
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_benchmark_names.py
            --exe $<TARGET_FILE:atx-vol-e2e-hotpath-bench>
            --required fit/e2e/spy_real
            --required fit/e2e/100name
            --required price/backtest/spy_real/cold
            --required price/backtest/spy_real/representative_eager
            --required price/backtest/spy_real/representative_warm
            --required price/backtest/spy_real/representative_screen_cold_confirm
    )
    set_tests_properties(atx-vol-e2e-benchmark-name-coverage PROPERTIES LABELS atx_vol)
    add_test(
        NAME atx-vol-e2e-benchmark-name-checker-unit
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_benchmark_names_test.py
    )
    set_tests_properties(atx-vol-e2e-benchmark-name-checker-unit PROPERTIES LABELS atx_vol)
endif()

# Structured vol-surface analytics throughput: the full compute_surface_analytics
# bundle (term structure + skew/wing + RND + var-swap), the density-off bundle,
# the risk-neutral-density and var-swap primitives, and the two-surface diff.
# Reuses the testkit skewed-surface fixture (tests/support/analytics_fixture.hpp)
# via the same-directory quoted include, so — like atx-vol-fitting-bench — it
# needs ../tests as an extra PRIVATE quoted-include root (bench/ has no sibling
# support/ analytics fixture).
add_executable(atx-vol-analytics-bench
    bench_main.cpp
    analytics_bench.cpp
)
target_link_libraries(atx-vol-analytics-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)
target_include_directories(atx-vol-analytics-bench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests
)

# Correctness-first V2 pipeline release gate. This target owns its main() and
# computes admitted-only p50/p95 distributions directly because rejected fits
# must not enter Google Benchmark's iteration timing population.
add_executable(atx-vol-surface-v2-bench
    surface_v2_bench.cpp
)
target_link_libraries(atx-vol-surface-v2-bench
    PRIVATE
        atx::vol
        atx::core
        atx_warnings
        Threads::Threads
)

# ── Sub-Sprint A (Agent A) — appended targets ─────────────────────────────
# American price/IV shootout: the µs/op-vs-error frontier across the standardized
# grid vs the ALO envelope (SSRN 2547027). Task A3.
add_executable(atx-vol-american-shootout-bench
    bench_main.cpp
    american_shootout_bench.cpp
)
target_link_libraries(atx-vol-american-shootout-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# Self-gating name-coverage CTest (mirrors the W0.1 pattern; does NOT rely on
# compare_baseline.py, which another sprint owns): fail loud if the shootout ever
# drops a tier row.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME atx-vol-american-shootout-name-coverage
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_benchmark_names.py
            --exe $<TARGET_FILE:atx-vol-american-shootout-bench>
            --required american/price/fast
            --required american/price/accurate
            --required american/price/reference
            --required american/iv/accurate
            --required american/corners/resolve
            --required american/boundary_batch/scalar
            --required american/boundary_batch/avx2
    )
    set_tests_properties(atx-vol-american-shootout-name-coverage PROPERTIES LABELS atx_vol)
endif()
