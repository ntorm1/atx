add_executable(build_universe build_universe.cpp)
target_link_libraries(build_universe PRIVATE atx::engine atx::core)

add_executable(build_ochain build_ochain.cpp)
target_link_libraries(build_ochain PRIVATE atx::engine atx::core)
