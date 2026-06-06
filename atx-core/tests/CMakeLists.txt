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
    spinlock_test.cpp
    seqlock_test.cpp
    spsc_queue_test.cpp
    mpmc_queue_test.cpp
    datetime_test.cpp
    simd_test.cpp
    online_stats_test.cpp
    quantile_test.cpp
    algo_test.cpp
    rolling_test.cpp
    cross_section_test.cpp
    linalg_test.cpp
    decompose_test.cpp
    solve_test.cpp
    spd_test.cpp
    pca_test.cpp
    regression_test.cpp
    domain_test.cpp
    column_test.cpp
    frame_test.cpp
    parquet_test.cpp
    db_sqlite_test.cpp
    dbn_test.cpp
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

# Arrow/Parquet for the parquet loader tests.
target_link_libraries(atx-core-tests PRIVATE Arrow::arrow_shared Parquet::parquet_shared)

# miniz + zstd: the Databento tests build .dbn.zst fixtures in-process. Linked to
# the (first-party) test target directly; their include dirs propagate here only,
# never to atx-core's public consumers. zstd::libzstd is the same vcpkg zstd Arrow
# links, so its DLL is already handled by the TARGET_RUNTIME_DLLS copy below.
target_link_libraries(atx-core-tests PRIVATE atx_miniz zstd::libzstd)

# Copy Arrow's runtime DLLs (and any other transitive shared deps) next to the
# test exe so it can be launched directly by ctest.
add_custom_command(TARGET atx-core-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:atx-core-tests> $<TARGET_FILE_DIR:atx-core-tests>
    COMMAND_EXPAND_LISTS)

include(GoogleTest)
gtest_discover_tests(atx-core-tests)
