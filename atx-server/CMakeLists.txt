find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
option(ATX_SERVER_WITH_GRPC "Build the gRPC workspace service and server" ON)
add_library(atx-server-runtime STATIC src/runtime_identity.cpp)
target_include_directories(atx-server-runtime PUBLIC include)
target_compile_features(atx-server-runtime PUBLIC cxx_std_20)
target_link_libraries(atx-server-runtime PRIVATE atx::core
    nlohmann_json::nlohmann_json atx_warnings)
add_library(atx-server-access STATIC src/access.cpp)
target_include_directories(atx-server-access PUBLIC include)
target_compile_features(atx-server-access PUBLIC cxx_std_20)
target_link_libraries(atx-server-access PRIVATE OpenSSL::Crypto
    nlohmann_json::nlohmann_json atx_warnings)
include(cmake/duckdb.cmake)
add_library(atx-server-database STATIC src/database.cpp src/database_duckdb.cpp
    src/database_duckdb_session.cpp src/database_duckdb_result.cpp)
target_include_directories(atx-server-database PUBLIC include)
target_compile_features(atx-server-database PUBLIC cxx_std_20)
target_link_libraries(atx-server-database PRIVATE atx_duckdb Threads::Threads atx_warnings)

add_library(atx-server-research STATIC
    src/research.cpp src/research_import.cpp src/research_metrics.cpp src/research_real.cpp
    src/research_dsl.cpp src/research_comparison.cpp src/research_expression.cpp
    src/research_dataset.cpp src/research_dataset_load.cpp src/research_dataset_replay.cpp
    src/research_portfolio.cpp src/research_portfolio_blend.cpp src/research_portfolio_replay.cpp
    src/research_paper.cpp src/research_paper_ledger.cpp)
target_include_directories(atx-server-research PUBLIC include)
target_compile_features(atx-server-research PUBLIC cxx_std_20)
target_link_libraries(atx-server-research PRIVATE atx::engine atx::core atx-impl-core
    nlohmann_json::nlohmann_json atx_duckdb atx_warnings)
add_library(atx-server-core STATIC src/store.cpp src/events.cpp src/scheduler.cpp
    src/workspace_validation.cpp src/library_validation.cpp src/library_store.cpp
    src/experiment_validation.cpp src/experiment_store.cpp src/experiment_jobs.cpp
    src/governance_audit.cpp src/governance_migration.cpp src/governance_store.cpp
    src/governance_evidence.cpp src/workflow_migration.cpp src/workflow_query.cpp
    src/workflow_store.cpp src/workflow_dispatch.cpp)
target_include_directories(atx-server-core PUBLIC include)
target_compile_features(atx-server-core PUBLIC cxx_std_20)
target_link_libraries(atx-server-core PUBLIC Threads::Threads
    PRIVATE atx_sqlite3 atx::core nlohmann_json::nlohmann_json atx_warnings)
if(ATX_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
    add_executable(atx-server-runtime-tests tests/runtime_identity_test.cpp)
    target_link_libraries(atx-server-runtime-tests PRIVATE atx-server-runtime
        nlohmann_json::nlohmann_json GTest::gtest_main atx_warnings)
    gtest_discover_tests(atx-server-runtime-tests
        PROPERTIES LABELS atx_server DISCOVERY_TIMEOUT 60)
    add_executable(atx-server-access-tests tests/access_test.cpp)
    target_link_libraries(atx-server-access-tests PRIVATE atx-server-access OpenSSL::Crypto
        nlohmann_json::nlohmann_json GTest::gtest_main atx_warnings)
    gtest_discover_tests(atx-server-access-tests
        PROPERTIES LABELS atx_server DISCOVERY_TIMEOUT 60)
    add_executable(atx-server-database-tests tests/database_test.cpp)
    target_link_libraries(atx-server-database-tests PRIVATE atx-server-database atx_duckdb
        GTest::gtest_main atx_warnings)
    atx_server_copy_duckdb(atx-server-database-tests)
    gtest_discover_tests(atx-server-database-tests PROPERTIES LABELS atx_server DISCOVERY_TIMEOUT 60)
    add_executable(atx-server-research-tests tests/research_test.cpp tests/research_real_test.cpp
        tests/research_dsl_test.cpp tests/research_dataset_test.cpp tests/analysis_test.cpp
        tests/research_portfolio_test.cpp tests/research_paper_test.cpp)
    target_link_libraries(atx-server-research-tests PRIVATE atx-server-research
        nlohmann_json::nlohmann_json atx::core atx::engine atx_duckdb
        GTest::gtest_main atx_warnings)
    atx_server_copy_duckdb(atx-server-research-tests)
    gtest_discover_tests(atx-server-research-tests
        PROPERTIES LABELS atx_server DISCOVERY_TIMEOUT 60)
    add_executable(atx-server-core-tests tests/core_test.cpp tests/workspace_test.cpp
        tests/library_test.cpp tests/experiment_test.cpp tests/governance_test.cpp
        tests/workflow_test.cpp)
    target_link_libraries(atx-server-core-tests PRIVATE atx-server-core
        atx_sqlite3 atx::core nlohmann_json::nlohmann_json GTest::gtest_main atx_warnings)
    gtest_discover_tests(atx-server-core-tests
        PROPERTIES LABELS atx_server DISCOVERY_TIMEOUT 60)
endif()
if(NOT ATX_SERVER_WITH_GRPC)
    return()
endif()
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)

set(atx_proto_root "${CMAKE_CURRENT_SOURCE_DIR}/proto")
set(atx_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(atx_proto "${atx_proto_root}/atx/v1/workspace.proto")
set(atx_proto_base "${atx_generated}/atx/v1/workspace")
file(MAKE_DIRECTORY "${atx_generated}/atx/v1")
add_custom_command(
    OUTPUT "${atx_proto_base}.pb.cc" "${atx_proto_base}.pb.h"
           "${atx_proto_base}.grpc.pb.cc" "${atx_proto_base}.grpc.pb.h"
    COMMAND protobuf::protoc
    ARGS "--proto_path=${atx_proto_root}" "--cpp_out=${atx_generated}"
         "--grpc_out=${atx_generated}"
         "--plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>" "${atx_proto}"
    DEPENDS "${atx_proto}" protobuf::protoc gRPC::grpc_cpp_plugin
    VERBATIM)
add_library(atx-server-proto STATIC
    "${atx_proto_base}.pb.cc" "${atx_proto_base}.grpc.pb.cc")
target_include_directories(atx-server-proto SYSTEM PUBLIC "${atx_generated}")
target_link_libraries(atx-server-proto PUBLIC protobuf::libprotobuf gRPC::grpc++)
include(cmake/protobuf-msvc-abi.cmake)
atx_server_protobuf_msvc_abi(atx-server-proto)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Protoc generates non-standard-layout offsets for its private, versioned runtime tables.
    # This diagnostic exception applies only to generated code, never first-party sources.
    if(MSVC)
        target_compile_options(atx-server-proto PRIVATE /clang:-Wno-invalid-offsetof)
    else()
        target_compile_options(atx-server-proto PRIVATE -Wno-invalid-offsetof)
    endif()
endif()

add_library(atx-server-transport STATIC src/service.cpp)
target_include_directories(atx-server-transport PUBLIC include)
target_link_libraries(atx-server-transport PUBLIC atx-server-core atx-server-proto atx-server-database
    atx-server-access
    PRIVATE OpenSSL::Crypto nlohmann_json::nlohmann_json atx-server-research atx_warnings)
add_executable(atx-server src/main.cpp)
target_link_libraries(atx-server PRIVATE atx-server-transport atx-server-research
    atx-server-runtime atx_warnings)
atx_server_copy_duckdb(atx-server)

if(ATX_BUILD_TESTS)
    add_executable(atx-server-tests tests/service_test.cpp tests/analysis_service_test.cpp
        tests/experiment_service_test.cpp tests/team_service_test.cpp tests/workflow_service_test.cpp)
    target_link_libraries(atx-server-tests PRIVATE atx-server-transport
        atx_sqlite3 OpenSSL::Crypto nlohmann_json::nlohmann_json GTest::gtest_main atx_warnings)
    atx_server_copy_duckdb(atx-server-tests)
    include(GoogleTest)
    gtest_discover_tests(atx-server-tests PROPERTIES LABELS atx_server DISCOVERY_TIMEOUT 60)
endif()
