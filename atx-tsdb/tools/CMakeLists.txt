add_executable(atx-tsdb-load load_main.cpp)
target_link_libraries(atx-tsdb-load PRIVATE atx::tsdb atx_warnings)

add_executable(atx-tsdb-stat stat_main.cpp)
target_link_libraries(atx-tsdb-stat PRIVATE atx::tsdb atx_warnings)
