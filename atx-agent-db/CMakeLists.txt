file(GLOB ATX_DB_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")

add_library(atx-db ${ATX_LIB_TYPE} ${ATX_DB_SOURCES})
add_library(atx::agent-db ALIAS atx-db)

target_include_directories(atx-db
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_features(atx-db PUBLIC cxx_std_20)
target_link_libraries(atx-db PUBLIC atx::core atx::kb PRIVATE atx_warnings)

add_executable(atx-db-cli tools/main.cpp)
set_target_properties(atx-db-cli PROPERTIES OUTPUT_NAME atx-db)
target_link_libraries(atx-db-cli PRIVATE atx::agent-db atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
