#pragma once

// Legacy per-family fitted-surface container: `C8Surface` over `C8Params`.
//
// DEMOTED (S4-T21 / plan 4.4). One of the four hand-duplicated "ascending-T
// stack of fitted slices + linear-in-total-variance time interpolation"
// containers that predate `CurveSurface` (see the `vol_curve.hpp` file header).
// The canonical pipeline is CurveSurface (fit) -> PricedSurface /
// PricedSurfaceView (serve) -> SurfaceSet (portfolio); this one lives in
// `detail/` with no stability promise. The C8 EVALUATORS stay public in
// `c8.hpp` — the arb validator and the curve registry name them; only the
// container moved.
//
// Internal code may keep using this; a public header may not NAME it —
// `vol_umbrella_test.cpp` (DemotedSurfaceContainersAreNotNamedInPublicHeaders)
// enforces that.
//
// Thread-safety (unchanged): a plain value type; concurrent reads (w / iv)
// against one instance are safe, and the `set_slice` mutator must not race any
// other access (the C "many readers OR one writer" contract).

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "atx/vol/c8.hpp"     // C8Params, c8_w
#include "atx/vol/types.hpp"  // Result, Status, kTMinEval

namespace atx::vol {

// ── C8Surface: standalone fitted-surface container ────────────────────────
//
// Holds one surface's C8 slices, sorted by ascending T, and answers w(k, T) /
// iv(k, T) by LINEAR-IN-TOTAL-VARIANCE time interpolation across the two
// bracketing slices — mirroring `VolSurface`'s blend logic and its Sprint-26
// no-extrapolation guards (a query past the longest slice, or more than 50%
// below the shortest, returns NaN).
//
// Precondition (documented, not verified — matches the C): slices are written
// in ascending-T order.
class C8Surface {
 public:
  // Construct an empty surface for `cap_slices` slices (reserved). Returns
  // InvalidArgument when cap_slices == 0.
  [[nodiscard]] static Result<C8Surface> create(std::uint32_t uid,
                                                std::size_t cap_slices);

  // Write the slice at `idx`, growing the active count to idx+1 when idx is at
  // or past the current high-water mark. OutOfRange if idx >= capacity().
  [[nodiscard]] Status set_slice(std::size_t idx, const C8Params& slice);

  [[nodiscard]] std::uint32_t uid() const noexcept { return uid_; }
  [[nodiscard]] std::size_t n_slices() const noexcept { return slices_.size(); }
  [[nodiscard]] std::size_t capacity() const noexcept { return cap_slices_; }
  [[nodiscard]] std::span<const C8Params> slices() const noexcept {
    return slices_;
  }

  // Total variance w = sigma^2*T at (k_log, T), linear-in-w across the two
  // bracketing slices. T floored to kTMinEval for bracketing. NaN when there
  // are no slices, when T exceeds the last slice's T, or when T (post-floor)
  // sits more than 50% below the first slice's T.
  [[nodiscard]] double w(double k_log, double T) const noexcept;

  // Implied vol sqrt(w(k_log, T) / T), dividing by the CALLER's un-floored T
  // (matches the C). NaN wherever w() is NaN / non-positive.
  [[nodiscard]] double iv(double k_log, double T) const noexcept;

 private:
  // Constructed only via create(); Rule of Zero otherwise (movable/copyable).
  C8Surface() = default;

  std::uint32_t uid_{};
  std::vector<C8Params> slices_{};
  std::size_t cap_slices_{};
};

}  // namespace atx::vol
