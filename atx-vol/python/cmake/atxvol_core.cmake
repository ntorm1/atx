# The atxvol `_core` extension, defined ONCE for both of its builds:
#
#   * the wheel project beside this directory (atx-vol/python/CMakeLists.txt,
#     scikit-build-core), target `_core`;
#   * the in-tree opt-in `ATX_VOL_PYTHON` block in atx-vol/CMakeLists.txt, target
#     `atx-vol-python-core`, which the `atx-vol-python` ctest lane gates on.
#
# Before 2026-10-23 the TU list lived only in the wheel project, so nothing the
# monorepo build ran ever compiled a binding: the ten TUs drifted against HEAD for
# weeks while every ctest lane reported the Python suite as Skipped. One list here
# means a TU added for one build cannot be missing from the other.
#
# The includer must already have run find_package(pybind11 CONFIG) and defined
# atx::vol.

get_filename_component(ATXVOL_PYTHON_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(ATXVOL_BINDING_SOURCES
    module.cpp
    pricing.cpp
    surface.cpp
    surface_db.cpp
    strategy.cpp
    backtest.cpp
    analytics.cpp
    dispersion.cpp
    fit.cpp
    quote_slice.cpp
)

# atxvol_add_core_module(<target>)
# Defines a pybind11 module target whose file is `_core<EXT_SUFFIX>` (the
# PYBIND11_MODULE name in module.cpp) whatever the CMake target is called.
function(atxvol_add_core_module target)
    set(_atxvol_sources "")
    foreach(_atxvol_tu IN LISTS ATXVOL_BINDING_SOURCES)
        list(APPEND _atxvol_sources "${ATXVOL_PYTHON_ROOT}/src/bindings/${_atxvol_tu}")
    endforeach()
    pybind11_add_module(${target} ${_atxvol_sources})
    set_target_properties(${target} PROPERTIES OUTPUT_NAME _core)
    target_include_directories(${target} PRIVATE "${ATXVOL_PYTHON_ROOT}/src/bindings")
    # api/ restructure (2026-08-14): PRIVATE src/ include root, for parity with
    # the other internal targets (atx-vol/CMakeLists.txt). Not architecturally
    # required -- the extension should only ever need the public atx/vol/api/
    # surface -- but every internal-facing target carries this root uniformly.
    target_include_directories(${target} PRIVATE "${ATXVOL_PYTHON_ROOT}/../src")
    # S5-T24: make_spy_synthetic_panel reaches the SPY known-truth fixture through
    # the Tier-B public header atx/vol/spy_fixture.hpp, so the extension needs
    # nothing but atx::vol's own include interface (no tests/ include root).
    target_link_libraries(${target} PRIVATE atx::vol)
endfunction()
