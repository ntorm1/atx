add_executable(atx-core-tests
    core_test.cpp
    error_test.cpp
    types_test.cpp
    log_test.cpp
)

target_link_libraries(atx-core-tests
    PRIVATE
        atx::core
        GTest::gtest_main
        atx_warnings
)

include(GoogleTest)
gtest_discover_tests(atx-core-tests)
