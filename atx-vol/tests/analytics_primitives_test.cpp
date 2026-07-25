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

// ── E5 / AN-P2-6: the delta convention is a CHOICE, not folklore ────────────
//
// Analytics wings/RR/BF resolve their strike with `resolve_strike_by_delta`,
// i.e. AMERICAN |delta| (dP/dS on the American mark). `projection.cpp` solves
// EUROPEAN B76 FORWARD delta, and `contract_projection.cpp` solves American
// delta seeded from a carry-discounted spot-delta inversion. A "25-delta RR"
// from `compute_surface_analytics` is therefore NOT the same strike as a
// 25-delta from `surface_solve_k_for_delta`, and neither matches the
// vendor-standard Black forward delta on a high-carry name.
//
// E5 keeps American as the DEFAULT (nothing silently moves) and adds an
// explicit B76-forward mode for vendor comparability. On a FLAT-vol surface the
// B76-forward strike has an exact closed form and can be hand-computed:
//
//     N(d1) = Delta_call        =>  d1 = z = N^-1(Delta)
//     ln(F/K) = z*v - 0.5*v^2   =>  K = F*exp(-z*v + 0.5*v^2),  v = sigma*sqrt(T)
//
// and for the put, N(d1) - 1 = -Delta  =>  d1 = N^-1(1 - Delta) = -z.
TEST(AnalyticsPrimitives, B76ForwardDeltaStrikeMatchesClosedForm) {
  constexpr double kSigma = 0.20;
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, kSigma);
  const double T = 0.50;
  const double F = ps.forward_at(T);
  const double v = kSigma * std::sqrt(T);
  constexpr double kDelta = 0.25;
  // z = N^-1(0.25), hand value to full double precision.
  constexpr double kZ25 = -0.6744897501960817;

  const double k_call_expected = F * std::exp(-kZ25 * v + 0.5 * v * v);
  const double k_put_expected = F * std::exp(kZ25 * v + 0.5 * v * v);

  const auto k_call = strike_at_delta(ps, T, Side::Call, kDelta, DeltaConvention::Forward);
  const auto k_put = strike_at_delta(ps, T, Side::Put, kDelta, DeltaConvention::Forward);
  ASSERT_TRUE(k_call.has_value()) << k_call.error().to_string();
  ASSERT_TRUE(k_put.has_value()) << k_put.error().to_string();

  EXPECT_NEAR(*k_call, k_call_expected, 1e-8 * k_call_expected)
      << "got " << *k_call << " want " << k_call_expected;
  EXPECT_NEAR(*k_put, k_put_expected, 1e-8 * k_put_expected)
      << "got " << *k_put << " want " << k_put_expected;
  // Sanity: the 25-delta call sits ABOVE the forward, the put BELOW.
  EXPECT_GT(*k_call, F);
  EXPECT_LT(*k_put, F);
}

// The two conventions really do disagree — otherwise the knob is decoration.
TEST(AnalyticsPrimitives, AmericanAndB76ForwardDeltaStrikesDiffer) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  const double T = 0.50;
  const auto k_am = strike_at_delta(ps, T, Side::Put, 0.25, DeltaConvention::American);
  const auto k_b76 = strike_at_delta(ps, T, Side::Put, 0.25, DeltaConvention::Forward);
  ASSERT_TRUE(k_am.has_value()) << k_am.error().to_string();
  ASSERT_TRUE(k_b76.has_value()) << k_b76.error().to_string();
  EXPECT_NE(*k_am, *k_b76) << "american=" << *k_am << " b76fwd=" << *k_b76;

  // American is the DEFAULT: the convention-free overload must be unchanged.
  const auto k_default = strike_at_delta(ps, T, Side::Put, 0.25);
  ASSERT_TRUE(k_default.has_value()) << k_default.error().to_string();
  EXPECT_DOUBLE_EQ(*k_default, *k_am);
}

TEST(AnalyticsPrimitives, RiskReversalHonorsDeltaConvention) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  const double T = 0.50;
  const auto rr_am = risk_reversal(ps, T, 0.25, DeltaConvention::American);
  const auto rr_b76 = risk_reversal(ps, T, 0.25, DeltaConvention::Forward);
  ASSERT_TRUE(rr_am.has_value()) << rr_am.error().to_string();
  ASSERT_TRUE(rr_b76.has_value()) << rr_b76.error().to_string();
  // Downside skew ⇒ both conventions report a positive equity-sign RR ...
  EXPECT_GT(*rr_am, 0.0);
  EXPECT_GT(*rr_b76, 0.0);
  // ... but they are NOT the same number, which is the whole point of AN-P2-6.
  EXPECT_NE(*rr_am, *rr_b76) << "american=" << *rr_am << " b76fwd=" << *rr_b76;

  // Default overload == American.
  const auto rr_default = risk_reversal(ps, T, 0.25);
  ASSERT_TRUE(rr_default.has_value()) << rr_default.error().to_string();
  EXPECT_DOUBLE_EQ(*rr_default, *rr_am);
}

// The aggregate honors the config knob, so the convention reaches the product
// surface (`TenorAnalytics::risk_reversal`) and not just the primitives.
TEST(AnalyticsPrimitives, AggregateHonorsDeltaConventionConfig) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  AnalyticsConfig cfg_am;
  cfg_am.compute_rnd = false;
  cfg_am.compute_varswap = false;
  cfg_am.delta_points = {0.25};
  AnalyticsConfig cfg_b76 = cfg_am;
  cfg_b76.delta_convention = DeltaConvention::Forward;
  EXPECT_EQ(cfg_am.delta_convention, DeltaConvention::American) << "American must be the default";

  const auto a = compute_surface_analytics(ps, cfg_am);
  const auto b = compute_surface_analytics(ps, cfg_b76);
  ASSERT_TRUE(a.has_value()) << a.error().to_string();
  ASSERT_TRUE(b.has_value()) << b.error().to_string();
  ASSERT_EQ(a->tenors.size(), b->tenors.size());

  bool any_valid = false;
  bool any_differs = false;
  for (std::size_t i = 0; i < a->tenors.size(); ++i) {
    if (!a->tenors[i].valid || !b->tenors[i].valid) {
      continue;
    }
    ASSERT_EQ(a->tenors[i].risk_reversal.size(), std::size_t{1});
    ASSERT_EQ(b->tenors[i].risk_reversal.size(), std::size_t{1});
    const double ra = a->tenors[i].risk_reversal[0];
    const double rb = b->tenors[i].risk_reversal[0];
    if (std::isfinite(ra) && std::isfinite(rb)) {
      any_valid = true;
      if (ra != rb) {
        any_differs = true;
      }
    }
  }
  EXPECT_TRUE(any_valid) << "fixture produced no comparable tenor";
  EXPECT_TRUE(any_differs) << "the config knob did not reach the wing solve";
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
