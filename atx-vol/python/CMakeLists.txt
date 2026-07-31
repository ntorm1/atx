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
# S4-T18: src/bindings/fit.cpp exposes the deterministic SPY known-truth fixture
# (`make_spy_synthetic_spec`) to Python. That fixture is a TEST fixture and moved
# to atx-vol/tests/support/spy_fixture.hpp when the public surface was tiered, so
# the binding needs tests/ as a quoted-include root. A shipped binding reaching
# into a test fixture is a smell, not a design: either the synthetic-panel
# builder is promoted to a real public header or the binding drops it. Flagged
# for Sprint 5 (5.6); NOT decided here, because deciding it means changing a
# public declaration, which S4-T18 is not authorized to do.
target_include_directories(_core PRIVATE "${ATX_ROOT}/atx-vol/tests")
target_link_libraries(_core PRIVATE atx::vol)

install(TARGETS _core
    LIBRARY DESTINATION atxvol COMPONENT python
    RUNTIME DESTINATION atxvol COMPONENT python
)
