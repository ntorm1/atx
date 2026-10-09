#pragma once

// Shared-boundary de-Am IV-solve internals for the calibration observation
// builder (calib.cpp). Split out of the public calib.hpp API surface (Task 4,
// atx-vol API restructure): both symbols are used only by calib.cpp and by
// calib_test.cpp's direct unit-test seam, never by any other production TU.

#include <cstdint>

namespace atx::vol::detail {

// W3.1 shared-boundary per-lane price-acceptance gate, exposed for direct test
// (internal: not part of the supported API surface).
//
// A shared lane's accepted sigma is the root of the NINE-node interpolated price
// map. Two distinct errors separate that map's value from the true American
// price at the same sigma:
//   * `price - mid`  — the root-find residual the lane actually converged to;
//   * `price - embedded` — the 9-vs-5 Richardson gap, which is this route's only
//     ESTIMATE of the nine-node map's own interpolation error.
// The quantity the sprint bounds is the true price error, and by the triangle
// inequality
//     |price_true(sigma) - mid| <= |price_true(sigma) - price| + |price - mid|
//                              ~= |price - embedded|          + |price - mid|,
// so the SUM is what must clear `budget`. Gating each term against `budget`
// independently would only prove the sum is within 2 x budget — a bound the
// sprint never claimed. Returns true iff the lane is acceptable.
//
// Fail-closed: a non-finite input or a non-positive budget is never acceptable.
[[nodiscard]] bool shared_lane_residual_within_budget(double price, double mid, double embedded,
                                                      double budget) noexcept;

// W3.1 shared-boundary per-lane root-finding bracket, exposed for direct test
// (internal: not part of the supported API surface).
//
// Brackets a root of the nine-node interpolated price map in sigma. The caller
// establishes the invariant `f_lo < 0 <= f_hi` on `[lo, hi]` before the first
// step; every `update` call with a FINITE `residual` preserves it. Evaluator-
// agnostic on purpose: the lane loop supplies residuals from the interpolant,
// and the unit test supplies them from a closed-form price, so the test drives
// the SAME stepping logic production runs.
//
// Precondition on `update`: `residual` must be finite. A non-finite residual is
// not sign-tested (`NaN < 0.0` is false), so it falls into the `f_hi = residual`
// branch and writes NaN over the invariant regardless of its true sign. The sole
// production caller (`iterate_shared_lanes`, calib.cpp) already checks
// `std::isfinite(residual)` before calling `update` and never calls it
// otherwise; any other caller (this type is an exposed `detail` type, not
// enforced by the compiler) must do the same.
//
// Termination is on bracket WIDTH (`hi - lo <= solve_tol`), because that is what
// `finalize_shared_lane` re-tests before accepting a lane -- so the width, not the
// residual, is the quantity a step must contract.
struct SharedLaneBracket {
  // Steps after which the secant is abandoned and every remaining step bisects
  // unconditionally. This is the constructive iteration bound (see next_sigma):
  // Illinois converges this map in ~5 steps and at worst 11 measured, so at 24
  // the backstop is >2x clear of the real workload and never fires in practice;
  // once it does fire, each step halves, and the widest bracket the route admits
  // (w0 <= kObsIvMax - kSharedMinSigma = 4.99) needs at most
  // ceil(log2(4.99 / 1e-9)) = 33 halvings against the tightest solve_tol. So a
  // lane terminates within 24 + 33 = 57 evaluations, inside the max_iter = 64 the
  // production route passes.
  static constexpr std::uint16_t kMaxSecantSteps = 24u;

  double lo{0.0};
  double hi{0.0};
  double f_lo{0.0};
  double f_hi{0.0};
  // Which endpoint survived the previous update: +1 = hi, -1 = lo, 0 = no update
  // yet. Drives the Illinois deflation in `update`.
  std::int8_t retained{0};
  // Steps folded in so far; arms the bisection backstop above.
  std::uint16_t steps{0};

  // Next sigma to probe: the Illinois-modified regula-falsi step while it lands
  // strictly inside the bracket, else the midpoint.
  [[nodiscard]] double next_sigma() const noexcept;

  // Fold a probe `(sigma, residual)` into the bracket, keeping the side whose
  // sign it matches, then apply the Illinois deflation (see below).
  // PRECONDITION: `residual` must be finite -- the caller establishes this (see
  // the struct comment above).
  void update(double sigma, double residual) noexcept;
};

} // namespace atx::vol::detail
