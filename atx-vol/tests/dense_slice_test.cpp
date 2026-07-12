#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "atx/vol/black76.hpp"      // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"        // FitObs
#include "atx/vol/dense_slice.hpp"  // fit_convex_slice, ConvexSliceFit
#include "atx/vol/arb.hpp"          // arb_check_calendar
#include "atx/vol/types.hpp"        // Side
#include "atx/vol/vol_curve.hpp"    // fit_slice_curve, CurveSurface

// Phase 1 of the arbitrage-constrained dense surface: the per-slice convex
// call-price QP. These tests pin the two properties the whole approach rests on:
//   (1) the fitted call curve is ALWAYS arbitrage-free (convex, non-increasing,
//       positive) — even fed non-convex / noisy input, because no-arb is a HARD
//       constraint of the QP, not a post-hoc repair; and
//   (2) on clean arbitrage-free data it NEAR-INTERPOLATES (recovers the truth
//       vol to a few bp), which is what a 3-DoF eSSVI cannot do.

namespace {

using atx::vol::black76_price;
using atx::vol::black76_value_and_vega;
using atx::vol::ConvexFitOpts;
using atx::vol::FitObs;
using atx::vol::fit_convex_slice;
using atx::vol::Side;

// One OTM observation at strike K under a given vol, with a Black-76 vega weight.
FitObs mk_obs(double F, double T, double df, double K, double vol,
              double spread = 0.02) {
  const Side side = (K >= F) ? Side::Call : Side::Put;
  FitObs o{};
  o.K = K;
  o.F = F;
  o.df = df;
  o.k = std::log(K / F);
  o.side = side;
  o.mid = black76_price(F, K, T, vol, df, side);
  o.spread = spread;
  o.vega = black76_value_and_vega(F, K, T, vol, df, side).vega;
  o.sigma_mkt = vol;
  return o;
}

// The fitted call curve must be convex, non-increasing, and non-negative.
void expect_arb_free(const atx::vol::ConvexSliceFit& fit) {
  const auto& u = fit.u;
  const auto& C = fit.C;
  ASSERT_GE(u.size(), std::size_t{3});
  for (std::size_t i = 0; i < C.size(); ++i) {
    const double intrinsic = fit.df * std::max(fit.F - u[i], 0.0);
    EXPECT_GE(C[i], intrinsic - 1.0e-8) << "intrinsic bound at node " << i;
    EXPECT_LE(C[i], fit.df * fit.F + 1.0e-8) << "upper bound at node " << i;
  }
  for (std::size_t i = 0; i + 1 < C.size(); ++i) {
    EXPECT_LE(C[i + 1], C[i] + 1.0e-7) << "monotone (call falls with strike) at " << i;
  }
  for (std::size_t i = 1; i + 1 < C.size(); ++i) {
    const double left = (C[i] - C[i - 1]) / (u[i] - u[i - 1]);
    const double right = (C[i + 1] - C[i]) / (u[i + 1] - u[i]);
    EXPECT_GE(right - left, -1.0e-7) << "convexity (butterfly) at node " << i;
  }
}

std::vector<double> strike_grid(double F) {
  std::vector<double> ks;
  for (double K = 0.70 * F; K <= 1.30 * F + 1e-9; K += 0.02 * F) {
    ks.push_back(K);
  }
  return ks;
}

// An in-the-money-heavy observation set (~11 strikes) at a flat vol, with one
// deep-ITM print forced far below its no-arb intrinsic floor (df*(F-K)) and
// pinned to a tight spread (=> high fit weight). With only convexity +
// monotonicity enforced (no slope-below bound), the near-interpolating fit can
// produce a segment whose slope dips below -df around this print — that is the
// case `bound_slope_below` is meant to rule out.
std::vector<FitObs> make_synthetic_slice_obs(double F, double T, double df,
                                              double sigma) {
  const std::vector<double> strikes = {40, 55, 65, 72, 78, 84, 90, 96, 104, 115, 130};
  std::vector<FitObs> obs;
  obs.reserve(strikes.size());
  for (const double K : strikes) {
    obs.push_back(mk_obs(F, T, df, K, sigma));
  }
  for (FitObs& o : obs) {
    if (std::fabs(o.K - 65.0) < 1.0e-9) {
      o.mid *= 0.5;             // artificially cheap deep-ITM print
      o.spread = 1.0e-4;        // tight spread => near-interpolated by the fit
      o.vega = std::max(o.vega, 1.0);
    }
  }
  return obs;
}

// A DISCRIMINATING call board for the interval-loss band test: a MID fit is pulled
// OUTSIDE a band while an INTERVAL fit can stay inside every band. The mechanism:
//
//   * a convex, decreasing, positive TRUE call curve `g_true` (quadratic in strike),
//   * plus a localized CONCAVE bump over the three central strikes (120/125/130) so
//     the board is NON-convex there — a convex fit cannot follow the bump,
//   * per-strike bands CENTERED on the (bumped) mids: wide on the shoulders and a
//     moderate band at the bump peak (K=125), each centered on `g_true` off the bump.
//
// The Mid loss anchors the fit to the mids (the band CENTERS) with weight
// vega²/spread². It cannot reproduce the concave bump, so the least-squares convex
// projection comes out nearly LINEAR from the peak outward — but LIFTED by the bump.
// Because `g_true` curves DOWN faster than any line on the right wing, that lifted
// Mid fit sits ABOVE the right-wing bands (K=135…150, centered on the steeply
// decaying `g_true`): a clear multi-band overshoot. The Interval loss owes zero data
// penalty anywhere inside a band, so it need not honor the lifted central mids; it
// threads the SMOOTHEST convex curve (a slightly steeper line) that stays within
// every band — that in-band curve is exactly the interval solution, so the
// convex-in-band feasible set is non-empty. Uniform strike grid + ≤ node_cap strikes
// ⇒ node grid == strike grid (design matrix B = identity), so the Mid violation is
// the LOSS's doing, not an interpolation artifact. All strikes are OTM calls (K≥F),
// keeping mids positive; NOT used by the A3/A4 tests; `sigma` only sets a realistic
// per-obs vega weight via mk_obs.
std::vector<FitObs> make_synthetic_slice_obs_wideband(double F, double T, double df,
                                                      double sigma) {
  const std::vector<double> strikes = {100, 105, 110, 115, 120, 125,
                                       130, 135, 140, 145, 150};
  const double kmax = 170.0;
  const auto g_true = [&](double K) { return 0.004 * (kmax - K) * (kmax - K); };
  std::vector<FitObs> obs;
  obs.reserve(strikes.size());
  for (const double K : strikes) {
    FitObs o = mk_obs(F, T, df, K, sigma);          // realistic side / vega weight
    double bump = 0.0;                               // localized concave hump…
    if (K == 120.0) bump = 1.5;
    if (K == 125.0) bump = 3.0;                      // …peak of the concavity
    if (K == 130.0) bump = 1.5;
    o.mid = g_true(K) + bump;                        // convex base + concave bump
    o.spread = (K == 125.0) ? 2.0 : 6.0;             // moderate peak band, wide else
    obs.push_back(o);
  }
  return obs;
}

}  // namespace

TEST(DenseSlice, FlatVolIsRecoveredAndArbFree) {
  constexpr double F = 100.0, T = 0.25, r = 0.03, vol = 0.22;
  const double df = std::exp(-r * T);
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, vol));
  }

  ConvexFitOpts opts;
  opts.lambda = 1.0e-4;  // near-interpolation
  const auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_arb_free(*fit);

  // Near-interpolation: a flat-vol board's call prices are exactly convex, so the
  // fit recovers the generating vol at every interior strike to a few bp.
  int checked = 0;
  for (const double K : strike_grid(F)) {
    if (K < 0.80 * F || K > 1.20 * F) continue;  // interior, high-vega
    const double iv = fit->iv(std::log(K / F));
    ASSERT_TRUE(std::isfinite(iv));
    EXPECT_NEAR(iv, vol, 0.01) << "K=" << K;
    ++checked;
  }
  EXPECT_GT(checked, 5);
}

TEST(DenseSlice, SmileIsRecovered) {
  constexpr double F = 100.0, T = 0.5, r = 0.03;
  const double df = std::exp(-r * T);
  // A mild arbitrage-free skew: down-sloping with convex curvature in k.
  auto vol_of = [](double k) { return 0.22 - 0.10 * k + 0.30 * k * k; };
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, vol_of(std::log(K / F))));
  }

  const auto fit = fit_convex_slice(obs, F, T, df, {});
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_arb_free(*fit);
  for (const double K : {95.0, 100.0, 105.0}) {
    const double iv = fit->iv(std::log(K / F));
    ASSERT_TRUE(std::isfinite(iv));
    EXPECT_NEAR(iv, vol_of(std::log(K / F)), 0.02) << "K=" << K;
  }
}

TEST(DenseSlice, NonConvexNoiseIsProjectedToArbFree) {
  constexpr double F = 100.0, T = 0.25, r = 0.03, vol = 0.22;
  const double df = std::exp(-r * T);
  std::vector<FitObs> obs;
  int i = 0;
  for (const double K : strike_grid(F)) {
    // Alternating +/- price perturbation makes the raw board non-convex (butterfly
    // arb) — the fit MUST still return a convex (arb-free) curve.
    FitObs o = mk_obs(F, T, df, K, vol);
    o.mid *= (i % 2 == 0) ? 1.06 : 0.94;
    obs.push_back(o);
    ++i;
  }
  const auto fit = fit_convex_slice(obs, F, T, df, {});
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_arb_free(*fit);  // the whole point: arb-free despite arb-violating input
}

TEST(DenseSlice, WideBoardUsesClusteredNodesAndStaysArbFree) {
  // A wide, dense board (>node_cap strikes) exercises the ATM-clustered node grid
  // + design matrix. The fit must stay arbitrage-free and still track a mild smile.
  constexpr double F = 600.0, T = 0.3, r = 0.03;
  const double df = std::exp(-r * T);
  auto vol_of = [](double k) { return 0.20 - 0.08 * k + 0.25 * k * k; };
  std::vector<FitObs> obs;
  for (double K = 0.55 * F; K <= 1.45 * F + 1e-9; K += 0.01 * F) {  // ~90 strikes
    obs.push_back(mk_obs(F, T, df, K, vol_of(std::log(K / F))));
  }
  ConvexFitOpts opts;
  opts.node_cap = 40;
  const auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_LE(fit->u.size(), std::size_t{40});  // capped node count
  EXPECT_GE(fit->u.size(), std::size_t{20});
  expect_arb_free(*fit);
  for (const double K : {560.0, 600.0, 640.0}) {
    const double iv = fit->iv(std::log(K / F));
    ASSERT_TRUE(std::isfinite(iv));
    EXPECT_NEAR(iv, vol_of(std::log(K / F)), 0.02) << "K=" << K;
  }
}

TEST(DenseSlice, RejectsTooFewStrikes) {
  constexpr double F = 100.0, T = 0.25;
  const double df = std::exp(-0.03 * T);
  std::vector<FitObs> obs = {mk_obs(F, T, df, 95.0, 0.2),
                             mk_obs(F, T, df, 105.0, 0.2)};
  EXPECT_FALSE(fit_convex_slice(obs, F, T, df, {}).has_value());
}

TEST(ConvexSliceFit, SlopeBelowBoundHonored) {
  using namespace atx::vol;
  // Build a simple in-the-money-heavy obs set where the unconstrained slope could
  // dip below -df; enable the bound and assert it holds at every node pair.
  std::vector<FitObs> obs = make_synthetic_slice_obs(/*F=*/100.0, /*T=*/0.5,
                                                     /*df=*/0.98, /*sigma=*/0.2);
  ConvexFitOpts opts; opts.bound_slope_below = true;
  auto fit = fit_convex_slice(obs, 100.0, 0.5, 0.98, opts);
  ASSERT_TRUE(fit.has_value());
  const double df = 0.98;
  for (std::size_t j = 0; j + 1 < fit->u.size(); ++j) {
    const double slope = (fit->C[j + 1] - fit->C[j]) / (fit->u[j + 1] - fit->u[j]);
    EXPECT_GE(slope, -df - 1e-7);
  }
}

TEST(ConvexSliceFit, CalendarFloorLiftsLowVarianceSlice) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  // Obs imply LOW vol (0.15); floor demands total variance of a 0.25-vol prev
  // slice at the SAME T. Floor must lift w above the unconstrained fit.
  std::vector<FitObs> obs = make_synthetic_slice_obs(F, T, df, 0.15);
  auto w_prev = [&](double /*k*/) {
    const double sig = 0.25;
    return sig * sig * T;   // flat prev total variance
  };
  auto free_fit = fit_convex_slice(obs, F, T, df, {});
  auto floored  = fit_convex_slice(obs, F, T, df, {}, w_prev);
  ASSERT_TRUE(free_fit && floored);
  // At the money, floored total variance >= prev (minus tol), and >= free fit.
  const double w_floor = 0.25 * 0.25 * T;
  const double s_atm = floored->iv(0.0);
  EXPECT_GE(s_atm * s_atm * T, w_floor - 1e-6);
  EXPECT_GE(floored->iv(0.0), free_fit->iv(0.0) - 1e-9);
}

TEST(ConvexSliceFit, CalendarFloorSlackIsBitIdentical) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  // A clean arbitrage-free board (real Black-76 prices, no synthetic mispricing —
  // unlike make_synthetic_slice_obs's deliberately cheapened deep-ITM print, which
  // makes even the UNCONSTRAINED fit dip under its own intrinsic value near that
  // print, so ANY positive-vol calendar floor would bind there regardless of
  // magnitude). Here every node's free-fit price sits at/above its true fair
  // value, so a prev vol far BELOW the fitted vol is genuinely slack everywhere.
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, 0.30));
  }
  auto w_prev = [&](double) { return 0.10 * 0.10 * T; };  // prev far BELOW → slack
  auto free_fit = fit_convex_slice(obs, F, T, df, {});
  auto floored  = fit_convex_slice(obs, F, T, df, {}, w_prev);
  ASSERT_TRUE(free_fit && floored);
  ASSERT_EQ(free_fit->C.size(), floored->C.size());
  for (std::size_t j = 0; j < free_fit->C.size(); ++j) {
    EXPECT_NEAR(free_fit->C[j], floored->C[j], 1e-12);  // slack ⇒ identical
  }
}

TEST(ConvexSliceFit, IntervalLossPutsPriceInsideBand) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  // Discriminating board: a concave bump the convex fit cannot follow, wide
  // shoulder bands, a moderate peak band (see make_synthetic_slice_obs_wideband).
  std::vector<FitObs> obs = make_synthetic_slice_obs_wideband(F, T, df, 0.20);

  // Call-folded per-obs band [c_bid, c_ask], matching the production composition
  // (half-spread invariant under put-call parity): c_ask = co + s/2,
  // c_bid = max(0, co − s/2), co the call-folded price of the obs.
  const auto band = [&](const FitObs& o) {
    const double co = (o.side == Side::Call) ? o.mid : o.mid + df * (F - o.K);
    return std::pair<double, double>{std::max(0.0, co - o.spread / 2),
                                     co + o.spread / 2};
  };

  // (1) LIVENESS GUARD — a default Mid fit MUST push at least one price OUTSIDE
  // its band. If interval loss ever silently degrades to Mid, this same board
  // would violate a band under the Interval branch too, failing part (2). So the
  // pairing (Mid violates ≥1 band ∧ Interval in-band everywhere) gives the test
  // teeth: it cannot pass unless the interval branch genuinely differs from Mid.
  auto mid_fit = fit_convex_slice(obs, F, T, df, ConvexFitOpts{});
  ASSERT_TRUE(mid_fit.has_value()) << mid_fit.error().to_string();
  int mid_violations = 0;
  double worst_out = 0.0;
  for (const auto& o : obs) {
    const auto [lo, hi] = band(o);
    const double c = mid_fit->call_price(o.K);
    if (c < lo - 1e-6 || c > hi + 1e-6) {
      ++mid_violations;
      worst_out = std::max(worst_out, std::max(lo - c, c - hi));
    }
  }
  ASSERT_GT(mid_violations, 0)
      << "board is not discriminating: the default Mid fit stays in every band, so "
         "the interval assertion below would pass even if interval loss regressed to "
         "Mid (worst overshoot " << worst_out << ")";

  // (2) The Interval fit must put EVERY fitted call price INSIDE its band.
  ConvexFitOpts opts;
  opts.loss = CalibLossKind::Interval;
  auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  for (const auto& o : obs) {
    const auto [lo, hi] = band(o);
    const double c = fit->call_price(o.K);
    EXPECT_GE(c, lo - 1e-6) << "interval fit below band at K=" << o.K;
    EXPECT_LE(c, hi + 1e-6) << "interval fit above band at K=" << o.K;
  }
}

TEST(ConvexSliceFit, IntervalDegenerateBandEqualsMid) {
  using namespace atx::vol;
  const double F = 100.0, T = 0.5, df = 0.98;
  auto obs = make_synthetic_slice_obs(F, T, df, 0.20);
  for (auto& o : obs) o.spread = 0.0;             // zero-width band == mid target
  ConvexFitOpts mid; auto a = fit_convex_slice(obs, F, T, df, mid);
  ConvexFitOpts iv; iv.loss = CalibLossKind::Interval;
  auto b = fit_convex_slice(obs, F, T, df, iv);
  ASSERT_TRUE(a && b);
  for (std::size_t j = 0; j < a->C.size(); ++j) EXPECT_NEAR(a->C[j], b->C[j], 1e-7);
}

TEST(ConvexSliceFit, NoiseAwareRegularizationUsesRobustErrorScale) {
  using namespace atx::vol;
  constexpr double F = 100.0, T = 0.25, df = 0.99;
  auto clean = make_synthetic_slice_obs(F, T, df, 0.22);
  auto noisy = clean;
  for (FitObs& o : clean) o.noise_sigma = 0.0025;
  for (FitObs& o : noisy) o.noise_sigma = 0.04;

  ConvexFitContext context;
  context.noise_aware_regularization = true;
  const auto clean_fit = fit_convex_slice(clean, F, T, df, {}, {}, context);
  const auto noisy_fit = fit_convex_slice(noisy, F, T, df, {}, {}, context);
  ASSERT_TRUE(clean_fit && noisy_fit);
  expect_arb_free(*clean_fit);
  expect_arb_free(*noisy_fit);
  EXPECT_NEAR(clean_fit->noise_scale, 0.25, 1.0e-12);
  EXPECT_NEAR(noisy_fit->noise_scale, 4.0, 1.0e-12);
  EXPECT_LT(clean_fit->effective_lambda, noisy_fit->effective_lambda);
}

TEST(ConvexSliceFit, RequiredCalendarKnotsAreExactConstrainedNodes) {
  using namespace atx::vol;
  constexpr double F = 100.0, T = 0.50, df = 0.98;
  auto obs = make_synthetic_slice_obs(F, T, df, 0.16);
  const std::vector<double> required = {-0.17, 0.11};
  ConvexFitContext context;
  context.required_k = std::span<const double>{required};
  auto w_prev = [](double k) { return 0.035 + 0.004 * k * k; };
  const auto fit = fit_convex_slice(obs, F, T, df, {}, w_prev, context);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_arb_free(*fit);
  for (const double k : required) {
    const double K = F * std::exp(k);
    const auto it = std::lower_bound(fit->u.begin(), fit->u.end(), K);
    ASSERT_NE(it, fit->u.end());
    EXPECT_NEAR(*it, K, 1.0e-10);
    const double sigma = fit->iv(k);
    ASSERT_TRUE(std::isfinite(sigma));
    EXPECT_GE(sigma * sigma * T, w_prev(k) - 1.0e-7);
  }
}

TEST(ConvexSliceFit, PowerWingsRemainBoundedMonotoneAndConvex) {
  using namespace atx::vol;
  constexpr double F = 100.0, T = 0.5, df = 0.98;
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) obs.push_back(mk_obs(F, T, df, K, 0.24));
  const auto fit = fit_convex_slice(obs, F, T, df);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();

  const std::vector<double> strikes = {10, 20, 40, 60, 70, 80, 100,
                                       120, 130, 150, 200, 300, 500};
  std::vector<double> calls;
  for (const double K : strikes) {
    const double c = fit->call_price(K);
    ASSERT_TRUE(std::isfinite(c));
    EXPECT_GE(c, df * std::max(F - K, 0.0) - 1.0e-8);
    EXPECT_LE(c, df * F + 1.0e-8);
    calls.push_back(c);
  }
  for (std::size_t i = 0; i + 1 < calls.size(); ++i) {
    EXPECT_LE(calls[i + 1], calls[i] + 1.0e-9);
  }
  for (std::size_t i = 1; i + 1 < calls.size(); ++i) {
    const double left = (calls[i] - calls[i - 1]) /
                        (strikes[i] - strikes[i - 1]);
    const double right = (calls[i + 1] - calls[i]) /
                         (strikes[i + 1] - strikes[i]);
    EXPECT_GE(right, left - 1.0e-8) << "wing convexity at K=" << strikes[i];
  }
  EXPECT_TRUE(std::isfinite(fit->iv(std::log(50.0 / F))));
  EXPECT_TRUE(std::isfinite(fit->iv(std::log(200.0 / F))));
}

TEST(ConvexSliceFit, SharedKCalendarRefitPreservesSliceConvexity) {
  using namespace atx::vol;
  constexpr double F = 100.0, df = 0.99;
  const double T0 = 0.25, T1 = 0.50;
  std::vector<FitObs> front;
  std::vector<FitObs> back;
  for (const double K : strike_grid(F)) {
    front.push_back(mk_obs(F, T0, df, K, 0.36));
    // Lower back total variance creates a crossing at every shared-k point.
    back.push_back(mk_obs(F, T1, df, K, 0.20));
  }
  CurveConfig cfg;
  cfg.kind = VolCurveKind::ConvexDense;
  cfg.convex.node_cap = 12; // force differing coarse nodes and off-node checks

  auto front_curve = fit_slice_curve(cfg, front, F, T0, df);
  ASSERT_TRUE(front_curve.has_value()) << front_curve.error().to_string();
  const IVolCurve* prev = front_curve->get();
  const std::function<double(double)> floor = [prev](double k) { return prev->w(k); };
  auto back_curve = fit_slice_curve(cfg, back, F, T1, df, floor);
  ASSERT_TRUE(back_curve.has_value()) << back_curve.error().to_string();

  const auto* back_dense = dynamic_cast<const ConvexDenseCurve*>(back_curve->get());
  ASSERT_NE(back_dense, nullptr);
  expect_arb_free(back_dense->fit());

  CurveSurface surface;
  surface.push(std::move(*front_curve));
  surface.push(std::move(*back_curve));
  const auto violations = arb_check_calendar(surface, -0.25, 0.25, 64);
  ASSERT_TRUE(violations.has_value()) << violations.error().to_string();
  EXPECT_TRUE(violations->empty());
}

// Oracle I-4: the active-set QP assumed a strictly feasible start it never
// verified. A short-dated slice with a deep-ITM `required_k` calendar knot
// (0DTE-style: T so small that Black's sigma=2 seed's time value underflows
// in double precision to EXACTLY the discounted intrinsic value) used to put
// x0 below the `g_j >= intrinsic + price_epsilon` row, freezing that row
// violated once a negative-alpha ratio-test step forced it into the working
// set, and the iteration-cap exit returned the point as Ok with no check at
// all. The fix must either converge to a genuinely feasible fit or return
// the documented Internal error — never Ok with a violated node.
TEST(ConvexSliceFit, DeepItmZeroDteRequiredKNeverReturnsOkWithViolation) {
  using namespace atx::vol;
  constexpr double F = 100.0, T = 1.0e-6, df = 1.0;  // ~0DTE: seconds to expiry
  std::vector<FitObs> obs;
  for (const double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
    obs.push_back(mk_obs(F, T, df, K, 0.5));
  }
  // Deep-ITM calendar knots (k=-0.5 => K~60.65): at T=1e-6, Black's d1/d2 for
  // these strikes saturate the normal CDF to exactly 1.0 in double precision,
  // so the OLD x0 = black76_price(F,K,T,2.0,df,Call) landed exactly on
  // intrinsic — precisely the I-4 scenario.
  const std::vector<double> required = {-0.5, -0.3, -0.1};
  ConvexFitContext context;
  context.required_k = std::span<const double>{required};

  const auto fit = fit_convex_slice(obs, F, T, df, {}, {}, context);
  if (fit.has_value()) {
    for (std::size_t i = 0; i < fit->u.size(); ++i) {
      const double intrinsic = fit->df * std::max(fit->F - fit->u[i], 0.0);
      EXPECT_GE(fit->C[i], intrinsic - 1.0e-7)
          << "node " << i << " (K=" << fit->u[i] << ") sub-intrinsic despite Ok status";
      EXPECT_LE(fit->C[i], fit->df * fit->F + 1.0e-7)
          << "node " << i << " (K=" << fit->u[i] << ") above the forward despite Ok status";
    }
  } else {
    EXPECT_EQ(fit.error().code(), ErrorCode::Internal)
        << "fail-closed rejection must be the documented Internal error, not a "
           "mis-tagged input-validation error: "
        << fit.error().to_string();
  }
}

// Same deep-ITM 0DTE board, but with the iteration cap forced to 1 — far too
// few iterations to work the deep-ITM rows into the active set through the
// normal ratio-test path even if the start were feasible. Must still never
// silently certify a violated point as Ok.
TEST(ConvexSliceFit, InfeasibleStartWithTinyIterationCapNeverReturnsOkWithViolation) {
  using namespace atx::vol;
  constexpr double F = 100.0, T = 1.0e-6, df = 1.0;
  std::vector<FitObs> obs;
  for (const double K : {90.0, 95.0, 100.0, 105.0, 110.0}) {
    obs.push_back(mk_obs(F, T, df, K, 0.5));
  }
  const std::vector<double> required = {-0.5, -0.3, -0.1};
  ConvexFitContext context;
  context.required_k = std::span<const double>{required};
  ConvexFitOpts opts;
  opts.max_iter = 1;

  const auto fit = fit_convex_slice(obs, F, T, df, opts, {}, context);
  if (fit.has_value()) {
    for (std::size_t i = 0; i < fit->u.size(); ++i) {
      const double intrinsic = fit->df * std::max(fit->F - fit->u[i], 0.0);
      EXPECT_GE(fit->C[i], intrinsic - 1.0e-7);
      EXPECT_LE(fit->C[i], fit->df * fit->F + 1.0e-7);
    }
  } else {
    EXPECT_EQ(fit.error().code(), ErrorCode::Internal);
  }
}

// Regression guard on the fix's OTHER direction: an ordinary, comfortably
// feasible-start board must NOT be rejected merely because the iteration cap
// is tiny. Active-set feasibility is maintained at every iterate once x0 is
// feasible, so a capped-but-feasible board is a legitimate (if suboptimal)
// Ok, not an error.
TEST(ConvexSliceFit, FeasibleStartWithTinyIterationCapStillSucceeds) {
  using namespace atx::vol;
  constexpr double F = 100.0, T = 0.25, df = 0.98;
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, 0.22));
  }
  ConvexFitOpts opts;
  opts.max_iter = 1;
  const auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  for (std::size_t i = 0; i < fit->u.size(); ++i) {
    const double intrinsic = fit->df * std::max(fit->F - fit->u[i], 0.0);
    EXPECT_GE(fit->C[i], intrinsic - 1.0e-7);
    EXPECT_LE(fit->C[i], fit->df * fit->F + 1.0e-7);
  }
}
