cmake_minimum_required(VERSION 3.25)

# Discover vcpkg before project() performs compiler/dependency detection. A
# caller-provided toolchain always wins.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{VCPKG_ROOT})
    set(CMAKE_TOOLCHAIN_FILE
        "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
        CACHE FILEPATH "vcpkg toolchain")
endif()

get_filename_component(ATX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
if(NOT DEFINED VCPKG_INSTALLED_DIR)
    if(EXISTS "${ATX_ROOT}/build-rel/vcpkg_installed")
        set(VCPKG_INSTALLED_DIR "${ATX_ROOT}/build-rel/vcpkg_installed"
            CACHE PATH "Reuse the monorepo vcpkg tree")
    elseif(EXISTS "${ATX_ROOT}/build/vcpkg_installed")
        set(VCPKG_INSTALLED_DIR "${ATX_ROOT}/build/vcpkg_installed"
            CACHE PATH "Reuse the monorepo vcpkg tree")
    endif()
endif()

# clang-cl can be invoked from a plain PowerShell session, where the Windows SDK
# tools are commonly absent from PATH. CMake needs mt.exe to link even its
# compiler probe, so locate the newest installed SDK explicitly.
if(WIN32 AND (NOT CMAKE_MT OR CMAKE_MT MATCHES "NOTFOUND"))
    file(GLOB ATXVOL_WINDOWS_SDK_BINS LIST_DIRECTORIES true
        "C:/Program Files (x86)/Windows Kits/10/bin/*")
    list(SORT ATXVOL_WINDOWS_SDK_BINS COMPARE NATURAL ORDER DESCENDING)
    foreach(ATXVOL_SDK_BIN IN LISTS ATXVOL_WINDOWS_SDK_BINS)
        if(EXISTS "${ATXVOL_SDK_BIN}/x64/mt.exe")
            set(CMAKE_MT "${ATXVOL_SDK_BIN}/x64/mt.exe" CACHE FILEPATH
                "Windows manifest tool" FORCE)
            break()
        endif()
    endforeach()
endif()

project(atxvol LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build atx-vol and its monorepo dependencies in the wheel build tree.
add_subdirectory("${ATX_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/atx-monorepo")

find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_core
    src/bindings/module.cpp
    src/bindings/pricing.cpp
    src/bindings/surface.cpp
    src/bindings/surface_db.cpp
    src/bindings/strategy.cpp
    src/bindings/backtest.cpp
    src/bindings/analytics.cpp
    src/bindings/dispersion.cpp
    src/bindings/fit.cpp
)
target_include_directories(_core PRIVATE src/bindings)
# api/ restructure (2026-08-14): PRIVATE src/ include root, for parity with
# the other internal targets (atx-vol/CMakeLists.txt). Not architecturally
# required -- python/ is one of the three external reachability roots the
# placement measurement itself was computed from, so _core should only ever
# need the public atx/vol/api/ surface -- but added per the restructure's own
# decision to give every internal-facing target this root uniformly.
target_include_directories(_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
# S5-T24 resolved the S4-T18 carried finding: src/bindings/fit.cpp exposes the
# deterministic SPY known-truth fixture (`make_spy_synthetic_spec`) to Python as
# `make_spy_synthetic_panel`, and it used to reach it through an extra
# atx-vol/tests quoted-include root -- a SHIPPED artifact reaching into a test
# fixture, which an installed tree (no tests/) cannot satisfy at all. The
# builder is now the Tier-B public header atx/vol/spy_fixture.hpp, so the
# extension needs nothing but atx::vol's own include interface.
target_link_libraries(_core PRIVATE atx::vol)

install(TARGETS _core
    LIBRARY DESTINATION atxvol COMPONENT python
    RUNTIME DESTINATION atxvol COMPONENT python
)
