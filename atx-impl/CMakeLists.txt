# atx-impl: end-to-end equity alpha pipeline reference binary.
# Split: a STATIC lib holds all logic so the test exe can link it (you cannot link
# an executable target). The atx-impl exe is just main.cpp over the lib.
add_library(atx-impl-core ${ATX_LIB_TYPE}
    src/config.cpp
    src/dispatch.cpp
    src/artifacts.cpp
    src/serialize_panel.cpp
    src/panel_artifact.cpp
    src/panel_pipeline.cpp
    src/stage_data_provenance.cpp
    src/serialize_genome.cpp
    src/stage_load.cpp
    src/stage_regime.cpp
    src/stage_panel.cpp
    src/asof_field.cpp
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
    src/replay_report.cpp
    src/replay_diagnostics.cpp
    src/equity_baseline_views.cpp
    src/stage_equity_baseline.cpp
    src/equity_allocation.cpp
    # Lane 8: combo -> per-decision preference seam for the equity book
    src/preference_source.cpp
    src/stage_equity_book.cpp
    src/trial_ledger.cpp
    src/prereg.cpp
    src/stage_equity_ic.cpp
    src/stage_equity_universe.cpp
    src/stage_run.cpp
    # p8 S2: the fund:: mega-alpha meta-book seam (opt-in; never called from stage_run.cpp)
    src/stage_metabook.cpp)
target_include_directories(atx-impl-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(atx-impl-core PUBLIC cxx_std_20)
target_link_libraries(atx-impl-core
    PUBLIC  atx::core atx::engine atx::tsdb
    PRIVATE atx_warnings nlohmann_json::nlohmann_json)
find_package(Threads REQUIRED)
target_link_libraries(atx-impl-core PRIVATE Threads::Threads)

# Stable third-party/std headers only; pipeline/config headers stay outside the
# PCH so an ordinary engine change does not invalidate this parse cache. PRIVATE
# keeps it out of downstream targets, and hygiene still disables ATX_USE_PCH.
if(ATX_USE_PCH)
    target_precompile_headers(atx-impl-core PRIVATE pch.hpp)
endif()

# ---------------------------------------------------------------------------
# engine_git_sha provenance bake (p7 S6-4).
# Resolve the engine's git commit at configure time and bake it into a
# private generated source consumed through build_provenance.hpp. Appends "-dirty"
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
# A changing command-line macro conflicts with clang-cl PCH reuse. Keep the
# value in one tiny generated implementation, outside the stable PCH payload.
# configure_file writes only when content changes, preserving no-op builds.
set(_atx_provenance_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/build_provenance.cpp")
configure_file(src/build_provenance.cpp.in "${_atx_provenance_cpp}" @ONLY)
target_sources(atx-impl-core PRIVATE "${_atx_provenance_cpp}")
set_source_files_properties("${_atx_provenance_cpp}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

add_executable(atx-impl src/main.cpp)
target_link_libraries(atx-impl PRIVATE atx-impl-core atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
# Lane 9: real-data DSL alpha mining stage (equity-mine)
target_sources(atx-impl-core PRIVATE src/stage_equity_mine.cpp)
