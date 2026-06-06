# atx-engine test suite. Per-unit *_test.cpp sources are auto-discovered, so each
# Phase-1 unit only drops a new test file; CONFIGURE_DEPENDS re-globs when the set
# of sources changes. Mirrors the atx-core test wiring (warnings gate + Threads).
file(GLOB ATX_ENGINE_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

add_executable(atx-engine-tests ${ATX_ENGINE_TEST_SOURCES})

target_link_libraries(atx-engine-tests
    PRIVATE
        atx::engine
        GTest::gtest_main
        atx_warnings
)

# Threads for the event bus's multi-threaded tests (std::thread).
find_package(Threads REQUIRED)
target_link_libraries(atx-engine-tests PRIVATE Threads::Threads)

# databento_pipeline_e2e_test threads the full loader -> bridge -> feed pipeline.
# It reuses atx-core's in-process DBN/zstd/zip fixture builders (dbn_fixture.hpp),
# which #include <miniz.h>/<zstd.h> and call mz_*/ZSTD_* directly — so this target
# needs that include dir and those two libs on its link line. (load_equs_summary_zip
# itself lives in atx::core, reachable transitively; Arrow/Parquet runtime DLLs are
# already staged in the shared bin/ dir by the atx-core test target's POST_BUILD copy.)
# zstd::libzstd is an imported target scoped to atx-core's directory (where its
# find_package lives); re-find it here so this target can link it. atx_miniz is a
# global (add_library) target, already visible.
find_package(zstd CONFIG REQUIRED)
target_include_directories(atx-engine-tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../atx-core/tests)
target_link_libraries(atx-engine-tests PRIVATE atx_miniz zstd::libzstd)

include(GoogleTest)
gtest_discover_tests(atx-engine-tests)
