#pragma once

#include "imgui.h"

namespace atx::ui {

struct Palette {
  static constexpr ImVec4 Canvas{0.855f, 0.870f, 0.890f, 1.0f};
  static constexpr ImVec4 Panel{0.965f, 0.970f, 0.976f, 1.0f};
  static constexpr ImVec4 Raised{0.900f, 0.918f, 0.940f, 1.0f};
  static constexpr ImVec4 Border{0.560f, 0.600f, 0.650f, 1.0f};
  static constexpr ImVec4 Text{0.105f, 0.130f, 0.165f, 1.0f};
  static constexpr ImVec4 Muted{0.380f, 0.420f, 0.475f, 1.0f};
  static constexpr ImVec4 Cyan{0.000f, 0.390f, 0.680f, 1.0f};
  static constexpr ImVec4 Amber{0.875f, 0.470f, 0.020f, 1.0f};
  static constexpr ImVec4 Coral{0.800f, 0.160f, 0.190f, 1.0f};
  static constexpr ImVec4 Lime{0.170f, 0.500f, 0.240f, 1.0f};
};

void apply_atx_theme(float scale = 1.0f);
void apply_atx_plot_theme();

} // namespace atx::ui
