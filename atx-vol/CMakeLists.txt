# ---- atx-vol: equity-options pricing & volatility-fitting library ----------
#
# Ported from the C17 `ats-vol` library into idiomatic C++20 following the atx
# agent profile (.agents/cpp/agent.md). Depends on atx-core for the error
# vocabulary (Result/Status) and the special-function math (normal CDF/PDF)
# folded in during the port. Layered above atx-core, beside atx-tsdb.
#
# Scope: the full ats-vol port (Black-76 + Greeks, implied vol, eSSVI/SVI/C8/
# CStar calibrators, Andersen-Lake/BAW American pricers, curves, universe,
# portfolio/risk on the PricedSurface stack, vol derivatives, surface archive)
# PLUS a Vola-Dynamics
# American-equity parity layer: de-Americanization (american_iv, deamer), hybrid
# dividends + borrow (dividend), exact S3 curve (s3), fit metrics / reduced-χ²
# (fit_metrics), parity metrics (parity), panel fixtures (panel), and the
# multi-expiry parity harness (surface_parity). See
# README.md and docs/superpowers/specs/2026-07-04-atx-vol-vola-parity-design.md.

add_library(atx-vol ${ATX_LIB_TYPE}
    src/version.cpp
    src/log.cpp
    src/instrumentation_abi.cpp
    src/black76.cpp
    src/greeks.cpp
    src/implied_vol.cpp
    src/surface.cpp
    src/rates_curve.cpp
    src/universe.cpp
    src/derivatives.cpp
    src/deriv_book.cpp
    src/detail/rv_lognormal.cpp
    src/vol_surface.cpp
    src/al_probe.cpp
    src/american.cpp
    src/american_batch.cpp
    src/detail/adjoint_greeks.cpp
    src/boundary_interp.cpp
    src/correction.cpp
    src/arb.cpp
    src/surface_policy.cpp
    src/risk_surface_validation.cpp
    src/convex_recovery.cpp
    src/calib.cpp
    src/prepared_fitting.cpp
    src/c8.cpp
    src/c8_calib.cpp
    src/essvi_calib.cpp
    src/svi_calib.cpp
    src/cstar.cpp
    src/cstar_calib.cpp
    src/dense_slice.cpp
    src/profile.cpp
    src/fit_policy.cpp
    src/data.cpp
    src/detail/archive_util.cpp
    src/surface_archive.cpp
    src/run_archive.cpp
    src/run_diagnostics.cpp
    src/surface_db.cpp
    src/batch.cpp
    src/projection.cpp
    src/contract_projection.cpp
    src/historical_projection.cpp
    src/american_iv.cpp
    src/dividend.cpp
    src/s3.cpp
    src/fit_metrics.cpp
    src/adjusted_greeks.cpp
    src/vol_time.cpp
    src/event_vol.cpp
    src/deamer.cpp
    src/parity.cpp
    src/panel.cpp
    src/surface_parity.cpp
    src/opra_panel.cpp
    src/opra_batch.cpp
    src/opra_hive.cpp
    src/session.cpp
    src/chain.cpp
    src/pricer_fitter.cpp
    src/vol_curve.cpp
    src/spline_curve.cpp
    src/curve_fit.cpp
    src/curve_selector.cpp
    src/priced_surface.cpp
    src/priced_surface_view.cpp
    src/prepared_portfolio.cpp
    src/pricing_executor.cpp
    src/portfolio_pricer.cpp
    src/fit_scheduler.cpp
    src/corpus_board_fit.cpp
    src/scenario_grid.cpp
    src/pnl_attribution.cpp
    src/corpus.cpp
    src/surface_db_populate.cpp
    src/surface_db_build.cpp
    src/surface_db_admin.cpp
    src/dispersion.cpp
    src/dispersion_strangle.cpp
    src/dispersion_backtest.cpp
    src/dispersion_surface_db.cpp
    src/backtest_driver.cpp
    src/dispersion_workflow.cpp
    src/listed_dispersion.cpp
    src/listed_opra.cpp
    src/listed_definitions_cache.cpp
    src/occ_ess.cpp
    src/listed_dispersion_schedule.cpp
    src/listed_dispersion_reconciliation.cpp
    src/listed_dispersion_strategy.cpp
    src/dispersion_run.cpp
    src/listed_dispersion_pipeline.cpp
    src/backtest.cpp
    src/backtest_template.cpp
    src/backtest_db.cpp
    src/backtest_db_build.cpp
    src/research_db.cpp
    src/research_validation.cpp
    src/strategy_pipeline.cpp
    src/quant_pipeline.cpp
    src/snapshot_cache.cpp
    src/strategy.cpp
    src/dispersion_strategy.cpp
    src/tearsheet.cpp
    src/run_report.cpp
    src/analytics_primitives.cpp
    src/analytics_density.cpp
    src/analytics_aggregate.cpp
    src/analytics_io.cpp
    src/earnings_term_fit.cpp
    src/sr_tenor_grid.cpp
    src/earnings_forecast_loader.cpp
    src/earnings_repro.cpp
    # declarative swap-lane DSL
    src/swap_leg.cpp
)
add_library(atx::vol ALIAS atx-vol)

# ---- SIMD/AVX2 vectorized batch kernels (fastpath) ---------------------------
# All src/simd/*.cpp join the library; the *_avx2.cpp translation units emit
# AVX2+FMA intrinsics and get the ISA enabled PER-FILE (NOT library-wide) so the
# rest of atx-vol stays baseline x86-64 and the binary loads on any CPU. Runtime
# dispatch (atx::vol::simd::have_avx2(), CPUID+XGETBV) calls the AVX2 kernels only
# when the host supports them. Per-file -mavx2 also defines __AVX2__/__FMA__,
# satisfying the compile-time guard in detail/vector_math.hpp and keeping clangd
# green. CONFIGURE_DEPENDS re-globs on build so new kernels need no CMake edit.
file(GLOB ATX_VOL_SIMD_SRC CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/simd/*.cpp)
target_sources(atx-vol PRIVATE ${ATX_VOL_SIMD_SRC})

file(GLOB ATX_VOL_SIMD_AVX2 CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/simd/*_avx2.cpp)
set_source_files_properties(${ATX_VOL_SIMD_AVX2}
    PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")

# ---- Generated version header (plan 5.3) ------------------------------------
# `project(atx VERSION ...)` is the single source of truth for the atx-vol
# version. It reaches C++ through exactly one generated file, which the Tier-A
# atx/vol/version.hpp includes; no header or .cpp restates the number.
#
# Generated into its OWN root (generated-include/) rather than into the source
# include/ tree, so `git status` stays clean and two build dirs at different
# versions cannot fight over one file. The install rule that ships it lives with
# the rest of the packaging in cmake/atx-vol-install.cmake; the INSTALL_INTERFACE
# include dir below already covers it, because it installs to the same
# atx/vol/detail/ prefix the source-tree detail headers land in.
set(ATX_VOL_GENERATED_INCLUDE "${CMAKE_CURRENT_BINARY_DIR}/generated-include")
# cmake/atx-vol-install.cmake needs this path to ship the generated header, and it
# is include()d at ROOT scope (after every add_subdirectory, because the export set
# spans modules), so hand the value up one level explicitly -- a plain set() here
# is invisible there.
set(ATX_VOL_GENERATED_INCLUDE "${ATX_VOL_GENERATED_INCLUDE}" PARENT_SCOPE)
# Relative to THIS directory, not CMAKE_SOURCE_DIR: atx-vol/python/CMakeLists.txt
# add_subdirectory()s the monorepo from its own project(), so CMAKE_SOURCE_DIR is
# not always the repo root, but atx-vol/../cmake always is the right place.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/atx-vol-version.hpp.in"
    "${ATX_VOL_GENERATED_INCLUDE}/atx/vol/detail/version_generated.hpp"
    @ONLY)

target_include_directories(atx-vol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${ATX_VOL_GENERATED_INCLUDE}>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-vol PUBLIC cxx_std_20)

# ---- Public-surface tiering (S4-T18 / plan 4.1) ------------------------------
# `include/atx/vol/` above is the SHIPPED public surface: Tier-A (the frozen v1
# API, and exactly what the `atx/vol/vol.hpp` umbrella includes) plus Tier-B
# (public but advanced — reachable by explicit include, deliberately outside the
# umbrella). `include/atx/vol/detail/` is internal machinery with no stability
# promise.
#
# Two header homes sit OUTSIDE that surface, each behind its own target so the
# separation is expressed in the build graph and not only in a directory name:
#
#   atx-vol-tools    — CLI-support headers (surface-db build/admin/populate/exit
#                      codes, the run-report writers, the tearsheet). Consumed by
#                      tools/*_main.cpp and the acceptance examples, not by a
#                      library consumer.
#   atx-vol-research — dispersion RUN ORCHESTRATION (the run/workflow/pipeline
#                      drivers and the run-archive artifacts they emit). The
#                      dispersion VOCABULARY (dispersion.hpp, listed_*.hpp)
#                      stays on the public surface; only the drivers move.
#
# MINIMAL SHAPE, AND IT STAYED THAT WAY. Both are INTERFACE targets that own an
# include root only — the implementing translation units still compile into
# atx-vol, so atx-vol carries the include roots PUBLIC. S4-T18 left a note here
# saying plan 5.6 would split those .cpp into real static libraries and demote
# these two edges to PRIVATE. 5.6 (S5-T27) promoted the tool EXECUTABLES instead
# (the ATX_BUILD_TOOLS block at the bottom of this file) and deliberately did NOT
# do the split. Two measured reasons, so the next reader does not re-plan it from
# the old note:
#
#   1. Flipping THIS edge to PRIVATE would change nothing a consumer can observe.
#      atx-vol links atx-vol-research PUBLIC, and atx-vol-research links
#      atx-vol-tools INTERFACE (below), so the tools include root reaches every
#      `atx::vol` consumer through research regardless. Containing the tools
#      headers means demoting the RESEARCH edge too — and the research headers
#      are shipped, so that is a distribution-surface decision, not a build tidy.
#   2. The .cpp cannot simply move down a layer. The research tier's own public
#      headers include the tools headers (research/dispersion_run.hpp and
#      research/backtest_driver.hpp both name TearSheet), while the tools sources
#      call back into the core library — so "tools as a lower static library" is a
#      two-way edge, not a tier. It needs a real decomposition, not a target move.
#
# Both remain open. Neither is a defect in what ships: the reachability is stated
# here and in cmake/atx-vol-install.cmake, and it is wider-than-ideal rather than
# wrong.
add_library(atx-vol-tools INTERFACE)
add_library(atx::vol::tools ALIAS atx-vol-tools)
target_include_directories(atx-vol-tools
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/tools/include>
        $<INSTALL_INTERFACE:include>
)

add_library(atx-vol-research INTERFACE)
add_library(atx::vol::research ALIAS atx-vol-research)
target_include_directories(atx-vol-research
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/research/include>
        $<INSTALL_INTERFACE:include>
)
# The run drivers report through the tearsheet / run-report writers, so research
# sits ABOVE tools in the tier order. Nothing may point the other way.
target_link_libraries(atx-vol-research INTERFACE atx-vol-tools)

target_link_libraries(atx-vol
    PUBLIC
        atx::core
        atx-vol-tools
        atx-vol-research
    PRIVATE
        atx::tsdb  # read-only mmap (tsdb::Mapping) for the S2 zero-read surface open
        atx_warnings
)

# ---- Algorithm counters (P0.2) ----
# PUBLIC so the library AND every consumer (bench/tests that include
# atx/vol/detail/counters.hpp) agree on the macro's value — the header's inline atomic
# storage is guarded by it, so a split view would be an ODR hazard. OFF by
# default: the definition is absent and the macros compile to nothing.
if(ATX_VOL_COUNTERS)
    target_compile_definitions(atx-vol PUBLIC ATX_VOL_COUNTERS)
endif()
if(ATX_VOL_PROFILE)
    target_compile_definitions(atx-vol PUBLIC ATX_VOL_PROFILE)
endif()

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)

    # Focused TDD gate for the bounded SPX/Wilmott research CLI. The example TU
    # is compiled with its main suppressed so the test exercises the exact
    # forward-estimation and scoring implementation shipped by the executable.
    add_executable(spx-wilmott-repro-tests
        tests/spx_wilmott_repro_test.cpp
        examples/spx_wilmott_repro.cpp)
    target_compile_definitions(spx-wilmott-repro-tests PRIVATE
        ATX_SPX_WILMOTT_REPRO_NO_MAIN)
    target_include_directories(spx-wilmott-repro-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    target_link_libraries(spx-wilmott-repro-tests PRIVATE
        atx::vol atx::core GTest::gtest_main atx_warnings)
    add_test(NAME SpxWilmottReproUnit COMMAND spx-wilmott-repro-tests)
    set_tests_properties(SpxWilmottReproUnit PROPERTIES LABELS atx_vol_fast)
endif()

# ---- Benchmarks (Google Benchmark; opt-in via the shared ATX_BUILD_BENCH) ----
# Mirrors atx-core/bench and atx-engine/bench. Three targets:
#   atx-vol-american-bench   — American pricer route/side/API/grid matrix
#   atx-vol-portfolio-bench  — PricedSurface queries + PortfolioPricer throughput
if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()

# Real-data OPRA parity + throughput benchmark (opt-in via ATX_BUILD_EXAMPLES,
# shared with atx-core's example gate). Loads a real Databento OPRA cbbo-1m NBBO
# slice, de-Americanizes + fits the surface, and reports fit quality + cold-vs-
# cached throughput. Links atx::vol (+ atx::core transitively).
if(ATX_BUILD_EXAMPLES)
    # Reproduce Vola Dynamics' 2019-08-26 SPX first-monthly smile from a paid
    # Databento CBBO snapshot. Research comparison only: no proprietary C13pm
    # compatibility is claimed or persisted.
    add_executable(spx_wilmott_repro examples/spx_wilmott_repro.cpp)
    target_include_directories(spx_wilmott_repro PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/examples)
    target_link_libraries(spx_wilmott_repro PRIVATE atx::vol atx::core atx_warnings)

    add_executable(opra_parity_bench examples/opra_parity_bench.cpp)
    target_link_libraries(opra_parity_bench PRIVATE atx::vol atx::core atx_warnings)

    # SOTA perf + accuracy proof on the synthetic SPY index fixture (no data
    # dependency; known-truth accuracy oracle). See examples/spy_surface_bench.cpp.
    add_executable(spy_surface_bench examples/spy_surface_bench.cpp)
    target_link_libraries(spy_surface_bench PRIVATE atx::vol atx::core atx_warnings)
    # S5-T24: the deterministic SPY known-truth fixture is the Tier-B public
    # header atx/vol/spy_fixture.hpp, so this example no longer needs tests/ as
    # an extra quoted-include root (S4-T18 had put it there).

    add_executable(spy_diag examples/spy_diag.cpp)
    target_link_libraries(spy_diag PRIVATE atx::vol atx::core atx_warnings)

    # bt-spyfit-rca DIAGNOSTIC: reproduce the SPY/XOM populate fit failure with
    # full risk-admission rejection detail. See examples/spy_fit_rca.cpp.
    add_executable(spy_fit_rca examples/spy_fit_rca.cpp)
    target_link_libraries(spy_fit_rca PRIVATE atx::vol atx::core atx_warnings)

    # SOTA %-within-bid-ask acceptance: eSSVI (3 DoF) vs dense C16 (16 DoF) on the
    # real SPY board, scored through the same re-Americanization. See
    # examples/spy_bidask_bench.cpp.
    add_executable(spy_bidask_bench examples/spy_bidask_bench.cpp)
    target_link_libraries(spy_bidask_bench PRIVATE atx::vol atx::core atx_warnings)

    # SOTA American-IV conversion throughput + accuracy proof (known-truth oracle,
    # no data dependency). Reports inversions/sec and round-trip sigma error for
    # the cold baseline vs the warm-started AloPricer path. See
    # examples/american_iv_bench.cpp.
    add_executable(american_iv_bench examples/american_iv_bench.cpp)
    target_link_libraries(american_iv_bench PRIVATE atx::vol atx::core atx_warnings)

    # Unified library layer (OptionChain -> PricerFitter -> parallel value_chain)
    # proof: parallel-inversion scaling {1,2,4,8 threads}, thread-count determinism,
    # and tick-to-quote update->revalue on the real SPY OPRA board. See
    # examples/chain_pricer_bench.cpp.
    add_executable(chain_pricer_bench examples/chain_pricer_bench.cpp)
    target_link_libraries(chain_pricer_bench PRIVATE atx::vol atx::core atx_warnings)

    # Ten real Databento OPRA SPY snapshots spanning intraday/stress regimes:
    # cold fit latency distribution with a per-slice 98% clean in-band gate.
    add_executable(spy_fit_matrix_bench examples/spy_fit_matrix_bench.cpp)
    target_link_libraries(spy_fit_matrix_bench PRIVATE atx::vol atx::core atx_warnings)

    # Dump the SPY Dec-2026 vol smile + bid/ask IV bands over z in [-2,2] (CSV on
    # stdout) for plotting. See examples/spy_dec_curve.cpp.
    add_executable(spy_dec_curve examples/spy_dec_curve.cpp)
    target_link_libraries(spy_dec_curve PRIVATE atx::vol atx::core atx_warnings)

    # Carry diagnostic: locate the put-call-consistent forward per expiry (repo's
    # own Andersen-Lake de-Am) vs the pipeline forward. See examples/spy_carry_diag.cpp.
    add_executable(spy_carry_diag examples/spy_carry_diag.cpp)
    target_link_libraries(spy_carry_diag PRIVATE atx::vol atx::core atx_warnings)

    # Out-of-sample honesty check: leave-every-other-strike-out generalization of
    # the convex-QP dense fit. See examples/spy_oos_check.cpp.
    add_executable(spy_oos_check examples/spy_oos_check.cpp)
    target_link_libraries(spy_oos_check PRIVATE atx::vol atx::core atx_warnings)

    # Breadth: one PricerFitter auto-selects the curve per underlying (index SPY
    # vs single-name XOM) with no per-symbol config. See examples/vol_breadth_bench.cpp.
    add_executable(vol_breadth_bench examples/vol_breadth_bench.cpp)
    target_link_libraries(vol_breadth_bench PRIVATE atx::vol atx::core atx_warnings)

    # Surface-archive (ATXVSA2) deserialization throughput: fit -> serialize -> reload,
    # reporting ns/surface for single-symbol random access + full reconstruct_all.
    # See examples/surface_archive_bench.cpp.
    add_executable(surface_archive_bench examples/surface_archive_bench.cpp)
    target_link_libraries(surface_archive_bench PRIVATE atx::vol atx::core atx_warnings)

    # PricedSurface-native portfolio pricer + Taylor PnL-explain throughput: mixed
    # multi-underlying book, kernel-floor vs price() overhead, thread scaling +
    # determinism, and pnl_explain rate. See examples/portfolio_pricer_bench.cpp.
    add_executable(portfolio_pricer_bench examples/portfolio_pricer_bench.cpp)
    target_link_libraries(portfolio_pricer_bench PRIVATE atx::vol atx::core atx_warnings)

    # H1 (WS-H) microbench: scalar per-contract american_greeks_al vs the 4-wide
    # laned american_put_greeks_batch bundle the FullGreeks risk loop dispatches
    # under ForceAvx2. Prints ns/contract + speedup. Reporting harness only.
    add_executable(american_laned_greeks_bench examples/american_laned_greeks_bench.cpp)
    target_link_libraries(american_laned_greeks_bench PRIVATE atx::vol atx::core atx_warnings)

    # Backtest engine (Phase B3) worked examples on a synthetic corpus: build ->
    # run_backtest -> tearsheet headline metrics -> write_backtest_tsv. Both are
    # OFF-by-default demonstrations; the worked-example GATE lives in the test
    # suite (tests/tearsheet_test.cpp, self-contained). See examples/*.cpp.
    add_executable(atxvol_dispersion_backtest examples/dispersion_backtest.cpp)
    target_link_libraries(atxvol_dispersion_backtest PRIVATE atx::vol atx::core atx_warnings)

    add_executable(atxvol_strategy_examples examples/strategy_examples.cpp)
    target_link_libraries(atxvol_strategy_examples PRIVATE atx::vol atx::core atx_warnings)

    # Deliverable: short the 40-delta 6m SPY strangle, restriked every day
    # (2026-01-02 -> 2026-07-02). Builds a deterministic rolling-vol synthetic SPY
    # corpus, runs the backtest (timed), confirms the quote-slice CSV + ATXVSA
    # surface-archive stores, and prints a tearsheet + TSV table. The correctness
    # GATE is tests/spy_strangle_backtest_test.cpp. See examples/spy_strangle_backtest.cpp.
    add_executable(spy_strangle_backtest examples/spy_strangle_backtest.cpp)
    target_link_libraries(spy_strangle_backtest PRIVATE atx::vol atx::core atx_warnings)

    # Build a REAL SPY surface corpus from a pulled OPRA parquet hive
    # (databento_bulk_opra output): load_opra_daterange -> build_corpus -> ATXVSA
    # archives + manifest, which spy_strangle_backtest --manifest then backtests.
    add_executable(spy_ytd_corpus examples/spy_ytd_corpus.cpp)
    target_link_libraries(spy_ytd_corpus PRIVATE atx::vol atx::core atx_warnings)

    # Validation harness: re-run the SAME short 40d 6m strangle a second way — from
    # the RAW OPRA mid marks on actual listed contracts — and compare its P&L series
    # against the interpolated-from-fits backtest (with a bridge series that splits
    # the gap into strike/tenor granularity vs fit-vs-market fidelity).
    add_executable(spy_strangle_tradeable examples/spy_strangle_tradeable.cpp)
    target_link_libraries(spy_strangle_tradeable PRIVATE atx::vol atx::core atx_warnings)

    # Point-in-time Databento OPRA definition export consumed by the strict
    # listed-dispersion workflow. Network access and cost are explicitly gated.
    add_executable(databento_spy_dispersion_definitions
        examples/databento_spy_dispersion_definitions.cpp)
    target_link_libraries(databento_spy_dispersion_definitions
        PRIVATE atx::vol atx::core databento::databento atx_warnings)

    # Fit a real OPRA hive (MAG7 + SPY) into a SurfaceDb via populate_surface_db,
    # honoring per-symbol manifest configs. Gate test: SurfaceDbPopulate
    # (tests/surface_db_populate_test.cpp). See examples/mag7_surfdb_populate.cpp.
    add_executable(mag7_surfdb_populate examples/mag7_surfdb_populate.cpp)
    target_link_libraries(mag7_surfdb_populate PRIVATE atx::vol atx::core atx_warnings)

    # WS-F F-c: fit + serialize the SPY-dispersion universe (SPY + top-N) across a
    # YTD date range from the real OPRA hive into a SurfaceDb via the fused
    # streaming populate_universe_streaming (cell-aware idempotent resume). Gate
    # test: SurfaceDbPopulate.UniverseStreamingResumeOverRealHive.
    add_executable(universe_surfdb_populate examples/universe_surfdb_populate.cpp)
    target_link_libraries(universe_surfdb_populate PRIVATE atx::vol atx::core atx_warnings)

    # Whole-universe auto-select fit stress harness: one OPRA snapshot minute,
    # N thousand underliers, PricerConfig::curve unset (unified policy +
    # CurveSelector routing per board), per-symbol outcome CSV + summary.
    add_executable(universe_autofit examples/universe_autofit.cpp)
    target_link_libraries(universe_autofit PRIVATE atx::vol atx::core atx_warnings)

    # Stable economic regression oracle over the ten SPY fit fixtures plus the
    # smoke100 OPRA snapshot. The CSV intentionally excludes wall-clock fields;
    # its companion checker requires two identical runs to be byte-for-byte equal.
    add_executable(accuracy_panel examples/accuracy_panel.cpp)
    target_link_libraries(accuracy_panel PRIVATE atx::vol atx::core atx_warnings)
    if(ATX_BUILD_TESTS)
        find_package(Python3 COMPONENTS Interpreter QUIET)
        if(Python3_Interpreter_FOUND)
            add_test(
                NAME AccuracyPanelDeterminism
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/accuracy_panel_determinism_test.py
                        --exe $<TARGET_FILE:accuracy_panel>
                        --spy-root ${CMAKE_SOURCE_DIR}/data/spy_fit_slices
                        --universe-root ${CMAKE_SOURCE_DIR}/data/opra_universe
                        --symbols
                            ${CMAKE_CURRENT_SOURCE_DIR}/tests/accuracy_panel_success_symbols.txt
                        --no-spy
                        --universe-limit 1
                        --mode commit
                        --require-ok
            )
            set_tests_properties(AccuracyPanelDeterminism PROPERTIES LABELS atx_vol_slow)
            add_test(
                NAME AccuracyPanelFailureDeterminism
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_CURRENT_SOURCE_DIR}/tests/accuracy_panel_determinism_test.py
                        --exe $<TARGET_FILE:accuracy_panel>
                        --spy-root ${CMAKE_SOURCE_DIR}/data/spy_fit_slices
                        --universe-root ${CMAKE_SOURCE_DIR}/data/opra_universe
                        --symbols
                            ${CMAKE_CURRENT_SOURCE_DIR}/tests/accuracy_panel_failure_symbols.txt
                        --no-spy
                        --universe-limit 1
                        --mode commit
            )
            set_tests_properties(AccuracyPanelFailureDeterminism PROPERTIES LABELS atx_vol_slow)
        endif()
    endif()

    # SpiderRock SRCubic cubic-spline surface parity benchmark: pin
    # VolCurveKind::SplineVol and fit a whole OPRA universe snapshot, reporting
    # throughput (surfaces/slices/quotes per sec), fit-latency percentiles, the
    # per-stage hot-path CPU split, and SpiderRock-native fit accuracy
    # (fitMaxPrcErr, bid/ask misses, zero-violation rate) with a 45s/90%
    # SpiderRock verdict. See examples/opra_spline_bench.cpp.
    add_executable(opra_spline_bench examples/opra_spline_bench.cpp)
    target_link_libraries(opra_spline_bench PRIVATE atx::vol atx::core atx_warnings)

    # Acceptance example: MAG7-vs-SPY dispersion-strangle backtest driven
    # entirely by a SurfaceDb -- open db -> Clock::from_surface_db ->
    # make_dispersion_strangle_spec -> DeclarativeStrategy -> run_backtest ->
    # tearsheet -> the five pinned run_report output files a later Python
    # renderer consumes. Gate test: Mag7DispersionBacktest
    # (tests/mag7_dispersion_backtest_test.cpp). See
    # examples/mag7_dispersion_backtest.cpp.
    add_executable(mag7_dispersion_backtest examples/mag7_dispersion_backtest.cpp)
    target_link_libraries(mag7_dispersion_backtest PRIVATE atx::vol atx::core atx_warnings)

    # Surface-db dispersion route example: a SurfaceDb root + a date window ->
    # run_surface_db_dispersion_backtest (the one-call entry point in
    # atx/vol/dispersion_surface_db.hpp: open db -> Clock::from_surface_db ->
    # Clock::between -> universe_from_surface_db or read_universe -> run_timed)
    # -> series.csv / strategy_metrics.csv / engine_metrics.csv. A thin argv +
    # artifact shell; every branch of the run itself is gated by
    # SurfaceDbDispersionBacktest (tests/surface_db_dispersion_backtest_test.cpp).
    # See examples/surface_db_dispersion_backtest.cpp.
    add_executable(surface_db_dispersion_backtest examples/surface_db_dispersion_backtest.cpp)
    target_link_libraries(surface_db_dispersion_backtest PRIVATE atx::vol atx::core atx_warnings)

    # WS-D acceptance driver: the vega-flat SPY dispersion PnL-track backtest
    # (long top-N 40Δ strangles vs a short SPY strangle, net-vega-zero at entry,
    # daily clip, daily delta-hedge, held to expiry) -> the pnl_track.tsv the
    # Python renderer tools/spy_dispersion_pnl_report.py turns into the
    # acceptance PNG. Strategy gate: SpyDispersionPnl
    # (tests/spy_dispersion_pnl_test.cpp); render gate: SpyDispersionPnlReport
    # (tests/spy_dispersion_pnl_report_test.py). See examples/spy_dispersion_pnl.cpp.
    add_executable(spy_dispersion_pnl examples/spy_dispersion_pnl.cpp)
    target_link_libraries(spy_dispersion_pnl PRIVATE atx::vol atx::core atx_warnings)

    # The strangle-vs-varswap comparison, DECLARATIVE: one fixed-expiry,
    # daily-restriked 40Δ strangle against one uncapped variance swap struck
    # fair and sized to the strangle's entry vega, both on ONE SurfaceDb clock,
    # delta-hedged daily — expressed as a ~20-line StrategySpec
    # (FixedExpiryRestrike + one MatchGroupVega swap leg) and emitting the
    # track.tsv the Python renderer tools/render_strangle_vs_varswap.py turns
    # into the comparison report. It replaced the bespoke
    # StrangleVsVarswapStrategy + 600-line driver behind a track-parity gate
    # (XOM 2026: per-row nav within 7e-9 dollars, swap columns bit-identical).
    # Strategy gates: StrategyRestrike*/SwapLeg (tests/strategy_test.cpp,
    # tests/swap_leg_test.cpp); render gate:
    # python/tests/test_render_strangle_vs_varswap.py (the atx-vol-python lane).
    # See examples/varswap_compare_example.cpp.
    add_executable(atx-vol-varswap-compare-example examples/varswap_compare_example.cpp)
    target_link_libraries(atx-vol-varswap-compare-example
        PRIVATE atx::vol atx::core atx_warnings)

    # Variance-strip diagnostic: per-session K_var at a constant tenor under the
    # default config and a ladder of pinned log-strike spans, plus the wing IVs
    # the strip reads — the instrument for attributing day-to-day fair-strike
    # movement to surface regions. See examples/varswap_strip_diag.cpp.
    add_executable(atx-vol-varswap-strip-diag examples/varswap_strip_diag.cpp)
    target_link_libraries(atx-vol-varswap-strip-diag
        PRIVATE atx::vol atx::core atx_warnings)

    # Projection-backed strategy-history database builder. The default template
    # is the requested 40-delta, three-calendar-month strangle, held to expiry
    # and delta-hedged at every stored close. Re-runs resume logical histories.
    add_executable(atx-vol-backtest-db-build tools/backtest_db_build_main.cpp)
    target_link_libraries(atx-vol-backtest-db-build PRIVATE atx::vol atx::core atx_warnings)

    # Point-in-time signal miner: BacktestDb diagnostics -> next-observation
    # outcomes -> purged walk-forward validation -> immutable ResearchDb Trial.
    add_executable(atx-vol-quant-research tools/quant_research_main.cpp)
    target_link_libraries(atx-vol-quant-research PRIVATE atx::vol atx::core atx_warnings)

    # Earnings-censored atmCen reproduction (Task 7): thin CLI over the shared
    # run_earnings_repro library pipeline (atx/vol/earnings_repro.hpp) -- OPRA
    # parquet -> fitted VolaSession -> Task 5 earnings TSV -> iEMove + the 12
    # SR-tenor atmCenI_{Nd} values. See examples/earnings_repro.cpp.
    add_executable(earnings-repro examples/earnings_repro.cpp)
    target_link_libraries(earnings-repro PRIVATE atx::vol atx::core atx_warnings)

    # AMZN-around-earnings CStar surface report (WS-2): load the committed real
    # OPRA cbbo-1m fixture, fit every expiry (eSSVI seed -> cstar_calibrate_slice),
    # and emit the per-slice CStar base params (theta,s2,c2,C_left,C_right), fit
    # RMSE, and butterfly no-arb margin to stdout + <out>/slices.csv. Proves the
    # front-expiry W-shape negative-curvature (c2<0) fit. See
    # examples/amzn_earnings_report.cpp.
    add_executable(amzn_earnings_report examples/amzn_earnings_report.cpp)
    target_link_libraries(amzn_earnings_report PRIVATE atx::vol atx::core atx_warnings)

    # Earnings-censored atmCen cohort VALIDATION driver (Task 9): batch the
    # shared harness (atx/vol/earnings_repro_config.hpp) over the checked-in
    # SpiderRock truth CSV -- per name, OPRA board -> run_earnings_repro under an
    # EarningsReproConfig -> per-tenor + cohort atmCenI RMSE + iEMove residual +
    # the nEarnCnt schedule-alignment gate. Task 10 sweeps conventions with it.
    # A name whose parquet is absent is SKIPPED, not fatal. See
    # tools/earnings_validation.cpp.
    add_executable(earnings-validation tools/earnings_validation.cpp)
    target_link_libraries(earnings-validation PRIVATE atx::vol atx::core atx_warnings)
endif()

# ---- Shipped operator CLIs (plan 5.6) ---------------------------------------
# THE TOOLS TIER. These three binaries are part of the v1 distribution: they are
# installed into <prefix>/bin by cmake/atx-vol-install.cmake and are ON by
# default (the ATX_BUILD_TOOLS option in the root CMakeLists states why). All
# three were previously gated behind ATX_BUILD_EXAMPLES, which is OFF by default
# and describes demonstrations — so the shipped tooling was, until now, built by
# nothing except an explicitly opted-in developer build.
#
# WHAT IS AND IS NOT IN THIS TIER. A tool here is something an OPERATOR runs
# against real artifacts: build a surface database, inspect one, drive the listed
# dispersion workflow end to end. The acceptance drivers, benchmarks and worked
# demonstrations above stay on the examples gate even when their source sits in
# tools/ (earnings-validation is the one such case: a research cohort harness
# over a checked-in truth CSV, not an operator binary).
#
# EXECUTABLE NAMES ARE DELIBERATELY UNCHANGED by the promotion, including the
# odd one out (`atxvol_spy_dispersion_backtest`). The name is load-bearing for
# consumers this build does not compile: bench/corpus_occupancy_probe.ps1 invokes
# the exe by path, python/tests/test_dispersion_runarchive_e2e.py locates it by
# filename, and python/tests/test_dispersion_e2e_discovery.py asserts the usage
# banner text line by line. Renaming is a separable change with its own blast
# radius; the promotion does not need it.
if(ATX_BUILD_TOOLS)
    # Surface-db production build CLI (Task 6): the one-call build_surface_db
    # driver wrapped in a hand-rolled arg loop -- create-or-open a SurfaceDb,
    # load an OPRA hive v2 window, auto-generate per-symbol fit configs, and
    # cell-aware-streaming-populate every (symbol, date) surface. Resumable/
    # idempotent (re-run over an unchanged hive fits zero). See
    # tools/surface_db_build_main.cpp and docs/surface-db-build.md.
    add_executable(atx-vol-surface-db-build tools/surface_db_build_main.cpp)
    target_link_libraries(atx-vol-surface-db-build PRIVATE atx::vol atx::core atx_warnings)

    # Surface-db MANAGEMENT/INSPECTION CLI: info / partitions / symbols / config
    # / query / verify over a built database, so verifying one needs no Python
    # binding. Thin shell over the surface_db_admin library
    # (atx/vol/tools/surface_db_admin.hpp); gate test: SurfaceDbAdmin
    # (tests/surface_db_admin_test.cpp). See tools/surface_db_main.cpp and
    # docs/surface-db-build.md.
    add_executable(atx-vol-surface-db tools/surface_db_main.cpp)
    target_link_libraries(atx-vol-surface-db PRIVATE atx::vol atx::core atx_warnings)

    # The listed SPY-dispersion workflow CLI: build-corpus / build-schedule /
    # run-backtest / project-schedule / run-projected-backtest /
    # run-surface-backtest / run-projected-var / verify / runarchive dump. Each
    # subcommand is a process boundary over the run directory; three dispatch
    # straight into library entry points and the rest compose named library seams
    # (the split table is in the source, and dispersion_run.hpp is its authority).
    # This is the binary the RunArchive artifacts and the Python renderer
    # (python/src/atxvol/report/dispersion.py) are written for.
    add_executable(atxvol_spy_dispersion_backtest tools/spy_dispersion_backtest.cpp)
    target_link_libraries(atxvol_spy_dispersion_backtest PRIVATE atx::vol atx::core atx_warnings)
endif()
