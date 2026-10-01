# atx-engine test suite — split into one executable per subsystem GROUP so a
# worktree can compile/link only the slice it touched instead of the whole suite.
#
#   master / full suite : cmake --preset ninja            (ATX_TEST_GROUPS=all)
#   worktree / partial   : cmake --preset ninja -DATX_TEST_GROUPS=risk;data
#   build one group      : ninja atx-engine-risk-tests
#
# Each tests/<grp>/ subdir globs to atx-engine-<grp>-tests (CONFIGURE_DEPENDS, so
# adding a *_test.cpp re-globs automatically). Touching one group's test relinks
# only that group's objects — not all ~187 — and groups build in parallel. On a
# merge to master you rebuild only the groups whose files actually changed.

# Canonical group set. A subdir must exist per name; unknown names in
# ATX_TEST_GROUPS are a hard error (catches typos in worktree selection).
set(ATX_ALL_TEST_GROUPS
    alpha risk data factory parallel learn eval library combine fund book core regime store quant)

set(ATX_TEST_GROUPS "all" CACHE STRING
    "Test groups to build: 'all', or a ;-list of: ${ATX_ALL_TEST_GROUPS}")

if(ATX_TEST_GROUPS STREQUAL "all")
    set(_atx_groups ${ATX_ALL_TEST_GROUPS})
else()
    set(_atx_groups ${ATX_TEST_GROUPS})
endif()

# Threads for the event bus's multi-threaded tests (std::thread).
find_package(Threads REQUIRED)
include(GoogleTest)

# The data group's databento_pipeline_e2e_test reuses atx-core's dbn_fixture.hpp
# (it #includes <miniz.h>/<zstd.h> and calls mz_*/ZSTD_* directly to build DBN/zip
# fixtures in-process), so the data target needs that include dir on its link line.
# zstd::libzstd is an imported target scoped to atx-core's directory (where its
# find_package lives); re-find it here so a data-group target can link it. atx_miniz
# is a global add_library target, already visible. (Arrow/Parquet/zstd runtime DLLs
# are auto-staged next to each test exe by the vcpkg toolchain's applocal deploy.)
find_package(zstd CONFIG REQUIRED)
set(_atx_core_tests_inc ${CMAKE_CURRENT_SOURCE_DIR}/../../atx-core/tests)

option(ATX_RISK_SECTOR_ASAN "Build the scoped W0-R04 native AddressSanitizer gate" OFF)
if(ATX_RISK_SECTOR_ASAN)
    if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "ATX_RISK_SECTOR_ASAN requires native Windows x64 clang-cl")
    endif()
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR ATX_SHARED_LIBS OR ATX_USE_PCH
       OR NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDLL")
        message(FATAL_ERROR "Use equity-asan: Release CRT, static libraries, PCH disabled")
    endif()
    execute_process(COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
        RESULT_VARIABLE _asan_resource_status OUTPUT_VARIABLE _asan_resource
        ERROR_VARIABLE _asan_resource_error OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _asan_resource_status EQUAL 0 OR _asan_resource STREQUAL "")
        message(FATAL_ERROR "Cannot detect clang ASan runtime: ${_asan_resource_error}")
    endif()
    file(TO_CMAKE_PATH "${_asan_resource}" _asan_resource)
    set(ATX_RISK_SECTOR_ASAN_RUNTIME "msvc" CACHE STRING "ASan runtime provider: msvc or clang")
    if(ATX_RISK_SECTOR_ASAN_RUNTIME STREQUAL "msvc")
        # Use one coherent runtime/import/thunk set from the active MSVC CRT.
        # The older bundled clang runtime cannot initialize on every Windows CRT.
        if(NOT DEFINED ENV{VCToolsInstallDir})
            message(FATAL_ERROR "MSVC ASan runtime detection needs vcvars64; use atx-build.ps1")
        endif()
        file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" _asan_vctools)
        set(_asan_dir "${_asan_vctools}/lib/x64")
        set(_asan_dll "${_asan_vctools}/bin/Hostx64/x64/clang_rt.asan_dynamic-x86_64.dll")
    elseif(ATX_RISK_SECTOR_ASAN_RUNTIME STREQUAL "clang")
        set(_asan_dir "${_asan_resource}/lib/windows")
        set(_asan_dll "${_asan_dir}/clang_rt.asan_dynamic-x86_64.dll")
    else()
        message(FATAL_ERROR "ATX_RISK_SECTOR_ASAN_RUNTIME must be msvc or clang")
    endif()
    set(_asan_import "${_asan_dir}/clang_rt.asan_dynamic-x86_64.lib")
    set(_asan_thunk "${_asan_dir}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")
    foreach(_asan_file IN ITEMS "${_asan_import}" "${_asan_thunk}" "${_asan_dll}")
        if(NOT EXISTS "${_asan_file}")
            message(FATAL_ERROR "clang ASan runtime component missing: ${_asan_file}")
        endif()
    endforeach()
    get_filename_component(_asan_bin "${_asan_resource}/../../../bin" ABSOLUTE)
    find_program(ATX_RISK_SECTOR_ASAN_SYMBOLIZER llvm-symbolizer HINTS "${_asan_bin}" REQUIRED)
    set(ATX_RISK_SECTOR_ASAN_RUNTIME_DIR "${_asan_dir}" CACHE INTERNAL "Detected ASan runtime libs")
    message(STATUS "W0-R04 ASan: ${_asan_dll}; symbolizer=${ATX_RISK_SECTOR_ASAN_SYMBOLIZER}")
    message(STATUS "W0-R04 ASan scope: production factor_model.cpp and fixture; deps uninstrumented")
    message(STATUS "W0-R04 ASan: STL logical-size annotations off for prebuilt dependency ABI")

    # Compile the real production TU directly: do not pull in or rebuild every
    # unrelated engine source merely to qualify this particular bounds defect.
    add_executable(atx-engine-risk-sector-asan
        ../src/risk/factor_model.cpp risk/risk_w0r0_sector_columns_test.cpp)
    target_include_directories(atx-engine-risk-sector-asan PRIVATE ../include)
    target_compile_features(atx-engine-risk-sector-asan PRIVATE cxx_std_20)
    target_compile_options(atx-engine-risk-sector-asan PRIVATE /fsanitize=address /Oy- /Z7)
    # The prebuilt MSVC-STL dependencies have annotate_string/vector=0. Match
    # their ABI without suppressing ASan heap-access instrumentation: the exact
    # Eigen beta[2] positive control must still detect the allocation overflow.
    # This gate does not qualify logical size-vs-capacity STL annotations.
    target_compile_definitions(atx-engine-risk-sector-asan PRIVATE
        ATX_RISK_SECTOR_ASAN=1 _DISABLE_STRING_ANNOTATION _DISABLE_VECTOR_ANNOTATION)
    target_link_libraries(atx-engine-risk-sector-asan PRIVATE
        atx::core GTest::gtest_main Threads::Threads atx_warnings atx-test-scratch "${_asan_import}")
    # CMake invokes lld-link directly, so reproduce the runtime options selected
    # by clang-cl's sanitizer driver rather than relying on an implicit link.
    target_link_options(atx-engine-risk-sector-asan PRIVATE
        "/WHOLEARCHIVE:${_asan_thunk}" /INCLUDE:__asan_seh_interceptor /INCREMENTAL:NO /DEBUG)
    add_custom_command(TARGET atx-engine-risk-sector-asan POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_asan_dll}" "$<TARGET_FILE_DIR:atx-engine-risk-sector-asan>" VERBATIM)
    gtest_discover_tests(atx-engine-risk-sector-asan TEST_PREFIX "RiskSectorAsan."
        DISCOVERY_TIMEOUT 60 PROPERTIES LABELS "risk_sector_asan")
endif()

# Minimal, stable PCH carriers avoid making one test executable a dependency of
# every other group. Changing -Groups order must not change the PCH path.
# Data needs its own compatible carrier: miniz adds a PUBLIC include directory,
# contrary to the previous comment claiming that all group flags were identical.
function(atx_engine_test_compile_environment target)
    target_link_libraries(${target} PRIVATE
        atx::engine GTest::gtest_main atx_warnings Threads::Threads)
    target_include_directories(${target} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${_atx_core_tests_inc})
endfunction()

foreach(grp IN LISTS _atx_groups)
    if(NOT grp IN_LIST ATX_ALL_TEST_GROUPS)
        message(FATAL_ERROR
            "ATX_TEST_GROUPS: unknown group '${grp}'. Valid: ${ATX_ALL_TEST_GROUPS}")
    endif()

    file(GLOB _grp_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${grp}/*_test.cpp")
    # Older cone/QP suites predate subsystem directories. Keep them in the risk
    # target so they cannot silently disappear from build and test discovery.
    if(grp STREQUAL "risk")
        file(GLOB _legacy_risk_sources CONFIGURE_DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/risk_*_test.cpp")
        list(APPEND _grp_sources ${_legacy_risk_sources})
    endif()
    if(NOT _grp_sources)
        message(WARNING "test group '${grp}' has no *_test.cpp sources; skipping")
        continue()
    endif()

    set(_tgt atx-engine-${grp}-tests)
    add_executable(${_tgt} ${_grp_sources})

    atx_engine_test_compile_environment(${_tgt})
    # FIX-3/F3-B: per-process scratch isolation belongs in executable consumers,
    # never in a PCH carrier. Keep the OBJECT library on each executable link line.
    target_link_libraries(${_tgt} PRIVATE atx-test-scratch)

    # tests root: "fixtures/..." is referenced relative to it (e.g. the alpha
    # kalman reference), so it must resolve from any group subdir.
    # atx-core/tests: dbn_fixture.hpp for the data group's databento e2e test.
    # The compile-environment helper gives each carrier these same include paths.

    if(ATX_USE_PCH)
        if(grp STREQUAL "data")
            set(_atx_pch_owner atx-engine-data-test-pch)
        else()
            set(_atx_pch_owner atx-engine-test-pch)
        endif()
        # Create only carriers with configured consumers, and only when PCH is
        # enabled. A tracked empty TU avoids configure-time timestamp churn.
        if(NOT TARGET ${_atx_pch_owner})
            add_library(${_atx_pch_owner} OBJECT EXCLUDE_FROM_ALL pch.cpp)
            atx_engine_test_compile_environment(${_atx_pch_owner})
            if(grp STREQUAL "data")
                target_link_libraries(${_atx_pch_owner} PRIVATE atx_miniz zstd::libzstd)
            endif()
            target_precompile_headers(${_atx_pch_owner} PRIVATE pch.hpp)
        endif()
        target_precompile_headers(${_tgt} REUSE_FROM ${_atx_pch_owner})
    endif()

    # The `parallel` group is opted OUT of Unity batching entirely: its test files
    # pervasively define identically-named file-local fixture helpers (kSlotSize,
    # make_input, digest_slots, sequential_digest, run, frictionless_sim, Lcg,
    # compile_named, shared_lib, ...) inside unnamed namespaces, which per
    # [namespace.unnamed] merge into ONE namespace when Unity concatenates several
    # files into a single TU -> ODR redefinition / ambiguous-call errors across many
    # file pairs (not just a fixed few, as in the factory group). Rather than chase
    # every colliding pair with SKIP_UNITY_BUILD_INCLUSION, this group simply compiles
    # file-by-file (as under -DATX_UNITY_BUILD=OFF); Unity here is only a build-speed
    # optimization, never a correctness requirement.
    if(ATX_UNITY_BUILD AND NOT grp STREQUAL "parallel")
        set_target_properties(${_tgt} PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE 16)

        # factory group only: factory_behavior_test.cpp / factory_cost_aware_fitness_test.cpp /
        # factory_nsga_search_test.cpp each wrap file-local fixture helpers (frictionless_sim,
        # Lcg, make_panel, noisy_close, fixture_panel, seed_exprs, ...) in `namespace { ... }`.
        # That gives each *file* internal linkage under normal (one-TU-per-file) compilation,
        # but per [namespace.unnamed], every unnamed-namespace body within a single translation
        # unit is a reopening of the SAME compiler-synthesized namespace — so when Unity
        # batching concatenates these 3 files into one TU, their identically-named helpers
        # become duplicate definitions in that one merged namespace (ODR redefinition /
        # ambiguous-call errors). Exclude just these 3 from unity batching so each keeps
        # compiling as its own real translation unit, exactly as it already does under
        # -DATX_UNITY_BUILD=OFF; every other file in the group still gets batched.
        if(grp STREQUAL "factory")
            set_source_files_properties(
                ${CMAKE_CURRENT_SOURCE_DIR}/factory/factory_behavior_test.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/factory/factory_cost_aware_fitness_test.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/factory/factory_nsga_search_test.cpp
                PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        endif()
        # book group only: the same unnamed-namespace collision. book_claims_state_test
        # and security_transition_test redefine digest()/evidence() from
        # book_claims_replay_test; book_replay_test redefines its kDay/kNaN/days()/
        # prices(). Verified: the remaining 10 book files (including the named-namespace
        # lane-8 files) compile as one concatenated TU.
        if(grp STREQUAL "book")
            set_source_files_properties(
                ${CMAKE_CURRENT_SOURCE_DIR}/book/book_claims_state_test.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/book/security_transition_test.cpp
                ${CMAKE_CURRENT_SOURCE_DIR}/book/book_replay_test.cpp
                PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)
        endif()
    endif()

    # data group only: the miniz/zstd link line for the databento e2e fixture.
    # atx_miniz links unconditionally (shared build too): WINDOWS_EXPORT_ALL_SYMBOLS
    # never exports symbols a DLL absorbed from a PRIVATE static lib, so atx-core.dll
    # does NOT re-export mz_* — each consumer carries its own copy. See atx-core/tests.
    if(grp STREQUAL "data")
        target_link_libraries(${_tgt} PRIVATE atx_miniz zstd::libzstd)
    endif()

    # PRE_TEST: enumerate at ctest time, not as a POST_BUILD step -- relinking no
    # longer pays a per-group test-enumeration run at build time.
    gtest_discover_tests(${_tgt} DISCOVERY_MODE PRE_TEST)
endforeach()

# Explicit, bounded IC-screen qualification. These sources remain in their
# normal subsystem targets; this additional target avoids rebuilding unrelated
# test translation units after a shared search/registry header changes.
# Run it directly with a gtest filter. Do not duplicate CTest registrations.
add_executable(atx-engine-ic-screen-tests EXCLUDE_FROM_ALL
    factory/ic_screen_test.cpp
    factory/factory_objective_ic_test.cpp
    factory/execution_objective_test.cpp
    factory/execution_fitness_test.cpp
    factory/factory_fidelity_test.cpp
    factory/search_progress_test.cpp
    eval/eval_trial_epoch_test.cpp
    eval/eval_trial_registry_test.cpp)
atx_engine_test_compile_environment(atx-engine-ic-screen-tests)
target_link_libraries(atx-engine-ic-screen-tests PRIVATE atx-test-scratch)
if(ATX_USE_PCH)
    if(NOT TARGET atx-engine-test-pch)
        add_library(atx-engine-test-pch OBJECT EXCLUDE_FROM_ALL pch.cpp)
        atx_engine_test_compile_environment(atx-engine-test-pch)
        target_precompile_headers(atx-engine-test-pch PRIVATE pch.hpp)
    endif()
    target_precompile_headers(atx-engine-ic-screen-tests REUSE_FROM atx-engine-test-pch)
endif()

# Focused equity review regressions link the production library without building
# every owning test group. Keep ordinary group registration unchanged.
add_executable(atx-engine-equity-review-tests EXCLUDE_FROM_ALL
    risk/risk_constraint_dispatch_test.cpp
    core/execution_sim_test.cpp
    core/backtest_accounting_test.cpp
    core/backtest_integration_test.cpp
    book/book_borrow_schedule_test.cpp
    book/book_w0b0_borrow_single_count_test.cpp
    book/book_claims_replay_test.cpp
    book/book_event_batch_test.cpp
    book/book_replay_cost_test.cpp
    factory/factory_fitness_test.cpp
    factory/factory_cpcv_cache_test.cpp
    factory/deflate_selection_running_n_test.cpp
    factory/search_progress_test.cpp
    factory/factory_oos_test.cpp
    combine/combine_walk_forward_test.cpp
    data/data_align_test.cpp
    data/data_adapt_feature_test.cpp)
atx_engine_test_compile_environment(atx-engine-equity-review-tests)
target_link_libraries(atx-engine-equity-review-tests PRIVATE atx-test-scratch)
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine-equity-review-tests REUSE_FROM atx-engine-test-pch)
endif()

# Bounded W1 qualification on production libraries, including existing oracle,
# streaming and walk-forward checks. Ordinary owning suites remain registered.
add_executable(atx-engine-w1-foundation-tests EXCLUDE_FROM_ALL
    alpha/alpha_ts_date_major_test.cpp
    alpha/alpha_ts_sliding_routing_test.cpp
    alpha/alpha_conformance_test.cpp
    alpha/alpha_widened_conformance_test.cpp
    alpha/alpha_oracle_test.cpp
    alpha/streaming_engine_test.cpp
    combine/combine_signal_cube_test.cpp
    combine/combine_ic_cache_test.cpp
    combine/combine_signal_combiner_test.cpp
    combine/combine_walk_forward_test.cpp)
set_property(SOURCE combine/combine_signal_cube_test.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
    "$<TARGET_PROPERTY:nlohmann_json::nlohmann_json,INTERFACE_INCLUDE_DIRECTORIES>")
atx_engine_test_compile_environment(atx-engine-w1-foundation-tests)
target_link_libraries(atx-engine-w1-foundation-tests PRIVATE atx-test-scratch)
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine-w1-foundation-tests REUSE_FROM atx-engine-test-pch)
endif()

# Shared cost-surface adapters; further B1 fixtures join this bounded target as
# their implementations are integrated. Normal owning core tests remain intact.
add_executable(atx-engine-w1-cost-tests EXCLUDE_FROM_ALL
    core/cost_modeled_inputs_test.cpp
    core/cost_surface_test.cpp
    core/calibration_refit_test.cpp
    core/calibration_test.cpp
    core/cost_integration_test.cpp
    book/book_replay_cost_test.cpp
    risk/risk_optimizer_cost_terms_test.cpp)
atx_engine_test_compile_environment(atx-engine-w1-cost-tests)
target_link_libraries(atx-engine-w1-cost-tests PRIVATE atx-test-scratch)
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine-w1-cost-tests REUSE_FROM atx-engine-test-pch)
endif()

add_executable(atx-engine-w1-eval-tests EXCLUDE_FROM_ALL
    eval/eval_cpcv_test.cpp
    eval/eval_cpcv_date_test.cpp
    eval/eval_pbo_test.cpp
    eval/eval_breadth_test.cpp
    eval/eval_regime_slice_test.cpp
    factory/factory_cpcv_cache_test.cpp
    factory/factory_research_driver_test.cpp
    factory/blocking_pbo_test.cpp
    learn/learn_panel_dataset_test.cpp
    learn/gbt_v2_test.cpp
    learn/learn_cpcv_date_test.cpp
    learn/learn_nn_gate_test.cpp
    combine/combine_combined_source_test.cpp)
atx_engine_test_compile_environment(atx-engine-w1-eval-tests)
target_link_libraries(atx-engine-w1-eval-tests PRIVATE atx-test-scratch)
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine-w1-eval-tests REUSE_FROM atx-engine-test-pch)
endif()

# Include legacy record/store contracts alongside the new compact format. This
# target stays explicit so storage work need not compile unrelated test groups.
add_executable(atx-engine-w1-library-tests EXCLUDE_FROM_ALL
    library/library_corr_index_test.cpp
    library/library_lifecycle_test.cpp
    library/library_compressed_test.cpp
    library/library_record_test.cpp
    library/library_store_test.cpp)
atx_engine_test_compile_environment(atx-engine-w1-library-tests)
target_link_libraries(atx-engine-w1-library-tests PRIVATE atx-test-scratch)
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine-w1-library-tests REUSE_FROM atx-engine-test-pch)
endif()

add_executable(atx-engine-w1-risk-tests EXCLUDE_FROM_ALL
    risk/risk_estimator_v2_test.cpp
    risk/risk_covariance_integration_test.cpp
    risk/risk_factor_model_test.cpp
    risk/risk_exposures_test.cpp
    risk/risk_w0r0_pit_test.cpp
    risk/risk_hybrid_model_test.cpp
    risk/risk_constraints_test.cpp
    risk/risk_qp_solver_test.cpp
    risk/risk_qp_factor_admm_test.cpp
    risk/risk_cost_terms_test.cpp
    risk/risk_cost_factor_v2_test.cpp
    risk/risk_discretize_test.cpp)
atx_engine_test_compile_environment(atx-engine-w1-risk-tests)
target_link_libraries(atx-engine-w1-risk-tests PRIVATE atx-test-scratch)
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine-w1-risk-tests REUSE_FROM atx-engine-test-pch)
endif()

add_executable(atx-engine-w1-data-tests EXCLUDE_FROM_ALL
    data/exposure_panel_test.cpp
    data/price_exposure_provider_test.cpp
    data/fundamental_clock_artifact_test.cpp
    data/data_panel_store_test.cpp
    data/data_history_panel_test.cpp
    data/data_point_in_time_universe_test.cpp
    data/data_security_link_intervals_test.cpp
    data/fundamental_fields_test.cpp)
atx_engine_test_compile_environment(atx-engine-w1-data-tests)
target_link_libraries(atx-engine-w1-data-tests PRIVATE atx-test-scratch)
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine-w1-data-tests REUSE_FROM atx-engine-test-pch)
endif()
