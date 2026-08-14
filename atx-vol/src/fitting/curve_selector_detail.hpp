#pragma once

// Curve-selector internals not part of the supported API surface. Split out
// of the public curve_selector.hpp API surface (Task 4, atx-vol API
// restructure): `slice_butterfly_violations` is defined in curve_selector.cpp
// and used only there and by curve_selector_test.cpp's direct unit-test seam,
// never by any other production TU.

#include <cstdint>

namespace atx::vol {

class IVolCurve;

namespace detail {

// Per-kind butterfly violation count for a fitted slice — the exact
// selection-time mapping `select_curve` applies to every candidate's fitted
// slice (closed-form Martini-Mingone for raw-SVI, grid Durrleman g-check for
// C8, the fitted diagnostic count carried on the params for SplineVol, and 0
// for the by-construction / out-of-scope kinds). Defined in curve_selector.cpp
// and exposed here (rather than staying an anonymous-namespace helper) purely
// as a unit-test seam — production code should go through `select_curve`,
// never call this directly. `k_lo`/`k_hi` bound the C8 grid (padded by the
// caller).
[[nodiscard]] std::uint32_t slice_butterfly_violations(const IVolCurve &cv, double T, double k_lo,
                                                       double k_hi) noexcept;

} // namespace detail

} // namespace atx::vol
