# Focused contract tests for the options/engine boundary.
add_executable(atx-options-engine-tests
    option_adaptive_coordinator_test.cpp
    option_execution_session_test.cpp
    option_execution_replay_test.cpp
    option_pretrade_risk_test.cpp
    option_research_panel_test.cpp
    option_scenario_cube_test.cpp)
target_link_libraries(atx-options-engine-tests
    PRIVATE
        atx::options-engine
        GTest::gtest_main
        atx_warnings)
include(GoogleTest)
gtest_discover_tests(atx-options-engine-tests DISCOVERY_MODE PRE_TEST)
