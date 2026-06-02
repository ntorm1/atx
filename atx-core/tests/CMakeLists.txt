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
    disruptor_test.cpp
    fixed_vector_test.cpp
    small_vector_test.cpp
    hash_map_test.cpp
    intrusive_list_test.cpp
)

target_link_libraries(atx-core-tests
    PRIVATE
        atx::core
        GTest::gtest_main
        atx_warnings
)

# Threads for the disruptor's multi-threaded tests (std::thread).
find_package(Threads REQUIRED)
target_link_libraries(atx-core-tests PRIVATE Threads::Threads)

include(GoogleTest)
gtest_discover_tests(atx-core-tests)
