// Tests for the risk-neutral density / model-free variance layer (analytics.hpp).
//
// The flat fixture (make_flat_surface, iv==sigma everywhere) has an exactly
// lognormal(sigma·√T) risk-neutral density, which pins the density mass, the
// mean (== forward), the near-symmetry of the log-return skew, and the model-free
// variance level (== sigma). The skewed fixture (ρ<0 downside skew) pins the
// SIGN of the risk-neutral skew (negative) and the convexity premium (positive).

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <gtest/gtest.h>

#include "atx/vol/analytics.hpp"
#include "atx/vol/priced_surface.hpp"
#include "support/analytics_fixture.hpp"

namespace atx::vol {
namespace {

// ATMF vol read straight off the surface (self-contained, no primitive dep).
[[nodiscard]] double atmf(const PricedSurface &ps, double T) { return ps.iv(ps.forward_at(T), T); }

TEST(AnalyticsDensity, RndConfigDefaults) {
  const RndConfig c{};
  EXPECT_LT(c.k_min, c.k_max);
  EXPECT_GT(c.n_grid, 0);
}

// ── Flat lognormal surface: exact analytic reference ────────────────────────

TEST(AnalyticsDensity, FlatRndShapeAndMass) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.5;
  const RndConfig cfg{};
  const auto res = risk_neutral_density(ps, T, cfg);
  ASSERT_TRUE(res.has_value());
  const RiskNeutralDensity &r = *res;

  EXPECT_TRUE(r.valid);
  EXPECT_NEAR(r.forward, 100.0, 1e-9);

  // Grid mass integrates to ~1 before normalization (density well captured).
  EXPECT_NEAR(r.mass_before_norm, 1.0, 0.02);

  // pdf is non-negative everywhere; CDF is monotone from ~0 to ~1.
  ASSERT_EQ(r.pdf.size(), r.cdf.size());
  ASSERT_GT(r.pdf.size(), 0u);
  for (double p : r.pdf) {
    EXPECT_GE(p, 0.0);
  }
  for (std::size_t i = 1; i < r.cdf.size(); ++i) {
    EXPECT_GE(r.cdf[i], r.cdf[i - 1]);
  }
  EXPECT_NEAR(r.cdf.front(), 0.0, 0.02);
  EXPECT_NEAR(r.cdf.back(), 1.0, 1e-6);
}

TEST(AnalyticsDensity, FlatRndMomentsAndBkmSkew) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.5;
  const auto res = risk_neutral_density(ps, T, RndConfig{});
  ASSERT_TRUE(res.has_value());
  const RiskNeutralDensity &r = *res;

  // Price-space mean equals the forward within 1% of F.
  EXPECT_NEAR(r.mean, r.forward, 0.01 * r.forward);

  // Lognormal log-returns are ~symmetric ⇒ BKM skew ≈ 0. (The forward-referenced
  // BKM drift is essential here — a spot-referenced drift would inject ~−0.5.)
  EXPECT_LT(std::fabs(r.bkm_skew), 0.15);

  // BKM variance ≈ sigma²·T = 0.04·0.5 = 0.02 for the flat surface.
  EXPECT_NEAR(r.bkm_variance, 0.02, 0.003);
}

TEST(AnalyticsDensity, FlatQuantilesMonotone) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const auto res = risk_neutral_density(ps, 0.5, RndConfig{});
  ASSERT_TRUE(res.has_value());
  const RiskNeutralDensity &r = *res;

  ASSERT_EQ(r.quantile_p.size(), r.quantile_k.size());
  ASSERT_GE(r.quantile_k.size(), 2u);
  for (std::size_t i = 1; i < r.quantile_k.size(); ++i) {
    EXPECT_LT(r.quantile_k[i - 1], r.quantile_k[i]); // ascending in probability
  }
  // The median quantile straddles the forward (lognormal median just below F).
  const auto mid = std::lower_bound(r.quantile_p.begin(), r.quantile_p.end(), 0.50);
  if (mid != r.quantile_p.end() && std::fabs(*mid - 0.50) < 1e-9) {
    const std::size_t j = static_cast<std::size_t>(mid - r.quantile_p.begin());
    EXPECT_LT(r.quantile_k[j], r.forward); // median < forward
    EXPECT_GT(r.quantile_k[j], 0.90 * r.forward);
  }
}

TEST(AnalyticsDensity, FlatVarSwapMatchesLevel) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.5;
  const auto vs = var_swap_vol(ps, T, RndConfig{});
  ASSERT_TRUE(vs.has_value());
  EXPECT_NEAR(*vs, 0.20, 0.01);

  // Flat smile ⇒ no convexity premium over ATMF.
  EXPECT_NEAR(*vs - atmf(ps, T), 0.0, 0.01);
}

TEST(AnalyticsDensity, FlatImpliedCdf) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.5;
  const double F = ps.forward_at(T);

  const double cdf_at_f = implied_cdf(ps, T, F, RndConfig{});
  ASSERT_TRUE(std::isfinite(cdf_at_f));
  // Median of the lognormal sits just below F ⇒ P(S_T < F) > 0.5 (but < 0.75).
  EXPECT_GT(cdf_at_f, 0.5);
  EXPECT_LT(cdf_at_f, 0.75);

  // Strictly increasing in strike.
  const double lo = implied_cdf(ps, T, 0.95 * F, RndConfig{});
  const double hi = implied_cdf(ps, T, 1.05 * F, RndConfig{});
  ASSERT_TRUE(std::isfinite(lo) && std::isfinite(hi));
  EXPECT_LT(lo, cdf_at_f);
  EXPECT_LT(cdf_at_f, hi);

  // Agrees with the density's prob_below_forward.
  const auto res = risk_neutral_density(ps, T, RndConfig{});
  ASSERT_TRUE(res.has_value());
  EXPECT_NEAR(cdf_at_f, res->prob_below_forward, 0.02);
}

TEST(AnalyticsDensity, ImpliedCdfBadArgsAreNaN) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  EXPECT_TRUE(std::isnan(implied_cdf(ps, -0.5, 100.0, RndConfig{})));
  EXPECT_TRUE(std::isnan(implied_cdf(ps, 0.5, -1.0, RndConfig{})));
}

// ── Skewed surface: sign checks ─────────────────────────────────────────────

TEST(AnalyticsDensity, SkewedRndHasNegativeSkew) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  const double T = 0.5;
  const auto res = risk_neutral_density(ps, T, RndConfig{});
  ASSERT_TRUE(res.has_value());
  // Downside (ρ<0) skew ⇒ negative risk-neutral skewness.
  EXPECT_LT(res->bkm_skew, 0.0);
}

TEST(AnalyticsDensity, SkewedVarSwapCarriesConvexityPremium) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  const double T = 0.5;
  const auto vs = var_swap_vol(ps, T, RndConfig{});
  ASSERT_TRUE(vs.has_value());
  // Convex smile ⇒ model-free vol strictly above ATMF.
  EXPECT_GT(*vs, atmf(ps, T));
}

// ── Guards ──────────────────────────────────────────────────────────────────

TEST(AnalyticsDensity, NonPositiveTIsError) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const RndConfig cfg{};

  const auto rnd0 = risk_neutral_density(ps, 0.0, cfg);
  ASSERT_FALSE(rnd0.has_value());
  EXPECT_EQ(rnd0.error().code(), ErrorCode::InvalidArgument);

  const auto rnd_neg = risk_neutral_density(ps, -0.25, cfg);
  ASSERT_FALSE(rnd_neg.has_value());
  EXPECT_EQ(rnd_neg.error().code(), ErrorCode::InvalidArgument);

  const auto vs = var_swap_vol(ps, 0.0, cfg);
  ASSERT_FALSE(vs.has_value());
  EXPECT_EQ(vs.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
} // namespace atx::vol
