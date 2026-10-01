// combine_group_residualise_test.cpp -- least-squares residual of one cross-section on earlier
// ones (platform v8 R-11, composition theme-resid-v1 in atx-impl).
//
// Suite: GroupResidualise

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/combine/group_residualise.hpp"

namespace atx_test_v8_group_residualise {

using atx::f64;
using atx::usize;
namespace cb = atx::engine::combine;
constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();
constexpr f64 kInf = std::numeric_limits<f64>::infinity();

f64 mean(const std::vector<f64>& v) {
  f64 sum = 0;
  for (const f64 x : v) sum += x;
  return sum / static_cast<f64>(v.size());
}

f64 dot(const std::vector<f64>& a, const std::vector<f64>& b) {
  f64 sum = 0;
  for (usize i = 0; i < a.size(); ++i) sum += a[i] * b[i];
  return sum;
}

std::vector<f64> centred(const std::vector<f64>& v) {
  const f64 m = mean(v);
  std::vector<f64> out(v.size());
  for (usize i = 0; i < v.size(); ++i) out[i] = v[i] - m;
  return out;
}

std::vector<f64> joined(const std::vector<std::vector<f64>>& columns) {
  std::vector<f64> out;
  for (const auto& c : columns) out.insert(out.end(), c.begin(), c.end());
  return out;
}

// Two standardised composites of seven names (each a permutation of the centred ranks).
const std::vector<f64> kY{0.5, -1.0 / 6, 1.0 / 3, -0.5, 0.0, 1.0 / 6, -1.0 / 3};
const std::vector<f64> kX{-1.0 / 3, 0.5, 1.0 / 6, 0.0, -0.5, 1.0 / 3, -1.0 / 6};

// The two-theme case in closed form: e = (y - mean y) - b (x - mean x), b = Sxy / Sxx; the
// residual is orthogonal to the constant and to the preceding composite to 1e-12.
TEST(GroupResidualise, TwoGroupsMatchTheClosedFormAndAreOrthogonal) {
  std::vector<f64> y = kY, columns = kX;
  const auto fit = cb::residualise_in_place(y, columns);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->rank, 1U);
  EXPECT_FALSE(fit->spanned);
  const auto xc = centred(kX), yc = centred(kY);
  const f64 b = dot(xc, yc) / dot(xc, xc);
  ASSERT_GT(std::abs(b), 0.05); // a real projection (b = -3/28), not a no-op
  for (usize i = 0; i < y.size(); ++i) EXPECT_NEAR(y[i], yc[i] - b * xc[i], 1e-14) << i;
  f64 sum = 0;
  for (const f64 e : y) sum += e;
  EXPECT_LE(std::abs(sum), 1e-12);
  EXPECT_LE(std::abs(dot(y, kX)), 1e-12);
  // The kept column is the unit-norm centred regressor.
  EXPECT_NEAR(dot(columns, columns), 1.0, 1e-14);
  EXPECT_LE(std::abs(mean(columns)), 1e-15);
}

// Two regressors against Cramer's rule on the centred normal equations; the residual is
// orthogonal to the constant and to both.
TEST(GroupResidualise, TwoRegressorsMatchCramerAndAreOrthogonal) {
  constexpr usize n = 9;
  std::vector<f64> y(n), x1(n), x2(n);
  for (usize i = 0; i < n; ++i) {
    const auto t = static_cast<f64>(i);
    y[i] = std::sin(1.1 * t) + 0.3 * t;
    x1[i] = std::cos(0.7 * t);
    x2[i] = static_cast<f64>(i % 4) - 1.5 + 0.1 * t;
  }
  std::vector<f64> e = y, columns = joined({x1, x2});
  const auto fit = cb::residualise_in_place(e, columns);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->rank, 2U);
  EXPECT_FALSE(fit->spanned);
  const auto yc = centred(y), c1 = centred(x1), c2 = centred(x2);
  const f64 s11 = dot(c1, c1), s12 = dot(c1, c2), s22 = dot(c2, c2), s1y = dot(c1, yc), s2y = dot(c2, yc);
  const f64 det = s11 * s22 - s12 * s12;
  ASSERT_GT(det, 1e-3);
  const f64 b1 = (s1y * s22 - s2y * s12) / det, b2 = (s11 * s2y - s12 * s1y) / det;
  for (usize i = 0; i < n; ++i) EXPECT_NEAR(e[i], yc[i] - b1 * c1[i] - b2 * c2[i], 1e-12) << i;
  f64 sum = 0;
  for (const f64 v : e) sum += v;
  EXPECT_LE(std::abs(sum), 1e-12);
  EXPECT_LE(std::abs(dot(e, x1)), 1e-12);
  EXPECT_LE(std::abs(dot(e, x2)), 1e-12);
}

// Affine, duplicated, all-zero and constant columns add nothing to the span: the residual is
// that of the independent column. More columns than the rows carry: rank n - 1, y spanned.
TEST(GroupResidualise, DependentColumnsAreSkipped) {
  std::vector<f64> single = kY, x = kX;
  ASSERT_TRUE(cb::residualise_in_place(single, x));
  std::vector<f64> affine(kX.size()), zeros(kX.size(), 0.0), constant(kX.size(), 5.0);
  for (usize i = 0; i < kX.size(); ++i) affine[i] = 3.0 * kX[i] + 2.0;
  std::vector<f64> y = kY, columns = joined({kX, affine, zeros, constant, kX});
  const auto fit = cb::residualise_in_place(y, columns);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->rank, 1U);
  EXPECT_FALSE(fit->spanned);
  for (usize i = 0; i < y.size(); ++i) EXPECT_NEAR(y[i], single[i], 1e-15) << i;
  // Three rows carry at most two centred directions.
  std::vector<f64> small{0.3, -0.1, 0.7};
  std::vector<f64> many = joined({{1.0, 2.0, 4.0}, {-1.0, 0.5, 0.25}, {0.0, 3.0, -2.0}, {5.0, 1.0, 1.5}});
  const auto full = cb::residualise_in_place(small, many);
  ASSERT_TRUE(full) << full.error().to_string();
  EXPECT_EQ(full->rank, 2U);
  EXPECT_TRUE(full->spanned);
  for (const f64 v : small) EXPECT_EQ(std::bit_cast<std::uint64_t>(v), std::bit_cast<std::uint64_t>(0.0));
}

// A dependent vector in the span (affine in the regressor, or constant) is exactly +0: no
// rounding noise survives to be re-ranked.
TEST(GroupResidualise, SpannedDependentBecomesExactlyZero) {
  std::vector<f64> y(kX.size()), columns = kX;
  for (usize i = 0; i < kX.size(); ++i) y[i] = 2.0 * kX[i] - 1.0;
  const auto fit = cb::residualise_in_place(y, columns);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_TRUE(fit->spanned);
  for (const f64 v : y) EXPECT_EQ(std::bit_cast<std::uint64_t>(v), std::bit_cast<std::uint64_t>(0.0));
  std::vector<f64> flat(5, 0.25), none;
  const auto constant = cb::residualise_in_place(flat, none);
  ASSERT_TRUE(constant);
  EXPECT_TRUE(constant->spanned);
  for (const f64 v : flat) EXPECT_EQ(v, 0.0);
}

// No regressor: the residual on the intercept alone is the demeaned vector; one row is spanned.
TEST(GroupResidualise, NoRegressorIsTheDemeanedVector) {
  std::vector<f64> y = kY, none;
  const auto fit = cb::residualise_in_place(y, none);
  ASSERT_TRUE(fit) << fit.error().to_string();
  EXPECT_EQ(fit->rank, 0U);
  EXPECT_FALSE(fit->spanned);
  const auto yc = centred(kY);
  for (usize i = 0; i < y.size(); ++i) EXPECT_NEAR(y[i], yc[i], 1e-15) << i;
  std::vector<f64> lone{0.7}, column{0.2};
  const auto one = cb::residualise_in_place(lone, column);
  ASSERT_TRUE(one);
  EXPECT_EQ(one->rank, 0U);
  EXPECT_TRUE(one->spanned);
  EXPECT_EQ(lone[0], 0.0);
}

TEST(GroupResidualise, RefusesBadShapesNonFiniteValuesAndTolerances) {
  std::vector<f64> empty, columns{1.0, 2.0};
  EXPECT_FALSE(cb::residualise_in_place(empty, columns));
  std::vector<f64> y{1.0, 2.0}, ragged{1.0, 2.0, 3.0};
  EXPECT_FALSE(cb::residualise_in_place(y, ragged)); // not whole columns of n
  std::vector<f64> nan_y{1.0, kNaN}, fine{0.5, -0.5};
  EXPECT_FALSE(cb::residualise_in_place(nan_y, fine));
  std::vector<f64> inf_columns{0.5, kInf};
  EXPECT_FALSE(cb::residualise_in_place(y, inf_columns));
  for (const f64 tolerance : {0.0, 1.0, -1e-10, kNaN}) EXPECT_FALSE(cb::residualise_in_place(y, fine, tolerance));
  // A refusal leaves both spans as they were.
  EXPECT_EQ(y, (std::vector<f64>{1.0, 2.0}));
  EXPECT_EQ(fine, (std::vector<f64>{0.5, -0.5}));
}

} // namespace atx_test_v8_group_residualise
