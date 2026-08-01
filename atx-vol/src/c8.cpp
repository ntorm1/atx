#include "atx/vol/c8.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <utility>

#include "atx/core/error.hpp"
#include "atx/vol/detail/legacy_c8_surface.hpp" // C8Surface (demoted, S4-T21)

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// Arb-projection grid resolution (ATS_VOL_C8_ARB_GRID_N in the C).
constexpr int kArbGridN = 200;

}  // namespace

// ── Compact-support bump basis ────────────────────────────────────────────

double c8_basis_atm(double k, double h_atm) noexcept {
  if (h_atm <= 0.0) {
    return 0.0;
  }
  const double u = k / h_atm;
  if (u < -1.0 || u > 1.0) {
    return 0.0;
  }
  const double one_minus_u2 = 1.0 - u * u;
  return one_minus_u2 * one_minus_u2 * (u * u - 1.0 / 7.0);
}

double c8_basis_left(double k, double k_L, double h_L) noexcept {
  if (h_L <= 0.0) {
    return 0.0;
  }
  const double u = (k - k_L) / h_L;
  if (u < -1.0 || u > 1.0) {
    return 0.0;
  }
  // max(0, -u)^2 is u^2 when u < 0, else 0.
  const double half = (u < 0.0) ? (u * u) : 0.0;
  if (half == 0.0) {
    return 0.0;
  }
  const double one_minus_u2 = 1.0 - u * u;
  return one_minus_u2 * one_minus_u2 * half;
}

double c8_basis_right(double k, double k_R, double h_R) noexcept {
  if (h_R <= 0.0) {
    return 0.0;
  }
  const double u = (k - k_R) / h_R;
  if (u < -1.0 || u > 1.0) {
    return 0.0;
  }
  const double half = (u > 0.0) ? (u * u) : 0.0;
  if (half == 0.0) {
    return 0.0;
  }
  const double one_minus_u2 = 1.0 - u * u;
  return one_minus_u2 * one_minus_u2 * half;
}

// ── Raw SVI + JW reparametrization ────────────────────────────────────────

double c8_raw_svi_w(double k, const C8RawSvi& raw) noexcept {
  const double dk = k - raw.m;
  return raw.a + raw.b * (raw.rho * dk + std::sqrt(dk * dk + raw.sigma * raw.sigma));
}

std::optional<C8Jw> c8_raw_to_jw(const C8RawSvi& raw, double T) noexcept {
  (void)T;  // T not used for raw->JW; kept for API symmetry.
  if (raw.b < 0.0 || raw.sigma <= 0.0 || raw.rho <= -1.0 || raw.rho >= 1.0) {
    return std::nullopt;
  }
  const double dk0 = -raw.m;
  const double r0 = std::sqrt(dk0 * dk0 + raw.sigma * raw.sigma);
  C8Jw jw;
  jw.v = raw.a + raw.b * (raw.rho * dk0 + r0);
  // d w / d k at k = 0 is b*(rho + dk0/r0); psi = 1/2 of that.
  jw.psi = 0.5 * raw.b * (raw.rho + dk0 / r0);
  jw.p = raw.b * (1.0 - raw.rho);
  jw.c = raw.b * (1.0 + raw.rho);
  jw.v_min = raw.a + raw.b * raw.sigma * std::sqrt(1.0 - raw.rho * raw.rho);
  return jw;
}

std::optional<C8RawSvi> c8_jw_to_raw(const C8Jw& jw, double T,
                                     double sigma_floor) noexcept {
  (void)T;  // T-domain sanity checks left to the calibrator.
  if (jw.v <= 0.0 || jw.p < 0.0 || jw.c < 0.0 || jw.v_min < 0.0 ||
      jw.v_min > jw.v + 1e-12) {
    return std::nullopt;
  }
  const double cp_sum = jw.p + jw.c;
  if (cp_sum <= 0.0) {
    return std::nullopt;
  }

  C8RawSvi raw;
  raw.b = 0.5 * cp_sum;                    // p = b*(1-rho), c = b*(1+rho)
  raw.rho = 1.0 - 2.0 * jw.p / cp_sum;

  // Clamp |rho| < 1 with a small floor so the LM cannot drive a
  // divide-by-(1-rho^2)=0.
  const double rho_clip = 1.0 - 1e-9;
  raw.rho = std::clamp(raw.rho, -rho_clip, rho_clip);

  // beta := rho - 2*psi/b (with psi = 1/2 * d w / d k at k = 0). Clip into the
  // open interval rather than failing — the LM may transiently propose JW
  // points just outside admissibility, and a hard refusal there would corrupt
  // the gradient/objective consistency.
  double beta = raw.rho - 2.0 * jw.psi / raw.b;
  beta = std::clamp(beta, -rho_clip, rho_clip);

  // Unified symmetric-smile (beta -> 0) and v == v_min degenerate branches:
  // both collapse to an ATM-centered (m=0) parabola with sigma = sigma_floor.
  const double beta_floor = 1e-3;
  if (std::fabs(jw.v - jw.v_min) < 1e-12 || std::fabs(beta) < beta_floor) {
    raw.m = 0.0;
    raw.sigma = sigma_floor;
  } else {
    // alpha from beta (preserving sign of beta so m has the right sign).
    const double alpha_mag = std::sqrt(1.0 / (beta * beta) - 1.0);
    const double alpha = (beta >= 0.0) ? alpha_mag : -alpha_mag;
    const double s_alpha = (alpha >= 0.0) ? 1.0 : -1.0;
    const double sqrt_omr2 = std::sqrt(1.0 - raw.rho * raw.rho);
    const double denom =
        -raw.rho + s_alpha * std::sqrt(1.0 + alpha * alpha) - alpha * sqrt_omr2;
    if (std::fabs(denom) < 1e-12) {
      raw.m = 0.0;
      raw.sigma = sigma_floor;
    } else {
      raw.m = (jw.v - jw.v_min) / (raw.b * denom);
      raw.sigma = alpha * raw.m;
      if (raw.sigma < sigma_floor) {
        raw.sigma = sigma_floor;
      }
    }
  }
  raw.a = jw.v_min - raw.b * raw.sigma * std::sqrt(1.0 - raw.rho * raw.rho);
  return raw;
}

std::optional<std::array<std::array<double, 5>, 5>> c8_jw_to_raw_jac(
    const C8Jw& jw, double T, double sigma_floor) noexcept {
  (void)T;  // matches c8_jw_to_raw: T-domain checks left to the calibrator.
  // Same admissibility gate as c8_jw_to_raw — an inadmissible JW point has no
  // conversion and hence no Jacobian (grad availability tracks the conversion).
  if (jw.v <= 0.0 || jw.p < 0.0 || jw.c < 0.0 || jw.v_min < 0.0 ||
      jw.v_min > jw.v + 1e-12) {
    return std::nullopt;
  }
  const double cp_sum = jw.p + jw.c;
  if (cp_sum <= 0.0) {
    return std::nullopt;
  }

  // ── Recompute the forward map bit-identically to c8_jw_to_raw so the
  //    Jacobian is the exact derivative of the code AS WRITTEN. ─────────────
  const double b = 0.5 * cp_sum;

  const double rho_clip = 1.0 - 1e-9;
  const double rho_u = 1.0 - 2.0 * jw.p / cp_sum;
  const double rho = std::clamp(rho_u, -rho_clip, rho_clip);
  const bool rho_clamped = (rho_u < -rho_clip) || (rho_u > rho_clip);

  const double beta_u = rho - 2.0 * jw.psi / b;
  const double beta = std::clamp(beta_u, -rho_clip, rho_clip);
  const bool beta_clamped = (beta_u < -rho_clip) || (beta_u > rho_clip);

  const double R = std::sqrt(1.0 - rho * rho);  // sqrt(1-rho^2), in a & denom

  const double beta_floor = 1e-3;
  const bool degenerate_pre =
      (std::fabs(jw.v - jw.v_min) < 1e-12) || (std::fabs(beta) < beta_floor);

  double m = 0.0;              // 0 on the degenerate branch
  double sigma = sigma_floor;  // floor on the degenerate branch
  bool m_const = true;         // m is a hard constant (0) -> zero m-row
  bool sigma_const = true;     // sigma pinned to the floor -> zero sigma-row
  double alpha = 0.0;
  double s_alpha = 0.0;
  double S = 0.0;  // sqrt(1+alpha^2)
  double denom = 0.0;
  if (!degenerate_pre) {
    const double alpha_mag = std::sqrt(1.0 / (beta * beta) - 1.0);
    alpha = (beta >= 0.0) ? alpha_mag : -alpha_mag;
    s_alpha = (alpha >= 0.0) ? 1.0 : -1.0;
    S = std::sqrt(1.0 + alpha * alpha);
    denom = -rho + s_alpha * S - alpha * R;
    if (std::fabs(denom) >= 1e-12) {
      m = (jw.v - jw.v_min) / (b * denom);
      m_const = false;
      const double sigma_raw = alpha * m;
      if (sigma_raw < sigma_floor) {
        sigma = sigma_floor;  // floor clamp active -> sigma stays constant
      } else {
        sigma = sigma_raw;
        sigma_const = false;
      }
    }
    // else |denom| tiny: m := 0, sigma := floor (both stay constant).
  }

  // ── Base column partials (branch-independent). Columns: v,psi,p,c,v_min. ──
  const double inv_cp2 = 1.0 / (cp_sum * cp_sum);
  const std::array<double, 5> b_j{0.0, 0.0, 0.5, 0.5, 0.0};  // b = (p+c)/2
  std::array<double, 5> rho_j{};                              // 0 when clamped
  if (!rho_clamped) {
    rho_j[2] = -2.0 * jw.c * inv_cp2;  // d rho / d p = -2c/(p+c)^2
    rho_j[3] = 2.0 * jw.p * inv_cp2;   // d rho / d c =  2p/(p+c)^2
  }
  // beta = rho - 2*psi/b: beta_p,c pick up +psi/b^2 via b's p,c dependence.
  std::array<double, 5> beta_j{};  // 0 when clamped
  if (!beta_clamped) {
    const double psi_over_b2 = jw.psi / (b * b);
    beta_j[0] = rho_j[0];                // v
    beta_j[1] = rho_j[1] - 2.0 / b;      // psi
    beta_j[2] = rho_j[2] + psi_over_b2;  // p
    beta_j[3] = rho_j[3] + psi_over_b2;  // c
    beta_j[4] = rho_j[4];                // v_min
  }

  // m- and sigma-rows: identically zero on the degenerate/floored branches.
  std::array<double, 5> m_j{};
  std::array<double, 5> sigma_j{};
  if (!m_const) {
    // alpha depends on beta only; from alpha^2 = 1/beta^2 - 1,
    //   d alpha / d beta = -1 / (beta^3 * alpha).
    const double dalpha_dbeta = -1.0 / (beta * beta * beta * alpha);
    std::array<double, 5> alpha_j{};
    for (std::size_t j = 0; j < 5; ++j) {
      alpha_j[j] = dalpha_dbeta * beta_j[j];
    }
    // denom = -rho + s_alpha*S - alpha*R, with S_j = alpha*alpha_j/S and
    //   R_j = -rho*rho_j/R:
    //   denom_j = (-1 + alpha*rho/R) rho_j + (s_alpha*alpha/S - R) alpha_j.
    const double coef_rho = -1.0 + alpha * rho / R;
    const double coef_alpha = s_alpha * alpha / S - R;
    std::array<double, 5> denom_j{};
    for (std::size_t j = 0; j < 5; ++j) {
      denom_j[j] = coef_rho * rho_j[j] + coef_alpha * alpha_j[j];
    }
    // m = N / (b*denom), N = v - v_min:
    //   m_j = N_j/(b*denom) - m*(b_j/b + denom_j/denom).
    const std::array<double, 5> N_j{1.0, 0.0, 0.0, 0.0, -1.0};
    const double inv_bden = 1.0 / (b * denom);
    for (std::size_t j = 0; j < 5; ++j) {
      m_j[j] = N_j[j] * inv_bden - m * (b_j[j] / b + denom_j[j] / denom);
    }
    // sigma = alpha*m, unless floored (then sigma_j stays 0).
    if (!sigma_const) {
      for (std::size_t j = 0; j < 5; ++j) {
        sigma_j[j] = alpha_j[j] * m + alpha * m_j[j];
      }
    }
  }

  // a = v_min - b*sigma*R (R = sqrt(1-rho^2)); full chain in b, sigma, rho:
  //   a_j = vmin_j - b_j*sigma*R - b*sigma_j*R + (b*sigma*rho/R) rho_j.
  const std::array<double, 5> vmin_j{0.0, 0.0, 0.0, 0.0, 1.0};
  const double bsig_rho_over_R = b * sigma * rho / R;
  std::array<double, 5> a_j{};
  for (std::size_t j = 0; j < 5; ++j) {
    a_j[j] = vmin_j[j] - b_j[j] * sigma * R - b * sigma_j[j] * R +
             bsig_rho_over_R * rho_j[j];
  }

  std::array<std::array<double, 5>, 5> jac{};
  jac[0] = a_j;      // d a
  jac[1] = b_j;      // d b
  jac[2] = rho_j;    // d rho
  jac[3] = m_j;      // d m
  jac[4] = sigma_j;  // d sigma
  return jac;
}

// ── Slice evaluator ───────────────────────────────────────────────────────

double c8_slice_w(const C8Params& s, double k_log,
                  const std::optional<C8RawSvi>& raw_conv) noexcept {
  if (!raw_conv.has_value()) {
    // jw_to_raw was hardened to always succeed on admissible input; if it still
    // rejects (e.g. v <= 0), fall back to the stored v to keep w finite.
    return (s.v > 0.0) ? s.v : 1e-9;
  }
  double w = c8_raw_svi_w(k_log, *raw_conv);
  if (!std::isfinite(w)) {
    w = (s.v > 0.0) ? s.v : 1e-9;
  }
  if (s.bumps_active) {
    w += s.kappa * c8_basis_atm(k_log, s.h_atm);
    w += s.q_L * c8_basis_left(k_log, s.k_L, s.h_L);
    w += s.q_R * c8_basis_right(k_log, s.k_R, s.h_R);
  }
  // Final floor: w must be strictly positive for IV inversion downstream.
  if (!(w > 1e-12)) {
    w = 1e-12;
  }
  return w;
}

double c8_slice_w(const C8Params& s, double k_log) noexcept {
  return c8_slice_w(
      s, k_log,
      c8_jw_to_raw(C8Jw{s.v, s.psi, s.p, s.c, s.v_min}, s.T, 1e-4));
}

// ── 8-parameter gradient ──────────────────────────────────────────────────

namespace {

// d w / d(a,b,rho,m,sigma) for raw SVI, in that order.
[[nodiscard]] std::array<double, 5> raw_svi_grad(double k,
                                                 const C8RawSvi& raw) noexcept {
  const double dk = k - raw.m;
  const double r = std::sqrt(dk * dk + raw.sigma * raw.sigma);
  return {
      1.0,                                      // d/da
      raw.rho * dk + r,                         // d/db
      raw.b * dk,                               // d/drho
      -raw.b * (raw.rho + dk / r),              // d/dm
      raw.b * raw.sigma / r,                    // d/dsigma
  };
}

}  // namespace

std::optional<std::array<double, 8>> c8_slice_grad_w(
    const C8Params& s, double k_log, const std::optional<C8RawSvi>& raw_conv,
    const std::optional<std::array<std::array<double, 5>, 5>>&
        jac_conv) noexcept {
  if (!raw_conv.has_value() || !jac_conv.has_value()) {
    return std::nullopt;
  }
  const std::array<double, 5> rg = raw_svi_grad(k_log, *raw_conv);

  std::array<double, 8> grad{};
  // Chain: d w / d jw_j = sum_i (d w / d raw_i) * (d raw_i / d jw_j).
  for (std::size_t j = 0; j < 5; ++j) {
    double acc = 0.0;
    for (std::size_t i = 0; i < 5; ++i) {
      acc += rg[i] * (*jac_conv)[i][j];
    }
    grad[j] = acc;
  }

  if (s.bumps_active) {
    grad[5] = c8_basis_atm(k_log, s.h_atm);
    grad[6] = c8_basis_left(k_log, s.k_L, s.h_L);
    grad[7] = c8_basis_right(k_log, s.k_R, s.h_R);
  } else {
    grad[5] = 0.0;
    grad[6] = 0.0;
    grad[7] = 0.0;
  }
  return grad;
}

std::optional<std::array<double, 8>> c8_slice_grad_w(const C8Params& s,
                                                     double k_log) noexcept {
  const C8Jw jw{s.v, s.psi, s.p, s.c, s.v_min};
  return c8_slice_grad_w(s, k_log, c8_jw_to_raw(jw, s.T, 1e-4),
                         c8_jw_to_raw_jac(jw, s.T, 1e-4));
}

// ── eSSVI warm-start seed ─────────────────────────────────────────────────

std::optional<C8Params> c8_seed_from_essvi(const EssviParams& src) noexcept {
  if (src.theta <= 0.0) {
    return std::nullopt;
  }
  C8Params dst;
  dst.T = src.T;
  dst.F = src.F;
  dst.expiry_ns = src.expiry_ns;
  dst.expiry_id = src.expiry_id;

  const double scale = std::sqrt(src.theta);  // ~ sigma_atm * sqrt(T)

  // Inner knots seed (v, psi, v_min); far-wing pairs seed the asymptotic linear
  // wing slopes (p, c) so the raw-SVI wings match eSSVI's natural wing rate
  // instead of the near-ATM slope.
  const std::array<double, 3> k_inner{-1.0 * scale, 0.0, 1.0 * scale};
  const std::array<double, 2> k_far_lo{-10.0 * scale, -5.0 * scale};
  const std::array<double, 2> k_far_hi{5.0 * scale, 10.0 * scale};
  std::array<double, 3> w_in{};
  std::array<double, 2> w_lo{};
  std::array<double, 2> w_hi{};
  for (std::size_t i = 0; i < 3; ++i) {
    w_in[i] = essvi_backbone_w(src, k_inner[i]);
  }
  for (std::size_t i = 0; i < 2; ++i) {
    w_lo[i] = essvi_backbone_w(src, k_far_lo[i]);
    w_hi[i] = essvi_backbone_w(src, k_far_hi[i]);
  }

  dst.v = w_in[1];
  dst.psi = 0.5 * (w_in[2] - w_in[0]) / (k_inner[2] - k_inner[0]);
  // Asymptotic wing slopes from the far-pair finite differences.
  dst.p = (w_lo[0] - w_lo[1]) / (k_far_lo[1] - k_far_lo[0]);  // d w / d|k| left
  dst.c = (w_hi[1] - w_hi[0]) / (k_far_hi[1] - k_far_hi[0]);  // d w / d k right
  if (dst.p < 1e-6) {
    dst.p = 1e-6;
  }
  if (dst.c < 1e-6) {
    dst.c = 1e-6;
  }
  if (std::fabs(dst.p - dst.c) < 1e-9) {
    dst.p *= 0.999;
  }
  // Clip p, c so JW admissibility b*(1+|rho|) <= 4/T holds with margin (the SVI
  // no-arb wing polytope, Roger Lee). b = (p+c)/2.
  const double b = 0.5 * (dst.p + dst.c);
  const double abs_rho = std::fabs(1.0 - 2.0 * dst.p / (dst.p + dst.c));
  const double b_limit = 4.0 / src.T;
  const double b_max =
      (1.0 + abs_rho > 0.0) ? (0.95 * b_limit / (1.0 + abs_rho)) : b_limit;
  if (b > b_max) {
    const double scale_pc = b_max / b;
    dst.p *= scale_pc;
    dst.c *= scale_pc;
  }

  // v_min from the minimum across all sampled knots, clipped to (0, v].
  double w_min = w_in[0];
  w_min = std::min({w_min, w_in[1], w_in[2], w_lo[0], w_lo[1], w_hi[0], w_hi[1]});
  if (w_min < 1e-9) {
    w_min = 1e-9;
  }
  if (w_min > dst.v) {
    w_min = dst.v;
  }
  dst.v_min = w_min;

  dst.kappa = 0.0;
  dst.q_L = 0.0;
  dst.q_R = 0.0;
  dst.h_atm = scale;
  dst.k_L = -2.5 * scale;
  dst.h_L = scale;
  dst.k_R = 2.5 * scale;
  dst.h_R = scale;
  dst.bumps_active = true;
  dst.arb_damping_factor = 1.0;
  dst.rmse_price = 0.0;
  dst.rmse_vol = 0.0;
  dst.n_lm_iters = 0;
  dst.n_irls_iters = 0;
  return dst;
}

// ── No-arbitrage (Roper density) projection ───────────────────────────────

namespace {

// w, w', w'' at k via central finite differences on c8_slice_w (h = 1e-4).
struct WDerivs {
  double w;
  double wp;
  double wpp;
};

[[nodiscard]] WDerivs w_and_derivs(const C8Params& s, double k) noexcept {
  const double h = 1e-4;
  const double w0 = c8_slice_w(s, k);
  const double wpl = c8_slice_w(s, k + h);
  const double wmn = c8_slice_w(s, k - h);
  return {w0, (wpl - wmn) / (2.0 * h), (wpl - 2.0 * w0 + wmn) / (h * h)};
}

[[nodiscard]] double roper_g_at(const C8Params& s, double k) noexcept {
  const WDerivs d = w_and_derivs(s, k);
  if (!(d.w > 0.0)) {
    return -kInf;
  }
  const double t = 1.0 - k * d.wp / (2.0 * d.w);
  return t * t - 0.25 * d.wp * d.wp * (1.0 / d.w + 0.25) + 0.5 * d.wpp;
}

// Bisect a single coefficient between 0 (arb-free) and `target`. Returns the
// largest fraction in [0,1] that keeps min g >= 0 with the other two
// coefficients held; writes coef = fraction*target on exit.
[[nodiscard]] double arb_bisect_one(C8Params& s, double& coef,
                                    double target) noexcept {
  coef = target;
  if (c8_min_roper_g(s) >= 0.0) {
    return 1.0;
  }
  double lo = 0.0;
  double hi = 1.0;
  for (int it = 0; it < 30; ++it) {
    const double mid = 0.5 * (lo + hi);
    coef = mid * target;
    if (c8_min_roper_g(s) >= 0.0) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  coef = lo * target;
  return lo;
}

}  // namespace

double c8_min_roper_g(const C8Params& s) noexcept {
  const double scale = std::sqrt(s.v > 0.0 ? s.v : 1e-9);
  // Cover the full bump support, not just +/-3 sigma_atm.
  const double pad = 0.5 * scale;
  const double k_lo_bump = s.k_L - s.h_L;
  const double k_hi_bump = s.k_R + s.h_R;
  double k_lo = -3.5 * scale - pad;
  double k_hi = 3.5 * scale + pad;
  if (k_lo_bump < k_lo) {
    k_lo = k_lo_bump - pad;
  }
  if (k_hi_bump > k_hi) {
    k_hi = k_hi_bump + pad;
  }
  double g_min = kInf;
  for (int i = 0; i < kArbGridN; ++i) {
    const double k = k_lo + (k_hi - k_lo) * static_cast<double>(i) /
                                static_cast<double>(kArbGridN - 1);
    g_min = std::min(g_min, roper_g_at(s, k));
  }
  return g_min;
}

void c8_arb_project(C8Params& s) noexcept {
  if (!s.bumps_active) {
    s.arb_damping_factor = 1.0;
    return;
  }
  if (c8_min_roper_g(s) >= 0.0) {
    s.arb_damping_factor = 1.0;
    return;
  }
  // Damp each bump independently (disjoint / near-disjoint supports): zero all
  // three, then re-introduce one at a time with per-coefficient bisection. ATM
  // first (most often binding), then left wing, then right wing.
  const double kappa_full = s.kappa;
  const double qL_full = s.q_L;
  const double qR_full = s.q_R;
  s.kappa = 0.0;
  s.q_L = 0.0;
  s.q_R = 0.0;
  const double l_atm = arb_bisect_one(s, s.kappa, kappa_full);
  const double l_L = arb_bisect_one(s, s.q_L, qL_full);
  const double l_R = arb_bisect_one(s, s.q_R, qR_full);
  s.arb_damping_factor = std::min({l_atm, l_L, l_R});
}

// ── C8Surface ──────────────────────────────────────────────────────────────

Result<C8Surface> C8Surface::create(std::uint32_t uid, std::size_t cap_slices) {
  if (cap_slices == 0) {
    return Err(ErrorCode::InvalidArgument,
               "C8Surface::create: cap_slices must be > 0");
  }
  C8Surface surf;
  surf.uid_ = uid;
  surf.cap_slices_ = cap_slices;
  surf.slices_.reserve(cap_slices);
  return Ok(std::move(surf));
}

Status C8Surface::set_slice(std::size_t idx, const C8Params& slice) {
  if (idx >= cap_slices_) {
    return Err(ErrorCode::OutOfRange, "C8Surface::set_slice: idx exceeds capacity");
  }
  if (idx >= slices_.size()) {
    slices_.resize(idx + 1);
  }
  slices_[idx] = slice;
  return Ok();
}

double C8Surface::w(double k_log, double T) const noexcept {
  const std::size_t n = slices_.size();
  if (n == 0) {
    return kNaN;
  }
  if (T < kTMinEval) {
    T = kTMinEval;
  }

  const double T0 = slices_[0].T;
  if (T <= T0) {
    // Short-T extrapolation guard: refuse when the query sits materially below
    // the first slice.
    if (T < 0.5 * T0) {
      return kNaN;
    }
    return c8_slice_w(slices_[0], k_log);
  }
  const double T_last = slices_[n - 1].T;
  if (T == T_last) {
    return c8_slice_w(slices_[n - 1], k_log);
  }
  if (T > T_last) {
    return kNaN;  // no extrapolation past the longest slice
  }

  // Binary search for the bracket [lo, hi] with T_lo <= T < T_hi.
  std::size_t lo = 0;
  std::size_t hi = n - 1;
  while (hi - lo > 1) {
    const std::size_t mid = (lo + hi) / 2;
    if (slices_[mid].T <= T) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double T_lo = slices_[lo].T;
  const double T_hi = slices_[hi].T;
  const double w_lo = c8_slice_w(slices_[lo], k_log);
  const double w_hi = c8_slice_w(slices_[hi], k_log);
  const double alpha = (T - T_lo) / (T_hi - T_lo);
  return w_lo + alpha * (w_hi - w_lo);
}

double C8Surface::iv(double k_log, double T) const noexcept {
  const double wv = w(k_log, T);
  if (!std::isfinite(wv) || wv <= 0.0) {
    return kNaN;
  }
  // Divide by the caller's ORIGINAL T (matches the C's surface_iv).
  return std::sqrt(wv / T);
}

}  // namespace atx::vol
