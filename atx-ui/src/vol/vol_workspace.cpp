#include "atx/ui/vol_workspace.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "atx/ui/theme.hpp"
#include "atx/ui/vol_workspace_model.hpp"
#include "atx/ui/widgets.hpp"
#include "imgui.h"
#include "implot.h"

namespace atx::ui {
namespace {

std::string number(double value, int precision = 2) {
  std::array<char, 64> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.*f", precision, value);
  return buffer.data();
}

std::string percent(double value, int precision = 2) {
  return number(value * 100.0, precision) + "%";
}

std::string count(std::size_t value) { return std::to_string(value); }

std::string short_date(const std::string &iso) { return iso.size() >= 10 ? iso.substr(5, 5) : iso; }

void render_error(const VolSurfaceSource &source) {
  badge("DATA ERROR", Palette::Coral);
  vertical_gap();
  ImGui::PushStyleColor(ImGuiCol_Text, Palette::Coral);
  ImGui::TextWrapped("%s", source.error().c_str());
  ImGui::PopStyleColor();
  vertical_gap(8.0f);
  section_header("SOURCE");
  ImGui::TextWrapped("%s", source.source_info().path.c_str());
}

void metric_cell(const char *label, const std::string &value, const ImVec4 &color) {
  ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, color);
  ImGui::TextUnformatted(value.c_str());
  ImGui::PopStyleColor();
}

} // namespace

VolWorkspace::VolWorkspace(VolSurfaceSource &source) : source_(source) {}

void VolWorkspace::select_expiry(std::size_t index) {
  if (index == source_.selected_expiry()) {
    return;
  }
  if (source_.select_expiry(index)) {
    state_.request_plot_fit();
  }
}

void VolWorkspace::render_market_strip() {
  if (!source_.ready()) {
    render_error(source_);
    return;
  }

  const VolCurveSlice &slice = source_.slice();
  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame;
  if (ImGui::BeginTable("symbol_summary", 6, kFlags)) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::PushStyleColor(ImGuiCol_Text, Palette::Text);
    ImGui::SetWindowFontScale(1.28f);
    ImGui::TextUnformatted(slice.symbol.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    badge("OPRA", Palette::Cyan);

    ImGui::TableSetColumnIndex(1);
    metric_cell("SPOT", number(slice.spot, 2), Palette::Text);
    ImGui::TableSetColumnIndex(2);
    metric_cell("MODEL", "HFT / LIN VAR", Palette::Cyan);
    ImGui::TableSetColumnIndex(3);
    metric_cell("SELECTED EXPIRY", slice.expiry_iso, Palette::Text);
    ImGui::TableSetColumnIndex(4);
    metric_cell("ATM VOL", percent(slice.atm_vol, 2), Palette::Amber);
    ImGui::TableSetColumnIndex(5);
    metric_cell("IN BID / ASK", percent(slice.fraction_in_band, 1),
                slice.fraction_in_band >= 0.8 ? Palette::Lime : Palette::Amber);
    ImGui::EndTable();
  }

  ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
  ImGui::TextUnformatted("EXPIRATION STRIP");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextDisabled("%zu listed / %s snapshot", source_.expiries().size(),
                      slice.snapshot_iso.c_str());

  const float strip_height = std::max(36.0f, ImGui::GetContentRegionAvail().y);
  ImGui::BeginChild("expiry_strip", ImVec2(0.0f, strip_height), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  for (std::size_t i = 0; i < source_.expiries().size(); ++i) {
    const ExpiryInfo &expiry = source_.expiries()[i];
    ImGui::PushID(static_cast<int>(i));
    const bool selected = i == source_.selected_expiry();
    if (selected) {
      ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.72f, 0.86f, 0.74f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.66f, 0.82f, 0.69f, 1.0f));
      ImGui::PushStyleColor(ImGuiCol_Text, Palette::Text);
    }
    const std::string label = short_date(expiry.iso_date) + "  " +
                              number(expiry.years * 365.25, 0) + "D\n" +
                              percent(expiry.atm_vol, 1) + " / " + count(expiry.strike_count);
    if (ImGui::Button(label.c_str(), ImVec2(94.0f, 36.0f))) {
      select_expiry(i);
    }
    if (selected && ImGui::IsWindowAppearing()) {
      ImGui::SetScrollHereX(0.5f);
    }
    if (selected) {
      ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    if (i + 1 < source_.expiries().size()) {
      ImGui::SameLine();
    }
  }
  ImGui::EndChild();
}

void VolWorkspace::render_market_panel() {
  if (!source_.ready()) {
    render_error(source_);
    return;
  }

  const VolCurveSlice &slice = source_.slice();
  section_header("CURVE VIEW", slice.expiry_iso);
  ImGui::SetNextItemWidth(-1.0f);
  if (ImGui::BeginCombo("##control_expiry", slice.expiry_iso.c_str())) {
    for (std::size_t i = 0; i < source_.expiries().size(); ++i) {
      const bool selected = i == source_.selected_expiry();
      const ExpiryInfo &expiry = source_.expiries()[i];
      const std::string label = expiry.iso_date + "  /  " + number(expiry.years * 365.25, 0) + "D";
      if (ImGui::Selectable(label.c_str(), selected)) {
        select_expiry(i);
      }
      if (selected) {
        ImGui::SetItemDefaultFocus();
      }
    }
    ImGui::EndCombo();
  }

  vertical_gap(5.0f);
  section_header("OBSERVATIONS");
  ImGui::Checkbox("Current NBBO", &state_.show_market_band);
  ImGui::SameLine();
  ImGui::Checkbox("Mid marks", &state_.show_market_mid);
  ImGui::Checkbox("Calls", &state_.show_calls);
  ImGui::SameLine();
  ImGui::Checkbox("Puts", &state_.show_puts);
  bool previous_unavailable = false;
  ImGui::BeginDisabled();
  ImGui::Checkbox("Previous snapshot", &previous_unavailable);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::SetTooltip("No historical snapshot is loaded in this workspace.");
  }

  vertical_gap(5.0f);
  section_header("THEORETICAL");
  ImGui::Checkbox("Admitted risk surface", &state_.show_model);
  ImGui::SameLine();
  badge("RISK", Palette::Lime);
  ImGui::Checkbox("Market mark overlay", &state_.show_market_mark);

  vertical_gap(5.0f);
  section_header("FIT QUALITY", source_.diagnostics().quality_mode);
  const auto quality_button = [&](const char *label, UiFitQualityMode mode) {
    const bool selected = source_.quality_mode() == mode;
    if (selected) {
      ImGui::BeginDisabled();
    }
    const bool clicked = ImGui::SmallButton(label);
    if (selected) {
      ImGui::EndDisabled();
    }
    if (clicked && source_.set_quality_mode(mode)) {
      state_.request_plot_fit();
    }
  };
  quality_button("LATENCY", UiFitQualityMode::Latency);
  ImGui::SameLine();
  quality_button("BALANCED", UiFitQualityMode::Balanced);
  ImGui::SameLine();
  quality_button("ACCURACY", UiFitQualityMode::Accuracy);

  vertical_gap(5.0f);
  section_header("X AXIS");
  if (ImGui::RadioButton("Normalized strike  z", state_.x_axis == VolXAxisMode::NormalizedStrike)) {
    state_.x_axis = VolXAxisMode::NormalizedStrike;
    state_.request_plot_fit();
  }
  if (ImGui::RadioButton("Absolute strike  K", state_.x_axis == VolXAxisMode::Strike)) {
    state_.x_axis = VolXAxisMode::Strike;
    state_.request_plot_fit();
  }
  ImGui::BeginDisabled(state_.x_axis != VolXAxisMode::NormalizedStrike);
  if (ImGui::SmallButton("ATM +/-1")) {
    state_.set_normalized_window(1.0);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("ATM +/-2")) {
    state_.set_normalized_window(2.0);
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("ALL")) {
    state_.set_normalized_window(2.4);
  }
  ImGui::EndDisabled();

  vertical_gap(5.0f);
  section_header("Y AXIS");
  if (ImGui::RadioButton("Implied volatility", state_.y_axis == VolYAxisMode::ImpliedVol)) {
    state_.y_axis = VolYAxisMode::ImpliedVol;
    state_.request_plot_fit();
  }
  if (ImGui::RadioButton("Total variance", state_.y_axis == VolYAxisMode::TotalVariance)) {
    state_.y_axis = VolYAxisMode::TotalVariance;
    state_.request_plot_fit();
  }

  vertical_gap(7.0f);
  if (ImGui::Button("FIT VIEW", ImVec2(-1.0f, 0.0f))) {
    state_.request_plot_fit();
  }
}

void VolWorkspace::render_curve_panel() {
  if (!source_.ready()) {
    render_error(source_);
    return;
  }

  const VolCurveSlice &slice = source_.slice();
  ImGui::PushStyleColor(ImGuiCol_Text, Palette::Text);
  ImGui::Text("%s  /  %s", slice.symbol.c_str(), slice.expiry_iso.c_str());
  ImGui::PopStyleColor();
  ImGui::SameLine();
  badge(slice.model_name, Palette::Lime);
  ImGui::SameLine();
  ImGui::TextDisabled("F %.2f   T %.0fD   ATM %.2f%%", slice.forward, slice.years * 365.25,
                      slice.atm_vol * 100.0);

  const bool total_variance = state_.y_axis == VolYAxisMode::TotalVariance;
  std::vector<double> model_x;
  std::vector<double> model_y;
  model_x.reserve(slice.curve.size());
  model_y.reserve(slice.curve.size());
  for (const VolCurvePoint &point : slice.curve) {
    model_x.push_back(state_.plot_x(point.z, point.strike));
    model_y.push_back(state_.plot_y(point.model_iv, slice.years));
  }
  std::vector<double> mark_x;
  std::vector<double> mark_y;
  mark_x.reserve(slice.market_mark_curve.size());
  mark_y.reserve(slice.market_mark_curve.size());
  for (const VolCurvePoint &point : slice.market_mark_curve) {
    mark_x.push_back(state_.plot_x(point.z, point.strike));
    mark_y.push_back(state_.plot_y(point.model_iv, slice.years));
  }

  struct QuoteSeries {
    std::vector<double> x;
    std::vector<double> mid;
    std::vector<double> neg;
    std::vector<double> pos;
  } calls, puts;
  calls.x.reserve(slice.quotes.size());
  calls.mid.reserve(slice.quotes.size());
  calls.neg.reserve(slice.quotes.size());
  calls.pos.reserve(slice.quotes.size());
  puts.x.reserve(slice.quotes.size());
  puts.mid.reserve(slice.quotes.size());
  puts.neg.reserve(slice.quotes.size());
  puts.pos.reserve(slice.quotes.size());
  for (const VolQuotePoint &quote : slice.quotes) {
    if (!state_.quote_visible(quote.side)) {
      continue;
    }
    QuoteSeries &series = quote.side == 'C' ? calls : puts;
    const double bid = state_.plot_y(quote.bid_iv, slice.years);
    const double mid = state_.plot_y(quote.mid_iv, slice.years);
    const double ask = state_.plot_y(quote.ask_iv, slice.years);
    series.x.push_back(state_.plot_x(quote.z, quote.strike));
    series.mid.push_back(mid);
    series.neg.push_back(mid - bid);
    series.pos.push_back(ask - mid);
  }

  if (state_.fit_plot_next) {
    if (state_.x_axis == VolXAxisMode::NormalizedStrike) {
      ImPlot::SetNextAxisLimits(ImAxis_X1, -state_.normalized_window, state_.normalized_window,
                                ImPlotCond_Always);
      ImPlot::SetNextAxisToFit(ImAxis_Y1);
    } else {
      ImPlot::SetNextAxesToFit();
    }
    state_.fit_plot_next = false;
  }

  const char *x_label =
      state_.x_axis == VolXAxisMode::NormalizedStrike ? "NORMALIZED STRIKE  z" : "STRIKE  K";
  const char *y_label = total_variance ? "TOTAL VARIANCE" : "IMPLIED VOLATILITY";
  if (ImPlot::BeginPlot("##served_curve", ImVec2(-1.0f, -1.0f),
                        ImPlotFlags_NoTitle | ImPlotFlags_Crosshairs)) {
    ImPlot::SetupAxes(x_label, y_label, ImPlotAxisFlags_NoHighlight, ImPlotAxisFlags_NoHighlight);
    ImPlot::SetupAxisFormat(ImAxis_X1,
                            state_.x_axis == VolXAxisMode::NormalizedStrike ? "%.2f" : "%.0f");
    ImPlot::SetupAxisFormat(ImAxis_Y1, total_variance ? "%.4f" : "%.3f");
    ImPlot::SetupLegend(ImPlotLocation_NorthEast, ImPlotLegendFlags_Horizontal);

    if (state_.show_market_band) {
      const auto plot_errors = [](const char *id, const QuoteSeries &series, const ImVec4 &color) {
        if (series.x.empty()) {
          return;
        }
        ImPlotSpec spec;
        spec.LineColor = color;
        spec.LineWeight = 1.0f;
        spec.Size = 3.0f;
        spec.Flags = ImPlotItemFlags_NoLegend;
        ImPlot::PlotErrorBars(id, series.x.data(), series.mid.data(), series.neg.data(),
                              series.pos.data(), static_cast<int>(series.x.size()), spec);
      };
      plot_errors("##call_nbbo", calls, Palette::Cyan);
      plot_errors("##put_nbbo", puts, Palette::Coral);
    }

    if (state_.show_market_mid) {
      const auto plot_mids = [](const char *label, const QuoteSeries &series, const ImVec4 &color,
                                ImPlotMarker marker) {
        if (series.x.empty()) {
          return;
        }
        ImPlotSpec spec;
        spec.LineColor = color;
        spec.Marker = marker;
        spec.MarkerSize = 3.4f;
        spec.MarkerFillColor = color;
        spec.MarkerLineColor = color;
        ImPlot::PlotScatter(label, series.x.data(), series.mid.data(),
                            static_cast<int>(series.x.size()), spec);
      };
      plot_mids("CALL MID", calls, Palette::Cyan, ImPlotMarker_Up);
      plot_mids("PUT MID", puts, Palette::Coral, ImPlotMarker_Down);
    }

    if (state_.show_model && !model_x.empty()) {
      ImPlotSpec model_spec;
      model_spec.LineColor = Palette::Text;
      model_spec.LineWeight = 2.0f;
      ImPlot::PlotLine("RISK", model_x.data(), model_y.data(),
                       static_cast<int>(model_x.size()), model_spec);
    }
    if (state_.show_market_mark && !mark_x.empty()) {
      ImPlotSpec mark_spec;
      mark_spec.LineColor = Palette::Amber;
      mark_spec.LineWeight = 1.0f;
      ImPlot::PlotLine("MARK", mark_x.data(), mark_y.data(), static_cast<int>(mark_x.size()),
                       mark_spec);
    }

    const double atm = state_.x_axis == VolXAxisMode::NormalizedStrike ? 0.0 : slice.forward;
    ImPlotSpec atm_spec;
    atm_spec.LineColor = Palette::Border;
    atm_spec.LineWeight = 1.0f;
    atm_spec.Flags = ImPlotItemFlags_NoLegend;
    ImPlot::PlotInfLines("##atm", &atm, 1, atm_spec);

    if (ImPlot::IsPlotHovered() && !slice.quotes.empty()) {
      const double mouse_x = ImPlot::GetPlotMousePos().x;
      const auto nearest =
          std::min_element(slice.quotes.begin(), slice.quotes.end(),
                           [&](const VolQuotePoint &lhs, const VolQuotePoint &rhs) {
                             const double lhs_x = state_.plot_x(lhs.z, lhs.strike);
                             const double rhs_x = state_.plot_x(rhs.z, rhs.strike);
                             return std::fabs(lhs_x - mouse_x) < std::fabs(rhs_x - mouse_x);
                           });
      ImGui::BeginTooltip();
      ImGui::Text("%c  K %.2f  z %+.3f", nearest->side, nearest->strike, nearest->z);
      ImGui::Separator();
      ImGui::TextColored(Palette::Cyan, "bid     %.3f", nearest->bid_iv);
      ImGui::Text("mid     %.3f", nearest->mid_iv);
      ImGui::TextColored(Palette::Coral, "ask     %.3f", nearest->ask_iv);
      ImGui::TextColored(Palette::Text, "model   %.3f", nearest->model_iv);
      ImGui::EndTooltip();
    }
    ImPlot::EndPlot();
  }
}

void VolWorkspace::render_fit_panel() {
  if (!source_.ready()) {
    render_error(source_);
    return;
  }
  const VolCurveSlice &slice = source_.slice();
  const SurfaceDiagnostics &diag = source_.diagnostics();

  section_header("SURFACE BUNDLE", slice.symbol);
  badge(diag.risk_state, diag.using_fallback ? Palette::Amber : Palette::Lime);
  ImGui::SameLine();
  badge(diag.quality_mode, Palette::Cyan);
  vertical_gap(7.0f);
  key_value("COLD FIT", number(source_.fit_milliseconds(), 1) + " ms");
  key_value("VALIDATION", number(diag.validation_milliseconds, 2) + " ms");
  key_value("RISK MODEL", diag.risk_model);
  key_value("MARK MODEL", diag.mark_model);
  key_value("GENERATION", count(diag.served_generation));
  key_value("SURFACE SLICES", count(diag.fitted_slices));
  key_value("FIT QUOTES", count(diag.fitted_quotes));
  key_value("CALENDAR", diag.calendar_arb_free ? "ADMITTED" : "FAILED",
            diag.calendar_arb_free ? &Palette::Lime : &Palette::Coral);
  key_value("BUTTERFLY VIOLATIONS", count(diag.butterfly_violations),
            diag.butterfly_violations == 0 ? &Palette::Lime : &Palette::Coral);
  key_value("CARRY", diag.carry_confident ? "CONFIDENT" : "DEGRADED",
            diag.carry_confident ? &Palette::Lime : &Palette::Amber);
  key_value("IV INVERSION", diag.inversion_certified ? "CERTIFIED" : "DEGRADED",
            diag.inversion_certified ? &Palette::Lime : &Palette::Amber);
  key_value("IV FALLBACKS", count(diag.inversion_fallbacks));

  vertical_gap(9.0f);
  section_header("SELECTED SLICE", slice.expiry_iso);
  key_value("IN BID / ASK", percent(slice.fraction_in_band, 1),
            slice.fraction_in_band >= 0.8 ? &Palette::Lime : &Palette::Amber);
  key_value("IV RMSE", number(slice.rmse_iv * 10000.0, 1) + " bp");
  key_value("MAX IV ERROR", number(slice.max_abs_error * 10000.0, 1) + " bp");
  key_value("FORWARD", number(slice.forward, 3));
  key_value("EFFECTIVE CARRY", percent(slice.carry, 3));
  key_value("ATM VOL", percent(slice.atm_vol, 3), &Palette::Amber);
  key_value("OBSERVATIONS", count(slice.observations));

  vertical_gap(9.0f);
  section_header("PIPELINE");
  key_value("SOURCE", source_.source_info().provider);
  key_value("MARKS", source_.source_info().feed);
  key_value("AMERICAN IV", "CACHE ASSISTED");
  key_value("CONTRACTS", count(source_.contract_count()));
  key_value("DROPPED", count(source_.dropped_count()));
  ImGui::PushStyleColor(ImGuiCol_Text, Palette::Muted);
  ImGui::TextWrapped("%s", source_.source_info().path.c_str());
  ImGui::PopStyleColor();
}

void VolWorkspace::render_term_panel() {
  if (!source_.ready()) {
    render_error(source_);
    return;
  }
  const std::vector<ExpiryInfo> &expiries = source_.expiries();
  if (expiries.empty()) {
    return;
  }

  std::vector<double> days;
  std::vector<double> atm_vols;
  std::vector<double> total_variances;
  std::vector<double> forward_variances;
  days.reserve(expiries.size());
  atm_vols.reserve(expiries.size());
  total_variances.reserve(expiries.size());
  forward_variances.reserve(expiries.size());
  for (const ExpiryInfo &expiry : expiries) {
    days.push_back(expiry.years * 365.25);
    atm_vols.push_back(expiry.atm_vol * 100.0);
    total_variances.push_back(expiry.total_variance);
    forward_variances.push_back(expiry.forward_variance);
  }

  const ExpiryInfo &selected = expiries[source_.selected_expiry()];
  section_header("MODEL TERM STRUCTURE", source_.slice().snapshot_iso);
  ImGui::TextDisabled("%zu expiries", expiries.size());
  ImGui::SameLine();
  ImGui::Text("SELECTED %s  %.0fD  ATM %.2f%%  FWD %.2f", selected.iso_date.c_str(),
              selected.years * 365.25, selected.atm_vol * 100.0, selected.forward);

  if (!ImPlot::BeginPlot("##atm_term", ImVec2(-1.0f, -1.0f),
                         ImPlotFlags_NoTitle | ImPlotFlags_Crosshairs)) {
    return;
  }
  ImPlot::SetupAxis(ImAxis_X1, "DAYS TO EXPIRY", ImPlotAxisFlags_NoHighlight);
  ImPlot::SetupAxis(ImAxis_Y1, "ATM IMPLIED VOL", ImPlotAxisFlags_NoHighlight);
  ImPlot::SetupAxis(ImAxis_Y2, "TOTAL / FORWARD VARIANCE",
                    ImPlotAxisFlags_AuxDefault | ImPlotAxisFlags_NoHighlight);
  ImPlot::SetupAxisFormat(ImAxis_X1, "%.0fD");
  ImPlot::SetupAxisFormat(ImAxis_Y1, "%.1f%%");
  ImPlot::SetupAxisFormat(ImAxis_Y2, "%.4f");
  ImPlot::SetupLegend(ImPlotLocation_NorthEast, ImPlotLegendFlags_Horizontal);

  ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
  ImPlotSpec vol_spec;
  vol_spec.LineColor = Palette::Cyan;
  vol_spec.LineWeight = 2.0f;
  vol_spec.Marker = ImPlotMarker_Circle;
  vol_spec.MarkerSize = 4.0f;
  ImPlot::PlotLine("ATM VOL", days.data(), atm_vols.data(), static_cast<int>(days.size()),
                   vol_spec);

  ImPlot::SetAxes(ImAxis_X1, ImAxis_Y2);
  ImPlotSpec total_var_spec;
  total_var_spec.LineColor = Palette::Amber;
  total_var_spec.LineWeight = 1.5f;
  ImPlot::PlotLine("TOTAL VAR", days.data(), total_variances.data(),
                   static_cast<int>(days.size()), total_var_spec);
  ImPlotSpec forward_var_spec;
  forward_var_spec.LineColor = Palette::Coral;
  forward_var_spec.LineWeight = 1.0f;
  ImPlot::PlotLine("FORWARD VAR", days.data(), forward_variances.data(),
                   static_cast<int>(days.size()), forward_var_spec);

  ImPlot::SetAxes(ImAxis_X1, ImAxis_Y1);
  const double selected_day = selected.years * 365.25;
  ImPlotSpec selected_spec;
  selected_spec.LineColor = Palette::Lime;
  selected_spec.LineWeight = 1.0f;
  selected_spec.Flags = ImPlotItemFlags_NoLegend;
  ImPlot::PlotInfLines("##selected_expiry", &selected_day, 1, selected_spec);

  if (ImPlot::IsPlotHovered()) {
    const double mouse_day = ImPlot::GetPlotMousePos(ImAxis_X1, ImAxis_Y1).x;
    const std::optional<std::size_t> nearest_index = nearest_expiry_by_days(expiries, mouse_day);
    if (nearest_index.has_value()) {
      const ExpiryInfo &nearest = expiries[*nearest_index];
      ImGui::BeginTooltip();
      ImGui::Text("%s / %.0fD", nearest.iso_date.c_str(), nearest.years * 365.25);
      ImGui::Separator();
      ImGui::TextColored(Palette::Cyan, "ATM VOL  %.2f%%", nearest.atm_vol * 100.0);
      ImGui::TextColored(Palette::Amber, "FORWARD  %.3f", nearest.forward);
      ImGui::Text("TOTAL VAR %.6f", nearest.total_variance);
      ImGui::TextColored(Palette::Coral, "FWD VAR   %.6f", nearest.forward_variance);
      ImGui::Text("CARRY    %.3f%%", nearest.carry * 100.0);
      ImGui::Text("STRIKES  %zu", nearest.strike_count);
      ImGui::TextDisabled("Double-click to select");
      ImGui::EndTooltip();
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        select_expiry(*nearest_index);
      }
    }
  }
  ImPlot::EndPlot();
}

void VolWorkspace::render_quote_panel() {
  if (!source_.ready()) {
    render_error(source_);
    return;
  }
  const VolCurveSlice &slice = source_.slice();
  std::vector<std::size_t> visible;
  visible.reserve(slice.quotes.size());
  for (std::size_t i = 0; i < slice.quotes.size(); ++i) {
    if (state_.quote_visible(slice.quotes[i].side)) {
      visible.push_back(i);
    }
  }
  section_header("OTM STRIKE LADDER", count(visible.size()) + " clean marks");

  constexpr ImGuiTableFlags kFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                     ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY |
                                     ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable |
                                     ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;
  if (!ImGui::BeginTable("quote_ladder", 14, kFlags, ImVec2(0.0f, -1.0f), 1180.0f)) {
    return;
  }
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("STRIKE", ImGuiTableColumnFlags_WidthFixed, 64.0f);
  ImGui::TableSetupColumn("SIDE", ImGuiTableColumnFlags_WidthFixed, 38.0f);
  ImGui::TableSetupColumn("BID PX");
  ImGui::TableSetupColumn("ASK PX");
  ImGui::TableSetupColumn("BID IV");
  ImGui::TableSetupColumn("ASK IV");
  ImGui::TableSetupColumn("THEO PX");
  ImGui::TableSetupColumn("EDGE PX");
  ImGui::TableSetupColumn("IV EDGE");
  ImGui::TableSetupColumn("DELTA");
  ImGui::TableSetupColumn("GAMMA");
  ImGui::TableSetupColumn("THETA/D");
  ImGui::TableSetupColumn("VEGA");
  ImGui::TableSetupColumn("BAND", ImGuiTableColumnFlags_WidthFixed, 42.0f);
  ImGui::TableHeadersRow();

  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(visible.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const VolQuotePoint &quote = slice.quotes[visible[static_cast<std::size_t>(row)]];
      const QuoteEdgeMetrics edge = quote_edge_metrics(quote);
      const ImU32 tint =
          ImGui::ColorConvertFloat4ToU32(quote.side == 'C' ? ImVec4(0.88f, 0.95f, 0.99f, 0.55f)
                                                           : ImVec4(0.99f, 0.90f, 0.91f, 0.55f));
      ImGui::TableNextRow();
      ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, tint);
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%.2f", quote.strike);
      ImGui::TableSetColumnIndex(1);
      ImGui::TextColored(quote.side == 'C' ? Palette::Cyan : Palette::Coral, "%c", quote.side);
      ImGui::TableSetColumnIndex(2);
      ImGui::TextColored(Palette::Cyan, "%.2f", quote.bid_price);
      ImGui::TableSetColumnIndex(3);
      ImGui::TextColored(Palette::Coral, "%.2f", quote.ask_price);
      ImGui::TableSetColumnIndex(4);
      ImGui::TextColored(Palette::Cyan, "%.3f", quote.bid_iv);
      ImGui::TableSetColumnIndex(5);
      ImGui::TextColored(Palette::Coral, "%.3f", quote.ask_iv);
      ImGui::TableSetColumnIndex(6);
      ImGui::Text("%.2f", quote.theoretical_price);
      ImGui::TableSetColumnIndex(7);
      ImGui::TextColored(std::fabs(edge.price_edge) <= 0.05 ? Palette::Lime : Palette::Amber,
                         "%+.2f", edge.price_edge);
      ImGui::TableSetColumnIndex(8);
      ImGui::TextColored(std::fabs(edge.iv_edge_bp) < 10.0 ? Palette::Lime : Palette::Amber,
                         "%+.1f", edge.iv_edge_bp);
      ImGui::TableSetColumnIndex(9);
      ImGui::Text("%+.3f", quote.delta);
      ImGui::TableSetColumnIndex(10);
      ImGui::Text("%.5f", quote.gamma);
      ImGui::TableSetColumnIndex(11);
      ImGui::Text("%+.3f", quote.theta / 365.25);
      ImGui::TableSetColumnIndex(12);
      ImGui::Text("%.3f", quote.vega);
      ImGui::TableSetColumnIndex(13);
      ImGui::TextColored(edge.model_in_bid_ask ? Palette::Lime : Palette::Coral,
                         edge.model_in_bid_ask ? "IN" : "OUT");
    }
  }
  ImGui::EndTable();
}

} // namespace atx::ui
