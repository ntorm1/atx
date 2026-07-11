#include "atx/vol/c8_calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/linalg/solve.hpp"   // solve_spd
#include "atx/vol/detail/robust.hpp"   // huber_weights_strided (k = 1.345)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::linalg::MatX;
using atx::core::linalg::solve_spd;
using atx::core::linalg::VecX;

namespace {

// ── x-space <-> field helpers ─────────────────────────────────────────────

[[nodiscard]] double sigmoid(double z) noexcept {
  if (z > 40.0) {
    return 1.0;
  }
  if (z < -40.0) {
    return 0.0;
  }
  return 1.0 / (1.0 + std::exp(-z));
}

[[nodiscard]] double inv_sigmoid(double y) noexcept {
  y = std::clamp(y, 1e-12, 1.0 - 1e-12);
  return std::log(y / (1.0 - y));
}

// Map an 8-vector of d/d(JW-param) partials into the x-space row, folding the
// v_min-depends-on-v coupling into column 0 (d v/dx0 = v, d v_min/dx0 = v_min):
//   col 0: dw/dv * v + dw/dvmin * v_min
//   col 1: dw/dpsi
//   col 2: dw/dp * p
//   col 3: dw/dc * c
//   col 4: dw/dvmin * v * sig*(1-sig)
//   col 5..7: dw/d(kappa, q_L, q_R)
[[nodiscard]] std::array<double, 8> map_grad_jw_to_x(
    const C8Params& s, const std::array<double, 8>& g) noexcept {
  const double sig = (s.v > 0.0) ? (s.v_min / s.v) : 0.5;
  const double dvmin_dx4 = s.v * sig * (1.0 - sig);
  return {
      g[0] * s.v + g[4] * s.v_min,
      g[1],
      g[2] * s.p,
      g[3] * s.c,
      g[4] * dvmin_dx4,
      g[5],
      g[6],
      g[7],
  };
}

// ── Vol-domain normal equations (mid supplied in total-variance) ──────────

struct NormalEq {
  std::array<std::array<double, 8>, 8> H{};
  std::array<double, 8> g{};
  double sse{0.0};
  std::size_t grad_failures{0};  // obs whose analytic gradient was unavailable
};

[[nodiscard]] NormalEq build_normal_eq(const C8Params& s,
                                       std::span<const double> k,
                                       std::span<const double> mid,
                                       std::span<const double> sd,
                                       std::span<const double> weights,
                                       double eps_floor) noexcept {
  NormalEq ne{};
  const std::size_t n = k.size();
  // The JW->raw conversion and its 5x5 Jacobian are strike-invariant (functions
  // of `s` only), so hoist both out of the per-observation loop: one c8_jw_to_raw
  // + one analytic c8_jw_to_raw_jac per call instead of 12 conversions per obs.
  const C8Jw jw{s.v, s.psi, s.p, s.c, s.v_min};
  const std::optional<C8RawSvi> raw_conv = c8_jw_to_raw(jw, s.T, 1e-4);
  const std::optional<std::array<std::array<double, 5>, 5>> jac_conv =
      c8_jw_to_raw_jac(jw, s.T, 1e-4);
  for (std::size_t i = 0; i < n; ++i) {
    const double sdi = (sd[i] > eps_floor) ? sd[i] : eps_floor;
    const double w_model = c8_slice_w(s, k[i], raw_conv);
    const double w0 = weights.empty() ? 1.0 : weights[i];
    const double rw = std::sqrt(w0 > 0.0 ? w0 : 0.0);
    const double r = rw * (w_model - mid[i]) / sdi;
    ne.sse += r * r;

    const std::optional<std::array<double, 8>> grad =
        c8_slice_grad_w(s, k[i], raw_conv, jac_conv);
    if (!grad.has_value()) {
      ++ne.grad_failures;
      continue;
    }
    std::array<double, 8> row = map_grad_jw_to_x(s, *grad);
    const double scale = rw / sdi;
    for (std::size_t j = 0; j < 8; ++j) {
      row[j] *= scale;
    }
    for (std::size_t j = 0; j < 8; ++j) {
      ne.g[j] += row[j] * r;
      for (std::size_t kk = 0; kk <= j; ++kk) {
        ne.H[j][kk] += row[j] * row[kk];
      }
    }
  }
  // Mirror the lower triangle into the upper.
  for (std::size_t j = 0; j < 8; ++j) {
    for (std::size_t kk = j + 1; kk < 8; ++kk) {
      ne.H[j][kk] = ne.H[kk][j];
    }
  }
  return ne;
}

// True when too few observations produced a valid analytic gradient for the 8x8
// normal system to be determined. c8_slice_grad_w fails (nullopt) when the JW
// point — or its central-difference perturbation — leaves the admissibility
// region, which can silently zero out whole gradient rows and leave the LM
// "converging" on a rank-deficient (gradient-blind) step. The 8-DoF fit needs at
// least min(8, n) valid rows; below that the driver must fail the slice rather
// than accept a garbage fit. (A well-posed fit has zero gradient failures, so
// this never fires on healthy data.)
[[nodiscard]] bool c8_grad_underdetermined(
    const C8Params& s, std::span<const double> k, std::span<const double> mid,
    std::span<const double> sd, std::span<const double> weights,
    double eps_floor) noexcept {
  const std::size_t n = k.size();
  if (n == 0) {
    return true;
  }
  const NormalEq ne = build_normal_eq(s, k, mid, sd, weights, eps_floor);
  const std::size_t valid =
      (ne.grad_failures <= n) ? (n - ne.grad_failures) : 0;
  const std::size_t need = (n < 8u) ? n : static_cast<std::size_t>(8);
  return valid < need;
}

// Solve the LM-damped normal equations (H with diagonal scaled by (1+lambda))
// for the step dx = -(Hd)^-1 g via the shared SPD solver. Returns nullopt when
// the damped system is not positive-definite (caller bumps lambda and retries).
[[nodiscard]] std::optional<std::array<double, 8>> solve_lm_step(
    const NormalEq& ne, double lambda) {
  MatX A(8, 8);
  VecX rhs(8);
  for (int j = 0; j < 8; ++j) {
    for (int kk = 0; kk < 8; ++kk) {
      A(j, kk) = ne.H[static_cast<std::size_t>(j)][static_cast<std::size_t>(kk)];
    }
    double diag = ne.H[static_cast<std::size_t>(j)][static_cast<std::size_t>(j)] *
                  (1.0 + lambda);
    if (diag < 1e-12) {
      diag = 1e-12;
    }
    A(j, j) = diag;
    rhs(j) = -ne.g[static_cast<std::size_t>(j)];
  }
  const auto res = solve_spd(A, rhs);
  if (!res.has_value()) {
    return std::nullopt;
  }
  std::array<double, 8> dx{};
  for (int j = 0; j < 8; ++j) {
    dx[static_cast<std::size_t>(j)] = (*res)(j);
  }
  return dx;
}

// Inner Levenberg-Marquardt loop over the 8-DoF x-vector. Mutates `s` to the
// best point found; returns the number of accepted steps.
int fit_lm_inner(C8Params& s, std::span<const double> k,
                 std::span<const double> mid, std::span<const double> sd,
                 std::span<const double> weights, int max_inner_iters,
                 double eps_floor) {
  const std::size_t n = k.size();
  // De-saturate a (near-)degenerate v_min == v seed before packing. Two
  // saturation mechanisms make such a seed un-fittable as-is:
  //   1. c8_pack maps frac = v_min/v through inv_sigmoid, clamped at 1-1e-12,
  //      so x4 ~ 27.6 and dv_min/dx4 = v*sig*(1-sig) ~ 1e-12*v — the x4
  //      direction is gradient-dead and frac stays pinned at 1 forever;
  //   2. v - v_min < 1e-12 keeps c8_jw_to_raw on its degenerate branch
  //      (m := 0, sigma := sigma_floor), whose Jacobian m/sigma rows are
  //      identically zero — v and psi become gradient-dead too, and the LM
  //      can only fit a V-kink + bumps (observed to run away along the flat
  //      log(c) direction until exp underflows c to exactly 0).
  // Pulling v_min to at most v*(1 - kVminInteriorEps) revives both: the
  // sigmoid derivative dv_min/dx4 becomes ~eps*v and v - v_min ~ eps*v sits
  // on the generic conversion branch with live m/sigma/psi partials.
  // eps = 1e-3 was measured necessary on the degenerate-seed repro
  // (calib_robustness_test): at eps = 1e-6, dv_min/dx4 ~ 4e-8 is still
  // effectively dead AND sigma = alpha*m ~ 3e-5 stays under the 1e-4 floor
  // (sigma-row still zero), reproducing the runaway; at 1e-3 the same fit
  // converges to w-RMSE ~ 0.0065 half-spreads with v_min traveling freely.
  constexpr double kVminInteriorEps = 1e-3;
  if (s.v > 0.0 && s.v_min > s.v * (1.0 - kVminInteriorEps)) {
    s.v_min = s.v * (1.0 - kVminInteriorEps);
  }
  std::array<double, 8> x = c8_pack(s);
  double lambda = 1e-3;
  // Hold the current point's normal equations across iterations. `s` only
  // changes on an accepted step, and build_normal_eq is a pure function of the
  // point, so the accepted trial's NE is bit-identical to the NE the next
  // iteration would recompute for `s` — carry it forward instead (one
  // build_normal_eq per iteration for the trial, not two).
  NormalEq ne_cur = build_normal_eq(s, k, mid, sd, weights, eps_floor);
  double best_sse = ne_cur.sse;

  int it_done = 0;
  for (int it = 0; it < max_inner_iters; ++it) {
    const std::optional<std::array<double, 8>> dx = solve_lm_step(ne_cur, lambda);
    if (!dx.has_value()) {
      lambda *= 4.0;
      continue;
    }
    std::array<double, 8> x_trial{};
    for (std::size_t j = 0; j < 8; ++j) {
      x_trial[j] = x[j] + (*dx)[j];
    }
    C8Params trial = s;
    c8_unpack(x_trial, s.T, trial);
    const NormalEq ne_trial =
        build_normal_eq(trial, k, mid, sd, weights, eps_floor);
    const double sse_trial = ne_trial.sse;

    if (sse_trial < best_sse) {
      best_sse = sse_trial;
      x = x_trial;
      s = trial;
      ne_cur = ne_trial;  // reuse: identical inputs -> identical NE next iter
      lambda *= 0.5;
      it_done = it + 1;
      s.n_lm_iters = it_done;
      s.rmse_vol = std::sqrt(sse_trial / static_cast<double>(n));
    } else {
      lambda *= 4.0;
      if (lambda > 1e8) {
        break;
      }
    }
  }
  return it_done;
}

}  // namespace

// ── x-space reparametrization ─────────────────────────────────────────────

std::array<double, 8> c8_pack(const C8Params& s) noexcept {
  const double frac = (s.v > 1e-12) ? (s.v_min / s.v) : 0.5;
  return {
      std::log(s.v > 1e-12 ? s.v : 1e-12),
      s.psi,
      std::log(s.p > 1e-12 ? s.p : 1e-12),
      std::log(s.c > 1e-12 ? s.c : 1e-12),
      inv_sigmoid(frac),
      s.kappa,
      s.q_L,
      s.q_R,
  };
}

void c8_unpack(const std::array<double, 8>& x, double T, C8Params& s) noexcept {
  (void)T;
  s.v = std::exp(x[0]);
  s.psi = x[1];
  s.p = std::exp(x[2]);
  s.c = std::exp(x[3]);
  s.v_min = s.v * sigmoid(x[4]);
  s.kappa = x[5];
  s.q_L = x[6];
  s.q_R = x[7];
}

// ── Vol-domain residual SSE ───────────────────────────────────────────────

double c8_residual_sse(const C8Params& s, std::span<const double> k,
                       std::span<const double> mid,
                       std::span<const double> half_spread,
                       double eps_floor) noexcept {
  const std::size_t n = k.size();
  if (n == 0 || mid.size() != n || half_spread.size() != n) {
    return 0.0;
  }
  double sse = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double w_model = c8_slice_w(s, k[i]);
    const double sd = (half_spread[i] > eps_floor) ? half_spread[i] : eps_floor;
    const double r = (w_model - mid[i]) / sd;
    sse += r * r;
  }
  return sse;
}

// ── Per-slice vol-domain LM fit ───────────────────────────────────────────

Status c8_fit_slice_lm(C8Params& s, std::span<const double> k,
                       std::span<const double> mid,
                       std::span<const double> spread, int max_inner_iters,
                       double eps_floor) {
  const std::size_t n = k.size();
  if (n == 0 || mid.size() != n || spread.size() != n || max_inner_iters <= 0) {
    return Err(ErrorCode::InvalidArgument,
               "c8_fit_slice_lm: empty/mismatched spans or non-positive iters");
  }
  (void)fit_lm_inner(s, k, mid, spread, std::span<const double>{},
                     max_inner_iters, eps_floor);
  // Gradient-rank guard: fail rather than accept a gradient-blind fit whose
  // 8x8 normal system was under-determined (too many gradient failures).
  if (c8_grad_underdetermined(s, k, mid, spread, std::span<const double>{},
                              eps_floor)) {
    return Err(ErrorCode::Unavailable,
               "c8_fit_slice_lm: gradient rank-deficient — too few valid "
               "gradient rows to determine the 8 parameters");
  }
  c8_arb_project(s);
  return Ok();
}

// ── Per-slice quality gate ────────────────────────────────────────────────

bool c8_apply_quality_gate(C8Params& slice, const C8Params& seed,
                           double seed_rmse_price, double tolerance) noexcept {
  const bool unconditional = (tolerance < 0.0);
  const double tol = (tolerance > 0.0) ? tolerance : 1.05;

  // Defensive: revert on non-finite RMSE / inadmissible parameters regardless
  // of tolerance (NaN > x silently evaluates false, which would otherwise let a
  // blown-up slice persist).
  const bool slice_ok = std::isfinite(slice.rmse_price) &&
                        std::isfinite(slice.v) && slice.v > 0.0 &&
                        std::isfinite(slice.p) && slice.p > 0.0 &&
                        std::isfinite(slice.c) && slice.c > 0.0 &&
                        std::isfinite(slice.v_min) && std::isfinite(slice.kappa) &&
                        std::isfinite(slice.q_L) && std::isfinite(slice.q_R);

  if (unconditional || !slice_ok ||
      slice.rmse_price > tol * seed_rmse_price) {
    slice = seed;
    slice.bumps_active = false;
    slice.arb_damping_factor = 1.0;
    return true;
  }
  return false;
}

// ── Chain-level slice calibration ─────────────────────────────────────────

Result<C8SliceFit> c8_calib_slice(const C8Params& seed, const Chain& chain,
                                  double F, double T, double df,
                                  const CalibOpts& opts) {
  ATX_TRY(auto obs_set, build_observations(chain, F, T, df, opts));
  const std::vector<FitObs>& obs = obs_set.obs;
  const std::size_t n = obs.size();

  // Vol-domain targets: mid = total-variance w_mkt; the per-obs scale is the
  // spread's total-variance equivalent (d w / d sigma = 2*sigma*T).
  constexpr double kSdFloor = 1e-9;
  std::vector<double> k(n);
  std::vector<double> mid(n);
  std::vector<double> sd(n);
  for (std::size_t i = 0; i < n; ++i) {
    k[i] = obs[i].k;
    mid[i] = obs[i].w_mkt;
    const double sd_w = 2.0 * obs[i].sigma_mkt * T * obs[i].noise_sigma;
    sd[i] = (sd_w > kSdFloor) ? sd_w : kSdFloor;
  }

  C8Params params = seed;
  params.T = T;
  params.F = F;
  params.bumps_active = true;

  const std::span<const double> ks{k};
  const std::span<const double> mids{mid};
  const std::span<const double> sds{sd};

  const C8Params seed_snapshot = params;
  const double seed_sse = c8_residual_sse(seed_snapshot, ks, mids, sds, kSdFloor);
  const double seed_rmse = std::sqrt(seed_sse / static_cast<double>(n));

  // IRLS-Huber outer loop around the LM inner (q90-anchored Huber, k = 1.345).
  std::vector<double> weights(n, 1.0);
  std::vector<double> r_abs(n, 0.0);
  int inner_total = 0;
  std::uint16_t outer_used = 0;
  for (std::uint16_t outer = 0; outer < opts.max_outer_iter; ++outer) {
    inner_total += fit_lm_inner(params, ks, mids, sds,
                                std::span<const double>{weights},
                                static_cast<int>(opts.max_inner_iter), kSdFloor);
    outer_used = static_cast<std::uint16_t>(outer + 1);
    params.n_irls_iters = outer_used;
    for (std::size_t i = 0; i < n; ++i) {
      const double w_model = c8_slice_w(params, k[i]);
      r_abs[i] = std::fabs((w_model - mid[i]) / sd[i]);
    }
    detail::huber_weights_strided(std::span<const double>{r_abs},
                                  std::span<double>{weights},
                                  detail::kHuberDefaultK<double>);
  }

  // Gradient-rank guard: fail rather than accept a gradient-blind fit whose 8x8
  // normal system was under-determined (mirrors c8_fit_slice_lm).
  if (c8_grad_underdetermined(params, ks, mids, sds,
                              std::span<const double>{weights}, kSdFloor)) {
    return Err(ErrorCode::Unavailable,
               "c8_calib_slice: gradient rank-deficient — too few valid "
               "gradient rows to determine the 8 parameters");
  }

  c8_arb_project(params);
  const double fit_sse = c8_residual_sse(params, ks, mids, sds, kSdFloor);
  params.rmse_price = std::sqrt(fit_sse / static_cast<double>(n));
  (void)c8_apply_quality_gate(params, seed_snapshot, seed_rmse, 1.05);

  double max_resid_vol = 0.0;
  for (std::size_t i = 0; i < n; ++i) {
    const double w_model = c8_slice_w(params, k[i]);
    const double sig_model = (w_model > 0.0) ? std::sqrt(w_model / T) : 0.0;
    max_resid_vol = std::max(max_resid_vol, std::fabs(sig_model - obs[i].sigma_mkt));
  }

  C8SliceFit out;
  out.params = params;
  out.diag.rmse_vol_vega_weighted = params.rmse_price;
  out.diag.max_residual_vol = max_resid_vol;
  out.diag.outer_iters = outer_used;
  out.diag.inner_iters_total = static_cast<std::uint16_t>(inner_total);
  out.diag.n_quotes_used = static_cast<std::uint32_t>(n);
  return Ok(std::move(out));
}

}  // namespace atx::vol
