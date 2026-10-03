// Platform P9 B2 (K-P9-5 conditional; Andrews, Kitagawa & McCloskey 2024): the winner's-curse
// estimate in the closed-form normal case. X ~ N(mu, se^2) selected on X > c:
//   * the expected accepted estimate under mu = 0 at c = 0 is se sqrt(2 / pi) (half-normal mean);
//   * the conditional median of X given selection, fed back as the raw estimate, inverts to mu
//     (the median-unbiased estimator recovers the mean whose conditional median it is);
//   * far above the threshold the selection is irrelevant: the median is the raw value and the
//     interval is raw -/+ z(1 - alpha/2) se;
//   * the cumulative gain sums the selected cells only.

#include <cmath>
#include <numbers>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/types.hpp"
#include "atx/engine/eval/stats_ext.hpp"
#include "atx/engine/research/eval/conditional.hpp"

namespace {

namespace ev = atx::engine::research::eval;
using atx::f64;

// Q^{-1}(p): the z with P(N(0, 1) > z) = p.
f64 upper_quantile(f64 p) { return atx::engine::eval::norm_ppf(1.0 - p); }

// The conditional median of X ~ N(mu, se^2) given X > c: Q((x - mu)/se) = Q((c - mu)/se) / 2.
f64 conditional_median(f64 mu, f64 se, f64 c) {
  const f64 qc = std::exp(ev::log_upper_tail((c - mu) / se));
  return mu + se * upper_quantile(qc / 2.0);
}

TEST(Conditional, ClosedFormNormalCase) {
  const f64 se = 0.65;
  const auto null = ev::conditional_estimate(0.3, se, 0.0, 0.05);
  ASSERT_TRUE(null.has_value());
  EXPECT_NEAR(null->null_bias, se * std::sqrt(2.0 / std::numbers::pi), 1e-14);

  for (const f64 mu : {-0.5, 0.0, 0.4, 1.2}) {
    const f64 x = conditional_median(mu, se, 0.0);
    const auto e = ev::conditional_estimate(x, se, 0.0, 0.05);
    ASSERT_TRUE(e.has_value());
    ASSERT_TRUE(e->selected) << mu;
    ASSERT_TRUE(e->median_unbiased.has_value()) << mu;
    EXPECT_NEAR(*e->median_unbiased, mu, 1e-9) << mu;
    ASSERT_TRUE(e->ci_lo.has_value() && e->ci_hi.has_value());
    EXPECT_LT(*e->ci_lo, mu);
    EXPECT_GT(*e->ci_hi, mu);
    EXPECT_LT(*e->median_unbiased, x); // the winner's curse shrinks the raw estimate
  }

  const f64 raw = 1.7;
  const auto far = ev::conditional_estimate(raw, 0.1, raw - 40.0, 0.05);
  ASSERT_TRUE(far.has_value());
  ASSERT_TRUE(far->median_unbiased && far->ci_lo && far->ci_hi);
  const f64 z = atx::engine::eval::norm_ppf(0.975);
  EXPECT_NEAR(*far->median_unbiased, raw, 1e-9);
  EXPECT_NEAR(*far->ci_lo, raw - z * 0.1, 1e-9);
  EXPECT_NEAR(*far->ci_hi, raw + z * 0.1, 1e-9);
  EXPECT_FALSE(far->clamped);
}

TEST(Conditional, UnselectedCellsAreListedNotSummed) {
  const std::vector<ev::LineageCell> cells = {
      {"a", 0.5, 0.4, 0.0}, {"b", -0.2, 0.3, 0.0}, {"c", 0.8, 0.5, 0.0}};
  const auto cum = ev::cumulative_conditional(cells, 0.05);
  ASSERT_TRUE(cum.has_value());
  EXPECT_EQ(cum->cells, 2U);
  EXPECT_EQ(cum->rows.size(), 3U);
  EXPECT_FALSE(cum->rows[1].selected);
  EXPECT_FALSE(cum->rows[1].median_unbiased.has_value());
  EXPECT_DOUBLE_EQ(cum->raw_sum, 0.5 + 0.8);
  ASSERT_TRUE(cum->adjusted_sum.has_value());
  EXPECT_DOUBLE_EQ(*cum->adjusted_sum,
                   *cum->rows[0].median_unbiased + *cum->rows[2].median_unbiased);
  EXPECT_LT(*cum->adjusted_sum, cum->raw_sum);
  EXPECT_DOUBLE_EQ(cum->null_bias_sum, cum->rows[0].null_bias + cum->rows[2].null_bias);
}

TEST(Conditional, RefusesANonPositiveSe) {
  EXPECT_FALSE(ev::conditional_estimate(0.5, 0.0, 0.0, 0.05).has_value());
  EXPECT_FALSE(ev::conditional_estimate(0.5, 0.3, 0.0, 1.0).has_value());
}

} // namespace
