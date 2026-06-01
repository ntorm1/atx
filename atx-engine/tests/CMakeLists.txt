add_executable(atx-engine-tests
    engine_test.cpp
)

target_link_libraries(atx-engine-tests
    PRIVATE
        atx::engine
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(atx-engine-tests)
