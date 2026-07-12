#pragma once

#include <algorithm>

namespace atx::ui {

enum class VolXAxisMode { NormalizedStrike, Strike };
enum class VolYAxisMode { ImpliedVol, TotalVariance };

// Persistent interaction state is deliberately independent of ImGui and the
// market-data adapter. It can be serialized, tested, and reused by additional
// workspaces (replay, risk, relative value) without copying panel logic.
struct VolWorkspaceState {
  VolXAxisMode x_axis{VolXAxisMode::NormalizedStrike};
  VolYAxisMode y_axis{VolYAxisMode::ImpliedVol};
  bool show_model{true};
  bool show_market_band{true};
  bool show_market_mid{true};
  bool show_calls{true};
  bool show_puts{true};
  double normalized_window{2.0};
  bool fit_plot_next{true};

  [[nodiscard]] bool quote_visible(char side) const noexcept {
    return (side == 'C' && show_calls) || (side == 'P' && show_puts);
  }

  [[nodiscard]] double plot_x(double z, double strike) const noexcept {
    return x_axis == VolXAxisMode::NormalizedStrike ? z : strike;
  }

  [[nodiscard]] double plot_y(double iv, double years) const noexcept {
    return y_axis == VolYAxisMode::TotalVariance ? iv * iv * years : iv;
  }

  void set_normalized_window(double value) noexcept {
    normalized_window = std::clamp(value, 0.25, 4.0);
    fit_plot_next = true;
  }

  void request_plot_fit() noexcept { fit_plot_next = true; }
};

} // namespace atx::ui
