#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <optional>

#include "atx/vol/american.hpp"

// G4 (gaps-review finding 5): public early-exercise boundary + assignment-risk
// screen. Validates exercise_boundary() against:
//   - the pricer's own smooth-paste seam (value matching vs andersen_lake),
//   - the near-expiry homogeneity limit B(0+) = K*min(1,r/q) (put) /
//     K*max(1,r/q) (call), DERIVED from the code's al_xmax_put regime math,
//   - the perpetual level B∞ at long T,
//   - monotonicity in T,
//   - the McDonald-Schroder call/put symmetry map B_call*B_put(swapped) == K^2,
//   - the regime sentinels (European -> OutOfRange, double-continuation ->
//     NotImplemented, bad inputs -> InvalidArgument).
// And the assignment_risk carry-benefit-vs-time-value flips.

namespace {

using atx::vol::andersen_lake;
using atx::vol::assignment_risk;
using atx::vol::AssignmentRisk;
using atx::vol::exercise_boundary;
using atx::vol::Side;
using atx::core::ErrorCode;

// Perpetual American put exercise boundary B∞ = K*γ/(γ-1), γ the negative root of
// (σ²/2)γ² + (r-q-σ²/2)γ - r = 0. (q=0 for the case exercised below.)
double perpetual_put_boundary(double K, double r, double q, double sigma) {
  const double a = 0.5 * sigma * sigma;
  const double b = (r - q - 0.5 * sigma * sigma);
  const double c = -r;
  const double disc = std::sqrt(b * b - 4.0 * a * c);
  const double gamma_neg = (-b - disc) / (2.0 * a);
  return K * gamma_neg / (gamma_neg - 1.0);
}

// ── Near-expiry limit B(0+): the code's own homogeneity scale al_xmax_put ──
//
// B_put(0+) = K*min(1, r/q); B_call(0+) = K*max(1, r/q) (the reflected xmax).
// NOTE: the r<=q vs r>q labelling in the task prose is reversed — it is r>=q
// (not r<=q) that gives B_put(0+) = K, per K*min(1, r/q). These assert the
// code-derived formula.
TEST(ExerciseBoundary, NearExpiryLimitPutRgeQ) {
  // r > q -> min(1, r/q) = 1 -> B_put(0+) = K exactly.
  auto b = exercise_boundary(100.0, 0.0, 0.25, 0.06, 0.02, Side::Put);
  ASSERT_TRUE(b.has_value());
  EXPECT_NEAR(*b, 100.0, 1e-9);
}

TEST(ExerciseBoundary, NearExpiryLimitPutRltQ) {
  // r < q -> min(1, r/q) = r/q -> B_put(0+) = K*(r/q) < K.
  const double K = 100.0, r = 0.02, q = 0.06;
  auto b = exercise_boundary(K, 0.0, 0.25, r, q, Side::Put);
  ASSERT_TRUE(b.has_value());
  EXPECT_NEAR(*b, K * std::fmin(1.0, r / q), 1e-9); // 33.333...
}

TEST(ExerciseBoundary, NearExpiryLimitCall) {
  const double K = 100.0;
  // Call, r > q -> max(1, r/q) = r/q -> B_call(0+) = K*(r/q) > K.
  auto c_hi = exercise_boundary(K, 0.0, 0.25, 0.06, 0.02, Side::Call);
  ASSERT_TRUE(c_hi.has_value());
  EXPECT_NEAR(*c_hi, K * std::fmax(1.0, 0.06 / 0.02), 1e-6); // 300
  // Call, r < q -> max(1, r/q) = 1 -> B_call(0+) = K.
  auto c_lo = exercise_boundary(K, 0.0, 0.25, 0.02, 0.06, Side::Call);
  ASSERT_TRUE(c_lo.has_value());
  EXPECT_NEAR(*c_lo, K, 1e-6);
}

// ── Perpetual level: B_put(T -> inf) -> B∞ ────────────────────────────────
TEST(ExerciseBoundary, PerpetualLimit) {
  const double K = 100.0, r = 0.05, q = 0.0, s = 0.2;
  const double binf = perpetual_put_boundary(K, r, q, s); // 71.4286
  auto b100 = exercise_boundary(K, 100.0, s, r, q, Side::Put);
  ASSERT_TRUE(b100.has_value());
  // Within 0.2% of the analytic perpetual level (measured rel err ~7e-5).
  EXPECT_NEAR(*b100, binf, binf * 2.0e-3);
  // Approaches from above (the boundary decreases toward B∞).
  EXPECT_GT(*b100, binf - 1.0e-3);
  // Closer to B∞ than a much shorter maturity.
  auto b5 = exercise_boundary(K, 5.0, s, r, q, Side::Put);
  ASSERT_TRUE(b5.has_value());
  EXPECT_LT(std::abs(*b100 - binf), std::abs(*b5 - binf));
}

// ── Monotonicity in T: the put critical price decreases with maturity ─────
TEST(ExerciseBoundary, MonotoneDecreasingInTPut) {
  const double K = 100.0, r = 0.05, q = 0.0, s = 0.2;
  const double Ts[] = {0.5, 1.0, 5.0, 20.0, 50.0, 100.0};
  double prev = 1e18;
  for (double T : Ts) {
    auto b = exercise_boundary(K, T, s, r, q, Side::Put);
    ASSERT_TRUE(b.has_value());
    EXPECT_LT(*b, prev) << "T=" << T;
    prev = *b;
  }
}

// ── Value matching: exposed boundary IS the pricer's own boundary ─────────
//
// Tight parity vs the internal Andersen-Lake boundary: at S = B the American put
// mark equals intrinsic (smooth paste); just above B it carries positive time
// value (continuation region); just below B it is clamped to intrinsic (exercise
// region). Both exercise_boundary and andersen_lake use the same ACCURATE preset.
TEST(ExerciseBoundary, ValueMatchesInternalBoundaryPut) {
  const double K = 100.0, r = 0.06, q = 0.02, s = 0.25, T = 0.5;
  auto b = exercise_boundary(K, T, s, r, q, Side::Put);
  ASSERT_TRUE(b.has_value());
  const double B = *b;
  EXPECT_GT(B, 0.0);
  EXPECT_LT(B, K); // put boundary sits below the strike

  auto pB = andersen_lake(B, K, T, s, r, q, Side::Put);
  auto pUp = andersen_lake(B * 1.05, K, T, s, r, q, Side::Put);
  auto pDn = andersen_lake(B * 0.95, K, T, s, r, q, Side::Put);
  ASSERT_TRUE(pB.has_value() && pUp.has_value() && pDn.has_value());

  // At the boundary: mark == intrinsic to the pricer's accuracy.
  EXPECT_NEAR(*pB, K - B, 1e-2);
  // Above the boundary: strictly above intrinsic (real time value remains).
  EXPECT_GT(*pUp, (K - B * 1.05) + 1e-3);
  // Below the boundary: exercise region, clamped exactly to intrinsic.
  EXPECT_NEAR(*pDn, K - B * 0.95, 1e-9);
}

// ── McDonald-Schroder symmetry: B_call(r,q) * B_put(q,r) == K^2 ───────────
TEST(ExerciseBoundary, CallPutSymmetry) {
  const double K = 100.0, s = 0.25, T = 0.5;
  const double r = 0.06, q = 0.02;
  auto bcall = exercise_boundary(K, T, s, r, q, Side::Call);
  auto bput = exercise_boundary(K, T, s, /*r=*/q, /*q=*/r, Side::Put); // swapped
  ASSERT_TRUE(bcall.has_value() && bput.has_value());
  EXPECT_NEAR((*bcall) * (*bput), K * K, K * K * 1e-12);
  EXPECT_GT(*bcall, K); // call boundary above the strike
}

// ── Regime sentinels ──────────────────────────────────────────────────────
TEST(ExerciseBoundary, EuropeanRegimeNoFiniteBoundary) {
  // No-dividend American call == European: no finite (upper) boundary.
  auto c = exercise_boundary(100.0, 0.5, 0.25, /*r=*/0.05, /*q=*/0.0, Side::Call);
  ASSERT_FALSE(c.has_value());
  EXPECT_EQ(c.error().code(), ErrorCode::OutOfRange);
  // European put regime (r <= 0 && r <= q): no finite (lower) boundary.
  auto p = exercise_boundary(100.0, 0.5, 0.25, /*r=*/0.0, /*q=*/0.05, Side::Put);
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error().code(), ErrorCode::OutOfRange);
}

TEST(ExerciseBoundary, DoubleContinuationNotImplemented) {
  // Put double-continuation corner q < r <= 0.
  auto p = exercise_boundary(100.0, 0.5, 0.25, /*r=*/-0.01, /*q=*/-0.05, Side::Put);
  ASSERT_FALSE(p.has_value());
  EXPECT_EQ(p.error().code(), ErrorCode::NotImplemented);
}

TEST(ExerciseBoundary, InvalidArguments) {
  EXPECT_EQ(exercise_boundary(-1.0, 0.5, 0.25, 0.05, 0.0, Side::Put).error().code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(exercise_boundary(100.0, -0.5, 0.25, 0.05, 0.0, Side::Put).error().code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(exercise_boundary(100.0, 0.5, -0.25, 0.05, 0.0, Side::Put).error().code(),
            ErrorCode::InvalidArgument);
  EXPECT_EQ(exercise_boundary(100.0, 0.5, 0.25, std::nan(""), 0.0, Side::Put).error().code(),
            ErrorCode::InvalidArgument);
}

// ── Assignment-risk screen ────────────────────────────────────────────────
//
// deep-ITM CALL: pending dividend q*S*T vs remaining time value.
// deep-ITM PUT : interest on the strike r*K*T vs remaining time value.
TEST(ExerciseBoundary, AssignmentRiskCallDividendFlip) {
  const double S = 150.0, K = 100.0, T = 0.5, s = 0.25, r = 0.03;
  // High dividend -> pending div (q*S*T) exceeds the (near-zero) time value.
  auto hi = assignment_risk(S, K, T, s, r, /*q=*/0.10, Side::Call);
  ASSERT_TRUE(hi.has_value());
  EXPECT_TRUE(hi->at_risk);
  EXPECT_GT(hi->margin, 0.0);
  EXPECT_NEAR(hi->carry_benefit, 0.10 * S * T, 1e-12);
  // Tiny dividend -> pending div below the time value: flag clears.
  auto lo = assignment_risk(S, K, T, s, r, /*q=*/0.005, Side::Call);
  ASSERT_TRUE(lo.has_value());
  EXPECT_FALSE(lo->at_risk);
  EXPECT_LT(lo->margin, 0.0);
}

TEST(ExerciseBoundary, AssignmentRiskPutInterestFlip) {
  const double S = 85.0, K = 100.0, T = 0.5, s = 0.25, q = 0.0;
  // High rate -> interest on strike (r*K*T) exceeds the small time value.
  auto hi = assignment_risk(S, K, T, s, /*r=*/0.10, q, Side::Put);
  ASSERT_TRUE(hi.has_value());
  EXPECT_TRUE(hi->at_risk);
  EXPECT_GT(hi->margin, 0.0);
  EXPECT_NEAR(hi->carry_benefit, 0.10 * K * T, 1e-12);
  // Low rate -> interest below the retained time value: flag clears.
  auto lo = assignment_risk(S, K, T, s, /*r=*/0.005, q, Side::Put);
  ASSERT_TRUE(lo.has_value());
  EXPECT_FALSE(lo->at_risk);
  EXPECT_LT(lo->margin, 0.0);
}

TEST(ExerciseBoundary, AssignmentRiskOtmNeverFlagged) {
  // Out-of-the-money: no intrinsic, never an assignment candidate.
  auto call = assignment_risk(80.0, 100.0, 0.5, 0.25, 0.03, 0.10, Side::Call);
  ASSERT_TRUE(call.has_value());
  EXPECT_FALSE(call->at_risk);
  auto put = assignment_risk(120.0, 100.0, 0.5, 0.25, 0.10, 0.0, Side::Put);
  ASSERT_TRUE(put.has_value());
  EXPECT_FALSE(put->at_risk);
}

} // namespace
