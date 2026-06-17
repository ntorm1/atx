#include <cmath>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/data/history_panel.hpp"

namespace {
using atx::engine::data::orats_total_return_close;
} // namespace

TEST(DataHistoryTri, EqualsCloseTimesCumReturnFactor) {
  const std::vector<atx::f64> close = {606.98, 614.48, 617.62};
  const std::vector<atx::f64> caf = {0.0299354, 0.0299354, 0.0299354};
  const auto tri = orats_total_return_close(close, caf);
  ASSERT_EQ(tri.size(), 3u);
  for (size_t i = 0; i < close.size(); ++i)
    EXPECT_NEAR(tri[i], close[i] * caf[i], 1e-9) << "i=" << i;
  // AAPL 2012-03-26 present-basis adjusted close ≈ 18.17 (606.98 * 0.0299354).
  // Tolerance 0.01 to accommodate the ≈ annotation; the per-element loop above
  // verifies the exact product to 1e-9.
  EXPECT_NEAR(tri[0], 18.17, 0.01);
}

TEST(DataHistoryTri, ContinuousAcrossA4For1Split) {
  // A 4:1 split: raw close halves-then-quarters across the ex-date while the
  // cumulative factor steps by 4x, so the adjusted series is continuous (no jump).
  // Pre-split close 400 with factor 0.25; ex-date close 100 with factor 1.0.
  const std::vector<atx::f64> close = {400.0, 100.0, 101.0};
  const std::vector<atx::f64> caf = {0.25, 1.0, 1.0};
  const auto tri = orats_total_return_close(close, caf);
  EXPECT_NEAR(tri[0], 100.0, 1e-9);
  EXPECT_NEAR(tri[1], 100.0, 1e-9);  // no discontinuity at the ex-date
  EXPECT_NEAR(tri[2], 101.0, 1e-9);
}

TEST(DataHistoryTri, NanFactorIsGapNeverZeroFilled) {
  const std::vector<atx::f64> close = {100.0, std::nan(""), 102.0};
  const std::vector<atx::f64> caf = {1.0, std::nan(""), 1.0};
  const auto tri = orats_total_return_close(close, caf);
  EXPECT_DOUBLE_EQ(tri[0], 100.0);
  EXPECT_TRUE(std::isnan(tri[1]));
  EXPECT_DOUBLE_EQ(tri[2], 102.0);
}
