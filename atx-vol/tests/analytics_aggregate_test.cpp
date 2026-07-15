// Tests for the aggregators + two-surface diff (analytics.hpp).
//
// Synthetic in-memory eSSVI surfaces (support/analytics_fixture.hpp) give known
// analytic properties: the flat surface (iv==sigma at every (K,T)) pins the ATMF
// term structure, the wing/skew statistics (all zero), the convexity premium, and
// the expected move to closed forms; the skewed surface (rho<0) pins the SIGN of
// the tenor skew and risk reversal. The earnings solver is checked on a hand-made
// two-pillar surface whose long slice embeds a known per-event eMove.

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/analytics.hpp"
#include "atx/vol/event_vol.hpp"
#include "atx/vol/priced_surface.hpp"
#include "support/analytics_fixture.hpp"

namespace atx::vol {
namespace {

// A φ=0 eSSVI PricedSurface with per-slice total variance θ_i (w(k)=θ, so
// iv(K,T)=sqrt(θ/T) at every strike). Mirrors testkit::make_flat_surface but
// takes an explicit θ per pillar, so a long slice can embed an earnings lump
// θ = σ²·T + n·eMove².
[[nodiscard]] PricedSurface make_theta_surface(std::uint32_t uid, double S, double fwd,
                                               const std::vector<double> &Ts,
                                               const std::vector<double> &thetas) {
  CurveSurface cs;
  std::vector<SliceContext> ctx;
  std::uint16_t i = 0;
  for (std::size_t s = 0; s < Ts.size(); ++s) {
    const double T = Ts[s];
    EssviParams e{};
    e.theta = thetas[s];
    e.phi = 0.0;
    e.rho = 0.0;
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
  pc.S = S;
  pc.r = testkit::kFixtureRate;
  pc.now_ts_ns = testkit::kFixtureNow;
  pc.method = AmericanMethod::AndersenLake;
  pc.al_opts = al_fast_opts();
  pc.uid = uid;
  return testkit::unwrap_surface(PricedSurface::create(std::move(cs), std::move(ctx), pc));
}

[[nodiscard]] const TenorAnalytics *find_tenor(const SurfaceAnalytics &a,
                                               const std::string &label) {
  for (const TenorAnalytics &t : a.tenors) {
    if (t.label == label) {
      return &t;
    }
  }
  return nullptr;
}

[[nodiscard]] const TenorDiff *find_tenor(const SurfaceDiff &d, const std::string &label) {
  for (const TenorDiff &t : d.tenors) {
    if (t.label == label) {
      return &t;
    }
  }
  return nullptr;
}

TEST(AnalyticsAggregate, DefaultConfigHasTenors) {
  const AnalyticsConfig cfg{};
  EXPECT_FALSE(cfg.tenors.tenors_years.empty());
}

// ── Single-surface bundle: flat lognormal reference ─────────────────────────

TEST(AnalyticsAggregate, FlatSurfaceAnalytics) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const auto res = compute_surface_analytics(ps);
  ASSERT_TRUE(res.has_value());
  const SurfaceAnalytics &a = *res;

  EXPECT_TRUE(a.valid);
  EXPECT_EQ(a.uid, 1u);
  EXPECT_NEAR(a.spot, 100.0, 1e-9);

  const TenorAnalytics *t3m = find_tenor(a, "3m");
  ASSERT_NE(t3m, nullptr);
  ASSERT_TRUE(t3m->valid);
  EXPECT_NEAR(t3m->atm_vol, 0.20, 1e-3);
  EXPECT_NEAR(t3m->skew_slope, 0.0, 1e-3);
  EXPECT_NEAR(t3m->convexity_premium, 0.0, 0.02);
  EXPECT_NEAR(t3m->expected_move, 0.79788 * t3m->atm_vol * std::sqrt(t3m->tenor_years), 1e-6);

  // Risk-neutral densities computed and valid (default rnd_tenors_years).
  ASSERT_FALSE(a.densities.empty());
  for (const RiskNeutralDensity &d : a.densities) {
    EXPECT_TRUE(d.valid);
  }

  // Flat surface ⇒ flat term structure.
  EXPECT_NEAR(a.ts_slope_1m_3m, 0.0, 1e-3);

  // At least one standard-grid tenor is in-domain (a.valid is the any-valid flag).
  std::size_t n_valid = 0;
  for (const TenorAnalytics &t : a.tenors) {
    if (t.valid) {
      ++n_valid;
    }
  }
  EXPECT_GT(n_valid, 0u);
}

// ── Single-surface bundle: skewed surface sign checks ───────────────────────

TEST(AnalyticsAggregate, SkewedSurfaceTenorShape) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  const auto res = compute_surface_analytics(ps);
  ASSERT_TRUE(res.has_value());

  const TenorAnalytics *t3m = find_tenor(*res, "3m");
  ASSERT_NE(t3m, nullptr);
  ASSERT_TRUE(t3m->valid);
  // Downside skew ⇒ ∂σ/∂k < 0 and σ(25Δ put) > σ(25Δ call) ⇒ RR > 0.
  EXPECT_LT(t3m->skew_slope, 0.0);
  ASSERT_FALSE(t3m->risk_reversal.empty());
  EXPECT_GT(t3m->risk_reversal[0], 0.0);
}

// ── Two-surface diff: pure level shift ──────────────────────────────────────

TEST(AnalyticsAggregate, SurfaceDiffFlatLevelShift) {
  const PricedSurface a = testkit::make_flat_surface(7, 100.0, 100.0, 0.20);
  const PricedSurface b = testkit::make_flat_surface(7, 100.0, 100.0, 0.22);
  const auto res = compute_surface_diff(a, b);
  ASSERT_TRUE(res.has_value());
  const SurfaceDiff &d = *res;

  EXPECT_TRUE(d.valid);
  EXPECT_NEAR(d.log_return, 0.0, 1e-12); // same spot

  const TenorDiff *t3m = find_tenor(d, "3m");
  ASSERT_NE(t3m, nullptr);
  ASSERT_TRUE(t3m->valid);
  EXPECT_NEAR(t3m->d_atm_vol, 0.02, 1e-3);
  EXPECT_NEAR(t3m->d_vol_fixed_strike, 0.02, 1e-3);
  EXPECT_NEAR(t3m->d_skew_slope, 0.0, 1e-3);

  // Flat skew ⇒ no sticky-strike prediction ⇒ residual is the whole ATM move.
  EXPECT_NEAR(d.residual_atm_move, 0.02, 1e-3);
}

TEST(AnalyticsAggregate, SurfaceDiffMismatchedUidIsError) {
  const PricedSurface a = testkit::make_flat_surface(7, 100.0, 100.0, 0.20);
  const PricedSurface b = testkit::make_flat_surface(8, 100.0, 100.0, 0.22);
  const auto res = compute_surface_diff(a, b);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── Earnings implied move ────────────────────────────────────────────────────

TEST(AnalyticsAggregate, EarningsImpliedMoveRecoversEmbeddedMove) {
  // Two pillars sharing σ_C = 0.20; the long slice carries one event with a
  // known eMove = 0.06 (θ₁ = σ²·T₁ + eMove²). The event at 0.075 lands in
  // (now, 0.10] but not (now, 0.05], so the (0.05, 0.10) pair brackets it.
  const std::vector<double> Ts = {0.05, 0.10};
  const std::vector<double> thetas = {
      0.20 * 0.20 * 0.05,
      0.20 * 0.20 * 0.10 + 0.06 * 0.06,
  };
  const PricedSurface ps = make_theta_surface(3, 100.0, 100.0, Ts, thetas);
  const EventSchedule sched = testkit::make_event_schedule(0.075);
  EventContext ctx;
  ctx.schedule = &sched;

  const auto e = earnings_implied_move(ps, ctx);
  ASSERT_TRUE(e.has_value());
  EXPECT_NEAR(*e, 0.06, 5e-3);
}

TEST(AnalyticsAggregate, EarningsImpliedMoveFlatSurfaceIsZero) {
  // No event lump: both slices are pure diffusion at the same σ, so the solved
  // e² clamps to ~0.
  const PricedSurface ps = testkit::make_flat_surface(4, 100.0, 100.0, 0.20);
  const EventSchedule sched = testkit::make_event_schedule(0.075);
  EventContext ctx;
  ctx.schedule = &sched;

  const auto e = earnings_implied_move(ps, ctx);
  ASSERT_TRUE(e.has_value());
  EXPECT_NEAR(*e, 0.0, 1e-3);
}

TEST(AnalyticsAggregate, EarningsImpliedMoveNullScheduleIsError) {
  const PricedSurface ps = testkit::make_flat_surface(4, 100.0, 100.0, 0.20);
  EventContext ctx; // schedule == nullptr
  const auto e = earnings_implied_move(ps, ctx);
  ASSERT_FALSE(e.has_value());
  EXPECT_EQ(e.error().code(), ErrorCode::InvalidArgument);
}

} // namespace
} // namespace atx::vol
