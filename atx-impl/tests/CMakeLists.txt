find_package(Threads REQUIRED)

# These existing fixtures select their own CRT declaration policy before includes
# (getenv / optional-data hooks). A forced PCH would precede that local policy.
# Keep the exception per source, shared by ordinary and focused owning targets.
set_source_files_properties(
    alpha101_orats_test.cpp discover_test.cpp fundamental_zoo_test.cpp
    seed_parse_test.cpp single_alpha_capacity_test.cpp
    PROPERTIES SKIP_PRECOMPILE_HEADERS ON)


# Keep the ordinary/focused test targets and the PCH carrier in exactly the same
# compile environment. In particular, GTest, JSON, miniz and the fixture-path
# definition make this different from the production library's environment.
function(atx_impl_test_compile_environment target)
    target_link_libraries(${target} PRIVATE
        atx-impl-core GTest::gtest_main atx_warnings nlohmann_json::nlohmann_json
        Threads::Threads atx_miniz)
    # alpha101_orats_test reads fixtures/alpha101.txt at runtime.
    target_compile_definitions(${target} PRIVATE
        ATX_IMPL_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endfunction()

# A stable carrier avoids coupling focused checks to the ordinary test binary
# or its worker dependency. Scratch initialization belongs in executables only.
if(ATX_USE_PCH)
    add_library(atx-impl-test-pch OBJECT EXCLUDE_FROM_ALL pch.cpp)
    atx_impl_test_compile_environment(atx-impl-test-pch)
    target_precompile_headers(atx-impl-test-pch PRIVATE pch.hpp)
endif()

function(atx_impl_test_target_environment target)
    atx_impl_test_compile_environment(${target})
    target_link_libraries(${target} PRIVATE atx-test-scratch)
    if(ATX_USE_PCH)
        target_precompile_headers(${target} REUSE_FROM atx-impl-test-pch)
    endif()
endfunction()

file(GLOB ATX_IMPL_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(atx-impl-tests ${ATX_IMPL_TEST_SOURCES})
# AtxImplSweep.SweepParallelEqualsSequentialDigest spawns atx-shm-worker.exe (process executor);
# ensure the worker is built into bin/ whenever the test target is built.
add_dependencies(atx-impl-tests atx-shm-worker)
# FIX-3/F3-B: per-process temp root. 42 files in this suite build scratch paths as
# fixed leaves under the machine-wide temp dir (canonically sweep_test.cpp's
# atx_sweep_* library/output roots) and remove_all them on entry, so two concurrent
# runs of this binary delete each other's trees mid-test. See //tests/CMakeLists.txt.
atx_impl_test_target_environment(atx-impl-tests)
include(GoogleTest)
# PRE_TEST: enumerate at ctest time, not as a POST_BUILD step -- relinking no longer
# pays a test-enumeration run (and a non-launchable exe no longer fails the *build*).
gtest_discover_tests(atx-impl-tests DISCOVERY_MODE PRE_TEST)

# Focused search-screen and evaluation-recipe checks on the production library.
# The ordinary owning suite and its CTest registrations remain unchanged.
add_executable(atx-impl-ic-screen-tests EXCLUDE_FROM_ALL
    ic_screen_config_test.cpp
    stage_equity_mine_core_test.cpp
    stage_equity_mine_pipeline_test.cpp
    stage_equity_mine_cli_test.cpp
    store_discover_test.cpp
    discover_test.cpp
    provenance_test.cpp)
atx_impl_test_target_environment(atx-impl-ic-screen-tests)

# Bounded W1 universe/provenance and allocation contracts. No worker dependency.
add_executable(atx-impl-w1-contract-tests EXCLUDE_FROM_ALL
    prereg_test.cpp
    config_equity_ic_test.cpp
    stage_equity_ic_test.cpp
    panel_test.cpp
    stage_equity_universe_test.cpp
    stage_data_provenance_test.cpp
    equity_allocation_test.cpp
    equity_allocation_factor_test.cpp
    stage_equity_baseline_test.cpp
    config_equity_book_test.cpp)
atx_impl_test_target_environment(atx-impl-w1-contract-tests)

# Narrow recent-strategy contracts reuse the existing stable test PCH. Include
# the shared execution ledger checks without building every factory/mine test.
add_executable(atx-impl-strategy-tests EXCLUDE_FROM_ALL
    strategy_dsl_test.cpp
    strategy_runner_test.cpp
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/data/strategy_data_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/execution_schedule_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/execution_cash_claim_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/execution_stock_transition_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/execution_objective_test.cpp")
atx_impl_test_target_environment(atx-impl-strategy-tests)

# Small postimplementation checks for the fast research path and its actual DSLs.
add_executable(atx-impl-strategy-ic-tests EXCLUDE_FROM_ALL
    strategy_ic_composition_test.cpp
    strategy_ic_theme_resid_test.cpp  # platform v8 R-11 (theme-resid-v1)
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/combine/combine_group_residualise_test.cpp"
    strategy_ic_runner_test.cpp
    strategy_marginal_ic_test.cpp
    # platform v8 R-10: composition ic-shrink-v1 (the rule and its engine kernel)
    strategy_ic_shrink_test.cpp
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/combine/combine_group_shrink_test.cpp"
    # platform v8 X (XCOMB): composition theme-erc-v1 (the rule and its engine kernel)
    strategy_ic_theme_erc_test.cpp
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/combine/combine_group_erc_test.cpp"
    # platform v8 Y (YCOMB): composition schedule theme-tsmom-v1 (the mass kernel and the schedule)
    strategy_ic_theme_tsmom_test.cpp
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/ic_screen_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/ic_research_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/combine/combine_marginal_rank_ic_test.cpp")
atx_impl_test_target_environment(atx-impl-strategy-ic-tests)

add_executable(atx-impl-strategy-target-tests EXCLUDE_FROM_ALL
    strategy_target_replay_test.cpp
    strategy_price_exposures_test.cpp
    strategy_nav_replay_test.cpp
    strategy_live_test.cpp
    strategy_cost_v2_test.cpp
    strategy_risk_model_test.cpp
    strategy_spo_test.cpp
    strategy_spo_pin_test.cpp
    strategy_spo_v3_test.cpp
    strategy_spo_v3_pin_test.cpp
    strategy_risk_target_test.cpp
    strategy_vol_target_test.cpp  # platform v8 Y (YCOMB): vol-target-v1
    strategy_norm_score_test.cpp  # platform v8 Y (YCOMB): norm-score-v1
    strategy_two_speed_test.cpp  # platform v8 Y-5 (YCOMB): two-speed-v1 (composition sleeves, NAV construction)
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/book/book_target_shaping_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/book/book_risk_target_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/book/book_inverse_vol_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/book/book_vol_target_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/book/book_normal_score_test.cpp"
    # platform v8 Y (YCOMB) rule Y-5: two-speed netted sleeves (engine kernel; NAV wiring is a follow-up)
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/book/book_two_speed_test.cpp")
atx_impl_test_target_environment(atx-impl-strategy-target-tests)

# Platform v8 H-3: the mining verb on its synthetic role, with the engine's signal-fitness
# search path, research IC fitness and op-catalogue tests it builds on. Run it directly with a
# gtest filter or by CTest label atx_research (registered at the end of this file; not in the
# atx_equity_strategy label).
add_executable(atx-impl-strategy-mine-tests EXCLUDE_FROM_ALL
    strategy_mine_test.cpp
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/factory_signal_fitness_test.cpp")
atx_impl_test_target_environment(atx-impl-strategy-mine-tests)

# Platform v8 E-3: the three strategy executables are ctest-visible under one label, so a built tree qualifies them
# with `ctest -L atx_equity_strategy`. They stay EXCLUDE_FROM_ALL: PRE_TEST discovery enumerates each at ctest time,
# and one that is not built appears as <target>_NOT_BUILT, which carries no label. The prefix keeps their names
# apart from the same sources registered by atx-impl-tests above and by the atx-engine groups.
foreach(_strategy_tests IN ITEMS
        atx-impl-strategy-tests atx-impl-strategy-ic-tests atx-impl-strategy-target-tests)
    gtest_discover_tests(${_strategy_tests} DISCOVERY_MODE PRE_TEST TEST_PREFIX "${_strategy_tests}."
        PROPERTIES LABELS atx_equity_strategy)
endforeach()

# P9 lane T1 (gate G-P4): the research test executables join CTest under the label atx_research (one label per
# target: a list value does not pass through gtest_discover_tests' PROPERTIES intact; the regex
# `ctest -L "atx_research|atx_equity_strategy"` selects both sets). Deferred to the end of this directory, so a target
# a later block defines is registered too; a name with no target is skipped, and a target already registered
# (CTEST_DISCOVERED_TEST_COUNTER set) is left alone. atx-engine-research-admission-tests is listed here as well in case
# lane B1 defines it outside atx-engine/tests (that directory's own deferred call registers it when it exists there).
function(atx_impl_register_research_tests)
    foreach(_research_tests IN ITEMS
            atx-impl-strategy-mine-tests          # platform v8 H-3 mining verb
            atx-engine-research-admission-tests)  # P9 lane B1 (research/admission), fallback registration
        if(NOT TARGET ${_research_tests})
            continue()
        endif()
        get_property(_registered TARGET ${_research_tests} PROPERTY CTEST_DISCOVERED_TEST_COUNTER SET)
        if(_registered)
            continue()
        endif()
        gtest_discover_tests(${_research_tests} DISCOVERY_MODE PRE_TEST TEST_PREFIX "${_research_tests}."
            PROPERTIES LABELS atx_research)
    endforeach()
endfunction()
cmake_language(DEFER CALL atx_impl_register_research_tests)
