#pragma once

#include <span>
#include <string_view>

#include "imgui.h"

namespace atx::ui {

struct Metric {
  std::string_view label;
  std::string_view value;
  ImVec4 color;
};

void section_header(std::string_view label, std::string_view detail = {});
void badge(std::string_view label, const ImVec4 &color);
void metric_strip(std::string_view id, std::span<const Metric> metrics);
void key_value(std::string_view key, std::string_view value, const ImVec4 *value_color = nullptr);
void vertical_gap(float pixels = 3.0f);

} // namespace atx::ui
