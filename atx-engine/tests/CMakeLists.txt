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

include(GoogleTest)
gtest_discover_tests(atx-engine-tests)
