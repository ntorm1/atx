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

# ── The version mirror, checked here rather than only after a wheel exists ────
#
# `pyproject.toml`'s `version =` is a hand-maintained copy of
# `project(atx VERSION ...)`, because a PEP 621 table cannot read a CMake
# variable. `tests/test_atxvol.py` asserts the compiled `atx::vol::version()`
# against `importlib.metadata.version("atxvol")`, which is the right assertion --
# but it can only run once a wheel has been BUILT and INSTALLED, so a bump to the
# root project version leaves the mirror stale with a fully green tree until
# someone happens to cut a wheel. That is exactly what happened at 1.1.0.
#
# This closes the gap at CONFIGURE time, before anything is compiled, and names
# the file to edit. Deliberately a plain regex rather than a TOML parser: CMake
# has no TOML reader, and the alternative -- a scikit-build-core dynamic-version
# provider -- would move the version out of the static PEP 621 metadata other
# tooling reads directly.
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/pyproject.toml" ATXVOL_PYPROJECT_TEXT)
set(ATXVOL_VERSION_KEY_RE "[\r\n][ \t]*version[ \t]*=[ \t]*\"([0-9]+\\.[0-9]+\\.[0-9]+)\"")
if(NOT ATXVOL_PYPROJECT_TEXT MATCHES "${ATXVOL_VERSION_KEY_RE}")
    message(FATAL_ERROR
        "atx-vol/python/pyproject.toml: no 'version = \"X.Y.Z\"' line found to check against "
        "project(atx VERSION ...). If the key moved or became dynamic, update this guard too -- "
        "do not delete it: the wheel-test assertion it backs up only runs after a wheel exists.")
endif()
set(ATXVOL_PYPROJECT_VERSION "${CMAKE_MATCH_1}")
# The other side of the mirror is read from the ROOT directory's scope, never this one:
# project(atx VERSION ...) runs inside the add_subdirectory() above, so `atx_VERSION` is a
# variable of that directory and was always empty here -- which failed every wheel
# configure with "... the root CMakeLists is ." (found 2026-10-23). An unreadable value
# stops the configure rather than passing or misreporting the comparison.
get_directory_property(ATXVOL_ROOT_VERSION DIRECTORY "${ATX_ROOT}" DEFINITION atx_VERSION)
if(NOT ATXVOL_ROOT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR
        "could not read project(atx VERSION ...) from ${ATX_ROOT}/CMakeLists.txt (got "
        "'${ATXVOL_ROOT_VERSION}'); the version-mirror check below needs it. If the root "
        "project was renamed, read its <name>_VERSION here instead.")
endif()
if(NOT ATXVOL_PYPROJECT_VERSION VERSION_EQUAL "${ATXVOL_ROOT_VERSION}")
    message(FATAL_ERROR
        "version mirror drifted: atx-vol/python/pyproject.toml declares "
        "${ATXVOL_PYPROJECT_VERSION}, but project(atx VERSION ...) in the root CMakeLists is "
        "${ATXVOL_ROOT_VERSION}. Bump the 'version =' line in atx-vol/python/pyproject.toml to "
        "${ATXVOL_ROOT_VERSION}.")
endif()

find_package(Python COMPONENTS Interpreter Development.Module REQUIRED)
find_package(pybind11 CONFIG REQUIRED)

# The TU list, include roots and link line are shared with the in-tree
# ATX_VOL_PYTHON target (atx-vol/CMakeLists.txt) through one file, so the two
# builds cannot drift apart. The wheel's target keeps the plain name `_core`.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/atxvol_core.cmake")
atxvol_add_core_module(_core)

install(TARGETS _core
    LIBRARY DESTINATION atxvol COMPONENT python
    RUNTIME DESTINATION atxvol COMPONENT python
)

# The DLL closure travels WITH the module. `_core` imports vcpkg's arrow.dll /
# parquet.dll (atx-core's unconditional Parquet loader) and, through them, zstd /
# lz4 / snappy / brotli / bz2 / zlib / libcrypto; Python 3.8+ resolves an
# extension's DLL imports from the extension's OWN directory (never PATH), so a
# wheel holding only `_core*.pyd` cannot be imported anywhere -- the first wheel
# built from this recipe in months installed cleanly and then failed `import
# atxvol` with "DLL load failed while importing _core: The specified module could
# not be found" (lane PY, 2026-10-23). vcpkg's applocal POST_BUILD step has
# already copied exactly that closure beside the built module, so install every
# DLL in the module's own output directory (non-recursive: bin/ holds the CLIs'
# copies). Collision with a same-process pyarrow is a separate, documented
# limitation (README "Known limitation").
install(CODE "
    file(GLOB _atxvol_dlls LIST_DIRECTORIES false \"$<TARGET_FILE_DIR:_core>/*.dll\")
    if(NOT _atxvol_dlls)
        message(FATAL_ERROR \"atxvol: no DLLs beside $<TARGET_FILE:_core>; the vcpkg applocal step did not run\")
    endif()
    file(INSTALL \${_atxvol_dlls} DESTINATION \"\${CMAKE_INSTALL_PREFIX}/atxvol\")
" COMPONENT python)
