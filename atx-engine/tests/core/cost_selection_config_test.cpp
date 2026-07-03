// S4-0 accept: cost_selection_config_defaults. CostSelectionConfig is the
// inert-default toggle S4-4 reads (and Sprint 5 threads into FitnessCfg) to
// charge sqrt-law impact cost in the search SELECTION scalar. This test pins
// the default-constructed values so a future edit cannot silently flip the
// boundary pin (impact_in_selection=false must always mean "byte-identical
// selection" -- see fitness_cost_selection_test.cpp, S4-4).
#include <gtest/gtest.h>

#include "atx/engine/cost/cost_selection_config.hpp"

namespace {

TEST(CostSelectionConfig, DefaultsAreInert) {
  const atx::engine::cost::CostSelectionConfig cfg{};
  EXPECT_FALSE(cfg.impact_in_selection);
  EXPECT_DOUBLE_EQ(cfg.selection_aum, 0.0);
  EXPECT_DOUBLE_EQ(cfg.cost_weight, 1.0);
}

} // namespace
