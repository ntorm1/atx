// Tests for the single-surface analytics primitives (analytics.hpp).
//
// Synthetic in-memory eSSVI surfaces (support/analytics_fixture.hpp) give known
// analytic properties: the flat surface has iv==sigma at every (K,T) so every
// wing/skew/term-structure statistic has a closed form; the skewed surface has a
// genuine downside skew (rho<0). The dispersion correlation helpers are pure
// algebra checked against hand-computed baskets.

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/analytics.hpp"
#include "atx/vol/event_vol.hpp"
#include "atx/vol/priced_surface.hpp"
#include "support/analytics_fixture.hpp"

namespace atx::vol {
namespace {

TEST(AnalyticsPrimitives, StandardTenorGridShape) {
  const TenorGrid g = TenorGrid::standard();
  EXPECT_EQ(g.tenors_years.size(), g.labels.size());
  EXPECT_FALSE(g.tenors_years.empty());
}

// Validates the shared fixture: a φ=0 eSSVI surface is flat in log-moneyness.
TEST(AnalyticsPrimitives, FlatFixtureIsFlat) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.35;
  const double F = ps.forward_at(T);
  EXPECT_NEAR(F, 100.0, 1e-9);
  const double atm = ps.iv(F, T);
  EXPECT_NEAR(atm, 0.20, 1e-6);
  EXPECT_NEAR(ps.iv(F * std::exp(0.10), T), 0.20, 1e-6);
  EXPECT_NEAR(ps.iv(F * std::exp(-0.10), T), 0.20, 1e-6);
}

// ── atmf_forward / atmf_vol ─────────────────────────────────────────────────

TEST(AnalyticsPrimitives, AtmfForwardAndVolFlat) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.35;
  EXPECT_NEAR(atmf_forward(ps, T), 100.0, 1e-9);
  EXPECT_NEAR(atmf_vol(ps, T), 0.20, 1e-6);
}

TEST(AnalyticsPrimitives, AtmfForwardAndVolRejectBadT) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  EXPECT_TRUE(std::isnan(atmf_forward(ps, 0.0)));
  EXPECT_TRUE(std::isnan(atmf_forward(ps, -1.0)));
  EXPECT_TRUE(std::isnan(atmf_forward(ps, std::numeric_limits<double>::quiet_NaN())));
  EXPECT_TRUE(std::isnan(atmf_forward(ps, std::numeric_limits<double>::infinity())));
  EXPECT_TRUE(std::isnan(atmf_vol(ps, 0.0)));
  EXPECT_TRUE(std::isnan(atmf_vol(ps, -0.5)));
}

// ── vol_at_delta ────────────────────────────────────────────────────────────

TEST(AnalyticsPrimitives, VolAtDeltaFlatBothSides) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.35;
  const auto put = vol_at_delta(ps, T, Side::Put, 0.25);
  const auto call = vol_at_delta(ps, T, Side::Call, 0.25);
  ASSERT_TRUE(put.has_value());
  ASSERT_TRUE(call.has_value());
  EXPECT_NEAR(*put, 0.20, 1e-6);
  EXPECT_NEAR(*call, 0.20, 1e-6);
}

TEST(AnalyticsPrimitives, VolAtDeltaRejectsOutOfRange) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.35;
  for (const double bad : {0.0, 1.0, -0.1, 1.5}) {
    const auto r = vol_at_delta(ps, T, Side::Put, bad);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
  }
}

// ── vol_at_moneyness ────────────────────────────────────────────────────────

TEST(AnalyticsPrimitives, VolAtMoneynessFlat) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.35;
  EXPECT_NEAR(vol_at_moneyness(ps, T, 0.90), 0.20, 1e-6);
  EXPECT_NEAR(vol_at_moneyness(ps, T, 1.00), 0.20, 1e-6);
  EXPECT_NEAR(vol_at_moneyness(ps, T, 1.10), 0.20, 1e-6);
  EXPECT_TRUE(std::isnan(vol_at_moneyness(ps, 0.0, 1.0)));
  EXPECT_TRUE(std::isnan(vol_at_moneyness(ps, T, 0.0)));
  EXPECT_TRUE(std::isnan(vol_at_moneyness(ps, T, -1.0)));
}

// ── risk_reversal / butterfly ───────────────────────────────────────────────

TEST(AnalyticsPrimitives, RiskReversalAndButterflyFlatAreZero) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.35;
  const auto rr = risk_reversal(ps, T, 0.25);
  const auto bf = butterfly(ps, T, 0.25);
  ASSERT_TRUE(rr.has_value());
  ASSERT_TRUE(bf.has_value());
  EXPECT_NEAR(*rr, 0.0, 1e-6);
  EXPECT_NEAR(*bf, 0.0, 1e-6);
}

TEST(AnalyticsPrimitives, RiskReversalPositiveOnDownsideSkew) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  const double T = 0.35;
  const auto rr = risk_reversal(ps, T, 0.25);
  ASSERT_TRUE(rr.has_value());
  // rho < 0 ⇒ downside rich ⇒ σ(put) > σ(call) ⇒ RR > 0.
  EXPECT_GT(*rr, 0.0);
}

// ── skew_curvature ──────────────────────────────────────────────────────────

TEST(AnalyticsPrimitives, SkewCurvatureFlatIsFlat) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const SkewCurvature sc = skew_curvature(ps, 0.35, 0.10);
  ASSERT_TRUE(sc.valid);
  EXPECT_NEAR(sc.atm, 0.20, 1e-6);
  EXPECT_NEAR(sc.skew_slope, 0.0, 1e-6);
  EXPECT_NEAR(sc.curvature, 0.0, 1e-6);
}

TEST(AnalyticsPrimitives, SkewCurvatureNegativeSlopeOnDownsideSkew) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  const SkewCurvature sc = skew_curvature(ps, 0.35, 0.10);
  ASSERT_TRUE(sc.valid);
  // Downside skew: iv higher at lower strikes (k<0) ⇒ ∂σ/∂k < 0.
  EXPECT_LT(sc.skew_slope, 0.0);
}

TEST(AnalyticsPrimitives, SkewCurvatureInvalidForBadArgs) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  EXPECT_FALSE(skew_curvature(ps, 0.0, 0.10).valid);
  EXPECT_FALSE(skew_curvature(ps, 0.35, 0.0).valid);
  EXPECT_FALSE(skew_curvature(ps, 0.35, -0.10).valid);
}

// ── forward_vol ─────────────────────────────────────────────────────────────

TEST(AnalyticsPrimitives, ForwardVolFlatBetweenTenors) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  // Both pillars of the fixture grid; flat total variance ⇒ forward vol == sigma.
  EXPECT_NEAR(forward_vol(ps, 0.35, 0.50), 0.20, 1e-6);
}

TEST(AnalyticsPrimitives, ForwardVolNaNWhenNotOrdered) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  EXPECT_TRUE(std::isnan(forward_vol(ps, 0.50, 0.35))); // T2 < T1
  EXPECT_TRUE(std::isnan(forward_vol(ps, 0.35, 0.35))); // T2 == T1
  EXPECT_TRUE(std::isnan(forward_vol(ps, 0.0, 0.50)));  // T1 <= 0
}

// ── atmf_vol_ex_earnings ────────────────────────────────────────────────────

TEST(AnalyticsPrimitives, AtmfVolExEarningsCensorsOneEvent) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.50;
  const EventSchedule sched = testkit::make_event_schedule(0.25);
  EventContext ctx;
  ctx.schedule = &sched;
  ctx.implied_emove = 0.06;

  // w = sigma²·T = 0.02; one event in (now, T]; wc = w − 1·eMove².
  const double expected = std::sqrt((0.20 * 0.20 * 0.50 - 0.06 * 0.06) / 0.50);
  EXPECT_NEAR(atmf_vol_ex_earnings(ps, T, ctx), expected, 1e-6);
  // Sanity: stripping earnings lowers the ATM vol below the raw 0.20.
  EXPECT_LT(atmf_vol_ex_earnings(ps, T, ctx), 0.20);
}

TEST(AnalyticsPrimitives, AtmfVolExEarningsNaNWithoutSchedule) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  EventContext ctx; // schedule == nullptr, implied_emove == 0
  EXPECT_TRUE(std::isnan(atmf_vol_ex_earnings(ps, 0.50, ctx)));

  const EventSchedule sched = testkit::make_event_schedule(0.25);
  EventContext ctx_no_move;
  ctx_no_move.schedule = &sched;
  ctx_no_move.implied_emove = 0.0; // non-positive eMove ⇒ NaN
  EXPECT_TRUE(std::isnan(atmf_vol_ex_earnings(ps, 0.50, ctx_no_move)));
}

// G9: an eMove whose event variance overshoots the total ATM variance (the censor
// would floor to ~0 and hand back a spurious near-zero vol) ⇒ NaN, not a number.
TEST(AnalyticsPrimitives, AtmfVolExEarningsNaNOnOvershoot) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const double T = 0.05; // w_atm = σ²·T = 0.04·0.05 = 0.002
  const EventSchedule sched = testkit::make_event_schedule(0.025); // one event in (now, T]
  EventContext ctx;
  ctx.schedule = &sched;
  ctx.implied_emove = 0.10; // n·eMove² = 0.01 ≥ w_atm = 0.002 ⇒ NaN
  EXPECT_TRUE(std::isnan(atmf_vol_ex_earnings(ps, T, ctx)));
}

// ── implied_correlation_clean / dirty ───────────────────────────────────────

TEST(AnalyticsPrimitives, ImpliedCorrelationCleanRecoversHalf) {
  const std::vector<double> w = {0.5, 0.5};
  const std::vector<double> var = {0.04, 0.04};
  // idx_var = Σ wᵢ²varᵢ + ρ·(Σ_{i≠j} wᵢwⱼ√(varᵢvarⱼ)) = 0.02 + 0.5·0.02 = 0.03.
  const auto rho = implied_correlation_clean(0.03, w, var);
  ASSERT_TRUE(rho.has_value());
  EXPECT_NEAR(*rho, 0.5, 1e-12);
}

TEST(AnalyticsPrimitives, ImpliedCorrelationCleanSingleNameIsError) {
  const std::vector<double> w = {1.0};
  const std::vector<double> var = {0.04};
  // denom = S1² − S2 = 0.2² − 0.04 = 0 ⇒ non-positive cross term ⇒ Err.
  const auto rho = implied_correlation_clean(0.04, w, var);
  ASSERT_FALSE(rho.has_value());
  EXPECT_EQ(rho.error().code(), ErrorCode::InvalidArgument);
}

TEST(AnalyticsPrimitives, ImpliedCorrelationCleanRejectsSizeMismatch) {
  const std::vector<double> w = {0.5, 0.5};
  const std::vector<double> var = {0.04};
  const auto mismatch = implied_correlation_clean(0.03, w, var);
  ASSERT_FALSE(mismatch.has_value());
  EXPECT_EQ(mismatch.error().code(), ErrorCode::InvalidArgument);

  const std::vector<double> empty;
  const auto emptied = implied_correlation_clean(0.03, empty, empty);
  ASSERT_FALSE(emptied.has_value());
  EXPECT_EQ(emptied.error().code(), ErrorCode::InvalidArgument);
}

TEST(AnalyticsPrimitives, ImpliedCorrelationDirtyRecoversTarget) {
  const std::vector<double> w = {0.5, 0.5};
  const std::vector<double> vol = {0.20, 0.30};
  // S1 = Σ wᵢvolᵢ = 0.25; idx_var = S1²·0.6 ⇒ ρ = idx_var / S1² = 0.6.
  const double s1 = 0.5 * 0.20 + 0.5 * 0.30;
  const double idx_var = s1 * s1 * 0.6;
  const auto rho = implied_correlation_dirty(idx_var, w, vol);
  ASSERT_TRUE(rho.has_value());
  EXPECT_NEAR(*rho, 0.6, 1e-12);
}

TEST(AnalyticsPrimitives, ImpliedCorrelationDirtyRejectsEmpty) {
  const std::vector<double> empty;
  const auto r = implied_correlation_dirty(0.03, empty, empty);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
} // namespace atx::vol
