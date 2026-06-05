file(GLOB ATX_TSDB_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")

add_executable(atx-tsdb-tests ${ATX_TSDB_TEST_SOURCES})

target_link_libraries(atx-tsdb-tests
    PRIVATE
        atx::tsdb
        GTest::gtest_main
        atx_warnings
)

find_package(Threads REQUIRED)
target_link_libraries(atx-tsdb-tests PRIVATE Threads::Threads)

include(GoogleTest)
gtest_discover_tests(atx-tsdb-tests)
