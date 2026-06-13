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
)
add_library(atx::engine ALIAS atx-engine)

target_include_directories(atx-engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-engine PUBLIC cxx_std_20)

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

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()
