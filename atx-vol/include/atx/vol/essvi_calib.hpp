#pragma once

// eSSVI per-slice calibrator + surface driver — the load-bearing volatility
// calibration path for atx-vol.
//
// Ported from the C `ats-vol` library (ats_calibrate_essvi.c: the per-slice
// cube-space Levenberg-Marquardt fitter `ats_vol_essvi_fit_observations`, the
// surface driver `ats_vol_essvi_calib_surface`, and the Mingone sequential
// driver `ats_calibrate_essvi_sequential.c`). The refactor to the atx house
// style (.agents/cpp/agent.md) routes failure through `Result`/`Status`
// (never the C's negative-integer `AtsVolStatus`), replaces the C's goto /
// fixed-point loops with bounded for-loops, and reuses the already-ported
// shared calibration infrastructure (`calib.hpp`), the eSSVI math
// (`vol_surface.hpp`), the robust IRLS helpers (`detail/robust.hpp`), the
// static-arb projections (`arb.hpp`), and the atx-core SPD linear solver.
//
// ── The optimizer ─────────────────────────────────────────────────────────
//
// The fit lives in the Mingone unit cube (psi, p, lambda) ∈ (0, 1)^3, which
// maps to natural (theta, phi, rho) through the Gatheral-Jacquier butterfly
// bound. Every slice produced by this cube-space LM is butterfly-arb-free by
// construction (Mingone 2022, arXiv:2204.00312; Gatheral-Jacquier 2014,
// arXiv:1204.0646). Cross-slice calendar arbitrage is handled by a post-fit
// projection on theta (`arb_project_calendar_essvi`).
//
// PORT NOTE — objective domain. The C's current (Sprint 13b) objective is a
// price-domain LM (B76(sigma_model) + American-correction vs the option mid).
// This port fits in the **total-variance (w) domain**:
//
//     minimize  L(psi, p, lambda) = Σ_i weight_i · (w_model(k_i) − w_mkt_i)²
//
// with w_model = essvi_backbone_w(cube → natural, k_i), the outer loop an IRLS
// Huber reweight, a Lee-bound projection + phi_max clamp per outer iteration,
// and a Morozov noise-floor stop. This is the objective the shared atx obs
// builder (`FitObs::w_mkt`) and eSSVI gradient (`essvi_w_grad3`) directly
// support, and the domain the wing-residual layer (`w_mkt − backbone`) is
// posed in. The American-correction-in-LM (Andersen-Lake) path, the AVX2 SoA
// kernels, the Fengler overlay, and three-way candidate selection are all
// deferred (see the PORT NOTEs in the source). The Jacobian is analytic:
// the natural-space eSSVI gradient (`essvi_w_grad3`) composed with the closed
// form of the Mingone cube → natural reparametrization Jacobian.
//
// ── Thread-safety ─────────────────────────────────────────────────────────
//
// `essvi_fit_slice` and `essvi_w_cube_grad` are pure reads of their inputs
// (no globals, no shared state) — safe to call concurrently. The surface
// drivers take exclusive ownership of the `VolSurface` they write for the
// call duration (the C's "many readers OR one writer" contract).

#include <array>
#include <span>

#include "atx/vol/calib.hpp"         // CalibOpts, FitObs, FitDiag
#include "atx/vol/curve.hpp"         // CurveSet
#include "atx/vol/types.hpp"         // Result, Status
#include "atx/vol/universe.hpp"      // Underlying, Chain
#include "atx/vol/vol_surface.hpp"   // EssviParams, VolSurface

namespace atx::vol {

// ── Per-slice cube-space Levenberg-Marquardt fit ─────────────────────────
//
// Fit one expiry's eSSVI slice to the observation set `obs` (survivors of the
// quote-filter cascade — build them with `build_observations`). Seeds the cube
// from the ATM total variance, iterates (psi, p, lambda) with an IRLS-Huber
// outer loop over an inner damped Gauss-Newton (Levenberg-Marquardt) loop,
// clamps each step onto the Lee/butterfly-admissible cone, and stops on the
// parameter-norm tolerance (or the Morozov noise floor when enabled).
//
// Returns the natural-form slice: theta/phi/rho plus the converged cube
// coordinates psi/p/lambda and T/F stamped in. When `opts.residual_disable`
// is false the additive wing-residual layer is fit on top of the backbone
// (ridge LS on `w_mkt − backbone`, HINGE_QUAD basis); see the PORT NOTE in the
// source for the deferred per-slice Roper projection.
//
// @param obs   surviving observations for this slice (>= 1; the surface driver
//              enforces the `min_obs_per_slice` floor upstream).
// @param T     year-fraction to expiry (> 0).
// @param F     forward at this expiry (stamped into the returned slice; the
//              w-domain fit itself does not read it).
// @param opts  calibration policy (iteration caps, Huber k, Lee/Morozov knobs).
// @param out_diag  optional; populated with the per-slice fit diagnostics.
// @param theta_floor  optional lower bound on ATM total variance theta (== w(0)).
//              0 (default) leaves the natural theta band untouched — byte-
//              identical to the historical signature. A positive value raises
//              the cube's theta_lo to at least `theta_floor` (capped just below
//              theta_hi), so the fit cannot place theta below it. Passing the
//              PREVIOUS slice's theta yields a calendar-monotone term structure
//              at the ATM level BY CONSTRUCTION (the floor binds only where the
//              raw fit would invert, so quality is preserved elsewhere). This is
//              the seam the calendar-monotone surface fit drives; it mirrors the
//              theta floor `essvi_calib_surface_sequential` applies internally.
// @param warm  optional warm-start seed — a PREVIOUS fit of (ideally) this same
//              expiry. When non-null the WHOLE Mingone cube (psi == level, p ==
//              curvature, lambda == skew) seeds from `warm`'s converged
//              coordinates instead of the cold seed (a crude band-ratio psi +
//              neutral p / lambda). A tick-to-quote refit thus starts essentially
//              at the prior optimum and converges in far fewer LM iterations at
//              the same fit quality — the incremental-update hot path. Null
//              (default) is byte-identical to the cold fit. Additionally, if
//              `opts.prior_strength > 0` a
//              Tikhonov term shrinks the cube toward `warm` (scaled to the
//              dataset weight), stabilising thin / noisy tick updates; with a
//              null `warm` that field is inert.
// @return InvalidArgument if `obs` is empty or T <= 0; Unavailable if the LM
//         produced a non-finite / degenerate slice; otherwise Ok(slice).
[[nodiscard]] Result<EssviParams> essvi_fit_slice(
    std::span<const FitObs> obs, double T, double F, const CalibOpts& opts,
    FitDiag* out_diag = nullptr, double theta_floor = 0.0,
    const EssviParams* warm = nullptr);

// Gradient of the eSSVI backbone total variance w.r.t. the Mingone cube
// coordinates ∂w/∂(psi, p, lambda) at log-moneyness `k_log`, evaluated at the
// slice's stored (theta, phi, rho) and cube (psi, p, lambda) on the DEFAULT
// Mingone theta band derived from `slice.T`. This is the analytic Jacobian the
// per-slice LM consumes, exposed for calibration diagnostics and testing.
//
// Precondition (documented, not verified): the slice's (theta, phi, rho) are
// consistent with its (psi, p, lambda) under the default band — i.e. the slice
// came from `essvi_reparam_to_natural` / a fit. Returned as {dw/dpsi, dw/dp,
// dw/dlambda}.
[[nodiscard]] std::array<double, 3> essvi_w_cube_grad(const EssviParams& slice,
                                                      double k_log) noexcept;

// ── Surface driver (per-slice, calendar-projected) ───────────────────────
//
// Loop the underlier's chains in ascending-T order; for each resolve (F, T,
// df) from the curve set, build the observation set, fit the slice, and write
// it into `surface` (which must be eSSVI-parametrized). After all slices are
// fit, run the backbone calendar projection (`arb_project_calendar_essvi`) so
// the surface is calendar-arb-free. Populates `surface`'s diagnostics
// (rmse_vol, n_quotes_used/dropped) and, if provided, `out_diag`.
//
// @param prior  optional previously-fit surface (e.g. the prior snapshot's
//              calibration) used to warm-start each slice's fit. For a slice
//              being fit at maturity T, the prior slice minimizing |T_prior -
//              T| is used as the warm seed (see `essvi_fit_slice`'s `warm`
//              param) iff |T_prior - T| <= kWarmPriorMaxTenorGap (5 calendar
//              days in year-fraction units — cross-snapshot T drifts intraday
//              and across a few days; beyond that a stale seed is worse than
//              cold) AND that prior slice is a usable eSSVI fit (finite
//              theta/psi/p/lambda and theta > 0). No match, an empty/null
//              `prior`, or a non-eSSVI `prior` surface falls back to the cold
//              seed exactly as when `prior` is null. Null (default) is
//              byte-identical to the historical behavior — nothing about the
//              fit changes, only the LM's starting point.
// @param n_workers  PERF-ONLY thread count for the per-expiry chain fan-out.
//              The chains are fit independently (a two-phase driver: a parallel
//              per-chain fit, then a serial in-order reduction that writes the
//              surface + FitDiag), so the worker count is a pure performance
//              knob: fitted params, slice order/count, Status codes, and FitDiag
//              are BIT-IDENTICAL for every worker count and vs the serial path.
//              0 (default) resolves to `atx_auto_worker_count()` (honors the
//              ATX_VOL_FIT_WORKERS env cap, else hardware_concurrency); 1 forces
//              the serial path. Values above the chain count are clamped down.
//              The calib_pool per-name fan-out passes 1 here to avoid nesting a
//              second fan-out under it (oversubscription).
// @return InvalidArgument if `surface` is not eSSVI-parametrized; NotFound if
//         the underlier has no chains or not a single slice fit; otherwise Ok.
[[nodiscard]] Status essvi_calib_surface(VolSurface& surface,
                                         const Underlying& under,
                                         const CurveSet& curves,
                                         const CalibOpts& opts,
                                         FitDiag* out_diag = nullptr,
                                         const VolSurface* prior = nullptr,
                                         unsigned n_workers = 0);

// Mingone sequential surface driver. Identical chain walk to
// `essvi_calib_surface`, but each slice is fit with a theta floor equal to the
// previous slice's theta (theta_0 floored at the band minimum), so the fitted
// term structure is theta-monotone by construction — no post-fit calendar
// projection of the ATM level is needed. See the PORT NOTE in the source for
// the (φ, ρ)-wing coupling the θ-floor alone does not remove.
//
// @param prior  same warm-start contract as `essvi_calib_surface`'s `prior`.
// @return same contract as `essvi_calib_surface`.
[[nodiscard]] Status essvi_calib_surface_sequential(VolSurface& surface,
                                                    const Underlying& under,
                                                    const CurveSet& curves,
                                                    const CalibOpts& opts,
                                                    FitDiag* out_diag = nullptr,
                                                    const VolSurface* prior = nullptr);

}  // namespace atx::vol
