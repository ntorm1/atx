#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "atx/vol/api/storage/s3.hpp"

// S3 / SSVI three-parameter shape curve (Vola Dynamics, Klassen 2017):
//   - c2 = 0 "takeover-for-cash" kink: sigma^2 = sigma0^2 * max(1 + s2*z, 0);
//     symmetric constant when s2 = 0.
//   - s2 = 0, c2 > 0: symmetric smile with g(0) = 1 + 0.5*c2 >= 1.
//   - ATF bound: at |s2| == s3_atf_max_skew the density g(0) == 0; just above
//     is negative, just below positive.
//   - density g(z) >= 0 across a wide z-grid for an arb-free parameter set.
//   - wings signs and the Lee wing bound sigma_hat0 * C_pm <= 2.
//   - analytic g matches a central-difference Roper g.
//   - the least-squares seed recovers injected parameters from clean samples.

namespace {

using atx::vol::ErrorCode;
using atx::vol::S3Params;
using atx::vol::S3Wings;
using atx::vol::s3_atf_arb_free;
using atx::vol::s3_atf_max_skew;
using atx::vol::s3_density_g;
using atx::vol::s3_iv;
using atx::vol::s3_seed_from_ivs;
using atx::vol::s3_sigma2_of_z;
using atx::vol::s3_total_var;
using atx::vol::s3_wings;

// Central-difference Roper density from s3_total_var, computed with a step
// proportional to sigma_hat0 (so the finite-difference error scales with the
// z coordinate rather than the amplified k coordinate).
[[nodiscard]] double fd_density_g(double z, double T, const S3Params& p) {
  const double shat0 = p.sigma0 * std::sqrt(T);
  const double k = shat0 * z;
  const double h = 1.0e-3 * shat0;
  const double w_lo = s3_total_var(k - h, T, p);
  const double w_mi = s3_total_var(k, T, p);
  const double w_hi = s3_total_var(k + h, T, p);
  const double wp = (w_hi - w_lo) / (2.0 * h);
  const double wpp = (w_hi - 2.0 * w_mi + w_lo) / (h * h);
  const double inner = 1.0 - 0.5 * k * wp / w_mi;
  return inner * inner - 0.25 * wp * wp * (0.25 + 1.0 / w_mi) + 0.5 * wpp;
}

// ── c2 == 0: kinked "takeover-for-cash" shape ────────────────────────────

TEST(S3, C2ZeroS2Zero_ConstantAndSymmetric) {
  const S3Params p{/*sigma0=*/0.2, /*s2=*/0.0, /*c2=*/0.0};
  const double s2atm = p.sigma0 * p.sigma0;
  for (int i = 0; i <= 12; ++i) {
    const double z = 0.5 * static_cast<double>(i);
    EXPECT_NEAR(s3_sigma2_of_z(z, p), s2atm, 1.0e-15);
    // Symmetric: sigma^2(z) == sigma^2(-z).
    EXPECT_NEAR(s3_sigma2_of_z(z, p), s3_sigma2_of_z(-z, p), 1.0e-15);
  }
}

TEST(S3, C2ZeroS2Positive_MatchesClampedLinearKink) {
  // s2 = 0.3 places the kink at z = -1/s2 = -3.333..., off the 0.5-spaced grid
  // so no sample lands exactly on the (FP-cancellation-prone) kink point.
  const S3Params p{/*sigma0=*/0.25, /*s2=*/0.3, /*c2=*/0.0};
  for (int i = -12; i <= 12; ++i) {
    const double z = 0.5 * static_cast<double>(i);
    // f(z) collapses to max(1 + s2*z, 0) in the c2 -> 0 limit.
    const double f_expected = std::max(1.0 + p.s2 * z, 0.0);
    const double expected = p.sigma0 * p.sigma0 * f_expected;
    EXPECT_NEAR(s3_sigma2_of_z(z, p), expected, 1.0e-12);
    // Essentially non-negative (a floored value may sit an ULP below 0).
    EXPECT_GE(s3_sigma2_of_z(z, p), -1.0e-14);
  }
  // Strictly positive where 1 + s2*z > 0 (e.g. z = -2.0 => 1 - 0.6 = 0.4).
  EXPECT_GT(s3_sigma2_of_z(-2.0, p), 0.0);
}

// ── s2 == 0, c2 > 0: symmetric smile ─────────────────────────────────────

TEST(S3, S2ZeroC2Positive_SymmetricSmileWithDensityAtLeastOne) {
  const S3Params p{/*sigma0=*/0.2, /*s2=*/0.0, /*c2=*/0.6};
  const double T = 0.5;
  for (int i = 1; i <= 10; ++i) {
    const double z = 0.4 * static_cast<double>(i);
    EXPECT_NEAR(s3_sigma2_of_z(z, p), s3_sigma2_of_z(-z, p), 1.0e-14);
  }
  // g(0) = 1 + 0.5*c2 when s2 == 0 (the skew term vanishes).
  EXPECT_NEAR(s3_density_g(0.0, T, p), 1.0 + 0.5 * p.c2, 1.0e-12);
  EXPECT_GE(s3_density_g(0.0, T, p), 1.0);
}

// ── ATF no-arbitrage bound at the boundary ───────────────────────────────

TEST(S3, AtfBound_AtExactMaxSkew_DensityZero) {
  const double sigma0 = 0.3;
  const double T = 1.0;
  const double c2 = 0.4;
  const double shat0 = sigma0 * std::sqrt(T);
  const double s2 = s3_atf_max_skew(c2, shat0);
  const S3Params p{sigma0, s2, c2};
  EXPECT_NEAR(s3_density_g(0.0, T, p), 0.0, 1.0e-9);
  EXPECT_TRUE(s3_atf_arb_free(p, T));
}

TEST(S3, AtfBound_JustAboveMaxSkew_DensityNegative) {
  const double sigma0 = 0.3;
  const double T = 1.0;
  const double c2 = 0.4;
  const double shat0 = sigma0 * std::sqrt(T);
  const double s2 = 1.0001 * s3_atf_max_skew(c2, shat0);
  const S3Params p{sigma0, s2, c2};
  EXPECT_LT(s3_density_g(0.0, T, p), 0.0);
  EXPECT_FALSE(s3_atf_arb_free(p, T));
}

TEST(S3, AtfBound_JustBelowMaxSkew_DensityPositive) {
  const double sigma0 = 0.3;
  const double T = 1.0;
  const double c2 = 0.4;
  const double shat0 = sigma0 * std::sqrt(T);
  const double s2 = 0.9999 * s3_atf_max_skew(c2, shat0);
  const S3Params p{sigma0, s2, c2};
  EXPECT_GT(s3_density_g(0.0, T, p), 0.0);
  EXPECT_TRUE(s3_atf_arb_free(p, T));
}

TEST(S3, AtfMaxSkew_MatchesClosedForm) {
  const double c2 = 0.7;
  const double shat0 = 0.15;
  const double expected =
      std::sqrt((4.0 + 2.0 * c2) / (1.0 + 0.25 * shat0 * shat0));
  EXPECT_NEAR(s3_atf_max_skew(c2, shat0), expected, 1.0e-15);
}

// ── Global density positivity on a wide grid ─────────────────────────────

TEST(S3, Density_ArbFreeParams_NonNegativeAcrossWideGrid) {
  // Mild, ATF-admissible smile; sweep z in [-6, 6] as arb.hpp does (+/-5.5).
  const S3Params p{/*sigma0=*/0.2, /*s2=*/0.15, /*c2=*/0.4};
  const double T = 0.25;
  ASSERT_TRUE(s3_atf_arb_free(p, T));
  for (int i = -120; i <= 120; ++i) {
    const double z = 0.05 * static_cast<double>(i);  // [-6, 6]
    // Use the library's own -1e-9 flag tolerance (arb_check_butterfly).
    EXPECT_GE(s3_density_g(z, T, p), -1.0e-9) << "z=" << z;
  }
}

// ── Wings: signs and the Lee wing bound ──────────────────────────────────

TEST(S3, Wings_SignsAndLeeBound) {
  const S3Params p{/*sigma0=*/0.2, /*s2=*/0.3, /*c2=*/0.5};
  const S3Wings w = s3_wings(p);
  // c2 > 0 => both wings strictly positive; positive skew => c_plus > c_minus.
  EXPECT_GT(w.c_plus, 0.0);
  EXPECT_GT(w.c_minus, 0.0);
  EXPECT_GT(w.c_plus, w.c_minus);
  // Closed form.
  const double root = std::sqrt(0.25 * p.s2 * p.s2 + 0.5 * p.c2);
  EXPECT_NEAR(w.c_plus, root + 0.5 * p.s2, 1.0e-15);
  EXPECT_NEAR(w.c_minus, root - 0.5 * p.s2, 1.0e-15);
  // Lee wing bound sigma_hat0 * C_pm <= 2 (flagged, here satisfied).
  const double T = 0.25;
  const double shat0 = p.sigma0 * std::sqrt(T);
  EXPECT_LE(shat0 * w.c_plus, 2.0);
  EXPECT_LE(shat0 * w.c_minus, 2.0);
}

TEST(S3, Wings_SymmetricWhenSkewZero) {
  const S3Params p{/*sigma0=*/0.2, /*s2=*/0.0, /*c2=*/0.5};
  const S3Wings w = s3_wings(p);
  EXPECT_NEAR(w.c_plus, w.c_minus, 1.0e-15);
}

// ── Analytic density vs central difference ───────────────────────────────

TEST(S3, Density_AnalyticMatchesCentralDifference) {
  const S3Params p{/*sigma0=*/0.2, /*s2=*/0.15, /*c2=*/0.4};
  const double T = 0.25;
  for (int i = -8; i <= 8; ++i) {
    const double z = 0.5 * static_cast<double>(i);  // [-4, 4]
    EXPECT_NEAR(s3_density_g(z, T, p), fd_density_g(z, T, p), 1.0e-6)
        << "z=" << z;
  }
}

// ── total-variance / iv consistency ──────────────────────────────────────

TEST(S3, TotalVar_ConsistentWithSigma2AndIv) {
  const S3Params p{/*sigma0=*/0.22, /*s2=*/0.1, /*c2=*/0.3};
  const double T = 0.5;
  const double shat0 = p.sigma0 * std::sqrt(T);
  for (int i = -6; i <= 6; ++i) {
    const double z = 0.4 * static_cast<double>(i);
    const double k = shat0 * z;
    // w(k) == T * sigma^2(z).
    EXPECT_NEAR(s3_total_var(k, T, p), T * s3_sigma2_of_z(z, p), 1.0e-14);
    // iv == sqrt(w / T).
    EXPECT_NEAR(s3_iv(k, T, p), std::sqrt(s3_total_var(k, T, p) / T), 1.0e-14);
  }
  // ATM iv equals sigma0.
  EXPECT_NEAR(s3_iv(0.0, T, p), p.sigma0, 1.0e-12);
}

// ── Seed recovery ────────────────────────────────────────────────────────

TEST(S3, Seed_CleanSamples_RecoversInjectedParams) {
  const S3Params truth{/*sigma0=*/0.22, /*s2=*/0.3, /*c2=*/0.5};
  const double T = 0.5;
  std::vector<double> k;
  std::vector<double> iv;
  for (int i = 0; i <= 20; ++i) {
    const double kk = -0.25 + 0.5 * static_cast<double>(i) / 20.0;
    k.push_back(kk);
    iv.push_back(s3_iv(kk, T, truth));
  }
  const auto res = s3_seed_from_ivs(k, iv, T);
  ASSERT_TRUE(res.has_value());
  const S3Params got = res.value();
  EXPECT_NEAR(got.sigma0, truth.sigma0, 0.03 * truth.sigma0);
  EXPECT_NEAR(got.s2, truth.s2, 0.03 * std::fabs(truth.s2));
  EXPECT_NEAR(got.c2, truth.c2, 0.03 * truth.c2);
}

TEST(S3, Seed_LengthMismatch_ReturnsInvalidArgument) {
  const std::vector<double> k{-0.1, 0.0, 0.1};
  const std::vector<double> iv{0.2, 0.2};
  const auto res = s3_seed_from_ivs(k, iv, 0.5);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(S3, Seed_NonPositiveT_ReturnsInvalidArgument) {
  const std::vector<double> k{-0.1, 0.0, 0.1};
  const std::vector<double> iv{0.21, 0.20, 0.21};
  const auto res = s3_seed_from_ivs(k, iv, 0.0);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

}  // namespace
