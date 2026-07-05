#include <gtest/gtest.h>

#include <cmath>

#include "atx/core/math.hpp"

// Coverage for the standard-normal helpers folded in from the C ats-vol
// library. The contract is the textbook Φ / φ, so the tests pin known values,
// symmetry, monotonicity, tail behaviour, and agreement with libm.

namespace {

using atx::core::norm_cdf;
using atx::core::norm_pdf;

TEST(NormPdf, AtZero_EqualsInvSqrt2Pi) {
  // φ(0) = 1/√(2π) ≈ 0.3989422804.
  EXPECT_NEAR(norm_pdf(0.0), 0.3989422804014327, 1e-15);
}

TEST(NormPdf, IsSymmetric) {
  for (double x : {0.25, 0.5, 1.0, 2.5, 6.0}) {
    EXPECT_NEAR(norm_pdf(x), norm_pdf(-x), 1e-18);
  }
}

TEST(NormPdf, MatchesLibmFormula) {
  for (double x : {-3.0, -1.0, 0.3, 1.7, 4.2}) {
    constexpr double two_pi = 2.0 * 3.141592653589793238462643383279;
    const double expected = std::exp(-0.5 * x * x) / std::sqrt(two_pi);
    EXPECT_NEAR(norm_pdf(x), expected, 1e-15);
  }
}

TEST(NormCdf, AtZero_EqualsHalf) {
  EXPECT_DOUBLE_EQ(norm_cdf(0.0), 0.5);
}

TEST(NormCdf, ReflectionIdentity) {
  // Φ(x) + Φ(-x) = 1 for all x.
  for (double x : {0.1, 0.75, 1.5, 3.0, 5.5}) {
    EXPECT_NEAR(norm_cdf(x) + norm_cdf(-x), 1.0, 1e-15);
  }
}

TEST(NormCdf, KnownQuantiles) {
  // Textbook values.
  EXPECT_NEAR(norm_cdf(1.0), 0.8413447460685429, 1e-15);
  EXPECT_NEAR(norm_cdf(-1.0), 0.15865525393145705, 1e-15);
  EXPECT_NEAR(norm_cdf(1.959963984540054), 0.975, 1e-12);  // 97.5th pct
}

TEST(NormCdf, MonotoneIncreasing) {
  double prev = norm_cdf(-8.0);
  for (double x = -7.5; x <= 8.0; x += 0.5) {
    const double cur = norm_cdf(x);
    EXPECT_GT(cur, prev);
    prev = cur;
  }
}

TEST(NormCdf, TailsAreBounded) {
  EXPECT_GE(norm_cdf(-40.0), 0.0);
  EXPECT_LE(norm_cdf(40.0), 1.0);
  EXPECT_LT(norm_cdf(-10.0), 1e-20);
  // Φ(10) rounds to exactly 1.0 in double (1 - Φ(10) ≈ 7.6e-24 < ½ulp), so
  // compare against a representable near-one bound rather than 1 - 1e-20.
  EXPECT_GE(norm_cdf(10.0), 1.0 - 1e-15);
}

TEST(NormCdf, MatchesErfcDefinition) {
  for (double x : {-4.0, -1.3, 0.0, 0.9, 3.1}) {
    const double expected = 0.5 * std::erfc(-x / std::sqrt(2.0));
    EXPECT_NEAR(norm_cdf(x), expected, 1e-16);
  }
}

} // namespace
