# ---- atx-vol module test slices (build-infra sprint 2026-10-16) -------------
#
# WHY: lanes must never build the monolithic atx-vol-tests. It is ~215 test TUs
# behind a PCH plus one giant link (138-181 s under ThinLTO), and its PCH-consumer
# objects never transfer between worktrees through ccache: the clang .pch embeds
# absolute paths and the target carries absolute-path -D values, so every pool
# tree recompiles all of them (measured: 0/3 hits cross-tree on black76/greeks,
# vs 242/242 direct hits for the PCH-less focused targets). A lane needs one
# suite, so it builds the slice that owns that suite instead.
#
# WHAT: every *_test.cpp of atx-vol-tests is partitioned into one module slice
# executable, atx-vol-<slice>-slice, by the stem tables below (stem = basename
# minus "_test.cpp"). The partition is computed from atx-vol-tests' own SOURCES
# at configure time, so a test file added to atx-vol-tests needs NO edit here:
# an unlisted stem falls to a prefix rule, and failing that to the `misc` slice.
# Suites that already own a focused exe (atx-vol-quote-slice-tests, the
# american-markov family, atx-vol-umbrella-tests) are not duplicated into slices.
#
# Slices are PCH-less on purpose (cross-tree cacheable) and are registered with
# ctest per gtest case under the prefix "<slice>/" and label atx_vol_slice only,
# so release qualification (-L atx_vol_fast / atx_vol_slow) still runs each case
# exactly once, through atx-vol-tests. Lane loop:
#   atx-build.ps1 build atx-vol-fitting-slice -Preset vol-rel -Jobs 2
#   atx-build.ps1 -Ctest -Preset vol-rel -R "^fitting/CalibTest\."
#
# The monolithic target is untouched: these slices only read its properties.
# Included at the END of tests/CMakeLists.txt so every target_sources() and
# target_compile_definitions() call on atx-vol-tests has already run.

set(ATX_VOL_SLICES core pricing simd fitting marketdata storage analytics backtest
    dispersion oracle misc)

# Stems owned by a pre-existing focused exe: skipped here.
set(ATX_VOL_SLICE_OWNED_ELSEWHERE
    quote_slice american_price_slice quote_fit_policy               # atx-vol-quote-slice-tests
    quote_surface slice_channel term_prior                          # atx-vol-quote-slice-tests
    quote_slice_vol_space                                           # atx-vol-quote-slice-tests
    quote_slice_identification                                      # atx-vol-quote-slice-tests
    quote_slice_publication                                         # atx-vol-quote-slice-tests
    american_markov american_markov_batch american_markov_resolvent # atx-vol-american-markov-tests
    american_markov_pwl american_markov_grid pwl_strike
    american_markov_fit american_markov_seed                        # atx-vol-american-markov-*-tests
    american_markov_market american_markov_policy
    dividend_schedule                                               # atx-vol-dividend-schedule-*
    vol_umbrella)                                                   # atx-vol-umbrella-tests

set(ATX_VOL_SLICE_core_STEMS
    version log_sink counters phase_profile solve_ledger vol_time parallel_for
    process_scratch test_paths)
set(ATX_VOL_SLICE_pricing_STEMS
    black76 greeks implied_vol iv_seed iv_convergence scalar_erfc
    american american_batch american_bindkey american_bulk_rung american_exercise_boundary
    american_geometry american_iv american_negrate_domain adjoint_greeks adjusted_greeks dividend rates_curve
    derivatives deriv_distribution deriv_fixtures deriv_greeks cboe_strip swap_leg theo
    pricing_executor batch laned_greek_ok_stamp evaluate_resolved_ok_stamp)
set(ATX_VOL_SLICE_fitting_STEMS
    surface vol_surface correction arb risk_surface_validation surface_v2_qualification
    convex_recovery quote_feasibility calib calib_robustness prepared_fitting c8 c8_calib
    essvi_anchored essvi_calib essvi_deam svi_calib cstar cstar_calib s3 dense_slice
    profile fit_policy curve_selector projection fit_metrics deamer parity surface_parity
    session session_serve pricer_fitter vol_curve spline_curve curve_noarb curve_fit_parallel
    curve_fit_carry_fallback curve_fit_coverage robust surface_overlay mark_rescue)
set(ATX_VOL_SLICE_marketdata_STEMS
    universe data opra_panel opra_hive opra_batch corpus listed_opra
    listed_definitions_cache occ_ess ingest_scale catalog spy_real spy_fit_corpus
    opra_breadth_corpus breadth_regime spy_bidask_regression)
set(ATX_VOL_SLICE_storage_STEMS
    run_archive run_diagnostics surface_db surface_db_populate surface_db_build
    surface_db_admin surface_db_populate_policy backtest_db backtest_db_build research_db
    writer_lock surface_archive_v2 surface_archive_v2_adversarial surface_archive_durability
    zc_view_parity_diag spy_archive_roundtrip track_key track_store track_compact_reconcile
    track_gc sweep_driver run_report chain_export)
set(ATX_VOL_SLICE_analytics_STEMS
    contract_projection historical_projection var var_report var_validation event_vol
    scenario_grid pnl_attribution deriv_pnl analytics_primitives analytics_density
    analytics_aggregate analytics_io earnings_term_fit earnings_forecast_loader
    earnings_repro_smoke earnings_validation amzn_earnings realized_vol breakeven)
set(ATX_VOL_SLICE_backtest_STEMS
    deriv_book panel priced_surface prepared_portfolio portfolio_pricer margin backtest
    backtest_incremental backtest_template backtest_exec backtest_leak backtest_driver
    backtest_real backtest_swap research_validation strategy_pipeline quant_pipeline
    strategy tearsheet benchmark_stats structure_panel vega_panel spy_strangle_backtest
    spy_portfolio_pnl pnl_greeks_consistency lifecycle_integration surface_db_backtest
    bev_label_factory_gate)
set(ATX_VOL_SLICE_dispersion_STEMS
    dispersion dispersion_strangle dispersion_workflow dispersion_run dispersion_run_config
    dispersion_realism_flags dispersion_backtest_regime listed_dispersion
    listed_dispersion_schedule listed_dispersion_reconciliation listed_dispersion_strategy
    listed_dispersion_pipeline mag7_dispersion_backtest spy_dispersion_pnl
    surface_db_dispersion_backtest multiname_pipeline)
set(ATX_VOL_SLICE_oracle_STEMS oracle_bench)

# Fallback for stems added after this table was written: first matching prefix
# wins, then `misc`.
set(ATX_VOL_SLICE_PREFIX_RULES
    "simd_=simd" "american_=pricing" "deriv_=pricing" "iv_=pricing"
    "surface_db_=storage" "surface_archive_=storage" "track_=storage" "backtest_db_=storage"
    "listed_dispersion_=dispersion" "dispersion_=dispersion"
    "backtest_=backtest" "strategy_=backtest"
    "analytics_=analytics" "earnings_=analytics" "var_=analytics"
    "opra_=marketdata" "listed_=marketdata" "spy_=marketdata"
    "curve_=fitting" "essvi_=fitting" "cstar_=fitting" "c8_=fitting" "surface_=fitting"
    "oracle_=oracle")

get_target_property(_atx_vol_mono_sources atx-vol-tests SOURCES)
get_target_property(_atx_vol_mono_defs atx-vol-tests COMPILE_DEFINITIONS)
get_target_property(_atx_vol_mono_incs atx-vol-tests INCLUDE_DIRECTORIES)
if(NOT _atx_vol_mono_defs)
    set(_atx_vol_mono_defs "")
endif()

foreach(_slice IN LISTS ATX_VOL_SLICES)
    set(_atx_vol_slice_${_slice}_SRCS "")
endforeach()
set(_atx_vol_slice_support_SRCS "")

foreach(_src IN LISTS _atx_vol_mono_sources)
    get_filename_component(_name "${_src}" NAME)
    if(NOT _name MATCHES "_test\\.cpp$")
        # support/*.cpp and ../tools/oracle_*.cpp: shared by every slice.
        list(APPEND _atx_vol_slice_support_SRCS "${_src}")
        continue()
    endif()
    string(REGEX REPLACE "_test\\.cpp$" "" _stem "${_name}")
    if(_stem IN_LIST ATX_VOL_SLICE_OWNED_ELSEWHERE)
        continue()
    endif()
    set(_owner "")
    foreach(_slice IN LISTS ATX_VOL_SLICES)
        if(_stem IN_LIST ATX_VOL_SLICE_${_slice}_STEMS)
            set(_owner "${_slice}")
            break()
        endif()
    endforeach()
    if(NOT _owner)
        foreach(_rule IN LISTS ATX_VOL_SLICE_PREFIX_RULES)
            string(REPLACE "=" ";" _pair "${_rule}")
            list(GET _pair 0 _prefix)
            list(GET _pair 1 _slice)
            string(FIND "${_stem}" "${_prefix}" _at)
            if(_at EQUAL 0)
                set(_owner "${_slice}")
                break()
            endif()
        endforeach()
    endif()
    if(NOT _owner)
        set(_owner misc)
    endif()
    list(APPEND _atx_vol_slice_${_owner}_SRCS "${_src}")
endforeach()

# Support TUs compile once per tree and link into every slice.
add_library(atx-vol-test-slice-support OBJECT ${_atx_vol_slice_support_SRCS})
target_link_libraries(atx-vol-test-slice-support PUBLIC
    atx::vol atx::core GTest::gtest atx_warnings atx-test-scratch atx-vol-test-paths)
target_include_directories(atx-vol-test-slice-support PRIVATE ${_atx_vol_mono_incs})
target_compile_definitions(atx-vol-test-slice-support PRIVATE ${_atx_vol_mono_defs})

set(ATX_VOL_SLICE_TARGETS "")
foreach(_slice IN LISTS ATX_VOL_SLICES)
    if(NOT _atx_vol_slice_${_slice}_SRCS)
        continue()
    endif()
    set(_tgt "atx-vol-${_slice}-slice")
    add_executable(${_tgt} ${_atx_vol_slice_${_slice}_SRCS})
    target_link_libraries(${_tgt} PRIVATE
        atx-vol-test-slice-support
        atx::vol atx::core GTest::gtest_main atx_warnings atx-test-scratch atx-vol-test-paths)
    if(WIN32)
        target_link_libraries(${_tgt} PRIVATE psapi)
    endif()
    target_include_directories(${_tgt} PRIVATE ${_atx_vol_mono_incs})
    target_compile_definitions(${_tgt} PRIVATE ${_atx_vol_mono_defs})
    gtest_discover_tests(${_tgt}
        TEST_PREFIX "${_slice}/"
        PROPERTIES LABELS atx_vol_slice SKIP_REGULAR_EXPRESSION "${ATX_VOL_GTEST_SKIP_REGEX}"
        DISCOVERY_MODE PRE_TEST)
    list(LENGTH _atx_vol_slice_${_slice}_SRCS _n)
    list(APPEND ATX_VOL_SLICE_TARGETS "${_tgt}(${_n})")
endforeach()
message(STATUS "atx-vol test slices: ${ATX_VOL_SLICE_TARGETS}")
