#pragma once

// THE butterfly finite-difference rule of atx-vol, stated once.
//
// The Lee/Roper-Durrleman risk-neutral density in total-variance coordinates,
//
//   g(k) = (1 - k*w'/(2w))^2 - (w'/2)^2 * (1/4 + 1/w) + w''/2
//
// with w' and w'' taken by CENTRAL difference on a uniform k-grid of the total
// surface variance, sharing FD neighbours across sample points. A grid point
// whose g dips strictly below `kButterflyDensityFloor` is a butterfly violation.
//
// WHY THIS HEADER EXISTS. Four call sites each carried a hand-written copy of
// this loop -- arb.cpp's shared per-slice scan, arb.cpp's `IVolCurve` overload,
// arb.cpp's count-only `arb_check_total_surface_all`, and spline_curve.cpp's
// post-fit diagnostic -- while arb.cpp asserted in a comment that it held "the
// SINGLE implementation", which was false in its own file. The tolerance and the
// stencil are ONE rule: naming the constant while leaving four copies of the
// stencil would have moved the drift hazard down a level rather than closing it.
// So both live here, together, and every site is a call rather than a copy.
//
// WHY HERE and not `types.hpp`, where this sprint homed `kCalendarTotalVarianceTol`.
// That constant is public because consumers are contractually required to state
// MULTIPLES of it rather than their own literal (types.hpp documents the rule).
// Nothing outside the four scans below ever names this floor -- it is only ever
// compared against inside the loop -- and the v1.x API is frozen, so publishing it
// would widen a frozen surface for zero callers. `src/<module>/` is where atx-vol
// keeps header-only internals shared across translation units (the tier the
// api-restructure put in the retired `detail/`'s place), and this header includes
// nothing but <cmath>/<cstdint>, so it sits below every consumer:
// the include-cycle wall that forced `kCalendarTotalVarianceTol` out of `arb.hpp`
// (see arb.hpp's note) cannot recur here.

#include <cmath>
#include <cstdint>

namespace atx::vol::detail {

// Density floor: `g(k)` strictly below this counts as a butterfly violation. The
// sign lives INSIDE the constant so every site keeps the `g < floor` comparison
// it already had and no site has to re-derive a negation.
inline constexpr double kButterflyDensityFloor = -1.0e-9;

// Stencil-centre total-variance positivity guard. At or below it the 1/w terms of
// g carry no information about the density, so the point is not evaluatable.
inline constexpr double kButterflyStencilWFloor = 1.0e-12;

// What a scan does with a grid point whose stencil is not evaluatable.
//
// This enum exists because the four unified sites did NOT agree here, and the
// disagreement is substantive rather than an artifact of copying: the `IVolCurve`
// overload is the independent post-projection GATE, so a point it could not
// evaluate must not read as clean, and it records one infinite-slack violation
// instead. The other three are surface walks and diagnostics that skip. Folding
// the two together would have been a behaviour change wearing a cleanup's
// clothes.
enum class ButterflyStencilPolicy : std::uint8_t {
  // Skip the point silently.
  Skip = 0,
  // Hand the point to `on_unusable`, and additionally treat a non-finite CENTRE
  // as unusable rather than letting an infinity flow through the arithmetic.
  ReportUnusable = 1,
};

// Scan the INTERIOR points of the uniform grid that splits [k_min, k_max] into
// `n_grid` cells. `on_violation(k, g)` fires at each point breaching the floor;
// `on_unusable(k)` fires only under `ReportUnusable`. The caller owns the entry
// guards (`k_max > k_min`, and whatever minimum `n_grid` its contract states).
template <class WOfK, class OnViolation, class OnUnusable>
void butterfly_density_scan(const WOfK &w_of_k, double k_min, double k_max,
                            std::uint32_t n_grid, ButterflyStencilPolicy policy,
                            const OnViolation &on_violation,
                            const OnUnusable &on_unusable) {
  const double dk = (k_max - k_min) / static_cast<double>(n_grid);
  const double inv_2dk = 0.5 / dk;
  const double inv_dksq = 1.0 / (dk * dk);
  const bool report = (policy == ButterflyStencilPolicy::ReportUnusable);
  for (std::uint32_t g = 1; g < n_grid; ++g) {
    const double k = k_min + static_cast<double>(g) * dk;
    const double w_lo = w_of_k(k - dk);
    const double w_mi = w_of_k(k);
    const double w_hi = w_of_k(k + dk);
    if (!(w_mi > kButterflyStencilWFloor) || !std::isfinite(w_lo) ||
        !std::isfinite(w_hi) || (report && !std::isfinite(w_mi))) {
      if (report) {
        on_unusable(k);
      }
      continue;
    }
    const double w_p = (w_hi - w_lo) * inv_2dk;                 // w'(k)
    const double w_pp = (w_hi - 2.0 * w_mi + w_lo) * inv_dksq;  // w''(k)

    const double term1_inner = 1.0 - 0.5 * k * w_p / w_mi;
    const double term1 = term1_inner * term1_inner;
    const double term2 = 0.25 * w_p * w_p * (0.25 + 1.0 / w_mi);
    const double term3 = 0.5 * w_pp;
    const double g_density = term1 - term2 + term3;

    if (g_density < kButterflyDensityFloor) {
      on_violation(k, g_density);
    }
  }
}

// Skip-policy convenience for the three sites that ignore an unusable stencil.
// A forwarding one-liner, NOT a second statement of the rule.
template <class WOfK, class OnViolation>
void butterfly_density_scan(const WOfK &w_of_k, double k_min, double k_max,
                            std::uint32_t n_grid,
                            const OnViolation &on_violation) {
  butterfly_density_scan(w_of_k, k_min, k_max, n_grid,
                         ButterflyStencilPolicy::Skip, on_violation,
                         [](double) {});
}

}  // namespace atx::vol::detail
