#include <string>
#include <gtest/gtest.h>
#include "atx/engine/regime/series.hpp"

namespace atxtest_regime_series {
using atx::engine::regime::parse_derived_spec;

TEST(RegimeSeries, PrefixIsRegimeUnderscore) {
  EXPECT_EQ(atx::engine::regime::kRegimePrefix, "regime_");
}

TEST(RegimeSeries, ParseDerivedSpec_Subtraction) {
  auto r = parse_derived_spec("t10y2y = dgs10 - dgs2");
  ASSERT_TRUE(r.has_value()) << (r ? "" : r.error().message());
  EXPECT_EQ(r.value().name, "t10y2y");
  EXPECT_EQ(r.value().lhs, "dgs10");
  EXPECT_EQ(r.value().op, '-');
  EXPECT_EQ(r.value().rhs, "dgs2");
}

TEST(RegimeSeries, ParseDerivedSpec_RejectsMissingOperator) {
  EXPECT_FALSE(parse_derived_spec("t10y2y = dgs10 dgs2").has_value());
  EXPECT_FALSE(parse_derived_spec("garbage").has_value());
}
}  // namespace atxtest_regime_series
