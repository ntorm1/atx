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

# ============================================================================
#  Build-speed & caching  (fast cold + incremental, worktree-friendly)
#  Every knob is an option so the canonical/CI path can pin the strict/static
#  build; the `ninja` + `dev` presets flip the fast ones on. See scripts/dev-setup.md.
# ============================================================================

# C++20 + Ninja otherwise runs a C++-module dependency scan on every TU. atx uses
# no modules, so disable it — one fewer step per compile (cold AND incremental).
# (CMAKE_CXX_SCAN_FOR_MODULES is 3.28+; older CMake silently ignores it.)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Skip linking/recompiling deps a target does not actually reference (3.19+).
set(CMAKE_OPTIMIZE_DEPENDENCIES ON)

# ---- Compiler cache (ccache preferred, sccache fallback) --------------------
# ccache (>=4.13) caches clang-cl including MSVC-style PCH TUs (/Yc /Yu /Fp),
# which sccache refuses (447 "non-cacheable /Fp" calls in the 2026-07-12 stats).
# Cross-worktree object reuse needs the CCACHE_* env keys set by the presets
# (BASEDIR/NOHASHDIR/SLOPPINESS/IGNOREOPTIONS) + the -ffile-prefix-map below;
# see scripts/dev-setup.md "Verified cache mechanics". No-op when neither tool
# is installed, so a bare `cmake` still configures.
set(ATX_COMPILER_CACHE "AUTO" CACHE STRING
    "Compiler cache launcher: AUTO (ccache then sccache), ccache, sccache, or OFF")
if(NOT ATX_COMPILER_CACHE STREQUAL "OFF" AND NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(_atx_cache_candidates ccache sccache)
    if(NOT ATX_COMPILER_CACHE STREQUAL "AUTO")
        set(_atx_cache_candidates ${ATX_COMPILER_CACHE})
    endif()
    find_program(ATX_CACHE_EXE NAMES ${_atx_cache_candidates})
    if(ATX_CACHE_EXE)
        set(CMAKE_C_COMPILER_LAUNCHER   "${ATX_CACHE_EXE}" CACHE STRING "" FORCE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${ATX_CACHE_EXE}" CACHE STRING "" FORCE)
        message(STATUS "atx: compiler cache enabled (${ATX_CACHE_EXE})")
    endif()
endif()

# Deprecated alias (pre-2026-07 presets/scripts): ATX_USE_SCCACHE=OFF disables caching.
option(ATX_USE_SCCACHE "DEPRECATED: use ATX_COMPILER_CACHE=OFF" ON)
if(NOT ATX_USE_SCCACHE)
    set(CMAKE_C_COMPILER_LAUNCHER "" CACHE STRING "" FORCE)
    set(CMAKE_CXX_COMPILER_LAUNCHER "" CACHE STRING "" FORCE)
endif()

# Strip the build-root prefix from __FILE__, debug info, and diagnostics so a TU
# compiled in ANY worktree yields byte-identical output -> compiler-cache HITS across
# worktrees (and reproducible builds). clang/clang-cl accept -ffile-prefix-map.
# NOTE: the flag's VALUE embeds this worktree's path, so ccache must be told to
# ignore it when hashing (CCACHE_IGNOREOPTIONS in the presets) or every worktree
# would hash differently and never share objects.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # clang-cl (MSVC frontend) rejects the GNU-spelled flag as an unknown
    # argument, which /WX then promotes to a hard error; forward it through the
    # embedded clang driver with /clang: instead. Plain clang (GNU frontend)
    # takes it directly.
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_compile_options("/clang:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
    else()
        add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
    endif()
endif()

# ---- Fast linker: LLD ------------------------------------------------------
# lld-link ships with the clang-cl toolchain and links far faster than MSVC
# link.exe (the dev loop relinks ~16 first-party exes). CMAKE_LINKER_TYPE is the
# clean knob on 3.29+; older CMake falls back to the clang driver flag.
option(ATX_USE_LLD "Link with LLD (lld-link) instead of MSVC link.exe" ON)
if(ATX_USE_LLD AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        set(CMAKE_LINKER_TYPE LLD)
    else()
        add_link_options(-fuse-ld=lld)
    endif()
endif()

# ---- Throttle concurrent links ---------------------------------------------
# Compiles are CPU-bound and parallelize freely; links (each pulling Arrow + the
# engine) are RAM/IO-bound. A dedicated Ninja job pool keeps a "build all tests"
# from launching 14 heavy links at once and thrashing memory. (Ninja-only; ignored
# by other generators.)
set_property(GLOBAL PROPERTY JOB_POOLS atx_link=4)
set(CMAKE_JOB_POOL_LINK atx_link)

# ---- First-party library linkage ------------------------------------------
# STATIC by default (canonical/CI, self-contained exes). ATX_SHARED_LIBS=ON (set by
# the `dev` preset) builds the atx libraries as DLLs: each test exe then links a thin
# import lib instead of embedding a full copy of the engine -> much faster relinks and
# far smaller per-worktree build dirs (one engine DLL vs ~14 static copies). clang-cl +
# WINDOWS_EXPORT_ALL_SYMBOLS auto-exports; the DLLs land in bin/ beside the exes that
# load them, so ctest/applocal need no extra wiring.
option(ATX_SHARED_LIBS "Build atx libraries as shared DLLs (dev: faster links, smaller artifacts)" OFF)
if(ATX_SHARED_LIBS)
    set(ATX_LIB_TYPE SHARED)
    set(CMAKE_WINDOWS_EXPORT_ALL_SYMBOLS ON)
else()
    set(ATX_LIB_TYPE STATIC)
endif()

# ---- Dependencies ----
# Shared FetchContent clone/build cache: every preset (not just `dev`) reuses
# $env{ATX_DEPS_DIR} (C:\atx-cache\deps) when it is set, so a fresh worktree
# never re-clones spdlog/eigen/xsimd/... Falls back to the standard per-build
# _deps dir when the env var is absent (CI, bare cmake).
if(DEFINED ENV{ATX_DEPS_DIR} AND NOT FETCHCONTENT_BASE_DIR)
    set(FETCHCONTENT_BASE_DIR "$ENV{ATX_DEPS_DIR}" CACHE PATH "shared FetchContent dir")
endif()
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
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        # Visual Studio emits one cl invocation with many translation units. /MP
        # fans them out; clang-cl diagnoses this MSVC-only switch as unused.
        target_compile_options(atx_warnings INTERFACE /MP)
    endif()
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

# Cross-sectional VM kernels: the default fast O(n) grouped path (cs_ops.hpp) is
# byte-identical to the O(n^2) reference (the alpha_cs_test oracle differential
# proves it). This switch compiles the literal O(n^2) reference loops instead —
# an audit / bisection hatch only. It MUST be a WHOLE-BUILD define: cs_ops.hpp's
# kernels are inline, so a per-target definition would be an ODR violation. The
# global add_compile_definitions below (before any add_subdirectory) guarantees
# every TU sees the same definition.
option(ATX_ALPHA_CS_REFERENCE "Compile the O(n^2) reference cross-sectional VM kernels (byte-identical audit path; default OFF = fast O(n))" OFF)
if(ATX_ALPHA_CS_REFERENCE)
    add_compile_definitions(ATX_ALPHA_CS_REFERENCE)
endif()

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

# ---- atx-vol algorithm counters (P0.2; opt-in, compile-time) ----
# When ON, atx-vol is compiled with -DATX_VOL_COUNTERS so the pricer/cache/
# portfolio hot paths increment the header-only atomic counters in
# atx/vol/counters.hpp; the bench targets dump them into the JSON. Default OFF:
# ATX_VOL_COUNT(...) expands to ((void)0) — zero runtime cost, zero ABI change.
# The definition is attached to the atx-vol target in atx-vol/CMakeLists.txt.
option(ATX_VOL_COUNTERS "Instrument atx-vol pricing hot paths with opt-in algorithm counters" OFF)
option(ATX_VOL_PROFILE "Instrument atx-vol phase wall times with compile-time opt-in timers" OFF)

# Only configure tests / GoogleTest when this is the top-level project.
option(ATX_BUILD_TESTS "Build ATX tests" ${PROJECT_IS_TOP_LEVEL})
option(ATX_BUILD_UI "Build the ATX volatility desktop workspace" OFF)

if(ATX_BUILD_TESTS)
    enable_testing()

    # GoogleTest comes from vcpkg (manifest dep), not a per-build-dir FetchContent
    # clone+compile. The vcpkg binary cache builds gtest once and every fresh build
    # dir / worktree links the cached binary instead of recompiling the framework
    # from source — that source build was pure tax on every cold build. CRT linkage
    # follows the active triplet (x64-windows = dynamic /MD), matching the engine.
    find_package(GTest CONFIG REQUIRED)

    # FIX-3/F3-B: repo-wide test support (`atx-test-scratch`). Added BEFORE the
    # project subdirectories so every test target below can link it.
    add_subdirectory(tests)
endif()

add_subdirectory(atx-core)
add_subdirectory(atx-vol)
add_subdirectory(atx-tsdb)
add_subdirectory(atx-engine)
add_subdirectory(atx-impl)
if(ATX_BUILD_UI)
    add_subdirectory(atx-ui)
endif()
