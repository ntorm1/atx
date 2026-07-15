// Tests for the single-surface analytics primitives (analytics.hpp).
// Scaffold — replaced with full coverage during implementation.

#include <cmath>

#include <gtest/gtest.h>

#include "atx/vol/analytics.hpp"
#include "atx/vol/priced_surface.hpp"
#include "support/analytics_fixture.hpp"

namespace atx::vol {
namespace {

TEST(AnalyticsPrimitives, StandardTenorGridShape) {
  const TenorGrid g = TenorGrid::standard();
  EXPECT_EQ(g.tenors_years.size(), g.labels.size());
  EXPECT_FALSE(g.tenors_years.empty());
}

// Validates the shared fixture: a φ=0 eSSVI surface is flat in log-moneyness.
TEST(AnalyticsPrimitives, FlatFixtureIsFlat) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.35;
  const double F = ps.forward_at(T);
  EXPECT_NEAR(F, 100.0, 1e-9);
  const double atm = ps.iv(F, T);
  EXPECT_NEAR(atm, 0.20, 1e-6);
  EXPECT_NEAR(ps.iv(F * std::exp(0.10), T), 0.20, 1e-6);
  EXPECT_NEAR(ps.iv(F * std::exp(-0.10), T), 0.20, 1e-6);
}

}  // namespace
}  // namespace atx::vol
