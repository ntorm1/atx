#include <array>

#include <gtest/gtest.h>

#include "atx/ui/vol_workspace_model.hpp"
#include "atx/ui/vol_workspace_state.hpp"

namespace {

TEST(VolWorkspaceState, QuoteVisibilityAndAxesAreDeterministic) {
  atx::ui::VolWorkspaceState state;
  EXPECT_TRUE(state.quote_visible('C'));
  EXPECT_TRUE(state.quote_visible('P'));
  state.show_puts = false;
  EXPECT_FALSE(state.quote_visible('P'));
  EXPECT_DOUBLE_EQ(state.plot_x(-0.5, 100.0), -0.5);
  state.x_axis = atx::ui::VolXAxisMode::Strike;
  EXPECT_DOUBLE_EQ(state.plot_x(-0.5, 100.0), 100.0);
  state.y_axis = atx::ui::VolYAxisMode::TotalVariance;
  EXPECT_DOUBLE_EQ(state.plot_y(0.2, 0.5), 0.02);
}

TEST(VolWorkspaceState, NormalizedWindowIsClampedAndRequestsFit) {
  atx::ui::VolWorkspaceState state;
  state.fit_plot_next = false;
  state.set_normalized_window(20.0);
  EXPECT_DOUBLE_EQ(state.normalized_window, 4.0);
  EXPECT_TRUE(state.fit_plot_next);
}

TEST(VolWorkspaceModel, ChoosesRequestedOrNearestDefaultExpiry) {
  const std::array expiries{
      atx::ui::ExpiryInfo{.iso_date = "2026-07-01", .years = 30.0 / 365.25},
      atx::ui::ExpiryInfo{.iso_date = "2026-12-01", .years = 180.0 / 365.25},
      atx::ui::ExpiryInfo{.iso_date = "2027-06-01", .years = 365.0 / 365.25},
  };
  EXPECT_EQ(atx::ui::choose_initial_expiry(expiries, "2027-06-01"), 2U);
  EXPECT_EQ(atx::ui::choose_initial_expiry(expiries, "missing"), 1U);
  EXPECT_EQ(atx::ui::nearest_expiry_by_days(expiries, 40.0), 0U);
}

TEST(VolWorkspaceModel, ComputesPriceAndVolEdges) {
  const atx::ui::VolQuotePoint quote{
      .mid_price = 2.0,
      .theoretical_price = 2.15,
      .bid_iv = 0.18,
      .ask_iv = 0.22,
      .mid_iv = 0.20,
      .model_iv = 0.21,
  };
  const atx::ui::QuoteEdgeMetrics edge = atx::ui::quote_edge_metrics(quote);
  EXPECT_NEAR(edge.price_edge, 0.15, 1.0e-12);
  EXPECT_NEAR(edge.iv_edge_bp, 100.0, 1.0e-12);
  EXPECT_TRUE(edge.model_in_bid_ask);
}

} // namespace
