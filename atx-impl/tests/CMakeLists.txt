file(GLOB ATX_IMPL_TEST_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp")
add_executable(atx-impl-tests ${ATX_IMPL_TEST_SOURCES})
# AtxImplSweep.SweepParallelEqualsSequentialDigest spawns atx-shm-worker.exe (process executor);
# ensure the worker is built into bin/ whenever the test target is built.
add_dependencies(atx-impl-tests atx-shm-worker)
target_link_libraries(atx-impl-tests
    PRIVATE atx-impl-core GTest::gtest_main atx_warnings)
find_package(Threads REQUIRED)
target_link_libraries(atx-impl-tests PRIVATE Threads::Threads)
# The load test builds a synthetic ORATS zip in-process via <miniz.h>.
target_link_libraries(atx-impl-tests PRIVATE atx_miniz)
# alpha101_orats_test reads fixtures/alpha101.txt at runtime; pass the source dir.
target_compile_definitions(atx-impl-tests PRIVATE
    ATX_IMPL_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
include(GoogleTest)
gtest_discover_tests(atx-impl-tests)
