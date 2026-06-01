add_library(atx-engine STATIC
    src/engine.cpp
)
add_library(atx::engine ALIAS atx-engine)

target_include_directories(atx-engine
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-engine PUBLIC cxx_std_20)

# Engine builds on top of core.
target_link_libraries(atx-engine PUBLIC atx::core)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
