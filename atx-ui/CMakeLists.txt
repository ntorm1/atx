# ---- atx-ui: reusable immediate-mode desktop application framework ----------

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glfw)

# Docking is maintained as a tagged branch by Dear ImGui. Pinning the release
# keeps worktree and CI builds reproducible while retaining DockBuilder support.
FetchContent_Declare(
    imgui_src
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.92.7-docking
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(imgui_src)

add_library(atx-ui-imgui STATIC
    ${imgui_src_SOURCE_DIR}/imgui.cpp
    ${imgui_src_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_src_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_src_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_src_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
target_include_directories(atx-ui-imgui SYSTEM PUBLIC
    ${imgui_src_SOURCE_DIR}
    ${imgui_src_SOURCE_DIR}/backends
)
target_link_libraries(atx-ui-imgui PUBLIC glfw)
target_compile_features(atx-ui-imgui PUBLIC cxx_std_20)

# ImPlot master is pinned to a known commit because the revived project has not
# cut a post-0.16 release compatible with Dear ImGui 1.92 yet.
FetchContent_Declare(
    implot_src
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        d65a2bef53d32502407de3a4be80f191e2f412d7
)
FetchContent_MakeAvailable(implot_src)

add_library(atx-ui-implot STATIC
    ${implot_src_SOURCE_DIR}/implot.cpp
    ${implot_src_SOURCE_DIR}/implot_items.cpp
)
target_include_directories(atx-ui-implot SYSTEM PUBLIC ${implot_src_SOURCE_DIR})
target_link_libraries(atx-ui-implot PUBLIC atx-ui-imgui)
target_compile_features(atx-ui-implot PUBLIC cxx_std_20)

find_package(OpenGL REQUIRED)

add_library(atx-ui-framework STATIC
    src/framework/application.cpp
    src/framework/theme.cpp
    src/framework/widgets.cpp
)
add_library(atx::ui-framework ALIAS atx-ui-framework)
target_include_directories(atx-ui-framework PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(atx-ui-framework
    PUBLIC atx-ui-imgui atx-ui-implot
    PRIVATE OpenGL::GL atx_warnings
)
target_compile_features(atx-ui-framework PUBLIC cxx_std_20)

add_library(atx-ui-vol STATIC
    src/vol/spy_opra_surface.cpp
    src/vol/vol_workspace.cpp
)
add_library(atx::ui-vol ALIAS atx-ui-vol)
target_include_directories(atx-ui-vol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(atx-ui-vol
    PUBLIC atx::ui-framework atx::vol
    PRIVATE atx_warnings
)
target_compile_features(atx-ui-vol PUBLIC cxx_std_20)

add_executable(atx-ui src/main.cpp)
target_link_libraries(atx-ui PRIVATE atx::ui-vol atx_warnings)
target_compile_features(atx-ui PRIVATE cxx_std_20)

if(ATX_BUILD_TESTS)
    add_subdirectory(tests)
endif()

