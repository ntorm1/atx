cmake_minimum_required(VERSION 3.20)

project(atx
    VERSION 0.1.0
    DESCRIPTION "ATX monorepo: core + engine"
    LANGUAGES CXX
)

# ---- Global settings ----
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put all build artifacts in predictable locations.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ---- Dependencies ----
include(FetchContent)

# spdlog: logging backend (bundles fmt).
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.14.1
)
FetchContent_MakeAvailable(spdlog)

# TartanLlama/expected: std::expected-alike (Result<T, E>).
set(EXPECTED_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(EXPECTED_BUILD_PACKAGE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    tl-expected
    GIT_REPOSITORY https://github.com/TartanLlama/expected.git
    GIT_TAG        v1.1.0
)
FetchContent_MakeAvailable(tl-expected)

# Only configure tests / GoogleTest when this is the top-level project.
option(ATX_BUILD_TESTS "Build ATX tests" ${PROJECT_IS_TOP_LEVEL})

if(ATX_BUILD_TESTS)
    enable_testing()

    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    # For Windows/MSVC: keep CRT linkage consistent.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_subdirectory(atx-core)
add_subdirectory(atx-engine)
