#include "atx/vol/essvi_calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/linalg/solve.hpp"   // solve_spd
#include "atx/vol/arb.hpp"             // arb_project_calendar_essvi, arb_check_total_surface_all
#include "atx/vol/deamer.hpp"          // DeAmOptions (opt-in de-Am route)
#include "atx/vol/detail/calib_shared.hpp"  // detail::outer_cap + shared LM constants
#include "atx/vol/detail/resid_basis.hpp"  // dense C2 residual basis (shared with hot-path eval)
#include "atx/vol/detail/robust.hpp"   // huber_weights_strided
#include "atx/vol/detail/parallel_for.hpp"    // parallel_for_dynamic, atx_auto_worker_count
#include "atx/vol/simd/essvi_batch.hpp"  // batched w + w-grad kernels (fit hot path)
#include "atx/vol/vol_surface.hpp"     // essvi_backbone_w, essvi_w_grad3, essvi_phi_max

// eSSVI per-slice cube-space Levenberg-Marquardt + surface drivers.
//
// See essvi_calib.hpp for the design and the objective-domain PORT NOTE. The
// file is organized as: (1) the Mingone reparam box constants + cube helpers;
// (2) the analytic cube Jacobian; (3) the LM step + Lee projection; (4) the
// per-slice fit core; (5) the optional wing-residual ridge LS; (6) the surface
// drivers.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;

namespace {

// ── Mingone reparam box (must match src/vol_surface.cpp) ─────────────────
constexpr double kRhoMax = 0.999;
constexpr double kPhiMin = 1.0e-4;
constexpr double kSigmaLow = 0.005;
constexpr double kSigmaHigh = 5.000;
constexpr double kDefaultT = 1.0 / 365.25;

// Keep the cube strictly interior so central differences and the reparam
// clamps never collapse a step onto a box face.
constexpr double kCubeEdge = 1.0e-6;

// The hot-path total-variance positivity net `essvi_total_w` applies (must match
// the 1e-12 floor in src/vol_surface.cpp, documented on the declaration in
// vol_surface.hpp). Total variance AT this value is clamped, not fitted: the
// evaluator substituted it for a non-positive backbone+residual sum. Anything at
// or below it is economically zero vol and must never be scored as a valid fit.
constexpr double kTotalWFloor = 1.0e-12;

// Below this year-fraction a chain is treated as expired and skipped by the
// surface driver (mirrors the C `T_MIN_FIT`: half an hour in year units).
constexpr double kTMinFit = 1.0 / (365.25 * 24.0 * 2.0);

// Surface-driver warm-start prior-slice matching tolerance: the closest prior
// slice by |T_prior - T| is used as the warm seed only within this gap (5
// calendar days in year-fraction units). Cross-snapshot T drifts intraday and
// across a few days; beyond this a stale seed is worse than a cold fit.
constexpr double kWarmPriorMaxTenorGap = 5.0 / 365.0;

// Task C-8: window + grid for the honest post-fit `opts.validate_no_arb`
// audit in `calib_surface_impl`. Deliberately the SAME (k_min, k_max, n_grid)
// the surface-driver tests independently certify clean via
// `arb_check_total_surface_all` (EssviCalibSurface.RecoversSyntheticSurface_
// WithinTolerance / EssviCalibSurfaceSequential.ProducesThetaMonotoneSurface),
// not the wider [-1.5, 1.5] the theta-project REPAIR above uses: at that
// width, per-slice-independent phi/rho fit noise in the unconstrained
// extrapolated wings can trip the tight (1e-12) calendar tolerance even on a
// genuinely clean board, which would make the audit a false-positive trap
// rather than an honest gate. [-0.5, 0.5] is comfortably past every synthetic
// fixture's quoted range while staying inside the region those fixtures are
// proven arb-free on.
constexpr double kNoArbAuditKMin = -0.5;
constexpr double kNoArbAuditKMax = 0.5;
constexpr std::uint32_t kNoArbAuditGrid = 64u;

// LM control constants — canonical values live in
// atx/vol/detail/calib_shared.hpp and are shared with the SVI-MM and CStar
// fitters (routed here so a value can never drift between calibrators).
using detail::kLambdaGrow;
using detail::kLambdaLmInit;
using detail::kLambdaLmMax;
using detail::kLambdaLmMin;
using detail::kLambdaShrink;
using detail::kLmInnerDefault;
using detail::kLmTrialCap;
using detail::kOuterStallSse;
using detail::kTolParamDefault;

[[nodiscard]] double eff_T(double T) noexcept { return (T > 0.0) ? T : kDefaultT; }

// The [theta_lo, theta_hi] band the cube's psi axis spans. The default band
// reproduces `essvi_reparam_to_natural` exactly; the sequential driver passes a
// raised `theta_lo` floor to force a theta-monotone term structure.
struct ThetaBand {
  double lo{0.0};
  double hi{0.0};
};

[[nodiscard]] ThetaBand default_band(double T) noexcept {
  const double te = eff_T(T);
  return {kSigmaLow * kSigmaLow * te, kSigmaHigh * kSigmaHigh * te};
}

// Cube -> natural on an explicit theta band. Equals `essvi_reparam_to_natural`
// when `band == default_band(T)`.
[[nodiscard]] EssviNatural cube_to_natural(double psi, double p, double lambda,
                                           const ThetaBand& band) noexcept {
  psi = std::clamp(psi, 0.0, 1.0);
  p = std::clamp(p, 0.0, 1.0);
  lambda = std::clamp(lambda, 0.0, 1.0);
  const double rho = -kRhoMax + 2.0 * kRhoMax * lambda;
  const double theta = band.lo + psi * (band.hi - band.lo);
  const double phi_hi = essvi_phi_max(theta, rho);
  const double phi = kPhiMin + p * (phi_hi - kPhiMin);
  return {theta, phi, rho};
}

// Build a bare backbone slice (theta/phi/rho + cube coords) from a cube point.
[[nodiscard]] EssviParams slice_from_cube(double psi, double p, double lambda,
                                          const ThetaBand& band,
                                          double T) noexcept {
  const EssviNatural n = cube_to_natural(psi, p, lambda, band);
  EssviParams s{};
  s.theta = n.theta;
  s.phi = n.phi;
  s.rho = n.rho;
  s.psi = psi;
  s.p = p;
  s.lambda = lambda;
  s.T = T;
  return s;
}

void clamp_cube(double& psi, double& p, double& lambda) noexcept {
  psi = std::clamp(psi, kCubeEdge, 1.0 - kCubeEdge);
  p = std::clamp(p, kCubeEdge, 1.0 - kCubeEdge);
  lambda = std::clamp(lambda, kCubeEdge, 1.0 - kCubeEdge);
}

// Weighted SSE of the w-domain residual Σ w_i (w_model(k_i) − w_mkt_i)².
// ── Warm-start cube prior (Tikhonov shrinkage toward a previous fit) ──────
//
// A soft pull of the cube (psi, p, lambda) toward a target cube — the previous
// fit's converged coordinates on a tick-to-quote refit. `strength` is already
// scaled to the dataset's total weight in `fit_core` (so `opts.prior_strength`
// reads as a fraction of the data's influence), meaning cube_sse / lm_step use
// it raw. A null `CubePrior*` (or strength <= 0) is a no-op — byte-identical to
// the historical fit. The penalty is added to BOTH the SSE (so the LM's
// accept/reject sees it) and the Gauss-Newton normal equations (so the step
// descends it), in the same factor-of-2-dropped convention the obs term uses.
struct CubePrior {
  double psi{0.0};
  double p{0.0};
  double lambda{0.0};
  double strength{0.0};  // >0 enables; already weight-scaled
};

// `kbuf` is the hoisted contiguous obs-k array (built once per fit); `wbuf` is
// reusable length-n scratch the batch kernel writes the model w into. One
// `essvi_backbone_w_batch` call replaces the per-obs scalar backbone eval; the
// band/prior/accumulation logic is unchanged.
[[nodiscard]] double cube_sse(std::span<const FitObs> obs,
                              std::span<const double> weights,
                              const ThetaBand& band, double T, double psi,
                              double p, double lambda,
                              std::span<const double> kbuf,
                              std::span<double> wbuf,
                              const CubePrior* prior = nullptr) noexcept {
  const EssviParams s = slice_from_cube(psi, p, lambda, band, T);
  const std::size_t n = obs.size();
  simd::essvi_backbone_w_batch(s, kbuf.data(), wbuf.data(), n);
  double acc = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double r = wbuf[i] - obs[i].w_mkt;
    acc += weights[i] * r * r;
  }
  if (prior != nullptr && prior->strength > 0.0) {
    const double dpsi = psi - prior->psi;
    const double dp = p - prior->p;
    const double dl = lambda - prior->lambda;
    acc += prior->strength * (dpsi * dpsi + dp * dp + dl * dl);
  }
  return acc;
}

// ── Analytic cube Jacobian ───────────────────────────────────────────────
//
// dw/dcube = ∂w/∂θ·dθ/dcube + ∂w/∂φ·dφ/dcube + ∂w/∂ρ·dρ/dcube, with the
// natural-space gradient from `essvi_w_grad3` and the Mingone reparam Jacobian
// in closed form:
//   dθ/dψ = (θ_hi − θ_lo)               dθ/dp = dθ/dλ = 0
//   dρ/dλ = 2·kRhoMax                    dρ/dψ = dρ/dp = 0
//   φ = φ_min + p·(φ_hi(θ,ρ) − φ_min)
//     dφ/dp = φ_hi − φ_min
//     dφ/dψ = p·dφ_hi/dθ·(dθ/dψ)
//     dφ/dλ = p·dφ_hi/dρ·(dρ/dλ)
// φ_hi = min(4/s, 2/√s), s = θ(1+|ρ|).
struct CubeGradFactors {
  double d_theta_psi{0.0};    // dθ/dψ
  double dphihi_dtheta{0.0};  // dφ_hi/dθ
  double dphihi_drho{0.0};    // dφ_hi/dρ
  double phi_hi{0.0};         // φ_hi(θ, ρ)
  double drho_dlambda{0.0};   // dρ/dλ
  double p{0.0};              // cube p (needed by dφ/dψ, dφ/dλ)
};

[[nodiscard]] CubeGradFactors cube_grad_factors(double theta, double rho,
                                                double p,
                                                const ThetaBand& band) noexcept {
  CubeGradFactors f{};
  f.d_theta_psi = band.hi - band.lo;
  f.drho_dlambda = 2.0 * kRhoMax;
  f.phi_hi = essvi_phi_max(theta, rho);
  f.p = p;

  const double ar = std::fabs(rho);
  const double sgn = (rho > 0.0) ? 1.0 : ((rho < 0.0) ? -1.0 : 0.0);
  const double s = theta * (1.0 + ar);
  double dphihi_ds = 0.0;
  if (s > 0.0) {
    // φ_hi switches branch at s == 4 (both give φ_hi == 1 there). Pick the
    // active branch; the measure-zero kink is irrelevant to the LM.
    dphihi_ds = (s <= 4.0) ? (-1.0 / (s * std::sqrt(s)))  // d(2 s^-1/2)/ds
                           : (-4.0 / (s * s));            // d(4/s)/ds
  }
  f.dphihi_dtheta = dphihi_ds * (1.0 + ar);
  f.dphihi_drho = dphihi_ds * (theta * sgn);
  return f;
}

// ∂w/∂(ψ, p, λ) from the natural gradient `g == {∂w/∂θ, ∂w/∂φ, ∂w/∂ρ}` and the
// reparam factors.
[[nodiscard]] std::array<double, 3> cube_grad_from(const std::array<double, 3>& g,
                                                   const CubeGradFactors& f) noexcept {
  const double dw_psi = f.d_theta_psi * (g[0] + g[1] * f.p * f.dphihi_dtheta);
  const double dw_p = g[1] * (f.phi_hi - kPhiMin);
  const double dw_lambda =
      f.drho_dlambda * (g[1] * f.p * f.dphihi_drho + g[2]);
  return {dw_psi, dw_p, dw_lambda};
}

// Residuals + analytic Jacobian columns at the current cube point, single pass.
// The per-obs natural w + gradient come from ONE fused batch kernel call over
// the hoisted `kbuf` (writing model w into `wbuf` and the three natural partials
// into `dth`/`dphi`/`drho` scratch); the residual and the scalar cube-space
// gradient map (`cube_grad_from`) are then formed per obs exactly as before.
void residuals_and_jac(std::span<const FitObs> obs, const ThetaBand& band,
                       double T, double psi, double p, double lambda,
                       std::span<const double> kbuf, std::span<double> wbuf,
                       std::span<double> dth, std::span<double> dphi,
                       std::span<double> drho, std::span<double> r,
                       std::span<double> jpsi, std::span<double> jp,
                       std::span<double> jl) noexcept {
  const EssviNatural nat = cube_to_natural(psi, p, lambda, band);
  EssviParams s{};
  s.theta = nat.theta;
  s.phi = nat.phi;
  s.rho = nat.rho;
  s.T = T;
  const CubeGradFactors f = cube_grad_factors(nat.theta, nat.rho, p, band);
  const std::size_t n = obs.size();
  simd::essvi_backbone_w_grad_batch(s, kbuf.data(), wbuf.data(), dth.data(),
                                    dphi.data(), drho.data(), n);
  for (std::size_t i = 0; i < n; ++i) {
    r[i] = wbuf[i] - obs[i].w_mkt;
    const std::array<double, 3> dw =
        cube_grad_from({dth[i], dphi[i], drho[i]}, f);
    jpsi[i] = dw[0];
    jp[i] = dw[1];
    jl[i] = dw[2];
  }
}

// ── LM step ──────────────────────────────────────────────────────────────
//
// One Levenberg-Marquardt step on the cube: H = JᵀWJ (LM-damped on the
// diagonal), g = JᵀWr, solve (H_damped) δ = −g via the atx-core SPD Cholesky,
// accept on SSE decrease (else grow the damping and retry). Returns the new
// SSE, `prev_sse` if no trial improved, or −1 on an unrecoverable solve.
[[nodiscard]] double lm_step(std::span<const FitObs> obs,
                             std::span<const double> weights,
                             const ThetaBand& band, double T, double& psi,
                             double& p, double& lambda, double prev_sse,
                             double& lambda_lm, std::span<const double> kbuf,
                             std::span<double> wbuf, std::span<double> dth,
                             std::span<double> dphi, std::span<double> drho,
                             std::span<double> r, std::span<double> jpsi,
                             std::span<double> jp, std::span<double> jl,
                             const CubePrior* prior = nullptr) {
  residuals_and_jac(obs, band, T, psi, p, lambda, kbuf, wbuf, dth, dphi, drho, r,
                    jpsi, jp, jl);

  double h00 = 0.0, h01 = 0.0, h02 = 0.0, h11 = 0.0, h12 = 0.0, h22 = 0.0;
  double g0 = 0.0, g1 = 0.0, g2 = 0.0;
  for (std::size_t i = 0; i < obs.size(); ++i) {
    const double wi = weights[i];
    const double j0 = jpsi[i];
    const double j1 = jp[i];
    const double j2 = jl[i];
    const double ri = r[i];
    h00 += wi * j0 * j0;
    h01 += wi * j0 * j1;
    h02 += wi * j0 * j2;
    h11 += wi * j1 * j1;
    h12 += wi * j1 * j2;
    h22 += wi * j2 * j2;
    g0 += wi * j0 * ri;
    g1 += wi * j1 * ri;
    g2 += wi * j2 * ri;
  }
  // Warm-start prior: a diagonal Tikhonov term pulling the cube toward the
  // target. Contributes `strength` to each Hessian diagonal and
  // `strength·(x−x0)` to the gradient (the factor-of-2 the obs term drops).
  if (prior != nullptr && prior->strength > 0.0) {
    const double s = prior->strength;
    h00 += s;
    h11 += s;
    h22 += s;
    g0 += s * (psi - prior->psi);
    g1 += s * (p - prior->p);
    g2 += s * (lambda - prior->lambda);
  }

  // FT-P: reuse the 3x3 damped-normal-matrix / rhs storage across damping trials
  // (and across lm_step calls) instead of allocating MatX(3,3)/VecX(3) every
  // trial. thread_local => race-free under the parallel per-slice fit fan-out;
  // both are fully overwritten each trial, so the solve is bit-identical.
  thread_local MatX hd(3, 3);
  thread_local VecX ng(3);
  for (int trial = 0; trial < kLmTrialCap; ++trial) {
    const double d = 1.0 + lambda_lm;
    hd(0, 0) = h00 * d;
    hd(0, 1) = h01;
    hd(0, 2) = h02;
    hd(1, 0) = h01;
    hd(1, 1) = h11 * d;
    hd(1, 2) = h12;
    hd(2, 0) = h02;
    hd(2, 1) = h12;
    hd(2, 2) = h22 * d;
    ng(0) = -g0;
    ng(1) = -g1;
    ng(2) = -g2;

    const auto sol = atx::core::linalg::solve_spd(hd, ng);
    if (!sol.has_value()) {
      lambda_lm *= kLambdaGrow;
      if (lambda_lm > kLambdaLmMax) {
        return -1.0;
      }
      continue;
    }
    const VecX& step = *sol;
    double psi_new = psi + step(0);
    double p_new = p + step(1);
    double lambda_new = lambda + step(2);
    clamp_cube(psi_new, p_new, lambda_new);

    const double new_sse = cube_sse(obs, weights, band, T, psi_new, p_new,
                                    lambda_new, kbuf, wbuf, prior);
    if (new_sse < prev_sse) {
      psi = psi_new;
      p = p_new;
      lambda = lambda_new;
      lambda_lm *= kLambdaShrink;
      if (lambda_lm < kLambdaLmMin) {
        lambda_lm = kLambdaLmMin;
      }
      return new_sse;
    }
    lambda_lm *= kLambdaGrow;
    if (lambda_lm > kLambdaLmMax) {
      return prev_sse;  // stuck at (or near) the minimum
    }
  }
  return prev_sse;
}

// Lee (2004) wing-slope projection: if θ·φ·(1+|ρ|) > 4 shrink `p` (which scales
// φ) by bisection to the largest admissible value. Returns true if it fired.
// FT-C3: with θ = TOTAL variance the Lee/Gatheral wing-slope constraint is T-FREE
// — θ·φ·(1+|ρ|) ≤ 4 — exactly the bound the Mingone cube's essvi_phi_max already
// enforces (b1 = 4/(θ(1+|ρ|))). The previous 4/T form was a no-op for T<1 and
// over-tight for T>1: a 2y high-vol steep-skew slice with θφ(1+|ρ|)≈2.7 ≤ 4 was
// silently flattened to 4/T = 2. This projection is now redundant with the cube
// clamp but harmless — kept as an explicit belt-and-braces wing gate.
bool lee_project(double& psi, double& p, double& lambda, const ThetaBand& band,
                 double T) noexcept {
  const EssviNatural n = cube_to_natural(psi, p, lambda, band);
  if (!(T > 0.0) || !(n.theta > 0.0) || !(n.phi > 0.0)) {
    return false;
  }
  const double rhs = 4.0;
  if (n.theta * n.phi * (1.0 + std::fabs(n.rho)) <= rhs) {
    return false;
  }
  double lo = kCubeEdge;
  double hi = p;
  for (int it = 0; it < 32; ++it) {
    const double mid = 0.5 * (lo + hi);
    const EssviNatural nn = cube_to_natural(psi, mid, lambda, band);
    if (nn.theta * nn.phi * (1.0 + std::fabs(nn.rho)) <= rhs) {
      lo = mid;
    } else {
      hi = mid;
    }
    if (hi - lo < 1.0e-9) {
      break;
    }
  }
  p = lo;
  return true;
}

// ── Dense C2 residual layer (smoothing spline on w_mkt − backbone) ───────────
//
// The wing-only HINGE_QUAD layer (below) corrects the deep wings but is zero
// inside |y| < kResidInnerY, so it never tightens the high-vega core — and the
// core is exactly where the penny bid-ask band is narrowest and % within bid-ask
// is decided. This layer instead fits a DENSE C2 bump basis over the WHOLE smile
// (detail/resid_basis.hpp) to the vega-weighted total-variance residual, with a
// 2nd-difference roughness penalty so it behaves as a smoothing spline:
// near-interpolating where quotes are dense and clean (the liquid core), smooth
// where they are sparse (the wings). A butterfly guard raises the penalty until
// the total (backbone + residual) density g(k) >= 0; if no arb-free fit is found
// the slice is left backbone-only.

// Lee/Roper density g(k) = (1 - k·w'/2w)² - (w'²/4)(¼ + 1/w) + w''/2 on the TOTAL
// (backbone + residual) variance, min over a grid spanning the fitted range.
// Central finite differences (matches arb.hpp's butterfly check). Returns a large
// negative sentinel where total variance has COLLAPSED (treated as a violation).
//
// The collapse test is `w <= kTotalWFloor`, NOT `w <= 0`: `essvi_total_w` already
// substitutes the 1e-12 hot-path net for any non-positive backbone+residual sum,
// so a `w > 0` test can never fire and the whole guard is dead code. Worse, the
// clamp makes a collapsed stencil perfectly FLAT — w == w(k±h) == 1e-12 — so
// w' = w'' = 0 and the density reads g = +1, i.e. the most arb-free score
// possible, while the slice serves ~1e-6 vol. Testing against the floor catches
// both the clamped case and the (unclamped) sub-floor case; strictly above the
// floor the formula is self-policing, because a genuinely tiny w with a non-zero
// slope drives -(w'²/4)/w hugely negative on its own.
//
// A collapse is scored as a VIOLATION rather than an outright fit rejection, per
// this file's convention: the caller's greedy coordinate projection then damps
// the offending residual coefficient and retries, and falls back to a
// backbone-only slice if no arb-free residual is reachable.
[[nodiscard]] double dense_resid_min_g(const EssviParams& s, double kmax,
                                      double* k_worst = nullptr) noexcept {
  constexpr int kNg = 80;
  const double klo = -1.15 * kmax;
  const double khi = 1.15 * kmax;
  const double h = std::max(1.0e-4, (khi - klo) / (4.0 * kNg));
  double gmin = std::numeric_limits<double>::infinity();
  for (int i = 0; i <= kNg; ++i) {
    const double k = klo + (khi - klo) * static_cast<double>(i) / kNg;
    const double w = essvi_total_w(s, k);
    if (!(w > kTotalWFloor) || !std::isfinite(w)) {
      if (k_worst != nullptr) {
        *k_worst = k;
      }
      return -1.0e9;
    }
    const double wp = essvi_total_w(s, k + h);
    const double wm = essvi_total_w(s, k - h);
    const double dw = (wp - wm) / (2.0 * h);
    const double d2w = (wp - 2.0 * w + wm) / (h * h);
    const double a = 1.0 - k * dw / (2.0 * w);
    const double g = a * a - 0.25 * dw * dw * (0.25 + 1.0 / w) + 0.5 * d2w;
    if (g < gmin) {
      gmin = g;
      if (k_worst != nullptr) {
        *k_worst = k;
      }
    }
  }
  return gmin;
}

// Fit the dense C2 residual on the converged backbone. Weighted (vega²/spread²)
// least squares of (w_mkt − backbone) against the N-bump basis + a
// 2nd-difference roughness penalty λ·(D²)ᵀD², escalating λ until the fit is
// butterfly-arb-free. Writes resid_coef[0..N-1] / resid_scale / kind / n_basis on
// success; leaves the slice backbone-only otherwise.
void fit_dense_residual(std::span<const FitObs> obs, EssviParams& slice,
                        const CalibOpts& opts) {
  double kmax = 0.0;
  for (const FitObs& o : obs) {
    kmax = std::max(kmax, std::fabs(o.k));
  }
  if (!(kmax > 0.0)) {
    return;
  }
  const int N = detail::resid_bump_count(
      opts.residual_n_basis_terms != 0 ? opts.residual_n_basis_terms : 12);
  const double scale = kmax;

  // Precompute per-obs basis rows, the backbone-residual target (w-space), and
  // the base (vega²/spread²) weight. Since ~1/4 of a raw penny-quote SPY board is
  // locally butterfly-violating (stale / one-sided / non-synchronous quotes that
  // NO arb-free surface can reproduce), a plain LSQ residual chases that noise and
  // manufactures arbitrage. The IRLS-Huber loop below rejects those outliers so
  // the layer fits the smooth consensus of the CLEAN quotes and stays arb-free.
  const std::size_t n = obs.size();
  std::vector<std::array<double, 16>> B(n);
  std::vector<double> target(n);
  std::vector<double> base_w(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double y = std::clamp(obs[i].k / scale, -1.0, 1.0);
    detail::resid_bump_basis(y, N, B[i]);
    target[i] = obs[i].w_mkt - essvi_backbone_w(slice, obs[i].k);
    base_w[i] = (obs[i].weight_w > 0.0) ? obs[i].weight_w : 1.0;
  }

  const double huber_k = (opts.huber_k > 0.0) ? opts.huber_k : 1.5;
  const double lam_f =
      (opts.residual_ridge_factor > 0.0) ? opts.residual_ridge_factor : 5.0e-4;

  std::vector<double> w_eff = base_w;  // IRLS-mutable weights
  std::array<double, 16> coef{};
  bool have = false;
  for (int pass = 0; pass < 4; ++pass) {
    // Weighted normal equations Bᵀ W B c = Bᵀ W target + roughness + ridge.
    MatX A = MatX::Zero(N, N);
    VecX rhs = VecX::Zero(N);
    for (std::size_t i = 0; i < n; ++i) {
      const double wt = w_eff[i];
      if (!(wt > 0.0)) {
        continue;
      }
      const auto& bi = B[i];
      for (int r = 0; r < N; ++r) {
        const auto rr = static_cast<std::size_t>(r);
        rhs(r) += wt * bi[rr] * target[i];
        for (int c = 0; c <= r; ++c) {
          A(r, c) += wt * bi[rr] * bi[static_cast<std::size_t>(c)];
        }
      }
    }
    for (int r = 0; r < N; ++r) {
      for (int c = r + 1; c < N; ++c) {
        A(r, c) = A(c, r);
      }
    }
    double tr = 0.0;
    for (int i = 0; i < N; ++i) {
      tr += A(i, i);
    }
    if (!(tr > 0.0)) {
      if (have) {
        break;
      }
      return;
    }
    const double tr_avg = tr / static_cast<double>(N);
    // Soft 2nd-difference roughness penalty λ·(D²)ᵀD² (smoothing-spline behavior)
    // + tiny conditioning ridge. Kept soft: near-interpolate the dense clean core.
    const double lam = lam_f * tr_avg;
    for (int r = 1; r < N - 1; ++r) {
      const int idx[3] = {r - 1, r, r + 1};
      const double val[3] = {1.0, -2.0, 1.0};
      for (int a = 0; a < 3; ++a) {
        for (int c = 0; c < 3; ++c) {
          A(idx[a], idx[c]) += lam * val[a] * val[c];
        }
      }
    }
    for (int i = 0; i < N; ++i) {
      A(i, i) += 1.0e-9 * tr_avg + 1.0e-15;
    }
    const auto sol = atx::core::linalg::solve_spd(A, rhs);
    if (!sol.has_value()) {
      if (have) {
        break;
      }
      return;  // leave backbone-only (safe)
    }
    for (int j = 0; j < N; ++j) {
      coef[static_cast<std::size_t>(j)] = (*sol)(j);
    }
    have = true;
    if (pass == 3) {
      break;
    }
    // Robust reweight: standardized residual z = (fit − target)·√(base_w) is the
    // residual in noise units (base_w ≈ 1/σ_w²); Huber down-weights |z| > k so the
    // arb-violating quotes stop driving the fit.
    for (std::size_t i = 0; i < n; ++i) {
      double fit = 0.0;
      for (int j = 0; j < N; ++j) {
        fit += coef[static_cast<std::size_t>(j)] * B[i][static_cast<std::size_t>(j)];
      }
      const double z = (fit - target[i]) * std::sqrt(base_w[i]);
      const double az = std::fabs(z);
      const double hw = (az <= huber_k) ? 1.0 : huber_k / az;
      w_eff[i] = base_w[i] * hw;
    }
  }
  if (!have) {
    return;
  }

  // Static-arb safety by LOCAL greedy coordinate projection. The robust residual
  // still over-bends in thin regions (density g(k) craters where it chases a
  // penny-noise quote), but a GLOBAL scalar damp would strip the whole slice's
  // correction to fix one point. Instead: repeatedly locate the worst-g grid
  // point and HALVE the single coefficient whose bump dominates there, until
  // g(k) >= 0 everywhere — so the correction survives wherever it is arb-safe.
  auto store = [&](EssviParams& s, const std::array<double, 16>& c) {
    s.resid_coef = {};
    for (int j = 0; j < N; ++j) {
      s.resid_coef[static_cast<std::size_t>(j)] = c[static_cast<std::size_t>(j)];
    }
    s.resid_scale = scale;
    s.resid_basis_kind = ResidualBasisKind::C2Bspline;
    s.resid_n_basis = static_cast<std::uint8_t>(N);
  };

  std::array<double, 16> row{};
  for (int iter = 0; iter < 80; ++iter) {
    EssviParams trial = slice;
    store(trial, coef);
    double k_worst = 0.0;
    const double gmin = dense_resid_min_g(trial, kmax, &k_worst);
    if (gmin >= 0.0) {
      slice = trial;  // arb-free — accept the locally-projected residual
      return;
    }
    // Halve the coefficient whose basis bump dominates the violation at k_worst.
    const double y = std::clamp(k_worst / scale, -1.0, 1.0);
    detail::resid_bump_basis(y, N, row);
    int jbest = -1;
    double best = 0.0;
    for (int j = 0; j < N; ++j) {
      const auto jj = static_cast<std::size_t>(j);
      const double contrib = std::fabs(coef[jj]) * row[jj];
      if (contrib > best) {
        best = contrib;
        jbest = j;
      }
    }
    if (jbest < 0) {
      break;  // no active coefficient at the violation — cannot repair locally
    }
    coef[static_cast<std::size_t>(jbest)] *= 0.5;
  }
  // Did not converge to arb-free within the budget: leave the slice
  // backbone-only (safe) rather than serve a butterfly-arb residual.
}

// ── Optional wing-residual layer (ridge LS on w_mkt − backbone) ──────────
//
// HINGE_QUAD basis (slots 1..4 of resid_coef == {yp, yp², yc, yc²}, matching
// `essvi_residual_w`). Inverse-noise weighted ridge least squares.
//
// PORT NOTE — the C follows the ridge LS with a per-slice Roper density
// projection (`ats_vol_essvi_residual_arb_project`) that damps the coefficients
// until g(k) >= 0. That per-slice projector is NOT exposed by the ported
// arb.hpp (which surfaces only the surface-level `arb_repair_calendar_residual`
// / total-surface projectors), so it is STILL deferred here (Task C-8 did not
// port it either): the fitted residual is left un-projected. Task C-8 made
// the "caller/surface-level check" this note promises concrete and FAIL-
// CLOSED rather than aspirational: `essvi_calib_surface`'s `validate_no_arb`
// audit (this file, `calib_surface_impl`) catches an un-projected butterfly
// violation on the alternate-driver path, over its fixed `[-0.5, 0.5]`
// band (essvi_calib.hpp); `validate_served_shape_over_quotes`
// (vol_curve.cpp) catches one on the canonical `fit_slice_curve` serving
// path, over the full quoted range +/- 0.5. Neither REPAIRS the residual —
// both refuse to serve a slice/surface the deferred projection would have
// fixed, which is the substitute this port takes for it.
// The layer is off by default (`opts.residual_disable == true`).
void fit_wing_residual(std::span<const FitObs> obs, EssviParams& slice,
                       const CalibOpts& opts) {
  double kmax = 0.0;
  for (const FitObs& o : obs) {
    kmax = std::max(kmax, std::fabs(o.k));
  }
  if (!(kmax > 0.0)) {
    return;
  }
  constexpr double kInnerY = 0.4;
  constexpr int kNb = 4;
  const double scale = kmax;
  const double ridge =
      (opts.residual_ridge_factor > 0.0) ? opts.residual_ridge_factor : 1.0e-3;

  MatX a = MatX::Zero(kNb, kNb);
  VecX rhs = VecX::Zero(kNb);
  for (const FitObs& o : obs) {
    const double y = std::clamp(o.k / scale, -1.0, 1.0);
    const double yp = (y < -kInnerY) ? (-y - kInnerY) : 0.0;
    const double yc = (y > kInnerY) ? (y - kInnerY) : 0.0;
    const std::array<double, kNb> b{yp, yp * yp, yc, yc * yc};
    const double resid = o.w_mkt - essvi_backbone_w(slice, o.k);
    const double wt = (o.weight_w > 0.0) ? o.weight_w : 1.0;
    for (int ai = 0; ai < kNb; ++ai) {
      rhs(ai) += wt * b[static_cast<std::size_t>(ai)] * resid;
      for (int bi = 0; bi < kNb; ++bi) {
        a(ai, bi) += wt * b[static_cast<std::size_t>(ai)] *
                     b[static_cast<std::size_t>(bi)];
      }
    }
  }
  for (int ai = 0; ai < kNb; ++ai) {
    a(ai, ai) += ridge;
  }
  const auto sol = atx::core::linalg::solve_spd(a, rhs);
  if (!sol.has_value()) {
    return;  // leave the slice backbone-only
  }
  const VecX& c = *sol;
  slice.resid_coef[1] = c(0);
  slice.resid_coef[2] = c(1);
  slice.resid_coef[3] = c(2);
  slice.resid_coef[4] = c(3);
  slice.resid_scale = scale;
  slice.resid_basis_kind = ResidualBasisKind::HingeQuad;
  slice.resid_n_basis = 5;
}

// ── Per-slice fit-core scratch arena ─────────────────────────────────────
//
// Owns the 13 per-`fit_core` vectors so they can be allocated ONCE and reused
// across every slice a driver fits (serial drivers keep one; the parallel
// driver keeps one PER WORKER, indexed by the fan-out's worker id — race-free).
// `reset(n)` re-establishes each buffer as exactly n zero-initialised doubles,
// reproducing the historical `std::vector<double> x(n)` value-initialisation
// bit-for-bit: every element is 0.0 before any read, so reuse can never leak a
// prior slice's state, and no allocation occurs once capacity has stabilised.
struct FitScratch {
  std::vector<double> base_w;  // fixed obs weights
  std::vector<double> active;  // IRLS-mutable weight copy
  std::vector<double> kbuf;    // contiguous obs log-moneyness (LM-invariant)
  std::vector<double> wbuf;    // batch model-w output
  std::vector<double> dth;     // batch ∂w/∂theta
  std::vector<double> dphi;    // batch ∂w/∂phi
  std::vector<double> drho;    // batch ∂w/∂rho
  std::vector<double> r;       // LM residuals
  std::vector<double> jpsi;    // LM Jacobian column ∂/∂psi
  std::vector<double> jp;      // LM Jacobian column ∂/∂p
  std::vector<double> jl;      // LM Jacobian column ∂/∂lambda
  std::vector<double> r_abs;   // |w-residual| for the IRLS reweight
  std::vector<double> hw;      // Huber weights

  void reset(std::size_t n) {
    base_w.assign(n, 0.0);
    active.assign(n, 0.0);
    kbuf.assign(n, 0.0);
    wbuf.assign(n, 0.0);
    dth.assign(n, 0.0);
    dphi.assign(n, 0.0);
    drho.assign(n, 0.0);
    r.assign(n, 0.0);
    jpsi.assign(n, 0.0);
    jp.assign(n, 0.0);
    jl.assign(n, 0.0);
    r_abs.assign(n, 0.0);
    hw.assign(n, 0.0);
  }
};

// ── Per-slice fit core (parameterized by the theta band) ─────────────────
[[nodiscard]] Result<EssviParams> fit_core(std::span<const FitObs> obs, double T,
                                           double F, const CalibOpts& opts,
                                           const ThetaBand& band,
                                           FitDiag* out_diag, FitScratch& scratch,
                                           const EssviParams* warm = nullptr) {
  if (obs.empty() || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "essvi_fit_slice: empty observations or non-positive T");
  }
  const std::size_t n = obs.size();

  // Reset the scratch arena to n zeroed doubles (matches the historical fresh
  // per-call vector(n) value-init exactly; no allocation once warmed). Local
  // references below keep the LM body identical to the pre-arena code.
  scratch.reset(n);

  // Base (fixed) weights and the IRLS-mutable active copy.
  std::vector<double>& base_w = scratch.base_w;
  std::vector<double>& active = scratch.active;
  double total_base_w = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    base_w[i] = (obs[i].weight_w > 0.0) ? obs[i].weight_w : 1.0;
    active[i] = base_w[i];
    total_base_w += base_w[i];
  }

  // Cube seed. The COLD path seeds psi from the ATM total variance (a crude
  // band-ratio level estimate) and p / lambda neutral. A WARM-start refit seeds
  // the whole cube (psi, p, lambda) from the PREVIOUS fit's CONVERGED
  // coordinates: for a tick-to-quote update of the same expiry that lands the LM
  // essentially at the optimum, so it converges in far fewer inner iterations
  // than the cold seed (the crude band-ratio psi is nowhere near as good as the
  // prior optimum). A null `warm` is byte-identical to the historical seed.
  double psi = 0.5;
  double p = 0.3;
  double lambda = 0.4;
  if (warm != nullptr) {
    psi = warm->psi;
    p = warm->p;
    lambda = warm->lambda;
  } else {
    double atm_w = 0.0;
    double atm_absk = std::numeric_limits<double>::infinity();
    for (const FitObs& o : obs) {
      if (std::fabs(o.k) < atm_absk) {
        atm_absk = std::fabs(o.k);
        atm_w = o.w_mkt;
      }
    }
    if (atm_w > 0.0 && band.hi > band.lo) {
      psi = std::clamp((atm_w - band.lo) / (band.hi - band.lo), 0.01, 0.99);
    }
  }
  clamp_cube(psi, p, lambda);

  // Optional warm-start Tikhonov prior. Enabled only when a warm slice is given
  // AND opts.prior_strength > 0. The user-facing strength (0..1, "fraction of
  // the data's influence") is scaled here by the dataset's total weight so the
  // penalty is dimensionally comparable to the obs SSE; downstream it is used
  // raw. Anchored at the CLAMPED seed cube (== warm cube), the fit's home base.
  CubePrior cube_prior{};
  const bool use_prior = (warm != nullptr) && (opts.prior_strength > 0.0);
  if (use_prior) {
    cube_prior.psi = std::clamp(warm->psi, kCubeEdge, 1.0 - kCubeEdge);
    cube_prior.p = p;
    cube_prior.lambda = lambda;
    cube_prior.strength = opts.prior_strength * total_base_w;
  }
  const CubePrior* prior = use_prior ? &cube_prior : nullptr;

  // Morozov noise floor in w-domain SSE units (off by default). The C poses it
  // in price units; here the per-obs half-spread is mapped to a w-noise via
  // dw/dσ = 2σT — δ² = Σ w_i · (σ_mkt·T·spread/vega)².
  double morozov_floor = 0.0;
  if (opts.morozov_stop) {
    double sum = 0.0;
    std::size_t cnt = 0;
    for (std::size_t i = 0; i < n; ++i) {
      if (obs[i].spread > 0.0 && obs[i].vega > 0.0) {
        const double hs_w =
            obs[i].sigma_mkt * T * obs[i].spread / obs[i].vega;
        sum += base_w[i] * hs_w * hs_w;
        ++cnt;
      }
    }
    if (cnt > 0) {
      const double tau = (opts.morozov_tau > 0.0) ? opts.morozov_tau : 1.1;
      morozov_floor = tau * tau * sum;
    }
  }

  const std::uint16_t max_outer = detail::outer_cap(opts);
  const std::uint16_t max_inner =
      (opts.max_inner_iter > 0) ? opts.max_inner_iter : kLmInnerDefault;
  const double tol_param =
      (opts.tol_param > 0.0) ? opts.tol_param : kTolParamDefault;

  // Hoisted contiguous obs-k buffer: k never changes across LM iterations, so
  // the batched w/w-grad kernels read it directly (built once here, where the
  // obs span is fixed). Plus the reusable per-iteration batch-output scratch
  // (model w + the three natural partials) — all allocated once per fit_core,
  // never per LM iteration.
  std::vector<double>& kbuf = scratch.kbuf;
  for (std::size_t i = 0; i < n; ++i) {
    kbuf[i] = obs[i].k;
  }
  std::vector<double>& wbuf = scratch.wbuf;
  std::vector<double>& dth = scratch.dth;
  std::vector<double>& dphi = scratch.dphi;
  std::vector<double>& drho = scratch.drho;

  // LM scratch (residuals + 3 Jacobian columns).
  std::vector<double>& r = scratch.r;
  std::vector<double>& jpsi = scratch.jpsi;
  std::vector<double>& jp = scratch.jp;
  std::vector<double>& jl = scratch.jl;
  std::vector<double>& r_abs = scratch.r_abs;
  std::vector<double>& hw = scratch.hw;

  std::uint16_t outer_iters = 0;
  std::uint16_t inner_total = 0;
  double prev_outer_sse = std::numeric_limits<double>::infinity();

  for (std::uint16_t outer = 0; outer < max_outer; ++outer) {
    double sse = cube_sse(obs, active, band, T, psi, p, lambda, kbuf, wbuf, prior);
    double lambda_lm = kLambdaLmInit;
    for (std::uint16_t inner = 0; inner < max_inner; ++inner) {
      const double psi_old = psi;
      const double p_old = p;
      const double lambda_old = lambda;
      const double new_sse =
          lm_step(obs, active, band, T, psi, p, lambda, sse, lambda_lm, kbuf,
                  wbuf, dth, dphi, drho, r, jpsi, jp, jl, prior);
      ++inner_total;
      if (new_sse < 0.0) {
        break;  // unrecoverable solve; keep the last good cube
      }
      sse = new_sse;
      const double dpsi = psi - psi_old;
      const double dp = p - p_old;
      const double dl = lambda - lambda_old;
      const double step2 = dpsi * dpsi + dp * dp + dl * dl;
      if (step2 < tol_param * tol_param) {
        break;
      }
      if (morozov_floor > 0.0 && sse <= morozov_floor) {
        break;
      }
    }
    outer_iters = static_cast<std::uint16_t>(outer + 1);

    if (morozov_floor > 0.0 && sse <= morozov_floor) {
      break;
    }
    if (opts.lee_bound_project) {
      (void)lee_project(psi, p, lambda, band, T);
    }
    if (std::fabs(prev_outer_sse - sse) < kOuterStallSse) {
      break;
    }
    prev_outer_sse = sse;

    // IRLS Huber reweight on the |w-residual| distribution (reuse the ported
    // q90-anchored strided helper). At a clean optimum residuals collapse to
    // zero and the weights stay unit, so recovery is unaffected.
    const EssviParams cur = slice_from_cube(psi, p, lambda, band, T);
    for (std::size_t i = 0; i < n; ++i) {
      r_abs[i] = std::fabs(essvi_backbone_w(cur, obs[i].k) - obs[i].w_mkt);
    }
    detail::huber_weights_strided<double>(
        std::span<const double>{r_abs}, std::span<double>{hw}, opts.huber_k);
    for (std::size_t i = 0; i < n; ++i) {
      active[i] = base_w[i] * hw[i];
    }
  }

  // Map cube -> natural and populate the slice.
  const EssviNatural nat = cube_to_natural(psi, p, lambda, band);
  if (!(std::isfinite(nat.theta) && std::isfinite(nat.phi) &&
        std::isfinite(nat.rho) && nat.theta > 0.0)) {
    return Err(ErrorCode::Unavailable,
               "essvi_fit_slice: LM produced a degenerate slice");
  }
  EssviParams slice{};
  slice.theta = nat.theta;
  slice.phi = nat.phi;
  slice.rho = nat.rho;
  slice.psi = psi;
  slice.p = p;
  slice.lambda = lambda;
  slice.T = T;
  slice.F = F;

  // Optional additive residual layer (fit on the converged backbone). The dense
  // C2 basis tightens the whole smile (incl. the high-vega core, where % within
  // bid-ask is decided); HINGE_QUAD is the wing-only legacy layer.
  if (!opts.residual_disable) {
    if (opts.residual_basis_kind == ResidualBasisKind::C2Bspline) {
      fit_dense_residual(obs, slice, opts);
    } else if (opts.residual_basis_kind == ResidualBasisKind::HingeQuad) {
      fit_wing_residual(obs, slice, opts);
    }
  }

  // Diagnostics: vol-domain vega-agnostic RMSE and max residual (vol pts).
  double sumw = 0.0;
  double sumwr2 = 0.0;
  double max_res = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    double w_pred = essvi_total_w(slice, obs[i].k);
    if (w_pred < 1.0e-12) {
      w_pred = 1.0e-12;
    }
    const double sig_pred = std::sqrt(w_pred / T);
    const double dv = sig_pred - obs[i].sigma_mkt;
    sumw += base_w[i];
    sumwr2 += base_w[i] * dv * dv;
    max_res = std::max(max_res, std::fabs(dv));
  }
  const double rmse = (sumw > 1.0e-15) ? std::sqrt(sumwr2 / sumw) : 0.0;

  if (out_diag != nullptr) {
    out_diag->rmse_vol_vega_weighted = rmse;
    out_diag->max_residual_vol = max_res;
    out_diag->outer_iters = outer_iters;
    out_diag->inner_iters_total = inner_total;
    out_diag->n_quotes_used = static_cast<std::uint32_t>(n);
  }
  return Ok(slice);
}

// A prior slice is a usable warm seed only if its cube/theta coordinates are
// finite and represent an actual fit (theta > 0). `vol_surface.hpp` carries no
// separate per-slice fit-validity flag, so this finiteness + positivity check
// is the guard.
[[nodiscard]] bool is_usable_warm_seed(const EssviParams& s) noexcept {
  return std::isfinite(s.theta) && s.theta > 0.0 && std::isfinite(s.psi) &&
         std::isfinite(s.p) && std::isfinite(s.lambda);
}

// Pick `prior`'s slice minimizing |T_prior - T| and return it as the warm
// seed iff the gap is within `kWarmPriorMaxTenorGap` and the slice is a
// usable eSSVI fit. Returns nullptr (cold fit) on a null/empty/non-eSSVI
// `prior`, no slice within tolerance, or an unusable closest slice — `prior`
// surfaces only populate their `essvi_slices()` vector when eSSVI-
// parametrized, so a non-eSSVI prior naturally yields an empty scan here.
[[nodiscard]] const EssviParams* find_warm_seed(const VolSurface* prior,
                                                double T) noexcept {
  if (prior == nullptr || prior->param() != Parametrization::Essvi) {
    return nullptr;
  }
  const EssviParams* best = nullptr;
  double best_gap = std::numeric_limits<double>::infinity();
  for (const EssviParams& s : prior->essvi_slices()) {
    const double gap = std::fabs(s.T - T);
    if (gap < best_gap) {
      best_gap = gap;
      best = &s;
    }
  }
  if (best == nullptr || best_gap > kWarmPriorMaxTenorGap) {
    return nullptr;
  }
  return is_usable_warm_seed(*best) ? best : nullptr;
}

// One chain's independent fit work: obs-build -> theta band -> warm seed ->
// fit_core -> post-fit sigma clamp. PURE w.r.t. shared state (reads `c`,
// `curves`, `opts`, `prior` const; writes only its own `scratch`), so it is
// safe to run concurrently over disjoint chains — this is the unit BOTH surface
// drivers use: the serial path calls it inline (live theta_floor), the parallel
// path fans it out. `theta_floor` (> 0 only in the sequential driver) raises the
// theta band's lower bound; in the parallel driver it is always 0. The returned
// result carries everything phase 2 needs to replicate the serial control flow
// WITHOUT recomputing.
struct ChainFitResult {
  bool obs_built = false;       // build_observations succeeded -> n_dropped valid
  std::uint32_t n_dropped = 0;  // dropped-quote count for this chain
  bool success = false;         // a slice was produced AND passed the sigma clamp
  EssviParams slice{};          // fitted slice (valid iff success)
  FitDiag diag{};               // per-chain diagnostics (valid iff success)
};

[[nodiscard]] ChainFitResult fit_one_chain(const Chain& c, const CurveSet& curves,
                                           const CalibOpts& opts, bool sequential,
                                           double theta_floor,
                                           const VolSurface* prior,
                                           FitScratch& scratch, double S,
                                           const DeAmOptions* deam) {
  ChainFitResult res{};
  const double T = c.T;
  if (!(T > kTMinFit)) {
    return res;  // sub-threshold expiry: no obs, no slot
  }
  const double F = curves.forward.forward_at(c.expiry_id);
  if (!std::isfinite(F) || !(F > 0.0)) {
    return res;
  }
  const double df = curves.yield.disc(T);

  // Observation build. `deam == nullptr` is the raw Black-76 inversion (today's
  // path, byte-identical). Otherwise de-Americanize: strip each American mid to
  // its European-equivalent vol via `build_observations_european`, deriving the
  // per-chain carry (S from the underlier, r from the yield curve at this T; the
  // European carry q_eff = r − ln(F/S)/T is formed inside the builder). The
  // caches/al_opts/iv_tol/iv_max_iter knobs select the cold vs cached hot path.
  // This runs inside the (possibly parallel) per-chain worker — see the thread-
  // safety note on `calib_surface_impl` below.
  const Result<ObsSet> obs_res =
      (deam != nullptr)
          ? build_observations_european(c, S, curves.yield.zero(T), F, T, df, opts,
                                        deam->caches, deam->al_opts, deam->iv_tol,
                                        deam->iv_max_iter)
          : build_observations(c, F, T, df, opts);
  if (!obs_res.has_value()) {
    return res;  // too few survivors / malformed chain: skip this expiry
  }
  const ObsSet& os = *obs_res;
  res.obs_built = true;
  res.n_dropped = os.n_dropped;
  if (os.obs.size() < opts.min_obs_per_slice) {
    return res;  // below the min-obs floor: drops counted, no slot
  }

  // Theta band: default, or floored at the previous slice's theta. Cap the
  // raised floor a RELATIVE margin below theta_hi so the band stays strictly
  // non-inverted (theta_lo < theta_hi) even when theta_hi is near zero on a
  // tiny-T slice — an absolute 1e-12 gap could vanish against a small theta_hi.
  ThetaBand band = default_band(T);
  if (sequential && theta_floor > band.lo) {
    const double margin = std::max(2.0e-12, 1.0e-9 * band.hi);
    band.lo = std::min(theta_floor, band.hi - margin);
  }

  const EssviParams* warm = find_warm_seed(prior, T);

  FitDiag diag{};
  const Result<EssviParams> fit = fit_core(
      std::span<const FitObs>{os.obs}, T, F, opts, band, &diag, scratch, warm);
  if (!fit.has_value()) {
    return res;  // LM failure: skip (fallback machinery deferred, see PORT NOTE)
  }
  EssviParams slice = *fit;
  slice.T = T;
  slice.F = F;
  slice.expiry_id = c.expiry_id;
  slice.expiry_ns = c.expiry_ns;

  // Post-fit ATM-σ clamp (mirror of the C robustness guard).
  if (opts.max_post_fit_sigma > 0.0 && slice.theta > 0.0) {
    const double sigma_atm = std::sqrt(slice.theta / T);
    if (sigma_atm > opts.max_post_fit_sigma) {
      return res;  // clamp reject: drops already counted, no slot
    }
  }

  res.success = true;
  res.slice = slice;
  res.diag = diag;
  return res;
}

// Shared chain walk for both surface drivers. `sequential` raises the theta
// floor to the previous slice's theta (a RAW loop-carry, so that driver stays
// serial). `prior` (optional) is a previously-fit surface each slice is warm-
// started from via `find_warm_seed`. `n_workers` fans the (order-independent)
// NON-sequential per-expiry fit across a thread pool; the serial phase-2
// reduction keeps results + FitDiag bit-identical at every worker count.
[[nodiscard]] Status calib_surface_impl(VolSurface& surface,
                                        const Underlying& under,
                                        const CurveSet& curves,
                                        const CalibOpts& opts,
                                        FitDiag* out_diag, bool sequential,
                                        const VolSurface* prior,
                                        unsigned n_workers,
                                        const DeAmOptions* deam) {
  ATX_TRY_VOID(validate_calib_options(opts));
  if (surface.param() != Parametrization::Essvi) {
    return Err(ErrorCode::InvalidArgument,
               "essvi_calib_surface: surface is not eSSVI-parametrized");
  }
  if (under.chains.empty()) {
    return Err(ErrorCode::NotFound, "essvi_calib_surface: no chains");
  }

  // Spot for the opt-in de-Am route (unused on the raw path). Derived once from
  // the underlier, mirroring `run_deam_prepass` (curve_fit.cpp): the per-chain
  // r/df come from the yield curve at each chain's T inside `fit_one_chain`.
  const double S = under.spot;

  std::uint32_t total_used = 0;
  std::uint32_t total_dropped = 0;
  double sum_sse = 0.0;
  double sum_w = 0.0;
  double max_res = 0.0;
  std::uint32_t agg_outer = 0;
  std::uint32_t agg_inner = 0;
  std::size_t n_fit_ok = 0;

  // Sequential theta floor: 0 disables it (first slice / non-sequential).
  double theta_floor = 0.0;

  const std::size_t n_chains = under.chains.size();

  // Phase-2 consumption of one chain's fit result, in ORIGINAL chain order.
  // Replicates the serial control flow EXACTLY: accumulate this chain's drops
  // (only when its observations were built — matching the serial `total_dropped
  // += os.n_dropped` placement after a successful build), then, on success,
  // compact it into the next ascending-T slot (`set_slice_essvi(n_fit_ok, ...)`
  // — a PREFIX-SUM over earlier successes, NOT a disjoint write, so failed
  // chains leave no hole), raise the theta floor, and reduce its diagnostics in
  // chain order (identical FP summation order => identical bits). The caller
  // checks the capacity break BEFORE invoking, so speculative fits beyond the
  // break are discarded here unconsumed and never counted.
  auto consume = [&](const ChainFitResult& res) -> Status {
    if (res.obs_built) {
      total_dropped += res.n_dropped;
    }
    if (!res.success) {
      return Ok();
    }
    const Status set_st = surface.set_slice_essvi(n_fit_ok, res.slice);
    if (!set_st.has_value()) {
      return set_st;  // capacity / parametrization mismatch (should not happen)
    }
    theta_floor = res.slice.theta;
    total_used += res.diag.n_quotes_used;
    sum_sse += res.diag.rmse_vol_vega_weighted * res.diag.rmse_vol_vega_weighted *
               static_cast<double>(res.diag.n_quotes_used);
    sum_w += static_cast<double>(res.diag.n_quotes_used);
    max_res = std::max(max_res, res.diag.max_residual_vol);
    agg_outer += res.diag.outer_iters;
    agg_inner += res.diag.inner_iters_total;
    ++n_fit_ok;
    return Ok();
  };

  // Worker count: 0 -> auto (env cap / hardware_concurrency), clamped to the
  // chain count. The sequential driver's RAW theta-floor loop-carry forbids
  // fan-out, so it always takes the serial path.
  unsigned nt = (n_workers == 0u) ? atx_auto_worker_count() : n_workers;
  if (nt > n_chains) {
    nt = static_cast<unsigned>(n_chains);
  }
  const bool use_parallel = !sequential && nt > 1u && n_chains > 1u;

  if (!use_parallel) {
    // Serial driver (also the sequential driver's only path): compute each
    // chain inline — passing the LIVE theta_floor — then consume it. One
    // scratch arena reused across every chain.
    FitScratch scratch;
    for (std::size_t i = 0; i < n_chains; ++i) {
      if (n_fit_ok >= surface.capacity()) {
        break;
      }
      const ChainFitResult res = fit_one_chain(
          under.chains[i], curves, opts, sequential, theta_floor, prior, scratch,
          S, deam);
      const Status st = consume(res);
      if (!st.has_value()) {
        return st;
      }
    }
  } else {
    // Parallel driver (non-sequential): phase 1 fits every chain into its own
    // DISJOINT slot (theta_floor is always 0 here — the floor is a sequential-
    // only feature), one scratch arena per worker indexed by the fan-out's
    // worker id (race-free). Phase 2 walks the results in original chain order
    // and runs the SAME `consume` reduction, so slice order/count + FitDiag are
    // bit-identical to the serial path. A worker exception is recorded as a
    // failed slot — an escape would std::terminate the jthread.
    std::vector<ChainFitResult> results(n_chains);
    std::vector<FitScratch> scratch(nt);
    parallel_for_dynamic(n_chains, nt, [&](std::size_t i, unsigned wid) {
      try {
        results[i] = fit_one_chain(under.chains[i], curves, opts,
                                   /*sequential=*/false, /*theta_floor=*/0.0,
                                   prior, scratch[wid], S, deam);
      } catch (...) {
        results[i] = ChainFitResult{};  // failed slot (no obs, no slice)
      }
    });
    for (std::size_t i = 0; i < n_chains; ++i) {
      if (n_fit_ok >= surface.capacity()) {
        break;
      }
      const Status st = consume(results[i]);
      if (!st.has_value()) {
        return st;
      }
    }
  }

  // Backbone calendar projection (theta-scale "Project"-style bump). FT-C9a: this
  // moves the ATM total-variance level to remove calendar crossings — the
  // quality-destroying repair the README warns about. It is now an EXPLICIT
  // opt-in (essvi_alt_driver_theta_project), NOT folded into validate_no_arb, so
  // the default alternate-driver path leaves the fitted term structure untouched.
  if (n_fit_ok >= 2 && opts.essvi_alt_driver_theta_project) {
    (void)arb_project_calendar_essvi(surface, -1.5, 1.5, 64u);
  }

  // FIT-C1: honest post-fit no-arb audit. `opts.validate_no_arb`'s documented
  // contract (calib.hpp) is "run the static-arb validators at the end and
  // bail on a violation" — this driver used to leave the knob dead (the
  // out_diag block below unconditionally stamped n_butterfly_viol = 0 with no
  // check ever run, and nothing ever bailed). Scan the ASSEMBLED surface
  // (which includes the optional, un-projected wing-residual layer — see the
  // PORT NOTE on `fit_wing_residual`) for real calendar + butterfly
  // violations. Runs AFTER the opt-in theta-project repair above, so a caller
  // enabling BOTH knobs gets repair-then-audit.
  std::uint32_t n_calendar_viol = 0u;
  std::uint32_t n_butterfly_viol = 0u;
  if (n_fit_ok > 0 && opts.validate_no_arb) {
    ATX_TRY(const TotalSurfaceArbCounts arb_counts,
            arb_check_total_surface_all(surface, kNoArbAuditKMin, kNoArbAuditKMax,
                                        kNoArbAuditGrid));
    n_calendar_viol = arb_counts.n_calendar;
    n_butterfly_viol = arb_counts.n_butterfly;
  }

  const double rmse_global = (sum_w > 0.0) ? std::sqrt(sum_sse / sum_w) : 0.0;
  VolSurface::Diagnostics d{};
  d.rmse_vol = rmse_global;
  d.max_residual_vol = max_res;
  d.n_quotes_used = total_used;
  d.n_quotes_dropped = total_dropped;
  surface.set_diagnostics(d);

  if (out_diag != nullptr) {
    out_diag->rmse_vol_vega_weighted = rmse_global;
    out_diag->max_residual_vol = max_res;
    out_diag->outer_iters =
        static_cast<std::uint16_t>(std::min<std::uint32_t>(agg_outer, 0xFFFFu));
    out_diag->inner_iters_total =
        static_cast<std::uint16_t>(std::min<std::uint32_t>(agg_inner, 0xFFFFu));
    out_diag->n_quotes_used = total_used;
    // Real counts from the audit above (0 when opts.validate_no_arb left it
    // un-run, which is not itself a "verified clean" claim — see FitDiag).
    out_diag->n_butterfly_viol = n_butterfly_viol;
    out_diag->n_calendar_viol = n_calendar_viol;
  }

  if (n_fit_ok == 0) {
    return Err(ErrorCode::NotFound, "essvi_calib_surface: no slice fit");
  }
  if (opts.validate_no_arb && (n_calendar_viol > 0u || n_butterfly_viol > 0u)) {
    return Err(ErrorCode::Unavailable,
               "essvi_calib_surface: post-fit static-arb audit found " +
                   std::to_string(n_calendar_viol) + " calendar + " +
                   std::to_string(n_butterfly_viol) +
                   " butterfly violation(s); refusing to serve an "
                   "arbitrageable surface");
  }
  return Ok();
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

Result<EssviParams> essvi_fit_slice(std::span<const FitObs> obs, double T,
                                    double F, const CalibOpts& opts,
                                    FitDiag* out_diag, double theta_floor,
                                    const EssviParams* warm) {
  const Status option_status = validate_calib_options(opts);
  if (!option_status.has_value()) {
    return Err(option_status.error());
  }
  ThetaBand band = default_band(T);
  // Raise the cube's theta_lo to the floor (calendar-monotone seam), exactly as
  // calib_surface_impl does for the sequential driver. 0 / <= band.lo is a no-op.
  // Relative margin keeps theta_lo < theta_hi strictly even for a near-zero
  // theta_hi (see calib_surface_impl).
  if (theta_floor > band.lo) {
    const double margin = std::max(2.0e-12, 1.0e-9 * band.hi);
    band.lo = std::min(theta_floor, band.hi - margin);
  }
  // A per-call scratch arena: this public single-slice entry point is a pure
  // read of its inputs (safe to call concurrently), so it owns its own arena.
  // The surface drivers reuse ONE arena per worker across many slices instead.
  FitScratch scratch;
  return fit_core(obs, T, F, opts, band, out_diag, scratch, warm);
}

std::array<double, 3> essvi_w_cube_grad(const EssviParams& slice,
                                        double k_log) noexcept {
  const ThetaBand band = default_band(slice.T);
  const CubeGradFactors f = cube_grad_factors(slice.theta, slice.rho, slice.p, band);
  return cube_grad_from(essvi_w_grad3(slice, k_log), f);
}

Status essvi_calib_surface(VolSurface& surface, const Underlying& under,
                           const CurveSet& curves, const CalibOpts& opts,
                           FitDiag* out_diag, const VolSurface* prior,
                           unsigned n_workers, const DeAmOptions* deam) {
  return calib_surface_impl(surface, under, curves, opts, out_diag,
                            /*sequential=*/false, prior, n_workers, deam);
}

Status essvi_calib_surface_sequential(VolSurface& surface,
                                      const Underlying& under,
                                      const CurveSet& curves,
                                      const CalibOpts& opts, FitDiag* out_diag,
                                      const VolSurface* prior,
                                      const DeAmOptions* deam) {
  // Sequential driver: the theta-floor RAW loop-carry is inherently serial, so
  // it always takes the serial path (n_workers is inert here — pass 1).
  return calib_surface_impl(surface, under, curves, opts, out_diag,
                            /*sequential=*/true, prior, /*n_workers=*/1u, deam);
}

}  // namespace atx::vol
