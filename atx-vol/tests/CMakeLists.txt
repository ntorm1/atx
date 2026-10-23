# ── L7-T4 (2026-08-23): the SKIPPED-is-not-PASSED contract ───────────────────
#
# A `GTEST_SKIP()`ping case leaves the process exit code at 0, so a naive ctest
# registration reports PASS for a test that never ran. With 168 GTEST_SKIP sites
# across 50 files in this tree — most of them fixture gates over data/ that a
# clean checkout does not have (root .gitignore excludes /data/) — that would
# make every end-to-end fit-quality and no-arb gate green-by-vacuity.
#
# MEASURED, NOT ASSUMED (2026-08-23, CMake 3.29.0): the per-case lanes are
# already honest. `GoogleTestAddTests.cmake` (CMake's own module, shipped at
# share/cmake-3.29/Modules/) writes `SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]"`
# onto every discovered test unconditionally, and `ctest -R ^AmznEarnings`
# reports all seven fixture-gated cases as `***Skipped` and lists them under
# "The following tests did not run". So the sprint's per-case gate output can be
# trusted. It is restated HERE, explicitly, because that default is a property
# of the toolchain rather than of this project: a CMake that predates it (the
# property itself is 3.16; the GoogleTest module's default is newer) would
# silently take the contract away, and a gate contract that depends on which
# cmake.exe is first on PATH is not a contract.
#
# NOT applied to the aggregate `--gtest_filter` lanes further down. Per CMake's
# own documentation this property marks a test skipped "regardless of the
# process exit code", so on a lane that runs hundreds of cases in ONE ctest
# entry a single skipped case would mask a genuine FAILURE in the same run.
# Those lanes get a `FAIL_REGULAR_EXPRESSION` instead — see each one.
set(ATX_VOL_GTEST_SKIP_REGEX "\\[  SKIPPED \\]")

# This entry point is also consumable without the engine/backtest test graph.
# Keep a focused executable so numerical and include checks stay inexpensive.
add_executable(atx-vol-quote-slice-tests
    quote_slice_test.cpp american_price_slice_test.cpp quote_fit_policy_test.cpp
    quote_surface_test.cpp slice_channel_test.cpp term_prior_test.cpp
    quote_slice_vol_space_test.cpp)
target_link_libraries(atx-vol-quote-slice-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
# The channel/term-prior suites test internal fitting headers (src/fitting/).
target_include_directories(atx-vol-quote-slice-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# Every suite the executable links must be listed: a gtest filter silently drops
# an unlisted one (QuoteSurfaceLadder.* was missing until L2d, so the knot-ladder
# tests never ran under this ctest).
string(JOIN ":" ATX_VOL_QUOTE_SLICE_SUITES QuoteSlice.* AmericanPriceSlice.* QuoteFitPolicy.*
       QuoteSliceChannel.* QuoteSurface.* QuoteSurfaceLadder.* QuoteSurfaceTiming.*
       SliceChannel.* SliceChannelFamily.* TermPrior.* QuoteSliceVolSpace.* DeAmIvCache.*
       AmericanCertification.* QuoteSurfaceRungMemo.* AmericanBoxCertificate.*
       QuoteSliceBoxCertificate.*)
add_test(NAME atx-vol-quote-slice
    COMMAND atx-vol-quote-slice-tests --gtest_filter=${ATX_VOL_QUOTE_SLICE_SUITES})
set_tests_properties(atx-vol-quote-slice PROPERTIES LABELS "atx_vol_fast")

# Implied forward / carry estimator and the quote slice's opt-in carry mode. The
# allocation test replaces global operator new, so it lives ONLY in this focused
# executable (the monolithic atx-vol-tests has its own replacement).
add_executable(atx-vol-implied-forward-tests
    implied_forward_test.cpp quote_slice_carry_test.cpp implied_forward_alloc_test.cpp)
target_link_libraries(atx-vol-implied-forward-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
add_test(NAME atx-vol-implied-forward COMMAND atx-vol-implied-forward-tests)
set_tests_properties(atx-vol-implied-forward PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

# Discrete dividend schedule inference from per-expiry (ddiv, ddiv_pv, rate). The
# allocation test links support/alloc_counter.cpp, which replaces global operator
# new, so it lives ONLY in this focused executable (the monolithic atx-vol-tests
# has its own replacement).
add_executable(atx-vol-dividend-schedule-tests
    dividend_schedule_test.cpp dividend_schedule_alloc_test.cpp support/alloc_counter.cpp)
target_link_libraries(atx-vol-dividend-schedule-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
target_include_directories(atx-vol-dividend-schedule-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME atx-vol-dividend-schedule COMMAND atx-vol-dividend-schedule-tests)
set_tests_properties(atx-vol-dividend-schedule PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

# ATXVFA fitted-surface archive (docs/storage/ARCHIVE_FORMAT.md). Focused and
# PCH-less: framing refusals, record round trips, series, the CLI. Links
# support/alloc_counter.cpp (replaces global operator new), so these files stay
# out of the monolithic atx-vol-tests. The series test drives the private
# storage/writer_lock.hpp to hold the lock.
add_executable(atx-vol-fit-archive-tests
    fit_archive_test.cpp fit_archive_records_test.cpp fit_archive_series_test.cpp
    support/alloc_counter.cpp)
target_link_libraries(atx-vol-fit-archive-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
target_include_directories(atx-vol-fit-archive-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
if(ATX_BUILD_TOOLS)
    # The CLI tool is defined after this directory; the generator expression and
    # the dependency resolve at generate time.
    target_sources(atx-vol-fit-archive-tests PRIVATE fit_archive_cli_test.cpp)
    target_compile_definitions(atx-vol-fit-archive-tests PRIVATE
        "ATX_VOL_SURFACE_ARCHIVE_CLI=\"$<TARGET_FILE:atx-vol-surface-archive>\"")
    add_dependencies(atx-vol-fit-archive-tests atx-vol-surface-archive)
endif()
add_test(NAME atx-vol-fit-archive COMMAND atx-vol-fit-archive-tests)
set_tests_properties(atx-vol-fit-archive PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

# The opt-in strike-smoothing lattice schemes of the dated-cash American route
# (DiscreteDivScheme, api/pricing/american.hpp). Focused executable for the same
# reason as the schedule target above: its allocation test replaces global
# operator new through support/alloc_counter.cpp.
add_executable(atx-vol-discrete-div-smoothing-tests
    discrete_div_smoothing_test.cpp discrete_div_smoothing_alloc_test.cpp
    support/alloc_counter.cpp)
target_link_libraries(atx-vol-discrete-div-smoothing-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
target_include_directories(atx-vol-discrete-div-smoothing-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME atx-vol-discrete-div-smoothing COMMAND atx-vol-discrete-div-smoothing-tests)
set_tests_properties(atx-vol-discrete-div-smoothing PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

# The dated-cash lattice KERNEL objects (api/pricing/american_discrete_div_kernel.hpp,
# lane L3f 2026-10-20): workspace bit-identity / concurrency, the Screen tier's
# measured bound, the seeded inversion's root distance and typed fallbacks. Focused
# executable for the same reason as the two above: its allocation test replaces
# global operator new through support/alloc_counter.cpp.
add_executable(atx-vol-discrete-div-kernel-tests
    discrete_div_kernel_test.cpp discrete_div_kernel_alloc_test.cpp
    support/alloc_counter.cpp)
target_link_libraries(atx-vol-discrete-div-kernel-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
target_include_directories(atx-vol-discrete-div-kernel-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME atx-vol-discrete-div-kernel COMMAND atx-vol-discrete-div-kernel-tests)
set_tests_properties(atx-vol-discrete-div-kernel PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

# The INDEPENDENT cash-dividend reference (src/pricing/american_discrete_div_reference.hpp,
# lane D0 2026-10-22). One focused executable, two ctest cases: the engine mechanics
# (fast label) and the data-driven matrix against the lattice kernel (slow label; its
# suite is also listed in ATX_VOL_SLOW_FILTER below so it stays slow-labelled if it is
# ever compiled into atx-vol-tests).
add_executable(atx-vol-discrete-div-reference-tests
    american_discrete_div_reference_test.cpp american_discrete_div_reference_matrix_test.cpp)
target_link_libraries(atx-vol-discrete-div-reference-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
target_include_directories(atx-vol-discrete-div-reference-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME atx-vol-discrete-div-reference COMMAND atx-vol-discrete-div-reference-tests
    "--gtest_filter=-DiscreteDivReferenceMatrix.*")
set_tests_properties(atx-vol-discrete-div-reference PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")
add_test(NAME atx-vol-discrete-div-reference-matrix COMMAND atx-vol-discrete-div-reference-tests
    "--gtest_filter=DiscreteDivReferenceMatrix.*")
set_tests_properties(atx-vol-discrete-div-reference-matrix PROPERTIES
    LABELS "atx_vol_slow" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}" TIMEOUT 1200)

# The PRODUCTION cash-dividend path (lane D1 2026-10-22): the cubic splice, the
# event-adaptive route, schedule-grouped batching and the event-aware sensitivities
# (api/pricing/american_discrete_div_kernel.hpp, american_discrete_div_batch.hpp). One
# focused executable, two ctest cases: the fast suites, and the slow comparison
# against D0's independent reference (suite also in ATX_VOL_SLOW_FILTER). The
# allocation test replaces global operator new through support/alloc_counter.cpp.
add_executable(atx-vol-discrete-div-production-tests
    american_discrete_div_production_test.cpp american_discrete_div_production_alloc_test.cpp
    american_discrete_div_production_matrix_test.cpp support/alloc_counter.cpp)
target_link_libraries(atx-vol-discrete-div-production-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
target_include_directories(atx-vol-discrete-div-production-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME atx-vol-discrete-div-production COMMAND atx-vol-discrete-div-production-tests
    "--gtest_filter=-DiscreteDivProductionMatrix.*")
set_tests_properties(atx-vol-discrete-div-production PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")
add_test(NAME atx-vol-discrete-div-production-matrix
    COMMAND atx-vol-discrete-div-production-tests "--gtest_filter=DiscreteDivProductionMatrix.*")
set_tests_properties(atx-vol-discrete-div-production-matrix PROPERTIES
    LABELS "atx_vol_slow" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}" TIMEOUT 1200)

add_executable(atx-vol-american-markov-tests
    american_markov_test.cpp american_markov_batch_test.cpp american_markov_resolvent_test.cpp
    american_markov_pwl_test.cpp american_markov_grid_test.cpp pwl_strike_test.cpp)
target_link_libraries(atx-vol-american-markov-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
add_test(NAME atx-vol-american-markov COMMAND atx-vol-american-markov-tests)
set_tests_properties(atx-vol-american-markov PROPERTIES LABELS "atx_vol_fast")

add_executable(atx-vol-american-markov-fit-tests american_markov_fit_test.cpp)
target_link_libraries(atx-vol-american-markov-fit-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
# The variance-time-knot basis tests drive the private
# fitting/american_markov_time_basis.hpp helper directly: no public fitter input
# can produce the unordered rows its refusals guard against.
target_include_directories(atx-vol-american-markov-fit-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME atx-vol-american-markov-fit COMMAND atx-vol-american-markov-fit-tests)
set_tests_properties(atx-vol-american-markov-fit PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

add_executable(atx-vol-american-markov-seed-tests american_markov_seed_test.cpp)
target_link_libraries(atx-vol-american-markov-seed-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
add_test(NAME atx-vol-american-markov-seed COMMAND atx-vol-american-markov-seed-tests)
set_tests_properties(atx-vol-american-markov-seed PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

add_executable(atx-vol-american-markov-market-tests american_markov_market_test.cpp)
target_link_libraries(atx-vol-american-markov-market-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
add_test(NAME atx-vol-american-markov-market COMMAND atx-vol-american-markov-market-tests)
set_tests_properties(atx-vol-american-markov-market PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

add_executable(atx-vol-american-markov-policy-tests american_markov_policy_test.cpp)
target_link_libraries(atx-vol-american-markov-policy-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
add_test(NAME atx-vol-american-markov-policy COMMAND atx-vol-american-markov-policy-tests)
set_tests_properties(atx-vol-american-markov-policy PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

# Pricing hot-path audit 2026-10-16 (tick-path lane): the chain-refresh entry
# points, the per-expiry calibration record and the per-contract spot ladders.
# Focused so the lane never relinks the monolithic exe. These three suites live
# ONLY here (not in atx-vol-tests or a slice), so atx_vol_fast runs each case
# once. The PricingExecutor suite (worker pinning behind the cpu_topology seam)
# is owned by atx-vol-pricing-slice / atx-vol-tests and is not duplicated here.
# support/alloc_counter.cpp replaces global operator new for THIS binary only
# (thread-local armed tally) to prove the tick path allocates nothing.
include(GoogleTest) # gtest_discover_tests (idempotent; also included further down)
add_executable(atx-vol-tick-path-tests
    chain_tick_test.cpp expiry_record_test.cpp spot_ladder_test.cpp
    support/alloc_counter.cpp)
target_link_libraries(atx-vol-tick-path-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
target_include_directories(atx-vol-tick-path-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# One ctest case per gtest case so each touched suite is selectable by an
# anchored -R (e.g. -R "^tick-path:ChainTickCached") and a vanished suite is
# visible.
gtest_discover_tests(atx-vol-tick-path-tests
    TEST_PREFIX "tick-path:"
    PROPERTIES LABELS atx_vol_fast SKIP_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}"
    DISCOVERY_MODE PRE_TEST)

# Chain-level batch price / Greeks / implied vol (batch-pricing lane, 2026-10-17):
# api/pricing/batch_chain.hpp. Focused, and this suite lives ONLY here (not in
# atx-vol-tests or a slice), so atx_vol_fast runs each case once.
# support/process_alloc_counter.cpp replaces global operator new for THIS binary only
# (alloc_counter.hpp's thread-local tally plus a process-wide one that sees the pool
# workers).
add_executable(atx-vol-chain-batch-tests batch_chain_test.cpp support/process_alloc_counter.cpp)
target_link_libraries(atx-vol-chain-batch-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
target_include_directories(atx-vol-chain-batch-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
gtest_discover_tests(atx-vol-chain-batch-tests
    TEST_PREFIX "chain-batch:"
    PROPERTIES LABELS atx_vol_fast SKIP_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}"
    DISCOVERY_MODE PRE_TEST)

# L2h-b sigma-q family (2026-10-21): the AlSigmaQFamily price path's zero-allocation
# count. support/alloc_counter.cpp replaces global operator new for THIS binary only, so
# the suite lives only here (its accuracy / identity suite AlSigmaQFamily.* is in
# atx-vol-tests and atx-vol-pricing-slice).
add_executable(atx-vol-sigma-q-family-alloc-tests
    american_sigma_q_family_alloc_test.cpp support/alloc_counter.cpp)
target_link_libraries(atx-vol-sigma-q-family-alloc-tests
    PRIVATE atx::vol atx::core atx_warnings GTest::gtest_main)
# K1 (r5): the fused price + vega entries are internal (src/pricing/american_sigma_q_family.hpp).
target_include_directories(atx-vol-sigma-q-family-alloc-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/../src)
add_test(NAME atx-vol-sigma-q-family-alloc COMMAND atx-vol-sigma-q-family-alloc-tests)
set_tests_properties(atx-vol-sigma-q-family-alloc PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

# Lane B0 benchmark clock contract (tools/bench_clocks.hpp): the exclusive phase
# arithmetic, the de-Am stage clock and the in-process cold reset. Lives only here (not
# in atx-vol-tests or a slice), so atx_vol_fast runs each case once. The library is a
# tools target, hence the guard.
if(ATX_BUILD_TOOLS)
    add_executable(atx-vol-bench-clocks-tests bench_clocks_test.cpp)
    target_link_libraries(atx-vol-bench-clocks-tests
        PRIVATE atx-vol-bench-clocks atx::vol atx::core atx_warnings GTest::gtest_main)
    target_include_directories(atx-vol-bench-clocks-tests
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
    add_test(NAME atx-vol-bench-clocks COMMAND atx-vol-bench-clocks-tests)
    set_tests_properties(atx-vol-bench-clocks PROPERTIES
        LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")
endif()

add_executable(atx-vol-umbrella-tests vol_umbrella_test.cpp)
target_link_libraries(atx-vol-umbrella-tests PRIVATE
    atx::vol atx::core atx_warnings GTest::gtest_main atx-vol-test-paths)
target_compile_definitions(atx-vol-umbrella-tests PRIVATE
    "ATX_VOL_INCLUDE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/../include\""
    "ATX_VOL_UMBRELLA_HPP=\"${CMAKE_CURRENT_SOURCE_DIR}/../include/atx/vol/api/vol.hpp\""
    "ATX_VOL_INSTALL_CMAKE=\"${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/atx-vol-install.cmake\"")
add_test(NAME atx-vol-umbrella COMMAND atx-vol-umbrella-tests)
set_tests_properties(atx-vol-umbrella PROPERTIES
    LABELS "atx_vol_fast" FAIL_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}")

add_executable(atx-vol-tests
    convex_recovery_test.cpp
    quote_feasibility_test.cpp
    version_test.cpp
    log_sink_test.cpp
    black76_test.cpp
    greeks_test.cpp
    implied_vol_test.cpp
    surface_test.cpp
    rates_curve_test.cpp
    universe_test.cpp
    ingest_scale_test.cpp
    derivatives_test.cpp
    vol_surface_test.cpp
    robust_test.cpp
    american_test.cpp
    american_batch_test.cpp
    american_geometry_test.cpp
    american_family_greeks_test.cpp
    american_sigma_q_family_test.cpp
    american_sigma_q_family_vega_test.cpp
    counters_test.cpp
    phase_profile_test.cpp
    correction_test.cpp
    support/oracle_pricer_pde.cpp
    support/oracle_pde_golden.cpp
    support/cached_artifacts.cpp
    test_paths_test.cpp
    arb_test.cpp
    risk_surface_validation_test.cpp
    surface_v2_qualification_test.cpp
    calib_test.cpp
    prepared_fitting_test.cpp
    c8_test.cpp
    c8_calib_test.cpp
    essvi_anchored_test.cpp
    essvi_calib_test.cpp
    essvi_deam_test.cpp
    svi_calib_test.cpp
    calib_robustness_test.cpp
    cstar_test.cpp
    cstar_calib_test.cpp
    dense_slice_test.cpp
    quote_slice_test.cpp
    quote_slice_vol_space_test.cpp
    quote_slice_carry_test.cpp
    implied_forward_test.cpp
    quote_surface_test.cpp
    slice_channel_test.cpp
    term_prior_test.cpp
    dividend_schedule_test.cpp
    quote_fit_policy_test.cpp
    american_price_slice_test.cpp
    american_markov_test.cpp
    american_markov_grid_test.cpp
    american_markov_batch_test.cpp
    american_markov_pwl_test.cpp
    american_markov_fit_test.cpp
    american_markov_resolvent_test.cpp
    american_markov_seed_test.cpp
    american_markov_market_test.cpp
    american_markov_policy_test.cpp
    pwl_strike_test.cpp
    profile_test.cpp
    fit_policy_test.cpp
    curve_selector_test.cpp
    data_test.cpp
    run_archive_test.cpp
    run_diagnostics_test.cpp
    surface_db_test.cpp
    batch_test.cpp
    projection_test.cpp
    contract_projection_test.cpp
    historical_projection_test.cpp
    var_test.cpp
    var_report_test.cpp
    var_validation_test.cpp
    american_iv_test.cpp
    dividend_test.cpp
    s3_test.cpp
    fit_metrics_test.cpp
    adjusted_greeks_test.cpp
    vol_time_test.cpp
    event_vol_test.cpp
    deamer_test.cpp
    parity_test.cpp
    panel_test.cpp
    surface_parity_test.cpp
    opra_panel_test.cpp
    opra_hive_test.cpp
    opra_batch_test.cpp
    session_test.cpp
    session_serve_test.cpp
    vol_umbrella_test.cpp
    spy_real_test.cpp
    curve_noarb_test.cpp
    vol_curve_test.cpp
    curve_fit_parallel_test.cpp
    curve_fit_carry_fallback_test.cpp
    curve_fit_coverage_test.cpp      # Task 1: k-coverage slice admission (PrepUncovered)
    parallel_for_test.cpp
    pricer_fitter_test.cpp
    spline_curve_test.cpp
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
    scenario_grid_test.cpp
    pnl_attribution_test.cpp
    lifecycle_integration_test.cpp
    corpus_test.cpp
    multiname_pipeline_test.cpp
    dispersion_test.cpp
    listed_dispersion_test.cpp
    listed_opra_test.cpp
    listed_definitions_cache_test.cpp
    occ_ess_test.cpp
    listed_dispersion_schedule_test.cpp
    listed_dispersion_reconciliation_test.cpp
    listed_dispersion_strategy_test.cpp
    listed_dispersion_pipeline_test.cpp
    dispersion_workflow_test.cpp
    backtest_test.cpp
    backtest_incremental_test.cpp
    backtest_template_test.cpp
    backtest_db_test.cpp
    backtest_db_build_test.cpp
    # backtest-production-lakehouse sprint, Task D3 -- appended target: the
    # cross-process manifest writer lock (detail/writer_lock.hpp) and its
    # BacktestDb/SurfaceDb persist_locked() retrofit. Registered
    # UNCONDITIONALLY (unlike catalog_test.cpp below): BacktestDb/SurfaceDb
    # are core atx-vol, compiled in every configuration, so this must be
    # exercised in the ATX_VOL_LAKEHOUSE=OFF build too.
    writer_lock_test.cpp
    research_db_test.cpp
    research_validation_test.cpp
    strategy_pipeline_test.cpp
    quant_pipeline_test.cpp
    strategy_test.cpp
    dispersion_strangle_test.cpp
    backtest_exec_test.cpp
    tearsheet_test.cpp
    benchmark_stats_test.cpp         # X5 IR / alpha / beta / tracking error
    run_report_test.cpp
    backtest_driver_test.cpp
    backtest_real_test.cpp
    spy_strangle_backtest_test.cpp
    surface_db_backtest_test.cpp
    structure_panel_test.cpp
    vega_panel_test.cpp
    surface_db_dispersion_backtest_test.cpp
    surface_db_populate_test.cpp
    surface_db_build_test.cpp
    surface_db_admin_test.cpp
    # atx-vol-chain-export: the per-contract fair-value/greek parquet emitter's
    # schema + row-assembly seam (tools/chain_export.hpp). The driver TU
    # (tools/chain_export_main.cpp) is argv/IO only and is NOT compiled here;
    # every decision that can silently corrupt a published row lives in the
    # header this suite includes (../tools is already a quoted-include root
    # below, added for surface_db_build_cli.hpp).
    chain_export_test.cpp
    mag7_dispersion_backtest_test.cpp
    spy_dispersion_pnl_test.cpp
    dispersion_run_test.cpp
    # Task E1 (backtest-lakehouse sprint) — appended target: the friction/
    # financing/policy CLI flag seam (examples/dispersion_realism_flags.hpp)
    # both mag7_dispersion_backtest and spy_dispersion_pnl include.
    dispersion_realism_flags_test.cpp
    # Task E1 fix round — appended target: the FrictionRegime stringifier seam
    # (tools/dispersion_backtest_regime.hpp) spy_dispersion_backtest.cpp uses.
    dispersion_backtest_regime_test.cpp
    analytics_primitives_test.cpp
    analytics_density_test.cpp
    analytics_aggregate_test.cpp
    analytics_io_test.cpp
    iv_convergence_test.cpp
    # Sub-Sprint A (Agent A) — appended targets:
    american_bindkey_test.cpp        # A4 (R-30) retained-geometry bind-key guardrail
    american_negrate_domain_test.cpp # A5 negative-rate / double-boundary domain map
    # Perf Phase 2b (surface-db sprint) — appended target:
    american_bulk_rung_test.cpp      # al_bulk_opts (ql_fast) rung + FitPreset::Bulk tier
    # Wave-B iv-kernel (Agent K) — appended targets:
    scalar_erfc_test.cpp             # K3-prep: scalar Cody erfc ULP + round-trip gate
    iv_seed_test.cpp                 # K2: Choi-2023 tighter seed accuracy + Halley-step gate
    # bt-hotpath WS-S (format agent) — appended targets:
    surface_archive_v2_test.cpp      # S1-S3: ATXVSA2 zero-copy view parity + subset-map isolation
    zc_view_parity_diag_test.cpp     # WS-ZC1: owned-reconstruct vs borrowed-view parity on a real archive
    # Pipeline-SOTA WS-C (storage hardening) — appended target:
    surface_archive_v2_adversarial_test.cpp # C1/C4: view semantic+bounds validation, corruption corpus, x-check
    surface_archive_durability_test.cpp      # C3: fsync-before-rename + reader-held rename retry / preserve-temp
    # Wave-P AAD greeks (Agent greeks) — appended target:
    adjoint_greeks_test.cpp          # P2: adjoint/IFT American greeks parity vs central-diff FD
    # solve-wall SOTA WS-V (verify agent) — appended target:
    solve_ledger_test.cpp            # V1: always-on solve-ledger cost-model gate (11/6 s/u)
    # Earnings-censored atmCen reproduction (Task 1) — appended target:
    earnings_term_fit_test.cpp       # T1: censored_atm_vol per-expiry primitive
    # Earnings-censored atmCen reproduction (Task 5) — appended target:
    earnings_forecast_loader_test.cpp # T5: forecast TSV -> sorted epoch-ns events
    # Earnings-censored atmCen reproduction (Task 7) — appended target:
    earnings_repro_smoke_test.cpp    # T7: OPRA -> fit -> atmCenI/iEMove end-to-end (real NVDA board)
    # Earnings-censored atmCen reproduction (Task 9) — appended target:
    earnings_validation_test.cpp     # T9: cohort truth CSV -> validate_cohort_name RMSE + nEarnCnt gate
    # AMZN-around-earnings CStar reproduction (WS-4) — appended target:
    amzn_earnings_test.cpp           # real OPRA board -> two-pass C8 fit: front c2_eff<-1, arb-free
    # pricing-greeks SOTA (Agent G4) — appended target:
    american_exercise_boundary_test.cpp # G4: public exercise_boundary + assignment_risk screen
    # SPY-dispersion hot-path WS-C (correctness cluster) — appended target:
    dispersion_workflow_test.cpp     # C1 point-in-time universe, C3 removals, M2 dup keys, M4 index symbol
    # SPY-dispersion hot-path WS-X (config + execution realism) — appended target:
    dispersion_run_config_test.cpp   # X1 strict typed run config, X2 frictions/financing, X6 cost model
    # Pipeline-SOTA WS-F (backtest economic realism) — appended target:
    backtest_leak_test.cpp           # F1 accounting-leak family, F2 quote-side fills, F5 subset-deser
    # Pipeline-SOTA FIX-2 (F2-A) — appended targets:
    # (support/process_scratch.cpp moved to //tests/support in FIX-3/F3-B and is now
    #  linked as the repo-wide atx-test-scratch OBJECT library, see below.)
    process_scratch_test.cpp         # F2-A: two live processes must not share a scratch root
    laned_greek_ok_stamp_test.cpp    # F2-B: laned Ok-stamp guards the REQUESTED greek set
    # Pipeline-SOTA FIX-3 (F3-A) — appended target:
    evaluate_resolved_ok_stamp_test.cpp # F3-A: the Ok-stamp agrees on the laned and scalar routes
    # deriv production sprint
    deriv_distribution_test.cpp
    deriv_greeks_test.cpp
    deriv_book_test.cpp
    backtest_swap_test.cpp           # Task 10: the engine's variance/vol swap lane
    # declarative swap-lane DSL
    swap_leg_test.cpp                # swap-leg toolkit (contract bridge + signal probe)
    # stress-day fit-fidelity sprint (Task 2) — appended target:
    surface_db_populate_policy_test.cpp # populate publish gate: min_worst_frac_within_bidask=0.35
    # R1 (top-of-book fit-refusal diagnosis): the MARK surface must be reachable
    # when the risk-geometry oracle refuses -- the outputs contract
    # (symbol_config_from_preset) and the fit_board rescue seam. Includes the
    # src-private marketdata/corpus_board_fit.hpp (../src is already a PRIVATE
    # quoted-include root below).
    mark_rescue_test.cpp
    # THEO-1 — appended target: realized-vol estimator suite
    realized_vol_test.cpp
    # THEO-2 — appended target: breakeven replay suite
    breakeven_test.cpp
    # THEO-6 — appended target: label-factory driver gate (pulls
    # examples/bev_label_factory.cpp in via #include, main() suppressed --
    # see that test file's banner comment)
    bev_label_factory_gate_test.cpp
    # THEO-7 — appended target: theo engine core
    theo_test.cpp
    # backtest-production-lakehouse sprint, Task B2 — appended target:
    margin_test.cpp                  # Reg-T + scenario-grid margin models, engine wiring
    # backtest-production-lakehouse sprint, Task D1 — appended target:
    track_key_test.cpp               # content-addressed TrackKey + economics-rev tripwire
    # vol-derivs production sprint Task 0: shared fixtures + MC oracle
    deriv_fixtures_test.cpp
    # vol-derivs production sprint Task F-9: CBOE discrete-strike variance sum
    cboe_strip_test.cpp
    # vol-derivs production sprint Task F-8: the shared smile-shift overlay
    surface_overlay_test.cpp
    deriv_pnl_test.cpp               # Task F-8: two-date swap P&L attribution
    # Oracle RSI bootstrap charter stage 2 — appended target: the
    # atx-vol-oracle-bench gate (band edges, tolerances, scorecard cell-key
    # schema + accounting, cohort JSON, partition-pruned reader, synthetic-
    # store end-to-end). The bench DRIVER TU (tools/oracle_bench_main.cpp)
    # joins via textual #include with main() suppressed (the
    # bev_label_factory_gate_test.cpp pattern above); the tool's three
    # LIBRARY TUs compile in via the target_sources below. OracleBench*
    # matches nothing in ATX_VOL_SLOW_FILTER, so the suite lands in the
    # atx_vol_fast label.
    oracle_bench_test.cpp
    # vrp_model_test.cpp (vrp-ml lane vrp-model) is NOT listed here: it needs
    # the atx-engine learn layer, so it builds into its own
    # atx-vol-vrp-model-tests exe further down, which exists only when the
    # engine is configured. Keeping it out keeps atx-engine out of this exe's
    # link closure (build-infra sprint 2026-10-16).
)

# Oracle RSI charter stage 2: the non-driver TUs of atx-vol-oracle-bench
# (headers + implementations under ../tools) compile into the test exe so
# OracleBench* exercises the shipped implementation, not a copy. tools/ is
# already a PRIVATE quoted-include root of this target (see
# target_include_directories below), which is how oracle_bench_test.cpp
# resolves both the headers and its textual #include of the driver TU.
# oracle_host_probe.cpp is here for the same reason it is a separate TU at all:
# --quiet-host needs <windows.h>, which must not reach the textually-included
# driver beside the Arrow headers.
target_sources(atx-vol-tests PRIVATE
    ../tools/oracle_convention_sweep.cpp
    ../tools/oracle_conventions.cpp
    ../tools/oracle_dividends.cpp
    ../tools/oracle_host_probe.cpp
    ../tools/oracle_scorecard.cpp
    ../tools/oracle_cohort_reader.cpp)

# backtest-production-lakehouse sprint, Task D2 — appended conditionally: the
# Parquet track store round-trip test includes Arrow/Parquet headers directly
# (to verify row groups / codec / sorting-columns from real file metadata) and
# needs TrackStore::write_staging/compact() actually compiled into atx-vol, so
# it only joins the suite when ATX_VOL_LAKEHOUSE is ON (mirrors how
# src/track_store.cpp itself is gated, atx-vol/CMakeLists.txt).
if(ATX_VOL_LAKEHOUSE)
    target_sources(atx-vol-tests PRIVATE track_store_test.cpp)
    # Task D3 -- appended conditionally alongside track_store_test.cpp: the
    # SQLite catalog (Catalog::open/probe/register_staging/mark_compacted/
    # record_trial/trial_stats) does not exist in the OFF build (src/
    # catalog.cpp joins atx-vol only under this same guard, atx-vol/
    # CMakeLists.txt). The cross-process WriterLock primitive it shares with
    # BacktestDb/SurfaceDb is tested separately and UNCONDITIONALLY in
    # writer_lock_test.cpp above -- that code path is core, not lakehouse-only.
    target_sources(atx-vol-tests PRIVATE catalog_test.cpp)
    # Task C3 -- appended conditionally alongside track_store_test.cpp/
    # catalog_test.cpp: the cache-first sweep driver (run_sweep) needs
    # Catalog::probe/register_staging/record_trial and TrackStore::write_staging
    # actually compiled into atx-vol (src/sweep_driver.cpp joins only under
    # this same guard, atx-vol/CMakeLists.txt).
    target_sources(atx-vol-tests PRIVATE sweep_driver_test.cpp)
    # D5 fix-round: track_compact's crash-recovery reconcile pass
    # (research/track_compact_reconcile.hpp) -- needs Catalog::mark_compacted/
    # list_by_status and TrackStore::compact() actually compiled into
    # atx-vol, same gate as the three test files immediately above.
    target_sources(atx-vol-tests PRIVATE track_compact_reconcile_test.cpp)
    # Task D6: last_access_ts-driven GC (research/track_gc.hpp) -- needs BOTH
    # Arrow/Parquet reading+writing AND Catalog, same gate as
    # track_compact_reconcile_test.cpp immediately above.
    target_sources(atx-vol-tests PRIVATE track_gc_test.cpp)
endif()
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
        atx-test-scratch    # FIX-3/F3-B: per-process temp root (was a listed source in F2-A)
)

# vrp-ml lane vrp-model: vrp_model_test.cpp covers the schema-v2
# IFairVolModel seam (pricing/theo.hpp: fair_vol_feature_count, linear-v2
# coefficient TSV, flat-array GBT scorer) and the walk-forward VRP trainer
# logic header (tools/vrp_train.hpp), which calls into atx::engine::learn
# (fit_gbt / fit_linear / elastic_net -- declared in headers, DEFINED in
# atx-engine/src/learn/*.cpp). That engine edge used to sit on atx-vol-tests
# itself, which made every vol lane that built the monolithic exe compile all
# of atx-engine (146 TUs, ~1000 CPU-s per tree under Release+ThinLTO). The
# suite now owns a dedicated exe that exists only when the engine is
# configured, i.e. NOT under ATX_VOL_ONLY (every vol-* preset); configure a vol
# preset with -DATX_VOL_ONLY=OFF to build it. Plain target name `atx-engine`
# (not the atx::engine ALIAS): atx-vol is add_subdirectory'd BEFORE atx-engine
# at the root and a plain name resolves at generate time. The atx-vol LIBRARY
# itself never includes an atx-engine header.
if(NOT ATX_VOL_ONLY)
    add_executable(atx-vol-vrp-model-tests vrp_model_test.cpp)
    target_link_libraries(atx-vol-vrp-model-tests PRIVATE
        atx::vol atx::core atx-engine GTest::gtest_main atx_warnings)
    target_include_directories(atx-vol-vrp-model-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools ${CMAKE_CURRENT_SOURCE_DIR}/../src)
endif()

# Wave E T7 fix round 1, I5: listed_definitions_cache_test.cpp's peak-RSS
# harness calls GetProcessMemoryInfo/EmptyWorkingSet (psapi.h). Linked
# explicitly rather than relying on the PSAPI_VERSION=2 kernel32-forwarding
# macro trick, so this does not depend on that SDK default.
if(WIN32)
    target_link_libraries(atx-vol-tests PRIVATE psapi)
endif()

# AMZN-earnings correctness gate (amzn_earnings_test.cpp) shares the report's fit
# via the header-only examples/amzn_earnings_fit.hpp — add examples/ as a PRIVATE
# quoted-include root (mirrors how bench adds ../tests) — and bake the committed
# fixture's absolute path so the test finds it regardless of the ctest CWD.
# Task 4 (sp100-surface-db sprint): surface_db_build_test.cpp's
# SurfaceDbBuildCli suite includes tools/surface_db_build_cli.hpp -- the
# --snapshot-suffix validator seam pulled out of surface_db_build_main.cpp's
# arg loop -- so add tools/ as a PRIVATE quoted-include root too (same pattern
# as the examples/ root two lines above).
target_include_directories(atx-vol-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../examples
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/../tools
                                                  ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(atx-vol-tests PRIVATE
    "ATX_AMZN_FIXTURE=\"${CMAKE_CURRENT_SOURCE_DIR}/data/amzn_earnings_2018/amzn_opra_cbbo1m_2018-04-26T1945Z.parquet\"")
# Same baked-absolute-path trick for the SHIPPED example config that operators
# copy (examples/sp100_dispersion_config.tsv). surface_db_dispersion_backtest_test
# parses the REAL file so an edit to it cannot rot unnoticed; the file is not a
# build input, so nothing else would notice a renamed key or a bad token.
target_compile_definitions(atx-vol-tests PRIVATE
    "ATX_SP100_DISPERSION_CONFIG=\"${CMAKE_CURRENT_SOURCE_DIR}/../examples/sp100_dispersion_config.tsv\"")

# S4-T18 (plan 3.8 + 4.1): vol_umbrella_test.cpp is the public-surface tiering
# CONTRACT test — besides including the umbrella it READS it back off disk and
# checks that its include list is exactly the Tier-A manifest. Bake the two
# paths in (same mechanism as ATX_AMZN_FIXTURE above) so the contract holds
# regardless of the ctest working directory.
target_compile_definitions(atx-vol-tests PRIVATE
    "ATX_VOL_INCLUDE_ROOT=\"${CMAKE_CURRENT_SOURCE_DIR}/../include\""
    "ATX_VOL_UMBRELLA_HPP=\"${CMAKE_CURRENT_SOURCE_DIR}/../include/atx/vol/api/vol.hpp\"")
# L7-T1: the third anchor of the same contract. The two above pin what the public
# include tree CONTAINS; this one pins what the package SHIPS, which is a separate
# file and was the half that had rotted — include/atx/vol/alpha/ sat on the
# target's public BUILD_INTERFACE with no install() rule at all, so seven headers
# compiled in-tree and were a hard `file not found` for every downstream consumer.
# Baked in the same way and for the same reason: the contract must hold under any
# ctest working directory.
target_compile_definitions(atx-vol-tests PRIVATE
    "ATX_VOL_INSTALL_CMAKE=\"${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/atx-vol-install.cmake\"")

# FRI-072: the single anchor every fixture/resource path in this test tree is
# resolved from (tests/support/test_paths.hpp). Same intent as the three baked
# paths above -- the contract must hold regardless of the working directory the
# binary is launched from -- but defined ONCE on an INTERFACE target in
# ../CMakeLists.txt, because bench/ and the example targets include the same
# testkit headers and must agree with this tree about where a fixture lives.
#
# The artifact cache inside it is anchored on CMAKE_BINARY_DIR rather than the
# repo root, and that difference is load-bearing: this repo is configured into
# more than one build tree (build/ and build-rel-avx2/ both exist) and the cache
# key carries a wire-format version and a fit-shape revision but NO
# build-configuration component, so a repo-root-relative cache would let a
# rel-avx2 run serve an archive fitted by the dev build.
target_link_libraries(atx-vol-tests PRIVATE atx-vol-test-paths)

# Plan 5.3: version_test.cpp compares the SHIPPED version against the build
# system's declaration, so it needs `project(atx VERSION ...)` verbatim. Baked in
# from ${PROJECT_VERSION} rather than restated, so this line can never become the
# fourth place the version is written down.
target_compile_definitions(atx-vol-tests PRIVATE
    "ATX_VOL_CMAKE_PROJECT_VERSION=\"${PROJECT_VERSION}\"")

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

# Stage 3's worktree-local convention gate deliberately owns a small target:
# it compiles only the isolated convention/sweep implementation and anchored
# tests, never the full atx-vol test executable.
add_executable(atx-vol-oracle-convention-tests
    oracle_conventions_test.cpp
    ../tools/oracle_convention_sweep.cpp
    ../tools/oracle_conventions.cpp
    ../tools/oracle_dividends.cpp)
target_link_libraries(atx-vol-oracle-convention-tests
    PRIVATE atx::vol atx::core GTest::gtest_main atx_warnings)
target_include_directories(atx-vol-oracle-convention-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# Discovery, not one filtered add_test(): a `--gtest_filter=OracleConvention.*`
# case exits 0 and reports 1/1 passed even when the filter matches ZERO tests,
# which the charter treats as a contract failure. One ctest case per gtest case
# turns a vanished suite into "No tests were found", and the gate pins the exact
# fully-qualified ID set the way it pins the OracleBench registry.
gtest_discover_tests(atx-vol-oracle-convention-tests
    PROPERTIES SKIP_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}"
    DISCOVERY_MODE PRE_TEST)

# Discrete-cash-dividend lane: the Vellekoop-Nieuwenhuis spliced CRR lattice
# (atx::vol, src/pricing/american_discrete_div.cpp) plus the `ddiv` schedule
# reconstructor (../tools/oracle_dividends.cpp, compiled straight in the way the
# convention target above compiles its own tools TUs).
#
# A NEW target rather than two more sources on atx-vol-tests, deliberately: that
# executable carries a PASS_REGULAR_EXPRESSION case-count pin (mode_b_targeted_tests,
# below) and a README-checked public-header registry, and neither should move for
# work that owns its own small target perfectly well. Nothing here is labelled, so
# `-L atx_vol_fast` / `-L atx_vol_slow` select exactly what they selected before.
add_executable(atx-vol-discrete-div-tests
    discrete_div_american_test.cpp
    oracle_dividends_test.cpp
    ../tools/oracle_dividends.cpp)
target_link_libraries(atx-vol-discrete-div-tests
    PRIVATE atx::vol atx::core GTest::gtest_main atx_warnings)
target_include_directories(atx-vol-discrete-div-tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# Same reason as the convention target: one ctest case per gtest case, so a
# vanished suite reports "No tests were found" instead of a vacuous 1/1 pass.
gtest_discover_tests(atx-vol-discrete-div-tests
    PROPERTIES SKIP_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}"
    DISCOVERY_MODE PRE_TEST)

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
    "MultinamePipeline.*:Corpus.*:CorpusGeneratedProperty.*:CorpusBuildSession.*:QualifiedCorpus.*:OpraBreadthCorpus.*:SpyFitCorpus.*:SigmaInterpCorpus.*:CallGreeksAl.*:CallGreeksFd.*:AndersenLake.*:AndersenLakeRegime.*:ImplicitDiff.*:SigmaInterp.*:CurveFitParallel.*:CurveFitParity.*:CurveSurfaceNoArb.*:SpyBidAskRegression.*:SpyPortfolioPnl.*:PnlGreeksConsistency.*:SpyArchiveRoundTrip.*:SpyRealCalendarReporting.*:SpyRealOpra.*:BacktestReal.*:BreadthRegime.*:WarmAcrossTime.*:BoundaryHoist.*:Dispersion.*:CorrectionCache.*:PricerFitterTest.*:PreparedPortfolio.*:AmericanGreeks.*:*SurfaceV2Qualification*:*SurfaceV2Fallback*:EarningsReproSmoke.*:EarningsValidation.*:AmznEarnings.*:BevPathLoader.*:DiscreteDivReferenceMatrix.*:DiscreteDivProductionMatrix.*")

# PRE_TEST: enumerate at ctest time, not as a POST_BUILD step -- relinking no longer
# pays TWO test-enumeration runs of this ~2600-test binary (one per call below), and a
# non-launchable exe no longer fails the *build*.
gtest_discover_tests(atx-vol-tests
    TEST_FILTER "-${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS atx_vol_fast SKIP_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}"
    DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(atx-vol-tests
    TEST_FILTER "${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS atx_vol_slow SKIP_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}"
    DISCOVERY_MODE PRE_TEST)
# VrpModel/VrpTrain cases keep the fast-lane label they had inside atx-vol-tests;
# their exe exists only when atx-engine is configured (see its definition above).
if(TARGET atx-vol-vrp-model-tests)
    gtest_discover_tests(atx-vol-vrp-model-tests
        PROPERTIES LABELS atx_vol_fast SKIP_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}"
        DISCOVERY_MODE PRE_TEST)
endif()

# Oracle RSI charter stage 4 (Mode B). scripts/oracle-targeted-gate.ps1
# -Gate mode_b_targeted_tests runs `ctest -R ^mode_b_targeted_tests$`, so a
# ctest case must carry EXACTLY that name. Neither mechanism above can provide
# one: gtest_discover_tests registers each case under its fully-qualified gtest
# name (OracleBenchModeB.X), which that anchored -R cannot match, and ctest
# LABELS are not selectable by -R at all. Hence one explicit add_test ALONGSIDE
# the discovery, not instead of it — every OracleBenchModeB case is therefore
# run twice in a full sweep, once under its own discovered id (pinned in
# $script:OracleBenchTestIds) and once inside this aggregate gate case. The
# duplication is deliberate; the alternative is a gate that selects nothing.
#
# The bare --gtest_filter case is exactly the hazard the convention-tests
# comment above describes: gtest exits 0 and reports "1/1 passed" when the
# filter matches ZERO tests, which the charter treats as a contract failure.
# PASS_REGULAR_EXPRESSION pins the exact case COUNT, so a renamed or vanished
# Mode B case fails this gate instead of passing it vacuously.
# FAIL_REGULAR_EXPRESSION is required beside it because setting a pass pattern
# makes ctest IGNORE the process exit code — without it a genuine assertion
# failure would still match "[  PASSED  ] 12 tests." never being printed... and
# in the partial-failure case gtest prints a LOWER passed count, so the pair
# closes both doors. Adding a Mode B case means bumping the 12 here (moved
# 5 -> 9 when the European-leg inversion landed its four cases, 9 -> 12 when
# the tree-leg lattice inversion landed its three).
add_test(NAME mode_b_targeted_tests
         COMMAND atx-vol-tests --gtest_filter=OracleBenchModeB.*)
set_tests_properties(mode_b_targeted_tests PROPERTIES
    PASS_REGULAR_EXPRESSION "\\[  PASSED  \\] 12 tests\\."
    FAIL_REGULAR_EXPRESSION "\\[  FAILED  \\]"
    LABELS atx_vol_fast)

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
    add_test(
        NAME Mag7DispersionReport
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/mag7_dispersion_report_test.py
    )
    set_tests_properties(Mag7DispersionReport PROPERTIES LABELS atx_vol)
    add_test(
        NAME SpyDispersionPnlReport
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/spy_dispersion_pnl_report_test.py
    )
    set_tests_properties(SpyDispersionPnlReport PROPERTIES LABELS atx_vol)
    # SR-DIVS: the `ddiv` -> discrete-schedule emitter (tools/spiderrock_dividends.py)
    # that feeds `atx-vol-chain-export --dividends`. Gated here because the emitter
    # MIRRORS the validated C++ reconstructor (tools/oracle_dividends.*) and a mirror
    # nobody runs is a copy that drifts. Pure tmp-dir fixtures -- no store, no network.
    add_test(
        NAME SpiderRockDividends
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/spiderrock_dividends_test.py
    )
    set_tests_properties(SpiderRockDividends PROPERTIES LABELS atx_vol)
    # vrp-ml round 2 gate: the panel-QA checker (scripts/vrp_panel_qa.py) grew a
    # stdlib-unittest suite (scripts/vrp_panel_qa_test.py, 14 cases, pure tmp-dir
    # fixtures). Registered here so the QA tool that guards the frozen
    # vrp_panel_v1 contract is itself gated; runs from scripts/ so its
    # `from vrp_panel_qa import ...` resolves via sys.path[0].
    add_test(
        NAME VrpPanelQa
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/vrp_panel_qa_test.py
    )
    set_tests_properties(VrpPanelQa PROPERTIES LABELS atx_vol)
    # FIX-5/I6: the atx-vol Python suite — eight test_*.py modules that, before
    # this line, were registered with ZERO ctest tests. `grep -rn "pytest"
    # --include=CMakeLists.txt .` returned nothing repo-wide and a grep for any
    # pytest test name in the whole-repo gate log returned 0, so an entire
    # workstream's output was gated by nothing. The driver explains why this is a
    # script and not a bare `-m pytest` (extension guard, source pinning, and two
    # host stalls that hang pytest); it exits SKIP_RETURN_CODE when the compiled
    # extension is absent, so the lane shows as Skipped-with-a-reason in the gate
    # log rather than vanishing. atx-vol/python is a STANDALONE scikit-build-core
    # project, not an add_subdirectory of this build, so there is no _core target
    # here to guard on at configure time — hence the run-time guard.
    add_test(
        NAME atx-vol-python
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/_ctest_pytest_driver.py
    )
    set_tests_properties(atx-vol-python PROPERTIES
        LABELS atx_vol
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

    # Standalone offline tools import their source by exact path and do not use
    # the compiled atxvol extension. Keep its checkout guard out of these lanes;
    # missing Python dependencies or skipped checks must fail explicitly.
    add_test(NAME atx-vol-quote-benchmark-tools
        COMMAND ${Python3_EXECUTABLE} -m pytest --noconftest -q
            ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_quote_benchmark.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_american_audit_compare.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_compose_predictions_by_status.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_shape_cohort.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_build_universe_dataset.py)
    set_tests_properties(atx-vol-quote-benchmark-tools PROPERTIES
        LABELS atx_vol_tools TIMEOUT 120
        FAIL_REGULAR_EXPRESSION "[0-9]+ skipped")
    if(ATX_BUILD_TOOLS)
        add_test(NAME atx-vol-quote-benchmark-native
            COMMAND ${CMAKE_COMMAND} -E env
                "ATX_QUOTE_BENCH_BINARY=$<TARGET_FILE:atx-vol-quote-bench>"
                ${Python3_EXECUTABLE} -m pytest --noconftest -q
                ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_quote_bench_cli.py
                ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_common_markov_bench_cli.py)
        set_tests_properties(atx-vol-quote-benchmark-native PROPERTIES
            LABELS atx_vol_tools TIMEOUT 120
            FAIL_REGULAR_EXPRESSION "[0-9]+ skipped")
        add_test(NAME atx-vol-american-markov-audit-native
            COMMAND ${CMAKE_COMMAND} -E env
                "ATX_AMERICAN_MARKOV_AUDIT_BINARY=$<TARGET_FILE:atx-vol-american-markov-audit>"
                ${Python3_EXECUTABLE} -m pytest --noconftest -q
                ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_american_markov_audit_cli.py)
        set_tests_properties(atx-vol-american-markov-audit-native PROPERTIES
            LABELS atx_vol_tools TIMEOUT 120
            FAIL_REGULAR_EXPRESSION "[0-9]+ skipped")
        add_test(NAME atx-vol-quote-benchmark-campaign
            COMMAND ${CMAKE_COMMAND} -E env
                "ATX_QUOTE_BENCH_BINARY=$<TARGET_FILE:atx-vol-quote-bench>"
                ${Python3_EXECUTABLE} -m pytest --noconftest -q
                ${CMAKE_CURRENT_SOURCE_DIR}/../python/tests/test_quote_benchmark_campaign.py)
        set_tests_properties(atx-vol-quote-benchmark-campaign PROPERTIES
            LABELS atx_vol_tools TIMEOUT 120
            FAIL_REGULAR_EXPRESSION "[0-9]+ skipped")
    endif()
endif()

# PR-C2 non-AVX2 scalar leg: a registered ctest lane (`atx-vol-pricing-forcescalar`,
# selectable with -R) that runs the pricing correctness suites with the whole process
# pinned to the scalar ISA (ATX_SIMD_ISA=ForceScalar), keeping the scalar
# patch-paths honest against the Auto-ON AVX2 marks. The filter runs only scalar
# VALUE suites plus the ScalarLegEnv self-check that PROVES the env diverted the
# process onto scalar (use_avx2()==false on an AVX2 host). It EXCLUDES the *Batch*
# suites (american_batch_test) — those install their own SIMD ISA override and
# restore it to Auto, which would clobber the env-seeded ForceScalar for later
# tests in this process. Under a normal run these tests run Auto via the
# gtest_discover labels above; here they re-run under ForceScalar.
add_test(
    NAME atx-vol-pricing-forcescalar
    COMMAND atx-vol-tests
        --gtest_filter=ScalarLegEnv.*:American*:AmericanIv*:CorrectionCache*:Bulk*:VolaSession*:*Deam*:*Parity*:-*Batch*:AvxBoundary*:VectorMath*:Simd*
)
# L7-T4: this lane is ONE ctest entry over hundreds of gtest cases, so a blanket
# SKIP_REGULAR_EXPRESSION would mark it skipped "regardless of the process exit
# code" (CMake's words) and hide a real failure that happened to share the run
# with a legitimately fixture-gated skip. The vacuity that actually matters here
# is narrower and has a name: `ScalarLegEnv.ForceScalarEnvSeedsScalarOverride`
# GTEST_SKIPs exactly when ATX_SIMD_ISA did NOT reach the process, i.e. exactly
# when every other case in this lane re-ran under the ordinary Auto ISA and the
# whole scalar leg proved nothing. Failing on THAT one skip line is loud where it
# should be and silent about the American*/CorrectionCache* fixture gates, which
# the per-case discovered lane already reports as Skipped in their own right.
set_tests_properties(atx-vol-pricing-forcescalar PROPERTIES
    ENVIRONMENT "ATX_SIMD_ISA=ForceScalar"
    FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\] ScalarLegEnv\\."
    LABELS atx_vol)

# Task F-8 fix round 8 (I-A): the P&L-explain column gate, moved off source-text
# parsing and onto the emitted artifact. The example TU is compiled with its
# `main` suppressed — the same shape as `spx-wilmott-repro-tests` in the parent
# CMakeLists — so the test drives the exact `attach_swap_columns` the executable
# ships, writes a real TSV, and diffs that file's header against
# `swap_explain_columns()`.
#
# WHY A SEPARATE TARGET rather than another source in `atx-vol-tests`:
# ATX_VARSWAP_COMPARE_NO_MAIN is a target-wide define, and the example TU opens
# with a file-scope `using namespace atx::vol;`. Keeping it in its own binary
# means neither reaches the main suite. It does NOT need ATX_BUILD_EXAMPLES —
# the TU is compiled here directly, so the gate runs in the default test build
# even though the example EXECUTABLE stays opt-in.
add_executable(varswap-compare-columns-tests
    varswap_compare_columns_test.cpp
    ../examples/varswap_compare_example.cpp)
# The gate TU includes "backtest/backtest_series_columns.hpp" straight out of
# src/ (the column manifest is deliberately not public API), so this target
# needs the same ../src include rung atx-vol-tests carries above — atx::vol
# does not export its src/ directory to consumers.
target_include_directories(varswap-compare-columns-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_compile_definitions(varswap-compare-columns-tests PRIVATE
    ATX_VARSWAP_COMPARE_NO_MAIN)
target_link_libraries(varswap-compare-columns-tests PRIVATE
    atx::vol atx::core GTest::gtest_main atx_warnings)
add_test(NAME VarswapCompareColumns COMMAND varswap-compare-columns-tests)
# L7-T4: a whole-binary lane, so the same reasoning as atx-vol-pricing-forcescalar
# applies — FAIL, not SKIP, because SKIP_REGULAR_EXPRESSION overrides the exit
# code and would swallow a co-occurring failure. This gate has NO fixture
# dependency (it compiles the example TU, writes a TSV to a temp dir and diffs
# the header), so any GTEST_SKIP here means the column contract stopped being
# checked, which must be loud rather than green.
set_tests_properties(VarswapCompareColumns PROPERTIES
    FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
    LABELS "atx_vol;atx_vol_fast")

# THE F-8 VERIFICATION FILTER, written down because a count without its filter is
# one string short of checkable. A reviewer could not re-derive a reported
# "569 enumerated / 567 passed" and correctly declined to guess a filter and then
# report agreement with its own guess. This is an ad-hoc lane selector, NOT a
# scope with a label; the label scopes remain `ctest -L atx_vol` (3204) and
# `-L atx_vol_fast` (2953), and those are what a reviewer can re-derive unaided.
#
#   ctest -R "ScenarioGrid|BacktestSwap|DerivPnl|DerivGreeks|DerivBook|SmileGreeks|SurfaceOverlay|StickyStrike|VolUmbrella|RunArchive|RunReport|Tearsheet|Backtest|SurfaceDb|VarswapCompareColumns|SpxWilmottRepro"
#
# The gap between "enumerated" and "executed" is gtest-`DISABLED_` entries, which
# `ctest -N` lists but no run counts. Runtime SKIPs (absent real-data fixtures)
# used to close that gap silently by counting as passed; since L7-T4 they report
# `***Skipped` per case and are listed under ctest's "The following tests did not
# run", so an enumerated-vs-passed gap now has exactly two named causes.

# ── alpha layer (include/atx/vol/alpha/**) ──────────────────────────────────
# Its own binary rather than another source in `atx-vol-tests`: the layer is
# header-only and links nothing but atx::core, so a 2-second target-scoped
# build is the whole iterate loop for it. Folding it into the 200-source
# monolith would trade that for a link of the entire vol library.
add_executable(atx-vol-alpha-tests
    alpha_registry_test.cpp
    alpha_strategy_test.cpp)
target_link_libraries(atx-vol-alpha-tests PRIVATE
    atx::vol atx::core GTest::gtest_main atx_warnings)
add_test(NAME AlphaRegistry COMMAND atx-vol-alpha-tests)
# L7-T4: same shape and same reason as VarswapCompareColumns — one ctest entry
# per BINARY, and the alpha layer is header-only with no fixture or environment
# gate anywhere in it, so a skip is vacuity, not an absent optional input.
set_tests_properties(AlphaRegistry PROPERTIES
    FAIL_REGULAR_EXPRESSION "\\[  SKIPPED \\]"
    LABELS "atx_vol;atx_vol_fast")

# Module test slices for lanes (atx-vol-<module>-slice): partitioned from
# atx-vol-tests' SOURCES, so keep this include LAST. See slices.cmake.
include(${CMAKE_CURRENT_SOURCE_DIR}/slices.cmake)
