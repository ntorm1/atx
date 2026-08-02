# atx-options-engine: typed point-in-time bridge from listed-options research
# into the generic cross-sectional portfolio and execution stack.
add_library(atx-options-engine ${ATX_LIB_TYPE}
    src/option_adaptive_coordinator.cpp
    src/option_execution_replay.cpp
    src/option_pretrade_risk.cpp
    src/option_research_panel.cpp
    src/option_scenario_cube.cpp)
add_library(atx::options-engine ALIAS atx-options-engine)

target_include_directories(atx-options-engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(atx-options-engine PUBLIC cxx_std_20)
target_link_libraries(atx-options-engine
    PUBLIC
        atx::engine
        atx::vol
    PRIVATE
        atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()
