cmake_minimum_required(VERSION 3.20)

project(atx
    VERSION 0.1.0
    DESCRIPTION "ATX monorepo: core + engine"
    LANGUAGES C CXX
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
    SYSTEM
)
FetchContent_MakeAvailable(tl-expected)

# ankerl::unordered_dense: fast flat hash map/set (header-only, bundles wyhash).
FetchContent_Declare(
    unordered_dense
    GIT_REPOSITORY https://github.com/martinus/unordered_dense.git
    GIT_TAG        v4.4.0
    SYSTEM
)
FetchContent_MakeAvailable(unordered_dense)

# Eigen: header-only linear algebra. We bypass Eigen's own CMake (it enables a
# Fortran toolchain for its test suite, which need not exist here) and expose the
# headers through a minimal INTERFACE target.
FetchContent_Declare(
    Eigen3
    GIT_REPOSITORY https://gitlab.com/libeigen/eigen.git
    GIT_TAG        3.4.0
)
FetchContent_GetProperties(Eigen3)
if(NOT eigen3_POPULATED)
    FetchContent_Populate(Eigen3)
    add_library(eigen INTERFACE)
    target_include_directories(eigen SYSTEM INTERFACE ${eigen3_SOURCE_DIR})
    add_library(Eigen3::Eigen ALIAS eigen)
endif()

# xsimd: header-only SIMD abstraction.
FetchContent_Declare(
    xsimd
    GIT_REPOSITORY https://github.com/xtensor-stack/xsimd.git
    GIT_TAG        13.0.0
    SYSTEM
)
FetchContent_MakeAvailable(xsimd)

# ---- Shared warnings gate (agent profile §8) ----
# Applied PRIVATE to atx targets only; third-party deps are SYSTEM includes so
# their headers never trip our warnings-as-errors.
option(ATX_WERROR "Treat warnings as errors on atx targets" ON)

# Opt-in faster zip inflate (Phase 4): when ON, ZipEntryReader decompresses zip
# entries via zlib-ng (2-4x faster than the vendored miniz inflate) behind the
# unchanged read() interface, validating CRC32 against the central directory.
# Default OFF -> the miniz path is used verbatim and zlib-ng is not linked.
option(ATX_FAST_INFLATE "Use zlib-ng for zip inflate in ZipEntryReader" OFF)

add_library(atx_warnings INTERFACE)
if(MSVC)
    target_compile_options(atx_warnings INTERFACE /W4 /permissive-)
    if(ATX_WERROR)
        target_compile_options(atx_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(atx_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    if(ATX_WERROR)
        target_compile_options(atx_warnings INTERFACE -Werror)
    endif()
endif()

# ---- Precompiled headers (fast cold / fresh-worktree builds) ----
# ON by default in the `ninja` and `dev` presets: the heavy+stable headers (Eigen,
# gtest, atx-core vocabulary) are parsed once into a PCH and reused by every TU,
# cutting a cold/fresh-worktree build of the ~189 first-party TUs from ~10 min to a
# couple. The PCH — not the object cache — is what makes a fresh worktree fast:
# sccache cannot share first-party objects across worktrees (its cache keys embed
# per-worktree absolute paths). Include-hygiene enforcement (every TU compiled with
# its own includes, catching missing/unused ones) lives in the `hygiene` preset
# (ATX_USE_PCH OFF) for CI/nightly, so the PCH never hides an include defect.
# The option default stays OFF so a bare `cmake` (no preset) gets the strict build.
option(ATX_USE_PCH "Use precompiled headers on atx targets (dev speed; ON in ninja/dev presets, OFF in hygiene)" OFF)
option(ATX_UNITY_BUILD "Unity/jumbo build atx test target (dev speed)" OFF)

# ---- Benchmarks (opt-in; Google Benchmark is a test/bench-only dep) ----
option(ATX_BUILD_BENCH "Build ATX benchmarks" OFF)
if(ATX_BUILD_BENCH)
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.9.0
        SYSTEM
    )
    FetchContent_MakeAvailable(benchmark)
endif()

# Only configure tests / GoogleTest when this is the top-level project.
option(ATX_BUILD_TESTS "Build ATX tests" ${PROJECT_IS_TOP_LEVEL})

if(ATX_BUILD_TESTS)
    enable_testing()

    # GoogleTest comes from vcpkg (manifest dep), not a per-build-dir FetchContent
    # clone+compile. The vcpkg binary cache builds gtest once and every fresh build
    # dir / worktree links the cached binary instead of recompiling the framework
    # from source — that source build was pure tax on every cold build. CRT linkage
    # follows the active triplet (x64-windows = dynamic /MD), matching the engine.
    find_package(GTest CONFIG REQUIRED)
endif()

add_subdirectory(atx-core)
add_subdirectory(atx-tsdb)
add_subdirectory(atx-engine)
add_subdirectory(atx-impl)
