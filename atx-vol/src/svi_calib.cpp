#include "atx/vol/svi_calib.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/linalg/solve.hpp"   // solve_spd
#include "atx/vol/arb.hpp"             // arb_project_calendar_svi
#include "atx/vol/black76.hpp"         // black76_price, black76_value_and_vega
#include "atx/vol/calib.hpp"           // CalibOpts, FitObs, FitDiag, build_observations
#include "atx/vol/detail/calib_shared.hpp"  // detail::outer_cap + shared LM constants

// Per-slice raw-SVI calibrators — implementation.
//
// The quasi-explicit fitter (De Marco & Martini, Zeliade WP zwp-0005, 2009)
// substitutes y = (k - m)/sigma, z = sqrt(y^2 + 1) and rotates 45 degrees into
// (u, v) so the SVI total-variance form becomes linear in (a, d_uv, c_uv) at
// fixed (m, sigma); the box |rho| <= 1 / Lee butterfly bound become simple
// coordinate bounds. The inner step is a bounded linear least squares (active
// set over 4 box constraints, normal equations backed by
// `atx::core::linalg::solve_spd`); the outer step is a 2-D Nelder-Mead on
// (m, sigma); the outer-outer loop is IRLS Huber reweighting. See
// ats_calibrate_svi.c for the full derivation and references.
//
// The Martini-Mingone fitter (arXiv:2005.03340 §6.3) is a 5-DoF price-domain
// Levenberg-Marquardt that projects every iterate onto the admissible polytope;
// see ats_calibrate_svi_mm.c.

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

// ── Numeric constants (no M_PI / magic literals on the hot path) ─────────
constexpr double kInvSqrt2 = 0.70710678118654752440;  // 1/sqrt(2)
constexpr double kSqrt2 = 1.41421356237309504880;      // sqrt(2)

// Below this year-fraction a slice has no useful Newton signal (same-day /
// intra-day slivers); the surface driver skips it (~30 minutes floor).
constexpr double kTMinFit = 1.0 / (365.25 * 24.0 * 2.0);

// Mingone projector edge pads (ats_calibrate_svi_mm.c MM_EDGE_*).
constexpr double kMmEdgeB = 1.0e-8;
constexpr double kMmEdgeSigma = 1.0e-6;
constexpr double kMmEdgeRho = 1.0e-4;
constexpr double kMmEdgeLee = 1.0e-9;
constexpr double kMmSigmaFloor = 0.05;  // physical sigma floor (1 vol point band)

// ── Small dense SPD solve backed by atx-core ─────────────────────────────

// Solve the symmetric positive-definite system H x = rhs for `n` in {3, 5},
// where `H` is row-major length n*n and already symmetric. Returns false when
// atx-core reports the matrix is not positive-definite (the caller then falls
// back to a degenerate handling exactly as the C did).
[[nodiscard]] bool solve_spd_dense(const double *H, const double *rhs, int n,
                                   double *out) {
  atx::core::linalg::MatX A(n, n);
  atx::core::linalg::VecX b(n);
  for (int i = 0; i < n; ++i) {
    b(i) = rhs[i];
    for (int j = 0; j < n; ++j) {
      A(i, j) = H[i * n + j];
    }
  }
  const auto res = atx::core::linalg::solve_spd(A, b);
  if (!res.has_value()) {
    return false;
  }
  for (int i = 0; i < n; ++i) {
    out[i] = (*res)(i);
  }
  return true;
}

// ── SVI raw evaluators (self-contained; no SviParams temporaries) ────────

[[nodiscard]] double svi_w_raw(double a, double b, double rho, double m,
                               double sigma, double k) noexcept {
  const double dk = k - m;
  return a + b * (rho * dk + std::sqrt(dk * dk + sigma * sigma));
}

// Post-fit positivity gate. A converged raw-SVI slice must have STRICTLY
// positive total variance everywhere the surface consumes it, else pricing it
// yields garbage IVs silently. The closed-form global minimum of w(k) is
// w_min = a + b*sigma*sqrt(1-rho^2); if that is > 0 the whole curve is > 0. The
// quasi-explicit fitter's non-negativity box and the SVI-MM admissibility
// projection already guarantee this, so this is a defense-in-depth check that
// pins the invariant against a future fitter change (the surface driver rejects
// the expiry on violation, mirroring the post-fit max-sigma clamp). Also spot-
// checks each observed strike so a non-finite param cannot slip through.
[[nodiscard]] bool svi_slice_variance_positive(
    const SviParams &s, std::span<const FitObs> obs) noexcept {
  const double disc = 1.0 - s.rho * s.rho;
  const double w_min = s.a + s.b * s.sigma * std::sqrt(disc > 0.0 ? disc : 0.0);
  if (!std::isfinite(w_min) || !(w_min > 0.0)) {
    return false;
  }
  for (const FitObs &o : obs) {
    const double w = svi_w_raw(s.a, s.b, s.rho, s.m, s.sigma, o.k);
    if (!std::isfinite(w) || !(w > 0.0)) {
      return false;
    }
  }
  return true;
}

// ── Inner: bounded linear least squares over (a, d_uv, c_uv) ─────────────

// Weighted SSE of the reduced linear model at coordinates `x = (a, d_uv, c_uv)`
// (ports the SSE recompute blocks in svi_blls_inner / build_and_solve_normal).
[[nodiscard]] double svi_qe_sse(std::span<const FitObs> obs, double m,
                                double sigma,
                                const std::array<double, 3> &x) noexcept {
  double sse = 0.0;
  for (const FitObs &o : obs) {
    const double yi = (o.k - m) / sigma;
    const double zi = std::sqrt(yi * yi + 1.0);
    const double ui = (yi + zi) * kInvSqrt2;
    const double vi = (zi - yi) * kInvSqrt2;
    const double w_pred = x[0] + x[1] * ui + x[2] * vi;
    const double r = w_pred - o.w_mkt;
    sse += o.active_weight_w * r * r;
  }
  return sse;
}

// Build the weighted normal equations A^T W A x = A^T W w for the free
// variables (`free_mask[i]` == true) at fixed (m, sigma), solve, and write the
// free entries of `x` (pinned entries are read for the reduced rhs). Returns
// the SSE at the resulting x. Mirrors `build_and_solve_normal`.
[[nodiscard]] double build_and_solve_normal(std::span<const FitObs> obs, double m,
                                            double sigma,
                                            const std::array<bool, 3> &free_mask,
                                            std::array<double, 3> &x) {
  std::array<double, 9> H{};
  std::array<double, 3> g{};
  for (const FitObs &o : obs) {
    const double yi = (o.k - m) / sigma;
    const double zi = std::sqrt(yi * yi + 1.0);
    const double ui = (yi + zi) * kInvSqrt2;
    const double vi = (zi - yi) * kInvSqrt2;
    const std::array<double, 3> ai{1.0, ui, vi};
    const double w_i = o.active_weight_w;
    const double t_i = o.w_mkt;
    for (int p = 0; p < 3; ++p) {
      g[static_cast<std::size_t>(p)] += w_i * ai[static_cast<std::size_t>(p)] * t_i;
      for (int q = 0; q <= p; ++q) {
        H[static_cast<std::size_t>(p * 3 + q)] +=
            w_i * ai[static_cast<std::size_t>(p)] * ai[static_cast<std::size_t>(q)];
      }
    }
  }
  // Mirror the symmetric upper triangle.
  H[1] = H[3];
  H[2] = H[6];
  H[5] = H[7];

  // Reduced rhs: substitute pinned variables.
  std::array<double, 3> rhs{};
  for (int p = 0; p < 3; ++p) {
    double v = g[static_cast<std::size_t>(p)];
    for (int q = 0; q < 3; ++q) {
      if (!free_mask[static_cast<std::size_t>(q)]) {
        v -= H[static_cast<std::size_t>(p * 3 + q)] * x[static_cast<std::size_t>(q)];
      }
    }
    rhs[static_cast<std::size_t>(p)] = v;
  }

  std::array<int, 3> free_idx{};
  int n_free = 0;
  for (int p = 0; p < 3; ++p) {
    if (free_mask[static_cast<std::size_t>(p)]) {
      free_idx[static_cast<std::size_t>(n_free++)] = p;
    }
  }

  if (n_free == 0) {
    // All pinned — nothing to solve.
  } else if (n_free == 1) {
    const int p = free_idx[0];
    const double hpp = H[static_cast<std::size_t>(p * 3 + p)];
    x[static_cast<std::size_t>(p)] =
        (hpp > 1.0e-14) ? rhs[static_cast<std::size_t>(p)] / hpp : 0.0;
  } else if (n_free == 2) {
    const int p = free_idx[0];
    const int q = free_idx[1];
    const double aa = H[static_cast<std::size_t>(p * 3 + p)];
    const double bb = H[static_cast<std::size_t>(p * 3 + q)];
    const double dd = H[static_cast<std::size_t>(q * 3 + q)];
    const double det = aa * dd - bb * bb;
    const double rp = rhs[static_cast<std::size_t>(p)];
    const double rq = rhs[static_cast<std::size_t>(q)];
    if (det > 1.0e-14) {
      x[static_cast<std::size_t>(p)] = (dd * rp - bb * rq) / det;
      x[static_cast<std::size_t>(q)] = (-bb * rp + aa * rq) / det;
    } else {
      x[static_cast<std::size_t>(p)] = (aa > 1.0e-14) ? rp / aa : 0.0;
      x[static_cast<std::size_t>(q)] = (dd > 1.0e-14) ? rq / dd : 0.0;
    }
  } else {
    // Full 3x3: back the Cholesky solve with atx-core (rhs == g here, since no
    // variable is pinned). Degenerate matrix -> zero everything (matches the C).
    std::array<double, 3> sol{};
    if (solve_spd_dense(H.data(), g.data(), 3, sol.data())) {
      x = sol;
    } else {
      x = {0.0, 0.0, 0.0};
    }
  }

  return svi_qe_sse(obs, m, sigma, x);
}

// Active-set bounded LSQ for (a, d_uv, c_uv) at fixed (m, sigma). Box:
// 0 <= a <= a_max, 0 <= d_uv, c_uv <= duvc_max. Returns SSE; writes `out`.
// Mirrors `svi_blls_inner`.
[[nodiscard]] double svi_blls_inner(std::span<const FitObs> obs, double m,
                                    double sigma, double a_max, double duvc_max,
                                    std::array<double, 3> &out) {
  std::array<bool, 3> free_mask{true, true, true};
  std::array<double, 3> x{0.0, 0.0, 0.0};
  const std::array<double, 3> upper{a_max, duvc_max, duvc_max};
  const std::array<double, 3> lower{0.0, 0.0, 0.0};

  double sse = build_and_solve_normal(obs, m, sigma, free_mask, x);

  for (int it = 0; it < 6; ++it) {
    int worst_idx = -1;
    double worst_violation = 0.0;
    bool worst_at_upper = false;
    for (int p = 0; p < 3; ++p) {
      const auto up = static_cast<std::size_t>(p);
      if (!free_mask[up]) {
        continue;
      }
      const double v_lo = lower[up] - x[up];  // > 0 if x[p] < lo
      const double v_hi = x[up] - upper[up];  // > 0 if x[p] > hi
      if (v_lo > worst_violation) {
        worst_violation = v_lo;
        worst_idx = p;
        worst_at_upper = false;
      }
      if (v_hi > worst_violation) {
        worst_violation = v_hi;
        worst_idx = p;
        worst_at_upper = true;
      }
    }
    if (worst_idx < 0) {
      break;  // feasible
    }
    const auto uw = static_cast<std::size_t>(worst_idx);
    x[uw] = worst_at_upper ? upper[uw] : lower[uw];
    free_mask[uw] = false;
    sse = build_and_solve_normal(obs, m, sigma, free_mask, x);
  }

  // Final clamp + SSE recompute (the last solve may not have reached feasibility).
  for (int p = 0; p < 3; ++p) {
    const auto up = static_cast<std::size_t>(p);
    if (x[up] < lower[up]) {
      x[up] = lower[up];
    }
    if (x[up] > upper[up]) {
      x[up] = upper[up];
    }
  }
  sse = svi_qe_sse(obs, m, sigma, x);
  out = x;
  return sse;
}

// ── Outer: 2-D Nelder-Mead on (m, sigma) ─────────────────────────────────

struct NmCtx {
  std::span<const FitObs> obs;
  double a_max{0.0};
  double duvc_max{0.0};
  double sigma_min{0.0};
  double sigma_max{0.0};
  double m_min{0.0};
  double m_max{0.0};
};

[[nodiscard]] double nm_eval(const NmCtx &c, double m, double sigma,
                             std::array<double, 3> &linear) {
  if (sigma < c.sigma_min) {
    sigma = c.sigma_min;
  }
  if (sigma > c.sigma_max) {
    sigma = c.sigma_max;
  }
  if (m < c.m_min) {
    m = c.m_min;
  }
  if (m > c.m_max) {
    m = c.m_max;
  }
  return svi_blls_inner(c.obs, m, sigma, c.a_max, c.duvc_max, linear);
}

// Nelder-Mead simplex search over (m, sigma). Writes the best vertex back into
// (*m, *sigma) and its inner linear optimum into `linear`. Mirrors `nm_search`.
void nm_search(const NmCtx &c, double &m, double &sigma,
               std::array<double, 3> &linear, std::uint16_t max_iter, double tol,
               std::uint16_t &out_iters_used) {
  std::array<std::array<double, 2>, 3> v{};
  std::array<double, 3> f{};
  std::array<std::array<double, 3>, 3> lin{};

  v[0] = {m, sigma};
  v[1] = {m + 0.05, sigma};
  v[2] = {m, sigma * 1.5};
  for (int i = 0; i < 3; ++i) {
    const auto ui = static_cast<std::size_t>(i);
    f[ui] = nm_eval(c, v[ui][0], v[ui][1], lin[ui]);
  }

  std::uint16_t iters_used = 0;
  for (std::uint16_t it = 0; it < max_iter; ++it) {
    iters_used = static_cast<std::uint16_t>(it + 1);
    // Sort ascending by f (bubble, n = 3).
    for (int i = 0; i < 3; ++i) {
      for (int j = i + 1; j < 3; ++j) {
        const auto ui = static_cast<std::size_t>(i);
        const auto uj = static_cast<std::size_t>(j);
        if (f[uj] < f[ui]) {
          std::swap(f[ui], f[uj]);
          std::swap(v[ui], v[uj]);
          std::swap(lin[ui], lin[uj]);
        }
      }
    }
    const double f_range = f[2] - f[0];
    const double v_range =
        std::fabs(v[2][0] - v[0][0]) + std::fabs(v[2][1] - v[0][1]);
    if (f_range < tol && v_range < 1.0e-7) {
      break;
    }

    const double cm = 0.5 * (v[0][0] + v[1][0]);
    const double cs = 0.5 * (v[0][1] + v[1][1]);

    // Reflection.
    const double rm = cm + (cm - v[2][0]);
    const double rs = cs + (cs - v[2][1]);
    std::array<double, 3> rlin{};
    const double rf = nm_eval(c, rm, rs, rlin);

    if (rf < f[0]) {
      // Expansion.
      const double em = cm + 2.0 * (cm - v[2][0]);
      const double es = cs + 2.0 * (cs - v[2][1]);
      std::array<double, 3> elin{};
      const double ef = nm_eval(c, em, es, elin);
      if (ef < rf) {
        v[2] = {em, es};
        f[2] = ef;
        lin[2] = elin;
      } else {
        v[2] = {rm, rs};
        f[2] = rf;
        lin[2] = rlin;
      }
    } else if (rf < f[1]) {
      v[2] = {rm, rs};
      f[2] = rf;
      lin[2] = rlin;
    } else {
      // Contraction toward the centroid.
      const double km = cm + 0.5 * (v[2][0] - cm);
      const double ks = cs + 0.5 * (v[2][1] - cs);
      std::array<double, 3> klin{};
      const double kf = nm_eval(c, km, ks, klin);
      if (kf < f[2]) {
        v[2] = {km, ks};
        f[2] = kf;
        lin[2] = klin;
      } else {
        // Shrink toward the best vertex.
        for (int i = 1; i < 3; ++i) {
          const auto ui = static_cast<std::size_t>(i);
          v[ui][0] = v[0][0] + 0.5 * (v[ui][0] - v[0][0]);
          v[ui][1] = v[0][1] + 0.5 * (v[ui][1] - v[0][1]);
          f[ui] = nm_eval(c, v[ui][0], v[ui][1], lin[ui]);
        }
      }
    }
  }

  int best = 0;
  if (f[1] < f[static_cast<std::size_t>(best)]) {
    best = 1;
  }
  if (f[2] < f[static_cast<std::size_t>(best)]) {
    best = 2;
  }
  const auto ub = static_cast<std::size_t>(best);
  m = v[ub][0];
  sigma = v[ub][1];
  linear = lin[ub];
  out_iters_used = iters_used;
}

// ── Mingone admissible-polytope projector ────────────────────────────────

// w_min slack a + b*sigma*sqrt(1-rho^2) (>= 0 iff admissible).
[[nodiscard]] double mm_w_min_slack(double a, double b, double rho,
                                    double sigma) noexcept {
  return a + b * sigma * std::sqrt(1.0 - rho * rho);
}

// Project (a, b, rho, m, sigma) onto the Mingone admissible polytope with the
// given edge pads. Returns true if any coordinate moved. `m` has no Mingone
// constraint. Mirrors `mm_project_admissible`.
bool mm_project_admissible(double T, double &a, double &b, double &rho,
                           double &sigma, double edge_b, double edge_sigma,
                           double edge_rho, double edge_a, double edge_lee) {
  bool touched = false;
  if (!(T > 0.0)) {
    return false;
  }
  if (b < edge_b) {
    b = edge_b;
    touched = true;
  }
  if (sigma < edge_sigma) {
    sigma = edge_sigma;
    touched = true;
  }
  if (rho > 1.0 - edge_rho) {
    rho = 1.0 - edge_rho;
    touched = true;
  }
  if (rho < -1.0 + edge_rho) {
    rho = -1.0 + edge_rho;
    touched = true;
  }
  // Lee: shrink b if b*(1+|rho|) exceeds 4/T - edge_lee.
  {
    const double lee_max = (4.0 / T - edge_lee) / (1.0 + std::fabs(rho));
    if (lee_max > 0.0 && b > lee_max) {
      b = lee_max;
      touched = true;
      if (b < edge_b) {
        b = edge_b;
      }
    }
  }
  // w_min >= edge_a: bump `a` by the deficit.
  {
    const double slack = mm_w_min_slack(a, b, rho, sigma);
    if (slack < edge_a) {
      a += (edge_a - slack);
      touched = true;
    }
  }
  return touched;
}

// Project with the production edge pads (edge_a scaled so the smile floor is
// sigma_floor^2 * T).
bool mm_project_default(double T, double &a, double &b, double &rho,
                        double &sigma) {
  return mm_project_admissible(T, a, b, rho, sigma, kMmEdgeB, kMmEdgeSigma,
                               kMmEdgeRho, kMmSigmaFloor * kMmSigmaFloor * T,
                               kMmEdgeLee);
}

// ── SVI-MM: price + analytic 5-column Jacobian ───────────────────────────

// d w / d(a, b, rho, m, sigma) at (b, rho, m, sigma) — closed form.
void svi_w_grad_at(double k, double b, double rho, double m, double sigma,
                   std::array<double, 5> &g) noexcept {
  const double dk = k - m;
  const double r = std::sqrt(dk * dk + sigma * sigma);
  const double inv_r = (r > 1.0e-300) ? 1.0 / r : 0.0;
  g[0] = 1.0;                           // d w / d a
  g[1] = rho * dk + r;                  // d w / d b
  g[2] = b * dk;                        // d w / d rho
  g[3] = -b * rho - b * dk * inv_r;     // d w / d m
  g[4] = b * sigma * inv_r;             // d w / d sigma
}

// Weighted price-domain SSE (pure European Black-76 prediction — see the PORT
// NOTE on the American-correction cache). Mirrors `svi_mm_sse`.
[[nodiscard]] double svi_mm_sse(std::span<const FitObs> obs, double T, double a,
                                double b, double rho, double m, double sigma) {
  double s = 0.0;
  for (const FitObs &o : obs) {
    double w_pred = svi_w_raw(a, b, rho, m, sigma, o.k);
    if (w_pred < 1.0e-12) {
      w_pred = 1.0e-12;
    }
    const double sig_pred = std::sqrt(w_pred / T);
    const double p_pred = black76_price(o.F, o.K, T, sig_pred, o.df, o.side);
    if (!std::isfinite(p_pred)) {
      continue;
    }
    const double r = p_pred - o.mid;
    s += o.active_weight_w * r * r;
  }
  return s;
}

// Per-observation LM workspace: base residuals + 5 Jacobian columns.
struct LmWorkspaceMm {
  std::vector<double> r0;
  std::array<std::vector<double>, 5> jcol;
};

// Residuals + analytic 5-column price Jacobian at (a, b, rho, m, sigma).
// Mirrors `svi_mm_residuals_and_jac` with corr == NULL.
void svi_mm_residuals_and_jac(std::span<const FitObs> obs, double T, double a,
                              double b, double rho, double m, double sigma,
                              LmWorkspaceMm &ws) {
  for (std::size_t i = 0; i < obs.size(); ++i) {
    const FitObs &o = obs[i];
    double w_pred = svi_w_raw(a, b, rho, m, sigma, o.k);
    if (w_pred < 1.0e-12) {
      w_pred = 1.0e-12;
    }
    const double sig_p = std::sqrt(w_pred / T);

    const Black76ValueVega bv =
        black76_value_and_vega(o.F, o.K, T, sig_p, o.df, o.side);
    const double p_pred = bv.price;
    ws.r0[i] = std::isfinite(p_pred) ? (p_pred - o.mid) : 0.0;

    // d Price / d sigma = B76 vega (no American correction term here).
    const double dprice_dsigma = bv.vega;

    // sigma chain rule: d sigma / d x = (1 / (2 sigma T)) d w / d x.
    std::array<double, 5> wg{};
    svi_w_grad_at(o.k, b, rho, m, sigma, wg);
    const double f = dprice_dsigma / (2.0 * sig_p * T);
    for (int j = 0; j < 5; ++j) {
      ws.jcol[static_cast<std::size_t>(j)][i] = f * wg[static_cast<std::size_t>(j)];
    }
  }
}

// Assemble H = J^T W J and g = J^T W r. Mirrors `build_normal_eqs_5`.
void build_normal_eqs_5(std::span<const FitObs> obs, const LmWorkspaceMm &ws,
                        std::array<double, 25> &H, std::array<double, 5> &g) {
  H.fill(0.0);
  g.fill(0.0);
  for (std::size_t i = 0; i < obs.size(); ++i) {
    const double wi = obs[i].active_weight_w;
    std::array<double, 5> j{};
    for (int p = 0; p < 5; ++p) {
      j[static_cast<std::size_t>(p)] = ws.jcol[static_cast<std::size_t>(p)][i];
    }
    const double r = ws.r0[i];
    for (int p = 0; p < 5; ++p) {
      g[static_cast<std::size_t>(p)] += wi * j[static_cast<std::size_t>(p)] * r;
      for (int q = 0; q <= p; ++q) {
        H[static_cast<std::size_t>(p * 5 + q)] +=
            wi * j[static_cast<std::size_t>(p)] * j[static_cast<std::size_t>(q)];
      }
    }
  }
  for (int p = 0; p < 5; ++p) {
    for (int q = 0; q < p; ++q) {
      H[static_cast<std::size_t>(q * 5 + p)] = H[static_cast<std::size_t>(p * 5 + q)];
    }
  }
}

// One projected-LM step. Returns the post-step SSE if a step is accepted, or
// `prev_sse` when every backtrack is rejected. Mirrors `svi_mm_lm_step`.
[[nodiscard]] double svi_mm_lm_step(std::span<const FitObs> obs, double T,
                                    double &a, double &b, double &rho, double &m,
                                    double &sigma, double prev_sse,
                                    double &lambda_lm, LmWorkspaceMm &ws) {
  svi_mm_residuals_and_jac(obs, T, a, b, rho, m, sigma, ws);

  std::array<double, 25> H{};
  std::array<double, 5> g{};
  build_normal_eqs_5(obs, ws, H, g);

  for (int trial = 0; trial < detail::kLmTrialCap; ++trial) {
    std::array<double, 25> hd = H;
    const double damp = 1.0 + lambda_lm;
    hd[0] *= damp;
    hd[6] *= damp;
    hd[12] *= damp;
    hd[18] *= damp;
    hd[24] *= damp;

    const std::array<double, 5> neg_g{-g[0], -g[1], -g[2], -g[3], -g[4]};
    std::array<double, 5> step{};
    if (!solve_spd_dense(hd.data(), neg_g.data(), 5, step.data())) {
      lambda_lm *= detail::kLambdaGrow;
      if (lambda_lm > detail::kLambdaLmMax) {
        return prev_sse;
      }
      continue;
    }

    double frac = 1.0;
    double a_new = 0.0;
    double b_new = 0.0;
    double rho_new = 0.0;
    double m_new = 0.0;
    double sigma_new = 0.0;
    double new_sse = prev_sse;
    bool accepted = false;
    for (int bt = 0; bt < 6; ++bt) {
      a_new = a + frac * step[0];
      b_new = b + frac * step[1];
      rho_new = rho + frac * step[2];
      m_new = m + frac * step[3];
      sigma_new = sigma + frac * step[4];

      (void)mm_project_default(T, a_new, b_new, rho_new, sigma_new);

      new_sse = svi_mm_sse(obs, T, a_new, b_new, rho_new, m_new, sigma_new);
      if (std::isfinite(new_sse) && new_sse < prev_sse) {
        accepted = true;
        break;
      }
      frac *= 0.5;
    }

    if (accepted) {
      a = a_new;
      b = b_new;
      rho = rho_new;
      m = m_new;
      sigma = sigma_new;
      lambda_lm *= detail::kLambdaShrink;
      if (lambda_lm < detail::kLambdaLmMin) {
        lambda_lm = detail::kLambdaLmMin;
      }
      return new_sse;
    }
    lambda_lm *= detail::kLambdaGrow;
    if (lambda_lm > detail::kLambdaLmMax) {
      return prev_sse;
    }
  }
  return prev_sse;
}

// ── Working-copy helper ──────────────────────────────────────────────────

// The C fitters mutate the observation array (active_weight_w, and the SVI
// wing-floor overwrites weight_w). The public API takes a `const` span, so the
// fitters run on a private copy.
[[nodiscard]] std::vector<FitObs> copy_obs(std::span<const FitObs> obs) {
  return std::vector<FitObs>(obs.begin(), obs.end());
}

}  // namespace

// ── Quasi-explicit SVI fit ───────────────────────────────────────────────

Result<SviParams> svi_fit_slice(std::span<const FitObs> obs, double T, double F,
                                const CalibOpts &opts, FitDiag *diag) {
  if (obs.empty() || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "svi_fit_slice: empty observations or non-positive T");
  }

  std::vector<FitObs> work = copy_obs(obs);
  const std::size_t n = work.size();

  // Wing-floor weight (Sprint 07 B2): floor each obs weight at
  // alpha * max(weight) so OTM rows keep a non-zero gradient.
  if (opts.wing_floor_alpha > 0.0) {
    double max_weight = 0.0;
    for (const FitObs &o : work) {
      if (o.weight_w > max_weight) {
        max_weight = o.weight_w;
      }
    }
    const double weight_floor = opts.wing_floor_alpha * max_weight;
    for (FitObs &o : work) {
      if (o.weight_w < weight_floor) {
        o.weight_w = weight_floor;
      }
    }
  }

  for (FitObs &o : work) {
    o.active_weight_w = o.weight_w;
  }

  // Bounds: m within the data span (a touch wider); sigma in [tiny, span].
  double k_min = work[0].k;
  double k_max = work[0].k;
  double w_max = work[0].w_mkt;
  for (const FitObs &o : work) {
    if (o.k < k_min) {
      k_min = o.k;
    }
    if (o.k > k_max) {
      k_max = o.k;
    }
    if (o.w_mkt > w_max) {
      w_max = o.w_mkt;
    }
  }
  const double kspan = k_max - k_min;
  const double a_max = w_max * 1.5 + 1.0e-3;
  double sigma_max = (kspan > 0.05) ? kspan : 0.5;
  if (sigma_max < 0.05) {
    sigma_max = 0.05;
  }
  const double sigma_min = 1.0e-3;

  double m_cur = 0.0;
  double sigma_cur = 0.10;

  NmCtx nm{};
  nm.obs = std::span<const FitObs>(work);
  nm.a_max = a_max;
  nm.duvc_max = 2.0 * kSqrt2 * sigma_max;
  nm.sigma_min = sigma_min;
  nm.sigma_max = sigma_max;
  nm.m_min = k_min - 0.5 * (kspan + 0.1);
  nm.m_max = k_max + 0.5 * (kspan + 0.1);

  std::array<double, 3> linear{};
  std::uint16_t outer_total_iters = 0;
  std::uint16_t inner_total_iters = 0;

  const std::uint16_t max_outer = detail::outer_cap(opts);
  const std::uint16_t max_inner = static_cast<std::uint16_t>(
      (opts.max_inner_iter > 0) ? opts.max_inner_iter : 200);
  const double tol = (opts.tol_residual > 0.0) ? opts.tol_residual : 1.0e-10;

  std::vector<double> resid_scratch(n, 0.0);
  double prev_sse = std::numeric_limits<double>::infinity();

  for (std::uint16_t outer = 0; outer < max_outer; ++outer) {
    // The (u, v) box scales with the current sigma; be generous (the real
    // butterfly bound is enforced post-hoc).
    nm.duvc_max = 2.0 * kSqrt2 * ((sigma_cur > sigma_min) ? sigma_cur : sigma_min);
    nm.duvc_max *= 4.0;

    std::uint16_t nm_iters_this_pass = 0;
    nm_search(nm, m_cur, sigma_cur, linear, max_inner, tol, nm_iters_this_pass);
    ++outer_total_iters;
    inner_total_iters =
        static_cast<std::uint16_t>(inner_total_iters + nm_iters_this_pass);

    // IRLS Huber reweight on sigma-space residuals.
    double sumw = 0.0;
    double sumwr2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const FitObs &o = work[i];
      const double yi = (o.k - m_cur) / sigma_cur;
      const double zi = std::sqrt(yi * yi + 1.0);
      const double ui = (yi + zi) * kInvSqrt2;
      const double vi = (zi - yi) * kInvSqrt2;
      double w_pred = linear[0] + linear[1] * ui + linear[2] * vi;
      if (w_pred < 1.0e-12) {
        w_pred = 1.0e-12;
      }
      const double sig_pred = std::sqrt(w_pred / T);
      const double r_sig = sig_pred - o.sigma_mkt;
      resid_scratch[i] = r_sig;
      sumw += o.weight_w;
      sumwr2 += o.weight_w * r_sig * r_sig;
    }
    const double sigma_resid = (sumw > 1.0e-15) ? std::sqrt(sumwr2 / sumw) : 0.0;

    double sse_sigma = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const double r = resid_scratch[i];
      sse_sigma += work[i].weight_w * r * r;
    }
    if (std::fabs(prev_sse - sse_sigma) < 1.0e-15) {
      break;
    }
    prev_sse = sse_sigma;

    if (sigma_resid > 1.0e-12) {
      const double huber = (opts.huber_k > 0.0) ? opts.huber_k : 1.5;
      for (std::size_t i = 0; i < n; ++i) {
        const double r_norm = std::fabs(resid_scratch[i]) / sigma_resid;
        work[i].active_weight_w = (r_norm <= huber)
                                      ? work[i].weight_w
                                      : work[i].weight_w * (huber / r_norm);
      }
    }
  }

  // Map (a, d_uv, c_uv) -> (a, b, rho).
  const double a_fit = linear[0];
  const double d_uv = linear[1];
  const double c_uv = linear[2];
  const double c_raw = (d_uv + c_uv) * kInvSqrt2;  // b * sigma
  const double d_raw = (d_uv - c_uv) * kInvSqrt2;  // b * rho * sigma
  const double b_fit = c_raw / sigma_cur;
  double rho_fit = (c_raw > 1.0e-12) ? d_raw / c_raw : 0.0;
  if (rho_fit > 0.999) {
    rho_fit = 0.999;
  }
  if (rho_fit < -0.999) {
    rho_fit = -0.999;
  }

  // Final vega-weighted RMSE in sigma space.
  double s = 0.0;
  double w_acc = 0.0;
  double mx = 0.0;
  for (const FitObs &o : work) {
    double w_pred = svi_w_raw(a_fit, b_fit, rho_fit, m_cur, sigma_cur, o.k);
    if (w_pred < 1.0e-12) {
      w_pred = 1.0e-12;
    }
    const double sig_pred = std::sqrt(w_pred / T);
    const double r = sig_pred - o.sigma_mkt;
    s += o.weight_w * r * r;
    w_acc += o.weight_w;
    if (std::fabs(r) > mx) {
      mx = std::fabs(r);
    }
  }
  const double rmse = (w_acc > 1.0e-15) ? std::sqrt(s / w_acc) : 0.0;

  SviParams out{};
  out.a = a_fit;
  out.b = b_fit;
  out.rho = rho_fit;
  out.m = m_cur;
  out.sigma = sigma_cur;
  out.T = T;
  out.F = F;

  if (diag != nullptr) {
    diag->rmse_vol_vega_weighted = rmse;
    diag->max_residual_vol = mx;
    diag->outer_iters = outer_total_iters;
    diag->inner_iters_total = inner_total_iters;
    diag->n_quotes_used = static_cast<std::uint32_t>(n);
  }

  return Ok(out);
}

// ── Martini-Mingone constrained fit ──────────────────────────────────────

Result<SviParams> svi_mm_fit_slice(std::span<const FitObs> obs, double T,
                                   double F, const CalibOpts &opts,
                                   FitDiag *diag) {
  if (obs.empty() || !(T > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "svi_mm_fit_slice: empty observations or non-positive T");
  }

  // Seed from the quasi-explicit fit on the ORIGINAL IV-domain weights (before
  // the price-domain weight overwrite below). svi_fit_slice copies the span, so
  // the caller's observations are untouched.
  double a = 0.0;
  double b = 0.1;
  double rho = -0.20;
  double m = 0.0;
  double sigma = 0.10;
  {
    const Result<SviParams> seed = svi_fit_slice(obs, T, F, opts, nullptr);
    if (seed.has_value()) {
      a = seed->a;
      b = seed->b;
      rho = seed->rho;
      m = seed->m;
      sigma = seed->sigma;
    }
  }

  std::vector<FitObs> work = copy_obs(obs);
  const std::size_t n = work.size();

  // Price-domain weight 1 / (spread^2 + (0.1*tick*vega)^2) (Sprint 13b).
  {
    constexpr double kTick = 0.01;
    constexpr double kTickFloor = 0.1;
    for (FitObs &o : work) {
      const double sp = (o.spread > 0.0) ? o.spread : 0.0;
      const double v = (o.vega > 0.0) ? o.vega : 0.0;
      const double floor_term = kTickFloor * kTick * v;
      const double denom2 = sp * sp + floor_term * floor_term + 1.0e-18;
      o.weight_w = 1.0 / denom2;
      o.active_weight_w = o.weight_w;
    }
  }

  // Wing-floor weight on the price-domain weight (Sprint 07 B2; alpha 0 = off).
  if (opts.wing_floor_alpha > 0.0) {
    double max_weight = 0.0;
    for (const FitObs &o : work) {
      if (o.weight_w > max_weight) {
        max_weight = o.weight_w;
      }
    }
    const double weight_floor = opts.wing_floor_alpha * max_weight;
    for (FitObs &o : work) {
      if (o.weight_w < weight_floor) {
        o.weight_w = weight_floor;
      }
      o.active_weight_w = o.weight_w;
    }
  }

  // Project the seed onto the admissible polytope.
  (void)mm_project_default(T, a, b, rho, sigma);

  const std::span<const FitObs> wspan{work};
  const std::uint16_t max_outer = detail::outer_cap(opts);
  const std::uint16_t max_inner = static_cast<std::uint16_t>(
      (opts.max_inner_iter > 0) ? opts.max_inner_iter : detail::kLmInnerDefault);
  const double tol_param =
      (opts.tol_param > 0.0) ? opts.tol_param : detail::kTolParamDefault;

  LmWorkspaceMm ws{};
  ws.r0.assign(n, 0.0);
  for (auto &col : ws.jcol) {
    col.assign(n, 0.0);
  }

  std::uint16_t outer_iters = 0;
  std::uint16_t inner_total = 0;
  double prev_outer_sse = std::numeric_limits<double>::infinity();

  // Morozov noise-floor stop in price-domain SSE units (Sprint 07 B6).
  double morozov_floor = 0.0;
  if (opts.morozov_stop) {
    double sum = 0.0;
    std::size_t cnt = 0;
    for (const FitObs &o : work) {
      if (o.spread > 0.0) {
        const double hs = 0.5 * o.spread;
        sum += o.active_weight_w * hs * hs;
        ++cnt;
      }
    }
    if (cnt > 0) {
      const double tau = (opts.morozov_tau > 0.0) ? opts.morozov_tau : 1.1;
      morozov_floor = tau * tau * sum;
    }
  }

  std::vector<double> resid_scratch(n, 0.0);

  for (std::uint16_t outer = 0; outer < max_outer; ++outer) {
    double sse = svi_mm_sse(wspan, T, a, b, rho, m, sigma);
    double lambda_lm = detail::kLambdaLmInit;

    for (std::uint16_t inner = 0; inner < max_inner; ++inner) {
      const double a_old = a;
      const double b_old = b;
      const double rho_old = rho;
      const double m_old = m;
      const double sigma_old = sigma;

      const double new_sse =
          svi_mm_lm_step(wspan, T, a, b, rho, m, sigma, sse, lambda_lm, ws);
      ++inner_total;
      const double da = a - a_old;
      const double db = b - b_old;
      const double dr = rho - rho_old;
      const double dm = m - m_old;
      const double ds = sigma - sigma_old;
      const double step_norm2 = da * da + db * db + dr * dr + dm * dm + ds * ds;
      sse = new_sse;
      if (step_norm2 < tol_param * tol_param) {
        break;
      }
      if (morozov_floor > 0.0 && sse <= morozov_floor) {
        break;
      }
    }
    if (morozov_floor > 0.0 && sse <= morozov_floor) {
      outer_iters = static_cast<std::uint16_t>(outer + 1);
      break;
    }
    outer_iters = static_cast<std::uint16_t>(outer + 1);

    // IRLS Huber reweight on price residuals normalized by half-spread (q90).
    double sumw = 0.0;
    double sumwr2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      const FitObs &o = work[i];
      double w_pred = svi_w_raw(a, b, rho, m, sigma, o.k);
      if (w_pred < 1.0e-12) {
        w_pred = 1.0e-12;
      }
      const double sig_pred = std::sqrt(w_pred / T);
      const double p_pred = black76_price(o.F, o.K, T, sig_pred, o.df, o.side);
      const double r_px = std::isfinite(p_pred) ? (p_pred - o.mid) : 0.0;
      resid_scratch[i] = r_px;
      sumw += o.weight_w;
      sumwr2 += o.weight_w * r_px * r_px;
    }
    const double rms_resid_px = (sumw > 1.0e-15) ? std::sqrt(sumwr2 / sumw) : 0.0;
    if (std::fabs(prev_outer_sse - sse) < detail::kOuterStallSse) {
      break;
    }
    prev_outer_sse = sse;

    if (rms_resid_px > 1.0e-12) {
      bool have_spread = false;
      for (const FitObs &o : work) {
        if (o.spread > 1.0e-9) {
          have_spread = true;
          break;
        }
      }
      if (have_spread) {
        // q90 of half-spread-normalized residuals, floored at 1.5.
        std::vector<double> rnorm(n, 0.0);
        for (std::size_t i = 0; i < n; ++i) {
          const double hs =
              (work[i].spread > 1.0e-9) ? 0.5 * work[i].spread : 1.0e-9;
          rnorm[i] = std::fabs(resid_scratch[i]) / hs;
        }
        std::size_t q_idx = static_cast<std::size_t>(0.90 * static_cast<double>(n));
        if (q_idx >= n) {
          q_idx = n - 1;
        }
        // Partial selection sort up to q_idx (bounded; n is per-slice small).
        for (std::size_t i = 0; i <= q_idx && i < n; ++i) {
          std::size_t mn = i;
          for (std::size_t j = i + 1; j < n; ++j) {
            if (rnorm[j] < rnorm[mn]) {
              mn = j;
            }
          }
          std::swap(rnorm[i], rnorm[mn]);
        }
        double huber_threshold = rnorm[q_idx];
        if (huber_threshold < 1.5) {
          huber_threshold = 1.5;
        }
        for (std::size_t i = 0; i < n; ++i) {
          const double hs =
              (work[i].spread > 1.0e-9) ? 0.5 * work[i].spread : 1.0e-9;
          const double r_norm = std::fabs(resid_scratch[i]) / hs;
          work[i].active_weight_w =
              (r_norm <= huber_threshold)
                  ? work[i].weight_w
                  : work[i].weight_w * (huber_threshold / r_norm);
        }
      } else {
        const double huber = (opts.huber_k > 0.0) ? opts.huber_k : 1.5;
        for (std::size_t i = 0; i < n; ++i) {
          const double r_norm = std::fabs(resid_scratch[i]) / rms_resid_px;
          work[i].active_weight_w = (r_norm <= huber)
                                        ? work[i].weight_w
                                        : work[i].weight_w * (huber / r_norm);
        }
      }
    }
  }

  // Final defensive projection (belt-and-braces; every accepted iterate already
  // passed through the projector).
  (void)mm_project_default(T, a, b, rho, sigma);

  // Final RMSE in vol-bps-equivalent of the price residual (r_px / vega).
  double sumw = 0.0;
  double sumwr2 = 0.0;
  double max_res = 0.0;
  for (const FitObs &o : work) {
    double w_pred = svi_w_raw(a, b, rho, m, sigma, o.k);
    if (w_pred < 1.0e-12) {
      w_pred = 1.0e-12;
    }
    const double sig_pred = std::sqrt(w_pred / T);
    const double p_pred = black76_price(o.F, o.K, T, sig_pred, o.df, o.side);
    const double r_px = std::isfinite(p_pred) ? (p_pred - o.mid) : 0.0;
    const double v = (o.vega > 1.0e-12) ? o.vega : 1.0e-12;
    const double r = r_px / v;
    sumw += o.weight_w;
    sumwr2 += o.weight_w * r * r;
    if (std::fabs(r) > max_res) {
      max_res = std::fabs(r);
    }
  }
  const double rmse = (sumw > 1.0e-15) ? std::sqrt(sumwr2 / sumw) : 0.0;

  SviParams out{};
  out.a = a;
  out.b = b;
  out.rho = rho;
  out.m = m;
  out.sigma = sigma;
  out.T = T;
  out.F = F;

  if (diag != nullptr) {
    diag->rmse_vol_vega_weighted = rmse;
    diag->max_residual_vol = max_res;
    diag->outer_iters = outer_iters;
    diag->inner_iters_total = inner_total;
    diag->n_quotes_used = static_cast<std::uint32_t>(n);
  }

  return Ok(out);
}

// ── Raw <-> Jump-Wings ───────────────────────────────────────────────────

SviJwParams svi_raw_to_jw(const SviParams &raw) noexcept {
  const double a = raw.a;
  const double b = raw.b;
  const double rho = raw.rho;
  const double m = raw.m;
  const double sigma = raw.sigma;
  const double T = raw.T;

  SviJwParams jw{};
  jw.T = T;

  const double m2_plus_s2 = m * m + sigma * sigma;
  const double sq_ms = std::sqrt(m2_plus_s2);
  const double w0 = a + b * (sq_ms - rho * m);
  const double v = (T > 0.0) ? (w0 / T) : 0.0;
  jw.v = v;

  const double sqrt_vt = std::sqrt(v * T);
  const double inv_sq_ms = (sq_ms > 1.0e-15) ? 1.0 / sq_ms : 0.0;
  jw.psi = (sqrt_vt > 1.0e-15) ? 0.5 * b * (rho - m * inv_sq_ms) / sqrt_vt : 0.0;
  jw.p = (sqrt_vt > 1.0e-15) ? b * (1.0 - rho) / sqrt_vt : 0.0;
  jw.c = (sqrt_vt > 1.0e-15) ? b * (1.0 + rho) / sqrt_vt : 0.0;

  const double w_min = a + b * sigma * std::sqrt(1.0 - rho * rho);
  jw.v_min = (T > 0.0) ? (w_min / T) : 0.0;
  return jw;
}

Result<SviParams> svi_jw_to_raw(const SviJwParams &jw) {
  const double v = jw.v;
  const double psi = jw.psi;
  const double p = jw.p;
  const double c = jw.c;
  const double v_min = jw.v_min;
  const double T = jw.T;

  if (!(T > 0.0) || !(v > 0.0) || !(v_min > 0.0)) {
    return Err(ErrorCode::OutOfRange, "svi_jw_to_raw: non-positive T / v / v_min");
  }
  if (!(p > 0.0) || !(c > 0.0)) {
    return Err(ErrorCode::OutOfRange, "svi_jw_to_raw: non-positive wing slope");
  }
  if (v_min > v) {
    return Err(ErrorCode::OutOfRange, "svi_jw_to_raw: v_min exceeds v");
  }

  const double sqrt_vt = std::sqrt(v * T);
  const double b = 0.5 * sqrt_vt * (c + p);
  const double rho = 1.0 - 2.0 * p / (p + c);
  if (std::fabs(rho) >= 1.0) {
    return Err(ErrorCode::OutOfRange, "svi_jw_to_raw: |rho| >= 1");
  }

  const double beta = rho - 2.0 * psi * sqrt_vt / b;
  if (std::fabs(beta) > 1.0) {
    return Err(ErrorCode::OutOfRange, "svi_jw_to_raw: |beta| > 1");
  }

  SviParams out{};
  out.T = T;

  // Symmetric smile (m = 0).
  if (std::fabs(beta) < 1.0e-12) {
    const double sigma =
        (v - v_min) * T / (b * (1.0 - std::sqrt(1.0 - rho * rho)) + 1.0e-18);
    out.a = v_min * T - b * sigma * std::sqrt(1.0 - rho * rho);
    out.b = b;
    out.rho = rho;
    out.m = 0.0;
    out.sigma = sigma;
    return Ok(out);
  }

  const double alpha2 = 1.0 / (beta * beta) - 1.0;
  if (alpha2 < 0.0) {
    return Err(ErrorCode::OutOfRange, "svi_jw_to_raw: negative alpha^2");
  }
  const double alpha = ((beta > 0.0) ? 1.0 : -1.0) * std::sqrt(alpha2);

  const double sgn_a = (alpha >= 0.0) ? 1.0 : -1.0;
  const double d = -rho + sgn_a * std::sqrt(1.0 + alpha2) -
                   alpha * std::sqrt(1.0 - rho * rho);
  if (std::fabs(d) < 1.0e-15) {
    return Err(ErrorCode::OutOfRange, "svi_jw_to_raw: degenerate denominator");
  }

  const double m = (v - v_min) * T / (b * d);
  const double sigma = alpha * m;
  if (sigma <= 0.0) {
    return Err(ErrorCode::OutOfRange, "svi_jw_to_raw: non-positive sigma");
  }

  out.a = v_min * T - b * sigma * std::sqrt(1.0 - rho * rho);
  out.b = b;
  out.rho = rho;
  out.m = m;
  out.sigma = sigma;
  return Ok(out);
}

// ── Surface drivers ──────────────────────────────────────────────────────

namespace {

// Shared aggregation across the per-slice fits for both surface drivers.
struct SurfaceAccum {
  std::uint32_t total_used{0};
  std::uint32_t total_dropped{0};
  double sum_sse{0.0};
  double sum_w{0.0};
  double max_res{0.0};
  std::uint32_t agg_outer{0};
  std::uint32_t agg_inner{0};
  std::uint16_t n_fit_ok{0};
};

void stamp_surface(VolSurface &surface, const SurfaceAccum &acc, FitDiag *diag) {
  const double rmse_global =
      (acc.sum_w > 0.0) ? std::sqrt(acc.sum_sse / acc.sum_w) : 0.0;
  VolSurface::Diagnostics d{};
  d.rmse_vol = rmse_global;
  d.max_residual_vol = acc.max_res;
  d.n_quotes_used = acc.total_used;
  d.n_quotes_dropped = acc.total_dropped;
  surface.set_diagnostics(d);

  if (diag != nullptr) {
    diag->rmse_vol_vega_weighted = rmse_global;
    diag->max_residual_vol = acc.max_res;
    diag->outer_iters = static_cast<std::uint16_t>(
        (acc.agg_outer > 0xFFFFu) ? 0xFFFFu : acc.agg_outer);
    diag->inner_iters_total = static_cast<std::uint16_t>(
        (acc.agg_inner > 0xFFFFu) ? 0xFFFFu : acc.agg_inner);
    diag->n_quotes_used = acc.total_used;
  }
}

}  // namespace

Status svi_calib_surface(VolSurface &surface, const Underlying &under,
                         const CurveSet &cs, const CalibOpts &opts,
                         FitDiag *diag) {
  if (surface.param() != Parametrization::Svi) {
    return Err(ErrorCode::InvalidArgument,
               "svi_calib_surface: surface is not SVI-parametrized");
  }
  if (under.chains.empty()) {
    return Err(ErrorCode::NotFound, "svi_calib_surface: underlying has no chains");
  }

  SurfaceAccum acc{};

  for (const Chain &c : under.chains) {
    if (static_cast<std::size_t>(acc.n_fit_ok) >= surface.capacity()) {
      break;
    }
    const double T = c.T;
    if (!(T > kTMinFit)) {
      continue;
    }
    const double F = cs.forward.forward_at(c.expiry_id);
    if (!std::isfinite(F)) {
      continue;
    }
    const double df = cs.yield.disc(T);

    const Result<ObsSet> obs_res = build_observations(c, F, T, df, opts);
    if (!obs_res.has_value()) {
      // NotFound => too few quotes; drops are tallied inside build_observations
      // only on the success path, so nothing to add here (matches the C, which
      // still adds n_drop — see PORT NOTE). Skip this chain.
      continue;
    }
    const ObsSet &os = obs_res.value();
    acc.total_dropped += os.n_dropped;

    // Decline-to-fit on insufficient signal (Sprint 24 Phase B).
    if (opts.min_obs_per_slice > 0 &&
        static_cast<std::uint32_t>(os.obs.size()) < opts.min_obs_per_slice) {
      continue;
    }

    FitDiag sd{};
    const Result<SviParams> fit_res =
        svi_fit_slice(std::span<const FitObs>(os.obs), T, F, opts, &sd);
    if (!fit_res.has_value()) {
      continue;
    }
    SviParams slice = fit_res.value();

    // Post-fit sigma clamp (Sprint 24 Phase B + Sprint 26).
    const double w_atm = slice.a + slice.b * (-slice.rho * slice.m +
                                              std::sqrt(slice.m * slice.m +
                                                        slice.sigma * slice.sigma));
    if (w_atm > 0.0) {
      const double sigma_atm = std::sqrt(w_atm / T);
      if (opts.max_post_fit_sigma > 0.0 && sigma_atm > opts.max_post_fit_sigma) {
        continue;
      }
      if (c.source_atm_vol_present && std::isfinite(c.source_atm_vol) &&
          c.source_atm_vol > 0.0 && sigma_atm > 2.5 * c.source_atm_vol) {
        continue;
      }
    }

    // Post-fit positivity gate: reject a converged slice with non-positive total
    // variance rather than store a silent garbage fit (see the helper).
    if (!svi_slice_variance_positive(slice, std::span<const FitObs>(os.obs))) {
      continue;
    }

    slice.F = F;
    slice.expiry_id = c.expiry_id;
    slice.expiry_ns = c.expiry_ns;
    slice.T = T;
    const Status set_rc = surface.set_slice_svi(acc.n_fit_ok, slice);
    if (!set_rc.has_value()) {
      continue;
    }

    acc.total_used += sd.n_quotes_used;
    acc.sum_sse += sd.rmse_vol_vega_weighted * sd.rmse_vol_vega_weighted *
                   static_cast<double>(sd.n_quotes_used);
    acc.sum_w += static_cast<double>(sd.n_quotes_used);
    if (sd.max_residual_vol > acc.max_res) {
      acc.max_res = sd.max_residual_vol;
    }
    acc.agg_outer += sd.outer_iters;
    acc.agg_inner += sd.inner_iters_total;
    ++acc.n_fit_ok;
  }

  if (acc.n_fit_ok >= 2 && opts.validate_no_arb) {
    const Status proj = arb_project_calendar_svi(surface, -1.5, 1.5, 64u);
    (void)proj;  // non-fatal; only fails on k_max <= k_min (not the case here)
  }

  stamp_surface(surface, acc, diag);
  return (acc.n_fit_ok > 0)
             ? Ok()
             : Err(ErrorCode::NotFound, "svi_calib_surface: no slice fit");
}

Status svi_mm_calib_surface(VolSurface &surface, const Underlying &under,
                            const CurveSet &cs, const CalibOpts &opts,
                            FitDiag *diag) {
  if (surface.param() != Parametrization::SviMm) {
    return Err(ErrorCode::InvalidArgument,
               "svi_mm_calib_surface: surface is not SVI-MM-parametrized");
  }
  if (under.chains.empty()) {
    return Err(ErrorCode::NotFound,
               "svi_mm_calib_surface: underlying has no chains");
  }

  SurfaceAccum acc{};

  for (const Chain &c : under.chains) {
    if (static_cast<std::size_t>(acc.n_fit_ok) >= surface.capacity()) {
      break;
    }
    const double T = c.T;
    if (!(T > kTMinFit)) {
      continue;
    }
    const double F = cs.forward.forward_at(c.expiry_id);
    if (!std::isfinite(F)) {
      continue;
    }
    const double df = cs.yield.disc(T);

    const Result<ObsSet> obs_res = build_observations(c, F, T, df, opts);
    if (!obs_res.has_value()) {
      continue;
    }
    const ObsSet &os = obs_res.value();
    acc.total_dropped += os.n_dropped;

    FitDiag sd{};
    const Result<SviParams> fit_res =
        svi_mm_fit_slice(std::span<const FitObs>(os.obs), T, F, opts, &sd);
    if (!fit_res.has_value()) {
      continue;
    }
    SviParams slice = fit_res.value();
    // Post-fit positivity gate (defense-in-depth; the MM projection already
    // guarantees w_min >= edge_a > 0 — see the helper).
    if (!svi_slice_variance_positive(slice, std::span<const FitObs>(os.obs))) {
      continue;
    }
    slice.F = F;
    slice.expiry_id = c.expiry_id;
    slice.expiry_ns = c.expiry_ns;
    slice.T = T;
    const Status set_rc = surface.set_slice_svi(acc.n_fit_ok, slice);
    if (!set_rc.has_value()) {
      continue;
    }

    acc.total_used += sd.n_quotes_used;
    acc.sum_sse += sd.rmse_vol_vega_weighted * sd.rmse_vol_vega_weighted *
                   static_cast<double>(sd.n_quotes_used);
    acc.sum_w += static_cast<double>(sd.n_quotes_used);
    if (sd.max_residual_vol > acc.max_res) {
      acc.max_res = sd.max_residual_vol;
    }
    acc.agg_outer += sd.outer_iters;
    acc.agg_inner += sd.inner_iters_total;
    ++acc.n_fit_ok;
  }

  if (acc.n_fit_ok >= 2 && opts.validate_no_arb) {
    const Status proj = arb_project_calendar_svi(surface, -1.5, 1.5, 64u);
    (void)proj;
  }

  stamp_surface(surface, acc, diag);
  return (acc.n_fit_ok > 0)
             ? Ok()
             : Err(ErrorCode::NotFound, "svi_mm_calib_surface: no slice fit");
}

}  // namespace atx::vol
