add_executable(atx-core-tests
    core_test.cpp
    error_test.cpp
    types_test.cpp
    log_test.cpp
    platform_test.cpp
    bit_test.cpp
    util_test.cpp
    safe_math_test.cpp
    math_test.cpp
    hash_test.cpp
    decimal_test.cpp
    random_test.cpp
    aligned_test.cpp
    arena_test.cpp
    object_pool_test.cpp
    ring_buffer_test.cpp
)

target_link_libraries(atx-core-tests
    PRIVATE
        atx::core
        GTest::gtest_main
        atx_warnings
)

include(GoogleTest)
gtest_discover_tests(atx-core-tests)
