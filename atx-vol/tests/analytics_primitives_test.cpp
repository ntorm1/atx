// Tests for the single-surface analytics primitives (analytics.hpp).
//
// Synthetic in-memory eSSVI surfaces (support/analytics_fixture.hpp) give known
// analytic properties: the flat surface has iv==sigma at every (K,T) so every
// wing/skew/term-structure statistic has a closed form; the skewed surface has a
// genuine downside skew (rho<0). The dispersion correlation helpers are pure
// algebra checked against hand-computed baskets.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/american.hpp"  // al_fast_opts, AmericanMethod
#include "atx/vol/analytics.hpp"
#include "atx/vol/event_vol.hpp"
#include "atx/vol/priced_surface.hpp"
#include "atx/vol/surface_parity.hpp" // SliceContext
#include "atx/vol/vol_curve.hpp"      // CurveSurface, EssviCurve
#include "atx/vol/vol_surface.hpp"    // EssviParams
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

  // FIX-E M-5: the bundle RECORDS the convention it was produced under. Without
  // it the emitted artifact is a set of wing numbers with no statement of what
  // "25 delta" meant — which is where AN-P2-6's fragmentation actually bites.
  EXPECT_EQ(a->delta_convention, DeltaConvention::American);
  EXPECT_EQ(b->delta_convention, DeltaConvention::Forward);
}

// ── FIX-E I-1: `compute_surface_diff` must honour the same knob ──────────────
//
// The diff resolved its four 25Δ wings with the convention-free `vol_at_delta`
// overload while reading the SAME `cfg` for `skew_k_ref` eight lines later. So
// one `AnalyticsConfig` produced a B76-forward analytics bundle and an American
// diff bundle in the same TU, and `d_vol_fixed_delta` / `d_risk_reversal_25` /
// `d_butterfly_25` are all exported (analytics_io.cpp) — two shipped artifacts
// disagreeing on convention, which is a fresh instance of exactly the
// fragmentation AN-P2-6 exists to cure.
TEST(AnalyticsPrimitives, SurfaceDiffHonorsDeltaConventionConfig) {
  // Same underlying (uid must match), skewed so the two conventions resolve
  // genuinely different strikes; `vol_bump` gives the diff something to measure.
  const PricedSurface a = testkit::make_skewed_surface(5, 100.0, 100.0, testkit::kFixtureNow, 0.0);
  const PricedSurface b =
      testkit::make_skewed_surface(5, 102.0, 102.0, testkit::kFixtureNow + testkit::kDayNs, 0.01);

  AnalyticsConfig cfg_am;
  cfg_am.compute_rnd = false;
  cfg_am.compute_varswap = false;
  AnalyticsConfig cfg_b76 = cfg_am;
  cfg_b76.delta_convention = DeltaConvention::Forward;

  const auto d_am = compute_surface_diff(a, b, cfg_am);
  const auto d_b76 = compute_surface_diff(a, b, cfg_b76);
  ASSERT_TRUE(d_am.has_value()) << d_am.error().to_string();
  ASSERT_TRUE(d_b76.has_value()) << d_b76.error().to_string();
  ASSERT_EQ(d_am->tenors.size(), d_b76->tenors.size());

  // The recorded provenance (M-5).
  EXPECT_EQ(d_am->delta_convention, DeltaConvention::American);
  EXPECT_EQ(d_b76->delta_convention, DeltaConvention::Forward);

  bool any_valid = false;
  bool any_differs = false;
  bool fixed_strike_identical = true;
  for (std::size_t i = 0; i < d_am->tenors.size(); ++i) {
    const TenorDiff &x = d_am->tenors[i];
    const TenorDiff &y = d_b76->tenors[i];
    if (!x.valid || !y.valid) {
      continue;
    }
    if (std::isfinite(x.d_risk_reversal_25) && std::isfinite(y.d_risk_reversal_25) &&
        std::isfinite(x.d_vol_fixed_delta) && std::isfinite(y.d_vol_fixed_delta)) {
      any_valid = true;
      if (x.d_risk_reversal_25 != y.d_risk_reversal_25 ||
          x.d_vol_fixed_delta != y.d_vol_fixed_delta) {
        any_differs = true;
      }
    }
    // CONTROL: the non-delta fields are convention-independent, so a difference
    // there would mean the test is measuring something other than the knob.
    if (std::isfinite(x.d_vol_fixed_strike) && x.d_vol_fixed_strike != y.d_vol_fixed_strike) {
      fixed_strike_identical = false;
    }
  }
  EXPECT_TRUE(any_valid) << "fixture produced no comparable tenor";
  EXPECT_TRUE(any_differs) << "compute_surface_diff ignored cfg.delta_convention";
  EXPECT_TRUE(fixed_strike_identical)
      << "a non-delta diff field moved with the convention — the knob reached too far";
}

// ── FIX-E I-3 / I-4: the forward-delta convergence gate ─────────────────────
//
// The B76-forward strike solve is an undamped fixed point in log-strike whose
// contraction factor is |(−z + v)·(dσ/dk)·√T|. On an eSSVI slice the at-the-
// money slope is dσ/dk = σ·ρ·φ/2, so `phi` is a direct dial on that factor —
// which makes this fixture a controlled test of the CONVERGENCE CRITERION
// rather than of any particular surface.
//
// The criterion had no test at all: `ce70b6c` added the `converged` guard and
// touched `derivatives_test.cpp` / `multiname_pipeline_test.cpp` only. Its first
// cut (1e-14 in ln K, 64 steps) demanded a contraction factor below ≈0.60 and
// returned `Err` — hence a NaN RR/BF cell — on steeper single-name smiles that
// converge perfectly well. Both sides are covered here: a steep wing that MUST
// come back with a value, and a genuinely divergent solve that MUST come back as
// an error rather than a plausible-looking strike.
[[nodiscard]] PricedSurface make_steep_smile_surface(std::uint32_t uid, double fwd,
                                                     double sigma_atm, double phi, double rho,
                                                     const std::vector<double> &Ts) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  std::uint16_t i = 0;
  for (const double T : Ts) {
    EssviParams e{};
    e.theta = sigma_atm * sigma_atm * T; // ATM total variance ⇒ iv(F,T) == sigma_atm
    e.phi = phi;
    e.rho = rho;
    e.psi = 0.5;
    e.p = 0.5;
    e.lambda = 0.5;
    e.T = T;
    e.F = fwd;
    e.expiry_id = i;
    cs.push(std::make_unique<EssviCurve>(e, std::exp(-testkit::kFixtureRate * T)));
    ctx.push_back(SliceContext{T, fwd, 0.0, 0.0, 250, 7});
    ++i;
  }
  PricingContext pc;
  pc.S = fwd;
  pc.r = testkit::kFixtureRate;
  pc.now_ts_ns = testkit::kFixtureNow;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return testkit::unwrap_surface(PricedSurface::create(std::move(cs), std::move(ctx), pc));
}

// The 25Δ CALL is the discriminating side: z = N^-1(0.25) < 0, so the factor
// |(−z + v)| ≈ 0.67 + v ADDS rather than nearly cancelling (on the put side
// z > 0 and (−z + v) is a difference that collapses towards 0 once the wing vol
// rises). ρ > 0 puts the steep side of the eSSVI smile under that call.
constexpr double kZCall25 = -0.6744897501960817; // N^-1(0.25)

// AN INDEPENDENT REPLAY OF THE FIXED POINT, off the surface, counting how many
// steps it needs to reach each tolerance. This is what makes the two tests
// below non-vacuous WITHOUT restating the criterion under test: it measures the
// input's difficulty directly, so "the old 1e-14/64-step criterion could not
// reach this, the new 1e-10/128-step one can" becomes a MEASUREMENT rather than
// an assertion about constants.
struct StepCounts {
  int to_1e14{-1}; // steps to a 1e-14 log-strike step (-1 == never, within cap)
  int to_1e10{-1}; // steps to a 1e-10 log-strike step (-1 == never, within cap)
};

[[nodiscard]] StepCounts forward_delta_step_counts(const PricedSurface &ps, double T, double z,
                                                   int cap) {
  const double F = ps.forward_at(T);
  const double sqrt_T = std::sqrt(T);
  double K = F;
  StepCounts out;
  for (int i = 1; i <= cap; ++i) {
    const double sigma = ps.iv(K, T);
    if (!std::isfinite(sigma) || sigma <= 0.0) {
      break;
    }
    const double v = sigma * sqrt_T;
    const double K_next = F * std::exp(-z * v + 0.5 * v * v);
    if (!std::isfinite(K_next) || K_next <= 0.0) {
      break;
    }
    const double step = std::fabs(std::log(K_next / K));
    K = K_next;
    if (out.to_1e10 < 0 && step <= 1.0e-10) {
      out.to_1e10 = i;
    }
    if (step <= 1.0e-14) {
      out.to_1e14 = i;
      break;
    }
  }
  return out;
}

// A steep smile that CONVERGES. Synthetic and deliberately extreme — the point
// is to exercise the convergence criterion, not to claim a market-realistic
// board — but it is a perfectly well-posed solve with a genuine fixed point, and
// the pre-FIX-E criterion refused it.
TEST(AnalyticsPrimitives, ForwardDeltaSolveSucceedsOnASteepSmileWing) {
  constexpr double kT = 0.10;
  const std::vector<double> Ts = {0.05, 0.10, 0.25};
  const PricedSurface ps = make_steep_smile_surface(31, 100.0, 0.50, 30.0, 0.9, Ts);

  const auto k = strike_at_delta(ps, kT, Side::Call, 0.25, DeltaConvention::Forward);
  ASSERT_TRUE(k.has_value()) << "a usable steep-smile wing was refused: " << k.error().to_string();
  EXPECT_GT(*k, ps.forward_at(kT)); // a 25Δ call is struck above the forward

  // The answer really satisfies the forward-delta fixed point, so "converged"
  // means converged and not merely "stopped".
  const double F = ps.forward_at(kT);
  const double v = ps.iv(*k, kT) * std::sqrt(kT);
  const double k_fixed_point = F * std::exp(-kZCall25 * v + 0.5 * v * v);
  EXPECT_NEAR(*k, k_fixed_point, 1.0e-8 * *k);

  // NON-VACUITY. Replay the fixed point independently: the OLD criterion
  // (1e-14 in ln K within 64 steps) does not reach this input, so before FIX-E
  // I-3 this wing came back as `Err` and `value_or_nan` turned the RR/BF cell
  // into NaN. The new criterion does reach it, comfortably inside its budget.
  const StepCounts steps = forward_delta_step_counts(ps, kT, kZCall25, 4096);
  std::printf("[fix-e I-3] steep wing K=%.6f steps_to_1e10=%d steps_to_1e14=%d\n", *k,
              steps.to_1e10, steps.to_1e14);
  EXPECT_TRUE(steps.to_1e14 < 0 || steps.to_1e14 > 64)
      << "fixture is not hard enough: the retired 1e-14/64-step gate already reached it in "
      << steps.to_1e14 << " steps";
  ASSERT_GT(steps.to_1e10, 0) << "fixture does not converge at all — wrong test";
  EXPECT_LE(steps.to_1e10, 128) << "fixture does not fit the shipped 128-step budget";
}

// A genuinely DIVERGENT solve: no tolerance is reachable, so the only honest
// answer is an error rather than the last iterate.
TEST(AnalyticsPrimitives, ForwardDeltaSolveReportsNonConvergenceInsteadOfALastIterate) {
  constexpr double kT = 0.50;
  const std::vector<double> Ts = {0.25, 0.50, 1.00};
  const PricedSurface ps = make_steep_smile_surface(32, 100.0, 0.30, 200.0, 0.9, Ts);

  const auto k = strike_at_delta(ps, kT, Side::Call, 0.25, DeltaConvention::Forward);
  EXPECT_FALSE(k.has_value()) << "a non-convergent solve returned a plausible-looking strike: "
                              << (k.has_value() ? *k : 0.0);
  if (!k.has_value()) {
    // FIX-E M-3: a solve failure is `Unavailable`, not `InvalidArgument` —
    // nothing about the ARGUMENTS was invalid.
    std::printf("[fix-e I-4] non-convergent code=%d msg=%s\n", static_cast<int>(k.error().code()),
                k.error().to_string().c_str());
    EXPECT_EQ(k.error().code(), ErrorCode::Unavailable);
  }

  // NON-VACUITY: the input really is beyond any tolerance, not merely beyond the
  // shipped budget. 4096 replay steps reach neither 1e-10 nor 1e-14, so widening
  // the iteration budget — the fix this one deliberately is NOT — would not help.
  const StepCounts steps = forward_delta_step_counts(ps, kT, kZCall25, 4096);
  EXPECT_LT(steps.to_1e10, 0) << "fixture converges in " << steps.to_1e10
                              << " steps; it is slow, not divergent";
  EXPECT_LT(steps.to_1e14, 0);

  // The aggregate turns that error into a NaN cell rather than a number.
  const auto rr = risk_reversal(ps, kT, 0.25, DeltaConvention::Forward);
  EXPECT_FALSE(rr.has_value());
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
