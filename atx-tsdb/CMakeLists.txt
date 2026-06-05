# atx-tsdb: immutable shared-memory time-series store. Sources are globbed so a
# new unit only drops a src/*.cpp; CONFIGURE_DEPENDS re-globs on change.
file(GLOB ATX_TSDB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(atx-tsdb STATIC ${ATX_TSDB_SOURCES})
add_library(atx::tsdb ALIAS atx-tsdb)

target_include_directories(atx-tsdb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-tsdb PUBLIC cxx_std_20)

target_link_libraries(atx-tsdb
    PUBLIC
        atx::core
    PRIVATE
        atx_warnings
)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

add_subdirectory(tools)
