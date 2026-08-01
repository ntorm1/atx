# ---- atx-vol out-of-tree smoke consumer (plan 5.1 exit criterion) ----------
#
# NOT part of the monorepo build: nothing add_subdirectory()s this file, and it
# is deliberately excluded from the main tree. It is a STANDALONE project that
# consumes atx-vol exactly the way a downstream does:
#
#   * find_package(atx-vol REQUIRED) against an INSTALL PREFIX,
#   * no FetchContent of anything (in particular NOT tl::expected, which is the
#     public error type -- see atx/vol/types.hpp),
#   * no add_subdirectory of the atx tree, no knowledge of where it was built.
#
# scripts/atx-vol-test-package.ps1 drives it: fresh prefix -> cmake --install ->
# configure -> build -> run twice -> byte-compare the two runs.
#
# The vcpkg-provided dependencies atx::core exposes PUBLICLY (Arrow, Parquet,
# zstd) are found by the generated atx-volConfig.cmake through find_dependency,
# so this project only needs them on CMAKE_PREFIX_PATH / the vcpkg toolchain --
# the same thing any real consumer of an Arrow-exposing library must do.
cmake_minimum_required(VERSION 3.25)

# Mirror atx-vol/python/CMakeLists.txt: a caller-provided toolchain always wins,
# otherwise pick up vcpkg from the environment before project() probes.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain")
endif()

project(atx-vol-test-package LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(atx-vol REQUIRED)

add_executable(atx-vol-smoke smoke.cpp)
target_link_libraries(atx-vol-smoke PRIVATE atx::vol)
