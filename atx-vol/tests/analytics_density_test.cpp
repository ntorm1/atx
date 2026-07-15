// Tests for the risk-neutral density / model-free variance layer (analytics.hpp).
// Scaffold — replaced with full coverage during implementation.

#include <gtest/gtest.h>

#include "atx/vol/analytics.hpp"

namespace atx::vol {
namespace {

TEST(AnalyticsDensity, RndConfigDefaults) {
  const RndConfig c{};
  EXPECT_LT(c.k_min, c.k_max);
  EXPECT_GT(c.n_grid, 0);
}

}  // namespace
}  // namespace atx::vol
