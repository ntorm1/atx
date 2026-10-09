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

#include "atx/vol/api/analytics/analytics.hpp"
#include "atx/vol/api/analytics/event_vol.hpp"
#include "atx/vol/api/backtest/priced_surface.hpp"
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

// ── Extrapolation gate (G7) ─────────────────────────────────────────────────

TEST(AnalyticsAggregate, ExtrapolationGateMarksOutOfRangeTenors) {
  // Flat fixture pillars span 0.05 … 1.0, so 1w/2w (below) and 18m/2y (above) are
  // flat-extrapolated: marked extrapolated + excluded from `valid`; 3m is fitted.
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const auto res = compute_surface_analytics(ps);
  ASSERT_TRUE(res.has_value());
  const SurfaceAnalytics &a = *res;

  const TenorAnalytics *t1w = find_tenor(a, "1w");
  ASSERT_NE(t1w, nullptr);
  EXPECT_TRUE(t1w->extrapolated);
  EXPECT_FALSE(t1w->valid);

  const TenorAnalytics *t2y = find_tenor(a, "2y");
  ASSERT_NE(t2y, nullptr);
  EXPECT_TRUE(t2y->extrapolated);
  EXPECT_FALSE(t2y->valid);

  const TenorAnalytics *t3m = find_tenor(a, "3m");
  ASSERT_NE(t3m, nullptr);
  EXPECT_TRUE(t3m->valid);
  EXPECT_FALSE(t3m->extrapolated);
}

// ── RND shared-grid copy-back (G8) ──────────────────────────────────────────

TEST(AnalyticsAggregate, RndCopyBackSharesGridAtAlignedTenor) {
  const PricedSurface ps = testkit::make_skewed_surface(2, 100.0, 100.0);
  const auto res = compute_surface_analytics(ps);
  ASSERT_TRUE(res.has_value());
  const SurfaceAnalytics &a = *res;

  const TenorAnalytics *t3m = find_tenor(a, "3m");
  ASSERT_NE(t3m, nullptr);
  ASSERT_TRUE(t3m->valid);
  // The 91d default RND tenor now aligns with the 3m tenor, so its BKM skew is
  // copied back (nonzero on a genuinely skewed surface).
  EXPECT_NE(t3m->rnd_skewness, 0.0);
  EXPECT_TRUE(std::isfinite(t3m->var_swap_vol));

  // And the copied var_swap_vol is exactly the matching density's (no rebuild).
  const RiskNeutralDensity *match = nullptr;
  for (const RiskNeutralDensity &dn : a.densities) {
    if (std::fabs(dn.T - 91.0 / 365.25) < 1.5 / 365.25) {
      match = &dn;
      break;
    }
  }
  ASSERT_NE(match, nullptr);
  EXPECT_DOUBLE_EQ(t3m->var_swap_vol, match->var_swap_vol);
}

// ── Forward-vol series + normalized skew ────────────────────────────────────

TEST(AnalyticsAggregate, ForwardVolSegmentsFlatAreFlat) {
  const PricedSurface ps = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const auto res = compute_surface_analytics(ps);
  ASSERT_TRUE(res.has_value());
  const SurfaceAnalytics &a = *res;
  ASSERT_FALSE(a.forward_vol_segments.empty());
  for (const double seg : a.forward_vol_segments) {
    EXPECT_NEAR(seg, 0.20, 1e-2);
  }
}

TEST(AnalyticsAggregate, NormalizedSkewFlatZeroSkewedNegative) {
  const PricedSurface flat = testkit::make_flat_surface(1, 100.0, 100.0, 0.20);
  const auto rf = compute_surface_analytics(flat);
  ASSERT_TRUE(rf.has_value());
  const TenorAnalytics *f3m = find_tenor(*rf, "3m");
  ASSERT_NE(f3m, nullptr);
  EXPECT_NEAR(f3m->skew_slope_sqrt_t, 0.0, 1e-3);
  EXPECT_NEAR(f3m->skew_slope_norm, 0.0, 1e-3);

  const PricedSurface skew = testkit::make_skewed_surface(2, 100.0, 100.0);
  const auto rs = compute_surface_analytics(skew);
  ASSERT_TRUE(rs.has_value());
  const TenorAnalytics *s3m = find_tenor(*rs, "3m");
  ASSERT_NE(s3m, nullptr);
  EXPECT_LT(s3m->skew_slope_sqrt_t, 0.0);
  EXPECT_LT(s3m->skew_slope_norm, 0.0);
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
  // Pure level shift ⇒ the 25Δ wings move together, so the risk-reversal and
  // butterfly changes are both ~0.
  EXPECT_NEAR(t3m->d_risk_reversal_25, 0.0, 1e-3);
  EXPECT_NEAR(t3m->d_butterfly_25, 0.0, 1e-3);

  // Flat skew + equal forwards ⇒ no sticky-strike prediction ⇒ residual is the
  // whole ATM move.
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

// ── E3a / AN-P1-3: joint eMove vs the naive two-pillar solve ────────────────
//
// The AAPL-shaped case from the atmCen convention sweep: NO near expiry spans
// the event, so the only bracketing pair is WIDE (0.02y, 0.30y). `implied_emove`
// assumes one flat censored instantaneous variance across that bracket, so the
// censored term structure's own steepness inside it aliases straight into e².
// Algebraically, with n1 = 0 and n2 = 1,
//
//     e²_two-pillar = eMove² + T2·(σ_C(T2)² − σ_C(T1)²)
//
// which for this contango censored curve is a large POSITIVE bias — the sweep
// measured AAPL at +173%, and this fixture reproduces +172%.
//
// The observations are generated EXACTLY from the SpiderRock decomposition
// w_i = n_i·eMove² + σ_C(T_i)²·T_i with σ_C(T) = lt + (st−lt)·e^{−decay·T}, so
// the joint fit over ALL SIX pillars has a zero-residual solution at the truth
// and no excuse to miss it.
TEST(AnalyticsAggregate, JointEmoveRecoversTruthWhereTwoPillarIsBiased) {
  constexpr double kSt = 0.22;
  constexpr double kLt = 0.28;
  constexpr double kDecay = 1.5;
  constexpr double kTruthEmove = 0.0208; // the sweep's AAPL truth
  const auto sigma_c = [](double T) { return kLt + (kSt - kLt) * std::exp(-kDecay * T); };

  const std::vector<double> Ts = {0.02, 0.30, 0.55, 0.80, 1.05, 1.30};
  const std::vector<std::size_t> ns = {0, 1, 1, 1, 2, 2};
  std::vector<double> thetas;
  thetas.reserve(Ts.size());
  for (std::size_t i = 0; i < Ts.size(); ++i) {
    const double s = sigma_c(Ts[i]);
    thetas.push_back(static_cast<double>(ns[i]) * kTruthEmove * kTruthEmove + s * s * Ts[i]);
  }
  const PricedSurface ps = make_theta_surface(11, 100.0, 100.0, Ts, thetas);

  // Two events: one at 0.05y (between the 0.02 and 0.30 pillars — the wide
  // bracket) and one at 0.90y (between 0.80 and 1.05). Both sit well away from
  // any pillar so no year-fraction convention detail can flip a count.
  const EventSchedule sched(std::vector<std::int64_t>{
      testkit::kFixtureNow + static_cast<std::int64_t>(0.05 * testkit::kYearNs),
      testkit::kFixtureNow + static_cast<std::int64_t>(0.90 * testkit::kYearNs)});
  for (std::size_t i = 0; i < Ts.size(); ++i) {
    ASSERT_EQ(count_events_at(sched, testkit::kFixtureNow, Ts[i]), ns[i]) << "T=" << Ts[i];
  }

  EventContext ctx;
  ctx.schedule = &sched;

  const auto e = earnings_implied_move(ps, ctx);
  ASSERT_TRUE(e.has_value()) << e.error().to_string();
  EXPECT_NEAR(*e, kTruthEmove, 0.25 * kTruthEmove)
      << "eMove=" << *e << " truth=" << kTruthEmove
      << " err=" << 100.0 * (*e - kTruthEmove) / kTruthEmove << "%";

  // Non-vacuity: the accuracy above must come from the JOINT fit, not from a
  // lucky two-pillar fallback. The `_ex` overload reports which solve ran.
  const auto ex = earnings_implied_move_ex(ps, ctx);
  ASSERT_TRUE(ex.has_value()) << ex.error().to_string();
  EXPECT_EQ(ex->method, EmoveMethod::Joint);
  EXPECT_EQ(ex->expiry_count, Ts.size());
  EXPECT_DOUBLE_EQ(ex->emove, *e);

  // And the two-pillar solve this replaced really is the +173% answer, so the
  // test is measuring a fix and not an unrelated coincidence.
  const double w1 = ps.total_variance(ps.forward_at(Ts[0]), Ts[0]);
  const double w2 = ps.total_variance(ps.forward_at(Ts[1]), Ts[1]);
  const auto two_pillar = implied_emove(w1, Ts[0], ns[0], w2, Ts[1], ns[1]);
  ASSERT_TRUE(two_pillar.has_value()) << two_pillar.error().to_string();
  EXPECT_GT(*two_pillar, 2.5 * kTruthEmove)
      << "two-pillar=" << *two_pillar << " (err "
      << 100.0 * (*two_pillar - kTruthEmove) / kTruthEmove << "%)";
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

// ── Term-structure ex-earnings uses censored-SPACE interpolation (Seam S2) ────

// A tenor straddling one earnings date: the two bracket pillars carry n_lo = 0
// (T_lo = 0.05) and n_hi = 1 (T_hi = 0.10, one eMove = 0.06 lump embedded), and a
// mid tenor T_q = 0.08 (> the 0.075 event) has n_query = 1. The censored-space
// path (the new default) must censor the two pillars SEPARATELY and interpolate
// the CENSORED variance in T (recovering σ_C exactly), NOT censor a single plain
// cross-pillar interpolated variance once. The two disagree by exactly
// eMove²·[n_lo + α(n_hi − n_lo) − n_query] in total-variance space.
TEST(AnalyticsAggregate, AtmVolExEarnUsesCensoredSpaceInterpolation) {
  constexpr double kSigmaC = 0.20; // common censored (event-free) diffusive vol
  constexpr double kEmove = 0.06;  // per-event move embedded in the long slice
  constexpr double kTlo = 0.05;
  constexpr double kThi = 0.10;
  constexpr double kTq = 0.08; // mid tenor, strictly inside (T_lo, T_hi)

  const std::vector<double> Ts = {kTlo, kThi};
  const std::vector<double> thetas = {
      kSigmaC * kSigmaC * kTlo,                   // n_lo = 0: pure diffusion
      kSigmaC * kSigmaC * kThi + kEmove * kEmove, // n_hi = 1: + one eMove lump
  };
  const PricedSurface ps = make_theta_surface(5, 100.0, 100.0, Ts, thetas);
  const EventSchedule sched = testkit::make_event_schedule(0.075);

  EventContext ctx_cen; // censored-space (the new default)
  ctx_cen.schedule = &sched;
  ctx_cen.implied_emove = kEmove;
  ctx_cen.censor_space = true;

  EventContext ctx_plain = ctx_cen; // legacy plain-space (A/B flag off)
  ctx_plain.censor_space = false;

  const std::int64_t now_ns = ps.pricing().now_ts_ns;
  const std::size_t n_lo = count_events_at(sched, now_ns, kTlo);
  const std::size_t n_hi = count_events_at(sched, now_ns, kThi);
  const std::size_t n_q = count_events_at(sched, now_ns, kTq);
  ASSERT_EQ(n_lo, 0u);
  ASSERT_EQ(n_hi, 1u);
  ASSERT_EQ(n_q, 1u);

  // Reference: censor the two bracket pillars separately, interpolate the
  // censored variance in T, and — because this entry point returns the
  // EX-earnings (event-free) vol — do NOT re-add the query lump (n_query = 0).
  // Pillar variances are read from the SAME surface the implementation reads.
  const double w_lo = ps.total_variance(ps.forward_at(kTlo), kTlo);
  const double w_hi = ps.total_variance(ps.forward_at(kThi), kThi);
  const double w_cen_query =
      event_aware_w(w_lo, kTlo, n_lo, w_hi, kThi, n_hi, kTq, /*n_query=*/0, kEmove);
  const double ref_vol = std::sqrt(w_cen_query / kTq);

  const double vol_cen = atmf_vol_ex_earnings(ps, kTq, ctx_cen);
  const double vol_plain = atmf_vol_ex_earnings(ps, kTq, ctx_plain);

  // Both branches produce a finite vol.
  ASSERT_TRUE(std::isfinite(vol_cen));
  ASSERT_TRUE(std::isfinite(vol_plain));

  // (a) censored-space equals the event_aware_w reference and recovers σ_C.
  EXPECT_NEAR(vol_cen, ref_vol, 1e-9);
  EXPECT_NEAR(vol_cen, kSigmaC, 1e-9);

  // (b) it DIFFERS from the legacy plain-space (censor-the-interpolated-w) value.
  EXPECT_GT(std::fabs(vol_cen - vol_plain), 1e-3);

  // (c) the two total-variance FORMS differ by exactly
  //     eMove²·[n_lo + α(n_hi − n_lo) − n_query], with α the T-interpolation
  //     weight of T_q between the pillars. Plain form: w_surf(T_q) − n_q·eMove²
  //     (== vol_plain²·T_q); censored form: w_cen_query (== vol_cen²·T_q).
  const double alpha = (kTq - kTlo) / (kThi - kTlo);
  const double wc_plain = vol_plain * vol_plain * kTq;
  const double diff_term =
      kEmove * kEmove *
      (static_cast<double>(n_lo) +
       alpha * (static_cast<double>(n_hi) - static_cast<double>(n_lo)) -
       static_cast<double>(n_q));
  EXPECT_NEAR(wc_plain - w_cen_query, diff_term, 1e-12);
}

} // namespace
} // namespace atx::vol
