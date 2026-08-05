#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "atx/vol/arb.hpp"          // arb_check_calendar
#include "atx/vol/black76.hpp"      // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"        // FitObs
#include "atx/vol/dense_slice.hpp"  // fit_convex_slice, ConvexSliceFit, kMaxIntervalSlackRows
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
using atx::vol::ErrorCode;
using atx::vol::fit_convex_slice;
using atx::vol::FitObs;
using atx::vol::Side;

// One OTM observation at strike K under a given vol, with a Black-76 vega weight.
FitObs mk_obs(double F, double T, double df, double K, double vol, double spread = 0.02) {
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
void expect_arb_free(const atx::vol::ConvexSliceFit &fit) {
  const auto &u = fit.u;
  const auto &C = fit.C;
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

void expect_converged_qp_diagnostics(const atx::vol::ConvexSliceFit &fit) {
  EXPECT_GT(fit.qp_iterations, std::size_t{0});
  EXPECT_TRUE(std::isfinite(fit.qp_stationarity));
  EXPECT_TRUE(std::isfinite(fit.qp_primal_violation));
  EXPECT_TRUE(std::isfinite(fit.qp_complementarity));
  EXPECT_TRUE(std::isfinite(fit.qp_dual_violation));
  EXPECT_GE(fit.qp_stationarity, 0.0);
  EXPECT_GE(fit.qp_primal_violation, 0.0);
  EXPECT_GE(fit.qp_complementarity, 0.0);
  EXPECT_GE(fit.qp_dual_violation, 0.0);
  EXPECT_LE(fit.qp_stationarity, 1.0e-8);
  EXPECT_LE(fit.qp_primal_violation, 1.0e-8);
  EXPECT_LE(fit.qp_complementarity, 1.0e-8);
  EXPECT_LE(fit.qp_dual_violation, 1.0e-8);
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
std::vector<FitObs> make_synthetic_slice_obs(double F, double T, double df, double sigma) {
  const std::vector<double> strikes = {40, 55, 65, 72, 78, 84, 90, 96, 104, 115, 130};
  std::vector<FitObs> obs;
  obs.reserve(strikes.size());
  for (const double K : strikes) {
    obs.push_back(mk_obs(F, T, df, K, sigma));
  }
  for (FitObs &o : obs) {
    if (std::fabs(o.K - 65.0) < 1.0e-9) {
      o.mid *= 0.5;      // artificially cheap deep-ITM print
      o.spread = 1.0e-4; // tight spread => near-interpolated by the fit
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
std::vector<FitObs> make_synthetic_slice_obs_wideband(double F, double T, double df, double sigma) {
  const std::vector<double> strikes = {100, 105, 110, 115, 120, 125, 130, 135, 140, 145, 150};
  const double kmax = 170.0;
  const auto g_true = [&](double K) { return 0.004 * (kmax - K) * (kmax - K); };
  std::vector<FitObs> obs;
  obs.reserve(strikes.size());
  for (const double K : strikes) {
    FitObs o = mk_obs(F, T, df, K, sigma); // realistic side / vega weight
    double bump = 0.0;                     // localized concave hump…
    if (K == 120.0)
      bump = 1.5;
    if (K == 125.0)
      bump = 3.0; // …peak of the concavity
    if (K == 130.0)
      bump = 1.5;
    o.mid = g_true(K) + bump;            // convex base + concave bump
    o.spread = (K == 125.0) ? 2.0 : 6.0; // moderate peak band, wide else
    obs.push_back(o);
  }
  return obs;
}

} // namespace

TEST(DenseSlice, FlatVolIsRecoveredAndArbFree) {
  constexpr double F = 100.0, T = 0.25, r = 0.03, vol = 0.22;
  const double df = std::exp(-r * T);
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, vol));
  }

  ConvexFitOpts opts;
  opts.lambda = 1.0e-4; // near-interpolation
  const auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_arb_free(*fit);

  // Near-interpolation: a flat-vol board's call prices are exactly convex, so the
  // fit recovers the generating vol at every interior strike to a few bp.
  int checked = 0;
  for (const double K : strike_grid(F)) {
    if (K < 0.80 * F || K > 1.20 * F)
      continue; // interior, high-vega
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
  expect_arb_free(*fit); // the whole point: arb-free despite arb-violating input
}

TEST(DenseSlice, WideBoardUsesClusteredNodesAndStaysArbFree) {
  // A wide, dense board (>node_cap strikes) exercises the ATM-clustered node grid
  // + design matrix. The fit must stay arbitrage-free and still track a mild smile.
  constexpr double F = 600.0, T = 0.3, r = 0.03;
  const double df = std::exp(-r * T);
  auto vol_of = [](double k) { return 0.20 - 0.08 * k + 0.25 * k * k; };
  std::vector<FitObs> obs;
  for (double K = 0.55 * F; K <= 1.45 * F + 1e-9; K += 0.01 * F) { // ~90 strikes
    obs.push_back(mk_obs(F, T, df, K, vol_of(std::log(K / F))));
  }
  ConvexFitOpts opts;
  opts.node_cap = 40;
  const auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  EXPECT_LE(fit->u.size(), std::size_t{40}); // capped node count
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
  std::vector<FitObs> obs = {mk_obs(F, T, df, 95.0, 0.2), mk_obs(F, T, df, 105.0, 0.2)};
  EXPECT_FALSE(fit_convex_slice(obs, F, T, df, {}).has_value());
}

TEST(DenseSlice, RejectsNonFiniteInputsOptionsAndObservations) {
  constexpr double F = 100.0, T = 0.25, vol = 0.22;
  const double df = std::exp(-0.03 * T);
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, vol));
  }

  const auto expect_invalid = [](const auto &result) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
  };
  expect_invalid(fit_convex_slice(obs, inf, T, df, {}));
  expect_invalid(fit_convex_slice(obs, F, nan, df, {}));
  expect_invalid(fit_convex_slice(obs, F, T, inf, {}));

  ConvexFitOpts bad_opts;
  bad_opts.lambda = nan;
  expect_invalid(fit_convex_slice(obs, F, T, df, bad_opts));

  std::vector<FitObs> bad_obs = obs;
  bad_obs[3].vega = inf;
  expect_invalid(fit_convex_slice(bad_obs, F, T, df, {}));
  bad_obs = obs;
  bad_obs[3].K = inf;
  expect_invalid(fit_convex_slice(bad_obs, F, T, df, {}));
  bad_obs = obs;
  bad_obs[3].mid = nan;
  expect_invalid(fit_convex_slice(bad_obs, F, T, df, {}));
}

TEST(DenseSlice, ZeroIterationBudgetReportsNonConvergence) {
  constexpr double F = 100.0, T = 0.25, vol = 0.22;
  const double df = std::exp(-0.03 * T);
  std::vector<FitObs> obs;
  for (const double K : strike_grid(F)) {
    obs.push_back(mk_obs(F, T, df, K, vol));
  }

  ConvexFitOpts opts;
  opts.max_iter = 0;
  const auto fit = fit_convex_slice(obs, F, T, df, opts);

  ASSERT_FALSE(fit.has_value());
  EXPECT_EQ(fit.error().code(), ErrorCode::Internal);
}

TEST(DenseSlice, BindingConstraintsAreReportedInDiagnostics) {
  constexpr double F = 100.0, T = 0.25, vol = 0.22;
  const double df = std::exp(-0.03 * T);
  std::vector<FitObs> obs;
  int index = 0;
  for (const double K : strike_grid(F)) {
    FitObs o = mk_obs(F, T, df, K, vol);
    o.mid *= (index % 2 == 0) ? 1.06 : 0.94;
    obs.push_back(o);
    ++index;
  }

  const auto fit = fit_convex_slice(obs, F, T, df, {});

  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_converged_qp_diagnostics(*fit);
  EXPECT_GT(fit->n_active, std::size_t{0});
}

TEST(DenseSlice, ExtremeUnrelatedCoordinateCannotMaskNegativeNode) {
  constexpr double F = 1.0e9, T = 0.25, df = 0.99, vol = 0.22;
  std::vector<FitObs> obs;
  for (const double K : {0.50 * F, 0.75 * F, F, 1.50 * F, 3.00 * F}) {
    obs.push_back(mk_obs(F, T, df, K, vol, 1.0e-6));
  }
  // Apply a small O(1) calendar floor only to the far wing while unrelated ITM
  // nodes remain O(1e8). A global ||x|| scale can incorrectly certify a material
  // violation of this sparse local row.
  const auto far_wing_floor = [](double k) {
    return k > 1.0 ? 0.04 : std::numeric_limits<double>::quiet_NaN();
  };
  const double required_floor = black76_price(F, 3.0 * F, T, std::sqrt(0.04 / T), df, Side::Call);
  ASSERT_GT(required_floor, 0.1);

  ConvexFitOpts opts;
  opts.lambda = 0.0;
  const auto fit = fit_convex_slice(obs, F, T, df, opts, far_wing_floor);

  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_converged_qp_diagnostics(*fit);
  ASSERT_FALSE(fit->C.empty());
  EXPECT_GE(fit->C.back(), required_floor - 1.0e-8);
}

TEST(DenseSlice, PositiveIterationCapExhaustionIsNotPublishedForEitherLoss) {
  constexpr double F = 100.0, T = 0.25, vol = 0.22;
  const double df = std::exp(-0.03 * T);
  std::vector<FitObs> obs;
  int index = 0;
  for (const double K : strike_grid(F)) {
    FitObs o = mk_obs(F, T, df, K, vol);
    o.mid *= (index % 2 == 0) ? 1.06 : 0.94;
    obs.push_back(o);
    ++index;
  }

  ConvexFitOpts mid_opts;
  mid_opts.max_iter = 1;
  const auto mid = fit_convex_slice(obs, F, T, df, mid_opts);
  ASSERT_FALSE(mid.has_value());
  EXPECT_EQ(mid.error().code(), ErrorCode::Internal);

  ConvexFitOpts interval_opts;
  interval_opts.loss = atx::vol::CalibLossKind::Interval;
  interval_opts.max_iter = 1;
  const auto interval = fit_convex_slice(obs, F, T, df, interval_opts);
  ASSERT_FALSE(interval.has_value());
  EXPECT_EQ(interval.error().code(), ErrorCode::Internal);
}

// WP10: the Interval loss materializes dense (N+2M)x(N+2M) system matrices,
// O(M^2) in the distinct-strike count M. A pathologically wide board must fail
// LOUD and BOUNDED (InvalidArgument) before that allocation, not die in an
// unbounded alloc. node_cap bounds N at 40, so ~1100 distinct strikes drive
// N + 2M = 40 + 2200 = 2240 past kMaxIntervalSlackRows (2048). The obs carry
// only trivial finite values -- the guard fires before any heavy solve work.
TEST(DenseSlice, IntervalLossRejectsOversizedSlackSystemBeforeAllocation) {
  constexpr double F = 100.0, T = 0.25, df = 0.98;
  constexpr int kStrikes = 1100; // 2*1100 + 40 = 2240 > kMaxIntervalSlackRows
  static_assert(2 * kStrikes + 40 > atx::vol::kMaxIntervalSlackRows,
                "test must exceed the interval slack cap");
  std::vector<FitObs> obs;
  obs.reserve(kStrikes);
  for (int i = 0; i < kStrikes; ++i) {
    FitObs o{};
    o.K = 10.0 + 0.1 * static_cast<double>(i); // distinct, positive, ascending
    o.F = F;
    o.df = df;
    o.k = std::log(o.K / F);
    o.side = Side::Call;
    o.mid = 1.0; // trivial finite non-negative call price
    o.spread = 0.02;
    o.vega = 1.0;
    o.sigma_mkt = 0.2;
    obs.push_back(o);
  }
  ConvexFitOpts opts;
  opts.loss = atx::vol::CalibLossKind::Interval;
  const auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_FALSE(fit.has_value());
  EXPECT_EQ(fit.error().code(), ErrorCode::InvalidArgument);
  EXPECT_NE(fit.error().message().find("too large"), std::string::npos)
      << "guard message: " << fit.error().message();
}

TEST(DenseSlice, HostileSlopeBoundStartIsRepairedBeforeCertifiedFit) {
  constexpr double F = 100.0, T = 0.25, df = 0.98;
  std::vector<FitObs> obs;
  for (const double K : {100.0, 101.0, 102.0, 103.0, 104.0}) {
    obs.push_back(mk_obs(F, T, df, K, 0.22));
  }
  obs.front().mid = 20.0;

  ConvexFitOpts opts;
  opts.bound_slope_below = true;
  const auto fit = fit_convex_slice(obs, F, T, df, opts);

  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_converged_qp_diagnostics(*fit);
  for (std::size_t i = 0; i + 1 < fit->u.size(); ++i) {
    const double slope = (fit->C[i + 1] - fit->C[i]) / (fit->u[i + 1] - fit->u[i]);
    EXPECT_GE(slope, -df - 1.0e-8);
  }
}

TEST(ConvexSliceFit, SlopeBelowBoundHonored) {
  using namespace atx::vol;
  // Build a simple in-the-money-heavy obs set where the unconstrained slope could
  // dip below -df; enable the bound and assert it holds at every node pair.
  std::vector<FitObs> obs = make_synthetic_slice_obs(/*F=*/100.0, /*T=*/0.5,
                                                     /*df=*/0.98, /*sigma=*/0.2);
  ConvexFitOpts opts;
  opts.bound_slope_below = true;
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
    return sig * sig * T; // flat prev total variance
  };
  auto free_fit = fit_convex_slice(obs, F, T, df, {});
  auto floored = fit_convex_slice(obs, F, T, df, {}, w_prev);
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
  auto w_prev = [&](double) { return 0.10 * 0.10 * T; }; // prev far BELOW → slack
  auto free_fit = fit_convex_slice(obs, F, T, df, {});
  auto floored = fit_convex_slice(obs, F, T, df, {}, w_prev);
  ASSERT_TRUE(free_fit && floored);
  ASSERT_EQ(free_fit->C.size(), floored->C.size());
  for (std::size_t j = 0; j < free_fit->C.size(); ++j) {
    EXPECT_NEAR(free_fit->C[j], floored->C[j], 1e-12); // slack ⇒ identical
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
  const auto band = [&](const FitObs &o) {
    const double co = (o.side == Side::Call) ? o.mid : o.mid + df * (F - o.K);
    return std::pair<double, double>{std::max(0.0, co - o.spread / 2), co + o.spread / 2};
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
  for (const auto &o : obs) {
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
         "Mid (worst overshoot "
      << worst_out << ")";

  // (2) The Interval fit must put EVERY fitted call price INSIDE its band.
  ConvexFitOpts opts;
  opts.loss = CalibLossKind::Interval;
  auto fit = fit_convex_slice(obs, F, T, df, opts);
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_converged_qp_diagnostics(*fit);
  for (const auto &o : obs) {
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
  for (auto &o : obs)
    o.spread = 0.0; // zero-width band == mid target
  ConvexFitOpts mid;
  auto a = fit_convex_slice(obs, F, T, df, mid);
  ConvexFitOpts iv;
  iv.loss = CalibLossKind::Interval;
  auto b = fit_convex_slice(obs, F, T, df, iv);
  ASSERT_TRUE(a && b);
  for (std::size_t j = 0; j < a->C.size(); ++j)
    EXPECT_NEAR(a->C[j], b->C[j], 1e-7);
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
  // MERGE (rule 6): main's exact-QP contract (50956be) only PUBLISHES a
  // KKT-certified solve; a one-iteration cap on this board does not reach the
  // certificate, so the documented Internal is the correct merged outcome
  // (main's sibling PositiveIterationCapExhaustionIsNotPublishedForEitherLoss
  // pins exactly that). If a future faster convergence does certify in one
  // step, the returned nodes must still be inside the no-arb price cone. So
  // accept EITHER a certified feasible fit OR the documented Internal --
  // matching the sibling InfeasibleStartWithTinyIterationCap guard.
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

// ── Regression: the degenerate board the ratio test used to diverge on ──────

// Every other fit in this file is small and well-conditioned enough that the
// active-set walk never meets a family of blocking constraints that tie to the
// last few ulp. The board below does. It is captured VERBATIM from production
// rather than hand-built because that tie structure is what the pathology needs
// and it is not something a hand-written smile reproduces: a 3000-board
// randomized sweep over realistic synthetic slices produced only iteration-cap
// exhaustion, never the divergence.
//
// Provenance: SPY 2025-04-09, `--preset populate`, the ConvexDense fit of the
// 12th expiry (T = 0.0575 yr, 60 fit rows, 40 QP nodes, 237 constraint rows,
// calendar floor active, no required_k). It is the FIRST slice of that board on
// which the pre- and post-fix solvers disagree, so every input here — including
// the calendar floor inherited from the last committed slice — is identical
// under both. Before the ratio-test clamp this solve stepped backwards out of
// its own feasible region and exited the iteration cap with a SATURATED scaled
// primal violation (1.0), so `fit_convex_slice` returned
// `Internal: QP failed KKT certification` and `fit_curve_surface` dropped the
// expiry. That single defect cost this board 20 of its 34 expiries, the
// long-dated tail included.
namespace {
namespace qp_divergence_board {
constexpr double kQpBoardNodeCap = 40;
constexpr double kQpBoardF = 545.70935693518186;
constexpr double kQpBoardT = 0.057504372956118335;
constexpr double kQpBoardDf = 0.99751315797872819;
constexpr double kQpBoardEffLambda = 0.00097538814207589043;

// (This slice was fit with no required_k knots, so ConvexFitContext carries none.)

// K, mid, spread, vega, side(0=call,1=put) per fit observation.
struct QpBoardRow {
  double K, mid, spread, vega;
  int side;
};
constexpr QpBoardRow kQpBoardObs[] = {
    {300, 0.15490549320512684, 0.069999999999999993, 1.441349412941918, 1},
    {440, 1.6573977542406566, 0.16000000000000014, 13.553691037953243, 1},
    {450, 1.8118452557575755, 0.18999999999999995, 15.11902621641363, 1},
    {465, 2.2903338639230473, 0.28999999999999959, 18.753282355278166, 1},
    {480, 2.987759842214309, 0.20999999999999996, 23.506131580118783, 1},
    {500, 4.5107862018667708, 0.23000000000000043, 31.990102368764543, 1},
    {510, 5.6792459970090823, 0.25999999999999979, 37.10050144129135, 1},
    {525, 8.2762687345718025, 0.3100000000000005, 45.125129374257789, 1},
    {526, 8.3847470361933052, 0.58999999999999986, 45.552790009643871, 1},
    {527, 8.7027368822228723, 0.36999999999999922, 46.115397217545336, 1},
    {528, 8.8010595521160138, 0.53000000000000114, 46.52304539704231, 1},
    {529, 9.1787803259391101, 0.36999999999999922, 47.081083852598844, 1},
    {534, 10.347106446449846, 0.65000000000000036, 49.20510943992322, 1},
    {535, 10.729214776140822, 0.44000000000000128, 49.615812165891867, 1},
    {536, 10.916395700531348, 0.66000000000000014, 49.954723705498381, 1},
    {540, 12.25291437481947, 0.41999999999999993, 51.176333209121651, 1},
    {541, 12.454064257155212, 0.6899999999999995, 51.401055277621197, 1},
    {545, 14.006379748238254, 0.44999999999999929, 52.003969497679265, 1},
    {546, 14.020000000000007, 0.58000000000000007, 52.060792532428415, 0},
    {549, 12.240000000000014, 0.58000000000000007, 51.97305071564228, 0},
    {550, 11.505000000000015, 0.16999999999999993, 51.841604472655582, 0},
    {551, 11.099999999999985, 0.5600000000000005, 51.677847274796946, 0},
    {555, 8.8949999999999889, 0.33000000000000007, 50.41672296991684, 0},
    {556, 8.4800000000000022, 0.5, 49.988504411462237, 0},
    {560, 6.5950000000000726, 0.19000000000000039, 47.506186430462478, 0},
    {565, 4.6700000000000825, 0.28000000000000025, 43.016679362629375, 0},
    {569, 3.4950000000000006, 0.43000000000000016, 38.615304185635729, 0},
    {570, 3.1499999999999932, 0.1599999999999997, 37.127571379878901, 0},
    {574, 2.3299999999999912, 0.41999999999999993, 32.286504094479817, 0},
    {575, 2.0649999999999653, 0.13000000000000034, 30.577533168402482, 0},
    {578, 1.6149999999999944, 0.21000000000000019, 26.84564145863725, 0},
    {579, 1.4000000000000317, 0.15999999999999992, 24.991323081097292, 0},
    {585, 0.85500000000001375, 0.15000000000000002, 18.503065700149282, 0},
    {587, 0.66999999999997673, 0.21999999999999997, 15.94691619788043, 0},
    {588, 0.63999999999998358, 0.14000000000000001, 15.382063575855106, 0},
    {590, 0.52999999999998415, 0.12000000000000005, 13.564616889875103, 0},
    {591, 0.44999999999998486, 0.099999999999999978, 12.232303916920699, 0},
    {592, 0.45000000000001678, 0.12, 12.091468532900937, 0},
    {595, 0.3450000000000002, 0.089999999999999969, 10.010963918806995, 0},
    {596, 0.36000000000000559, 0.03999999999999998, 10.154039015145154, 0},
    {598, 0.28499999999999454, 0.089999999999999997, 8.6295603666796659, 0},
    {600, 0.26000000000000384, 0.020000000000000018, 7.9812844256735618, 0},
    {602, 0.21999999999999004, 0.059999999999999998, 7.0373357901609319, 0},
    {604, 0.20499999999999349, 0.070000000000000007, 6.5906236334903019, 0},
    {605, 0.17999999999998686, 0.040000000000000008, 5.9993380196768502, 0},
    {606, 0.19000000000000228, 0.059999999999999998, 6.151397316795725, 0},
    {612, 0.14000000000000759, 0.039999999999999994, 4.7500453956984554, 0},
    {617, 0.12499999999999688, 0.050000000000000017, 4.2074348559897432, 0},
    {619, 0.11499999999999476, 0.050000000000000003, 3.9071189225831624, 0},
    {620, 0.095000000000000251, 0.009999999999999995, 3.3960120446845816, 0},
    {621, 0.10500000000000415, 0.050000000000000003, 3.6089596582915351, 0},
    {624, 0.10499999999999794, 0.050000000000000003, 3.5214030133380745, 0},
    {625, 0.080000000000005483, 0.020000000000000004, 2.8890123384965296, 0},
    {626, 0.095000000000002027, 0.050000000000000003, 3.2330501734304642, 0},
    {628, 0.084999999999998133, 0.029999999999999999, 2.9443100807170004, 0},
    {632, 0.090000000000000524, 0.039999999999999994, 2.973993412015234, 0},
    {638, 0.080000000000000168, 0.040000000000000001, 2.6217847807168404, 0},
    {650, 0.044999999999998486, 0.010000000000000002, 1.5960884607107408, 0},
    {660, 0.04999999999999645, 0.020000000000000004, 1.6249943867951848, 0},
    {665, 0.025000000000002211, 0.010000000000000002, 0.93931361929952617, 0},
};

// Calendar floor: w_prev(k) at each QP node.
constexpr double kQpBoardFloorK[] = {
    -0.59830404605415022, -0.54708529139362949, -0.49776892786342275, -0.45038695759774544,
    -0.40497385609121206, -0.36156693210583574, -0.32020676787690538, -0.28093776468453685,
    -0.24380882927187908, -0.20887425259462353, -0.17619485778689808, -0.14583953609275743,
    -0.11788736166389951, -0.092430607354431329, -0.069579238699492937, -0.049468002479767771,
    -0.032268499862821501, -0.018212145663844492, -0.0076421972138920585, -0.0011806253604015704,
    0.00039011978800999718, 0.0025252484462987518, 0.0060179279929847259, 0.010662637571673425,
    0.016345952990647887, 0.022991406725431041, 0.030542295766450078, 0.038954095078686898,
    0.048190468214783747, 0.058220925006148151, 0.069019336595708067, 0.080562938914159318,
    0.092831633877056244, 0.10580748185948863, 0.11947432236811996, 0.13381748367170773,
    0.14882355598459154, 0.16448021119082906, 0.180776057384824, 0.19770051994550283,
};
constexpr double kQpBoardFloorW[] = {
    0.093040053375774992, 0.08074280576220641, 0.06954076394164542, 0.059391017892435942,
    0.0502493689719644, 0.042070323232544261, 0.034807087831948295, 0.0284115708325339,
    0.022834384653592416, 0.01802485341307403, 0.013931024335320489, 0.011429848870867263,
    0.009328051650103468, 0.0075407544958615872, 0.0062268697888511471, 0.0052092268353457465,
    0.009332379229821346, 0.013368598733531018, 0.016433818278040486, 0.018324726511943137,
    0.018786436156547195, 0.01941533449602539, 0.020447302477738547, 0.021825776634243774,
    0.02352183913225548, 0.025517698477080335, 0.027801351339019915, 0.030364088355284622,
    0.033199084564759118, 0.036300504776212376, 0.039662890424246075, 0.043571519204782409,
    0.04814040836339796, 0.053083921528086969, 0.058410416690567443, 0.064128331150019008,
    0.070246105074809329, 0.076772125460278881, 0.083714684983781457, 0.091081951847231654,
};

} // namespace qp_divergence_board
} // namespace

TEST(ConvexSliceFit, DegenerateBoardIsCertifiedNotDiverged) {
  using namespace atx::vol;
  using namespace qp_divergence_board;

  std::vector<FitObs> obs;
  obs.reserve(std::size(kQpBoardObs));
  for (const QpBoardRow &row : kQpBoardObs) {
    FitObs o{};
    o.K = row.K;
    o.F = kQpBoardF;
    o.df = kQpBoardDf;
    o.k = std::log(row.K / kQpBoardF);
    o.side = (row.side == 0) ? Side::Call : Side::Put;
    o.mid = row.mid;
    o.spread = row.spread;
    o.vega = row.vega;
    obs.push_back(o);
  }

  // The calendar floor exactly as fit_curve_surface supplied it. w_prev(k) is
  // only ever evaluated at the QP's own node grid, and that grid is a pure
  // function of (obs, node_cap, required_k) — all reproduced above — so this
  // table rebuilds the floor rows bit-for-bit. A lookup miss would silently
  // drop a floor row and quietly change the problem, so misses are asserted on.
  std::size_t floor_hits = 0, floor_misses = 0;
  const auto w_prev = [&](double k) {
    for (std::size_t j = 0; j < std::size(kQpBoardFloorK); ++j) {
      if (std::fabs(kQpBoardFloorK[j] - k) <= 1.0e-12) {
        ++floor_hits;
        return kQpBoardFloorW[j];
      }
    }
    ++floor_misses;
    return std::numeric_limits<double>::quiet_NaN();
  };

  ConvexFitOpts opts;
  opts.lambda = kQpBoardEffLambda; // production's post-noise-scaling roughness
  opts.node_cap = static_cast<int>(kQpBoardNodeCap);
  opts.bound_slope_below = true;
  opts.loss = CalibLossKind::Mid;
  ConvexFitContext ctx;
  ctx.noise_aware_regularization = false; // already folded into opts.lambda above

  const auto fit = fit_convex_slice(obs, kQpBoardF, kQpBoardT, kQpBoardDf, opts, w_prev, ctx);

  EXPECT_EQ(floor_misses, std::size_t{0}) << "node grid drifted from the captured board";
  EXPECT_EQ(floor_hits, std::size(kQpBoardFloorK)) << "node grid drifted from the captured board";
  ASSERT_TRUE(fit.has_value()) << fit.error().to_string();
  expect_converged_qp_diagnostics(*fit);
  expect_arb_free(*fit);
}
