add_library(atx-engine STATIC
    src/engine.cpp
    src/factory/canonical.cpp
    src/factory/crossover.cpp
    src/factory/factory.cpp
    src/factory/fitness.cpp
    src/factory/genome.cpp
    src/factory/mutation.cpp
    src/factory/op_catalog.cpp
    src/factory/param_search.cpp
    src/factory/pool_view.cpp
    src/factory/research_driver.cpp
    src/factory/search_driver.cpp
    src/eval/cpcv.cpp
    src/eval/pbo.cpp
    src/eval/perf_metrics.cpp
    src/alpha/bytecode.cpp
    src/alpha/dag.cpp
    src/alpha/lexer.cpp
    src/alpha/oracle.cpp
    src/alpha/panel.cpp
    src/alpha/parser.cpp
    src/alpha/registry.cpp
    src/alpha/segment_panel.cpp
    src/alpha/typecheck.cpp
    src/alpha/unparse.cpp
    src/learn/elastic_net.cpp
    src/learn/ensemble.cpp
    src/learn/feature_matrix.cpp
    src/learn/gbt.cpp
    src/learn/hmm.cpp
    src/learn/latent.cpp
    src/learn/learned_source.cpp
    src/learn/linear_alpha.cpp
    src/learn/pipeline.cpp
    src/learn/train.cpp
    src/data/align.cpp
    src/data/catalog.cpp
    src/data/data_handler.cpp
    src/data/dataset.cpp
    src/data/shm_bar_feed.cpp
    src/parallel/batch_eval.cpp
    src/parallel/parallel_run.cpp
    src/parallel/process_executor.cpp
    src/parallel/scheduler.cpp
    src/parallel/shm_segment.cpp
    src/parallel/workload_eval.cpp
    src/parallel/workload_mine.cpp
    src/parallel/workload_streams.cpp
    src/validation/bias_audit.cpp
    # fund (p2 S2 — multi-strategy meta-book)
    src/fund/meta_allocator.cpp
    src/fund/cross_sleeve_risk.cpp
    src/fund/netting.cpp
    src/fund/meta_book.cpp
)
add_library(atx::engine ALIAS atx-engine)

target_include_directories(atx-engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-engine PUBLIC cxx_std_20)

# Opt-in PCH (dev speed): parse Eigen + atx-core vocabulary once, not once per TU.
# PRIVATE so it never leaks into consumers' include hygiene. See atx-engine/pch.hpp.
if(ATX_USE_PCH)
    target_precompile_headers(atx-engine PRIVATE pch.hpp)
endif()

# Engine builds on top of core; the warnings gate (§8) applies to engine TUs.
# atx::tsdb is PUBLIC: ShmBarFeed (a header in this target's public include tree)
# exposes atx::tsdb::SegmentReader in its interface, so consumers need it too.
target_link_libraries(atx-engine
    PUBLIC
        atx::core
        atx::tsdb
    PRIVATE
        atx_warnings
)

# The dedicated multi-process executor worker (S7-3, §0.8). ProcessExecutor spawns
# this beside the test/host module; it lands in build/bin/ via the global
# CMAKE_RUNTIME_OUTPUT_DIRECTORY, so it sits next to atx-engine-tests.exe.
add_executable(atx-shm-worker src/parallel/shm_worker_main.cpp)
target_link_libraries(atx-shm-worker PRIVATE atx::engine atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()
