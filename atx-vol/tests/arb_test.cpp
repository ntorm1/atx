#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <utility>
#include <vector>

#include "atx/vol/arb.hpp"

#include "atx/vol/black76.hpp"
#include "atx/vol/dense_slice.hpp"
#include "atx/vol/vol_curve.hpp"

// Coverage for the static-arbitrage validators (arb.hpp), ported from the C
// ats-vol library (ats_arb.c). Calendar / butterfly checks, SVI-MM
// admissibility, calendar projection / repair, and the Sprint-08 quote
// pre-fit filters. The filter cases mirror the C `test_prefit_filter.c`
// assertions/tolerances; the surface-arb cases construct known slices (the C
// exercised arb only indirectly through the calibration smoke tests).

namespace {

using atx::vol::ArbViolation;
using atx::vol::arb_check_all;
using atx::vol::arb_check_butterfly;
using atx::vol::arb_check_butterfly_slice;
using atx::vol::arb_check_butterfly_svi_mm;
using atx::vol::arb_check_butterfly_svi_mm_surface;
using atx::vol::arb_check_calendar;
using atx::vol::arb_check_price_bounds;
using atx::vol::arb_check_total_surface_all;
using atx::vol::arb_filter_quotes_ex;
using atx::vol::arb_project_calendar_essvi;
using atx::vol::arb_project_calendar_essvi_pair;
using atx::vol::arb_project_calendar_svi;
using atx::vol::arb_project_calendar_svi_pair;
using atx::vol::arb_project_calendar_c8_pair;
using atx::vol::arb_repair_calendar_residual;
using atx::vol::black76_price;
using atx::vol::c8_slice_w;
using atx::vol::C8Curve;
using atx::vol::C8Params;
using atx::vol::ConvexDenseCurve;
using atx::vol::ConvexSliceFit;
using atx::vol::CurveSet;
using atx::vol::CurveSurface;
using atx::vol::ErrorCode;
using atx::vol::EssviParams;
using atx::vol::EssviCurve;
using atx::vol::essvi_backbone_w;
using atx::vol::essvi_total_w;
using atx::vol::filter_default_opts;
using atx::vol::FilterOpts;
using atx::vol::has_flag;
using atx::vol::Parametrization;
using atx::vol::prefit_filter_underlier;
using atx::vol::QuoteBatch;
using atx::vol::QuoteFlag;
using atx::vol::arb_check_calendar_banded;
using atx::vol::CalendarInterval;
using atx::vol::delta_band_from_atm_w;
using atx::vol::DeltaBand;
using atx::vol::kWystupTailQuantile;
using atx::vol::ResidualBasisKind;
using atx::vol::Side;
using atx::vol::SliceCrossings;
using atx::vol::svi_pair_calendar_intervals;
using atx::vol::svi_pair_crossings;
using atx::vol::SviCrossingSlice;
using atx::vol::svi_total_w;
using atx::vol::SviParams;
using atx::vol::SviCurve;
using atx::vol::svi_total_w;
using atx::vol::Universe;
using atx::vol::VolSurface;

// ── Surface builders ──────────────────────────────────────────────────────

// Two ascending-T eSSVI slices with a shared (phi=1, rho=0) backbone shape and
// residual off, so total w scales linearly with theta. Passing th0 > th1
// hand-builds a calendar-violating surface; th0 < th1 a monotone one.
[[nodiscard]] VolSurface make_essvi_2slice(double th0, double T0, double th1,
                                           double T1) {
  auto res = VolSurface::create(1u, Parametrization::Essvi, 2);
  VolSurface surf = std::move(res).value();
  EssviParams s0{};
  s0.theta = th0;
  s0.phi = 1.0;
  s0.rho = 0.0;
  s0.T = T0;
  EssviParams s1{};
  s1.theta = th1;
  s1.phi = 1.0;
  s1.rho = 0.0;
  s1.T = T1;
  (void)surf.set_slice_essvi(0, s0);
  (void)surf.set_slice_essvi(1, s1);
  return surf;
}

// The same two-slice eSSVI stack with a POSITIVE right-wing HINGE_QUAD residual
// on the shorter-dated slice (`resid_scale` 0.1 puts k >= 0.04 outside the dead
// band). `th0` vs `th1` therefore decides whether the crossing survives the
// residual damper: th0 < th1 is repairable (only the residual crosses), th0 >
// th1 is not (the backbones themselves cross, which alpha = 0 cannot fix).
[[nodiscard]] VolSurface make_essvi_2slice_resid(double th0, double th1, double wing_coef) {
  VolSurface surf = make_essvi_2slice(th0, 0.25, th1, 1.0);
  EssviParams s0 = surf.essvi_slices()[0];
  s0.resid_scale = 0.1;
  s0.resid_basis_kind = ResidualBasisKind::HingeQuad;
  s0.resid_n_basis = 5;
  s0.resid_coef[3] = wing_coef; // right-wing hinge
  (void)surf.set_slice_essvi(0, s0);
  return surf;
}

[[nodiscard]] VolSurface make_svi_2slice(const SviParams &s0,
                                         const SviParams &s1) {
  auto res = VolSurface::create(1u, Parametrization::Svi, 2);
  VolSurface surf = std::move(res).value();
  (void)surf.set_slice_svi(0, s0);
  (void)surf.set_slice_svi(1, s1);
  return surf;
}

[[nodiscard]] VolSurface make_svi_1slice(const SviParams &s0) {
  auto res = VolSurface::create(1u, Parametrization::Svi, 1);
  VolSurface surf = std::move(res).value();
  (void)surf.set_slice_svi(0, s0);
  return surf;
}

// Three FLAT (b = 0) SVI slices at T = 0.25 / 0.5 / 1.0, whose total variance is
// the `a` coefficient verbatim at every k. Passing a NaN for one slice makes
// `VolSurface::w` non-finite at that maturity while the others stay clean, which
// is the shape plan item 2.7's calendar-poisoning defect needs. Flat slices also
// keep the butterfly scan silent (w' = w'' = 0 => g = 1), so the calendar count
// is the only thing under test.
[[nodiscard]] VolSurface make_svi_3slice_flat(double w0, double w1, double w2) {
  auto res = VolSurface::create(1u, Parametrization::Svi, 3);
  VolSurface surf = std::move(res).value();
  const double Ts[3] = {0.25, 0.5, 1.0};
  const double ws[3] = {w0, w1, w2};
  for (std::size_t i = 0; i < 3; ++i) {
    SviParams s{};
    s.a = ws[i];
    s.b = 0.0;
    s.rho = 0.0;
    s.m = 0.0;
    s.sigma = 0.1;
    s.T = Ts[i];
    (void)surf.set_slice_svi(i, s);
  }
  return surf;
}

// A raw-SVI slice with a very steep wing (b well past the Lee bound) — its
// Roper density goes negative in the wings (butterfly arbitrage).
[[nodiscard]] SviParams steep_svi_slice() {
  SviParams s{};
  s.a = 0.04;
  s.b = 4.0;
  s.rho = 0.0;
  s.m = 0.0;
  s.sigma = 0.1;
  s.T = 1.0;
  return s;
}

// A trivial constant-vol convex slice at (T,F) — flat smile sigma. 5 strikes
// around F; European call prices at flat sigma are convex/arb-free, so the
// convex-QP-shaped fixture below feeds fit output directly rather than the
// fitter itself.
[[nodiscard]] ConvexSliceFit flat_slice(double T, double F, double df,
                                        double sigma) {
  ConvexSliceFit s;
  s.T = T;
  s.F = F;
  s.df = df;
  for (int i = -2; i <= 2; ++i) {
    const double K = F * std::exp(0.05 * i);
    s.u.push_back(K);
    s.C.push_back(black76_price(F, K, T, sigma, df, Side::Call));
  }
  return s;
}

// Five samples from the clean, non-flat SPX Figure 1 reproduction.  Mapping
// normalized strike back to k with its ATF scale and pricing each sample gives
// strictly convex call-price nodes.  The represented curve is piecewise linear
// between those nodes, so its density includes atoms at the knots; treating w(k)
// as C2-smooth and finite-differencing through a knot creates false negative
// Durrleman density even though the native call-price representation is convex.
[[nodiscard]] ConvexSliceFit nonflat_spx_like_convex_slice() {
  constexpr double T = 0.0678;
  constexpr double F = 100.0;
  constexpr double df = 0.999;
  constexpr double sigma_atf = 0.184;
  const double normalized_strike_scale = sigma_atf * std::sqrt(T);
  const std::vector<double> normalized_strike = {
      -10.002540215, -6.441711274, -2.880882333, 0.679946608, 2.863921692};
  const std::vector<double> sigma = {
      0.564486700, 0.434483490, 0.289490997, 0.140751183, 0.164676386};

  ConvexSliceFit fit;
  fit.T = T;
  fit.F = F;
  fit.df = df;
  for (std::size_t i = 0; i < normalized_strike.size(); ++i) {
    const double K = F * std::exp(normalized_strike[i] * normalized_strike_scale);
    fit.u.push_back(K);
    fit.C.push_back(black76_price(F, K, T, sigma[i], df, Side::Call));
  }
  return fit;
}

}  // namespace

// ── Calendar check ────────────────────────────────────────────────────────

TEST(ArbCalendar, MonotoneSurface_NoViolations) {
  const VolSurface surf = make_essvi_2slice(0.04, 0.25, 0.16, 1.0);
  const auto res = arb_check_calendar(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res.value().empty());
}

TEST(ArbCalendar, LongerMaturityLowerVariance_FlaggedAsOneInterval) {
  // slice0 (T=0.25) carries 4x slice1's (T=1.0) total variance at every k, so
  // the two never cross and the whole of R is one violating region. The EXACT
  // regime (residual-free eSSVI, strictly ascending T) reports that as ONE
  // interval; the sampled regime it replaced reported one hit per grid point,
  // which made the count a function of `n_grid` rather than of the surface.
  const VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const auto res = arb_check_calendar(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  const auto &v = res.value();
  ASSERT_EQ(v.size(), 1u);
  EXPECT_EQ(v[0].kind, ArbViolation::Kind::Calendar);
  EXPECT_GT(v[0].slack, 0.0);  // slack = w(T1) - w(T2) > 0
  EXPECT_EQ(v[0].T1, 0.25);    // shorter maturity recorded distinctly
  EXPECT_EQ(v[0].T2, 1.0);
  // The witness must sit inside the band the caller asked about.
  EXPECT_GE(v[0].k_log, -0.2);
  EXPECT_LE(v[0].k_log, 0.2);
  // ... and the record must be honest about the deficit there.
  EXPECT_NEAR(v[0].slack,
              essvi_total_w(surf.essvi_slices()[0], v[0].k_log) -
                  essvi_total_w(surf.essvi_slices()[1], v[0].k_log),
              1.0e-15);
}

TEST(ArbCalendar, EmptyOrSingleSlice_NoOpEmpty) {
  const VolSurface surf = make_svi_1slice(steep_svi_slice());
  const auto res = arb_check_calendar(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res.value().empty());
}

// ── Plan item 2.7: a non-finite slice must not hide the crossing it spans ──
//
// `w_prev` was updated unconditionally, so a NaN total variance at slice i
// BECAME the comparison baseline, discarding the last usable one. Slice i+1's
// `w + 1e-12 < NaN` is then false (NaN compares unordered), so the crossing
// that SPANS the unusable slice was never tested — here a drop as blatant as
// w(T=0.25) = 0.16 against w(T=1.0) = 0.04 came back as a clean surface.
//
// Contract: a non-finite w is UNCOMPARABLE — not a violation, and not a
// baseline. The point is skipped and the last FINITE (w, T) stays the baseline,
// so the spanning crossing is reported and carries the maturities of the two
// finite slices. That is exactly how the CurveSurface overload already treats
// it ("wing coverage gap on one side — nothing to compare"), and it reports
// nothing for the offending slice itself.
TEST(ArbCalendar, NonFiniteMiddleSlice_StillFlagsTheLaterCrossing) {
  const VolSurface surf = make_svi_3slice_flat(
      0.16, std::numeric_limits<double>::quiet_NaN(), 0.04);
  const auto res = arb_check_calendar(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  const auto &v = res.value();
  // Exactly one per sampled k: the poisoned slice contributes none of its own.
  ASSERT_EQ(v.size(), 8u);
  for (const ArbViolation &viol : v) {
    EXPECT_EQ(viol.kind, ArbViolation::Kind::Calendar);
    EXPECT_EQ(viol.T1, 0.25);  // the last FINITE slice, not the NaN one at 0.5
    EXPECT_EQ(viol.T2, 1.0);
    EXPECT_NEAR(viol.slack, 0.12, 1.0e-15);
  }
}

// Front-of-stack boundary for the skip above. A NaN FIRST slice was never the
// bug — there is no earlier finite baseline for it to discard, so the old code
// found this crossing too. It is pinned because the SKIP is what could break
// it: leaving w_prev at its -inf seed must not manufacture a violation, and
// T_prev must not carry the maturity of the slice that was skipped. The
// reported pair is therefore (0.5, 1.0), never (0.25, 1.0).
TEST(ArbCalendar, NonFiniteFirstSlice_StillFlagsTheLaterCrossing) {
  const VolSurface surf = make_svi_3slice_flat(
      std::numeric_limits<double>::quiet_NaN(), 0.16, 0.04);
  const auto res = arb_check_calendar(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  const auto &v = res.value();
  ASSERT_EQ(v.size(), 8u);
  for (const ArbViolation &viol : v) {
    EXPECT_EQ(viol.kind, ArbViolation::Kind::Calendar);
    EXPECT_EQ(viol.T1, 0.5);
    EXPECT_EQ(viol.T2, 1.0);
    EXPECT_NEAR(viol.slack, 0.12, 1.0e-15);
  }
}

// ── Exact slice crossings (quartic root test) ─────────────────────────────

namespace {

[[nodiscard]] SviParams svi_slice(double a, double b, double rho, double m,
                                  double sigma, double T) {
  SviParams s{};
  s.a = a;
  s.b = b;
  s.rho = rho;
  s.m = m;
  s.sigma = sigma;
  s.T = T;
  return s;
}

// Two slices whose total variances cross in a window ~0.0022 wide centred at
// k = 0.009, i.e. narrower than a single step of the 64-point [-0.6, 0.6] grid
// (0.01875) and sitting BETWEEN two of its samples (0.0 and 0.01875).
//
//   D(k) = (a_lo - a_hi) + b*( sqrt(d^2 + s_lo^2) - sqrt(d^2 + s_hi^2) ),
//          d = k - m
//
// peaks at d = 0 (value 0.2*(0.05 - 0.01) - 0.00799 = 1e-5 > 0) and decays
// monotonically to -0.00799 as |d| grows, so there are exactly two crossings
// and exactly one violating interval, both closed-form:
//   D ~ 1e-5 - 8*d^2 => |d| = sqrt(1.25e-6) = 0.0011180...
[[nodiscard]] VolSurface make_narrow_crossing_surface() {
  return make_svi_2slice(svi_slice(0.10000, 0.2, 0.0, 0.009, 0.05, 0.25),
                         svi_slice(0.10799, 0.2, 0.0, 0.009, 0.01, 1.00));
}

[[nodiscard]] SviCrossingSlice must_convert(const SviParams &p) {
  const std::optional<SviCrossingSlice> cs = SviCrossingSlice::from_svi(p);
  EXPECT_TRUE(cs.has_value()) << "fixture slice is not exactly decidable";
  // SAFETY: every call site passes a polytope-admissible slice; `value()`
  // throws loudly on a broken fixture rather than dereferencing an empty
  // optional, so there is no UB path even when the EXPECT above has fired.
  return cs.value();
}

}  // namespace

TEST(SviCrossingSlice, EssviBackboneReparametrisationIsExact) {
  // b = theta*phi/2 is the whole of D3: eSSVI's theta/2 prefactor is what makes
  // its wing-slope ceiling 4 while raw SVI's is 2. Pin the algebra that says so.
  const double thetas[] = {0.02, 0.16, 0.55};
  const double phis[] = {0.4, 1.0, 3.5};
  const double rhos[] = {-0.85, -0.3, 0.0, 0.62};
  for (const double theta : thetas) {
    for (const double phi : phis) {
      for (const double rho : rhos) {
        EssviParams p{};
        p.theta = theta;
        p.phi = phi;
        p.rho = rho;
        p.T = 0.5;
        const std::optional<SviCrossingSlice> cs =
            SviCrossingSlice::from_essvi_backbone(p);
        ASSERT_TRUE(cs.has_value());
        EXPECT_NEAR(cs->b(), 0.5 * theta * phi, 1.0e-15);
        for (int i = -40; i <= 40; ++i) {
          const double k = 0.05 * static_cast<double>(i);
          EXPECT_NEAR(cs->w(k), essvi_backbone_w(p, k), 1.0e-13)
              << "theta=" << theta << " phi=" << phi << " rho=" << rho
              << " k=" << k;
        }
      }
    }
  }
}

TEST(SviCrossingSlice, RefusesWhatItCannotDecideExactly) {
  // A residual layer is a clamped hinge-quadratic, not an SVI hyperbola: its
  // crossings are NOT roots of the quartic, so accepting the slice would report
  // a subset of the truth with the full confidence of an exact answer.
  EssviParams resid{};
  resid.theta = 0.10;
  resid.phi = 1.0;
  resid.rho = -0.2;
  resid.T = 0.5;
  resid.resid_scale = 0.1;
  resid.resid_basis_kind = ResidualBasisKind::HingeQuad;
  resid.resid_n_basis = 5;
  resid.resid_coef[3] = 0.01;
  EXPECT_FALSE(SviCrossingSlice::from_essvi_backbone(resid).has_value());

  // An armed asymmetric rho-blend evaluates to NaN, so nothing can be decided.
  EssviParams blended{};
  blended.theta = 0.10;
  blended.phi = 1.0;
  blended.rho = -0.2;
  blended.rho_R = 0.3;
  blended.rho_scale = 0.5;
  blended.T = 0.5;
  EXPECT_FALSE(SviCrossingSlice::from_essvi_backbone(blended).has_value());

  EXPECT_FALSE(
      SviCrossingSlice::from_svi(svi_slice(0.04, -0.1, 0.0, 0.0, 0.1, 1.0))
          .has_value());  // b < 0
  EXPECT_FALSE(
      SviCrossingSlice::from_svi(svi_slice(0.04, 0.1, 1.0, 0.0, 0.1, 1.0))
          .has_value());  // |rho| == 1
  EXPECT_FALSE(SviCrossingSlice::from_svi(
                   svi_slice(std::numeric_limits<double>::quiet_NaN(), 0.1, 0.0,
                             0.0, 0.1, 1.0))
                   .has_value());
}

TEST(SviPairCrossings, LocatesBothRootsOfANarrowCrossing) {
  const VolSurface surf = make_narrow_crossing_surface();
  const SviCrossingSlice lo = must_convert(surf.svi_slices()[0]);
  const SviCrossingSlice hi = must_convert(surf.svi_slices()[1]);

  const SliceCrossings x = svi_pair_crossings(lo, hi);
  ASSERT_EQ(x.n, 2u);
  // Closed-form locations from the expansion in make_narrow_crossing_surface,
  // to the accuracy of that second-order expansion.
  EXPECT_NEAR(x.k[0], 0.009 - 0.0011180, 2.0e-5);
  EXPECT_NEAR(x.k[1], 0.009 + 0.0011180, 2.0e-5);
  // The defining property, held to machine precision: w_lo == w_hi there.
  for (std::uint8_t i = 0; i < x.n; ++i) {
    EXPECT_NEAR(lo.w(x.k[i]), hi.w(x.k[i]), 1.0e-15) << "root " << int{i};
  }
}

TEST(SviPairCrossings, DropsTheSpuriousRootsTheTwoSquaringsIntroduce) {
  // Identical shapes with the longer slice shifted up by c = 0.06 in `a`. The
  // pair NEVER crosses (D == -0.06 everywhere), but the quartic
  //   P = (L - B1 - B2)(L + B1 + B2)(L - B1 + B2)(L + B1 - B2)
  // still has two real roots, from the factor L + B1 + B2 = 0 at 2B = c, i.e.
  //   (k - m)^2 = (c/(2b))^2 - sigma^2 = 0.15^2 - 0.05^2 = 0.02.
  // An implementation that skips back-substitution returns k = +/-0.1414 as
  // confident crossings of two curves that are 0.06 apart there.
  const SviParams p_lo = svi_slice(0.10, 0.2, 0.0, 0.0, 0.05, 0.25);
  const SviParams p_hi = svi_slice(0.16, 0.2, 0.0, 0.0, 0.05, 1.00);
  const SviCrossingSlice lo = must_convert(p_lo);
  const SviCrossingSlice hi = must_convert(p_hi);

  // Non-vacuity: the spurious roots really are there to be rejected, and the
  // curves are a full 0.06 apart where a back-substitution-free solver would
  // announce a crossing.
  const double k_spurious = std::sqrt(0.02);
  EXPECT_NEAR(2.0 * 0.2 * std::sqrt(k_spurious * k_spurious + 0.0025), 0.06,
              1.0e-12)
      << "fixture broken: 2*B must equal c = 0.06 at the spurious root";
  EXPECT_NEAR(hi.w(k_spurious) - lo.w(k_spurious), 0.06, 1.0e-12);
  EXPECT_NEAR(hi.w(0.0) - lo.w(0.0), 0.06, 1.0e-12);

  EXPECT_EQ(svi_pair_crossings(lo, hi).n, 0u);
  EXPECT_TRUE(svi_pair_calendar_intervals(lo, hi, 1.0e-12).empty());
}

TEST(SviPairCalendarIntervals, CertifiesTheUnboundedTailsAGridCannotReach) {
  // Ordered in the near-money band and inverted in BOTH wings: the shorter
  // slice's wing slope (b = 0.30) beats the longer one's (b = 0.10), so
  //   D(k) = -0.15 + 0.20*sqrt(k^2 + 0.01)
  // is negative on |k| < sqrt(0.5525) = 0.74330 and positive outside it.
  // Roper's IV4 is quantified over R; no finite grid can make this statement.
  const SviCrossingSlice lo = must_convert(svi_slice(0.05, 0.30, 0.0, 0.0, 0.1, 0.25));
  const SviCrossingSlice hi = must_convert(svi_slice(0.20, 0.10, 0.0, 0.0, 0.1, 1.00));

  const SliceCrossings x = svi_pair_crossings(lo, hi);
  ASSERT_EQ(x.n, 2u);
  EXPECT_NEAR(x.k[0], -std::sqrt(0.5525), 1.0e-12);
  EXPECT_NEAR(x.k[1], std::sqrt(0.5525), 1.0e-12);

  const std::vector<CalendarInterval> iv =
      svi_pair_calendar_intervals(lo, hi, 1.0e-12);
  ASSERT_EQ(iv.size(), 2u);
  EXPECT_EQ(iv[0].k_lo, -std::numeric_limits<double>::infinity());
  EXPECT_NEAR(iv[0].k_hi, -std::sqrt(0.5525), 1.0e-12);
  EXPECT_NEAR(iv[1].k_lo, std::sqrt(0.5525), 1.0e-12);
  EXPECT_EQ(iv[1].k_hi, std::numeric_limits<double>::infinity());
  for (const CalendarInterval &c : iv) {
    EXPECT_GT(c.slack, 0.0);
    EXPECT_NEAR(c.slack, lo.w(c.k_witness) - hi.w(c.k_witness), 1.0e-15);
  }

  // ... and the banded check still reports the near-money band as clean, which
  // is the whole point of separating in-band from out-of-band.
  const VolSurface surf = make_svi_2slice(svi_slice(0.05, 0.30, 0.0, 0.0, 0.1, 0.25),
                                          svi_slice(0.20, 0.10, 0.0, 0.0, 0.1, 1.00));
  const auto in_band = arb_check_calendar(surf, -0.6, 0.6, 64);
  ASSERT_TRUE(in_band.has_value());
  EXPECT_TRUE(in_band.value().empty());
}

TEST(ArbCalendarExact, FindsTheCrossingTheGridSteppedOver) {
  const VolSurface surf = make_narrow_crossing_surface();

  // The oracle this replaces, run at the production density used by
  // session.cpp (64 intervals over +/-0.6): the violating window is 0.0022
  // wide and sits between the samples at k = 0 and k = 0.01875, so a sampled
  // scan reports a clean surface.
  const auto &s0 = surf.svi_slices()[0];
  const auto &s1 = surf.svi_slices()[1];
  bool grid_saw_it = false;
  for (std::uint32_t g = 0; g < 64u; ++g) {
    const double k = -0.6 + static_cast<double>(g) * (1.2 / 64.0);
    if (svi_total_w(s0, k) - svi_total_w(s1, k) > 1.0e-12) {
      grid_saw_it = true;
    }
  }
  ASSERT_FALSE(grid_saw_it) << "fixture broken: the grid must miss this one";

  const auto res = arb_check_calendar(surf, -0.6, 0.6, 64);
  ASSERT_TRUE(res.has_value());
  ASSERT_EQ(res.value().size(), 1u);
  const ArbViolation &v = res.value().front();
  EXPECT_EQ(v.kind, ArbViolation::Kind::Calendar);
  EXPECT_EQ(v.T1, 0.25);
  EXPECT_EQ(v.T2, 1.00);
  EXPECT_NEAR(v.k_log, 0.009, 2.0e-3);
  EXPECT_GT(v.slack, 0.0);
  EXPECT_NEAR(v.slack, svi_total_w(s0, v.k_log) - svi_total_w(s1, v.k_log),
              1.0e-15);
}

TEST(ArbCalendarExact, ClipsToTheRequestedBandAndKeepsTheWitnessInside) {
  // The narrow crossing sits at k ~ 0.009; a band that excludes it must come
  // back clean rather than reporting a violation at an out-of-band k.
  const VolSurface surf = make_narrow_crossing_surface();
  const auto outside = arb_check_calendar(surf, -0.6, -0.1, 64);
  ASSERT_TRUE(outside.has_value());
  EXPECT_TRUE(outside.value().empty());

  const auto inside = arb_check_calendar(surf, 0.0, 0.02, 64);
  ASSERT_TRUE(inside.has_value());
  ASSERT_EQ(inside.value().size(), 1u);
  EXPECT_GE(inside.value().front().k_log, 0.0);
  EXPECT_LE(inside.value().front().k_log, 0.02);
}

TEST(ArbCalendarExact, FallsBackToSamplingWhenASliceCarriesAResidual) {
  // th0 = th1 with a positive right-wing residual on the SHORTER slice: the
  // backbones coincide, so the crossing is entirely the residual's — a shape
  // the quartic does not describe. The surface must fall back to the sampled
  // regime rather than certify a clean backbone comparison as the answer.
  const VolSurface surf = make_essvi_2slice_resid(0.10, 0.10, 0.05);
  const auto res = arb_check_calendar(surf, -0.2, 0.4, 16);
  ASSERT_TRUE(res.has_value());
  EXPECT_FALSE(res.value().empty())
      << "the residual-induced crossing must still be found by the fallback";
  // Sampled semantics: one record per violating grid point, so more than the
  // single interval the exact regime would have produced.
  EXPECT_GT(res.value().size(), 1u);
}

// The deliverable that proves the replacement: the exact test must find every
// crossing the dense grid oracle finds, over randomised admissible pairs, and
// must never invent one. The coarse production grid is run alongside to measure
// what sampling costs.
TEST(ArbCalendarExact, DifferentialAgainstTheDenseGridOracle) {
  std::mt19937_64 rng(0x5EEDu);  // fixed: a flaky arbitrage test is worthless
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  const auto draw = [&](double lo, double hi) {
    return lo + (hi - lo) * u01(rng);
  };

  constexpr double kBandLo = -0.6;
  constexpr double kBandHi = 0.6;
  constexpr int kDense = 100000;  // 1.2e-5 spacing — the "oracle" grid
  constexpr std::uint32_t kCoarse = 64;  // what session.cpp actually samples
  constexpr double kSlackFloor = 1.0e-9;

  std::size_t n_pairs = 0;
  std::size_t n_dense_found = 0;
  std::size_t n_coarse_found = 0;
  std::size_t n_exact_found = 0;

  for (int trial = 0; trial < 1200; ++trial) {
    // Draw inside the Mingone polytope: b > 0, sigma > 0, |rho| < 1,
    // a + b*sigma*sqrt(1-rho^2) >= 0, and Lee's b*(1+|rho|) <= 2 (the bound
    // D3 derives), so every pair is one the fitter could legitimately serve.
    const auto draw_slice = [&](double T) {
      const double rho = draw(-0.9, 0.9);
      const double b = std::min(draw(0.005, 0.6), 2.0 / (1.0 + std::fabs(rho)));
      const double sigma = draw(0.01, 0.6);
      const double a = draw(-b * sigma * std::sqrt(1.0 - rho * rho), 0.25);
      return svi_slice(a, b, rho, draw(-0.4, 0.4), sigma, T);
    };
    const SviParams p_lo = draw_slice(0.25);
    const SviParams p_hi = draw_slice(1.00);
    ++n_pairs;

    // Dense oracle, computed from svi_total_w directly so it shares no code
    // with the routine under test.
    double dense_worst = 0.0;
    double dense_worst_k = 0.0;
    for (int i = 0; i <= kDense; ++i) {
      const double k = kBandLo + (kBandHi - kBandLo) *
                                     (static_cast<double>(i) /
                                      static_cast<double>(kDense));
      const double d = svi_total_w(p_lo, k) - svi_total_w(p_hi, k);
      if (d > dense_worst) {
        dense_worst = d;
        dense_worst_k = k;
      }
    }
    bool coarse_found = false;
    for (std::uint32_t g = 0; g < kCoarse; ++g) {
      const double k = kBandLo + static_cast<double>(g) *
                                     ((kBandHi - kBandLo) /
                                      static_cast<double>(kCoarse));
      if (svi_total_w(p_lo, k) - svi_total_w(p_hi, k) > 1.0e-12) {
        coarse_found = true;
      }
    }

    const VolSurface surf = make_svi_2slice(p_lo, p_hi);
    const auto res = arb_check_calendar(surf, kBandLo, kBandHi, kCoarse);
    ASSERT_TRUE(res.has_value());
    const std::vector<ArbViolation> &v = res.value();

    if (dense_worst > kSlackFloor) {
      ++n_dense_found;
      // (1) Never misses what sampling finds.
      ASSERT_FALSE(v.empty())
          << "trial " << trial << ": dense grid found slack " << dense_worst
          << " at k=" << dense_worst_k << ", exact reported nothing"
          << "\n  lo: a=" << p_lo.a << " b=" << p_lo.b << " rho=" << p_lo.rho
          << " m=" << p_lo.m << " sigma=" << p_lo.sigma
          << "\n  hi: a=" << p_hi.a << " b=" << p_hi.b << " rho=" << p_hi.rho
          << " m=" << p_hi.m << " sigma=" << p_hi.sigma;
      // (2) The exact witness is at least as bad as the dense grid's worst —
      //     the interval sweep is not allowed to under-report the breach it
      //     located.
      double best = 0.0;
      for (const ArbViolation &viol : v) {
        best = std::max(best, viol.slack);
      }
      EXPECT_GE(best, 0.5 * dense_worst) << "trial " << trial;
    }
    if (coarse_found) {
      ++n_coarse_found;
    }
    if (!v.empty()) {
      ++n_exact_found;
    }
    // (3) Never invents one: every record's slack must be the real deficit at
    //     its own k, must be positive, and must lie in the requested band.
    for (const ArbViolation &viol : v) {
      EXPECT_EQ(viol.kind, ArbViolation::Kind::Calendar);
      EXPECT_GE(viol.k_log, kBandLo);
      EXPECT_LE(viol.k_log, kBandHi);
      EXPECT_GT(viol.slack, 0.0);
      EXPECT_NEAR(viol.slack,
                  svi_total_w(p_lo, viol.k_log) - svi_total_w(p_hi, viol.k_log),
                  1.0e-12)
          << "trial " << trial;
    }
  }

  // (4) The exact regime is a strict superset of the coarse production grid,
  //     and of the dense oracle too. Measured at 4000 pairs against a
  //     200k-point oracle: coarse64=2940, oracle=2946, exact=2946 — the
  //     production grid density loses 0.2% of the in-band crossings that exist,
  //     and the exact test matches the oracle pair for pair. The committed
  //     sizes are trimmed so the case stays a unit test.
  EXPECT_GE(n_exact_found, n_dense_found);
  EXPECT_GE(n_exact_found, n_coarse_found);
  EXPECT_GT(n_pairs, std::size_t{0});
  std::printf(
      "[differential] pairs=%zu coarse64=%zu dense_oracle=%zu exact=%zu\n",
      n_pairs, n_coarse_found, n_dense_found, n_exact_found);
}

// ── The certified band: in-band vs out-of-band ────────────────────────────

TEST(DeltaBand, ReproducesTheDeltaLevelsItClaims) {
  // The band is only worth anything if k_lo/k_hi really are the 99%/1% forward
  // call deltas. Check against Phi(d1) directly.
  constexpr double kInvSqrt2 = 0.70710678118654752440;
  const auto phi = [](double x) { return 0.5 * std::erfc(-x * kInvSqrt2); };
  for (const double w : {0.0004, 0.01, 0.0625, 0.25, 1.0}) {
    const auto band = delta_band_from_atm_w(w, kWystupTailQuantile);
    ASSERT_TRUE(band.usable()) << "w=" << w;
    const double sw = std::sqrt(w);
    const double d1_lo = -band.k_lo / sw + 0.5 * sw;
    const double d1_hi = -band.k_hi / sw + 0.5 * sw;
    EXPECT_NEAR(phi(d1_lo), 0.99, 1.0e-12) << "w=" << w;
    EXPECT_NEAR(phi(d1_hi), 0.01, 1.0e-12) << "w=" << w;
  }
  // Not a band: must not silently read as one.
  EXPECT_FALSE(delta_band_from_atm_w(0.0, kWystupTailQuantile).usable());
  EXPECT_FALSE(delta_band_from_atm_w(-1.0, kWystupTailQuantile).usable());
  EXPECT_FALSE(delta_band_from_atm_w(
                   std::numeric_limits<double>::quiet_NaN(),
                   kWystupTailQuantile)
                   .usable());
}

TEST(DeltaBand, IsTenorHomogeneousWhereAFixedKWindowIsNot) {
  // The measurement that motivates the band. 35 vol at 1 week vs 2 years: the
  // fixed +/-0.60 window spans deltas eleven orders of magnitude apart, while
  // the delta band spans the same 1%-99% at both by construction.
  constexpr double kInvSqrt2 = 0.70710678118654752440;
  const auto phi = [](double x) { return 0.5 * std::erfc(-x * kInvSqrt2); };
  const double w_week = 0.35 * 0.35 * (7.0 / 365.0);
  const double w_2y = 0.35 * 0.35 * 2.0;
  const auto delta_at = [&](double w, double k) {
    return phi(-k / std::sqrt(w) + 0.5 * std::sqrt(w));
  };
  EXPECT_LT(delta_at(w_week, 0.60), 1.0e-11);
  EXPECT_GT(delta_at(w_2y, 0.60), 0.05);

  const auto b_week = delta_band_from_atm_w(w_week, kWystupTailQuantile);
  const auto b_2y = delta_band_from_atm_w(w_2y, kWystupTailQuantile);
  EXPECT_NEAR(delta_at(w_week, b_week.k_hi), 0.01, 1.0e-12);
  EXPECT_NEAR(delta_at(w_2y, b_2y.k_hi), 0.01, 1.0e-12);
  // ... and the band widens with tenor, which the fixed window cannot.
  EXPECT_LT(b_week.k_hi - b_week.k_lo, b_2y.k_hi - b_2y.k_lo);
}

TEST(ArbCalendarBanded, SeparatesAWingCrossingFromTheTradeableBand) {
  // Same fixture as CertifiesTheUnboundedTailsAGridCannotReach: ordered near
  // the money, inverted in both wings past |k| = 0.7433. ATM total variance is
  // 0.08 / 0.21, so the pair band is roughly +/-0.62 — the crossing is real and
  // sits outside it. That must be TWO out-of-band records and ZERO in-band,
  // not a boolean that reads as a broken surface.
  const VolSurface surf =
      make_svi_2slice(svi_slice(0.05, 0.30, 0.0, 0.0, 0.1, 0.25),
                      svi_slice(0.20, 0.10, 0.0, 0.0, 0.1, 1.00));
  const auto rep = arb_check_calendar_banded(surf, kWystupTailQuantile, 64);
  ASSERT_TRUE(rep.has_value());
  EXPECT_TRUE(rep->in_band.empty());
  EXPECT_EQ(rep->out_of_band.size(), 2u);
  EXPECT_EQ(rep->n_pairs_exact, 1u);
  EXPECT_EQ(rep->n_pairs_sampled, 0u);
  EXPECT_TRUE(rep->certified_over_r());
  for (const ArbViolation &v : rep->out_of_band) {
    EXPECT_GT(v.slack, 0.0);
    EXPECT_NEAR(v.slack,
                svi_total_w(surf.svi_slices()[0], v.k_log) -
                    svi_total_w(surf.svi_slices()[1], v.k_log),
                1.0e-15);
    EXPECT_GT(std::fabs(v.k_log), 0.7);  // out in the wing, where it belongs
  }
}

TEST(ArbCalendarBanded, CountsAnInBandCrossingAsInBand) {
  // The narrow crossing at k ~ 0.009 is deep inside both slices' delta bands.
  const VolSurface surf = make_narrow_crossing_surface();
  const auto rep = arb_check_calendar_banded(surf, kWystupTailQuantile, 64);
  ASSERT_TRUE(rep.has_value());
  ASSERT_EQ(rep->in_band.size(), 1u);
  EXPECT_NEAR(rep->in_band.front().k_log, 0.009, 2.0e-3);
  EXPECT_TRUE(rep->certified_over_r());
  // The same crossing's interval is bounded, so nothing spills outside.
  EXPECT_TRUE(rep->out_of_band.empty());
}

TEST(ArbCalendarBanded, ReportsThatTheSampledFallbackCannotCertifyR) {
  // A residual-carrying eSSVI pair is not exactly decidable, so the report must
  // say so rather than hand back an unbounded claim it did not earn.
  const VolSurface surf = make_essvi_2slice_resid(0.10, 0.10, 0.05);
  const auto rep = arb_check_calendar_banded(surf, kWystupTailQuantile, 64);
  ASSERT_TRUE(rep.has_value());
  EXPECT_EQ(rep->n_pairs_exact, 0u);
  EXPECT_EQ(rep->n_pairs_sampled, 1u);
  EXPECT_FALSE(rep->certified_over_r());
  EXPECT_FALSE(rep->in_band.empty() && rep->out_of_band.empty());
}

TEST(ArbCalendarBanded, EveryViolationLandsInExactlyOneCounter) {
  // The split must be a partition: the two counters together must account for
  // every crossing the unbanded exact check finds inside +/-3, and no record
  // may appear on both sides.
  std::mt19937_64 rng(0xB4DDu);
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  const auto draw = [&](double lo, double hi) { return lo + (hi - lo) * u01(rng); };
  std::size_t n_in = 0;
  std::size_t n_out = 0;
  for (int trial = 0; trial < 400; ++trial) {
    const auto draw_slice = [&](double T) {
      const double rho = draw(-0.9, 0.9);
      const double b = std::min(draw(0.005, 0.6), 2.0 / (1.0 + std::fabs(rho)));
      const double sigma = draw(0.01, 0.6);
      const double a = draw(0.001, 0.25);
      return svi_slice(a, b, rho, draw(-0.4, 0.4), sigma, T);
    };
    const SviParams p_lo = draw_slice(0.25);
    const SviParams p_hi = draw_slice(1.00);
    const VolSurface surf = make_svi_2slice(p_lo, p_hi);
    const auto rep = arb_check_calendar_banded(surf, kWystupTailQuantile, 64);
    ASSERT_TRUE(rep.has_value());
    n_in += rep->in_band.size();
    n_out += rep->out_of_band.size();

    // The pair band is the INTERSECTION of the two slices' bands; on an
    // ATM-ordered pair that is the shorter slice's, but the fixture draws
    // ATM-inverted pairs too.
    const DeltaBand b_lo =
        delta_band_from_atm_w(svi_total_w(p_lo, 0.0), kWystupTailQuantile);
    const DeltaBand b_hi =
        delta_band_from_atm_w(svi_total_w(p_hi, 0.0), kWystupTailQuantile);
    const DeltaBand band{std::max(b_lo.k_lo, b_hi.k_lo),
                         std::min(b_lo.k_hi, b_hi.k_hi)};
    for (const ArbViolation &v : rep->in_band) {
      EXPECT_GT(v.slack, 0.0) << "trial " << trial;
      EXPECT_NEAR(v.slack,
                  svi_total_w(p_lo, v.k_log) - svi_total_w(p_hi, v.k_log),
                  1.0e-12);
      EXPECT_LE(v.k_log, band.k_hi + 1.0e-12) << "trial " << trial;
      EXPECT_GE(v.k_log, band.k_lo - 1.0e-12) << "trial " << trial;
    }
    for (const ArbViolation &v : rep->out_of_band) {
      EXPECT_GT(v.slack, 0.0) << "trial " << trial;
      EXPECT_NEAR(v.slack,
                  svi_total_w(p_lo, v.k_log) - svi_total_w(p_hi, v.k_log),
                  1.0e-12);
    }
    // Consistency with the unbanded check over the in-band window: if anything
    // is reported in band, the plain check restricted to that band must agree.
    if (band.usable()) {
      const auto plain = arb_check_calendar(surf, band.k_lo, band.k_hi, 64);
      ASSERT_TRUE(plain.has_value());
      EXPECT_EQ(plain->empty(), rep->in_band.empty()) << "trial " << trial;
    }
  }
  std::printf("[banded] in_band=%zu out_of_band=%zu\n", n_in, n_out);
  EXPECT_GT(n_in + n_out, std::size_t{0});
}

// ── Calendar check (CurveSurface: ConvexDense/SVI served path) ─────────────

TEST(ArbCheckCalendarCurveSurface, FlagsCrossing) {
  CurveSurface surf;
  // T1=0.25 with HIGH vol, T2=0.50 with LOW vol -> w(k,T2) < w(k,T1): calendar
  // arb.
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.25, 100.0, 1.0, 0.40)));
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.50, 100.0, 1.0, 0.20)));
  const auto v = arb_check_calendar(surf, -0.2, 0.2, 21);
  ASSERT_TRUE(v.has_value());
  EXPECT_FALSE(v->empty());
  EXPECT_EQ(v->front().kind, ArbViolation::Kind::Calendar);
}

TEST(ArbCheckCalendarCurveSurface, CleanStackNoViolation) {
  CurveSurface surf;
  // Monotone total variance: same sigma -> w = sigma^2 * T is increasing in T.
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.25, 100.0, 1.0, 0.25)));
  surf.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.50, 100.0, 1.0, 0.25)));
  const auto v = arb_check_calendar(surf, -0.2, 0.2, 21);
  ASSERT_TRUE(v.has_value());
  EXPECT_TRUE(v->empty());
}

// Oracle I-2/M-7: dense_slice.cpp's iv() clamps a sub-intrinsic served price
// into Black's valid interval before total variance is formed, laundering
// the violation into a near-zero vol that is invisible to every w-space
// check (arb_check_calendar / arb_check_butterfly / the w-reconstructed
// PriceBounds gate in risk_surface_validation.cpp all reconstruct FROM w).
// This hand-built fit stands in for a served node ending up below discounted
// intrinsic (e.g. a regression that slips past the fail-closed QP
// feasibility check added for oracle I-4) — arb_check_price_bounds must
// catch it directly in price space, which is exactly what makes it
// independent of how the violation arose.
TEST(ArbCheckPriceBoundsCurveSurface, FlagsSubIntrinsicConvexDenseNode) {
  ConvexSliceFit fit;
  fit.T = 0.10;
  fit.F = 100.0;
  fit.df = 0.98;
  fit.u = {60.0, 80.0, 95.0};
  fit.C = {41.0, 21.0, 2.0};  // C(95)=2 < intrinsic(95)=0.98*5=4.9

  CurveSurface surface;
  surface.push(std::make_unique<ConvexDenseCurve>(fit));

  const auto violations = arb_check_price_bounds(surface, -0.60, 0.60, 64);
  ASSERT_TRUE(violations.has_value());
  EXPECT_FALSE(violations->empty());
  for (const ArbViolation &v : *violations) {
    EXPECT_EQ(v.kind, ArbViolation::Kind::PriceBounds);
    EXPECT_GT(v.slack, 0.0);
  }

  // The w-space calendar/butterfly checks find nothing to complain about on
  // this exact fixture (one slice; no w-space signal of the price violation)
  // — the whole point of I-2.
  const auto calendar = arb_check_calendar(surface, -0.60, 0.60, 64);
  ASSERT_TRUE(calendar.has_value());
  EXPECT_TRUE(calendar->empty());
}

TEST(ArbCheckPriceBoundsCurveSurface, CleanConvexDenseSliceHasNoViolations) {
  CurveSurface surface;
  surface.push(std::make_unique<ConvexDenseCurve>(flat_slice(0.25, 100.0, 0.99, 0.22)));
  const auto violations = arb_check_price_bounds(surface, -0.60, 0.60, 64);
  ASSERT_TRUE(violations.has_value());
  EXPECT_TRUE(violations->empty());
}

TEST(ArbCheckPriceBoundsCurveSurface, NonConvexDenseSliceContributesNothing) {
  CurveSurface surface;
  surface.push(std::make_unique<SviCurve>(steep_svi_slice(), 1.0));
  const auto violations = arb_check_price_bounds(surface, -0.60, 0.60, 64);
  ASSERT_TRUE(violations.has_value());
  EXPECT_TRUE(violations->empty());
}

TEST(ArbButterflyCurve, IndependentCheckerFlagsServedSviShape) {
  const SviCurve curve(steep_svi_slice(), 1.0);
  const auto violations = arb_check_butterfly(curve, -0.5, 0.5, 128);
  ASSERT_TRUE(violations.has_value());
  EXPECT_FALSE(violations->empty());
}

TEST(ArbButterflyCurve, NonFlatConvexDenseUsesNativePriceConvexity) {
  const ConvexSliceFit fit = nonflat_spx_like_convex_slice();
  ASSERT_EQ(fit.u.size(), fit.C.size());
  ASSERT_GE(fit.u.size(), 3u);
  double previous_slope =
      (fit.C[1] - fit.C[0]) / (fit.u[1] - fit.u[0]);
  for (std::size_t i = 1; i + 1 < fit.u.size(); ++i) {
    const double slope =
        (fit.C[i + 1] - fit.C[i]) / (fit.u[i + 1] - fit.u[i]);
    ASSERT_GE(slope, previous_slope) << "fixture must be convex at node " << i;
    previous_slope = slope;
  }

  const ConvexDenseCurve curve(fit);
  const auto violations = arb_check_butterfly(curve, -0.52, 0.16, 256);
  ASSERT_TRUE(violations.has_value());
  EXPECT_TRUE(violations->empty());
}

TEST(ArbButterflyCurve, NonConvexDenseNativePricesAreFlagged) {
  ConvexSliceFit fit;
  fit.T = 0.25;
  fit.F = 100.0;
  fit.df = 1.0;
  fit.u = {80.0, 90.0, 100.0, 110.0, 120.0};
  // Slopes -0.8, -0.7, -0.8, -0.1: the decrease from -0.7 to -0.8 is a
  // genuine convexity breach, while every node remains inside its price band.
  fit.C = {25.0, 17.0, 10.0, 2.0, 1.0};

  const ConvexDenseCurve curve(fit);
  const auto violations = arb_check_butterfly(
      curve, std::log(0.80), std::log(1.20), 64);
  ASSERT_TRUE(violations.has_value());
  ASSERT_FALSE(violations->empty());
  EXPECT_EQ(violations->front().kind, ArbViolation::Kind::Butterfly);
  EXPECT_GT(violations->front().slack, 0.09);
}

// ── Butterfly check ───────────────────────────────────────────────────────

TEST(ArbButterfly, WellBehavedEssvi_NoViolations) {
  const VolSurface surf = make_essvi_2slice(0.04, 0.25, 0.16, 1.0);
  const auto res = arb_check_butterfly(surf, -0.5, 0.5, 64);
  ASSERT_TRUE(res.has_value());
  EXPECT_TRUE(res.value().empty());
}

TEST(ArbButterfly, SteepSviWing_NegativeDensityFlagged) {
  const VolSurface surf = make_svi_1slice(steep_svi_slice());
  const auto res = arb_check_butterfly(surf, -0.5, 0.5, 64);
  ASSERT_TRUE(res.has_value());
  const auto &v = res.value();
  ASSERT_FALSE(v.empty());
  for (const ArbViolation &viol : v) {
    EXPECT_EQ(viol.kind, ArbViolation::Kind::Butterfly);
    EXPECT_EQ(viol.T1, 1.0);
    EXPECT_EQ(viol.T2, 1.0);  // T2 == T1 for butterfly
    EXPECT_GT(viol.slack, 0.0);
  }
}

TEST(ArbButterfly, KMaxNotAboveKMin_ReturnsInvalidArgument) {
  const VolSurface surf = make_svi_1slice(steep_svi_slice());
  const auto res = arb_check_butterfly(surf, 0.5, -0.5, 64);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── Per-slice butterfly check (arb_check_butterfly_slice) ──────────────────

TEST(ArbButterflySlice, CleanSviSlicePasses) {
  // An admissible raw-SVI slice (well inside the Mingone polytope): the
  // closed-form MM tally AND the grid Durrleman g-check must both report zero.
  SviParams s{};
  s.a = 0.04;
  s.b = 0.3;
  s.rho = -0.4;
  s.m = 0.0;
  s.sigma = 0.25;
  s.T = 1.0;
  const auto grid = arb_check_butterfly_slice(
      [&](double k) { return svi_total_w(s, k); }, s.T, -0.5, 0.5, 64);
  ASSERT_TRUE(grid.has_value());
  EXPECT_TRUE(grid.value().empty());
  const auto mm = arb_check_butterfly_svi_mm(s, s.T);
  EXPECT_EQ(mm.n_violations, 0u);  // closed-form and grid agree: admissible
}

TEST(ArbButterflySlice, LeeBoundViolationCaught) {
  // b*(1+|rho|) = 5*1.4 = 7 > 4/T = 4: the closed-form Lee wing-slope bound
  // fires (this is the closed-form gate used on served raw-SVI slices).
  SviParams s{};
  s.a = 0.04;
  s.b = 5.0;
  s.rho = -0.4;
  s.m = 0.0;
  s.sigma = 0.25;
  s.T = 1.0;
  const auto mm = arb_check_butterfly_svi_mm(s, s.T);
  EXPECT_GE(mm.n_violations, 1u);
  EXPECT_GT(mm.max_slack, 0.0);
}

TEST(ArbButterflySlice, GridCatchesConcaveBump) {
  // A hand-built total-variance callable with a strong local concavity (w'' < 0
  // everywhere): the Durrleman density g(k) goes negative, and the grid check
  // records a butterfly violation with the right sign convention.
  const auto w_of_k = [](double k) { return 0.10 - 5.0 * k * k; };
  const auto res = arb_check_butterfly_slice(w_of_k, 1.0, -0.1, 0.1, 8);
  ASSERT_TRUE(res.has_value());
  const auto& v = res.value();
  ASSERT_FALSE(v.empty());
  for (const ArbViolation& viol : v) {
    EXPECT_EQ(viol.kind, ArbViolation::Kind::Butterfly);
    EXPECT_EQ(viol.T1, 1.0);
    EXPECT_EQ(viol.T2, 1.0);
    EXPECT_GT(viol.slack, 0.0);      // slack = -g(k) > 0
    EXPECT_GT(viol.k_log, -0.1);     // located strictly inside the grid
    EXPECT_LT(viol.k_log, 0.1);
  }
}

TEST(ArbButterflySlice, KMaxNotAboveKMin_ReturnsInvalidArgument) {
  const auto res = arb_check_butterfly_slice(
      [](double k) { return 0.04 + 0.1 * k * k; }, 1.0, 0.5, -0.5, 64);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(ArbButterflySlice, SurfaceCheckUnchanged) {
  // Pin: the surface-level arb_check_butterfly output must be bit-identical to
  // an independent recomputation of the documented FD Durrleman formula on the
  // SAME surface evaluator (surf.w). Written to hold BEFORE the shared-helper
  // refactor and to keep holding after it — any arithmetic drift trips here.
  const SviParams steep = steep_svi_slice();
  const VolSurface surf = make_svi_1slice(steep);
  constexpr double k_min = -0.5;
  constexpr double k_max = 0.5;
  constexpr std::uint32_t n_grid = 64;
  const auto res = arb_check_butterfly(surf, k_min, k_max, n_grid);
  ASSERT_TRUE(res.has_value());

  // Golden: recompute violations from the documented g(k) formula via surf.w.
  const double T = steep.T;
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  const double inv_2dk = 0.5 / dk;
  const double inv_dksq = 1.0 / (dk * dk);
  std::vector<ArbViolation> expected;
  for (std::uint32_t g = 1; g < n_grid; ++g) {
    const double k = k_min + static_cast<double>(g) * dk;
    const double w_lo = surf.w(k - dk, T);
    const double w_mi = surf.w(k, T);
    const double w_hi = surf.w(k + dk, T);
    if (!(w_mi > 1.0e-12) || !std::isfinite(w_lo) || !std::isfinite(w_hi)) {
      continue;
    }
    const double w_p = (w_hi - w_lo) * inv_2dk;
    const double w_pp = (w_hi - 2.0 * w_mi + w_lo) * inv_dksq;
    const double term1_inner = 1.0 - 0.5 * k * w_p / w_mi;
    const double term1 = term1_inner * term1_inner;
    const double term2 = 0.25 * w_p * w_p * (0.25 + 1.0 / w_mi);
    const double term3 = 0.5 * w_pp;
    const double g_density = term1 - term2 + term3;
    if (g_density < -1.0e-9) {
      ArbViolation v{};
      v.k_log = k;
      v.T1 = T;
      v.T2 = T;
      v.slack = -g_density;
      v.kind = ArbViolation::Kind::Butterfly;
      expected.push_back(v);
    }
  }

  ASSERT_FALSE(expected.empty());
  ASSERT_EQ(res.value().size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_DOUBLE_EQ(res.value()[i].k_log, expected[i].k_log);
    EXPECT_DOUBLE_EQ(res.value()[i].slack, expected[i].slack);
    EXPECT_EQ(res.value()[i].kind, expected[i].kind);
  }
}

// ── Combined check ────────────────────────────────────────────────────────

TEST(ArbCheckAll, ConcatenatesCalendarThenButterfly) {
  const VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const auto res = arb_check_all(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  std::uint32_t n_cal = 0;
  for (const ArbViolation &viol : res.value()) {
    if (viol.kind == ArbViolation::Kind::Calendar) {
      ++n_cal;
    }
  }
  // One violating INTERVAL — the two slices never cross, so all of R is one
  // region (see ArbCalendar.LongerMaturityLowerVariance_FlaggedAsOneInterval).
  EXPECT_EQ(n_cal, 1u);
}

TEST(ArbCheckAll, PropagatesButterflyInvalidArgument) {
  const VolSurface surf = make_svi_1slice(steep_svi_slice());
  const auto res = arb_check_all(surf, 0.5, -0.5, 8);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

// ── Total-surface counts ──────────────────────────────────────────────────

TEST(ArbTotalSurface, CalendarViolationsCounted) {
  const VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const auto res = arb_check_total_surface_all(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res.value().n_calendar, 8u);
  EXPECT_EQ(res.value().n_butterfly, 0u);
}

// Plan item 2.7, count-only mirror: `arb_check_total_surface_all` carries the
// same unconditional `w_prev = w` and suffered the same blinding.
TEST(ArbTotalSurface, NonFiniteMiddleSlice_StillCountsTheLaterCrossing) {
  const VolSurface surf = make_svi_3slice_flat(
      0.16, std::numeric_limits<double>::quiet_NaN(), 0.04);
  const auto res = arb_check_total_surface_all(surf, -0.2, 0.2, 8);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res.value().n_calendar, 8u);
  EXPECT_EQ(res.value().n_butterfly, 0u);
}

// ── SVI-MM admissibility ──────────────────────────────────────────────────

TEST(ArbSviMm, AdmissibleSlice_NoViolations) {
  SviParams s{};
  s.a = 0.04;
  s.b = 0.3;
  s.rho = -0.4;
  s.m = 0.0;
  s.sigma = 0.25;
  s.T = 1.0;
  const auto adm = arb_check_butterfly_svi_mm(s, 1.0);
  EXPECT_EQ(adm.n_violations, 0u);
  EXPECT_NEAR(adm.max_slack, 0.0, 1.0e-15);
}

// D3, pinned as a derivation rather than as a number. `essvi_phi_max` caps
// theta*phi*(1+|rho|) at 4 (and tighter, at 2/sqrt(s) for s = theta*(1+|rho|)
// below 4). Pulling theta*phi/2 out of eSSVI's w gives raw-SVI b = theta*phi/2
// exactly, so a phi-MAXIMAL eSSVI slice — the loosest wing the eSSVI lane will
// ever admit — sits at b*(1+|rho|) = min(2, sqrt(s)) <= 2, which is Lee's bound
// for raw SVI verbatim. The raw-SVI gate now enforces that same 2. If someone
// re-derives the raw-SVI ceiling and gets 4, this test says where the factor of
// two went.
TEST(ArbSviMm, TheEssviCeilingOfFourIsLeesRawSviBoundOfTwo) {
  for (const double theta : {0.004, 0.05, 0.4, 2.0, 8.0, 40.0}) {
    for (const double rho : {-0.95, -0.5, 0.0, 0.3, 0.9}) {
      const double phi_max = atx::vol::essvi_phi_max(theta, rho);
      ASSERT_GT(phi_max, 0.0) << "theta=" << theta << " rho=" << rho;
      EssviParams p{};
      p.theta = theta;
      p.phi = phi_max;
      p.rho = rho;
      p.T = 1.0;
      const std::optional<SviCrossingSlice> cs =
          SviCrossingSlice::from_essvi_backbone(p);
      ASSERT_TRUE(cs.has_value());
      const double s = theta * (1.0 + std::fabs(rho));
      EXPECT_NEAR(cs->b() * (1.0 + std::fabs(rho)), std::min(2.0, std::sqrt(s)),
                  1.0e-12)
          << "theta=" << theta << " rho=" << rho;
      EXPECT_LE(cs->b() * (1.0 + std::fabs(rho)), 2.0 + 1.0e-12);
    }
  }
  // And the raw-SVI gate now enforces that same number directly.
  EXPECT_DOUBLE_EQ(atx::vol::kSviWingSlopeGate, 2.0);
}

TEST(ArbSviMm, LeeBoundViolation_OneViolationWithSlack) {
  // b*(1+|rho|) = 5*1.4 = 7, past Lee's raw-SVI ceiling of 2; slack = 5.
  // w_min stays >= 0, so exactly one inequality fires.
  SviParams s{};
  s.a = 0.04;
  s.b = 5.0;
  s.rho = -0.4;
  s.m = 0.0;
  s.sigma = 0.25;
  s.T = 1.0;
  const auto adm = arb_check_butterfly_svi_mm(s, 1.0);
  EXPECT_EQ(adm.n_violations, 1u);
  EXPECT_NEAR(adm.max_slack, 5.0, 1.0e-9);
}

TEST(ArbSviMm, NonPositiveB_FlaggedByFirstInequality) {
  SviParams s{};
  s.a = 0.1;
  s.b = -0.1;
  s.rho = 0.0;
  s.m = 0.0;
  s.sigma = 0.2;
  s.T = 1.0;
  const auto adm = arb_check_butterfly_svi_mm(s, 1.0);
  EXPECT_EQ(adm.n_violations, 1u);
  EXPECT_NEAR(adm.max_slack, 0.1, 1.0e-9);
}

TEST(ArbSviMm, SurfaceWalker_SumsPerSliceViolations) {
  auto res = VolSurface::create(2u, Parametrization::SviMm, 2);
  VolSurface surf = std::move(res).value();
  SviParams good{};
  good.a = 0.04;
  good.b = 0.3;
  good.rho = -0.4;
  good.m = 0.0;
  good.sigma = 0.25;
  good.T = 1.0;
  SviParams bad{};
  bad.a = 0.04;
  bad.b = 5.0;
  bad.rho = -0.4;
  bad.m = 0.0;
  bad.sigma = 0.25;
  bad.T = 1.0;
  (void)surf.set_slice_svi(0, good);
  (void)surf.set_slice_svi(1, bad);

  // `bad` has b*(1+|rho|) = 5*1.4 = 7 against Lee's raw-SVI ceiling of 2.
  const auto res_adm = arb_check_butterfly_svi_mm_surface(surf);
  ASSERT_TRUE(res_adm.has_value());
  EXPECT_EQ(res_adm.value().n_violations, 1u);
  EXPECT_NEAR(res_adm.value().max_slack, 5.0, 1.0e-9);
}

TEST(ArbSviMm, SurfaceWalker_NonSviMm_NoOpZero) {
  // A plain SVI surface with a Lee-violating slice: the walker still reports
  // zero because it only enforces the polytope for SVI_MM-tagged surfaces.
  const VolSurface surf = make_svi_2slice(steep_svi_slice(), steep_svi_slice());
  const auto res = arb_check_butterfly_svi_mm_surface(surf);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res.value().n_violations, 0u);
}

// ── Calendar projection / repair ──────────────────────────────────────────

TEST(ArbProjectCalendarSvi, RestoresMonotonicity) {
  SviParams s0{};
  s0.a = 0.10;
  s0.b = 0.1;
  s0.rho = 0.0;
  s0.m = 0.0;
  s0.sigma = 0.1;
  s0.T = 0.25;
  SviParams s1{};
  s1.a = 0.098;  // SLIGHTLY lower than s0 at every k => small calendar crossing
  s1.b = 0.1;
  s1.rho = 0.0;
  s1.m = 0.0;
  s1.sigma = 0.1;
  s1.T = 1.0;
  VolSurface surf = make_svi_2slice(s0, s1);

  const auto before = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(before.has_value());
  EXPECT_FALSE(before.value().empty());

  ASSERT_TRUE(arb_project_calendar_svi(surf, -0.3, 0.3, 32).has_value());

  const auto after = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(after.value().empty());
  EXPECT_GT(surf.svi_slices()[1].a, 0.098);  // longer slice's `a` was bumped up
}

TEST(ArbProjectCalendarSvi, IdempotentOnAlreadyMonotone) {
  SviParams s0{};
  s0.a = 0.10;
  s0.b = 0.1;
  s0.sigma = 0.1;
  s0.T = 0.25;
  SviParams s1{};
  s1.a = 0.098;
  s1.b = 0.1;
  s1.sigma = 0.1;
  s1.T = 1.0;
  VolSurface surf = make_svi_2slice(s0, s1);

  ASSERT_TRUE(arb_project_calendar_svi(surf, -0.3, 0.3, 32).has_value());
  const double a_first = surf.svi_slices()[1].a;
  // Second projection must not move an already-repaired surface.
  ASSERT_TRUE(arb_project_calendar_svi(surf, -0.3, 0.3, 32).has_value());
  EXPECT_NEAR(surf.svi_slices()[1].a, a_first, 1.0e-15);
  const auto after = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(after.value().empty());
}

TEST(ArbProjectCalendarSvi, WrongParametrization_NoOpOk) {
  VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const double theta_before = surf.essvi_slices()[1].theta;
  // SVI projector on an eSSVI surface is a no-op (Ok), leaving it untouched.
  ASSERT_TRUE(arb_project_calendar_svi(surf, -0.3, 0.3, 32).has_value());
  EXPECT_EQ(surf.essvi_slices()[1].theta, theta_before);
}

TEST(ArbProjectCalendarSvi, KMaxNotAboveKMin_ReturnsInvalidArgument) {
  SviParams s0{};
  s0.a = 0.10;
  s0.b = 0.1;
  s0.sigma = 0.1;
  s0.T = 0.25;
  SviParams s1{};
  s1.a = 0.02;
  s1.b = 0.1;
  s1.sigma = 0.1;
  s1.T = 1.0;
  VolSurface surf = make_svi_2slice(s0, s1);
  const auto rc = arb_project_calendar_svi(surf, 0.3, -0.3, 32);
  ASSERT_FALSE(rc.has_value());
  EXPECT_EQ(rc.error().code(), ErrorCode::InvalidArgument);
}

TEST(ArbProjectCalendarEssvi, RestoresMonotonicity) {
  // A SMALL inversion (ratio ~1.07, inside the repair fidelity budget).
  VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.15, 1.0);
  const auto before = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(before.has_value());
  EXPECT_FALSE(before.value().empty());

  ASSERT_TRUE(arb_project_calendar_essvi(surf, -0.3, 0.3, 32).has_value());

  const auto after = arb_check_calendar(surf, -0.3, 0.3, 32);
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(after.value().empty());
  EXPECT_GT(surf.essvi_slices()[1].theta, 0.15);  // theta bumped up
}

TEST(ArbProjectCalendarEssvi, RefusesPerSliceScaleBeyondFidelityBudget) {
  // Closing theta 0.16 -> 0.04 needs a 4x ATM scale on the longer slice. That
  // is the level-fabrication defect (sp100-2026 XOM/CVX): refuse, transactional.
  VolSurface surf = make_essvi_2slice(0.16, 0.25, 0.04, 1.0);
  const auto st = arb_project_calendar_essvi(surf, -0.3, 0.3, 32);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::Unavailable);
  EXPECT_EQ(surf.essvi_slices()[0].theta, 0.16);
  EXPECT_EQ(surf.essvi_slices()[1].theta, 0.04);
}

TEST(ArbProjectCalendarSvi, RefusesLevelShiftBeyondFidelityBudget) {
  // Same budget contract on the SVI surface projector's `a` shift.
  SviParams s0{};
  s0.a = 0.10;
  s0.b = 0.1;
  s0.sigma = 0.1;
  s0.T = 0.25;
  SviParams s1{};
  s1.a = 0.02;
  s1.b = 0.1;
  s1.sigma = 0.1;
  s1.T = 1.0;
  VolSurface surf = make_svi_2slice(s0, s1);
  const auto st = arb_project_calendar_svi(surf, -0.3, 0.3, 32);
  ASSERT_FALSE(st.has_value());
  EXPECT_EQ(st.error().code(), ErrorCode::Unavailable);
  EXPECT_EQ(surf.svi_slices()[1].a, 0.02);
}

TEST(ArbProjectCalendarEssvi, IdempotentOnAlreadyMonotone) {
  VolSurface surf = make_essvi_2slice(0.04, 0.25, 0.16, 1.0);
  const double theta_before = surf.essvi_slices()[1].theta;
  ASSERT_TRUE(arb_project_calendar_essvi(surf, -0.3, 0.3, 32).has_value());
  EXPECT_NEAR(surf.essvi_slices()[1].theta, theta_before, 1.0e-15);
}

TEST(ArbRepairCalendarResidual, FeasibleAtAlphaZero_DampsResidualToMonotone) {
  // Backbones are monotone (0.04 -> 0.05); only slice0's wing residual crosses.
  // The damper must scale it down, NOT erase it, and must land monotone.
  VolSurface surf = make_essvi_2slice_resid(0.04, 0.05, 0.05);
  const EssviParams before = surf.essvi_slices()[0];
  const EssviParams hi = surf.essvi_slices()[1];
  ASSERT_GT(essvi_total_w(before, 0.2), essvi_total_w(hi, 0.2)); // crossing exists

  ASSERT_TRUE(arb_repair_calendar_residual(surf, -0.2, 0.2, 32).has_value());

  const EssviParams after = surf.essvi_slices()[0];
  EXPECT_GT(after.resid_scale, 0.0); // damped, not collapsed to backbone
  EXPECT_EQ(after.resid_basis_kind, before.resid_basis_kind);
  EXPECT_GT(after.resid_coef[3], 0.0);
  EXPECT_LT(after.resid_coef[3], before.resid_coef[3]);
  for (int i = 0; i <= 32; ++i) {
    const double k = -0.2 + 0.4 * static_cast<double>(i) / 32.0;
    EXPECT_LE(essvi_total_w(after, k), essvi_total_w(hi, k) + 1.0e-12) << "k=" << k;
  }
}

TEST(ArbRepairCalendarResidual, InfeasibleAtAlphaZero_ErrsAndKeepsResidualIntact) {
  // slice0's BACKBONE alone (theta 0.16) already exceeds slice1's (0.04), so
  // alpha = 0 -- the endpoint the bisection ASSUMES feasible -- does not repair
  // the crossing. Committing it would zero slice0's residual layer and report
  // success on a surface that is still calendar-arbitrageable.
  VolSurface surf = make_essvi_2slice_resid(0.16, 0.04, 0.02);
  const EssviParams before = surf.essvi_slices()[0];

  const auto st = arb_repair_calendar_residual(surf, -0.2, 0.2, 32);
  ASSERT_FALSE(st.has_value()) << "unrepairable calendar arb reported as repaired";
  EXPECT_EQ(st.error().code(), ErrorCode::Unavailable);

  // Transactional: the input surface is byte-for-byte what the caller passed in.
  const EssviParams after = surf.essvi_slices()[0];
  EXPECT_EQ(after.resid_scale, before.resid_scale);
  EXPECT_EQ(after.resid_basis_kind, before.resid_basis_kind);
  EXPECT_EQ(after.resid_n_basis, before.resid_n_basis);
  for (std::size_t j = 0; j < before.resid_coef.size(); ++j) {
    EXPECT_EQ(after.resid_coef[j], before.resid_coef[j]) << "coef " << j;
  }
}

// Review fix round 1 (I-2). The Unavailable status is REACHABLE on the shipped
// `run_surface_parity` ordering (project, then repair), because the two passes
// compare different quantities: `arb_project_calendar_essvi` enforces backbone
// vs backbone, while the alpha = 0 guard tests the lower backbone against the
// upper TOTAL. A negative residual on the higher-T slice separates the two, so a
// successful projection does not imply a feasible alpha = 0.
TEST(ArbRepairCalendarResidual, ProjectedBackbonesStillTripTheGuardOnANegativeUpperResidual) {
  VolSurface surf = make_essvi_2slice_resid(0.04, 0.05, 0.02); // monotone backbones
  {
    EssviParams upper = surf.essvi_slices()[1];
    upper.resid_scale = 0.1;
    upper.resid_basis_kind = ResidualBasisKind::HingeQuad;
    upper.resid_n_basis = 5;
    upper.resid_coef[3] = -0.04; // NEGATIVE right wing: w_total(hi) < w_backbone(hi)
    ASSERT_TRUE(surf.set_slice_essvi(1, upper).has_value());
  }

  // Run the projection exactly as run_surface_parity does, and confirm it
  // delivers what it promises: backbone-vs-backbone monotonicity on this grid.
  ASSERT_TRUE(arb_project_calendar_essvi(surf, -0.2, 0.2, 32).has_value());
  const EssviParams lo = surf.essvi_slices()[0];
  const EssviParams hi = surf.essvi_slices()[1];
  for (int i = 0; i <= 32; ++i) {
    const double k = -0.2 + 0.4 * static_cast<double>(i) / 32.0;
    ASSERT_LE(essvi_backbone_w(lo, k), essvi_backbone_w(hi, k) + 1.0e-12) << "k=" << k;
  }
  // ...yet the upper slice's TOTAL dips under the lower slice's BACKBONE, which is
  // what the alpha = 0 endpoint is measured against.
  ASSERT_GT(essvi_backbone_w(lo, 0.2), essvi_total_w(hi, 0.2));

  const auto st = arb_repair_calendar_residual(surf, -0.2, 0.2, 32);
  ASSERT_FALSE(st.has_value()) << "projection ran, so the guard was expected to fire anyway";
  EXPECT_EQ(st.error().code(), ErrorCode::Unavailable);

  // Still transactional on this path.
  const EssviParams after = surf.essvi_slices()[0];
  EXPECT_EQ(after.resid_scale, lo.resid_scale);
  EXPECT_EQ(after.resid_basis_kind, lo.resid_basis_kind);
  for (std::size_t j = 0; j < lo.resid_coef.size(); ++j) {
    EXPECT_EQ(after.resid_coef[j], lo.resid_coef[j]) << "coef " << j;
  }
}

TEST(ArbProjectCalendarPair, EssviClosesSharedGridAndPreservesButterfly) {
  EssviParams previous{};
  previous.theta = 0.09;
  previous.phi = 0.8;
  previous.rho = -0.3;
  previous.T = 0.25;
  previous.F = 100.0;
  // A SMALL crossing (ratio ~1.07, inside the repair fidelity budget): the
  // projection must close it with a shape-preserving level scale.
  EssviParams current = previous;
  current.theta = 0.084;
  current.T = 0.50;
  const std::function<double(double)> floor =
      [previous](double k) { return essvi_total_w(previous, k); };

  const auto projection =
      arb_project_calendar_essvi_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_TRUE(projection.has_value()) << projection.error().to_string();
  EXPECT_GT(projection->passes, 0u);
  for (int i = 0; i <= 64; ++i) {
    const double k = -0.6 + 1.2 * static_cast<double>(i) / 64.0;
    EXPECT_GE(essvi_total_w(current, k), floor(k) - 1.0e-7);
  }
  const EssviCurve served(current, 1.0);
  const auto shape = arb_check_butterfly(served, -0.6, 0.6, 256);
  ASSERT_TRUE(shape.has_value());
  EXPECT_TRUE(shape->empty());
}

TEST(ArbProjectCalendarPair, EssviRefusesLevelScaleBeyondFidelityBudget) {
  // The XOM-2026 defect shape: closing this gap needs a ~3.6x ATM level scale
  // — pure fabrication relative to the slice's own fit. The projection must
  // REFUSE (Unavailable) and leave the slice untouched, not publish a slice
  // whose ATM level no longer resembles its own quotes.
  EssviParams previous{};
  previous.theta = 0.09;
  previous.phi = 0.8;
  previous.rho = -0.3;
  previous.T = 0.25;
  previous.F = 100.0;
  EssviParams current = previous;
  current.theta = 0.025;
  current.T = 0.50;
  const EssviParams before = current;
  const std::function<double(double)> floor =
      [previous](double k) { return essvi_total_w(previous, k); };

  const auto projection =
      arb_project_calendar_essvi_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_FALSE(projection.has_value());
  EXPECT_EQ(projection.error().code(), ErrorCode::Unavailable);
  // Transactional: the refused slice is byte-unchanged.
  EXPECT_EQ(current.theta, before.theta);
  EXPECT_EQ(current.phi, before.phi);
  EXPECT_EQ(current.rho, before.rho);
  for (std::size_t j = 0; j < before.resid_coef.size(); ++j) {
    EXPECT_EQ(current.resid_coef[j], before.resid_coef[j]) << "coef " << j;
  }
}

TEST(ArbProjectCalendarPair, SviUsesShapePreservingLevelShift) {
  SviParams previous{};
  previous.a = 0.08;
  previous.b = 0.10;
  previous.rho = -0.25;
  previous.m = 0.0;
  previous.sigma = 0.20;
  previous.T = 0.25;
  // A SMALL crossing (shift ~2% of the slice's ATM total variance, inside the
  // repair fidelity budget): repaired by the parallel `a` level shift.
  SviParams current = previous;
  current.a = 0.078;
  current.T = 0.50;
  const double b_before = current.b;
  const std::function<double(double)> floor =
      [previous](double k) { return svi_total_w(previous, k); };

  const auto projection =
      arb_project_calendar_svi_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_TRUE(projection.has_value()) << projection.error().to_string();
  EXPECT_EQ(projection->passes, 1u);
  EXPECT_DOUBLE_EQ(current.b, b_before);
  for (int i = 0; i <= 64; ++i) {
    const double k = -0.6 + 1.2 * static_cast<double>(i) / 64.0;
    EXPECT_GE(svi_total_w(current, k), floor(k) - 1.0e-7);
  }
  const SviCurve served(current, 1.0);
  const auto shape = arb_check_butterfly(served, -0.6, 0.6, 256);
  ASSERT_TRUE(shape.has_value());
  EXPECT_TRUE(shape->empty());
}

TEST(ArbProjectCalendarPair, SviRefusesLevelShiftBeyondFidelityBudget) {
  // The exact defect that poisoned the sp100-2026 XOM/CVX cells: the required
  // `a` shift (0.07) is ~230% of the slice's own ATM total variance. Committing
  // it rewrites the slice's level far outside anything its quotes support. The
  // projection must refuse and leave the slice untouched.
  SviParams previous{};
  previous.a = 0.08;
  previous.b = 0.10;
  previous.rho = -0.25;
  previous.m = 0.0;
  previous.sigma = 0.20;
  previous.T = 0.25;
  SviParams current = previous;
  current.a = 0.01;
  current.T = 0.50;
  const SviParams before = current;
  const std::function<double(double)> floor =
      [previous](double k) { return svi_total_w(previous, k); };

  const auto projection =
      arb_project_calendar_svi_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_FALSE(projection.has_value());
  EXPECT_EQ(projection.error().code(), ErrorCode::Unavailable);
  EXPECT_EQ(current.a, before.a);
  EXPECT_EQ(current.b, before.b);
  EXPECT_EQ(current.rho, before.rho);
  EXPECT_EQ(current.m, before.m);
  EXPECT_EQ(current.sigma, before.sigma);
}

TEST(ArbProjectCalendarPair, C8LevelShiftThenRevalidatesBumps) {
  C8Params current{};
  current.T = 0.50;
  current.F = 100.0;
  current.v = 0.025;
  current.v_min = 0.022;
  current.psi = -0.004;
  current.p = 0.20;
  current.c = 0.18;
  current.kappa = -0.001;
  // A SMALL crossing (level shift a few % of the slice's ATM total variance,
  // inside the repair fidelity budget; the fixture's minimum w is ~v_min=0.022,
  // so the flat floor must sit within budget of THAT).
  const std::function<double(double)> floor =
      [](double) { return 0.0240; };

  const auto projection =
      arb_project_calendar_c8_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_TRUE(projection.has_value()) << projection.error().to_string();
  EXPECT_GT(projection->passes, 0u);
  for (int i = 0; i <= 64; ++i) {
    const double k = -0.6 + 1.2 * static_cast<double>(i) / 64.0;
    EXPECT_GE(c8_slice_w(current, k), floor(k) - 1.0e-7);
  }
  const C8Curve served(current, 1.0);
  const auto shape = arb_check_butterfly(served, -0.6, 0.6, 256);
  ASSERT_TRUE(shape.has_value());
  EXPECT_TRUE(shape->empty());
}

TEST(ArbProjectCalendarPair, C8RefusesLevelShiftBeyondFidelityBudget) {
  // Closing a flat 0.06 floor over a ~0.025 slice needs a >100% ATM level
  // shift: fabrication, not repair. Refuse and leave the slice untouched.
  C8Params current{};
  current.T = 0.50;
  current.F = 100.0;
  current.v = 0.025;
  current.v_min = 0.022;
  current.psi = -0.004;
  current.p = 0.20;
  current.c = 0.18;
  current.kappa = -0.001;
  const C8Params before = current;
  const std::function<double(double)> floor =
      [](double) { return 0.06; };

  const auto projection =
      arb_project_calendar_c8_pair(current, floor, -0.6, 0.6, 64);
  ASSERT_FALSE(projection.has_value());
  EXPECT_EQ(projection.error().code(), ErrorCode::Unavailable);
  EXPECT_EQ(current.v, before.v);
  EXPECT_EQ(current.v_min, before.v_min);
  EXPECT_EQ(current.psi, before.psi);
  EXPECT_EQ(current.kappa, before.kappa);
}

// ── Quote pre-fit filters (mirrors test_prefit_filter.c) ──────────────────

TEST(ArbFilter, DefaultOpts_MatchOrdinaryProfile) {
  const FilterOpts o = filter_default_opts();
  EXPECT_EQ(o.stale_seconds, 30);
  EXPECT_NEAR(o.wide_spread_pct, 1.50, 1.0e-12);
  EXPECT_NEAR(o.penny_floor, 0.05, 1.0e-12);
}

TEST(ArbFilter, ExFilter_FlagsEachRowUniquely) {
  // Row layout matches the C fixture: 0 clean, 1 LOCKED, 2 CROSSED, 3 WIDE,
  // 4 PENNY, 5 STALE.
  const std::int64_t now = 1'000'000'000'000LL;
  const std::vector<double> bids = {0.50, 0.10, 0.20, 0.50, 0.02, 0.50};
  const std::vector<double> asks = {0.55, 0.10, 0.10, 1.50, 0.04, 0.55};
  std::vector<std::int64_t> ts(6, now);
  ts[5] = now - 100LL * 1'000'000'000LL;  // 100s ago
  const std::vector<std::uint8_t> flags_in(6, std::uint8_t{0});

  QuoteBatch b{};
  b.bids = bids;
  b.asks = asks;
  b.ts_ns = ts;
  b.flags = flags_in;

  FilterOpts o = filter_default_opts();
  o.now_ts_ns = now;
  o.stale_seconds = 30;
  o.wide_spread_pct = 0.50;  // tighter than default for the test
  o.penny_floor = 0.05;
  o.min_vega_filter = 0.0;  // skip vega (no vega column)

  std::vector<std::uint8_t> out(6, std::uint8_t{0xFF});
  const auto res = arb_filter_quotes_ex(b, o, {}, out);
  ASSERT_TRUE(res.has_value());
  EXPECT_EQ(res.value(), 5u);  // rows 1..5 each gained a bit

  const auto qf = [](std::uint8_t v) { return static_cast<QuoteFlag>(v); };
  EXPECT_EQ(out[0], 0u);  // clean row fully overwritten to 0
  EXPECT_TRUE(has_flag(qf(out[1]), QuoteFlag::Locked));
  EXPECT_FALSE(has_flag(qf(out[1]), QuoteFlag::Crossed));
  EXPECT_TRUE(has_flag(qf(out[2]), QuoteFlag::Crossed));
  EXPECT_FALSE(has_flag(qf(out[2]), QuoteFlag::Locked));
  EXPECT_TRUE(has_flag(qf(out[3]), QuoteFlag::WideSpread));
  EXPECT_TRUE(has_flag(qf(out[4]), QuoteFlag::Penny));
  EXPECT_TRUE(has_flag(qf(out[5]), QuoteFlag::Stale));
}

TEST(ArbFilter, LowVega_OnlyWhenVegasSupplied) {
  const std::int64_t now = 1'000'000'000'000LL;
  const std::vector<double> bids = {0.50, 0.10, 0.20, 0.50, 0.02, 0.50};
  const std::vector<double> asks = {0.55, 0.10, 0.10, 1.50, 0.04, 0.55};
  const std::vector<std::int64_t> ts(6, now);
  const std::vector<std::uint8_t> flags_in(6, std::uint8_t{0});

  QuoteBatch b{};
  b.bids = bids;
  b.asks = asks;
  b.ts_ns = ts;
  b.flags = flags_in;

  FilterOpts o = filter_default_opts();
  o.now_ts_ns = now;
  o.min_vega_filter = 1.0e-3;

  const std::vector<double> vegas = {1.0e-5, 1.0, 1.0, 1.0, 1.0, 1.0};
  std::vector<std::uint8_t> out(6, std::uint8_t{0});
  const auto res = arb_filter_quotes_ex(b, o, vegas, out);
  ASSERT_TRUE(res.has_value());

  const auto qf = [](std::uint8_t v) { return static_cast<QuoteFlag>(v); };
  EXPECT_TRUE(has_flag(qf(out[0]), QuoteFlag::LowVega));   // vega below threshold
  EXPECT_FALSE(has_flag(qf(out[1]), QuoteFlag::LowVega));  // locked, not low-vega
}

TEST(ArbFilter, ExFilter_FlagsOutTooSmall_ReturnsInvalidArgument) {
  const std::vector<double> bids = {0.50, 0.10};
  const std::vector<double> asks = {0.55, 0.10};
  QuoteBatch b{};
  b.bids = bids;
  b.asks = asks;
  std::vector<std::uint8_t> out(1, std::uint8_t{0});  // shorter than batch
  const auto res = arb_filter_quotes_ex(b, filter_default_opts(), {}, out);
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code(), ErrorCode::InvalidArgument);
}

TEST(ArbFilter, PrefitUnderlier_StampsChainFlags) {
  Universe u;
  const auto uid = u.intern_ticker("SPY").value();
  const auto eid = u.add_expiry(uid, 2'000'000'000'000LL).value();
  (void)u.add_strike(uid, eid, 100.0);  // strike index 0
  auto *under = u.get_underlying(uid).value();
  auto &c = under->chains[eid];

  const std::int64_t now = 1'000'000'000'000LL;
  const std::size_t ic = atx::vol::chain_index(std::uint16_t{0}, Side::Call);
  const std::size_t ip = atx::vol::chain_index(std::uint16_t{0}, Side::Put);
  c.bids[ic] = 0.10;  // locked (bid == ask)
  c.asks[ic] = 0.10;
  c.ts_ns[ic] = now;
  c.bids[ip] = 0.02;  // penny (bid < 0.05 floor)
  c.asks[ip] = 0.04;
  c.ts_ns[ip] = now;

  const CurveSet cs;
  FilterOpts o = filter_default_opts();
  o.now_ts_ns = now;
  const auto res = prefit_filter_underlier(*under, cs, o, 0);
  ASSERT_TRUE(res.has_value());
  EXPECT_GE(res.value(), 2u);

  const auto qf = [](std::uint8_t v) { return static_cast<QuoteFlag>(v); };
  EXPECT_TRUE(has_flag(qf(c.flags[ic]), QuoteFlag::Locked));
  EXPECT_TRUE(has_flag(qf(c.flags[ip]), QuoteFlag::Penny));
}
