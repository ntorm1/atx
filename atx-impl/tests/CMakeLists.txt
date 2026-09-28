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
    strategy_ic_runner_test.cpp
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/ic_screen_test.cpp"
    "${PROJECT_SOURCE_DIR}/atx-engine/tests/factory/ic_research_test.cpp")
atx_impl_test_target_environment(atx-impl-strategy-ic-tests)

add_executable(atx-impl-strategy-target-tests EXCLUDE_FROM_ALL
    strategy_target_replay_test.cpp
    strategy_price_exposures_test.cpp
    strategy_nav_replay_test.cpp
    strategy_live_test.cpp
    strategy_cost_v2_test.cpp
    strategy_risk_model_test.cpp)
atx_impl_test_target_environment(atx-impl-strategy-target-tests)
