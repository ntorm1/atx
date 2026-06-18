file(GLOB ATX_IMPL_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(atx-impl-tests ${ATX_IMPL_TEST_SOURCES})
target_link_libraries(atx-impl-tests
    PRIVATE atx-impl-core GTest::gtest_main atx_warnings)
find_package(Threads REQUIRED)
target_link_libraries(atx-impl-tests PRIVATE Threads::Threads)
# The load test builds a synthetic ORATS zip in-process via <miniz.h>.
target_link_libraries(atx-impl-tests PRIVATE atx_miniz)
include(GoogleTest)
gtest_discover_tests(atx-impl-tests)
