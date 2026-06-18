# atx-impl: end-to-end equity alpha pipeline reference binary.
# Split: a STATIC lib holds all logic so the test exe can link it (you cannot link
# an executable target). The atx-impl exe is just main.cpp over the lib.
add_library(atx-impl-core STATIC
    src/config.cpp
    src/dispatch.cpp
    src/artifacts.cpp
    src/serialize_panel.cpp
    src/stage_load.cpp
    src/stage_panel.cpp
    src/stage_discover.cpp
    src/stage_combine.cpp
    src/stage_optimize.cpp
    src/stage_report.cpp
    src/stage_run.cpp)
target_include_directories(atx-impl-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(atx-impl-core PUBLIC cxx_std_20)
target_link_libraries(atx-impl-core
    PUBLIC  atx::core atx::engine atx::tsdb
    PRIVATE atx_warnings)
find_package(Threads REQUIRED)
target_link_libraries(atx-impl-core PRIVATE Threads::Threads)

add_executable(atx-impl src/main.cpp)
target_link_libraries(atx-impl PRIVATE atx-impl-core atx_warnings)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()
