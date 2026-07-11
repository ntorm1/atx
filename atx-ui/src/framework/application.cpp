#include "atx/ui/application.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>

#include <GLFW/glfw3.h>

#include "atx/ui/theme.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include "imgui_internal.h"
#include "implot.h"

namespace atx::ui {
namespace {

void glfw_error_callback(int error, const char *description) {
  std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

std::string window_name(const PanelSpec &panel) { return panel.title + "###" + panel.id; }

void install_fonts() {
#if defined(_WIN32)
  constexpr const char *kUiFont = "C:/Windows/Fonts/segoeui.ttf";
  if (std::filesystem::exists(kUiFont)) {
    ImGui::GetIO().Fonts->AddFontFromFileTTF(kUiFont, 13.5f);
  }
#endif
}

std::string layout_ini_path() {
#if defined(_WIN32)
  char *local_app_data = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&local_app_data, &length, "LOCALAPPDATA") == 0 && local_app_data != nullptr) {
    const std::filesystem::path directory = std::filesystem::path(local_app_data) / "ATX";
    std::free(local_app_data);
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (!error) {
      return (directory / "atx-ui.ini").string();
    }
  }
#endif
  return "atx-ui.ini";
}

} // namespace

Application::Application(ApplicationConfig config) : config_(std::move(config)) {}

void Application::add_panel(PanelSpec panel) { panels_.push_back(std::move(panel)); }

void Application::set_status_text(std::string text) { status_text_ = std::move(text); }

void Application::request_layout_reset() noexcept { reset_layout_ = true; }

int Application::run() {
  glfwSetErrorCallback(glfw_error_callback);
  if (glfwInit() == GLFW_FALSE) {
    return 1;
  }

  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if defined(__APPLE__)
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
  int window_width = config_.width;
  int window_height = config_.height;
  int work_x = 0;
  int work_y = 0;
  int work_width = config_.width;
  int work_height = config_.height;
  GLFWmonitor *monitor = glfwGetPrimaryMonitor();
  if (monitor != nullptr) {
    glfwGetMonitorWorkarea(monitor, &work_x, &work_y, &work_width, &work_height);
    window_width = std::min(window_width, std::max(320, work_width - 40));
    window_height = std::min(window_height, std::max(240, work_height - 60));
  }
  GLFWwindow *window =
      glfwCreateWindow(window_width, window_height, config_.title.c_str(), nullptr, nullptr);
  if (window == nullptr) {
    glfwTerminate();
    return 1;
  }
  if (monitor != nullptr) {
    glfwSetWindowPos(window, work_x + (work_width - window_width) / 2,
                     work_y + (work_height - window_height) / 2);
  }
  glfwMakeContextCurrent(window);
  glfwSwapInterval(config_.vsync ? 1 : 0);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  const std::string ini_path = layout_ini_path();
  io.IniFilename = ini_path.c_str();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  if (config_.enable_viewports) {
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
  }
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  io.ConfigDockingWithShift = false;
  install_fonts();
  apply_atx_theme();
  apply_atx_plot_theme();

  if (!ImGui_ImplGlfw_InitForOpenGL(window, true) || !ImGui_ImplOpenGL3_Init("#version 330")) {
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  bool running = true;
  std::size_t frame = 0;
  while (running && glfwWindowShouldClose(window) == GLFW_FALSE) {
    glfwPollEvents();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    render_shell(running);
    for (PanelSpec &panel : panels_) {
      if (!panel.open) {
        continue;
      }
      const std::string name = window_name(panel);
      if (ImGui::Begin(name.c_str(), &panel.open, ImGuiWindowFlags_NoCollapse)) {
        panel.render();
      }
      ImGui::End();
    }

    ImGui::Render();
    int display_w = 0;
    int display_h = 0;
    glfwGetFramebufferSize(window, &display_w, &display_h);
    glViewport(0, 0, display_w, display_h);
    glClearColor(Palette::Canvas.x, Palette::Canvas.y, Palette::Canvas.z, Palette::Canvas.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    if ((io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) != 0) {
      GLFWwindow *backup = glfwGetCurrentContext();
      ImGui::UpdatePlatformWindows();
      ImGui::RenderPlatformWindowsDefault();
      glfwMakeContextCurrent(backup);
    }
    glfwSwapBuffers(window);

    ++frame;
    if (config_.frame_limit > 0 && frame >= config_.frame_limit) {
      running = false;
    }
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImPlot::DestroyContext();
  ImGui::DestroyContext();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}

void Application::render_shell(bool &running) {
  ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(viewport->Pos);
  ImGui::SetNextWindowSize(viewport->Size);
  ImGui::SetNextWindowViewport(viewport->ID);

  constexpr ImGuiWindowFlags kFlags =
      ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoTitleBar |
      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::Begin("ATX Workspace###atx_workspace", nullptr, kFlags);
  ImGui::PopStyleVar(3);

  if (ImGui::BeginMenuBar()) {
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Cyan);
    ImGui::TextUnformatted("ATX / VOL");
    ImGui::PopStyleColor();
    ImGui::Separator();
    if (ImGui::BeginMenu("WORKSPACE")) {
      if (ImGui::MenuItem("Reset layout")) {
        reset_layout_ = true;
      }
      ImGui::Separator();
      for (PanelSpec &panel : panels_) {
        ImGui::MenuItem(panel.title.c_str(), nullptr, &panel.open);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("SYSTEM")) {
      if (ImGui::MenuItem("Quit", "Alt+F4")) {
        running = false;
      }
      ImGui::EndMenu();
    }

    const float status_width = ImGui::CalcTextSize(status_text_.c_str()).x + 12.0f;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowWidth() - status_width));
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Lime);
    ImGui::Text("%s", status_text_.c_str());
    ImGui::PopStyleColor();
    ImGui::EndMenuBar();
  }

  const std::string dockspace_name = "ATXDockspace###" + config_.layout_id;
  const ImGuiID dockspace_id = ImGui::GetID(dockspace_name.c_str());
  if (reset_layout_ || ImGui::DockBuilderGetNode(dockspace_id) == nullptr) {
    build_default_layout(dockspace_id);
    reset_layout_ = false;
  }
  ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
  ImGui::End();
}

void Application::build_default_layout(unsigned int dockspace_id) {
  const ImGuiID root = static_cast<ImGuiID>(dockspace_id);
  ImGui::DockBuilderRemoveNode(root);
  ImGui::DockBuilderAddNode(root, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(root, ImGui::GetMainViewport()->WorkSize);

  ImGuiID center = root;
  ImGuiID top = 0;
  ImGuiID left = 0;
  ImGuiID left_bottom = 0;
  ImGuiID right = 0;
  ImGuiID bottom = center;
  ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.15f, &top, &center);
  ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.29f, &left, &center);
  ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.42f, &left_bottom, &left);
  ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.18f, &right, &center);
  const bool has_bottom = std::any_of(panels_.begin(), panels_.end(), [](const PanelSpec &panel) {
    return panel.initial_dock == DockSlot::Bottom;
  });
  if (has_bottom) {
    ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.24f, &bottom, &center);
  }

  for (const PanelSpec &panel : panels_) {
    ImGuiID target = center;
    switch (panel.initial_dock) {
    case DockSlot::Top:
      target = top;
      break;
    case DockSlot::Left:
      target = left;
      break;
    case DockSlot::LeftBottom:
      target = left_bottom;
      break;
    case DockSlot::Center:
      target = center;
      break;
    case DockSlot::Right:
      target = right;
      break;
    case DockSlot::Bottom:
      target = bottom;
      break;
    }
    const std::string name = window_name(panel);
    ImGui::DockBuilderDockWindow(name.c_str(), target);
  }
  ImGui::DockBuilderFinish(root);
}

} // namespace atx::ui

