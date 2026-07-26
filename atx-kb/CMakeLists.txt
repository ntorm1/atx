file(GLOB ATX_KB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(atx-kb ${ATX_LIB_TYPE} ${ATX_KB_SOURCES})
add_library(atx::kb ALIAS atx-kb)

target_include_directories(atx-kb
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-kb PUBLIC cxx_std_20)
target_link_libraries(atx-kb PUBLIC atx::core PRIVATE atx_warnings)

add_executable(atx-kb-cli tools/main.cpp)
set_target_properties(atx-kb-cli PROPERTIES OUTPUT_NAME atx-kb)
target_link_libraries(atx-kb-cli PRIVATE atx::kb atx_warnings)

option(ATX_KB_BUILD_EVAL "Build the dependency-free atx-kb quality evaluator" ${ATX_BUILD_TESTS})
if(ATX_KB_BUILD_EVAL)
    add_subdirectory(eval)
endif()

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
