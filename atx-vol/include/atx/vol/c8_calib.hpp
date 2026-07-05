#pragma once

// C8 calibrator — the sequential per-slice IRLS-Newton (Levenberg-Marquardt)
// fit over the 8-DoF vector (v, psi, p, c, v_min, kappa, q_L, q_R).
//
// Ported from the C `ats-vol` library (ats_calibrate_c8.c). The optimizer works
// in a reparametrized x-space that keeps the JW box a simple product of ranges
// (log v, psi, log p, log c, inv_sigmoid(v_min/v), kappa, q_L, q_R); the
// v_min-depends-on-v coupling enters as a cross-term in column 0 of the x-space
// Jacobian rather than by reparametrizing v_min. Each Newton step solves the
// damped normal equations (H + lambda*diag(H))·dx = -g via the shared SPD
// solver `atx::core::linalg::solve_spd` (the C hand-rolled an 8x8 Cholesky).
// The outer IRLS loop reweights with the q90-anchored Huber weights from
// `atx/vol/detail/robust.hpp` (k = 1.345). After the fit the bumps are damped
// by the Roper no-arb projection and a per-slice quality gate reverts to the
// seed if C8 did not improve on it.
//
// ── Scope of this port ────────────────────────────────────────────────────
//
// PORT NOTE — the C shipped TWO residual domains: a vol-domain path (mid in
// total-variance units) used by its unit tests, and a price-domain path that
// prices each obs through Black-76 + an Andersen-Lake American correction and
// scales by the half-spread. This port implements the VOL-DOMAIN path only
// (the numerically load-bearing core the recovery + capability tests exercise).
// The price-domain B76+AL residual and the surface-level orchestration that
// builds an eSSVI seed surface first (ats_vol_c8_calib_surface) are deferred:
// the eSSVI *calibrator* and the Andersen-Lake correction cache are not part of
// the ported atx-vol subset, so a faithful reproduction is not yet possible.
// `c8_calib_slice` below therefore anchors on the vol-domain (total-variance)
// targets that `build_observations` yields.

#include <array>
#include <cstddef>
#include <span>

#include "atx/vol/c8.hpp"        // C8Params, c8_slice_w
#include "atx/vol/calib.hpp"     // CalibOpts, FitDiag, build_observations
#include "atx/vol/types.hpp"     // Result, Status
#include "atx/vol/universe.hpp"  // Chain

namespace atx::vol {

// ── x-space reparametrization (pack / unpack) ─────────────────────────────
//
// x-layout: x[0]=log(v), x[1]=psi, x[2]=log(p), x[3]=log(c),
//           x[4]=inv_sigmoid(v_min/v), x[5]=kappa, x[6]=q_L, x[7]=q_R.

[[nodiscard]] std::array<double, 8> c8_pack(const C8Params& s) noexcept;

// Overwrite the 8 fitted fields of `s` from the x-vector (T is passed for API
// symmetry with the C; the backbone conversion needs no T here).
void c8_unpack(const std::array<double, 8>& x, double T, C8Params& s) noexcept;

// ── Vol-domain residual SSE (test / diagnostic API) ───────────────────────
//
// Sum over i of ((w_model(k_i) - mid_i) / max(half_spread_i, eps_floor))^2,
// with `mid` supplied in total-variance units. Spans must be the same length;
// a length mismatch yields 0.
[[nodiscard]] double c8_residual_sse(const C8Params& s, std::span<const double> k,
                                     std::span<const double> mid,
                                     std::span<const double> half_spread,
                                     double eps_floor) noexcept;

// ── Per-slice vol-domain LM fit ───────────────────────────────────────────
//
// Fit `s` to the vol-domain observations (mid, spread in total-variance units)
// with `max_inner_iters` Levenberg-Marquardt steps, then apply the Roper no-arb
// projection. Mirrors the C `ats_vol_c8_fit_slice_lm` vol-domain path.
//
// @return InvalidArgument if the spans are empty / mismatched or
//         max_inner_iters <= 0; otherwise Ok.
[[nodiscard]] Status c8_fit_slice_lm(C8Params& s, std::span<const double> k,
                                     std::span<const double> mid,
                                     std::span<const double> spread,
                                     int max_inner_iters, double eps_floor);

// ── Per-slice quality gate ────────────────────────────────────────────────
//
// Revert `slice` to `seed` (with bumps disabled) unless the C8 fit improved on
// the seed. Tolerance contract (matches the C):
//   tol  < 0 : unconditional revert (LM soft-fail signal);
//   tol == 0 : default 1.05;
//   tol  > 0 : revert when slice.rmse_price > tol * seed_rmse_price.
// A non-finite RMSE or non-finite/inadmissible slice parameter also reverts.
//
// @return true when the seed was restored, false when the C8 fit was kept.
[[nodiscard]] bool c8_apply_quality_gate(C8Params& slice, const C8Params& seed,
                                         double seed_rmse_price,
                                         double tolerance) noexcept;

// ── Chain-level slice calibration (uses build_observations) ───────────────

// The result of calibrating one chain's slice: the fitted parameters and the
// per-slice fit diagnostics.
struct C8SliceFit {
  C8Params params{};
  FitDiag diag{};
};

// Calibrate one slice from a live chain. Builds the observation set with the
// shared `build_observations` (applying the standard quote-filter cascade),
// then drives the vol-domain IRLS-Huber Newton fit from `seed`, damps the bumps
// with the Roper projection, and applies the quality gate against the seed.
//
// @return the seed's build_observations error (NotFound when fewer than 5 rows
//         survive, InvalidArgument on a malformed chain / non-positive F,T);
//         otherwise Ok with the fitted slice and diagnostics.
[[nodiscard]] Result<C8SliceFit> c8_calib_slice(const C8Params& seed,
                                                const Chain& chain, double F,
                                                double T, double df,
                                                const CalibOpts& opts);

}  // namespace atx::vol
