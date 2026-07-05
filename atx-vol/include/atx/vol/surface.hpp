#pragma once

// Volatility-surface per-slice evaluators + time-axis interpolation.
//
// Ported from the C `ats-vol` library (ats_vol_svi.c, ats_vol_essvi.c,
// ats_vol_surface.c / ats_vol_surface.h). Two closed-form per-slice
// parametrizations of total variance w(k) = sigma^2 * T:
//
//   Raw SVI (Gatheral form):
//     w(k) = a + b * (rho*(k-m) + sqrt((k-m)^2 + sigma^2))
//
//   eSSVI (Gatheral-Jacquier extended SSVI):
//     w(k) = (theta/2) * (1 + rho*phi*k + sqrt((phi*k+rho)^2 + (1-rho^2)))
//
// where k = log(K/F) is log-moneyness.
//
// A Surface<Slice> caches a small, fixed-capacity set of fitted slices
// sorted by ascending T and answers w(k, T) / sigma(k, T) queries by
// interpolating LINEARLY IN TOTAL VARIANCE across the two bracketing
// slices — never in sigma directly, which avoids the sigma-coordinate
// blow-up at small T (matches industry practice; see the C's
// ats_vol_surface.c file header).
//
// Extrapolation in T is never permitted: a query whose T exceeds the
// longest slice, or sits more than 50% below the shortest slice's T,
// returns NaN by design rather than silently fabricating a sigma (see the
// Sprint 26 note in the C's ats_vol_surface.c — this guard exists because
// silent short-T extrapolation historically produced multi-hundred-percent
// phantom vols when short-dated slices were starved of fit data).
//
// Scope note: this port covers ONLY the two per-slice evaluators and the
// surface time interpolation. Calibration (IRLS/LM/Newton fitting),
// arbitrage repair/projection, the Mingone cube reparametrization, the
// mmap surface archive, and the C8/CStar parametrizations are out of
// scope — see the porting task notes / atx-vol/README.md. The eSSVI slice
// here is deliberately the base 3-parameter Gatheral-Jacquier form only:
// the Sprint-15 asymmetric-rho blend (rho_R/rho_scale) and the Sprint-11/12
// wing residual (resid_coef/resid_scale/basis) are calibration-adjacent
// extensions layered on top of it and are not ported.
//
// Thread-safety: Surface<Slice> is a plain value type with no state shared
// across instances. Concurrent reads (w/iv queries) against the same
// instance from multiple threads are safe. set_slice() mutates and must
// not be called concurrently with any other access to the same instance
// — this mirrors the C's documented "many readers OR one writer" contract
// for AtsVolSurface.

#include <cstddef>
#include <vector>

#include "atx/vol/types.hpp"

namespace atx::vol {

// ── Raw SVI (Gatheral) per-slice parameters ─────────────────────────────

// w(k) = a + b * (rho*(k-m) + sqrt((k-m)^2 + sigma^2)).
//
// `T` (year-fraction to expiry) is carried on the slice only so a Surface
// can time-interpolate across slices; svi_w() itself is a pure function of
// (slice shape params, k_log) and does not read T.
struct SviSlice {
  double a{};
  double b{};
  double rho{};
  double m{};
  double sigma{};
  double T{};
};

// Raw-SVI total variance at log-moneyness `k_log`. Closed-form, ~5 FLOPs.
// No domain restrictions on the inputs are enforced here (matches the C,
// which is a bare arithmetic evaluator with no validation) — a slice with
// sigma == 0 and k_log == m yields w == a, same as the C.
[[nodiscard]] double svi_w(const SviSlice& slice, double k_log) noexcept;

// ── eSSVI (Gatheral-Jacquier) per-slice parameters ──────────────────────

// w(k) = (theta/2) * (1 + rho*phi*k + sqrt((phi*k+rho)^2 + (1-rho^2))).
//
// theta = ATM total variance (> 0), phi = curvature (> 0), rho = skew,
// in (-1, 1). `T` is carried for the same reason as SviSlice::T above.
struct EssviSlice {
  double theta{};
  double phi{};
  double rho{};
  double T{};
};

// eSSVI total variance at log-moneyness `k_log`. Closed-form, ~10 FLOPs.
// Equivalent to the C's `ats_vol_essvi_w_backbone`, and bit-identical to
// `ats_vol_essvi_w` whenever the C slice's `resid_scale == 0` (the wing
// residual is not modeled here — see the file header note).
[[nodiscard]] double essvi_w(const EssviSlice& slice, double k_log) noexcept;

// Gradient of eSSVI total variance with respect to (theta, phi, rho) at
// fixed k_log — the closed-form ingredient the C's IRLS/Newton calibrator
// consumes (the calibrator itself is not ported; the gradient is exposed
// because the C evaluator file exposes it as a standalone closed form).
struct EssviGrad {
  double dtheta;
  double dphi;
  double drho;
};
[[nodiscard]] EssviGrad essvi_w_grad(const EssviSlice& slice,
                                     double k_log) noexcept;

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
