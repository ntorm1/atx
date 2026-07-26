add_executable(atx-kb-eval eval_main.cpp)
target_link_libraries(atx-kb-eval PRIVATE atx::kb atx_warnings)

add_executable(atx-kb-vector-benchmark vector_benchmark.cpp)
target_link_libraries(atx-kb-vector-benchmark PRIVATE atx::kb atx_warnings)

add_custom_target(atx-kb-vector-benchmark-100k
    COMMAND $<TARGET_FILE:atx-kb-vector-benchmark>
        --documents 100000 --queries 8 --dimensions 48 --top-k 10 --ef-search 64
    DEPENDS atx-kb-vector-benchmark
    USES_TERMINAL
    COMMENT "Running opt-in 100k exact-versus-ANN evidence tier"
)

if(ATX_BUILD_TESTS)
    add_test(
        NAME atx-kb-eval-quality
        COMMAND $<TARGET_FILE:atx-kb-eval> --suite quality --gate
    )
    set_tests_properties(atx-kb-eval-quality PROPERTIES
        LABELS "atx_kb;atx_kb_eval"
        RUN_SERIAL TRUE
        TIMEOUT 120
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
    add_test(
        NAME atx-kb-vector-benchmark-gate
        COMMAND $<TARGET_FILE:atx-kb-vector-benchmark>
    )
    set_tests_properties(atx-kb-vector-benchmark-gate PROPERTIES
        LABELS "atx_kb;atx_kb_eval;benchmark"
        RUN_SERIAL TRUE
        TIMEOUT 180
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
endif()
