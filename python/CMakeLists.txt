cmake_minimum_required(VERSION 3.25)
project(atxpy LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build the monorepo (atx-core / atx-tsdb / atx-engine) in-tree. The root is the
# parent of python/; give it an explicit binary dir since it is outside our tree.
# As a subdir it is not top-level, so its tests/bench stay off by default (and we
# also force them off via cache defines in pyproject.toml).
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_BINARY_DIR}/atx-monorepo)

# pybind11 from the pip-installed package (scikit-build-core puts its CMake config
# dir on CMAKE_PREFIX_PATH at build time).
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_core
    src/_bindings/module.cpp
    src/_bindings/bind_core.cpp
    src/_bindings/bind_symbol.cpp
    src/_bindings/bind_config.cpp
    src/_bindings/bind_alpha.cpp
    src/_bindings/bind_backtest.cpp
    src/_bindings/bind_eval.cpp
    src/_bindings/bind_risk.cpp
    src/_bindings/bind_factory.cpp
    src/_bindings/shim/backtest_shim.cpp
    src/_bindings/shim/mining_shim.cpp
)
target_include_directories(_core PRIVATE src/_bindings)
target_link_libraries(_core PRIVATE atx::engine)
# pybind11 headers trip the engine's /WX gate, so the extension does NOT link
# atx_warnings; it compiles with the default (relaxed) warning profile.

install(TARGETS _core LIBRARY DESTINATION atxpy RUNTIME DESTINATION atxpy)
