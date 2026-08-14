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

#include "atx/vol/api/analytics/analytics.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
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
  EXPECT_LT(std::fabs(r.bkm_skew), 0.05);

  // BKM variance ≈ sigma²·T = 0.04·0.5 = 0.02 for the flat surface.
  EXPECT_NEAR(r.bkm_variance, 0.02, 1e-3);
}

// Closed-form lognormal moments on the flat surface (F=100, σ=0.20, T=0.5, so
// s² = σ²T = 0.02). Every reference below is exact for a lognormal(σ√T) RND; the
// uniform-in-k Simpson grid with 6σ tail coverage must reproduce them tightly.
TEST(AnalyticsDensity, FlatRndClosedFormMoments) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const auto res = risk_neutral_density(ps, 0.5, RndConfig{});
  ASSERT_TRUE(res.has_value());
  const RiskNeutralDensity &r = *res;

  // Price-space moments: mean = F, var = F²(e^{s²}−1), lognormal skew/kurtosis.
  EXPECT_NEAR(r.mean, 100.0, 0.3);
  EXPECT_NEAR(r.variance, 202.013, 3.0);  // F²(e^{s²}−1)
  EXPECT_NEAR(r.skewness, 0.42927, 0.03); // (e^{s²}+2)√(e^{s²}−1)
  EXPECT_NEAR(r.kurtosis, 3.32938, 0.05); // e^{4s²}+2e^{3s²}+3e^{2s²}−3 (raw)

  // BKM (log-return) strip: variance = σ²T, symmetric ⇒ skew ≈ 0, kurt ≈ 3.
  EXPECT_NEAR(r.bkm_variance, 0.02, 1e-3);
  EXPECT_NEAR(r.bkm_skew, 0.0, 0.05);
  EXPECT_NEAR(r.bkm_kurt, 3.0, 0.1);

  // Raw grid mass integrates to unit probability before normalization.
  EXPECT_NEAR(r.mass_before_norm, 1.0, 0.02);

  // Centered CDF: monotone, pinned at the ends.
  ASSERT_GT(r.cdf.size(), 0u);
  EXPECT_NEAR(r.cdf.front(), 0.0, 0.02);
  EXPECT_NEAR(r.cdf.back(), 1.0, 1e-6);
  for (std::size_t i = 1; i < r.cdf.size(); ++i) {
    EXPECT_GE(r.cdf[i], r.cdf[i - 1]);
  }
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

// Inverse-CDF quantiles vs the exact lognormal K_p = F·exp(−½σ²T + σ√T·Φ⁻¹(p)).
// The centered CDF (cumulative trapezoid, no left-Riemann half-cell bias) should
// land each quantile within ±0.6 in strike; a wider miss is a centering bug, not
// a tolerance to loosen.
TEST(AnalyticsDensity, FlatQuantilesClosedForm) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const auto res = risk_neutral_density(ps, 0.5, RndConfig{});
  ASSERT_TRUE(res.has_value());
  const RiskNeutralDensity &r = *res;

  // Aligned to the default RndConfig quantiles {0.05, 0.25, 0.50, 0.75, 0.95}.
  const double expected[] = {78.458, 90.098, 99.005, 108.914, 124.934};
  ASSERT_EQ(r.quantile_k.size(), 5u);
  for (std::size_t j = 0; j < 5; ++j) {
    EXPECT_NEAR(r.quantile_k[j], expected[j], 0.6) << "quantile index " << j;
  }
}

// prob_below_forward pins the CDF-centering fix: for a lognormal(σ√T) RND,
// P(S_T ≤ F) = Φ(½σ√T) = Φ(0.0707107) = 0.528188.
TEST(AnalyticsDensity, FlatProbBelowForwardCentered) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const auto res = risk_neutral_density(ps, 0.5, RndConfig{});
  ASSERT_TRUE(res.has_value());
  EXPECT_NEAR(res->prob_below_forward, 0.528188, 3e-3);
}

TEST(AnalyticsDensity, FlatVarSwapMatchesLevel) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.5;
  const auto vs = var_swap_vol(ps, T, RndConfig{});
  ASSERT_TRUE(vs.has_value());
  EXPECT_NEAR(*vs, 0.20, 2e-3);

  // Flat smile ⇒ no convexity premium over ATMF.
  EXPECT_NEAR(*vs - atmf(ps, T), 0.0, 2e-3);

  // The density's var_swap_vol is computed off the SAME shared grid/strip and
  // must equal the standalone primitive bit-for-bit.
  const auto res = risk_neutral_density(ps, T, RndConfig{});
  ASSERT_TRUE(res.has_value());
  EXPECT_DOUBLE_EQ(res->var_swap_vol, *vs);
}

// implied_cdf on the flat surface is exact: P(S_T ≤ F) = Φ(½σ√T) = 0.528188.
TEST(AnalyticsDensity, FlatImpliedCdfClosedForm) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double cdf_at_f = implied_cdf(ps, 0.5, 100.0, RndConfig{});
  ASSERT_TRUE(std::isfinite(cdf_at_f));
  EXPECT_NEAR(cdf_at_f, 0.528188, 5e-4);
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

// The fixture is fitted out to a 1y pillar; a 2y tenor is served by a FLAT-
// EXTRAPOLATED smile, so the density must flag `extrapolated`. An in-range tenor
// (0.5) sits inside the pillar span and stays un-flagged.
TEST(AnalyticsDensity, SkewedExtrapolatedFlagBeyondPillars) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);

  const auto in_range = risk_neutral_density(ps, 0.5, RndConfig{});
  ASSERT_TRUE(in_range.has_value());
  EXPECT_FALSE(in_range->extrapolated);

  const auto beyond = risk_neutral_density(ps, 2.0, RndConfig{});
  ASSERT_TRUE(beyond.has_value());
  EXPECT_TRUE(beyond->extrapolated);
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
