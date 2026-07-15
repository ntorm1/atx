// Tests for the CSV serializers (analytics.hpp).
// Scaffold — replaced with full coverage during implementation.

#include <gtest/gtest.h>

#include "atx/vol/analytics.hpp"

namespace atx::vol {
namespace {

TEST(AnalyticsIo, RndStructDefaults) {
  const RiskNeutralDensity r{};
  EXPECT_FALSE(r.valid);
  EXPECT_TRUE(r.strikes.empty());
}

}  // namespace
}  // namespace atx::vol
