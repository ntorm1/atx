add_executable(atx-vol-tests
    version_test.cpp
    black76_test.cpp
    greeks_test.cpp
    implied_vol_test.cpp
    surface_test.cpp
    curve_test.cpp
    universe_test.cpp
    ingest_scale_test.cpp
    derivatives_test.cpp
    vol_surface_test.cpp
    robust_test.cpp
    american_test.cpp
    american_batch_test.cpp
    counters_test.cpp
    phase_profile_test.cpp
    correction_test.cpp
    support/oracle_pricer_pde.cpp
    support/oracle_pde_golden.cpp
    support/cached_artifacts.cpp
    arb_test.cpp
    calib_test.cpp
    c8_test.cpp
    c8_calib_test.cpp
    essvi_calib_test.cpp
    essvi_deam_test.cpp
    svi_calib_test.cpp
    calib_robustness_test.cpp
    cstar_test.cpp
    cstar_calib_test.cpp
    dense_slice_test.cpp
    profile_test.cpp
    fit_policy_test.cpp
    data_test.cpp
    surface_archive_test.cpp
    surface_db_test.cpp
    batch_test.cpp
    portfolio_test.cpp
    bulk_test.cpp
    calib_pool_test.cpp
    projection_test.cpp
    contract_projection_test.cpp
    historical_projection_test.cpp
    portfolio_risk_test.cpp
    american_iv_test.cpp
    dividend_test.cpp
    s3_test.cpp
    fit_metrics_test.cpp
    deamer_test.cpp
    parity_test.cpp
    panel_test.cpp
    vola_parity_test.cpp
    surface_parity_test.cpp
    opra_panel_test.cpp
    opra_batch_test.cpp
    session_test.cpp
    vol_umbrella_test.cpp
    spy_real_test.cpp
    curve_noarb_test.cpp
    vol_curve_test.cpp
    curve_selector_test.cpp
    curve_fit_parallel_test.cpp
    parallel_for_test.cpp
    pricer_fitter_test.cpp
    spy_bidask_regression_test.cpp
    spy_fit_corpus_test.cpp
    breadth_regime_test.cpp
    opra_breadth_corpus_test.cpp
    spy_archive_roundtrip_test.cpp
    portfolio_pricer_test.cpp
    pricing_executor_test.cpp
    prepared_portfolio_test.cpp
    priced_surface_test.cpp
    spy_portfolio_pnl_test.cpp
    pnl_greeks_consistency_test.cpp
    lifecycle_integration_test.cpp
    corpus_test.cpp
    multiname_pipeline_test.cpp
    dispersion_test.cpp
    listed_dispersion_test.cpp
    listed_opra_test.cpp
    occ_ess_test.cpp
    listed_dispersion_schedule_test.cpp
    listed_dispersion_reconciliation_test.cpp
    listed_dispersion_strategy_test.cpp
    backtest_test.cpp
    strategy_test.cpp
    backtest_exec_test.cpp
    tearsheet_test.cpp
    backtest_real_test.cpp
    spy_strangle_backtest_test.cpp
)
# backtest_bench_test.cpp was relocated to bench/backtest_throughput_bench.cpp
# (it is a throughput benchmark, not a correctness gate).

# SIMD/AVX2 batch-kernel parity gates: any tests/simd_*_test.cpp auto-joins the
# test executable (subagents add per-kernel parity tests without editing here).
file(GLOB ATX_VOL_SIMD_TESTS CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/simd_*_test.cpp)
target_sources(atx-vol-tests PRIVATE ${ATX_VOL_SIMD_TESTS})

target_link_libraries(atx-vol-tests
    PRIVATE
        atx::vol
        atx::core
        GTest::gtest_main
        atx_warnings
)

# Precompiled header (opt-in: ATX_USE_PCH; ON in the ninja/dev/rel presets, OFF
# in `hygiene`). GoogleTest is #included by ~79 of the test TUs and is by far the
# dominant per-TU parse cost; precompiling it (plus the ubiquitous std headers and
# the stable atx-core vocabulary — see pch.hpp) parses that payload once instead
# of once per TU, which is the single biggest lever on this suite's compile time.
# Mirrors atx-engine/tests. The one monolithic target owns its own PCH.
if(ATX_USE_PCH)
    target_precompile_headers(atx-vol-tests PRIVATE pch.hpp)
endif()

include(GoogleTest)

# Suites measured >10s (serial debug, 2026-07-11). Everything here still runs
# under `ctest -L atx_vol`; `-L atx_vol_fast` is the developer inner loop.
# Re-derive from build/Testing/Temporary/LastTest.log when the shape changes.
#
# NOTE on labels: gtest_discover_tests's default POST_BUILD discovery mode
# cannot carry a multi-value `PROPERTIES LABELS "a;b"` list through to the
# generated set_tests_properties() call — the list is serialized through an
# unquoted `-D TEST_PROPERTIES=...` command-line round-trip (see CMake's
# GoogleTest.cmake) which irreversibly flattens the embedded ";" before the
# final script text is written, regardless of escaping at this call site
# (verified: `"atx_vol\;atx_vol_fast"` produces byte-identical, still-broken
# output to the unescaped form). Each test therefore gets exactly ONE bare
# label here (atx_vol_fast or atx_vol_slow). `ctest -L atx_vol` still selects
# both buckets because ctest's `-L`/`--label-regex` is an unanchored regex
# match, and "atx_vol" is a substring of both label names (verified with a
# `^atx_vol$` anchored probe matching neither) — so `-L atx_vol` remains
# equivalent to the pre-split behavior without needing a separate literal
# "atx_vol" label. `ctest --print-labels` will list atx_vol_fast/atx_vol_slow
# only, not a standalone "atx_vol" entry.
set(ATX_VOL_SLOW_FILTER
    "MultinamePipeline.*:Corpus.*:CorpusGeneratedProperty.*:CorpusBuildSession.*:QualifiedCorpus.*:OpraBreadthCorpus.*:SpyFitCorpus.*:SigmaInterpCorpus.*:CallGreeksAl.*:CallGreeksFd.*:AndersenLake.*:AndersenLakeRegime.*:ImplicitDiff.*:SigmaInterp.*:CurveFitParallel.*:CurveFitParity.*:CurveSurfaceNoArb.*:SpyBidAskRegression.*:SpyPortfolioPnl.*:PnlGreeksConsistency.*:SpyArchiveRoundTrip.*:SpyRealCalendarReporting.*:SpyRealOpra.*:BacktestReal.*:BreadthRegime.*:WarmAcrossTime.*:BoundaryHoist.*:Dispersion.*:CorrectionCache.*:PricerFitterTest.*:PreparedPortfolio.*:AmericanGreeks.*")

gtest_discover_tests(atx-vol-tests
    TEST_FILTER "-${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS atx_vol_fast)
gtest_discover_tests(atx-vol-tests
    TEST_FILTER "${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS atx_vol_slow)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(
        NAME atx-vol-reference-spy-dispersion
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/reference_spy_dispersion_test.py
    )
    set_tests_properties(atx-vol-reference-spy-dispersion PROPERTIES LABELS atx_vol)
    add_test(
        NAME atx-vol-download-occ-ess
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/download_occ_ess_test.py
    )
    set_tests_properties(atx-vol-download-occ-ess PROPERTIES LABELS atx_vol)
    add_test(
        NAME atx-vol-build-spy-top50-universe
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/build_spy_top50_universe_test.py
    )
    set_tests_properties(atx-vol-build-spy-top50-universe PROPERTIES LABELS atx_vol)
endif()
