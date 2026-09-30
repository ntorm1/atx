// atx::engine::book -- target tracking. The problem, the method and its convergence
// argument are stated in the header; this file is the arithmetic.
#include "atx/engine/book/target_tracking.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string>
#include <utility>

#include "atx/core/linalg/decompose.hpp"
#include "atx/core/linalg/linalg.hpp"

namespace atx::engine::book {
namespace {
using atx::f64;
using atx::u32;
using atx::usize;
namespace co = atx::core;
namespace la = atx::core::linalg;

constexpr f64 inf = std::numeric_limits<f64>::infinity();
constexpr f64 clip_relative = 1e-10;     // an eigenvalue below -1e-10 x the largest is counted
constexpr f64 singular_relative = 1e-12; // a 2 x 2 limit Gram with det below this is singular
constexpr f64 limit_slack = 1e-9;        // |w - w0| >= t (1 - slack): at the trade limit

bool finite_positive(f64 x) noexcept { return std::isfinite(x) && x > 0.0; }
bool finite_nonnegative(f64 x) noexcept { return std::isfinite(x) && x >= 0.0; }
f64 clamp_to(f64 x, f64 lo, f64 hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }

// ---- B products over the layout (intercept, one-hot group, dense styles) ------------------
// y = B'v (K entries).
void factor_exposure(const TrackingProblem& p, const f64* v, f64* y) {
  const usize k = p.factors.count(), styles = p.factors.styles, first = 1 + p.factors.groups;
  std::fill(y, y + k, 0.0);
  f64 intercept = 0.0;
  for (usize i = 0; i < p.n; ++i) {
    const f64 vi = v[i];
    intercept += vi;
    if (p.group[i] != 0) y[p.group[i]] += vi;
    const f64* row = p.styles.data() + i * styles;
    for (usize c = 0; c < styles; ++c) y[first + c] += row[c] * vi;
  }
  y[0] += intercept;
}
// (B u)_i for one name.
f64 factor_load(const TrackingProblem& p, usize i, const f64* u) {
  const usize styles = p.factors.styles, first = 1 + p.factors.groups;
  f64 v = u[0];
  if (p.group[i] != 0) v += u[p.group[i]];
  const f64* row = p.styles.data() + i * styles;
  for (usize c = 0; c < styles; ++c) v += row[c] * u[first + c];
  return v;
}

// ---- K x K dense helpers (row-major) --------------------------------------------------
// In place: the lower triangle of a becomes L with L L' = a. False if a is not PD.
bool cholesky_in_place(std::vector<f64>& a, usize k) {
  for (usize j = 0; j < k; ++j) {
    f64 diag = a[j * k + j];
    for (usize m = 0; m < j; ++m) diag -= a[j * k + m] * a[j * k + m];
    if (!(diag > 0.0) || !std::isfinite(diag)) return false;
    const f64 root = std::sqrt(diag);
    a[j * k + j] = root;
    for (usize i = j + 1; i < k; ++i) {
      f64 v = a[i * k + j];
      for (usize m = 0; m < j; ++m) v -= a[i * k + m] * a[j * k + m];
      a[i * k + j] = v / root;
    }
  }
  return true;
}
// y <- (L L')^{-1} y with L the lower triangle of l.
void cholesky_solve(const std::vector<f64>& l, usize k, f64* y) {
  for (usize i = 0; i < k; ++i) {
    f64 v = y[i];
    for (usize m = 0; m < i; ++m) v -= l[i * k + m] * y[m];
    y[i] = v / l[i * k + i];
  }
  for (usize i = k; i-- > 0;) {
    f64 v = y[i];
    for (usize m = i + 1; m < k; ++m) v -= l[m * k + i] * y[m];
    y[i] = v / l[i * k + i];
  }
}

// ---- the per-name prox ---------------------------------------------------------------------
// argmin_u (rho/2)(u - p)^2 + s|u| + eta|u|^{3/2}: 0 in the dead zone |p| <= s/rho, else
// sign(p) t^2 with t > 0 the root of t^2 + (1.5 eta/rho) t - (|p| - s/rho) = 0, written
// without cancellation.
f64 shrink(f64 p, f64 rho, f64 s, f64 eta) {
  const f64 q = std::abs(p) - s / rho;
  if (!(q > 0.0)) return 0.0;
  const f64 b = 1.5 * eta / rho;
  const f64 t = 2.0 * q / (b + std::sqrt(b * b + 4.0 * q));
  return p > 0.0 ? t * t : -(t * t);
}
// argmin_z (rho/2)(z - v)^2 + s|z - w0| + eta|z - w0|^{3/2} + b max(-z, 0) over [lo, hi].
// On z >= 0 there is no borrow; on z <= 0 the borrow is -b z, which shifts v by b/rho. The
// function is strictly convex, so its minimizer is the long side's when that is positive,
// else the short side's when that is negative, else the kink 0; the box then clips it.
f64 name_prox(f64 v, f64 rho, f64 s, f64 eta, f64 b, f64 w0, f64 lo, f64 hi) {
  f64 z = w0 + shrink(v - w0, rho, s, eta);
  if (!(z > 0.0)) {
    const f64 short_side = w0 + shrink(v + b / rho - w0, rho, s, eta);
    z = short_side < 0.0 ? short_side : 0.0;
  }
  return clamp_to(z, lo, hi);
}

// ---- the two limit rows in the x-update -------------------------------------------------
// With r = A H^{-1} q and the Gram M = A H^{-1} A' (A = [1'; beta']), x = H^{-1}(q - A'mu)
// solves min 1/2 x'Hx - q'x s.t. lo <= Ax <= hi for the KKT case in which each row is free
// (mu = 0), at lo (mu <= 0) or at hi (mu >= 0). The problem is strictly convex, so exactly one
// case satisfies its conditions; rounding is absorbed by taking the case with the smallest
// violation (in row-value units), the first in a fixed order on ties.
struct Gram {
  f64 m11{}, m12{}, m22{};
};
std::array<f64, 2> limit_multipliers(const std::array<f64, 2>& r, const Gram& g,
                                     const std::array<TrackingLimit, 2>& lim) {
  const std::array<f64, 2> diag{g.m11, g.m22};
  std::array<f64, 2> best{0.0, 0.0};
  f64 best_violation = inf;
  for (u32 s0 = 0; s0 < 3; ++s0) {
    for (u32 s1 = 0; s1 < 3; ++s1) {
      const std::array<u32, 2> state{s0, s1};
      std::array<f64, 2> target{0.0, 0.0};
      bool valid = true;
      usize active = 0;
      for (usize j = 0; j < 2; ++j) {
        if (state[j] == 0) continue;
        const bool equality = lim[j].lo == lim[j].hi;
        target[j] = state[j] == 1 ? lim[j].lo : lim[j].hi;
        valid = valid && std::isfinite(target[j]) && !(equality && state[j] == 1) &&
                diag[j] > 0.0;
        ++active;
      }
      if (!valid) continue;
      std::array<f64, 2> mu{0.0, 0.0};
      const f64 e0 = r[0] - target[0], e1 = r[1] - target[1];
      if (active == 2) {
        const f64 det = g.m11 * g.m22 - g.m12 * g.m12;
        if (!(det > singular_relative * g.m11 * g.m22)) continue;
        mu = {(g.m22 * e0 - g.m12 * e1) / det, (g.m11 * e1 - g.m12 * e0) / det};
      } else if (state[0] != 0) {
        mu[0] = e0 / g.m11;
      } else if (state[1] != 0) {
        mu[1] = e1 / g.m22;
      }
      const std::array<f64, 2> value{r[0] - g.m11 * mu[0] - g.m12 * mu[1],
                                     r[1] - g.m12 * mu[0] - g.m22 * mu[1]};
      f64 violation = 0.0;
      for (usize j = 0; j < 2; ++j) {
        const bool equality = lim[j].lo == lim[j].hi;
        if (state[j] == 0)
          violation = std::max({violation, lim[j].lo - value[j], value[j] - lim[j].hi});
        else if (state[j] == 1)
          violation = std::max(violation, mu[j] * diag[j]);
        else if (!equality)
          violation = std::max(violation, -mu[j] * diag[j]);
      }
      if (violation < best_violation) { best_violation = violation; best = mu; }
    }
  }
  return best;
}

// ---- the solver's state ---------------------------------------------------------------------
struct Workspace {
  explicit Workspace(const TrackingProblem& p)
      : k(p.factors.count()), rho(p.n), delta(p.n), lo(p.n), hi(p.n), c0(p.n), h_net(p.n),
        h_beta(p.n), q(p.n), x(p.n), z(p.n), u(p.n), k1(k), k2(k), w_root(k * k),
        capacitance(k * k), cols(2 + p.factors.styles), vals(2 + p.factors.styles) {}
  usize k;
  std::vector<f64> rho, delta, lo, hi, c0, h_net, h_beta, q, x, z, u, k1, k2;
  std::vector<f64> w_root;      // W, K x K row-major: W W' = gamma F (PSD part)
  std::vector<f64> capacitance; // lower triangle: chol(I + W' B' Delta^{-1} B W)
  std::vector<usize> cols;      // one name's nonzero columns of B
  std::vector<f64> vals;
  usize clipped{};
};

// W with W W' = gamma F_+ (F symmetrized, eigenvalues below 0 set to 0).
co::Status factor_root(const TrackingProblem& p, Workspace& ws) {
  const usize k = ws.k;
  la::MatX f(static_cast<Eigen::Index>(k), static_cast<Eigen::Index>(k));
  for (usize r = 0; r < k; ++r)
    for (usize c = 0; c < k; ++c)
      f(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c)) =
          0.5 * (p.covariance[r * k + c] + p.covariance[c * k + r]);
  ATX_TRY(const auto eig, la::symmetric_eig(f));
  f64 largest = 0.0;
  for (Eigen::Index c = 0; c < eig.values.size(); ++c)
    largest = std::max(largest, static_cast<f64>(eig.values(c)));
  const f64 scale = std::sqrt(p.gamma);
  ws.clipped = 0;
  for (usize c = 0; c < k; ++c) {
    const f64 lambda = eig.values(static_cast<Eigen::Index>(c));
    if (lambda < -clip_relative * largest) ++ws.clipped;
    const f64 root = lambda > 0.0 ? std::sqrt(lambda) : 0.0;
    for (usize r = 0; r < k; ++r)
      ws.w_root[r * k + c] =
          scale * eig.vectors(static_cast<Eigen::Index>(r), static_cast<Eigen::Index>(c)) * root;
  }
  return co::Ok();
}

// The capacitance I + W' G W, G = B' Delta^{-1} B, factored in place.
co::Status factor_capacitance(const TrackingProblem& p, Workspace& ws) {
  const usize k = ws.k, styles = p.factors.styles, first = 1 + p.factors.groups;
  std::vector<f64> g(k * k, 0.0), gw(k * k, 0.0);
  for (usize i = 0; i < p.n; ++i) {
    usize used = 0;
    ws.cols[used] = 0; ws.vals[used] = 1.0; ++used;
    if (p.group[i] != 0) { ws.cols[used] = p.group[i]; ws.vals[used] = 1.0; ++used; }
    for (usize c = 0; c < styles; ++c) {
      ws.cols[used] = first + c; ws.vals[used] = p.styles[i * styles + c]; ++used;
    }
    const f64 weight = 1.0 / ws.delta[i];
    for (usize a = 0; a < used; ++a)
      for (usize b = 0; b < used; ++b)
        g[ws.cols[a] * k + ws.cols[b]] += ws.vals[a] * ws.vals[b] * weight;
  }
  for (usize r = 0; r < k; ++r) // gw = G W
    for (usize c = 0; c < k; ++c) {
      f64 v = 0.0;
      for (usize m = 0; m < k; ++m) v += g[r * k + m] * ws.w_root[m * k + c];
      gw[r * k + c] = v;
    }
  for (usize r = 0; r < k; ++r) // I + W' (G W)
    for (usize c = 0; c < k; ++c) {
      f64 v = r == c ? 1.0 : 0.0;
      for (usize m = 0; m < k; ++m) v += ws.w_root[m * k + r] * gw[m * k + c];
      ws.capacitance[r * k + c] = v;
    }
  if (!cholesky_in_place(ws.capacitance, k))
    return co::Err(co::ErrorCode::Internal,
                   "target tracking: the capacitance is not positive definite");
  return co::Ok();
}

// out = H^{-1} q with H = gamma B F_+ B' + diag(Delta), by Woodbury (out may not alias q).
void apply_inverse(const TrackingProblem& p, Workspace& ws, const f64* q, f64* out) {
  const usize k = ws.k;
  for (usize i = 0; i < p.n; ++i) out[i] = q[i] / ws.delta[i];
  factor_exposure(p, out, ws.k1.data()); // B' Delta^{-1} q
  for (usize r = 0; r < k; ++r) {
    f64 v = 0.0;
    for (usize c = 0; c < k; ++c) v += ws.w_root[c * k + r] * ws.k1[c];
    ws.k2[r] = v; // W' B' Delta^{-1} q
  }
  cholesky_solve(ws.capacitance, k, ws.k2.data());
  for (usize r = 0; r < k; ++r) {
    f64 v = 0.0;
    for (usize c = 0; c < k; ++c) v += ws.w_root[r * k + c] * ws.k2[c];
    ws.k1[r] = v; // W S^{-1} W' B' Delta^{-1} q
  }
  for (usize i = 0; i < p.n; ++i) out[i] -= factor_load(p, i, ws.k1.data()) / ws.delta[i];
}

f64 band_excess(f64 value, const TrackingLimit& lim) {
  return value < lim.lo ? value - lim.lo : (value > lim.hi ? value - lim.hi : 0.0);
}
// (1'w, beta'w) outside their bands (0 inside).
std::array<f64, 2> limit_excess(const TrackingProblem& p, const std::vector<f64>& w) {
  f64 net = 0.0, beta = 0.0;
  for (usize i = 0; i < p.n; ++i) { net += w[i]; beta += p.beta[i] * w[i]; }
  return {band_excess(net, p.net), band_excess(beta, p.beta_limit)};
}
// A name the restoration may move: strictly inside its box and off its kinks (w0, 0), where
// the objective is differentiable.
bool movable(const TrackingProblem& p, const Workspace& ws, const std::vector<f64>& w, usize i) {
  return w[i] > ws.lo[i] && w[i] < ws.hi[i] && w[i] != p.current[i] && w[i] != 0.0;
}

// Puts w (inside its box) on both limits: each pass solves min 1/2 sum rho_i delta_i^2 s.t.
// the two row excesses vanish over the movable names, then clips to the box. Returns
// (passes run, final violation).
std::pair<usize, f64> restore_limits(const TrackingProblem& p, const Workspace& ws,
                                     std::vector<f64>& w) {
  for (usize pass = 0; pass < tracking_restore_passes; ++pass) {
    const auto excess = limit_excess(p, w);
    const f64 e0 = excess[0], e1 = excess[1];
    const f64 violation = std::max(std::abs(e0), std::abs(e1));
    if (violation <= tracking_limit_tolerance) return {pass, violation};
    f64 g11 = 0.0, g12 = 0.0, g22 = 0.0;
    for (usize i = 0; i < p.n; ++i) {
      if (!movable(p, ws, w, i)) continue;
      const f64 m = 1.0 / ws.rho[i];
      g11 += m; g12 += p.beta[i] * m; g22 += p.beta[i] * p.beta[i] * m;
    }
    f64 mu0 = 0.0, mu1 = 0.0;
    const f64 det = g11 * g22 - g12 * g12;
    if (det > singular_relative * g11 * g22) {
      mu0 = (g22 * e0 - g12 * e1) / det; mu1 = (g11 * e1 - g12 * e0) / det;
    } else if (std::abs(e0) >= std::abs(e1) && g11 > 0.0) {
      mu0 = e0 / g11;
    } else if (g22 > 0.0) {
      mu1 = e1 / g22;
    } else {
      return {pass, violation}; // no name can move
    }
    for (usize i = 0; i < p.n; ++i) {
      if (!movable(p, ws, w, i)) continue;
      w[i] = clamp_to(w[i] - (mu0 + mu1 * p.beta[i]) / ws.rho[i], ws.lo[i], ws.hi[i]);
    }
  }
  const auto excess = limit_excess(p, w);
  return {tracking_restore_passes, std::max(std::abs(excess[0]), std::abs(excess[1]))};
}
} // namespace

co::Status validate_tracking_problem(const TrackingProblem& p) {
  const usize n = p.n, k = p.factors.count();
  const auto sized = [n](const std::vector<f64>& v) { return v.size() == n; };
  if (p.group.size() != n || p.styles.size() != n * p.factors.styles ||
      p.covariance.size() != k * k || p.external_gap.size() != k || !sized(p.specific) ||
      !sized(p.target) || !sized(p.current) || !sized(p.linear_cost) ||
      !sized(p.impact_cost) || !sized(p.borrow_cost) || !sized(p.trade_limit) ||
      !sized(p.lower) || !sized(p.upper) || !sized(p.beta))
    return co::Err(co::ErrorCode::InvalidArgument, "target tracking: problem geometry");
  const auto band_ok = [](const TrackingLimit& l) {
    return !std::isnan(l.lo) && !std::isnan(l.hi) && l.lo <= l.hi && l.lo < inf && l.hi > -inf;
  };
  if (!finite_positive(p.gamma) || !band_ok(p.net) || !band_ok(p.beta_limit))
    return co::Err(co::ErrorCode::InvalidArgument, "target tracking: gamma or a limit band");
  const auto all_finite = [](const std::vector<f64>& v) {
    return std::all_of(v.begin(), v.end(), [](f64 x) { return std::isfinite(x); });
  };
  if (!all_finite(p.covariance) || !all_finite(p.external_gap) || !all_finite(p.styles))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target tracking: covariance, external gap or styles not finite");
  for (usize i = 0; i < n; ++i) {
    const bool ok = p.group[i] <= p.factors.groups && finite_positive(p.specific[i]) &&
        std::isfinite(p.target[i]) && std::isfinite(p.current[i]) && std::isfinite(p.beta[i]) &&
        finite_nonnegative(p.linear_cost[i]) && finite_nonnegative(p.impact_cost[i]) &&
        finite_nonnegative(p.borrow_cost[i]) && !std::isnan(p.trade_limit[i]) &&
        p.trade_limit[i] >= 0.0 && !std::isnan(p.lower[i]) && !std::isnan(p.upper[i]) &&
        p.lower[i] <= p.upper[i] && p.lower[i] < inf && p.upper[i] > -inf;
    if (!ok)
      return co::Err(co::ErrorCode::InvalidArgument, "target tracking: name " + std::to_string(i));
  }
  return co::Ok();
}

co::Result<TrackingTerms> tracking_terms(const TrackingProblem& p, std::span<const f64> w) {
  ATX_TRY_VOID(validate_tracking_problem(p));
  if (w.size() != p.n)
    return co::Err(co::ErrorCode::InvalidArgument, "target tracking: book size");
  const usize k = p.factors.count();
  std::vector<f64> gap(p.n), e(k);
  for (usize i = 0; i < p.n; ++i) gap[i] = w[i] - p.target[i];
  factor_exposure(p, gap.data(), e.data());
  for (usize c = 0; c < k; ++c) e[c] += p.external_gap[c];
  TrackingTerms t;
  for (usize r = 0; r < k; ++r) {
    f64 row = 0.0;
    for (usize c = 0; c < k; ++c) row += p.covariance[r * k + c] * e[c];
    t.tracking_variance += e[r] * row;
  }
  for (usize i = 0; i < p.n; ++i) {
    const f64 move = std::abs(w[i] - p.current[i]);
    t.tracking_variance += p.specific[i] * gap[i] * gap[i];
    t.trade_cost += p.linear_cost[i] * move + p.impact_cost[i] * move * std::sqrt(move);
    t.borrow += w[i] < 0.0 ? -p.borrow_cost[i] * w[i] : 0.0;
  }
  t.tracking = 0.5 * p.gamma * t.tracking_variance;
  t.objective = t.tracking + t.trade_cost + t.borrow;
  return co::Ok(t);
}

co::Result<TrackingSolution> solve_tracking(const TrackingProblem& p, const TrackingOptions& o,
                                            std::span<const f64> warm_dual) {
  ATX_TRY_VOID(validate_tracking_problem(p));
  if (o.max_iterations == 0 || !finite_positive(o.tolerance) ||
      (!warm_dual.empty() && warm_dual.size() != p.n))
    return co::Err(co::ErrorCode::InvalidArgument,
                   "target tracking: iterations, tolerance or warm dual size");
  for (const f64 v : warm_dual)
    if (!std::isfinite(v))
      return co::Err(co::ErrorCode::InvalidArgument, "target tracking: warm dual");
  const usize n = p.n;
  Workspace ws(p);
  // Metric, boxes, the constant part of the x-update's right-hand side.
  for (usize i = 0; i < n; ++i) {
    const f64 curvature = p.gamma * p.specific[i];
    ws.rho[i] = tracking_penalty_scale * curvature;
    ws.delta[i] = curvature + ws.rho[i];
    const f64 w0 = p.current[i], t = p.trade_limit[i];
    f64 lo = std::max(p.lower[i], w0 - t), hi = std::min(p.upper[i], w0 + t);
    if (lo > hi) lo = hi = w0 - t > p.upper[i] ? w0 - t : w0 + t; // move by t toward the box
    ws.lo[i] = lo; ws.hi[i] = hi;
  }
  ATX_TRY_VOID(factor_root(p, ws));
  ATX_TRY_VOID(factor_capacitance(p, ws));
  const usize k = ws.k;
  { // c0 = B W W' (B'a - e_out) + gamma d a: the tracking gradient's constant part
    factor_exposure(p, p.target.data(), ws.k1.data());
    for (usize c = 0; c < k; ++c) ws.k1[c] -= p.external_gap[c];
    for (usize r = 0; r < k; ++r) {
      f64 v = 0.0;
      for (usize c = 0; c < k; ++c) v += ws.w_root[c * k + r] * ws.k1[c];
      ws.k2[r] = v;
    }
    for (usize r = 0; r < k; ++r) {
      f64 v = 0.0;
      for (usize c = 0; c < k; ++c) v += ws.w_root[r * k + c] * ws.k2[c];
      ws.k1[r] = v;
    }
    for (usize i = 0; i < n; ++i)
      ws.c0[i] = factor_load(p, i, ws.k1.data()) + p.gamma * p.specific[i] * p.target[i];
  }
  // The limit rows through H^{-1}: h = H^{-1} a and their Gram.
  std::fill(ws.q.begin(), ws.q.end(), 1.0);
  apply_inverse(p, ws, ws.q.data(), ws.h_net.data());
  apply_inverse(p, ws, p.beta.data(), ws.h_beta.data());
  Gram gram;
  for (usize i = 0; i < n; ++i) { gram.m11 += ws.h_net[i]; gram.m22 += p.beta[i] * ws.h_beta[i]; }
  { // symmetrized: 1'H^{-1}beta and beta'H^{-1}1 agree up to rounding
    f64 a = 0.0, b = 0.0;
    for (usize i = 0; i < n; ++i) { a += ws.h_beta[i]; b += p.beta[i] * ws.h_net[i]; }
    gram.m12 = 0.5 * (a + b);
  }
  const std::array<TrackingLimit, 2> limits{p.net, p.beta_limit};
  // Start: the current book in its box; the dual cold (0) or warm.
  for (usize i = 0; i < n; ++i) {
    ws.z[i] = clamp_to(p.current[i], ws.lo[i], ws.hi[i]);
    ws.u[i] = warm_dual.empty() ? 0.0 : warm_dual[i] / ws.rho[i];
  }
  TrackingSolution sol;
  std::array<f64, 2> mu{0.0, 0.0};
  f64 primal = inf, dual = inf;
  usize iterations = 0;
  while (iterations < o.max_iterations && n > 0) {
    ++iterations;
    for (usize i = 0; i < n; ++i) ws.q[i] = ws.c0[i] + ws.rho[i] * (ws.z[i] - ws.u[i]);
    apply_inverse(p, ws, ws.q.data(), ws.x.data());
    f64 r0 = 0.0, r1 = 0.0;
    for (usize i = 0; i < n; ++i) { r0 += ws.x[i]; r1 += p.beta[i] * ws.x[i]; }
    mu = limit_multipliers({r0, r1}, gram, limits);
    primal = 0.0;
    f64 step = 0.0;
    for (usize i = 0; i < n; ++i) {
      const f64 x = ws.x[i] - mu[0] * ws.h_net[i] - mu[1] * ws.h_beta[i];
      const f64 relaxed = tracking_relaxation * x + (1.0 - tracking_relaxation) * ws.z[i];
      const f64 z = name_prox(relaxed + ws.u[i], ws.rho[i], p.linear_cost[i], p.impact_cost[i],
                              p.borrow_cost[i], p.current[i], ws.lo[i], ws.hi[i]);
      ws.u[i] += relaxed - z;
      primal = std::max(primal, std::abs(x - z));
      step = std::max(step, std::abs(z - ws.z[i]));
      ws.z[i] = z;
    }
    dual = tracking_penalty_scale * step; // rho (z - z_prev) / (gamma d)
    if (primal <= o.tolerance && dual <= o.tolerance) { sol.converged = true; break; }
  }
  if (n == 0) { primal = 0.0; dual = 0.0; sol.converged = true; }
  sol.w = ws.z;
  const auto [passes, violation] = restore_limits(p, ws, sol.w);
  sol.restore_passes = passes;
  sol.limit_violation = violation;
  sol.limits_met = violation <= tracking_limit_tolerance;
  sol.iterations = iterations;
  sol.primal_residual = primal;
  sol.dual_residual = dual;
  sol.clipped_eigenvalues = ws.clipped;
  sol.net_multiplier = mu[0];
  sol.beta_multiplier = mu[1];
  sol.dual.resize(n);
  for (usize i = 0; i < n; ++i) sol.dual[i] = ws.rho[i] * ws.u[i];
  ATX_TRY(sol.terms, tracking_terms(p, sol.w));
  sol.tracking_error = std::sqrt(std::max(sol.terms.tracking_variance, 0.0));
  for (usize i = 0; i < n; ++i) {
    const f64 move = std::abs(sol.w[i] - p.current[i]), t = p.trade_limit[i];
    if (sol.w[i] == p.current[i]) ++sol.no_trade;
    if (t > 0.0 && t < inf && move >= t * (1.0 - limit_slack)) ++sol.at_trade_limit;
  }
  sol.trade_limit_share = n ? static_cast<f64>(sol.at_trade_limit) / static_cast<f64>(n) : 0.0;
  return co::Ok(std::move(sol));
}
} // namespace atx::engine::book
