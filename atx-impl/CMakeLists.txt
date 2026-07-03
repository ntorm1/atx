# atx-impl: end-to-end equity alpha pipeline reference binary.
# Split: a STATIC lib holds all logic so the test exe can link it (you cannot link
# an executable target). The atx-impl exe is just main.cpp over the lib.
add_library(atx-impl-core STATIC
    src/config.cpp
    src/dispatch.cpp
    src/artifacts.cpp
    src/serialize_panel.cpp
    src/serialize_genome.cpp
    src/stage_load.cpp
    src/stage_regime.cpp
    src/stage_panel.cpp
    # Incremental panel append: append == full-rebuild byte-identical (p7 S6-1)
    src/append_history_panel.cpp
    # FINRA short-interest augment core (p7 S2-1 / track-b B1; CLI deferred to S7)
    src/stage_augment.cpp
    src/stage_discover.cpp
    src/stage_sweep.cpp
    src/store_progress_sink.cpp
    src/stage_combine.cpp
    src/stage_riskmodel.cpp
    src/stage_optimize.cpp
    src/stage_report.cpp
    src/stage_run.cpp)
target_include_directories(atx-impl-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(atx-impl-core PUBLIC cxx_std_20)
target_link_libraries(atx-impl-core
    PUBLIC  atx::core atx::engine atx::tsdb
    PRIVATE atx_warnings)
find_package(Threads REQUIRED)
target_link_libraries(atx-impl-core PRIVATE Threads::Threads)

# ---------------------------------------------------------------------------
# engine_git_sha provenance bake (p7 S6-4).
# Resolve the engine's git commit at configure time and bake it into a compile
# definition ATX_ENGINE_GIT_SHA consumed by stage_discover.cpp. Appends "-dirty"
# when the working tree has uncommitted changes; falls back to "unknown" if git
# is unavailable (CI / source tarball). This is provenance only — it never enters
# panel.bin or the discover search digest (guarded by the S6-5 tripwire).
# ---------------------------------------------------------------------------
set(ATX_ENGINE_GIT_SHA "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _atx_git_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE _atx_git_rc
        ERROR_QUIET)
    if(_atx_git_rc EQUAL 0 AND _atx_git_sha)
        set(ATX_ENGINE_GIT_SHA "${_atx_git_sha}")
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE _atx_git_dirty
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_atx_git_dirty)
            set(ATX_ENGINE_GIT_SHA "${ATX_ENGINE_GIT_SHA}-dirty")
        endif()
    endif()
endif()
message(STATUS "atx-impl engine_git_sha: ${ATX_ENGINE_GIT_SHA}")
target_compile_definitions(atx-impl-core PRIVATE
    ATX_ENGINE_GIT_SHA="${ATX_ENGINE_GIT_SHA}")

add_executable(atx-impl src/main.cpp)
target_link_libraries(atx-impl PRIVATE atx-impl-core atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
