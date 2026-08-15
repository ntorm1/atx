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
    essvi_calib_test.cpp
    essvi_deam_test.cpp
    svi_calib_test.cpp
    calib_robustness_test.cpp
    cstar_test.cpp
    cstar_calib_test.cpp
    dense_slice_test.cpp
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
)

# Oracle RSI charter stage 2: the three non-driver TUs of atx-vol-oracle-bench
# (headers + implementations under ../tools) compile into the test exe so
# OracleBench* exercises the shipped implementation, not a copy. tools/ is
# already a PRIVATE quoted-include root of this target (see
# target_include_directories below), which is how oracle_bench_test.cpp
# resolves both the headers and its textual #include of the driver TU.
target_sources(atx-vol-tests PRIVATE
    ../tools/oracle_conventions.cpp
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
    "MultinamePipeline.*:Corpus.*:CorpusGeneratedProperty.*:CorpusBuildSession.*:QualifiedCorpus.*:OpraBreadthCorpus.*:SpyFitCorpus.*:SigmaInterpCorpus.*:CallGreeksAl.*:CallGreeksFd.*:AndersenLake.*:AndersenLakeRegime.*:ImplicitDiff.*:SigmaInterp.*:CurveFitParallel.*:CurveFitParity.*:CurveSurfaceNoArb.*:SpyBidAskRegression.*:SpyPortfolioPnl.*:PnlGreeksConsistency.*:SpyArchiveRoundTrip.*:SpyRealCalendarReporting.*:SpyRealOpra.*:BacktestReal.*:BreadthRegime.*:WarmAcrossTime.*:BoundaryHoist.*:Dispersion.*:CorrectionCache.*:PricerFitterTest.*:PreparedPortfolio.*:AmericanGreeks.*:*SurfaceV2Qualification*:*SurfaceV2Fallback*:EarningsReproSmoke.*:EarningsValidation.*:AmznEarnings.*:BevPathLoader.*")

# PRE_TEST: enumerate at ctest time, not as a POST_BUILD step -- relinking no longer
# pays TWO test-enumeration runs of this ~2600-test binary (one per call below), and a
# non-launchable exe no longer fails the *build*.
gtest_discover_tests(atx-vol-tests
    TEST_FILTER "-${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS atx_vol_fast
    DISCOVERY_MODE PRE_TEST)
gtest_discover_tests(atx-vol-tests
    TEST_FILTER "${ATX_VOL_SLOW_FILTER}"
    PROPERTIES LABELS atx_vol_slow
    DISCOVERY_MODE PRE_TEST)

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
set_tests_properties(atx-vol-pricing-forcescalar PROPERTIES
    ENVIRONMENT "ATX_SIMD_ISA=ForceScalar"
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
target_compile_definitions(varswap-compare-columns-tests PRIVATE
    ATX_VARSWAP_COMPARE_NO_MAIN)
target_link_libraries(varswap-compare-columns-tests PRIVATE
    atx::vol atx::core GTest::gtest_main atx_warnings)
add_test(NAME VarswapCompareColumns COMMAND varswap-compare-columns-tests)
set_tests_properties(VarswapCompareColumns PROPERTIES LABELS "atx_vol;atx_vol_fast")

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
# `ctest -N` lists but no run counts; runtime SKIPs (absent real-data fixtures)
# execute and count as passed.
