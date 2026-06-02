add_library(atx-core STATIC
    src/core.cpp
    src/decimal.cpp
    src/datetime.cpp
    src/symbol.cpp
)
add_library(atx::core ALIAS atx-core)

target_include_directories(atx-core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-core PUBLIC cxx_std_20)

target_link_libraries(atx-core
    PUBLIC
        spdlog::spdlog
        tl::expected
        unordered_dense::unordered_dense
        Eigen3::Eigen
        xsimd
    PRIVATE
        atx_warnings
)

# Compile in every log level; runtime level (default err) does the filtering.
# Raise this in release builds to strip low-severity calls at compile time.
target_compile_definitions(atx-core
    PUBLIC
        SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

if(ATX_BUILD_BENCH)
    add_subdirectory(bench)
endif()
