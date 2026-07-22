# ---- atx-vol: equity-options pricing & volatility-fitting library ----------
#
# Ported from the C17 `ats-vol` library into idiomatic C++20 following the atx
# agent profile (.agents/cpp/agent.md). Depends on atx-core for the error
# vocabulary (Result/Status) and the special-function math (normal CDF/PDF)
# folded in during the port. Layered above atx-core, beside atx-tsdb.
#
# Scope: the full ats-vol port (Black-76 + Greeks, implied vol, eSSVI/SVI/C8/
# CStar calibrators, Andersen-Lake/BAW American pricers, curves, universe,
# portfolio/risk, vol derivatives, surface archive) PLUS a Vola-Dynamics
# American-equity parity layer: de-Americanization (american_iv, deamer), hybrid
# dividends + borrow (dividend), exact S3 curve (s3), fit metrics / reduced-χ²
# (fit_metrics), parity metrics (parity), panel fixtures (panel), and the
# single-/multi-expiry parity harness (vola_parity, surface_parity). See
# README.md and docs/superpowers/specs/2026-07-04-atx-vol-vola-parity-design.md.

add_library(atx-vol ${ATX_LIB_TYPE}
    src/version.cpp
    src/black76.cpp
    src/greeks.cpp
    src/implied_vol.cpp
    src/surface.cpp
    src/curve.cpp
    src/universe.cpp
    src/derivatives.cpp
    src/vol_surface.cpp
    src/american.cpp
    src/american_batch.cpp
    src/detail/adjoint_greeks.cpp
    src/boundary_interp.cpp
    src/correction.cpp
    src/arb.cpp
    src/surface_policy.cpp
    src/risk_surface_validation.cpp
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
    src/portfolio_price.cpp
    src/bulk.cpp
    src/portfolio_greeks.cpp
    # F2 quarantine: src/calib_pool.cpp (calibrate_pool + CadenceQueue) has NO
    # production caller — the corpus/populate hot path deliberately bypasses it
    # (see corpus_board_fit.hpp / corpus.cpp). It is compiled into the test target
    # ONLY (atx-vol/tests/CMakeLists.txt) so the shipped atx::vol library no longer
    # carries this unreachable multi-underlier universe-refit driver on its attack
    # surface. Re-add here (and drop the header's test-only guard) when a live
    # universe-refit driver is wired.
    src/projection.cpp
    src/contract_projection.cpp
    src/historical_projection.cpp
    src/portfolio_risk.cpp
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
    src/vola_parity.cpp
    src/surface_parity.cpp
    src/opra_panel.cpp
    src/opra_batch.cpp
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
    src/dispersion.cpp
    src/dispersion_strangle.cpp
    src/dispersion_backtest.cpp
    src/dispersion_workflow.cpp
    src/listed_dispersion.cpp
    src/listed_opra.cpp
    src/occ_ess.cpp
    src/listed_dispersion_schedule.cpp
    src/listed_dispersion_reconciliation.cpp
    src/listed_dispersion_strategy.cpp
    src/backtest.cpp
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

target_include_directories(atx-vol
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-vol PUBLIC cxx_std_20)

target_link_libraries(atx-vol
    PUBLIC
        atx::core
    PRIVATE
        atx::tsdb  # read-only mmap (tsdb::Mapping) for the S2 zero-read surface open
        atx_warnings
)

# ---- Algorithm counters (P0.2) ----
# PUBLIC so the library AND every consumer (bench/tests that include
# atx/vol/counters.hpp) agree on the macro's value — the header's inline atomic
# storage is guarded by it, so a split view would be an ODR hazard. OFF by
# default: the definition is absent and the macros compile to nothing.
if(ATX_VOL_COUNTERS)
    target_compile_definitions(atx-vol PUBLIC ATX_VOL_COUNTERS)
endif()
if(ATX_VOL_PROFILE)
    target_compile_definitions(atx-vol PUBLIC ATX_VOL_PROFILE)
endif()

# ---- v1 archive read/write — isolated OUT of the product library (WS-G G2) ----
# The legacy ATXVSA (v1) SurfaceArchive reader + write_surface_archive[_file] writer
# live in src/surface_archive_v1.cpp, NOT in atx::vol, so the product library links
# only the v2 (ATXVSA2 zero-copy) format (parent adjudication A). No product src/ TU
# calls v1 — the sole consumers are the throwaway migrator, a few round-trip/format
# tests, and two archive benches; each links this small static lib. It links atx::vol
# for the shared curve / priced-surface / detail symbols + the one shared external
# symbol legacy_surface_provenance() (dependency direction v1 -> vol, NEVER the
# reverse — no link cycle, and vol.lib stays provably v1-free). Built only when a
# dev consumer group is on.
if(ATX_BUILD_TESTS OR ATX_BUILD_BENCH OR ATX_BUILD_EXAMPLES)
    add_library(atx-vol-archive-v1 STATIC src/surface_archive_v1.cpp)
    add_library(atx::vol-archive-v1 ALIAS atx-vol-archive-v1)
    target_link_libraries(atx-vol-archive-v1
        PUBLIC
            atx::vol
        PRIVATE
            atx_warnings
    )
endif()

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
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
    add_executable(opra_parity_bench examples/opra_parity_bench.cpp)
    target_link_libraries(opra_parity_bench PRIVATE atx::vol atx::core atx_warnings)

    # SOTA perf + accuracy proof on the synthetic SPY index fixture (no data
    # dependency; known-truth accuracy oracle). See examples/spy_surface_bench.cpp.
    add_executable(spy_surface_bench examples/spy_surface_bench.cpp)
    target_link_libraries(spy_surface_bench PRIVATE atx::vol atx::core atx_warnings)

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

    # Surface-archive (v3) deserialization throughput: fit -> serialize -> reload,
    # reporting ns/surface for single-symbol random access + full map_all, plus the
    # hardware vs table CRC-32C rate. See examples/surface_archive_bench.cpp.
    add_executable(surface_archive_bench examples/surface_archive_bench.cpp)
    target_link_libraries(surface_archive_bench PRIVATE atx::vol atx::core atx-vol-archive-v1 atx_warnings)

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

    add_executable(atxvol_spy_dispersion_backtest examples/spy_dispersion_backtest.cpp)
    target_link_libraries(atxvol_spy_dispersion_backtest PRIVATE atx::vol atx::core atx_warnings)

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

    # WS-D acceptance driver: the vega-flat SPY dispersion PnL-track backtest
    # (long top-N 40Δ strangles vs a short SPY strangle, net-vega-zero at entry,
    # daily clip, daily delta-hedge, held to expiry) -> the pnl_track.tsv the
    # Python renderer tools/spy_dispersion_pnl_report.py turns into the
    # acceptance PNG. Strategy gate: SpyDispersionPnl
    # (tests/spy_dispersion_pnl_test.cpp); render gate: SpyDispersionPnlReport
    # (tests/spy_dispersion_pnl_report_test.py). See examples/spy_dispersion_pnl.cpp.
    add_executable(spy_dispersion_pnl examples/spy_dispersion_pnl.cpp)
    target_link_libraries(spy_dispersion_pnl PRIVATE atx::vol atx::core atx_warnings)

    # bt-hotpath WS-S (S3): THROWAWAY one-shot v1 .atxvsa -> v2 .atxvsa2 migrator.
    # Wave 2 (S4) runs it over the committed fixtures, then v1 read is deleted.
    add_executable(migrate_atxvsa_v1_to_v2 tools/migrate_atxvsa_v1_to_v2.cpp)
    target_link_libraries(migrate_atxvsa_v1_to_v2 PRIVATE atx::vol atx::core atx-vol-archive-v1 atx_warnings)

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
