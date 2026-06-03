# atx-engine micro-benchmark suite. Per-unit *_bench.cpp sources are appended to
# this executable as their units land (auto-discovered). Built only when
# ATX_BUILD_BENCH=ON. Mirrors atx-core/bench.
file(GLOB ATX_ENGINE_BENCH_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*_bench.cpp")

add_executable(atx-engine-bench
    bench_main.cpp
    ${ATX_ENGINE_BENCH_SOURCES}
)

target_link_libraries(atx-engine-bench
    PRIVATE
        atx::engine
        benchmark::benchmark
        atx_warnings
)

# Threads for the event bus throughput benchmarks (std::thread).
find_package(Threads REQUIRED)
target_link_libraries(atx-engine-bench PRIVATE Threads::Threads)
