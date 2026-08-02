#pragma once

// Volatility-surface per-slice closed-form evaluators.
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
// Scope note: this header ships ONLY the two per-slice evaluators (plus the
// eSSVI gradient). The legacy per-family container that stacked these slices
// by ascending T was demoted to `detail/legacy_surface.hpp` by S4-T21 (plan
// 4.4): the canonical pipeline is CurveSurface (fit) -> PricedSurface /
// PricedSurfaceView (serve) -> SurfaceSet (portfolio). Calibration
// (IRLS/LM/Newton fitting), arbitrage repair/projection, the Mingone cube
// reparametrization, the mmap surface archive, and the C8/CStar
// parametrizations are out of scope — see the porting task notes /
// atx-vol/README.md. The eSSVI slice here is deliberately the base
// 3-parameter Gatheral-Jacquier form only: the Sprint-15 asymmetric-rho
// blend (rho_R/rho_scale) and the Sprint-11/12 wing residual
// (resid_coef/resid_scale/basis) are calibration-adjacent extensions
// layered on top of it and are not ported.
//
// Thread-safety: every entry here is a pure function of its arguments over
// plain value types — safe to call concurrently from any number of threads.

#include "atx/vol/types.hpp"

namespace atx::vol {

// ── Raw SVI (Gatheral) per-slice parameters ─────────────────────────────

// w(k) = a + b * (rho*(k-m) + sqrt((k-m)^2 + sigma^2)).
//
// `T` (year-fraction to expiry) is carried on the slice only so a container
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

}  // namespace atx::vol
