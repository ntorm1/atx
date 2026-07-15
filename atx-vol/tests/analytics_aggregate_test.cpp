// Tests for the aggregators + two-surface diff (analytics.hpp).
// Scaffold — replaced with full coverage during implementation.

#include <gtest/gtest.h>

#include "atx/vol/analytics.hpp"

namespace atx::vol {
namespace {

TEST(AnalyticsAggregate, DefaultConfigHasTenors) {
  const AnalyticsConfig cfg{};
  EXPECT_FALSE(cfg.tenors.tenors_years.empty());
}

}  // namespace
}  // namespace atx::vol
