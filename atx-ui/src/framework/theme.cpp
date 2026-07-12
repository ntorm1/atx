#include "atx/ui/theme.hpp"

#include "implot.h"

namespace atx::ui {

void apply_atx_theme(float scale) {
  ImGuiStyle &style = ImGui::GetStyle();
  ImGui::StyleColorsLight(&style);

  style.WindowPadding = ImVec2(6.0f, 5.0f);
  style.FramePadding = ImVec2(5.0f, 2.0f);
  style.CellPadding = ImVec2(5.0f, 2.0f);
  style.ItemSpacing = ImVec2(5.0f, 3.0f);
  style.ItemInnerSpacing = ImVec2(4.0f, 2.0f);
  style.ScrollbarSize = 10.0f;
  style.GrabMinSize = 8.0f;
  style.WindowBorderSize = 1.0f;
  style.ChildBorderSize = 1.0f;
  style.PopupBorderSize = 1.0f;
  style.FrameBorderSize = 1.0f;
  style.TabBorderSize = 1.0f;
  style.WindowRounding = 0.0f;
  style.ChildRounding = 0.0f;
  style.FrameRounding = 1.0f;
  style.PopupRounding = 1.0f;
  style.ScrollbarRounding = 1.0f;
  style.GrabRounding = 1.0f;
  style.TabRounding = 0.0f;
  style.WindowMenuButtonPosition = ImGuiDir_None;
  style.SeparatorTextBorderSize = 1.0f;
  style.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
  style.ScaleAllSizes(scale);

  constexpr ImVec4 lavender{0.805f, 0.790f, 0.930f, 1.0f};
  constexpr ImVec4 lavender_hover{0.745f, 0.735f, 0.900f, 1.0f};
  ImVec4 *colors = style.Colors;
  colors[ImGuiCol_Text] = Palette::Text;
  colors[ImGuiCol_TextDisabled] = Palette::Muted;
  colors[ImGuiCol_WindowBg] = Palette::Panel;
  colors[ImGuiCol_ChildBg] = ImVec4(0.945f, 0.952f, 0.962f, 1.0f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.985f, 0.987f, 0.990f, 0.99f);
  colors[ImGuiCol_Border] = Palette::Border;
  colors[ImGuiCol_BorderShadow] = ImVec4(1.0f, 1.0f, 1.0f, 0.0f);
  colors[ImGuiCol_FrameBg] = ImVec4(0.985f, 0.987f, 0.990f, 1.0f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.900f, 0.925f, 0.955f, 1.0f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.835f, 0.885f, 0.935f, 1.0f);
  colors[ImGuiCol_TitleBg] = Palette::Raised;
  colors[ImGuiCol_TitleBgActive] = ImVec4(0.850f, 0.875f, 0.915f, 1.0f);
  colors[ImGuiCol_TitleBgCollapsed] = Palette::Raised;
  colors[ImGuiCol_MenuBarBg] = lavender;
  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.910f, 0.920f, 0.935f, 1.0f);
  colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.650f, 0.690f, 0.740f, 1.0f);
  colors[ImGuiCol_ScrollbarGrabHovered] = Palette::Muted;
  colors[ImGuiCol_ScrollbarGrabActive] = Palette::Cyan;
  colors[ImGuiCol_CheckMark] = Palette::Cyan;
  colors[ImGuiCol_SliderGrab] = Palette::Cyan;
  colors[ImGuiCol_SliderGrabActive] = Palette::Lime;
  colors[ImGuiCol_Button] = Palette::Raised;
  colors[ImGuiCol_ButtonHovered] = lavender;
  colors[ImGuiCol_ButtonActive] = lavender_hover;
  colors[ImGuiCol_Header] = ImVec4(0.835f, 0.885f, 0.930f, 1.0f);
  colors[ImGuiCol_HeaderHovered] = ImVec4(0.765f, 0.845f, 0.920f, 1.0f);
  colors[ImGuiCol_HeaderActive] = ImVec4(0.690f, 0.800f, 0.900f, 1.0f);
  colors[ImGuiCol_Separator] = Palette::Border;
  colors[ImGuiCol_SeparatorHovered] = Palette::Cyan;
  colors[ImGuiCol_SeparatorActive] = Palette::Cyan;
  colors[ImGuiCol_ResizeGrip] = ImVec4(0.0f, 0.39f, 0.68f, 0.12f);
  colors[ImGuiCol_ResizeGripHovered] = ImVec4(0.0f, 0.39f, 0.68f, 0.45f);
  colors[ImGuiCol_ResizeGripActive] = Palette::Cyan;
  colors[ImGuiCol_Tab] = ImVec4(0.885f, 0.900f, 0.925f, 1.0f);
  colors[ImGuiCol_TabHovered] = lavender;
  colors[ImGuiCol_TabSelected] = Palette::Panel;
  colors[ImGuiCol_TabSelectedOverline] = Palette::Cyan;
  colors[ImGuiCol_TabDimmed] = Palette::Raised;
  colors[ImGuiCol_TabDimmedSelected] = ImVec4(0.910f, 0.920f, 0.935f, 1.0f);
  colors[ImGuiCol_DockingPreview] = ImVec4(0.0f, 0.39f, 0.68f, 0.40f);
  colors[ImGuiCol_DockingEmptyBg] = Palette::Canvas;
  colors[ImGuiCol_PlotLines] = Palette::Cyan;
  colors[ImGuiCol_PlotLinesHovered] = Palette::Amber;
  colors[ImGuiCol_PlotHistogram] = Palette::Amber;
  colors[ImGuiCol_PlotHistogramHovered] = Palette::Coral;
  colors[ImGuiCol_TableHeaderBg] = ImVec4(0.855f, 0.875f, 0.905f, 1.0f);
  colors[ImGuiCol_TableBorderStrong] = Palette::Border;
  colors[ImGuiCol_TableBorderLight] = ImVec4(0.720f, 0.750f, 0.790f, 1.0f);
  colors[ImGuiCol_TableRowBg] = ImVec4(0.985f, 0.987f, 0.990f, 1.0f);
  colors[ImGuiCol_TableRowBgAlt] = ImVec4(0.925f, 0.940f, 0.955f, 1.0f);
  colors[ImGuiCol_TextSelectedBg] = ImVec4(0.30f, 0.60f, 0.88f, 0.30f);
  colors[ImGuiCol_NavCursor] = Palette::Cyan;
}

void apply_atx_plot_theme() {
  ImPlotStyle &style = ImPlot::GetStyle();
  style.PlotBorderSize = 1.0f;
  style.MinorAlpha = 0.32f;
  style.MajorTickLen = ImVec2(0.0f, 5.0f);
  style.MinorTickLen = ImVec2(0.0f, 3.0f);
  style.MajorTickSize = ImVec2(1.0f, 1.0f);
  style.MinorTickSize = ImVec2(1.0f, 1.0f);
  style.MajorGridSize = ImVec2(1.0f, 1.0f);
  style.MinorGridSize = ImVec2(1.0f, 1.0f);
  style.PlotPadding = ImVec2(7.0f, 6.0f);
  style.LabelPadding = ImVec2(4.0f, 4.0f);
  style.LegendPadding = ImVec2(6.0f, 3.0f);
  style.LegendInnerPadding = ImVec2(4.0f, 2.0f);
  style.LegendSpacing = ImVec2(7.0f, 1.0f);
  style.MousePosPadding = ImVec2(6.0f, 3.0f);
  style.PlotDefaultSize = ImVec2(400.0f, 300.0f);
  style.PlotMinSize = ImVec2(220.0f, 160.0f);

  style.Colors[ImPlotCol_PlotBg] = ImVec4(0.985f, 0.987f, 0.990f, 1.0f);
  style.Colors[ImPlotCol_PlotBorder] = Palette::Border;
  style.Colors[ImPlotCol_LegendBg] = ImVec4(0.960f, 0.965f, 0.975f, 0.94f);
  style.Colors[ImPlotCol_LegendBorder] = Palette::Border;
  style.Colors[ImPlotCol_LegendText] = Palette::Text;
  style.Colors[ImPlotCol_AxisText] = Palette::Muted;
  style.Colors[ImPlotCol_AxisGrid] = ImVec4(0.42f, 0.46f, 0.52f, 0.34f);
  style.Colors[ImPlotCol_AxisTick] = Palette::Border;
  style.Colors[ImPlotCol_AxisBg] = ImVec4(0.960f, 0.965f, 0.975f, 1.0f);
  style.Colors[ImPlotCol_AxisBgHovered] = Palette::Raised;
  style.Colors[ImPlotCol_AxisBgActive] = Palette::Raised;
  style.Colors[ImPlotCol_Selection] = Palette::Cyan;
  style.Colors[ImPlotCol_Crosshairs] = Palette::Muted;
}

} // namespace atx::ui
