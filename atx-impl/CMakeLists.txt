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
    src/strategy_runner.cpp
    # IC runner (platform v8 B-3 split; shared internals in strategy_ic_detail.hpp)
    src/strategy_ic_runner.cpp
    src/strategy_ic_library.cpp
    src/strategy_ic_admission.cpp
    src/strategy_ic_signal_cache.cpp
    src/strategy_ic_result_cache.cpp
    src/strategy_ic_composition.cpp
    # platform v8 R-10: composition rule ic-shrink-v1 (verified by the theme_standardise rule table)
    src/strategy_ic_shrink.cpp
    src/strategy_ic_theme_resid.cpp  # platform v8 R-11: composition rule theme-resid-v1
    # platform v8 F-2: `atx-equity-strategy-ic marginal` (contract K6, marginal_ic.json)
    src/strategy_marginal_ic.cpp
    # platform v8 H-3: the shared research-role loader and the mining verb (mined-v1)
    src/strategy_research_role.cpp
    src/strategy_mine.cpp
    src/strategy_mine_pool.cpp
    src/strategy_mine_promote.cpp
    src/strategy_mine_rule.cpp
    src/strategy_mine_trials.cpp
    src/strategy_target_replay.cpp
    src/strategy_price_exposures.cpp
    src/strategy_nav_replay.cpp
    # v7 daily decide path: deploy manifest, decide verb (the NAV replay's own DECIDE)
    src/strategy_live.cpp
    # v7 W4: f64 holdings layout + reader, share orders, broker reconciliation
    src/strategy_holdings.cpp
    src/strategy_orders.cpp
    src/strategy_reconcile.cpp
    # platform v7 L4: cost model v2 + aim-partial-v6 + the NAV hook; atx-risk-v1 + bias harness
    src/strategy_cost_v2.cpp
    src/strategy_nav_v7.cpp
    src/strategy_risk_model.cpp
    src/strategy_risk_verb.cpp
    # platform v7 W1: spo-v1, the cost-aware single-period optimiser (FISTA, factor risk)
    src/strategy_spo.cpp
    # platform v8 R-6: spo-v3, target tracking (its published blocks; the engine's solver)
    src/strategy_spo_v3.cpp
    # platform v8 R-8: risk-target-v1, the NAV hook's ex-ante risk target (the engine's kernel)
    src/strategy_risk_target.cpp
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
# This TU intentionally changes CRT declarations before its first include to use
# portable exclusive fopen("wx"). Preserve that local environment: force-including
# the common PCH would parse the CRT before the source's suppression takes effect.
set_source_files_properties(src/trial_ledger.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

# Full recent-data IC runs need optimized VM/collection loops even in the warm
# iterative Debug tree. Keep the Debug CRT, assertions, FP and ISA settings;
# change only these private implementations, not the target or shared PCH.
# clang-cl /Ob0 leaves -fno-inline behind even after /O2 /Ob2; explicitly clear
# it with the public clang driver option. LLVM ignores the inherited /RTC1.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(SOURCE src/strategy_ic_runner.cpp src/strategy_ic_composition.cpp src/strategy_target_replay.cpp src/strategy_price_exposures.cpp src/strategy_nav_replay.cpp
        src/strategy_ic_library.cpp src/strategy_ic_admission.cpp src/strategy_ic_signal_cache.cpp
        src/strategy_ic_result_cache.cpp src/strategy_ic_shrink.cpp
        src/strategy_ic_theme_resid.cpp
        src/strategy_cost_v2.cpp src/strategy_nav_v7.cpp src/strategy_risk_model.cpp src/strategy_risk_verb.cpp
        src/strategy_spo.cpp src/strategy_spo_v3.cpp
        APPEND PROPERTY COMPILE_OPTIONS /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/strategy_ic_runner.cpp src/strategy_ic_composition.cpp src/strategy_target_replay.cpp src/strategy_price_exposures.cpp src/strategy_nav_replay.cpp
        src/strategy_ic_library.cpp src/strategy_ic_admission.cpp src/strategy_ic_signal_cache.cpp
        src/strategy_ic_result_cache.cpp src/strategy_ic_shrink.cpp
        src/strategy_ic_theme_resid.cpp
        src/strategy_cost_v2.cpp src/strategy_nav_v7.cpp src/strategy_risk_model.cpp src/strategy_risk_verb.cpp
        src/strategy_spo.cpp src/strategy_spo_v3.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
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

# Small supplied-DSL research entry point. Reuse the warm production libraries;
# this does not require the broad pipeline main or a second compiler environment.
add_executable(atx-equity-strategy EXCLUDE_FROM_ALL tools/equity_strategy.cpp)
target_link_libraries(atx-equity-strategy PRIVATE atx-impl-core atx_warnings)

# Fast supplied-DSL screening and streaming composition share the warm core PCH.
# Research options live in private sources, outside the pipeline config headers.
add_executable(atx-equity-strategy-ic EXCLUDE_FROM_ALL tools/equity_strategy_ic.cpp)
target_link_libraries(atx-equity-strategy-ic PRIVATE atx-impl-core atx_warnings)

# Reuse a saved composition for portfolio iteration without reevaluating DSLs.
add_executable(atx-equity-strategy-targets EXCLUDE_FROM_ALL tools/equity_strategy_targets.cpp)
target_link_libraries(atx-equity-strategy-targets PRIVATE atx-impl-core atx_warnings)

# atx-risk-v1 fundamental risk model + bias harness (platform v7 L4): the `risk` verb.
add_executable(atx-equity-strategy-risk EXCLUDE_FROM_ALL tools/equity_strategy_risk.cpp)
target_link_libraries(atx-equity-strategy-risk PRIVATE atx-impl-core atx_warnings)

# Platform v8 H-3: the mining verb (built and self-tested; real campaigns only under OD-7).
add_executable(atx-equity-strategy-mine EXCLUDE_FROM_ALL tools/equity_strategy_mine.cpp)
target_link_libraries(atx-equity-strategy-mine PRIVATE atx-impl-core atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
# Lane 9: real-data DSL alpha mining stage (equity-mine)
target_sources(atx-impl-core PRIVATE src/stage_equity_mine.cpp)
# platform v8 D-2: the `exposures` verb (contract K2) of atx-equity-strategy-targets
target_sources(atx-impl-core PRIVATE src/strategy_exposures_verb.cpp)
