#include "atx/ui/widgets.hpp"

#include <string>

#include "atx/ui/theme.hpp"

namespace atx::ui {

void section_header(std::string_view label, std::string_view detail) {
  ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
  ImGui::TextUnformatted(label.data(), label.data() + label.size());
  ImGui::PopStyleColor();
  if (!detail.empty()) {
    const float width = ImGui::CalcTextSize(detail.data(), detail.data() + detail.size()).x;
    ImGui::SameLine(ImGui::GetContentRegionMax().x - width);
    ImGui::TextUnformatted(detail.data(), detail.data() + detail.size());
  }
  ImGui::Separator();
}

void badge(std::string_view label, const ImVec4 &color) {
  const ImVec2 padding{6.0f, 2.0f};
  const ImVec2 text_size = ImGui::CalcTextSize(label.data(), label.data() + label.size());
  const ImVec2 pos = ImGui::GetCursorScreenPos();
  ImDrawList *draw = ImGui::GetWindowDrawList();
  const ImVec2 max{pos.x + text_size.x + padding.x * 2.0f, pos.y + text_size.y + padding.y * 2.0f};
  ImVec4 fill = color;
  fill.w = 0.13f;
  draw->AddRectFilled(pos, max, ImGui::ColorConvertFloat4ToU32(fill), 2.0f);
  draw->AddRect(pos, max, ImGui::ColorConvertFloat4ToU32(color), 2.0f);
  draw->AddText(ImVec2(pos.x + padding.x, pos.y + padding.y), ImGui::ColorConvertFloat4ToU32(color),
                label.data(), label.data() + label.size());
  ImGui::Dummy(ImVec2(max.x - pos.x, max.y - pos.y));
}

void metric_strip(std::string_view id, std::span<const Metric> metrics) {
  if (metrics.empty()) {
    return;
  }
  const std::string table_id{id};
  if (!ImGui::BeginTable(table_id.c_str(), static_cast<int>(metrics.size()),
                         ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
    return;
  }
  ImGui::TableNextRow();
  for (std::size_t i = 0; i < metrics.size(); ++i) {
    ImGui::TableSetColumnIndex(static_cast<int>(i));
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
    ImGui::TextUnformatted(metrics[i].label.data(),
                           metrics[i].label.data() + metrics[i].label.size());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, metrics[i].color);
    ImGui::TextUnformatted(metrics[i].value.data(),
                           metrics[i].value.data() + metrics[i].value.size());
    ImGui::PopStyleColor();
  }
  ImGui::EndTable();
}

void key_value(std::string_view key, std::string_view value, const ImVec4 *value_color) {
  ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
  ImGui::TextUnformatted(key.data(), key.data() + key.size());
  ImGui::PopStyleColor();
  ImGui::SameLine();
  const float width = ImGui::CalcTextSize(value.data(), value.data() + value.size()).x;
  ImGui::SameLine(ImGui::GetContentRegionMax().x - width);
  if (value_color != nullptr) {
    ImGui::PushStyleColor(ImGuiCol_Text, *value_color);
  }
  ImGui::TextUnformatted(value.data(), value.data() + value.size());
  if (value_color != nullptr) {
    ImGui::PopStyleColor();
  }
}

void vertical_gap(float pixels) { ImGui::Dummy(ImVec2(0.0f, pixels)); }

} // namespace atx::ui
