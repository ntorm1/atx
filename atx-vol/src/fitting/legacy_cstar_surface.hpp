#pragma once

// Legacy per-family fitted-surface container: `CStarSurface` over `CStarParams`.
//
// DEMOTED (S4-T21 / plan 4.4). One of the four hand-duplicated "ascending-T
// stack of fitted slices + linear-in-total-variance time interpolation"
// containers that predate `CurveSurface` (see the `vol_curve.hpp` file header).
// The canonical pipeline is CurveSurface (fit) -> PricedSurface /
// PricedSurfaceView (serve) -> SurfaceSet (portfolio); this one lives in
// `detail/` with no stability promise. The CStar EVALUATORS and the calendar
// projection stay public in `cstar.hpp`; only the container moved.
//
// Internal code may keep using this; a public header may not NAME it —
// `vol_umbrella_test.cpp` (DemotedSurfaceContainersAreNotNamedInPublicHeaders)
// enforces that.
//
// Thread-safety (unchanged): a plain value type with no cross-instance shared
// state; concurrent reads (w / iv / slices) against one instance are safe, and
// the mutators (set_slice, mutable_slices + calendar projection) must not run
// concurrently with any other access (the C's "many readers OR one writer"
// contract).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "fitting/cstar.hpp"  // CStarParams, cstar_w
#include "atx/vol/api/core/types.hpp"  // Result, Status, kTMinEval

namespace atx::vol {

// ── Standalone surface container ───────────────────────────────────────────
//
// Holds CStar slices sorted by ascending T. Answers w/iv by linear-in-total-
// variance time interpolation with the same Sprint-26 no-extrapolation guards
// as `VolSurface` (query past the longest slice, or > 50% below the shortest,
// returns NaN). Precondition (documented, not verified — matches the C):
// slices are written in ascending-T order.
class CStarSurface {
 public:
  // Construct an empty surface with capacity for `cap_slices` slices.
  // InvalidArgument if cap_slices == 0.
  [[nodiscard]] static Result<CStarSurface> create(std::uint32_t uid,
                                                   std::size_t cap_slices);

  // Write the slice at `idx`, growing the active count to idx+1 if at/past the
  // high-water mark. OutOfRange if idx >= capacity().
  [[nodiscard]] Status set_slice(std::size_t idx, const CStarParams& slice);

  [[nodiscard]] std::uint32_t uid() const noexcept { return uid_; }
  [[nodiscard]] std::size_t n_slices() const noexcept { return slices_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return cap_slices_; }

  [[nodiscard]] std::span<const CStarParams> slices() const noexcept {
    return slices_;
  }
  // Mutable view (for the calendar projection / in-place repair).
  [[nodiscard]] std::span<CStarParams> mutable_slices() noexcept {
    return slices_;
  }

  // Total variance at (k_log, T), linear-in-w across the two bracketing slices.
  [[nodiscard]] double w(double k_log, double T) const noexcept;
  // Implied vol sqrt(w / T), dividing by the caller's un-floored T.
  [[nodiscard]] double iv(double k_log, double T) const noexcept;

 private:
  CStarSurface() = default;  // constructed only via create(); Rule of Zero

  std::uint32_t uid_{};
  std::vector<CStarParams> slices_{};
  std::size_t cap_slices_{};
};

}  // namespace atx::vol
