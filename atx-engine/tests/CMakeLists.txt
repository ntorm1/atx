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
    alpha risk data factory parallel learn eval library combine fund book core regime store)

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

# First configured group owns the shared PCH; the rest REUSE_FROM it so gtest+Eigen
# are parsed exactly once across the whole suite. Compile flags are kept identical
# across every group target (same include dirs) so the PCH is reuse-compatible — the
# data group's extra dependency is link-only (miniz/zstd), which does not affect PCH.
set(_atx_pch_owner "")

foreach(grp IN LISTS _atx_groups)
    if(NOT grp IN_LIST ATX_ALL_TEST_GROUPS)
        message(FATAL_ERROR
            "ATX_TEST_GROUPS: unknown group '${grp}'. Valid: ${ATX_ALL_TEST_GROUPS}")
    endif()

    file(GLOB _grp_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/${grp}/*_test.cpp")
    if(NOT _grp_sources)
        message(WARNING "test group '${grp}' has no *_test.cpp sources; skipping")
        continue()
    endif()

    set(_tgt atx-engine-${grp}-tests)
    add_executable(${_tgt} ${_grp_sources})

    target_link_libraries(${_tgt}
        PRIVATE
            atx::engine
            GTest::gtest_main
            atx_warnings
            Threads::Threads)

    # tests root: "fixtures/..." is referenced relative to it (e.g. the alpha
    # kalman reference), so it must resolve from any group subdir.
    # atx-core/tests: dbn_fixture.hpp for the data group's databento e2e test.
    # Added to every group target (uniform flags) to keep the shared PCH reusable.
    target_include_directories(${_tgt} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${_atx_core_tests_inc})

    if(ATX_USE_PCH)
        if(_atx_pch_owner STREQUAL "")
            target_precompile_headers(${_tgt} PRIVATE pch.hpp)
            set(_atx_pch_owner ${_tgt})
        else()
            target_precompile_headers(${_tgt} REUSE_FROM ${_atx_pch_owner})
        endif()
    endif()

    if(ATX_UNITY_BUILD)
        set_target_properties(${_tgt} PROPERTIES
            UNITY_BUILD ON
            UNITY_BUILD_BATCH_SIZE 16)
    endif()

    # data group only: the miniz/zstd link line for the databento e2e fixture.
    if(grp STREQUAL "data")
        target_link_libraries(${_tgt} PRIVATE atx_miniz zstd::libzstd)
    endif()

    gtest_discover_tests(${_tgt})
endforeach()
