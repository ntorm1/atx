#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace atx::ui {

enum class DockSlot {
  Top,
  Left,
  LeftBottom,
  Center,
  Right,
  Bottom,
};

struct PanelSpec {
  std::string id;
  std::string title;
  DockSlot initial_dock{DockSlot::Center};
  std::function<void()> render;
  bool open{true};
};

struct ApplicationConfig {
  std::string title{"ATX"};
  // Bump this when the default panel topology changes. Persisted user layouts
  // for other workspaces remain untouched while the new schema is initialized.
  std::string layout_id{"default-v1"};
  int width{1520};
  int height{880};
  bool enable_viewports{false};
  bool vsync{true};
  std::size_t frame_limit{0};
};

// Owns the GLFW/OpenGL/ImGui lifecycle and a persistent dockable panel registry.
// Domain applications supply only panel render functions; frame, menu, docking,
// DPI/font setup, and clean shutdown remain shared infrastructure.
class Application {
public:
  explicit Application(ApplicationConfig config = {});

  Application(const Application &) = delete;
  Application &operator=(const Application &) = delete;

  void add_panel(PanelSpec panel);
  void set_status_text(std::string text);
  void request_layout_reset() noexcept;

  [[nodiscard]] int run();

private:
  void render_shell(bool &running);
  void build_default_layout(unsigned int dockspace_id);

  ApplicationConfig config_;
  std::vector<PanelSpec> panels_;
  std::string status_text_{"READY"};
  bool reset_layout_{false};
};

} // namespace atx::ui

