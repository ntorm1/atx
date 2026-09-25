// W0-D0 — the total-return index keeps accumulated dividends across a gap (D-04).
//
// Suite: DataAdjustGap_W0d0
//
// The legacy rule re-anchored the TRI at S_t after any gap, dropping every dividend
// reinvested before it: a 3% payer five years in showed a phantom -14% step. The
// default rule resumes the index by the price ratio prev_TRI * S_t / S_last.

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"

#include "atx/engine/data/adjust.hpp"

namespace atx_test_w0_d0_adjust_gap {

using atx::engine::data::adjust_total_return;
using atx::engine::data::AdjustedSeries;
using atx::engine::data::TriGapRule;

namespace {

constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

// A flat-priced payer: raw close 100 every session, a 0.75 cash dividend every
// 63 sessions (3% a year), `years` x 252 sessions, then one missing session and
// one more valid session at the same price.
struct PayerSeries {
  std::vector<atx::f64> close;
  std::vector<atx::f64> factor;
  std::vector<atx::f64> dividend;
  atx::usize gap = 0; // index of the missing session
};

[[nodiscard]] PayerSeries payer(atx::usize years) {
  PayerSeries s;
  const atx::usize n = years * 252 + 2;
  s.close.assign(n, 100.0);
  s.factor.assign(n, 1.0);
  s.dividend.assign(n, 0.0);
  for (atx::usize t = 63; t + 2 < n; t += 63) {
    s.dividend[t] = 0.75;
  }
  s.gap = n - 2;
  s.close[s.gap] = kNaN;
  return s;
}

} // namespace

// Acceptance: a 3% payer shows no phantom drop across a gap.
TEST(DataAdjustGap_W0d0, ThreePercentPayerShowsNoPhantomDropAcrossGap) {
  const PayerSeries s = payer(5);
  const AdjustedSeries v2 = adjust_total_return(s.close, s.factor, s.dividend);
  const AdjustedSeries v1 =
      adjust_total_return(s.close, s.factor, s.dividend, TriGapRule::ReanchorV1);
  const atx::usize before = s.gap - 1;
  const atx::usize after = s.gap + 1;
  ASSERT_TRUE(std::isnan(v2.total_return_index[s.gap]));

  // 19 quarterly dividends of 0.75 on a flat 100 price: TRI = 100 * 1.0075^19.
  EXPECT_NEAR(v2.total_return_index[before], 100.0 * std::pow(1.0075, 19.0), 1e-9);
  const atx::f64 step_v2 = v2.total_return_index[after] / v2.total_return_index[before] - 1.0;
  const atx::f64 step_v1 = v1.total_return_index[after] / v1.total_return_index[before] - 1.0;
  // The price did not move across the gap, so the index must not move either.
  EXPECT_EQ(step_v2, 0.0);
  EXPECT_DOUBLE_EQ(v2.total_return_index[after], v2.total_return_index[before]);
  // The legacy rule dropped all 19 dividends: 1 / 1.0075^19 - 1 = -13.24%.
  EXPECT_NEAR(step_v1, 1.0 / std::pow(1.0075, 19.0) - 1.0, 1e-12);
  EXPECT_LT(step_v1, -0.13);
  std::printf("[tri-gap] 3%% payer, 5y: RatioChainV2 step=%.6f  ReanchorV1 step=%.6f\n", step_v2,
              step_v1);
  // Everything before the gap is identical under both rules.
  for (atx::usize t = 0; t < s.gap; ++t) {
    EXPECT_EQ(v1.total_return_index[t], v2.total_return_index[t]) << t;
    EXPECT_EQ(v1.total_return[t], v2.total_return[t]) << t;
  }
  // The one-day return at the resumption cell is undefined under both rules (0).
  EXPECT_EQ(v2.total_return[after], 0.0);
  EXPECT_EQ(v1.total_return[after], 0.0);
}

// A price move across the gap is carried by the ratio; the chain continues after.
TEST(DataAdjustGap_W0d0, GapCarriesThePriceRatioAndChainContinues) {
  const std::vector<atx::f64> close = {100.0, 101.0, kNaN, kNaN, 90.9, 92.0};
  const std::vector<atx::f64> factor = {1.0, 1.0, 1.0, 1.0, 1.0, 1.0};
  const std::vector<atx::f64> div = {0.0, 2.0, 0.0, 0.0, 0.0, 0.0};
  const AdjustedSeries adj = adjust_total_return(close, factor, div);
  // TRI_1 = 100 * (101 + 2) / 100 = 103.
  EXPECT_DOUBLE_EQ(adj.total_return_index[1], 103.0);
  // Resume: 103 * 90.9 / 101 = 92.7 (the 2.0 dividend is kept).
  EXPECT_DOUBLE_EQ(adj.total_return_index[4], 103.0 * (90.9 / 101.0));
  EXPECT_NEAR(adj.total_return_index[4] / adj.total_return_index[1], 90.9 / 101.0, 1e-15);
  EXPECT_DOUBLE_EQ(adj.total_return[5], 92.0 / 90.9 - 1.0);
  EXPECT_DOUBLE_EQ(adj.total_return_index[5], adj.total_return_index[4] * (92.0 / 90.9));
}

// A split inside the gap: S is split-adjusted, so the ratio uses the continuous S.
TEST(DataAdjustGap_W0d0, SplitInsideTheGapUsesSplitAdjustedPrices) {
  // 2:1 split on index 2 (missing): raw 100 -> 50, factor 0.5 -> 1.0.
  const std::vector<atx::f64> close = {100.0, kNaN, 50.0};
  const std::vector<atx::f64> factor = {0.5, kNaN, 1.0};
  const std::vector<atx::f64> div = {0.0, 0.0, 0.0};
  const AdjustedSeries adj = adjust_total_return(close, factor, div);
  EXPECT_DOUBLE_EQ(adj.total_return_index[0], 50.0);
  EXPECT_DOUBLE_EQ(adj.total_return_index[2], 50.0); // no fabricated split jump
}

// The first valid cell still anchors at S (nothing to chain from) under both rules.
TEST(DataAdjustGap_W0d0, LeadingGapAnchorsAtTheFirstValidClose) {
  const std::vector<atx::f64> close = {kNaN, kNaN, 40.0, 41.0};
  const std::vector<atx::f64> factor = {1.0, 1.0, 2.0, 2.0};
  const std::vector<atx::f64> div = {0.0, 0.0, 0.0, 0.0};
  for (const TriGapRule rule : {TriGapRule::ReanchorV1, TriGapRule::RatioChainV2}) {
    const AdjustedSeries adj = adjust_total_return(close, factor, div, rule);
    EXPECT_TRUE(std::isnan(adj.total_return_index[0]));
    EXPECT_DOUBLE_EQ(adj.total_return_index[2], 80.0);
    EXPECT_DOUBLE_EQ(adj.total_return[2], 0.0);
  }
}

// Legacy reproduction: ReanchorV1 still sets TRI = S after a gap.
TEST(DataAdjustGap_W0d0, ReanchorV1ReproducesTheLegacyLevel) {
  const std::vector<atx::f64> close = {100.0, 101.0, kNaN, 90.9};
  const std::vector<atx::f64> factor = {1.0, 1.0, 1.0, 1.0};
  const std::vector<atx::f64> div = {0.0, 2.0, 0.0, 0.0};
  const AdjustedSeries v1 = adjust_total_return(close, factor, div, TriGapRule::ReanchorV1);
  EXPECT_DOUBLE_EQ(v1.total_return_index[3], 90.9);
}

} // namespace atx_test_w0_d0_adjust_gap
