# atx-core micro-benchmark suite. Per-module *_bench.cpp sources are appended to
# this executable as their modules land. Built only when ATX_BUILD_BENCH=ON.
add_executable(atx-core-bench
    bench_main.cpp
    ring_buffer_bench.cpp
    disruptor_bench.cpp
    spsc_queue_bench.cpp
    mpmc_queue_bench.cpp
    simd_bench.cpp
)

target_link_libraries(atx-core-bench
    PRIVATE
        atx::core
        benchmark::benchmark
        atx_warnings
)

# Threads for the disruptor throughput benchmarks (std::thread).
find_package(Threads REQUIRED)
target_link_libraries(atx-core-bench PRIVATE Threads::Threads)
