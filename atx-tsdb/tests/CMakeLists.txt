file(GLOB ATX_TSDB_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

add_executable(atx-tsdb-tests ${ATX_TSDB_TEST_SOURCES})

target_link_libraries(atx-tsdb-tests
    PRIVATE
        atx::tsdb
        GTest::gtest_main
        atx_warnings
        # FIX-3/F3-B: per-process temp root. load_parquet_test.cpp's axis1.seg /
        # atx_dated_root / atx_dated_segs are fixed names under the machine-wide temp
        # dir that remove_all on entry, so two concurrent runs of this binary delete
        # each other's fixtures mid-test. See //tests/CMakeLists.txt.
        atx-test-scratch
)

find_package(Threads REQUIRED)
target_link_libraries(atx-tsdb-tests PRIVATE Threads::Threads)

include(GoogleTest)
# PRE_TEST: enumerate at ctest time, not as a POST_BUILD step -- relinking no longer
# pays a test-enumeration run (and a non-launchable exe no longer fails the *build*).
gtest_discover_tests(atx-tsdb-tests DISCOVERY_MODE PRE_TEST)
