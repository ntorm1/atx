#pragma once

// Shared price-space projection between ConvexSliceFit::iv() and the ConvexDense
// calendar-admission scan (Task P-5 review I-1). Both call sites need to decide
// whether a fitted call price is invertible to a Black-76 vol -- iv() to actually
// invert it, scan_k (src/vol_curve.cpp) to compare it against a floor price
// without inverting. Both MUST agree on which prices are invertible and which
// (clamped) price they invert/compare, or the two paths silently disagree in
// exactly the wing region where prices sit close to the intrinsic/forward no-arb
// bound -- this is the defect class that produced a 3e-3-5e-3 `w` divergence in
// P-5's first (rejected) attempt at the price-space scan, caught only by diffing
// against a captured baseline, not by inspection. One shared definition removes
// the possibility of the two drifting apart again.

#include <algorithm>
#include <cmath>
#include <limits>

#include "atx/vol/black76.hpp" // black76_price
#include "atx/vol/types.hpp"   // Side, kIvMax

namespace atx::vol::detail {

// Projects a raw fitted call price `c` (strike K, forward F, discount df, time
// T) into Black's open interval, exactly as ConvexSliceFit::iv() does before it
// would bisect: price-space convex wings legitimately approach intrinsic/zero
// so closely that a finite-vol root is lost to floating-point cancellation, so
// the price is pulled away from the (lower, upper) = (discounted intrinsic,
// discounted forward) bound by a small epsilon. Also verifies the projected
// price is bracketable within [.., kIvMax] the way iv()'s bisection requires.
//
// Returns the projected (clamped) price on success; NaN under every condition
// iv() itself would return NaN for (non-finite input price, a bound-degenerate
// no-arb interval, or a price implying a vol beyond kIvMax) -- so a caller
// comparing this return against another price is making exactly the same
// admit/reject decision iv()-then-invert would have made, without inverting.
[[nodiscard]] inline double safe_call_price(double F, double K, double T, double df,
                                            double c) noexcept {
  if (!std::isfinite(c)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  const double lower = df * std::max(F - K, 0.0);
  const double upper = df * F;
  const double gap = upper - lower;
  const double epsilon = std::min(1.0e-6 * std::max(1.0, upper), 0.25 * gap);
  const double safe_price =
      std::min(std::max(c, lower + epsilon), std::nextafter(upper, lower));
  if (!(safe_price > lower) || !(safe_price < upper)) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  if (black76_price(F, K, T, kIvMax, df, Side::Call) < safe_price) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  return safe_price;
}

} // namespace atx::vol::detail
