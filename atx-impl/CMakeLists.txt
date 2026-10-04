# atx-impl: end-to-end equity alpha pipeline reference binary.
# Split: STATIC libs hold all logic so the test exes can link them (you cannot link
# an executable target). The atx-impl exe is just main.cpp over them.
#
# P9 C3 (F-10): two libraries. atx-impl-strategy is the research path: the strategy_* verbs
# (IC runner, marginal, mining, targets, NAV replay and its src/book/ files, risk model, spo)
# and the few shared files they read (artifacts, serialize_panel, panel_artifact,
# stage_data_provenance, the build provenance). The research exes (atx-equity-strategy*) link it
# alone. atx-impl-pipeline is the pipeline: the stage_* files, config and dispatch, which the
# research exes never call; it links atx-impl-strategy for those shared files. atx-impl-core is
# an INTERFACE library over both, so the atx-impl exe and every test exe keep linking one name.
# A new research source goes into atx-impl-strategy (target_sources on the INTERFACE
# atx-impl-core would compile nothing).
# P9 C3 (task 1, move-only): strategy_nav_replay.cpp split at the reviewed seams (validation,
# liquidity, MARK, EXECUTE, DECIDE, the session loop, summaries, writers, loaders, lockstep, run,
# publication, the command line, the decide-path seams); book/nav_replay_internal.hpp holds what
# they share. They keep strategy_nav_replay.cpp's Debug /O2 flag set (below).
set(_atx_nav_book_sources
    src/book/nav_validate.cpp
    src/book/nav_liquidity.cpp
    src/book/nav_mark.cpp
    src/book/nav_execute.cpp
    src/book/nav_decide.cpp
    src/book/nav_session.cpp
    src/book/nav_summarize.cpp
    src/book/nav_write.cpp
    src/book/nav_load.cpp
    src/book/nav_lockstep.cpp
    src/book/nav_run.cpp
    src/book/nav_publish.cpp
    src/book/nav_cli.cpp
    src/book/nav_deploy.cpp
)
add_library(atx-impl-strategy ${ATX_LIB_TYPE}
    src/artifacts.cpp
    src/serialize_panel.cpp
    src/panel_artifact.cpp
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
    src/strategy_ic_theme_erc.cpp  # platform v8 X (XCOMB): composition rule theme-erc-v1
    src/strategy_ic_theme_tsmom.cpp  # platform v8 Y (YCOMB): composition schedule theme-tsmom-v1
    src/strategy_ic_two_speed.cpp  # platform v8 Y-5 (YCOMB): composition sleeves two-speed-v1
    # platform v8 F-2: `atx-equity-strategy-ic marginal` (contract K6, marginal_ic.json)
    src/strategy_marginal_ic.cpp
    # platform v8 H-3: the shared research-role loader and the mining verb (mined-v1)
    src/strategy_research_role.cpp
    src/strategy_mine.cpp
    src/strategy_mine_ledger.cpp
    src/strategy_mine_pinned_file.cpp
    src/strategy_mine_pool.cpp
    src/strategy_mine_promote.cpp
    src/strategy_mine_rule.cpp
    src/strategy_mine_trials.cpp
    src/strategy_target_replay.cpp
    src/strategy_price_exposures.cpp
    ${_atx_nav_book_sources}
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
    # platform v8 Y (YCOMB): vol-target-v1, the risk target scaler's second law (its text)
    src/strategy_vol_target.cpp
    # P9 C3: the NAV rule registries (K-P9-7) and NavSpec (nav --spec)
    src/book/nav_rules.cpp
    src/book/nav_spec.cpp)

add_library(atx-impl-pipeline ${ATX_LIB_TYPE}
    src/config.cpp
    src/dispatch.cpp
    src/panel_pipeline.cpp
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

find_package(Threads REQUIRED)
foreach(_atx_impl_lib atx-impl-strategy atx-impl-pipeline)
    target_include_directories(${_atx_impl_lib} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_compile_features(${_atx_impl_lib} PUBLIC cxx_std_20)
    target_link_libraries(${_atx_impl_lib}
        PUBLIC  atx::core atx::engine atx::tsdb
        PRIVATE atx_warnings nlohmann_json::nlohmann_json)
    target_link_libraries(${_atx_impl_lib} PRIVATE Threads::Threads)
    # Stable third-party/std headers only; pipeline/config headers stay outside the
    # PCH so an ordinary engine change does not invalidate this parse cache. PRIVATE
    # keeps it out of downstream targets, and hygiene still disables ATX_USE_PCH.
    if(ATX_USE_PCH)
        target_precompile_headers(${_atx_impl_lib} PRIVATE pch.hpp)
    endif()
endforeach()
target_link_libraries(atx-impl-pipeline PUBLIC atx-impl-strategy)
add_library(atx-impl-core INTERFACE)
target_link_libraries(atx-impl-core INTERFACE atx-impl-pipeline atx-impl-strategy)

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
    set_property(SOURCE src/strategy_ic_runner.cpp src/strategy_ic_composition.cpp src/strategy_target_replay.cpp src/strategy_price_exposures.cpp ${_atx_nav_book_sources}
        src/strategy_ic_library.cpp src/strategy_ic_admission.cpp src/strategy_ic_signal_cache.cpp
        src/strategy_ic_result_cache.cpp src/strategy_ic_shrink.cpp
        src/strategy_ic_theme_resid.cpp src/strategy_ic_theme_erc.cpp src/strategy_ic_theme_tsmom.cpp
        src/strategy_ic_two_speed.cpp
        src/strategy_cost_v2.cpp src/strategy_nav_v7.cpp src/strategy_risk_model.cpp src/strategy_risk_verb.cpp
        src/strategy_spo.cpp src/strategy_spo_v3.cpp
        APPEND PROPERTY COMPILE_OPTIONS /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/strategy_ic_runner.cpp src/strategy_ic_composition.cpp src/strategy_target_replay.cpp src/strategy_price_exposures.cpp ${_atx_nav_book_sources}
        src/strategy_ic_library.cpp src/strategy_ic_admission.cpp src/strategy_ic_signal_cache.cpp
        src/strategy_ic_result_cache.cpp src/strategy_ic_shrink.cpp
        src/strategy_ic_theme_resid.cpp src/strategy_ic_theme_erc.cpp src/strategy_ic_theme_tsmom.cpp
        src/strategy_ic_two_speed.cpp
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
# P9 C3: in atx-impl-strategy (the NAV summary's producer identity reads it; stage_discover
# reaches it through atx-impl-pipeline's link).
set(_atx_provenance_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/build_provenance.cpp")
configure_file(src/build_provenance.cpp.in "${_atx_provenance_cpp}" @ONLY)
target_sources(atx-impl-strategy PRIVATE "${_atx_provenance_cpp}")
set_source_files_properties("${_atx_provenance_cpp}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

add_executable(atx-impl src/main.cpp)
target_link_libraries(atx-impl PRIVATE atx-impl-core atx_warnings)

# Small supplied-DSL research entry point. Reuse the warm production libraries;
# this does not require the broad pipeline main or a second compiler environment.
# P9 C3: the research exes link atx-impl-strategy alone (no pipeline stage).
add_executable(atx-equity-strategy EXCLUDE_FROM_ALL tools/equity_strategy.cpp)
target_link_libraries(atx-equity-strategy PRIVATE atx-impl-strategy atx_warnings)

# Fast supplied-DSL screening and streaming composition share the warm core PCH.
# Research options live in private sources, outside the pipeline config headers.
add_executable(atx-equity-strategy-ic EXCLUDE_FROM_ALL tools/equity_strategy_ic.cpp)
target_link_libraries(atx-equity-strategy-ic PRIVATE atx-impl-strategy atx_warnings)

# Reuse a saved composition for portfolio iteration without reevaluating DSLs.
add_executable(atx-equity-strategy-targets EXCLUDE_FROM_ALL tools/equity_strategy_targets.cpp)
target_link_libraries(atx-equity-strategy-targets PRIVATE atx-impl-strategy atx_warnings)

# atx-risk-v1 fundamental risk model + bias harness (platform v7 L4): the `risk` verb.
add_executable(atx-equity-strategy-risk EXCLUDE_FROM_ALL tools/equity_strategy_risk.cpp)
target_link_libraries(atx-equity-strategy-risk PRIVATE atx-impl-strategy atx_warnings)

# Platform v8 H-3: the mining verb (built and self-tested; real campaigns only under OD-7).
add_executable(atx-equity-strategy-mine EXCLUDE_FROM_ALL tools/equity_strategy_mine.cpp)
target_link_libraries(atx-equity-strategy-mine PRIVATE atx-impl-strategy atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
# Lane 9: real-data DSL alpha mining stage (equity-mine)
target_sources(atx-impl-pipeline PRIVATE src/stage_equity_mine.cpp)
# platform v8 D-2: the `exposures` verb (contract K2) of atx-equity-strategy-targets
target_sources(atx-impl-strategy PRIVATE src/strategy_exposures_verb.cpp)
# P9 S1: the marginal verb's pair cache (--pair-cache)
target_sources(atx-impl-strategy PRIVATE src/strategy_marginal_pair_cache.cpp)
# platform P9 B1: the `factors` verb (contract K-P9-4) of atx-equity-strategy-targets. Its per-candidate rank and
# projection loops run over every decision of a full role, so the warm Debug tree gives this TU the same private
# /O2 as the IC TUs above (Debug CRT, assertions and FP settings unchanged; no shared PCH under other flags).
target_sources(atx-impl-strategy PRIVATE src/strategy_factors_verb.cpp)
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(SOURCE src/strategy_factors_verb.cpp APPEND PROPERTY COMPILE_OPTIONS /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/strategy_factors_verb.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
# P9 lane D1: the IC runner's composition rule table and theme table (strategy_ic_rules.hpp,
# contract K-P9-6), with the IC TUs' Debug /O2 flag set (the list above) for speed. (P9 C3, W1-MIN
# M-5: the inline variables of strategy_ic_detail.hpp depend on ISA macros only, not on /O2, so
# any TU may include that header; the flag set is a speed choice, not a one-definition rule.)
target_sources(atx-impl-strategy PRIVATE src/strategy_ic_rules.cpp)
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(SOURCE src/strategy_ic_rules.cpp APPEND PROPERTY COMPILE_OPTIONS /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/strategy_ic_rules.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
# P9 lane AL-COMB: composition rules theme-cov-hedge-v1 (over engine combine/char_cov_hedge.hpp) and
# mom-volman-v1 (a theme_schedule rule), with the IC TUs' flag set as strategy_ic_rules.cpp above.
target_sources(atx-impl-strategy PRIVATE src/strategy_ic_theme_hedge.cpp src/strategy_ic_mom_volman.cpp)
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_property(SOURCE src/strategy_ic_theme_hedge.cpp src/strategy_ic_mom_volman.cpp
        APPEND PROPERTY COMPILE_OPTIONS /O2 /Ob2 /clang:-finline)
    set_source_files_properties(src/strategy_ic_theme_hedge.cpp src/strategy_ic_mom_volman.cpp
        PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
endif()
# P9 lane D2: the composition rules' kernels moved down to atx-engine-research-composition (migration plan
# section 1); strategy_ic_{shrink,theme_erc,theme_tsmom,theme_resid}.hpp and strategy_two_speed.hpp are thin
# includes of it, and the IC runner's verify and composition call it.
target_link_libraries(atx-impl-strategy PUBLIC atx-engine-research-composition)
# P9 lane C2: nav --calibrate-gross, Ruling PM6-6 in one process (strategy_nav_calibrate.hpp:
# the numpy mean of the S2 gross, the 4-decimal correction, the summary record).
target_sources(atx-impl-strategy PRIVATE src/strategy_nav_calibrate.cpp)
# P9 lane B2 (cross-lane append): the IC stage's trial ledger moved down to atx-engine-research-ledger;
# src/trial_ledger.{hpp,cpp} re-export it, so atx-impl-core links the library. (P9 C3 at the merge:
# trial_ledger.cpp and its users, stage_equity_ic / stage_equity_universe, are atx-impl-pipeline's.)
target_link_libraries(atx-impl-pipeline PUBLIC atx::engine-research-ledger)
