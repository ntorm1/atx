# Focused listed-options research, risk, and execution throughput benchmarks.
# Built only when the repository-wide ATX_BUILD_BENCH option is enabled.
add_executable(atx-options-engine-bench
    bench_main.cpp
    option_adaptive_coordinator_bench.cpp
    option_execution_session_bench.cpp
    option_execution_replay_bench.cpp
    option_pretrade_risk_bench.cpp)

target_link_libraries(atx-options-engine-bench
    PRIVATE
        atx::options-engine
        benchmark::benchmark
        atx_warnings)
