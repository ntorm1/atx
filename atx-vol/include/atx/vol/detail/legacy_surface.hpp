#pragma once

// Legacy per-family fitted-surface container: `Surface<Slice>` over the minimal
// `SviSlice` / `EssviSlice` evaluators.
//
// DEMOTED (S4-T21 / plan 4.4). atx-vol grew four hand-duplicated "ascending-T
// stack of fitted slices + linear-in-total-variance time interpolation"
// containers before `CurveSurface` unified them (see the `vol_curve.hpp` file
// header). The canonical pipeline is now CurveSurface (fit) -> PricedSurface /
// PricedSurfaceView (serve) -> SurfaceSet (portfolio); this container is one of
// the leftovers and lives here, in `detail/`, with no stability promise.
//
// It is NOT deleted: `surface.hpp`'s two closed-form evaluators are the direct
// port of the C's `ats_vol_surface.c` time interpolation, and this container is
// the only place that behaviour is exercised (surface_test.cpp), plus it is the
// surface type the `derivatives.hpp` templated fair-strike entries were written
// against. New code uses the canonical pipeline. Internal code may keep using
// this; a public header may not NAME it — `vol_umbrella_test.cpp`
// (DemotedSurfaceContainersAreNotNamedInPublicHeaders) enforces that.
//
// Thread-safety (unchanged): a plain value type with no state shared across
// instances. Concurrent reads (w/iv queries) against the same instance from
// multiple threads are safe. set_slice() mutates and must not be called
// concurrently with any other access to the same instance — this mirrors the
// C's documented "many readers OR one writer" contract for AtsVolSurface.

#include <cstddef>
#include <vector>

#include "atx/vol/surface.hpp"  // SviSlice, EssviSlice, svi_w, essvi_w
#include "atx/vol/types.hpp"

namespace atx::vol {

// ── Surface: fixed-capacity cache of fitted per-slice params ────────────
//
// `Slice` is SviSlice or EssviSlice (the only two instantiations provided
// by surface.cpp). Slots are addressed 0..capacity()-1; `capacity` mirrors
// the C's `ats_vol_surface_create(..., cap_slices)` arena preallocation.
// The active slice count grows to the highest index written by
// set_slice(), exactly as the C's `n_slices` high-water mark does.
//
// Precondition (not verified — matches the C, which documents but does not
// itself check this): slices must be written in ascending-T order for the
// time interpolation below to be well-defined.
//
// Extrapolation in T is never permitted: a query whose T exceeds the longest
// slice, or sits more than 50% below the shortest slice's T, returns NaN by
// design rather than silently fabricating a sigma (see the Sprint 26 note in
// the C's ats_vol_surface.c — this guard exists because silent short-T
// extrapolation historically produced multi-hundred-percent phantom vols when
// short-dated slices were starved of fit data).
template <class Slice>
class Surface {
 public:
  // `cap_slices` must be > 0; capacity is fixed for the life of the object.
  explicit Surface(std::size_t cap_slices) : slices_(cap_slices) {}

  // Write the slice at `idx`, growing the active slice count if `idx` is
  // at or past the current high-water mark. Returns ErrorCode::OutOfRange
  // if `idx >= capacity()` (mirrors the C's ATS_VOL_ERR_INVALID on a
  // cap_slices overrun in ats_vol_surface_set_slice_{svi,essvi}).
  [[nodiscard]] Status set_slice(std::size_t idx, const Slice& slice);

  [[nodiscard]] std::size_t n_slices() const noexcept { return n_slices_; }
  [[nodiscard]] std::size_t capacity() const noexcept {
    return slices_.size();
  }

  // Total variance w = sigma^2 * T at (k_log, T), linearly interpolated in
  // w across the two bracketing slices' T. T is floored to kTMinEval
  // before bracketing/interpolating (matches the C).
  //
  // Returns NaN when: there are no slices; T (after the kTMinEval floor)
  // sits more than 50% below the first slice's T; or T exceeds the last
  // slice's T. Querying exactly at a slice's T (including the first or
  // last) evaluates that slice directly with no interpolation.
  [[nodiscard]] double w(double k_log, double T) const noexcept;

  // Implied vol sigma = sqrt(w(k_log, T) / T) — note T here is the
  // caller's original argument, NOT the internally-floored value used by
  // w()'s bracket search (matches the C's ats_vol_surface_iv exactly,
  // including the degenerate case where an original T <= 0 divides a
  // finite w by a non-positive number). NaN wherever w() is NaN or the
  // interpolated w is non-finite / non-positive.
  [[nodiscard]] double iv(double k_log, double T) const noexcept;

 private:
  [[nodiscard]] static double eval_w(const Slice& slice,
                                     double k_log) noexcept;

  std::vector<Slice> slices_;
  std::size_t n_slices_ = 0;
};

using SviSurface = Surface<SviSlice>;
using EssviSurface = Surface<EssviSlice>;

extern template class Surface<SviSlice>;
extern template class Surface<EssviSlice>;

}  // namespace atx::vol
