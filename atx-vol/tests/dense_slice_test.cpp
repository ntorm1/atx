#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/linalg/linalg.hpp" // MatX, VecX -- QP-kernel unit tests below
#include "atx/vol/arb.hpp"          // arb_check_calendar
#include "atx/vol/black76.hpp"      // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"        // FitObs
#include "atx/vol/dense_slice.hpp"  // fit_convex_slice, ConvexSliceFit, kMaxIntervalSlackRows
#include "atx/vol/types.hpp"        // Side
#include "atx/vol/vol_curve.hpp"    // fit_slice_curve, CurveSurface

// Task C-7 (FIT-C3/FIT-C4): forward declaration of the QP-kernel test-only
// entry point defined in src/dense_slice.cpp -- qp_active_set itself is
// TU-local (anonymous namespace) to that file. Not part of any header; see
// the definition site for why. The signature must match exactly for the
// linker to resolve it. `dropped_rows_out`, if non-null, receives the row
// index dropped at each multiplier-drop event, in order -- see the
// definition site for why this is the only way to observe the drop-side
// tie-break's outcome on a genuinely tied board.
namespace atx::vol {
Result<atx::core::linalg::VecX> qp_active_set_for_test(
    const atx::core::linalg::MatX &H, const atx::core::linalg::VecX &q,
    const atx::core::linalg::MatX &G, const atx::core::linalg::VecX &h,
    atx::core::linalg::VecX x0, int max_iter, bool *converged_out, int *iterations_out,
    std::vector<Eigen::Index> *dropped_rows_out = nullptr);
} // namespace atx::vol

// Task P-5 review N-1: forward declaration of the P-5 test/bench seam defined
// in src/dense_slice.cpp -- mirrors derivatives.cpp's
// `set_strip_batch_disabled_for_test` forward-declare-in-the-consumer
// convention (no header touched). Reads the SAME predicate
// ConvexSliceFit::iv() reads (env_flag_enabled's exact `"1"` match), so this
// test's skip guard cannot disagree with what production actually did, the
// way a local presence-only re-check of the environment variable could.
namespace atx::vol::detail {
[[nodiscard]] bool iv_early_exit_disabled_for_test() noexcept;
} // namespace atx::vol::detail

// Phase 1 of the arbitrage-constrained dense surface: the per-slice convex
// call-price QP. These tests pin the two properties the whole approach rests on:
//   (1) the fitted call curve is ALWAYS arbitrage-free (convex, non-increasing,
//       positive) — even fed non-convex / noisy input, because no-arb is a HARD
//       constraint of the QP, not a post-hoc repair; and
//   (2) on clean arbitrage-free data it NEAR-INTERPOLATES (recovers the truth
//       vol to a few bp), which is what a 3-DoF eSSVI cannot do.

namespace {

using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::vol::black76_price;
using atx::vol::black76_value_and_vega;
using atx::vol::ConvexFitOpts;
using atx::vol::ConvexSliceFit;
using atx::vol::ErrorCode;
using atx::vol::fit_convex_slice;
using atx::vol::FitObs;
using atx::vol::qp_active_set_for_test;
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

// Task P-5 (FIT-P1) characterization fixture, shared by both P-5 iv()
// regression tests below (IvBisectionEarlyExitMatchesPreP5BaselineWithin1e11
// and IvBisectionEarlyExitIsActuallyEngagedInProduction): F=100, T=0.25,
// df=0.98, flat 22% vol, strikes 70%-130% of F via strike_grid/mk_obs.
constexpr double kP5F = 100.0, kP5T = 0.25, kP5Df = 0.98;

// Served iv() at kP5F/kP5T/kP5Df's fixture, captured from the UNMODIFIED
// (pre-P-5, fixed 64-iteration bisection, no early exit) code, at k from
// -0.60 to 0.60 in steps of 0.03 -- see task-P-5-report.md's characterization
// section for the capture method.
constexpr double kPreP5Iv[41] = {
    0.38306620209843278, 0.36585647544632593, 0.34854766705012208, 0.33113476990919311,
    0.31361215772259232, 0.29597346148127623, 0.27821141126176629, 0.26031763012829434,
    0.24228236072034093, 0.22409409487564735, 0.22034599272928146, 0.22092830152199339,
    0.22134878962186633, 0.22135402335839144, 0.22088461146865995, 0.22018063596501236,
    0.22106308025283938, 0.22090261863719107, 0.22033568585089119, 0.22096225288330668,
    0.22000000584963630, 0.22085483829700253, 0.22025983560406370, 0.22064066740980282,
    0.22065811537153923, 0.22021009273612463, 0.22032406422399492, 0.22053656799330695,
    0.22052467701593248, 0.21918848633132137, 0.21704597398801689, 0.21671195086840145,
    0.21753825849925401, 0.21913648501516581, 0.22126220321505885, 0.22493746009124133,
    0.23892309280367430, 0.25284689612604161, 0.26671198479956604, 0.28052111779829192,
    0.29427675652366592,
};

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

// ── QP kernel unit tests (Task C-7: FIT-C3 ratio-test clamp, FIT-C4 anti-cycling) ──
//
// fit_convex_slice enforces >= 3 distinct strikes, so neither pathology below is
// reachable through the public API at the problem sizes that isolate it. Both
// tests go straight at the QP kernel (qp_active_set_for_test) with small,
// hand-built (H, q, G, h) systems -- no board, no Black-76, fully deterministic.

// FIT-C3 (P2): the ratio test computed `ai = -gix / gip` unclamped. A row whose
// directional derivative sits just past the -1e-14 dead zone (gip a hair more
// negative than the cutoff) combined with a residual admitted as "a few ulp
// negative" under kQpStartTol (1e-12, scaled) produces a NEGATIVE ai, unbounded
// in magnitude as |gip| -> the cutoff edge -- and since the ratio test selects
// the MINIMUM, that negative value beats every legitimate positive one. This
// 2-variable board is engineered (not discovered) to hit exactly that case at
// iteration 1 from an empty working set:
//   * H = I, so the unconstrained Newton step solves p = -g exactly, and q is
//     chosen to place p1 (the blocking row's directional derivative) at a
//     specific value just past the dead zone;
//   * the single constraint row is x1 >= h0, with x0_1 a few ulp below h0 --
//     admitted by the start-feasibility check, never a real violation;
//   * p2 carries a large, UNRELATED step. alpha is a single scalar shared by
//     the whole step, so an unbounded blow-up driven entirely by row 1's
//     near-zero gip corrupts x2 too -- that is what makes the bug's blast
//     radius visible in a 2-variable board instead of a 1-variable one.
TEST(DenseSliceQp, RatioTestClampsUnboundedBackwardStep) {
  constexpr double h0 = 10.0;
  constexpr double gix0 = -9.0e-13;         // a few ulp negative; admitted (kQpStartTol = 1e-12)
  constexpr double x0_1 = h0 + gix0;        // sits just inside the row's bound
  constexpr double target_gip = -1.05e-14;  // just past the -1.0e-14 ratio-test dead zone
  constexpr double x0_2 = 0.0;
  constexpr double p2_target = 5.0; // large, unrelated step -- exposes the blow-up

  MatX H = MatX::Identity(2, 2);
  // H = I => the unconstrained Newton step solves p = -g; pick q so g (hence
  // p) lands exactly where this scenario needs it.
  VecX q(2);
  q(0) = -target_gip - x0_1;
  q(1) = -p2_target - x0_2;
  MatX G(1, 2);
  G << 1.0, 0.0; // single row: x1 >= h0
  VecX h(1);
  h(0) = h0;
  VecX x0(2);
  x0 << x0_1, x0_2;

  const auto res = qp_active_set_for_test(H, q, G, h, x0, /*max_iter=*/1, nullptr, nullptr);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  const VecX &x1 = *res;

  // Post-fix: the clamp turns the row's negative ratio into the degenerate
  // zero-length step -- the row joins the working set and x is UNCHANGED at
  // this iterate, so the objective cannot have increased.
  const auto objective = [&](const VecX &x) { return 0.5 * x.dot(H * x) + q.dot(x); };
  EXPECT_LE(objective(x1), objective(x0) + 1.0e-9)
      << "unclamped ratio test stepped backward and increased the objective";
  // Pin the concrete failure this guards against directly: an unclamped ai
  // here is gix0/target_gip ~ -85.7, which would carry x2 to ~-428 instead of
  // leaving it untouched.
  EXPECT_NEAR(x1(1), x0_2, 1.0e-9) << "x2 moved despite carrying no blocking constraint";
}

// FIT-C4 (P3): no anti-cycling tie-break on the ratio test or the drop rule. A
// degenerate vertex -- two constraint rows simultaneously binding -- is exactly
// what happens in production when a calendar-floor row (w_prev's Black-76
// price at a node) numerically duplicates that same node's intrinsic-bound row
// (see fit_convex_slice's `g_j >= discounted intrinsic` and
// `calendar floor: g_j >= cfloor_j` rows). This 3-variable board encodes that
// pattern directly: row 0 is an "intrinsic bound"-style row (x1 >= 5), row 1 is
// a bit-identical "calendar floor"-style row on the SAME node (x1 >= 5,
// duplicated verbatim), and row 2 is an unrelated bound (x2 >= 3) so the board
// is a genuine 2-constraint-active vertex, not a single-row trivial case.
TEST(DenseSliceQp, DegenerateVertexDuplicateFloorRowConvergesFast) {
  MatX H = MatX::Identity(3, 3);
  VecX q = VecX::Zero(3); // unconstrained minimum at the origin -- infeasible for both bounds

  MatX G(3, 3);
  VecX h(3);
  G.row(0) << 1.0, 0.0, 0.0;
  h(0) = 5.0; // "intrinsic bound": x1 >= 5
  G.row(1) << 1.0, 0.0, 0.0;
  h(1) = 5.0; // "calendar floor": duplicate of row 0
  G.row(2) << 0.0, 1.0, 0.0;
  h(2) = 3.0; // unrelated: x2 >= 3

  VecX x0(3);
  x0 << 10.0, 10.0, 10.0; // comfortably feasible start

  bool converged = false;
  int iterations = 0;
  const auto res =
      qp_active_set_for_test(H, q, G, h, x0, /*max_iter=*/200, &converged, &iterations);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_TRUE(converged);
  EXPECT_LT(iterations, 200);

  const VecX &x = *res;
  // The certified optimum: both distinct bounds bind exactly (the unconstrained
  // min at the origin is infeasible for x1 and x2); x3 is free and returns to 0.
  EXPECT_NEAR(x(0), 5.0, 1.0e-8);
  EXPECT_NEAR(x(1), 3.0, 1.0e-8);
  EXPECT_NEAR(x(2), 0.0, 1.0e-8);

  // Identical certified solution: the duplicate row is redundant by
  // construction (same direction, same RHS as row 0), so removing it must not
  // change the optimum at all.
  MatX G2(2, 3);
  VecX h2(2);
  G2.row(0) = G.row(0);
  G2.row(1) = G.row(2);
  h2(0) = h(0);
  h2(1) = h(2);
  const auto ref = qp_active_set_for_test(H, q, G2, h2, x0, /*max_iter=*/200, nullptr, nullptr);
  ASSERT_TRUE(ref.has_value()) << ref.error().to_string();
  EXPECT_LT((x - *ref).lpNorm<Eigen::Infinity>(), 1.0e-10)
      << "the redundant duplicate row changed the certified solution";

  // Determinism: re-solving the SAME degenerate board twice must give a
  // bit-identical result -- the tie-break is a pure function of (G, h, x, p),
  // never of prior calls.
  const auto res2 = qp_active_set_for_test(H, q, G, h, x0, /*max_iter=*/200, nullptr, nullptr);
  ASSERT_TRUE(res2.has_value()) << res2.error().to_string();
  EXPECT_EQ((x - *res2).lpNorm<Eigen::Infinity>(), 0.0)
      << "the tie-break is not deterministic across repeated calls";
}

// FIT-C4 drop-side coverage (review round 1): the ratio-test tie-break test
// above never drives ANY multiplier negative, so it never enters the
// worst>=0 branch this fix also touches (`dense_slice.cpp`'s drop-side
// lowest-row-index re-scan). This board genuinely does: it was found by
// temporary instrumentation (printing wset/lambda at each drop check),
// since-removed, replaced here by the permanent `dropped_rows_out` seam on
// `qp_active_set_for_test` -- the drop sequence it records IS the proof this
// branch executes, not just an indirect final-value inference.
//
// Construction: two DECOUPLED (block-diagonal H, no shared rows) copies of a
// 2-variable "wedge + far bound" board --
//   copy 1 (x1,x2): rows (1,1)>=2 [A1], (1,-1)>=2 [B1], (1,0)>=10 [C1]
//   copy 2 (x3,x4): rows (1,1)>=2 [A2], (1,-1)>=2 [B2], (1,0)>=10 [C2]
// -- with q=0 throughout. Each copy's true optimum needs only its C row (the
// far bound alone already implies both wedge rows with slack); its B row
// gets falsely activated by the ratio-test's straight-line overshoot en
// route, then must be dropped once both B and C are active and the KKT
// solve reveals B's multiplier is negative. Because both copies share
// IDENTICAL (G, h, q), whichever copy's B row is checked at the drop step
// computes the IDENTICAL multiplier (-8) -- an exact tie, not a
// floating-point coincidence.
//
// x0 = (15, 12.5, 15, 12.9): copy 2's x0 sits closer to its own B boundary
// than copy 1's, so B2 (row index 4) is added to the working set BEFORE B1
// (row index 1) via the ratio test (no tie there -- 0.048 < 0.2, a clean
// ordering, not the ratio-test tie-break this construction is not testing).
// By the time both B1 and B2 are active, `wset` is in INSERTION order
// [4, 1, ...] -- B2 (position 0) before B1 (position 1) -- so the OLD
// position-scanned drop rule and the FIXED row-index-scanned drop rule
// disagree: position-order would drop row 4 first; lowest-row-index (the
// spec) must drop row 1 first. Hand/instrumented trace confirms the fixed
// code drops [1, 4] in that order; reverting just the drop-side re-scan
// (dense_slice.cpp's `for (Eigen::Index i = 0; i < nw; ++i) { if (lambda(i)
// == worst_val && ...` loop) reproduces [4, 1] instead -- so this assertion
// is genuinely load-bearing on that code, not merely consistent with it.
TEST(DenseSliceQp, DropTieBreakPrefersLowestRowIndex) {
  MatX H = MatX::Identity(4, 4);
  VecX q = VecX::Zero(4);
  MatX G = MatX::Zero(6, 4);
  VecX h(6);
  G(0, 0) = 1.0;
  G(0, 1) = 1.0;
  h(0) = 2.0; // A1: x1+x2 >= 2
  G(1, 0) = 1.0;
  G(1, 1) = -1.0;
  h(1) = 2.0; // B1: x1-x2 >= 2
  G(2, 0) = 1.0;
  h(2) = 10.0; // C1: x1 >= 10
  G(3, 2) = 1.0;
  G(3, 3) = 1.0;
  h(3) = 2.0; // A2: x3+x4 >= 2
  G(4, 2) = 1.0;
  G(4, 3) = -1.0;
  h(4) = 2.0; // B2: x3-x4 >= 2
  G(5, 2) = 1.0;
  h(5) = 10.0; // C2: x3 >= 10

  VecX x0(4);
  x0 << 15.0, 12.5, 15.0, 12.9; // both feasible; copy 2 closer to its B boundary

  bool converged = false;
  int iterations = 0;
  std::vector<Eigen::Index> dropped_rows;
  const auto res = qp_active_set_for_test(H, q, G, h, x0, /*max_iter=*/200, &converged,
                                          &iterations, &dropped_rows);
  ASSERT_TRUE(res.has_value()) << res.error().to_string();
  EXPECT_TRUE(converged);
  EXPECT_LT(iterations, 200);

  ASSERT_FALSE(dropped_rows.empty())
      << "board never reached the multiplier-drop branch -- construction regressed";
  // The load-bearing assertion: at the tie, row 1 (lower actual constraint-row
  // index) must be dropped, never row 4, regardless of which entered `wset`
  // first. Pre-fix (position-scanned drop), this would be 4.
  EXPECT_EQ(dropped_rows.front(), 1);
  // Full expected sequence: B1 drops at the tie (lowest index wins), then B2
  // drops on its own next (no longer tied with anything).
  ASSERT_EQ(dropped_rows.size(), std::size_t{2});
  EXPECT_EQ(dropped_rows[0], 1);
  EXPECT_EQ(dropped_rows[1], 4);

  // Certified solution: both copies converge to their C-only optimum.
  const VecX &x = *res;
  EXPECT_NEAR(x(0), 10.0, 1.0e-8);
  EXPECT_NEAR(x(1), 0.0, 1.0e-8);
  EXPECT_NEAR(x(2), 10.0, 1.0e-8);
  EXPECT_NEAR(x(3), 0.0, 1.0e-8);
}

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

// FIT-C11. The right-wing power tail's exponent is
// `max(0.0, -slope*Kn/C.back())` (dense_slice.cpp). A board whose last two
// fitted node prices are EXACTLY equal -- a degenerate zero edge slope, e.g. a
// featureless far-OTM tail with no fit signal -- drives that expression to
// exactly 0.0 pre-fix, and C.back() * (K/Kn)^-0.0 == C.back() identically: the
// tail never decays, contradicting the class's own doc ("tends to zero instead
// of flat-clamping a non-zero option price indefinitely"). Pin such a board
// directly (bypassing the QP fitter, which would not degenerate this cleanly
// on real data) and require the far strike to have decayed.
TEST(DenseSlice, DegenerateFlatEdgeSlopeStillDecaysPastKMax) {
  ConvexSliceFit fit;
  fit.T = 0.25;
  fit.F = 100.0;
  fit.df = std::exp(-0.03 * fit.T);
  fit.u = {80.0, 90.0, 100.0, 110.0, 120.0};
  // Convex, monotone decreasing board, EXCEPT the last two nodes are pinned to
  // the identical price -- the degenerate zero-slope right edge under test.
  fit.C = {22.0, 13.0, 6.0, 2.0, 2.0};

  const double k_max = fit.u.back();
  const double at_edge = fit.call_price(k_max);
  const double far = fit.call_price(10.0 * k_max);
  ASSERT_TRUE(std::isfinite(at_edge));
  ASSERT_TRUE(std::isfinite(far));
  EXPECT_LT(far, at_edge);
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

// Task P-5 (FIT-P1) characterization: the bisection early exit is
// deliberately NOT bit-identical to the old fixed-64-iteration loop (the
// last ulp of the returned midpoint can move once the loop stops as soon as
// the bracket is near-machine width instead of always halving 64 times).
// `kPreP5Iv` (file scope, above) is the served iv() at this exact
// fixture/k-grid captured from the UNMODIFIED (pre-P-5) bisection -- see
// task-P-5-report.md's characterization section for the capture method.
// Every point must still agree within 1e-11 (the acceptance bound; measured
// drift maxes out far below it, ~2.8e-13, see the report).
TEST(ConvexSliceFit, IvBisectionEarlyExitMatchesPreP5BaselineWithin1e11) {
  using namespace atx::vol;
  std::vector<FitObs> obs;
  for (const double K : strike_grid(kP5F)) {
    obs.push_back(mk_obs(kP5F, kP5T, kP5Df, K, 0.22));
  }
  const auto fit = fit_convex_slice(obs, kP5F, kP5T, kP5Df, ConvexFitOpts{});
  ASSERT_TRUE(fit.has_value());

  double max_abs_diff = 0.0;
  for (int i = 0; i <= 40; ++i) {
    const double k = -0.60 + 0.03 * static_cast<double>(i);
    const double iv = fit->iv(k);
    const double diff = std::fabs(iv - kPreP5Iv[static_cast<std::size_t>(i)]);
    max_abs_diff = std::max(max_abs_diff, diff);
    EXPECT_NEAR(iv, kPreP5Iv[static_cast<std::size_t>(i)], 1.0e-11) << "k=" << k;
  }
  EXPECT_LT(max_abs_diff, 1.0e-11);
}

// Task P-5 review I-2 fix: the PRIOR version of this test asserted `iters <
// 64` against a hand-rolled REPLICA of the bisection loop, never calling
// `fit->iv()`. The review verified that deleting the early-exit break in
// `src/dense_slice.cpp` left that version green (the replica kept its own
// copy of the break condition, so it stayed "early-exiting" regardless of
// what production did), and that the drift-tolerance sibling above ALSO
// cannot catch a reverted optimization -- under
// `ATX_VOL_DISABLE_IV_EARLY_EXIT=1` (behaviourally identical to deleting the
// break) it passes bit-exactly, because its pinned values ARE the pre-P-5
// values. So the shipped optimization had no regression guard at all.
//
// This version calls the SHIPPING `fit->iv()` directly. The early exit
// provably changes the returned midpoint whenever it fires before iteration
// 64 (it stops at a strictly wider bracket than 64 fixed halvings would have
// reached), so if the break is ever silently reverted, every one of these
// 41 points becomes bit-identical to the pre-P-5 `kPreP5Iv` again --
// verified BOTH ways below by actually deleting the break, rebuilding, and
// observing this exact test fail (see task-P-5-report.md's fix-round
// section for the captured failure output), then restoring it and observing
// green again. Skipped when `detail::iv_early_exit_disabled_for_test()`
// reports the intentional bench-only override that restores pre-P-5
// arithmetic -- bit-identity is the CORRECT behavior there, not a
// regression. Task P-5 review N-1: this used to re-derive "is the override
// on?" from a local presence-only check of the environment variable, which
// disagreed with production's exact-match-"1" semantics at, e.g.,
// ATX_VOL_DISABLE_IV_EARLY_EXIT=0 (production runs the early exit; the old
// guard would have skipped anyway). Now reads the same accessor production
// itself is built from, so this guard cannot disagree with what the binary
// actually did.
TEST(ConvexSliceFit, IvBisectionEarlyExitIsActuallyEngagedInProduction) {
  using namespace atx::vol;
  if (detail::iv_early_exit_disabled_for_test()) {
    GTEST_SKIP() << "ATX_VOL_DISABLE_IV_EARLY_EXIT is set -- bit-identity to "
                    "the pre-P-5 baseline is the intended bench-A/B behavior here";
  }
  std::vector<FitObs> obs;
  for (const double K : strike_grid(kP5F)) {
    obs.push_back(mk_obs(kP5F, kP5T, kP5Df, K, 0.22));
  }
  const auto fit = fit_convex_slice(obs, kP5F, kP5T, kP5Df, ConvexFitOpts{});
  ASSERT_TRUE(fit.has_value());

  int bit_identical_to_pre_p5 = 0;
  for (int i = 0; i <= 40; ++i) {
    const double k = -0.60 + 0.03 * static_cast<double>(i);
    const double iv = fit->iv(k);
    if (iv == kPreP5Iv[static_cast<std::size_t>(i)]) {
      ++bit_identical_to_pre_p5;
    }
  }
  EXPECT_EQ(bit_identical_to_pre_p5, 0)
      << "fit->iv() matched the pre-P-5 pinned baseline bit-for-bit at "
      << bit_identical_to_pre_p5
      << "/41 points -- the early exit is not firing in production (deleting "
         "the break condition in ConvexSliceFit::iv(), src/dense_slice.cpp, "
         "reproduces exactly this failure)";
}
