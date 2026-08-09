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
#   atx-vol-dispersion-bench <- dispersion_ladder_bench.cpp (overlapping-clip ladder:
#                                how per-step cost scales with a GROWING open book,
#                                the question the fixed-book backtest rows cannot ask)
#
# Shared, header-only helpers live beside them:
#   bench_util.hpp           — common per-registration knobs (warm-up, repetitions,
#                              p95 + CV custom statistics) and the opt-in counter dump
#   support/synth_book.hpp   — the data-free synthetic surfaces + 64-underlying book
#   support/synth_corpus.hpp — the data-free per-date ATXVSA2 archive corpus + Clock
#                              the BACKTEST engine needs (synth_book is in-memory only)
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
    var_bench.cpp
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

# Dispersion ladder (overlapping clips): a fresh 3M dispersion clip entered EVERY
# step and held to its own expiry, so the open book — and therefore the per-step
# pricing and daily delta-hedge cost — grows with the run. Driven through the
# LIBRARY dispersion path (make_dispersion_backtest_strategy), so it exercises the
# authoritative build_dispersion_book sizing rather than a look-alike StrategySpec.
add_executable(atx-vol-dispersion-bench
    bench_main.cpp
    dispersion_ladder_bench.cpp
)
target_link_libraries(atx-vol-dispersion-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# ── Task E3 (backtest-production-lakehouse sprint) — sweep cache bench ─────
# The sprint's headline cache number: `run_sweep` (Task C3, sweep_driver.hpp)
# over a 32-variant grid, COLD (empty lakehouse -- every variant a miss, a
# real backtest runs) vs WARM (same grid re-run against an already-populated
# lakehouse -- every variant a catalog probe hit, no backtest runs at all).
# Needs Catalog/TrackStore/sweep_driver actually compiled into atx-vol, same
# as tests/sweep_driver_test.cpp (tests/CMakeLists.txt gates that file the
# same way) -- so this whole target only exists when ATX_VOL_LAKEHOUSE is ON.
if(ATX_VOL_LAKEHOUSE)
    add_executable(atx-vol-sweep-cache-bench
        bench_main.cpp
        sweep_cache_bench.cpp
    )
    target_link_libraries(atx-vol-sweep-cache-bench
        PRIVATE
            atx::vol
            atx::core
            benchmark::benchmark
            atx_warnings
            Threads::Threads
    )
endif()

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

# K5 — IV inversion shootout: atx-vol scalar + AVX2 ns/op and σ error vs a
# long-double bisection oracle, standing vs the Jäckel LBR ~180 ns/op reference.
add_executable(atx-vol-iv-shootout-bench
    bench_main.cpp
    iv_shootout_bench.cpp
)
target_link_libraries(atx-vol-iv-shootout-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
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
            --required american/price/fast_baw16
            --required american/price/fast_qdplus16
            --required american/price/fast_qdplus8
            --required american/price/fast_baw8
            --required american/iv/accurate
            --required american/corners/resolve
            --required american/boundary_batch/scalar
            --required american/boundary_batch/avx2
            --required american/boundary_batch/scalar_qlfast
            --required american/boundary_batch/avx2_qlfast
    )
    set_tests_properties(atx-vol-american-shootout-name-coverage PROPERTIES LABELS atx_vol)
endif()

# ── Sub-Sprint S (Agent S) — appended targets ─────────────────────────────
# CStar (C16M) calibration + no-arb hot path (Sub-Sprint S: W5.1 table-driven
# arb projection, W5.2 analytic Jacobian + fused w/gradient). normal_eq/{legacy,
# fused} bracket the S2 fusion win; arb/cstar/{project,min_roper_g} baseline the
# S3 table-driven projection.
add_executable(atx-vol-cstar-bench
    bench_main.cpp
    cstar_bench.cpp
)
target_link_libraries(atx-vol-cstar-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# Whole-universe cycle-time harness (Sub-Sprint S / S6). Ingest -> fit -> archive
# per-stage breakdown over a self-contained 3-name smoke universe, Iterations(1)
# corpus-style. Harness only — no baseline recorded this sprint.
add_executable(atx-vol-universe-cycle-bench
    bench_main.cpp
    universe_cycle_bench.cpp
)
target_link_libraries(atx-vol-universe-cycle-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# CStar vs eSSVI evidence panel (Sub-Sprint S / S5). A standalone tool (its own
# main(), no benchmark::benchmark) that fits both engines to identical per-board
# observations and prints the ladder-decision metrics. Sourced from examples/ but
# wired here so it builds under the already-on ATX_BUILD_BENCH without pulling in
# the full ATX_BUILD_EXAMPLES set.
add_executable(atx-vol-cstar-panel
    ${CMAKE_CURRENT_SOURCE_DIR}/../examples/cstar_panel.cpp
)
target_link_libraries(atx-vol-cstar-panel
    PRIVATE
        atx::vol
        atx::core
        atx_warnings
        Threads::Threads
)

# ── WS-0 M1 (infra-measure) — bench-gate unit test ────────────────────────
# compare_baseline.py gates fit-row regressions/crashes. Its unit test pins the
# iteration-real_time fallback + CV-unguarded advisory so the fit/e2e blind spot
# (Iterations(1) rows emit no aggregate → invisible to the gate) stays closed.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME atx-vol-compare-baseline-unit
        COMMAND ${Python3_EXECUTABLE} -m unittest compare_baseline_test
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    )
    set_tests_properties(atx-vol-compare-baseline-unit PROPERTIES LABELS atx_vol)
endif()

# ── WS-0 M2 (infra-measure) — LBR same-host shootout ──────────────────────
# Vendored Peter Jäckel "Let's Be Rational" (bench/thirdparty/lets_be_rational,
# license verified permissive). Isolated warnings-off static lib, linked ONLY
# into the IV shootout so BM_IvShootout_Jaeckel can run head-to-head vs atx-vol
# on this host. target_link_libraries accumulates, so this appends the link to
# the existing atx-vol-iv-shootout-bench target without touching its own block.
add_subdirectory(thirdparty/lets_be_rational)
target_link_libraries(atx-vol-iv-shootout-bench PRIVATE lbr_lets_be_rational)

# ── WS-M M1 (infra-measure) — surface-archive serialize/deserialize bench ─────
# The missing ser/deser measurement (backtest hot-path sprint, task M1): serialize
# µs/surface + partition write MB/s, and deserialize µs-to-ready-to-price across the
# whole-board owned reconstruct (open + reconstruct_all_with_provenance), a
# pre-opened reconstruct, the single-symbol reconstruct, and the two zero-copy
# modes (mmap-open, subset-map). Count sweep {1,4,16,50,100} × {Essvi, ConvexDense}.
# ATXVSA2-only since the v1 format was deleted (release-v1 plan 3.5/3.6).
add_executable(atx-vol-surface-archive-bench
    bench_main.cpp
    surface_archive_bench.cpp
)
target_link_libraries(atx-vol-surface-archive-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# ── WS-K K1 (kernel) — AL preset ladder accuracy x cost frontier ───────────────
# One row per QuantLib-QdFpAmericanEngine-mapped preset rung (accurate / fast /
# ql_fast / ql_accurate / interpolated), scored on a real (or representative)
# SPY-OPRA strike/T/vol grid: us/solve + price abs err vs the richest in-repo
# scheme. Reaches the testkit SPY-fit fixture via `support/spy_fit_fixture.hpp`
# (real-board harvest when the parquet is present; representative fallback in
# source-only CI), so — like atx-vol-fitting-bench — it needs ../tests as an
# extra PRIVATE quoted-include root. Own target so it appends cleanly (one owner
# per TU).
add_executable(atx-vol-al-preset-ladder-bench
    bench_main.cpp
    al_preset_ladder_bench.cpp
)
target_link_libraries(atx-vol-al-preset-ladder-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)
target_include_directories(atx-vol-al-preset-ladder-bench
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests
)

# ── WS-F F-b (fit) — fused streaming fit->serialize throughput ─────────────────
# surfaces/sec through populate_surface_db (U1 streaming per-date fit->serialize->
# release into a SurfaceDb, ATXVSA2 writer). Distinct from corpus_build_bench.cpp
# (measure-owned, whole-corpus build_corpus boards/s); this times the SurfaceDb
# streaming populate the universe-date target rides on. Own target so it appends
# cleanly to the shared bench CMakeLists (one owner per TU).
add_executable(atx-vol-fit-serialize-bench
    bench_main.cpp
    fit_serialize_bench.cpp
)
target_link_libraries(atx-vol-fit-serialize-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# ── WS-P P7 (perf, bench-gaps 1-4) — pricing/greeks observability rows ─────────
# Three deterministic-counter rows that isolate the fitter's boundary-solve-bound
# hot loops so the sprint's fewer-solves wins are reportable + future-gateable:
#   fit/american_iv/cached_newton — cached-inversion Newton kernel (F1/F8 Clenshaw
#                                    traversal win; 200-inversion fixture)
#   deam/carry/resolve/{legacy,warm} — de-Am carry solve (P2 warm-start boundary
#                                    solves/slice, 474->139)
#   correction/cache/build/{put,call} — per-side correction-cache build (96 AL
#                                    boundary solves + DCT, perf gap 4)
#   scenario/grid/exact_solve_ledger — A7 (GR-P3-S): cold shocked solves the
#                                    scenario grid's Exact arm pays per grid, the
#                                    deterministic count A7's warm-start would cut
# Emits the always-on solve-ledger sl_* columns in every build (rel/rel-avx2) and
# the gated cnt_* columns under -DATX_VOL_COUNTERS=ON. Own target (one owner per TU).
add_executable(atx-vol-pg-observability-bench
    bench_main.cpp
    pg_observability_bench.cpp
)
target_link_libraries(atx-vol-pg-observability-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)

# Self-gating name-coverage CTest (mirrors the shootout W0.1 pattern): fail loud if
# any P7 observability row is ever dropped or renamed. Independent of
# compare_baseline.py (a separate sprint owns the baselines).
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME atx-vol-pg-observability-name-coverage
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/check_benchmark_names.py
            --exe $<TARGET_FILE:atx-vol-pg-observability-bench>
            --required fit/american_iv/cached_newton
            --required deam/carry/resolve/legacy
            --required deam/carry/resolve/warm
            --required correction/cache/build/put
            --required correction/cache/build/call
            --required scenario/grid/exact_solve_ledger
    )
    set_tests_properties(atx-vol-pg-observability-name-coverage PROPERTIES LABELS atx_vol)
endif()

# ── THEO-4/THEO-10 (theo-module sprint) — breakeven-vol batch label runner AND
# theo-sheet benchmarks, one TU/target (Task 10 added the theo/* cases to the
# same TU rather than a second target). bev/solve/126d_al_fast — single
# solve_breakeven_vol (Task 3) call on a synthetic 126-day GBM path, al_fast
# default; anchors the per-label solve cost. bev/batch/64jobs —
# solve_breakeven_batch (Task 4) over 64 independent jobs at auto worker
# count, the deterministic parallel fan-out this task adds over the single-
# label solve. theo/sheet_200q/price_theo_{true,false} — compute_theo_sheet
# (Task 10) over a 200-query screening sheet, with/without the cold American
# reprice at a shifted theo_vol (see theo_bench.cpp's own banner). Own target
# (one owner per TU).
add_executable(atx-vol-theo-bench
    bench_main.cpp
    theo_bench.cpp
)
target_link_libraries(atx-vol-theo-bench
    PRIVATE
        atx::vol
        atx::core
        benchmark::benchmark
        atx_warnings
        Threads::Threads
)
