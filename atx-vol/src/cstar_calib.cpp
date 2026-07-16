#include "atx/vol/cstar_calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/solve.hpp"  // solve_spd, MatX, VecX
#include "atx/vol/arb.hpp"           // arb_check_butterfly_slice
#include "atx/vol/black76.hpp"        // black76_price, black76_value_and_vega
#include "atx/vol/detail/calib_shared.hpp"  // shared LM damping constants
#include "atx/vol/detail/robust.hpp"  // huber_weights_strided

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::linalg::MatX;
using atx::core::linalg::solve_spd;
using atx::core::linalg::VecX;

namespace {

// LM / IRLS constants (ats_vol_cstar_internal.h). The LM damping schedule is
// shared with the eSSVI and SVI-MM fitters — canonical values live in
// atx/vol/detail/calib_shared.hpp (routed here so they cannot drift). The
// remaining constants are CStar-specific and deliberately NOT shared: the
// inner-block cap is a fixed 12 (a different role from the opts-driven fallback
// default), and the IRLS-outer count / total-iteration cap / Huber threshold
// have no counterpart in the other fitters.
constexpr double kLambdaInit = detail::kLambdaLmInit;
constexpr double kLambdaGrow = detail::kLambdaGrow;
constexpr double kLambdaShrink = detail::kLambdaShrink;
constexpr double kLambdaMax = detail::kLambdaLmMax;
constexpr double kLambdaMin = detail::kLambdaLmMin;
constexpr int kLmInnerMax = 12;
constexpr int kIrlsOuterMax = 4;
constexpr int kTotalIterCap = 72;
constexpr double kHuberK = 1.345;

// Solve the Marquardt-damped normal equations (H + λ·diag(H))·dx = −g via
// atx-core's SPD solver. Returns nullopt if the damped system is not
// positive-definite (caller grows λ and retries — mirrors the C's Cholesky
// failure branch). `H` is symmetric by construction.
[[nodiscard]] std::optional<VecX> solve_damped(const MatX& H, const VecX& g,
                                               double lambda) {
  const Eigen::Index d = H.rows();
  MatX Hd = H;
  for (Eigen::Index j = 0; j < d; ++j) {
    Hd(j, j) *= (1.0 + lambda);
    if (Hd(j, j) < 1.0e-18) {
      Hd(j, j) = 1.0e-18;
    }
  }
  auto res = solve_spd(Hd, VecX(-g));
  if (!res) {
    return std::nullopt;
  }
  return *res;
}

// ── Vol-domain block normal equations ──────────────────────────────────────

struct NormalEq {
  MatX H;
  VecX g;
  double sse{};
  int n_used{};
};

[[nodiscard]] NormalEq build_normal_eq_w(const CStarParams& s, CStarBlock block,
                                         int dim, std::span<const double> k_log,
                                         std::span<const double> w_target,
                                         std::span<const double> spread_w,
                                         std::span<const double> w_obs) {
  NormalEq ne;
  ne.H = MatX::Zero(dim, dim);
  ne.g = VecX::Zero(dim);

  const std::size_t n_obs = k_log.size();
  std::array<double, kCStarNParams> row_buf{};
  for (std::size_t i = 0; i < n_obs; ++i) {
    const double w_model = cstar_slice_w(s, k_log[i]);
    if (!std::isfinite(w_model)) {
      continue;
    }
    const double sd =
        (!spread_w.empty() && spread_w[i] > 1.0e-12) ? spread_w[i] : 1.0;
    const double w_w = w_obs.empty() ? 1.0 : w_obs[i];
    if (!(w_w > 0.0)) {
      continue;
    }
    const auto grad = cstar_slice_grad_w(s, k_log[i]);
    if (!grad) {
      continue;
    }
    const int row_dim = cstar_extract_block_grad(
        *grad, block, s.active_modes, std::span<double>{row_buf});
    if (row_dim != dim) {
      continue;
    }

    const double rw = std::sqrt(w_w);
    const double r = rw * (w_model - w_target[i]) / sd;
    ne.sse += r * r;
    ++ne.n_used;
    const double row_scale = rw / sd;
    for (int j = 0; j < dim; ++j) {
      row_buf[static_cast<std::size_t>(j)] *= row_scale;
    }
    for (int j = 0; j < dim; ++j) {
      const double rj = row_buf[static_cast<std::size_t>(j)];
      ne.g(j) += rj * r;
      for (int kk = 0; kk <= j; ++kk) {
        ne.H(j, kk) += rj * row_buf[static_cast<std::size_t>(kk)];
      }
    }
  }
  for (int j = 0; j < dim; ++j) {
    for (int kk = j + 1; kk < dim; ++kk) {
      ne.H(j, kk) = ne.H(kk, j);
    }
  }
  return ne;
}

}  // namespace

// ── Vol-domain block LM ─────────────────────────────────────────────────────

Result<CStarLmStatus> cstar_lm_inner_block_w(
    CStarParams& slice, CStarBlock block, std::span<const double> k_log,
    std::span<const double> w_target, std::span<const double> spread_w,
    std::span<const double> w_obs, int max_inner_iters, double& lambda_inout) {
  if (k_log.empty() || w_target.size() != k_log.size() || max_inner_iters <= 0) {
    return Err(ErrorCode::InvalidArgument,
               "cstar_lm_inner_block_w: invalid target spans or iteration cap");
  }
  const int dim = cstar_block_dim(block, slice.active_modes);
  if (dim <= 0) {
    return Ok(CStarLmStatus::Accepted);  // nothing to fit; not an error
  }

  double lambda = (lambda_inout > 0.0) ? lambda_inout : kLambdaInit;
  NormalEq ne =
      build_normal_eq_w(slice, block, dim, k_log, w_target, spread_w, w_obs);

  bool accepted = false;
  for (int it = 0; it < max_inner_iters; ++it) {
    auto dx = solve_damped(ne.H, ne.g, lambda);
    if (!dx) {
      lambda *= kLambdaGrow;
      if (lambda > kLambdaMax) {
        break;
      }
      continue;
    }

    CStarParams trial = slice;
    cstar_apply_block_step(
        trial, block,
        std::span<const double>{dx->data(), static_cast<std::size_t>(dx->size())});

    NormalEq ne_trial =
        build_normal_eq_w(trial, block, dim, k_log, w_target, spread_w, w_obs);
    if (ne_trial.sse < ne.sse) {
      slice = trial;
      ne = std::move(ne_trial);
      lambda *= kLambdaShrink;
      if (lambda < kLambdaMin) {
        lambda = kLambdaMin;
      }
      accepted = true;
      const double norm_dx =
          std::sqrt(dx->squaredNorm() / static_cast<double>(dim));
      if (norm_dx < 1.0e-7) {
        break;
      }
    } else {
      // Reject: `ne` already reflects the current (unchanged) slice, so no
      // rebuild is needed (the C rebuilt because it aliased one H buffer).
      lambda *= kLambdaGrow;
      if (lambda > kLambdaMax) {
        break;
      }
    }
  }

  lambda_inout = lambda;
  return Ok(accepted ? CStarLmStatus::Accepted : CStarLmStatus::Exhausted);
}

// ── Seed from eSSVI ─────────────────────────────────────────────────────────

namespace {

// Closed-form base match (θ, s2, c2, C_left, C_right) to an eSSVI slice.
[[nodiscard]] bool fit_base_from_essvi(const EssviParams& src,
                                       CStarParams& dst) {
  const double w0 = essvi_total_w(src, 0.0);
  if (!(w0 > 0.0)) {
    return false;
  }
  dst.theta = w0;
  const double sqrt_theta = std::sqrt(w0);

  // ATM derivatives by central FD in k-space: s2 = ½·(∂w/∂k|0) / √θ.
  constexpr double h = 1.0e-3;
  const double wp1 = essvi_total_w(src, +h);
  const double wm1 = essvi_total_w(src, -h);
  const double dw_dk = (wp1 - wm1) / (2.0 * h);
  dst.s2 = 0.5 * dw_dk / sqrt_theta;

  // d²w/dk²|0 = f''(0) = 2·c2  ⇒  c2 = ½·d²w/dk²|0.
  const double d2w_dk2 = (wp1 - 2.0 * w0 + wm1) / (h * h);
  dst.c2 = 0.5 * d2w_dk2;

  // Asymptotic wing slopes from f at z = ±2, ±4: C ≈ (w_far − w_near)/2 / θ.
  const double w_pos_far = essvi_total_w(src, +4.0 * sqrt_theta);
  const double w_pos_near = essvi_total_w(src, +2.0 * sqrt_theta);
  const double w_neg_far = essvi_total_w(src, -4.0 * sqrt_theta);
  const double w_neg_near = essvi_total_w(src, -2.0 * sqrt_theta);
  dst.C_right = (w_pos_far - w_pos_near) / 2.0 / w0;
  dst.C_left = (w_neg_far - w_neg_near) / (-2.0) / w0;
  if (!(dst.C_left > 0.0)) {
    dst.C_left = 1.0e-3;
  }
  if (!(dst.C_right > 0.0)) {
    dst.C_right = 1.0e-3;
  }
  return true;
}

// Ridge-LSQ the 11 modal coefficients against (w_essvi − w_base) at the 41
// z-knots, in normalized f-space (θ cancels). All-zero on an under-determined
// or non-PD system.
void fit_modes_from_essvi_residuals(CStarParams& dst, const EssviParams& src) {
  const double sqrt_theta = std::sqrt(dst.theta);

  std::vector<std::array<double, kCStarNModes>> A;
  std::vector<double> residuals;
  A.reserve(static_cast<std::size_t>(kCStarSeedGridN));
  residuals.reserve(static_cast<std::size_t>(kCStarSeedGridN));
  for (int i = 0; i < kCStarSeedGridN; ++i) {
    const double z = -5.0 + 10.0 * static_cast<double>(i) /
                               static_cast<double>(kCStarSeedGridN - 1);
    const double k = z * sqrt_theta;
    const double w_essvi = essvi_total_w(src, k);
    if (!std::isfinite(w_essvi) || !(w_essvi > 0.0)) {
      continue;
    }
    const double f_essvi = w_essvi / dst.theta;
    const double f_base = cstar_base(z, dst.s2, dst.c2, dst.C_left, dst.C_right);
    std::array<double, kCStarNModes> arow{};
    for (std::size_t j = 0; j < kCStarNModes; ++j) {
      arow[j] = cstar_basis(static_cast<int>(j), z);
    }
    A.push_back(arow);
    residuals.push_back(f_essvi - f_base);
  }

  dst.beta = {};
  if (A.size() < kCStarNModes) {
    return;
  }

  const auto dim = static_cast<Eigen::Index>(kCStarNModes);
  MatX M = MatX::Zero(dim, dim);
  VecX rhs = VecX::Zero(dim);
  for (std::size_t i = 0; i < A.size(); ++i) {
    for (Eigen::Index j = 0; j < dim; ++j) {
      const double aij = A[i][static_cast<std::size_t>(j)];
      rhs(j) += aij * residuals[i];
      for (Eigen::Index kk = 0; kk <= j; ++kk) {
        M(j, kk) += aij * A[i][static_cast<std::size_t>(kk)];
      }
    }
  }
  for (Eigen::Index j = 0; j < dim; ++j) {
    for (Eigen::Index kk = j + 1; kk < dim; ++kk) {
      M(j, kk) = M(kk, j);
    }
    M(j, j) += kCStarRidgePerMode[static_cast<std::size_t>(j)];
  }

  auto sol = solve_spd(M, rhs);
  if (!sol) {
    return;  // beta already zeroed above
  }
  for (std::size_t j = 0; j < kCStarNModes; ++j) {
    dst.beta[j] = (*sol)(static_cast<Eigen::Index>(j));
  }
}

}  // namespace

Result<CStarParams> cstar_seed_from_essvi(const EssviParams& src) {
  if (!(src.theta > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "cstar_seed_from_essvi: eSSVI theta must be > 0");
  }

  CStarParams dst{};
  dst.T = src.T;
  dst.F = src.F;
  dst.expiry_ns = src.expiry_ns;
  dst.expiry_id = src.expiry_id;
  dst.active_modes = cstar_tier_mask(CStarTier::C16);
  dst.fit_tier = CStarTier::C16;
  dst.arb_damping = 1.0;

  if (!fit_base_from_essvi(src, dst)) {
    return Err(ErrorCode::InvalidArgument,
               "cstar_seed_from_essvi: eSSVI ATM variance not positive");
  }
  fit_modes_from_essvi_residuals(dst, src);
  // The base shape alone is arb-free for sane (θ, s2, c2, C_L, C_R); the modes
  // can violate. Projection is best-effort here: it now propagates a residual
  // no-arb failure (Err), but the seed remains usable as a fallback baseline —
  // cstar_calibrate_slice re-checks admissibility with the authoritative
  // arb_check_butterfly_slice gate and rejects an inadmissible seed there. So a
  // projection failure is intentionally not fatal at seed time.
  (void)cstar_arb_project(dst);
  return Ok(dst);
}

// ── Price-domain per-slice IRLS-Huber block LM ─────────────────────────────

namespace {

// Half-spread in price units (floored), from a FitObs (spread = ask − bid).
[[nodiscard]] double half_spread_of(const FitObs& o) noexcept {
  return (o.spread > 1.0e-9) ? 0.5 * o.spread : 1.0e-9;
}

// One price-domain residual + 16-Jacobian row. European (Black-76) only.
struct PriceRow {
  double r{};
  std::array<double, kCStarNParams> J{};
  bool ok{false};
};

[[nodiscard]] PriceRow build_price_row(const CStarParams& s, const FitObs& o) {
  PriceRow out;
  const double k = o.k;  // FitObs::k is log(K/F)
  const double w_model = cstar_slice_w(s, k);
  if (!(w_model > 0.0)) {
    return out;
  }
  const double sigma_model = std::sqrt(w_model / s.T);
  if (!std::isfinite(sigma_model) || sigma_model <= 0.0) {
    return out;
  }

  const auto vv =
      black76_value_and_vega(o.F, o.K, s.T, sigma_model, o.df, o.side);
  if (!std::isfinite(vv.price) || !std::isfinite(vv.vega)) {
    return out;
  }
  // PORT NOTE: the C adds the Andersen-Lake American-exercise correction here
  // (P_pred = P_eu + F·C, plus its dσ term in dPrice_dsigma). That correction
  // is not ported — this path prices European only.
  const double P_pred = vv.price;
  const double dPrice_dsigma = vv.vega;
  const double dsig_dw = 1.0 / (2.0 * sigma_model * s.T);

  const auto grad = cstar_slice_grad_w(s, k);
  if (!grad) {
    return out;
  }

  const double half_spread = half_spread_of(o);
  out.r = (P_pred - o.mid) / half_spread;
  const double scale = (1.0 / half_spread) * dPrice_dsigma * dsig_dw;
  for (std::size_t j = 0; j < kCStarNParams; ++j) {
    out.J[j] = (*grad)[j] * scale;
  }
  out.ok = true;
  return out;
}

[[nodiscard]] NormalEq build_normal_eq_price(const CStarParams& s,
                                             CStarBlock block, int dim,
                                             std::span<const FitObs> obs,
                                             std::span<const double> w_obs) {
  NormalEq ne;
  ne.H = MatX::Zero(dim, dim);
  ne.g = VecX::Zero(dim);

  std::array<double, kCStarNParams> row_buf{};
  for (std::size_t i = 0; i < obs.size(); ++i) {
    const PriceRow pr = build_price_row(s, obs[i]);
    if (!pr.ok) {
      continue;
    }
    const double w_w = w_obs.empty() ? 1.0 : w_obs[i];
    if (!(w_w > 0.0)) {
      continue;
    }
    const int row_dim = cstar_extract_block_grad(
        pr.J, block, s.active_modes, std::span<double>{row_buf});
    if (row_dim != dim) {
      continue;
    }

    const double rw = std::sqrt(w_w);
    const double rw_r = rw * pr.r;
    ne.sse += rw_r * rw_r;
    ++ne.n_used;
    for (int j = 0; j < dim; ++j) {
      row_buf[static_cast<std::size_t>(j)] *= rw;
    }
    for (int j = 0; j < dim; ++j) {
      const double rj = row_buf[static_cast<std::size_t>(j)];
      ne.g(j) += rj * rw_r;
      for (int kk = 0; kk <= j; ++kk) {
        ne.H(j, kk) += rj * row_buf[static_cast<std::size_t>(kk)];
      }
    }
  }
  for (int j = 0; j < dim; ++j) {
    for (int kk = j + 1; kk < dim; ++kk) {
      ne.H(j, kk) = ne.H(kk, j);
    }
  }
  return ne;
}

[[nodiscard]] double price_sse_weighted(const CStarParams& s,
                                        std::span<const FitObs> obs,
                                        std::span<const double> w_obs) {
  double sse = 0.0;
  for (std::size_t i = 0; i < obs.size(); ++i) {
    const PriceRow pr = build_price_row(s, obs[i]);
    if (!pr.ok) {
      continue;
    }
    const double w_w = w_obs.empty() ? 1.0 : w_obs[i];
    if (!(w_w > 0.0)) {
      continue;
    }
    sse += w_w * pr.r * pr.r;
  }
  return sse;
}

// Reject only truly-broken trial slices (non-finite / non-positive θ or wing
// slopes); marginal arb is handled by the final projection (C Phase 4.5).
[[nodiscard]] bool arb_step_acceptable(const CStarParams& t) noexcept {
  if (!std::isfinite(t.theta) || !(t.theta > 1.0e-9)) {
    return false;
  }
  if (!std::isfinite(t.C_left) || !(t.C_left > 1.0e-9)) {
    return false;
  }
  if (!std::isfinite(t.C_right) || !(t.C_right > 1.0e-9)) {
    return false;
  }
  return true;
}

void lm_inner_block_price(CStarParams& slice, CStarBlock block,
                          std::span<const FitObs> obs,
                          std::span<const double> w_obs, int max_inner_iters,
                          double& lambda_inout) {
  const int dim = cstar_block_dim(block, slice.active_modes);
  if (dim <= 0) {
    return;
  }
  double lambda = (lambda_inout > 0.0) ? lambda_inout : kLambdaInit;

  NormalEq ne = build_normal_eq_price(slice, block, dim, obs, w_obs);
  if (ne.n_used < dim) {
    return;  // under-determined
  }

  for (int it = 0; it < max_inner_iters; ++it) {
    auto dx = solve_damped(ne.H, ne.g, lambda);
    if (!dx) {
      lambda *= kLambdaGrow;
      if (lambda > kLambdaMax) {
        break;
      }
      continue;
    }
    CStarParams trial = slice;
    cstar_apply_block_step(
        trial, block,
        std::span<const double>{dx->data(), static_cast<std::size_t>(dx->size())});
    if (!arb_step_acceptable(trial)) {
      lambda *= kLambdaGrow;
      if (lambda > kLambdaMax) {
        break;
      }
      continue;
    }

    const double sse_trial = price_sse_weighted(trial, obs, w_obs);
    if (sse_trial < ne.sse) {
      slice = trial;
      lambda *= kLambdaShrink;
      if (lambda < kLambdaMin) {
        lambda = kLambdaMin;
      }
      ne = build_normal_eq_price(slice, block, dim, obs, w_obs);
      const double norm_dx =
          std::sqrt(dx->squaredNorm() / static_cast<double>(dim));
      if (norm_dx < 1.0e-7) {
        break;
      }
    } else {
      lambda *= kLambdaGrow;
      if (lambda > kLambdaMax) {
        break;
      }
    }
  }
  lambda_inout = lambda;
}

// Outer IRLS-Huber loop driving the per-block LM (BASE → MODAL → FULL).
void fit_slice_block_lm_irls(CStarParams& slice, std::span<const FitObs> obs,
                             std::span<const double> base_weights,
                             std::span<double> w_huber,
                             std::span<double> r_abs) {
  const std::size_t n = obs.size();
  for (std::size_t i = 0; i < n; ++i) {
    const double base = base_weights.empty() ? 1.0 : base_weights[i];
    w_huber[i] = (base > 0.0) ? base : 0.0;
  }

  int total_iters = 0;
  double sse_prev = std::numeric_limits<double>::infinity();
  for (int outer = 0; outer < kIrlsOuterMax; ++outer) {
    double lambda = kLambdaInit;
    lm_inner_block_price(slice, CStarBlock::Base, obs, w_huber, kLmInnerMax,
                         lambda);
    total_iters += kLmInnerMax;

    if (slice.active_modes != 0u) {
      lambda = kLambdaInit;
      lm_inner_block_price(slice, CStarBlock::Modal, obs, w_huber, kLmInnerMax,
                           lambda);
      total_iters += kLmInnerMax;

      lambda = kLambdaInit;
      lm_inner_block_price(slice, CStarBlock::Full, obs, w_huber, kLmInnerMax,
                           lambda);
      total_iters += kLmInnerMax;
    }

    if (total_iters >= kTotalIterCap) {
      (void)cstar_arb_project(slice);
      return;
    }

    const double sse_now = price_sse_weighted(slice, obs, w_huber);
    if (std::isfinite(sse_prev) && std::isfinite(sse_now)) {
      const double denom = (sse_prev > 1.0e-12) ? sse_prev : 1.0e-12;
      if (std::fabs(sse_now - sse_prev) / denom < 1.0e-4) {
        break;
      }
    }
    sse_prev = sse_now;

    // IRLS reweight: Huber on the unweighted price residuals × base weight.
    for (std::size_t i = 0; i < n; ++i) {
      const PriceRow pr = build_price_row(slice, obs[i]);
      r_abs[i] = pr.ok ? std::fabs(pr.r) : 0.0;
    }
    detail::huber_weights_strided(std::span<const double>{r_abs.data(), n},
                                  w_huber, kHuberK);
    if (!base_weights.empty()) {
      for (std::size_t i = 0; i < n; ++i) {
        const double base = base_weights[i];
        w_huber[i] *= (base > 0.0) ? base : 0.0;
      }
    }
  }
  (void)cstar_arb_project(slice);
}

// Per-slice European price RMSE in half-spread units (quality-gate metric).
[[nodiscard]] double slice_rmse_price(const CStarParams& s,
                                      std::span<const FitObs> obs) {
  double sse = 0.0;
  int n_used = 0;
  for (const FitObs& o : obs) {
    const double w_model = cstar_slice_w(s, o.k);
    if (!(w_model > 0.0)) {
      continue;
    }
    const double sigma = std::sqrt(w_model / s.T);
    if (!std::isfinite(sigma) || sigma <= 0.0) {
      continue;
    }
    const double P_pred = black76_price(o.F, o.K, s.T, sigma, o.df, o.side);
    const double r = (P_pred - o.mid) / half_spread_of(o);
    if (!std::isfinite(r)) {
      continue;
    }
    sse += r * r;
    ++n_used;
  }
  if (n_used == 0) {
    return 0.0;
  }
  return std::sqrt(sse / static_cast<double>(n_used));
}

}  // namespace

Result<CStarParams> cstar_calibrate_slice(const EssviParams& essvi_seed,
                                          const Chain& chain, double df,
                                          const CalibOpts& opts) {
  ATX_TRY(CStarParams dst, cstar_seed_from_essvi(essvi_seed));
  const CStarParams seed_snapshot = dst;

  const double F = essvi_seed.F;
  const double T = essvi_seed.T;
  ATX_TRY(ObsSet obs_set, build_observations(chain, F, T, df, opts));
  const std::span<const FitObs> obs{obs_set.obs};
  if (obs.empty()) {
    return Err(ErrorCode::NotFound,
               "cstar_calibrate_slice: no usable observations");
  }

  const double cstar_seed_rmse = slice_rmse_price(seed_snapshot, obs);

  const std::size_t n = obs.size();
  std::vector<double> w_huber(n, 1.0);
  std::vector<double> r_abs(n, 0.0);
  fit_slice_block_lm_irls(dst, obs, /*base_weights=*/{},
                          std::span<double>{w_huber}, std::span<double>{r_abs});

  const double cstar_rmse = slice_rmse_price(dst, obs);
  dst.rmse_price = cstar_rmse;

  // Butterfly accept gate: a grid Durrleman g(k) >= 0 density check of the
  // fitted CStar slice over the observed strikes padded by 0.5 in log-moneyness
  // (accept-time only, one 64-point grid eval per slice). A violation folds
  // into the same revert-to-seed path as the RMSE gate below.
  double bf_k_lo = obs.front().k;
  double bf_k_hi = obs.front().k;
  for (const FitObs& o : obs) {
    bf_k_lo = std::min(bf_k_lo, o.k);
    bf_k_hi = std::max(bf_k_hi, o.k);
  }
  const auto fit_bf = arb_check_butterfly_slice(
      [&dst](double kk) { return cstar_slice_w(dst, kk); }, dst.T, bf_k_lo - 0.5,
      bf_k_hi + 0.5, 64u);
  const bool fit_butterfly_ok = fit_bf.has_value() && fit_bf->empty();

  const bool slice_ok = std::isfinite(cstar_rmse) &&
                        std::isfinite(dst.theta) && dst.theta > 0.0 &&
                        std::isfinite(dst.C_left) && std::isfinite(dst.C_right);
  if (!slice_ok || cstar_rmse > 1.05 * cstar_seed_rmse || !fit_butterfly_ok) {
    dst = seed_snapshot;
    dst.reverted_to_seed = true;
    dst.rmse_price = cstar_seed_rmse;
    // Never serve an arbitrageable slice: if even the reverted seed trips the
    // grid g-check, reject the calibration.
    const auto seed_bf = arb_check_butterfly_slice(
        [&dst](double kk) { return cstar_slice_w(dst, kk); }, dst.T,
        bf_k_lo - 0.5, bf_k_hi + 0.5, 64u);
    if (!(seed_bf.has_value() && seed_bf->empty())) {
      return Err(ErrorCode::Unavailable,
                 "cstar_calibrate_slice: slice butterfly-inadmissible after "
                 "revert-to-seed");
    }
  }
  return Ok(dst);
}

}  // namespace atx::vol
