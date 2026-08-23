#include "atx/vol/api/fitting/vol_surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

#include "atx/core/error.hpp"
#include "fitting/resid_basis.hpp"  // dense C2 residual basis (shared with calibrator)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Reparametrization box (ports the C's ats_vol_essvi reparam constants).
constexpr double kRhoMax = 0.999;
constexpr double kPhiMin = 1.0e-4;
constexpr double kSigmaLow = 0.005;
constexpr double kSigmaHigh = 5.000;
constexpr double kDefaultT = 1.0 / 365.25;

// Wing-residual dead-band half-width (INNER_Y in the C).
constexpr double kResidInnerY = 0.4;

// Slice-T match tolerance for find_exact_T: one bar in a 252d * 6.5h * 60m
// trading year.
constexpr double kExactTTol = 1.0 / (252.0 * 6.5 * 60.0);

// T with the "degenerate expiry" default the C applies before scaling theta.
[[nodiscard]] double effective_T(double T) noexcept {
  return (T > 0.0) ? T : kDefaultT;
}

}  // namespace

// ── eSSVI evaluators ─────────────────────────────────────────────────────

bool essvi_rho_blend_armed(const EssviParams& s) noexcept {
  return (s.rho_scale > 0.0) && (s.rho_R != s.rho);
}

double essvi_backbone_w(const EssviParams& s, double k_log) noexcept {
  if (essvi_rho_blend_armed(s)) {
    return kNaN;
  }
  const double theta = s.theta;
  const double phi = s.phi;
  const double rho = s.rho;
  const double pk = phi * k_log;
  const double inner = (pk + rho) * (pk + rho) + (1.0 - rho * rho);
  return 0.5 * theta * (1.0 + rho * pk + std::sqrt(inner));
}

double essvi_residual_w(const EssviParams& s, double k_log) noexcept {
  if (!(s.resid_scale > 0.0)) {
    return 0.0;
  }
  double y = k_log / s.resid_scale;
  y = std::clamp(y, -1.0, 1.0);

  int n_basis = (s.resid_n_basis != 0) ? static_cast<int>(s.resid_n_basis) : 5;
  n_basis = std::clamp(n_basis, 1, 16);

  std::array<double, 16> basis{};
  if (s.resid_basis_kind == ResidualBasisKind::C2Bspline) {
    // Dense full-smile C2 bump basis (near-money capable). The identical
    // evaluator the calibrator fit against — see detail/resid_basis.hpp.
    //
    // T4: `resid_bump_basis` re-clamps the count to [4, 16] internally, so a
    // params carrying `resid_n_basis` in {1, 2, 3} had FOUR bumps built while
    // the dot product below summed only the first 1-3 — the surface SERVED a
    // different total variance than was calibrated. The clamp is stated once,
    // in resid_basis.hpp, and read here; `essvi_slice_dof` reads the same one.
    n_basis = detail::resid_bump_count(n_basis);
    detail::resid_bump_basis(y, n_basis, basis);
  } else {
    // HINGE_QUAD (the shipped wing-only runtime basis): out[0] = 0; a clamped
    // hinge (+ its square) on each wing outside the dead band; out[>=5] = 0.
    // Chebyshev / WingBspline / Fengler tags fall through to this so a hot-path
    // eval always stays finite (see header).
    const double yp = (y < -kResidInnerY) ? (-y - kResidInnerY) : 0.0;
    const double yc = (y > kResidInnerY) ? (y - kResidInnerY) : 0.0;
    basis[1] = yp;
    basis[2] = yp * yp;
    basis[3] = yc;
    basis[4] = yc * yc;
  }

  double dw = 0.0;
  for (int j = 0; j < n_basis; ++j) {
    const auto idx = static_cast<std::size_t>(j);
    dw += s.resid_coef[idx] * basis[idx];
  }
  return dw;
}

double essvi_total_w(const EssviParams& s, double k_log) noexcept {
  const double w_back = essvi_backbone_w(s, k_log);
  if (!std::isfinite(w_back)) {
    return w_back;
  }
  if (!(s.resid_scale > 0.0)) {
    return w_back;
  }
  double w = w_back + essvi_residual_w(s, k_log);
  if (!(w > 0.0)) {
    w = 1.0e-12;  // hot-path positivity net
  }
  return w;
}

std::size_t essvi_slice_dof(const EssviParams& s) noexcept {
  // Backbone: one Mingone cube (psi, p, lambda) per slice — 3 fitted numbers.
  std::size_t dof = 3u;
  if (!(s.resid_scale > 0.0)) {
    return dof;  // residual disarmed: `essvi_total_w` serves the backbone only
  }
  if (s.resid_basis_kind == ResidualBasisKind::C2Bspline) {
    // `fit_dense_residual` writes exactly `resid_n_basis` fitted bump
    // coefficients; mirror `essvi_residual_w`'s 0 => 5 default and go through
    // THE clamp (`resid_bump_count`, resid_basis.hpp) so the dof counts exactly
    // the bumps the evaluator serves. T4: a private `min(n, 16)` here reported 1
    // to 3 for a params the evaluator serves on four bumps.
    const int requested = (s.resid_n_basis != 0u) ? static_cast<int>(s.resid_n_basis) : 5;
    dof += static_cast<std::size_t>(detail::resid_bump_count(std::clamp(requested, 1, 16)));
  } else {
    // HingeQuad (and the legacy tags that evaluate through its branch): the
    // fitter writes slots 1..4 ({yp, yp^2, yc, yc^2}); slot 0 is structurally
    // zero in the evaluator, so `resid_n_basis == 5` carries FOUR fitted
    // coefficients, not five.
    dof += 4u;
  }
  return dof;
}

std::array<double, 3> essvi_w_grad3(const EssviParams& s, double k_log) noexcept {
  if (essvi_rho_blend_armed(s)) {
    return {kNaN, kNaN, kNaN};
  }
  const double theta = s.theta;
  const double phi = s.phi;
  const double rho = s.rho;
  const double pk = phi * k_log;
  const double a = pk + rho;
  const double inner = a * a + (1.0 - rho * rho);
  const double r = std::sqrt(inner);
  const double dwdth = 0.5 * (1.0 + rho * pk + r);
  const double dwdphi = 0.5 * theta * (rho * k_log + (a * k_log) / r);
  const double dwdrho = 0.5 * theta * (pk + (a - rho) / r);
  return {dwdth, dwdphi, dwdrho};
}

std::array<double, 4> essvi_w_grad4(const EssviParams& s, double k_log) noexcept {
  if (essvi_rho_blend_armed(s)) {
    return {kNaN, kNaN, kNaN, kNaN};
  }
  const std::array<double, 3> g = essvi_w_grad3(s, k_log);
  // A slice has ONE rho, so all of the rho sensitivity sits on rho_L and the
  // rho_R partial is structurally zero — not "zero in the symmetric case".
  return {g[0], g[1], g[2], 0.0};
}

// ── Raw SVI ──────────────────────────────────────────────────────────────

double svi_total_w(const SviParams& s, double k_log) noexcept {
  const double dk = k_log - s.m;
  const double r = std::sqrt(dk * dk + s.sigma * s.sigma);
  return s.a + s.b * (s.rho * dk + r);
}

// ── Mingone cube reparametrization ───────────────────────────────────────

double essvi_phi_max(double theta, double rho) noexcept {
  if (!(theta > 0.0)) {
    return 0.0;
  }
  const double ar = std::fabs(rho);
  if (!(ar < 1.0)) {
    return 0.0;
  }
  const double s = theta * (1.0 + ar);
  const double b1 = 4.0 / s;
  const double b2 = 2.0 / std::sqrt(s);
  return std::min(b1, b2);
}

EssviNatural essvi_reparam_to_natural(double psi, double p, double lambda,
                                      double T) noexcept {
  psi = std::clamp(psi, 0.0, 1.0);
  p = std::clamp(p, 0.0, 1.0);
  lambda = std::clamp(lambda, 0.0, 1.0);

  const double Te = effective_T(T);
  const double rho = -kRhoMax + 2.0 * kRhoMax * lambda;
  const double th_lo = kSigmaLow * kSigmaLow * Te;
  const double th_hi = kSigmaHigh * kSigmaHigh * Te;
  const double theta = th_lo + psi * (th_hi - th_lo);
  const double phi_hi = essvi_phi_max(theta, rho);
  const double phi = kPhiMin + p * (phi_hi - kPhiMin);
  return {theta, phi, rho};
}

EssviCube essvi_natural_to_reparam(double theta, double phi, double rho,
                                   double T) noexcept {
  const double Te = effective_T(T);
  const double th_lo = kSigmaLow * kSigmaLow * Te;
  const double th_hi = kSigmaHigh * kSigmaHigh * Te;
  const double psi = std::clamp((theta - th_lo) / (th_hi - th_lo), 0.0, 1.0);
  const double lambda = std::clamp((rho + kRhoMax) / (2.0 * kRhoMax), 0.0, 1.0);
  const double rho_recov = -kRhoMax + 2.0 * kRhoMax * lambda;
  const double th_recov = th_lo + psi * (th_hi - th_lo);
  const double phi_hi = essvi_phi_max(th_recov, rho_recov);
  const double p = (phi_hi > kPhiMin)
                       ? std::clamp((phi - kPhiMin) / (phi_hi - kPhiMin), 0.0,
                                    1.0)
                       : 0.0;
  return {psi, p, lambda};
}

double essvi_rho_from_lambda(double lambda) noexcept {
  return -kRhoMax + 2.0 * kRhoMax * std::clamp(lambda, 0.0, 1.0);
}

double essvi_lambda_from_rho(double rho) noexcept {
  return std::clamp((rho + kRhoMax) / (2.0 * kRhoMax), 0.0, 1.0);
}

// ── VolSurface ───────────────────────────────────────────────────────────

Result<VolSurface> VolSurface::create(std::uint32_t uid, Parametrization param,
                                      std::size_t cap_slices) {
  if (cap_slices == 0) {
    return Err(ErrorCode::InvalidArgument,
               "VolSurface::create: cap_slices must be > 0");
  }
  VolSurface surf;
  surf.uid_ = uid;
  surf.param_ = param;
  surf.cap_slices_ = cap_slices;
  switch (param) {
  case Parametrization::Essvi:
    surf.essvi_.reserve(cap_slices);
    break;
  case Parametrization::Svi:
  case Parametrization::SviMm:
    surf.svi_.reserve(cap_slices);
    break;
  case Parametrization::Wing:
  case Parametrization::C8:
  case Parametrization::CStar16M:
    break;  // tag-only: no evaluatable slice storage
  }
  return Ok(std::move(surf));
}

Status VolSurface::set_slice_essvi(std::size_t idx, const EssviParams& slice) {
  if (param_ != Parametrization::Essvi) {
    return Err(ErrorCode::InvalidArgument,
               "VolSurface::set_slice_essvi: surface is not eSSVI-parametrized");
  }
  if (idx >= cap_slices_) {
    return Err(ErrorCode::OutOfRange,
               "VolSurface::set_slice_essvi: idx exceeds capacity");
  }
  if (idx >= essvi_.size()) {
    essvi_.resize(idx + 1);
  }
  essvi_[idx] = slice;
  return Ok();
}

Status VolSurface::set_slice_svi(std::size_t idx, const SviParams& slice) {
  if (param_ != Parametrization::Svi && param_ != Parametrization::SviMm) {
    return Err(ErrorCode::InvalidArgument,
               "VolSurface::set_slice_svi: surface is not SVI-parametrized");
  }
  if (idx >= cap_slices_) {
    return Err(ErrorCode::OutOfRange,
               "VolSurface::set_slice_svi: idx exceeds capacity");
  }
  if (idx >= svi_.size()) {
    svi_.resize(idx + 1);
  }
  svi_[idx] = slice;
  return Ok();
}

std::size_t VolSurface::n_slices() const noexcept {
  switch (param_) {
  case Parametrization::Essvi:
    return essvi_.size();
  case Parametrization::Svi:
  case Parametrization::SviMm:
    return svi_.size();
  case Parametrization::Wing:
  case Parametrization::C8:
  case Parametrization::CStar16M:
    return 0;
  }
  return 0;  // unreachable for valid enumerators
}

double VolSurface::eval_slice_w(std::size_t idx, double k_log) const noexcept {
  switch (param_) {
  case Parametrization::Essvi:
    return essvi_total_w(essvi_[idx], k_log);
  case Parametrization::Svi:
  case Parametrization::SviMm:
    return svi_total_w(svi_[idx], k_log);
  case Parametrization::Wing:
  case Parametrization::C8:
  case Parametrization::CStar16M:
    return kNaN;
  }
  return kNaN;  // unreachable for valid enumerators
}

double VolSurface::slice_T(std::size_t idx) const noexcept {
  switch (param_) {
  case Parametrization::Essvi:
    return essvi_[idx].T;
  case Parametrization::Svi:
  case Parametrization::SviMm:
    return svi_[idx].T;
  case Parametrization::Wing:
  case Parametrization::C8:
  case Parametrization::CStar16M:
    return kNaN;
  }
  return kNaN;  // unreachable for valid enumerators
}

double VolSurface::w(double k_log, double T) const noexcept {
  const std::size_t n = n_slices();
  if (n == 0) {
    return kNaN;
  }
  if (T < kTMinEval) {
    T = kTMinEval;
  }

  const double T0 = slice_T(0);
  if (T <= T0) {
    // Sprint-26 short-T extrapolation guard: refuse to extrapolate when the
    // query sits materially below the first slice.
    if (T < 0.5 * T0) {
      return kNaN;
    }
    return eval_slice_w(0, k_log);
  }
  // Exact hit at the longest slice must evaluate it, not fall into the
  // "T > last" extrapolation-forbidden branch.
  const double T_last = slice_T(n - 1);
  if (T == T_last) {
    return eval_slice_w(n - 1, k_log);
  }
  if (T > T_last) {
    return kNaN;  // extrapolation past the longest slice is never allowed
  }

  // Binary search for the bracket [lo, hi] with T_lo <= T < T_hi.
  std::size_t lo = 0;
  std::size_t hi = n - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    if (slice_T(mid) <= T) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double T_lo = slice_T(lo);
  const double T_hi = slice_T(hi);
  const double w_lo = eval_slice_w(lo, k_log);
  const double w_hi = eval_slice_w(hi, k_log);
  const double alpha = (T - T_lo) / (T_hi - T_lo);
  return w_lo + alpha * (w_hi - w_lo);
}

double VolSurface::iv(double k_log, double T) const noexcept {
  // The divisor below is the caller's raw T, so it — not just w() — decides
  // whether this query has an answer. w() FLOORS its argument to kTMinEval, so a
  // T <= 0 query against a surface whose first slice sits at or inside
  // 2 * kTMinEval (a 0DTE board in its last minutes) clears w()'s short-T guard
  // and returns a finite positive variance; sqrt(w / 0.0) then handed back +inf
  // as an implied vol. There is no vol at zero (or negative, or infinite) time
  // to expiry: refuse, as everywhere else the evaluation cannot be made.
  if (!(T > 0.0) || !std::isfinite(T)) {
    return kNaN;
  }
  const double wv = w(k_log, T);
  if (!std::isfinite(wv) || wv <= 0.0) {
    return kNaN;
  }
  // Divide by the caller's ORIGINAL T, not the floored value w() brackets on
  // (matches the C's ats_vol_surface_iv).
  return std::sqrt(wv / T);
}

std::uint16_t VolSurface::find_exact_T(double T_query) const noexcept {
  const std::size_t n = n_slices();
  for (std::size_t i = 0; i < n; ++i) {
    if (std::fabs(slice_T(i) - T_query) <= kExactTTol) {
      return static_cast<std::uint16_t>(i);
    }
  }
  return 0xFFFF;
}

double VolSurface::iv_on_slice(std::uint16_t slice_idx,
                               double k_log) const noexcept {
  const auto idx = static_cast<std::size_t>(slice_idx);
  if (idx >= n_slices()) {
    return kNaN;
  }
  const double Ts = slice_T(idx);
  if (!(Ts > 0.0)) {
    return kNaN;
  }
  const double wv = eval_slice_w(idx, k_log);
  if (!std::isfinite(wv) || wv <= 0.0) {
    return kNaN;
  }
  return std::sqrt(wv / Ts);
}

}  // namespace atx::vol
