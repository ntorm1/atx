#pragma once

// Shared vol-derivatives test fixture: flat-surface / flat-curve builders used
// by every deriv_*_test.cpp file that needs a minimal EssviSurface + CurveSet
// pair without pricing an entire fitted board.
//
// Copied verbatim (Task 3 / vol-of-vol) from the anonymous namespace of
// derivatives_test.cpp (`make_flat_surface`, `make_flat_curves`) into this
// shared, header-only home so later tests (deriv_distribution_test.cpp and
// beyond) do not each hand-roll their own copy. derivatives_test.cpp itself is
// intentionally left untouched -- it is not part of this task, and duplicating
// the two functions here (rather than having one file include the other's
// test .cpp) keeps each test binary independent of the other's translation
// unit.

#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/curve.hpp"
#include "atx/vol/surface.hpp"

namespace atx::vol::testsupport {

// Flat-vol synthetic surface: two eSSVI slices with theta = sigma^2 * T and
// near-zero curvature (phi ~ 0, rho = 0) so w(k) is essentially flat in k and
// iv(0, T) == sigma exactly for any T in [T_lo, T_hi].
inline EssviSurface make_flat_surface(double sigma, double T_lo, double T_hi) {
  EssviSurface surf(2);
  const EssviSlice s0{sigma * sigma * T_lo, 1.0e-6, 0.0, T_lo};
  const EssviSlice s1{sigma * sigma * T_hi, 1.0e-6, 0.0, T_hi};
  EXPECT_TRUE(surf.set_slice(0, s0).has_value());
  EXPECT_TRUE(surf.set_slice(1, s1).has_value());
  return surf;
}

// Flat-rate curve set with F == spot at both forward pillars. `rate` defaults
// to 0, the original zero-rate/no-carry behaviour every existing caller relies
// on; pass a non-zero flat continuously-compounded rate to exercise
// DISCOUNTING. The three pillars are collinear in log-df, so the
// Fritsch-Carlson interpolant is exactly linear across them and `zero(T)`
// returns `rate` at any T in range.
//
// The forwards deliberately stay at `spot` even under a non-zero rate: this is
// a discounting fixture, not a carry one, and holding F == S keeps the strip's
// k = 0 at the spot so the flat-surface analytic truths still hold.
inline CurveSet make_flat_curves(double spot, double T_lo, double T_hi, double rate = 0.0) {
  CurveSet cs;
  cs.spot = spot;
  const double t[] = {T_lo * 0.5, (T_lo + T_hi) * 0.5, T_hi * 2.0};
  const double r[] = {rate, rate, rate};
  EXPECT_TRUE(cs.set_yield(t, r).has_value());
  std::vector<ForwardPoint> pts(2);
  pts[0].T = T_lo;
  pts[0].F = spot;
  pts[1].T = T_hi;
  pts[1].F = spot;
  cs.forward.set(pts);
  return cs;
}

}  // namespace atx::vol::testsupport
