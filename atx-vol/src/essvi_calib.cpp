#include "atx/vol/essvi_calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/linalg/solve.hpp"   // solve_spd
#include "atx/vol/arb.hpp"             // arb_project_calendar_essvi
#include "atx/vol/detail/robust.hpp"   // huber_weights_strided
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

// Below this year-fraction a chain is treated as expired and skipped by the
// surface driver (mirrors the C `T_MIN_FIT`: half an hour in year units).
constexpr double kTMinFit = 1.0 / (365.25 * 24.0 * 2.0);

// LM control constants (mirror the C fitter).
constexpr double kLambdaLmInit = 1.0e-3;
constexpr double kLambdaLmMax = 1.0e8;
constexpr double kLambdaLmMin = 1.0e-12;
constexpr int kLmTrialCap = 8;

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
[[nodiscard]] double cube_sse(std::span<const FitObs> obs,
                              std::span<const double> weights,
                              const ThetaBand& band, double T, double psi,
                              double p, double lambda) noexcept {
  const EssviParams s = slice_from_cube(psi, p, lambda, band, T);
  double acc = 0.0;
  for (std::size_t i = 0; i < obs.size(); ++i) {
    const double r = essvi_backbone_w(s, obs[i].k) - obs[i].w_mkt;
    acc += weights[i] * r * r;
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
void residuals_and_jac(std::span<const FitObs> obs, const ThetaBand& band,
                       double T, double psi, double p, double lambda,
                       std::span<double> r, std::span<double> jpsi,
                       std::span<double> jp, std::span<double> jl) noexcept {
  const EssviNatural nat = cube_to_natural(psi, p, lambda, band);
  EssviParams s{};
  s.theta = nat.theta;
  s.phi = nat.phi;
  s.rho = nat.rho;
  s.T = T;
  const CubeGradFactors f = cube_grad_factors(nat.theta, nat.rho, p, band);
  for (std::size_t i = 0; i < obs.size(); ++i) {
    const double k = obs[i].k;
    r[i] = essvi_backbone_w(s, k) - obs[i].w_mkt;
    const std::array<double, 3> dw = cube_grad_from(essvi_w_grad3(s, k), f);
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
                             double& lambda_lm, std::span<double> r,
                             std::span<double> jpsi, std::span<double> jp,
                             std::span<double> jl) {
  residuals_and_jac(obs, band, T, psi, p, lambda, r, jpsi, jp, jl);

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

  for (int trial = 0; trial < kLmTrialCap; ++trial) {
    const double d = 1.0 + lambda_lm;
    MatX hd(3, 3);
    hd(0, 0) = h00 * d;
    hd(0, 1) = h01;
    hd(0, 2) = h02;
    hd(1, 0) = h01;
    hd(1, 1) = h11 * d;
    hd(1, 2) = h12;
    hd(2, 0) = h02;
    hd(2, 1) = h12;
    hd(2, 2) = h22 * d;
    VecX ng(3);
    ng(0) = -g0;
    ng(1) = -g1;
    ng(2) = -g2;

    const auto sol = atx::core::linalg::solve_spd(hd, ng);
    if (!sol.has_value()) {
      lambda_lm *= 10.0;
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

    const double new_sse =
        cube_sse(obs, weights, band, T, psi_new, p_new, lambda_new);
    if (new_sse < prev_sse) {
      psi = psi_new;
      p = p_new;
      lambda = lambda_new;
      lambda_lm *= 0.5;
      if (lambda_lm < kLambdaLmMin) {
        lambda_lm = kLambdaLmMin;
      }
      return new_sse;
    }
    lambda_lm *= 10.0;
    if (lambda_lm > kLambdaLmMax) {
      return prev_sse;  // stuck at (or near) the minimum
    }
  }
  return prev_sse;
}

// Lee (2004) wing-slope projection: if θ·φ·(1+|ρ|) > 4/T shrink `p` (which
// scales φ) by bisection to the largest admissible value. Returns true if it
// fired. The Mingone cube already enforces the (T-free) butterfly bound; this
// adds Lee's tighter short-dated bound.
bool lee_project(double& psi, double& p, double& lambda, const ThetaBand& band,
                 double T) noexcept {
  const EssviNatural n = cube_to_natural(psi, p, lambda, band);
  if (!(T > 0.0) || !(n.theta > 0.0) || !(n.phi > 0.0)) {
    return false;
  }
  const double rhs = 4.0 / T;
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

// Profile-aware outer IRLS cap (mirrors `ats_vol_calib_outer_cap`).
[[nodiscard]] std::uint16_t outer_cap(const CalibOpts& opts) noexcept {
  std::uint16_t c = 0;
  switch (opts.optimization_level) {
    case OptimizationLevel::QuickMark:
      c = opts.max_iter_quick_mark;
      break;
    case OptimizationLevel::Trading:
      c = opts.max_iter_trading;
      break;
    case OptimizationLevel::Risk:
      c = opts.max_iter_risk;
      break;
    case OptimizationLevel::Reference:
      c = opts.max_iter_reference;
      break;
    case OptimizationLevel::ColdFast:
      c = opts.max_iter_cold_fast;
      break;
  }
  if (c == 0) {
    c = (opts.max_outer_iter > 0) ? opts.max_outer_iter : 4;
  }
  return c;
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
// / total-surface projectors), so it is deferred here: the fitted residual is
// left un-projected and its static-arb safety is a caller/surface-level check.
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

// ── Per-slice fit core (parameterized by the theta band) ─────────────────
[[nodiscard]] Result<EssviParams> fit_core(std::span<const FitObs> obs, double T,
                                           double F, const CalibOpts& opts,
                                           const ThetaBand& band,
                                           FitDiag* out_diag) {
  if (obs.empty() || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "essvi_fit_slice: empty observations or non-positive T");
  }
  const std::size_t n = obs.size();

  // Base (fixed) weights and the IRLS-mutable active copy.
  std::vector<double> base_w(n);
  std::vector<double> active(n);
  for (std::size_t i = 0; i < n; ++i) {
    base_w[i] = (obs[i].weight_w > 0.0) ? obs[i].weight_w : 1.0;
    active[i] = base_w[i];
  }

  // Cube seed: psi from the ATM total variance, neutral p / lambda.
  double psi = 0.5;
  double p = 0.3;
  double lambda = 0.4;
  {
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

  const std::uint16_t max_outer = outer_cap(opts);
  const std::uint16_t max_inner =
      (opts.max_inner_iter > 0) ? opts.max_inner_iter : 12;
  const double tol_param = (opts.tol_param > 0.0) ? opts.tol_param : 1.0e-9;

  // LM scratch (residuals + 3 Jacobian columns).
  std::vector<double> r(n);
  std::vector<double> jpsi(n);
  std::vector<double> jp(n);
  std::vector<double> jl(n);
  std::vector<double> r_abs(n);
  std::vector<double> hw(n);

  std::uint16_t outer_iters = 0;
  std::uint16_t inner_total = 0;
  double prev_outer_sse = std::numeric_limits<double>::infinity();

  for (std::uint16_t outer = 0; outer < max_outer; ++outer) {
    double sse = cube_sse(obs, active, band, T, psi, p, lambda);
    double lambda_lm = kLambdaLmInit;
    for (std::uint16_t inner = 0; inner < max_inner; ++inner) {
      const double psi_old = psi;
      const double p_old = p;
      const double lambda_old = lambda;
      const double new_sse = lm_step(obs, active, band, T, psi, p, lambda, sse,
                                     lambda_lm, r, jpsi, jp, jl);
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
    if (std::fabs(prev_outer_sse - sse) < 1.0e-15) {
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

  // Optional additive wing-residual layer (fit on the converged backbone).
  if (!opts.residual_disable) {
    fit_wing_residual(obs, slice, opts);
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

// Shared chain walk for both surface drivers. `sequential` raises the theta
// floor to the previous slice's theta.
[[nodiscard]] Status calib_surface_impl(VolSurface& surface,
                                        const Underlying& under,
                                        const CurveSet& curves,
                                        const CalibOpts& opts,
                                        FitDiag* out_diag, bool sequential) {
  if (surface.param() != Parametrization::Essvi) {
    return Err(ErrorCode::InvalidArgument,
               "essvi_calib_surface: surface is not eSSVI-parametrized");
  }
  if (under.chains.empty()) {
    return Err(ErrorCode::NotFound, "essvi_calib_surface: no chains");
  }

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

  for (const Chain& c : under.chains) {
    if (n_fit_ok >= surface.capacity()) {
      break;
    }
    const double T = c.T;
    if (!(T > kTMinFit)) {
      continue;
    }
    const double F = curves.forward.forward_at(c.expiry_id);
    if (!std::isfinite(F) || !(F > 0.0)) {
      continue;
    }
    const double df = curves.yield.disc(T);

    const Result<ObsSet> obs_res = build_observations(c, F, T, df, opts);
    if (!obs_res.has_value()) {
      continue;  // too few survivors / malformed chain: skip this expiry
    }
    const ObsSet& os = *obs_res;
    total_dropped += os.n_dropped;
    if (os.obs.size() < opts.min_obs_per_slice) {
      continue;
    }

    // Theta band: default, or floored at the previous slice's theta.
    ThetaBand band = default_band(T);
    if (sequential && theta_floor > band.lo) {
      band.lo = std::min(theta_floor, band.hi - 1.0e-12);
    }

    FitDiag diag{};
    const Result<EssviParams> fit =
        fit_core(std::span<const FitObs>{os.obs}, T, F, opts, band, &diag);
    if (!fit.has_value()) {
      continue;  // LM failure: skip (fallback machinery deferred, see PORT NOTE)
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
        continue;
      }
    }

    const Status set_st = surface.set_slice_essvi(n_fit_ok, slice);
    if (!set_st.has_value()) {
      return set_st;  // capacity / parametrization mismatch (should not happen)
    }
    theta_floor = slice.theta;

    total_used += diag.n_quotes_used;
    sum_sse += diag.rmse_vol_vega_weighted * diag.rmse_vol_vega_weighted *
               static_cast<double>(diag.n_quotes_used);
    sum_w += static_cast<double>(diag.n_quotes_used);
    max_res = std::max(max_res, diag.max_residual_vol);
    agg_outer += diag.outer_iters;
    agg_inner += diag.inner_iters_total;
    ++n_fit_ok;
  }

  // Backbone calendar projection (no-op on an already-monotone surface). The
  // sequential driver produces a theta-monotone term structure by construction,
  // but the wing (φ, ρ) coupling can still induce small calendar crossings, so
  // the projection is run for both drivers when validation is on.
  if (n_fit_ok >= 2 && opts.validate_no_arb) {
    (void)arb_project_calendar_essvi(surface, -1.5, 1.5, 64u);
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
  }

  if (n_fit_ok == 0) {
    return Err(ErrorCode::NotFound, "essvi_calib_surface: no slice fit");
  }
  return Ok();
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

Result<EssviParams> essvi_fit_slice(std::span<const FitObs> obs, double T,
                                    double F, const CalibOpts& opts,
                                    FitDiag* out_diag, double theta_floor) {
  ThetaBand band = default_band(T);
  // Raise the cube's theta_lo to the floor (calendar-monotone seam), exactly as
  // calib_surface_impl does for the sequential driver. 0 / <= band.lo is a no-op.
  if (theta_floor > band.lo) {
    band.lo = std::min(theta_floor, band.hi - 1.0e-12);
  }
  return fit_core(obs, T, F, opts, band, out_diag);
}

std::array<double, 3> essvi_w_cube_grad(const EssviParams& slice,
                                        double k_log) noexcept {
  const ThetaBand band = default_band(slice.T);
  const CubeGradFactors f = cube_grad_factors(slice.theta, slice.rho, slice.p, band);
  return cube_grad_from(essvi_w_grad3(slice, k_log), f);
}

Status essvi_calib_surface(VolSurface& surface, const Underlying& under,
                           const CurveSet& curves, const CalibOpts& opts,
                           FitDiag* out_diag) {
  return calib_surface_impl(surface, under, curves, opts, out_diag,
                            /*sequential=*/false);
}

Status essvi_calib_surface_sequential(VolSurface& surface,
                                      const Underlying& under,
                                      const CurveSet& curves,
                                      const CalibOpts& opts, FitDiag* out_diag) {
  return calib_surface_impl(surface, under, curves, opts, out_diag,
                            /*sequential=*/true);
}

}  // namespace atx::vol
