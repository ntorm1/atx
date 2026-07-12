#pragma once

#include <cstddef>

#include "atx/ui/vol_surface_source.hpp"
#include "atx/ui/vol_workspace_state.hpp"

namespace atx::ui {

// Coordinated dock panels for one volatility surface. The panels share only a
// view model and selection state, so they can be rearranged or embedded in a
// larger immediate-mode application without hidden global state.
class VolWorkspace {
public:
  explicit VolWorkspace(VolSurfaceSource &source);

  void render_market_strip();
  void render_market_panel();
  void render_curve_panel();
  void render_fit_panel();
  void render_quote_panel();
  void render_term_panel();

private:
  void select_expiry(std::size_t index);

  VolSurfaceSource &source_;
  VolWorkspaceState state_;
};

} // namespace atx::ui
