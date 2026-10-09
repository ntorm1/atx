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

// The selection-time butterfly verdict for ONE fitted slice.
//
// A bare count cannot say "not checked" — zero and clean are the same value —
// and that conflation is exactly what let `VolCurveKind::LinearVariance` be
// reported as arb-free by a branch whose own comment conceded no check had
// run. `decided` is the missing bit: false means the slice's geometry could
// not be decided, and a caller must treat it as NOT clean.
struct SliceButterflyVerdict {
  std::uint32_t n_violations{0u};
  bool decided{true};

  [[nodiscard]] constexpr bool clean() const noexcept {
    return decided && n_violations == 0u;
  }
};

// Per-kind butterfly verdict for a fitted slice — the exact selection-time
// mapping `select_curve` applies to every candidate's fitted slice
// (closed-form Martini-Mingone plus a served-range grid scan for raw-SVI, grid
// Durrleman g-check for C8, the exact kink tally for LinearVariance, the fitted
// diagnostic count carried on the params for SplineVol, and 0 for the
// by-construction kinds ConvexDense / eSSVI). Defined in curve_selector.cpp and
// exposed here (rather than staying an anonymous-namespace helper) purely as a
// unit-test seam — production code should go through `select_curve`, never call
// this directly. `k_lo`/`k_hi` bound the C8 grid (padded by the caller).
[[nodiscard]] SliceButterflyVerdict slice_butterfly_violations(const IVolCurve &cv, double T,
                                                               double k_lo, double k_hi) noexcept;

} // namespace detail

} // namespace atx::vol
