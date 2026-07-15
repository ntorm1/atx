// Tests for the single-surface analytics primitives (analytics.hpp).
// Scaffold — replaced with full coverage during implementation.

#include <gtest/gtest.h>

#include "atx/vol/analytics.hpp"

namespace atx::vol {
namespace {

TEST(AnalyticsPrimitives, StandardTenorGridShape) {
  const TenorGrid g = TenorGrid::standard();
  EXPECT_EQ(g.tenors_years.size(), g.labels.size());
  EXPECT_FALSE(g.tenors_years.empty());
}

}  // namespace
}  // namespace atx::vol
