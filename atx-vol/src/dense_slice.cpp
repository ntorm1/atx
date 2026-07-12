#include "atx/vol/dense_slice.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp" // MatX, VecX
#include "atx/core/linalg/solve.hpp"  // solve, solve_spd
#include "atx/vol/black76.hpp"        // black76_price, black76_value_and_vega
#include "atx/vol/implied_vol.hpp"    // implied_vol

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::linalg::MatX;
using atx::core::linalg::solve;
using atx::core::linalg::solve_spd;
using atx::core::linalg::VecX;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kQpCertificateTol = 1.0e-8;
constexpr double kQpActiveTol = 1.0e-9;
constexpr double kQpStartTol = 1.0e-12;

struct QpSolveResult {
  VecX x;
  bool converged{false};
  int iterations{0};
  std::size_t active_count{0};
  double stationarity{0.0};
  double primal_violation{0.0};
  double complementarity{0.0};
  double dual_violation{0.0};
};

[[nodiscard]] double constraint_scale(const MatX &G, const VecX &h, Eigen::Index row,
                                      const VecX &x) noexcept {
  double participating_terms = 0.0;
  for (Eigen::Index column = 0; column < G.cols(); ++column) {
    participating_terms += std::fabs(G(row, column) * x(column));
  }
  return 1.0 + std::fabs(h(row)) + participating_terms;
}

[[nodiscard]] double scaled_primal_violation(const MatX &G, const VecX &h, const VecX &x) noexcept {
  if (!G.allFinite() || !h.allFinite() || !x.allFinite() || G.rows() != h.size() ||
      G.cols() != x.size()) {
    return std::numeric_limits<double>::infinity();
  }
  double violation = 0.0;
  for (Eigen::Index i = 0; i < G.rows(); ++i) {
    const double slack = G.row(i).dot(x) - h(i);
    const double scale = constraint_scale(G, h, i, x);
    if (!std::isfinite(slack) || !std::isfinite(scale) || !(scale > 0.0)) {
      return std::numeric_limits<double>::infinity();
    }
    violation = std::max(violation, std::max(-slack, 0.0) / scale);
  }
  return violation;
}

[[nodiscard]] QpSolveResult qp_result(const MatX &H, const VecX &q, const MatX &G, const VecX &h,
                                      VecX x, const std::vector<Eigen::Index> &wset,
                                      const VecX &lambda, int iterations,
                                      bool algorithm_converged) {
  QpSolveResult result;
  result.x = std::move(x);
  result.iterations = iterations;

  const bool dimensions_ok = H.rows() == H.cols() && H.rows() == result.x.size() &&
                             q.size() == result.x.size() && G.cols() == result.x.size() &&
                             G.rows() == h.size() &&
                             lambda.size() == static_cast<Eigen::Index>(wset.size());
  if (!dimensions_ok || !H.allFinite() || !q.allFinite() || !G.allFinite() || !h.allFinite() ||
      !result.x.allFinite() || !lambda.allFinite()) {
    result.stationarity = std::numeric_limits<double>::infinity();
    result.primal_violation = std::numeric_limits<double>::infinity();
    result.complementarity = std::numeric_limits<double>::infinity();
    result.dual_violation = std::numeric_limits<double>::infinity();
    return result;
  }

  const VecX hx = H * result.x;
  VecX dual_force = VecX::Zero(result.x.size());
  for (Eigen::Index i = 0; i < static_cast<Eigen::Index>(wset.size()); ++i) {
    const Eigen::Index row = wset[static_cast<std::size_t>(i)];
    if (row < 0 || row >= G.rows()) {
      result.stationarity = std::numeric_limits<double>::infinity();
      result.primal_violation = std::numeric_limits<double>::infinity();
      result.complementarity = std::numeric_limits<double>::infinity();
      result.dual_violation = std::numeric_limits<double>::infinity();
      return result;
    }
    dual_force += lambda(i) * G.row(row).transpose();
    const double slack = G.row(row).dot(result.x) - h(row);
    const double scale = constraint_scale(G, h, row, result.x);
    const double complementarity_scale = 1.0 + std::fabs(lambda(i)) * scale;
    result.complementarity =
        std::max(result.complementarity, std::fabs(lambda(i) * slack) / complementarity_scale);
    result.dual_violation =
        std::max(result.dual_violation, std::max(-lambda(i), 0.0) / (1.0 + std::fabs(lambda(i))));
  }

  const VecX dual_residual = hx + q - dual_force;
  const double stationarity_scale =
      1.0 + std::max({hx.lpNorm<Eigen::Infinity>(), q.lpNorm<Eigen::Infinity>(),
                      dual_force.lpNorm<Eigen::Infinity>()});
  if (!hx.allFinite() || !dual_force.allFinite() || !dual_residual.allFinite() ||
      !std::isfinite(stationarity_scale) || !(stationarity_scale > 0.0) ||
      !std::isfinite(result.complementarity) || !std::isfinite(result.dual_violation)) {
    result.stationarity = std::numeric_limits<double>::infinity();
    result.primal_violation = std::numeric_limits<double>::infinity();
    result.complementarity = std::numeric_limits<double>::infinity();
    result.dual_violation = std::numeric_limits<double>::infinity();
    return result;
  }
  result.stationarity = dual_residual.lpNorm<Eigen::Infinity>() / stationarity_scale;
  result.primal_violation = scaled_primal_violation(G, h, result.x);

  if (std::isfinite(result.primal_violation) && result.primal_violation <= kQpCertificateTol) {
    for (Eigen::Index i = 0; i < G.rows(); ++i) {
      const double slack = G.row(i).dot(result.x) - h(i);
      const double scale = constraint_scale(G, h, i, result.x);
      if (std::isfinite(slack) && std::isfinite(scale) && scale > 0.0 &&
          std::fabs(slack) / scale <= kQpActiveTol) {
        ++result.active_count;
      }
    }
  }

  const bool finite_certificate =
      std::isfinite(result.stationarity) && std::isfinite(result.primal_violation) &&
      std::isfinite(result.complementarity) && std::isfinite(result.dual_violation);
  result.converged =
      algorithm_converged && finite_certificate && result.stationarity <= kQpCertificateTol &&
      result.primal_violation <= kQpCertificateTol && result.complementarity <= kQpCertificateTol &&
      result.dual_violation <= kQpCertificateTol;
  return result;
}

// ── Primal active-set QP ────────────────────────────────────────────────────
//
// Minimize ½ xᵀH x + qᵀx subject to G x >= h, H symmetric positive-definite,
// starting from a STRICTLY feasible x0 (G x0 > h). h=0 recovers the prior
// homogeneous form bit-for-bit. Nocedal & Wright Alg. 16.3: each iterate solves
// the working-set-equality KKT
//   [ H   −G_Wᵀ ] [ p ]   [ −g ]
//   [ G_W    0  ] [ λ ] = [  0 ]
// (g = H x + q). At p ≈ 0 the λ are the constraint multipliers; a negative one
// means dropping that constraint decreases the objective. Otherwise a ratio test
// caps the step at the nearest inactive constraint and adds it to the set.
[[nodiscard]] atx::core::Result<QpSolveResult>
qp_active_set(const MatX &H, const VecX &q, const MatX &G, const VecX &h, VecX x, int max_iter) {
  const bool dimensions_ok = H.rows() == H.cols() && H.rows() == x.size() && q.size() == x.size() &&
                             G.cols() == x.size() && G.rows() == h.size();
  if (!dimensions_ok || !H.allFinite() || !q.allFinite() || !G.allFinite() || !h.allFinite() ||
      !x.allFinite()) {
    return Err(ErrorCode::Internal, "qp_active_set: non-finite or inconsistent problem");
  }
  const double initial_violation = scaled_primal_violation(G, h, x);
  if (!std::isfinite(initial_violation) || initial_violation > kQpStartTol) {
    return Err(ErrorCode::Internal, "qp_active_set: infeasible initial point");
  }

  const Eigen::Index n = H.rows();
  const Eigen::Index nc = G.rows();
  std::vector<char> in_w(static_cast<std::size_t>(nc), 0); // working-set membership
  std::vector<Eigen::Index> wset;                          // active row indices

  const double kZero = 1.0e-11;
  for (int iter = 0; iter < max_iter; ++iter) {
    const VecX g = H * x + q;
    const auto nw = static_cast<Eigen::Index>(wset.size());

    // Solve the working-set-equality QP for the step p (+ multipliers λ).
    VecX p = VecX::Zero(n);
    VecX lambda = VecX::Zero(nw);
    if (nw == 0) {
      auto sp = solve_spd(H, VecX(-g));
      if (!sp) {
        return Err(ErrorCode::Internal, "qp_active_set: unconstrained solve failed");
      }
      p = *sp;
    } else {
      MatX Gw(nw, n);
      for (Eigen::Index i = 0; i < nw; ++i) {
        Gw.row(i) = G.row(wset[static_cast<std::size_t>(i)]);
      }
      const Eigen::Index d = n + nw;
      MatX K = MatX::Zero(d, d);
      K.topLeftCorner(n, n) = H;
      K.topRightCorner(n, nw) = -Gw.transpose();
      K.bottomLeftCorner(nw, n) = Gw;
      VecX rhs = VecX::Zero(d);
      rhs.head(n) = -g;
      auto sk = solve(K, rhs);
      if (!sk) {
        // Linearly dependent active rows: drop the most-recently added and retry.
        const Eigen::Index drop = wset.back();
        in_w[static_cast<std::size_t>(drop)] = 0;
        wset.pop_back();
        continue;
      }
      p = sk->head(n);
      lambda = sk->tail(nw);
    }

    if (p.lpNorm<Eigen::Infinity>() <= kZero * (1.0 + x.lpNorm<Eigen::Infinity>())) {
      // At the working-set minimizer. If every multiplier is non-negative this is
      // the global optimum; otherwise drop the most-negative constraint.
      Eigen::Index worst = -1;
      double worst_val = -1.0e-9;
      for (Eigen::Index i = 0; i < nw; ++i) {
        if (lambda(i) < worst_val) {
          worst_val = lambda(i);
          worst = i;
        }
      }
      if (worst < 0) {
        return Ok(qp_result(H, q, G, h, std::move(x), wset, lambda, iter + 1, true)); // KKT-optimal
      }
      const Eigen::Index drop = wset[static_cast<std::size_t>(worst)];
      in_w[static_cast<std::size_t>(drop)] = 0;
      wset.erase(wset.begin() + worst);
      continue;
    }

    // Ratio test: largest α ∈ (0, 1] keeping G(x + αp) >= h for inactive rows.
    double alpha = 1.0;
    Eigen::Index block = -1;
    for (Eigen::Index i = 0; i < nc; ++i) {
      if (in_w[static_cast<std::size_t>(i)]) {
        continue;
      }
      const double gip = G.row(i).dot(p);
      if (gip < -1.0e-14) {
        const double gix = G.row(i).dot(x) - h(i); // residual to the RHS
        const double ai = -gix / gip;              // >= 0 since gix >= 0
        if (ai < alpha) {
          alpha = ai;
          block = i;
        }
      }
    }
    x += alpha * p;
    if (block >= 0) {
      in_w[static_cast<std::size_t>(block)] = 1;
      wset.push_back(block);
    }
  }
  return Ok(qp_result(H, q, G, h, std::move(x), {}, VecX{}, max_iter, false));
}

// One (strike, target call price, weight) fit row, after PCP-folding puts to calls
// and merging duplicate strikes.
struct Node {
  double u{}; // strike
  double c{}; // weighted-mean target European call price
  double w{}; // total weight
  double s{}; // weighted-mean bid-ask band width (call-invariant; interval loss)
};

} // namespace

double ConvexSliceFit::call_price(double K) const noexcept {
  if (u.empty()) {
    return kNaN;
  }
  if (K <= u.front()) {
    return C.front();
  }
  if (K >= u.back()) {
    return C.back();
  }
  // Bracket K and linearly interpolate (convexity-preserving: the piecewise-linear
  // interpolant of convex samples is itself convex, hence butterfly-arb-free).
  const auto it = std::upper_bound(u.begin(), u.end(), K);
  const std::size_t hi = static_cast<std::size_t>(it - u.begin());
  const std::size_t lo = hi - 1;
  const double t = (K - u[lo]) / (u[hi] - u[lo]);
  return C[lo] + t * (C[hi] - C[lo]);
}

double ConvexSliceFit::iv(double k_log) const noexcept {
  if (!(F > 0.0) || !(T > 0.0)) {
    return kNaN;
  }
  const double K = F * std::exp(k_log);
  const double c = call_price(K);
  if (!std::isfinite(c) || !(c > 0.0)) {
    return kNaN;
  }
  const auto iv = implied_vol(c, F, K, T, df, Side::Call);
  return iv.has_value() ? *iv : kNaN;
}

Result<ConvexSliceFit> fit_convex_slice(std::span<const FitObs> obs, double F, double T, double df,
                                        const ConvexFitOpts &opts,
                                        const std::function<double(double)> &w_prev) {
  if (!std::isfinite(F) || !std::isfinite(T) || !std::isfinite(df) || !(F > 0.0) || !(T > 0.0) ||
      !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "fit_convex_slice: F/T/df must be finite and positive");
  }
  if (!std::isfinite(opts.lambda) || opts.lambda < 0.0 || opts.max_iter < 0 ||
      (opts.loss != CalibLossKind::Mid && opts.loss != CalibLossKind::Interval)) {
    return Err(ErrorCode::InvalidArgument, "fit_convex_slice: invalid convex-fit options");
  }

  // Fold every quote to an equivalent European CALL price via put-call parity
  // (C = P + df·(F − K)); weight by vega²/spread² (the w-space fit weight). Merge
  // duplicate strikes by weighted mean.
  std::vector<Node> nodes;
  nodes.reserve(obs.size());
  for (const FitObs &o : obs) {
    if (!std::isfinite(o.K) || !std::isfinite(o.mid) || !std::isfinite(o.spread) ||
        !std::isfinite(o.vega) || (o.side != Side::Call && o.side != Side::Put)) {
      return Err(ErrorCode::InvalidArgument, "fit_convex_slice: non-finite or invalid observation");
    }
    if (!(o.K > 0.0)) {
      continue;
    }
    const double call = (o.side == Side::Call) ? o.mid : o.mid + df * (F - o.K);
    if (!std::isfinite(call) || call < 0.0) {
      return Err(ErrorCode::InvalidArgument, "fit_convex_slice: invalid call-folded observation");
    }
    const double spread = (o.spread > 1.0e-9) ? o.spread : 1.0e-9;
    const double vega = (o.vega > 0.0) ? o.vega : 1.0e-6;
    const double w = (vega * vega) / (spread * spread);
    if (!std::isfinite(w) || !(w > 0.0)) {
      return Err(ErrorCode::InvalidArgument, "fit_convex_slice: invalid observation weight");
    }
    // Raw (unclamped, call-invariant under put-call parity) band width for the
    // optional interval loss; the Mid data term does not read it.
    const double band = (o.spread > 0.0) ? o.spread : 0.0;
    nodes.push_back(Node{o.K, call, w, band});
  }
  if (nodes.size() < 3) {
    return Err(ErrorCode::InvalidArgument, "fit_convex_slice: fewer than 3 usable strikes");
  }
  std::sort(nodes.begin(), nodes.end(), [](const Node &a, const Node &b) { return a.u < b.u; });
  // Merge duplicate strikes (weighted mean of the call target).
  std::vector<Node> merged;
  merged.reserve(nodes.size());
  for (const Node &nd : nodes) {
    if (!merged.empty() && std::fabs(nd.u - merged.back().u) < 1.0e-9) {
      Node &m = merged.back();
      const double wsum = m.w + nd.w;
      m.c = (m.c * m.w + nd.c * nd.w) / (wsum > 0.0 ? wsum : 1.0);
      m.s = (m.s * m.w + nd.s * nd.w) / (wsum > 0.0 ? wsum : 1.0);
      m.w = wsum;
    } else {
      merged.push_back(nd);
    }
  }
  const auto m = static_cast<Eigen::Index>(merged.size());
  if (m < 3) {
    return Err(ErrorCode::InvalidArgument, "fit_convex_slice: fewer than 3 distinct strikes");
  }

  // Observations: one per distinct strike (Ko, target call co, weight wo).
  const Eigen::Index M = m;
  VecX Ko(M), co(M), wo(M);
  double wsum = 0.0;
  for (Eigen::Index i = 0; i < M; ++i) {
    const Node &nd = merged[static_cast<std::size_t>(i)];
    Ko(i) = nd.u;
    co(i) = nd.c;
    wo(i) = nd.w;
    wsum += nd.w;
  }
  const double w_mean = wsum / static_cast<double>(M);
  if (!Ko.allFinite() || !co.allFinite() || !wo.allFinite() || !std::isfinite(w_mean) ||
      !(w_mean > 0.0)) {
    return Err(ErrorCode::InvalidArgument, "fit_convex_slice: non-finite merged observations");
  }

  // Node grid: at most node_cap QP variables. Small boards use the strikes
  // directly; wider boards use a uniform-in-log-moneyness grid spanning the strike
  // range. The design matrix B (linear-in-K interpolation, matching call_price)
  // maps node values to observation prices, so the fit near-interpolates the smile
  // with far fewer variables than strikes — the QP stays O(node_cap³) per step.
  const int cap = (opts.node_cap >= 4) ? opts.node_cap : 40;
  const Eigen::Index N = std::min<Eigen::Index>(M, static_cast<Eigen::Index>(cap));
  VecX un(N);
  if (N == M) {
    un = Ko;
  } else {
    // Node grid CLUSTERED near the money (y = 0), where vega is largest and the
    // penny bid-ask band is tightest — so the fit resolves the ATM smile finely
    // (the metric-critical region) without spending nodes on the flat deep wings.
    // A |t|^warp warp of a uniform parameter t ∈ [−1,1] does the clustering.
    const double ylo = std::log(Ko(0) / F);
    const double yhi = std::log(Ko(M - 1) / F);
    constexpr double warp = 1.7;
    for (Eigen::Index j = 0; j < N; ++j) {
      const double t = -1.0 + 2.0 * static_cast<double>(j) / static_cast<double>(N - 1);
      const double wv = std::copysign(std::pow(std::fabs(t), warp), t);
      const double y = (wv < 0.0) ? (-wv) * ylo : wv * yhi;
      un(j) = F * std::exp(y);
    }
  }

  MatX B = MatX::Zero(M, N);
  for (Eigen::Index i = 0; i < M; ++i) {
    const double K = Ko(i);
    if (K <= un(0)) {
      B(i, 0) = 1.0;
      continue;
    }
    if (K >= un(N - 1)) {
      B(i, N - 1) = 1.0;
      continue;
    }
    Eigen::Index j = 0;
    while (j + 1 < N && un(j + 1) < K) {
      ++j;
    }
    const double t = (K - un(j)) / (un(j + 1) - un(j));
    B(i, j) = 1.0 - t;
    B(i, j + 1) = t;
  }

  // Objective ½gᵀHg + qᵀg over NODE values g: data BᵀWB + third-difference
  // roughness (penalizes curvature CHANGES, so it never fights convexity).
  const MatX BtW = B.transpose() * wo.asDiagonal(); // N×M
  MatX H = 2.0 * (BtW * B);                         // N×N (symmetric)
  const double lam = opts.lambda * w_mean;
  for (Eigen::Index i = 0; i + 3 < N; ++i) {
    const double row[4] = {-1.0, 3.0, -3.0, 1.0}; // 3rd difference
    for (int a = 0; a < 4; ++a) {
      for (int b = 0; b < 4; ++b) {
        H(i + a, i + b) += 2.0 * lam * row[a] * row[b];
      }
    }
  }
  H = 0.5 * (H + H.transpose()).eval(); // enforce exact symmetry for solve_spd
  for (Eigen::Index i = 0; i < N; ++i) {
    H(i, i) += 1.0e-9 * w_mean + 1.0e-15; // conditioning ridge
  }
  const VecX q = -2.0 * (BtW * co); // N

  // Calendar floor: if w_prev(k) (the previous, earlier-T expiry's total
  // variance at log-moneyness k) is supplied, the fitted call price at each
  // node must be >= the Black-76 price implied by that previous total
  // variance — so this slice's total variance cannot dip below the prior
  // expiry's at the nodes (calendar no-arbitrage). Skipped entirely (no rows
  // added) when w_prev is empty, or per-node when w_prev(k_j) is non-finite
  // or <= 0.
  std::vector<double> cfloor(static_cast<std::size_t>(N), 0.0);
  std::vector<char> has_floor(static_cast<std::size_t>(N), 0);
  if (w_prev) {
    for (Eigen::Index j = 0; j < N; ++j) {
      const double k = std::log(un(j) / F);
      const double wp = w_prev(k);
      if (std::isfinite(wp) && wp > 0.0) {
        const double sig = std::sqrt(wp / T);
        const double c = black76_price(F, un(j), T, sig, df, Side::Call);
        if (std::isfinite(c) && c > 0.0) {
          cfloor[static_cast<std::size_t>(j)] = c;
          has_floor[static_cast<std::size_t>(j)] = 1;
        }
      }
    }
  }

  // Constraints G g >= h on the node values: positivity (N), monotone
  // non-increasing (N−1), convexity via divided second differences (N−2),
  // (optionally) the slope-below bound (N−1), and (optionally) the calendar
  // floor (<= N). `hrows` carries each row's RHS in parallel with `rows` — 0
  // for the homogeneous positivity/monotone/convexity rows, −df·Δ for the
  // slope-below rows, cfloor_j for the calendar-floor rows.
  std::vector<VecX> rows;
  std::vector<double> hrows;
  rows.reserve(static_cast<std::size_t>(4 * N));
  hrows.reserve(static_cast<std::size_t>(4 * N));
  for (Eigen::Index i = 0; i < N; ++i) { // g_i >= 0
    VecX rrow = VecX::Zero(N);
    rrow(i) = 1.0;
    rows.push_back(rrow);
    hrows.push_back(0.0);
  }
  for (Eigen::Index i = 0; i + 1 < N; ++i) { // g_i - g_{i+1} >= 0
    VecX rrow = VecX::Zero(N);
    rrow(i) = 1.0;
    rrow(i + 1) = -1.0;
    rows.push_back(rrow);
    hrows.push_back(0.0);
  }
  for (Eigen::Index i = 1; i + 1 < N; ++i) { // convexity (divided 2nd difference)
    const double a = 1.0 / (un(i) - un(i - 1));
    const double b = 1.0 / (un(i + 1) - un(i));
    VecX rrow = VecX::Zero(N);
    rrow(i - 1) = a;
    rrow(i) = -(a + b);
    rrow(i + 1) = b;
    rows.push_back(rrow);
    hrows.push_back(0.0);
  }
  // NOTE: the ∂C/∂K >= −df slope-below bound (opts.bound_slope_below) is a
  // NON-homogeneous inequality (row·g >= −df·Δ), now encoded directly via the
  // augmented Gg >= h form of qp_active_set (rows below, h != 0).
  if (opts.bound_slope_below) {
    for (Eigen::Index i = 0; i + 1 < N; ++i) { // (g_{i+1} - g_i) >= -df*(u_{i+1}-u_i)
      VecX rrow = VecX::Zero(N);
      rrow(i + 1) = 1.0;
      rrow(i) = -1.0;
      rows.push_back(rrow);
      hrows.push_back(-df * (un(i + 1) - un(i)));
    }
  }
  for (Eigen::Index j = 0; j < N; ++j) { // calendar floor: g_j >= cfloor_j
    if (has_floor[static_cast<std::size_t>(j)]) {
      VecX rrow = VecX::Zero(N);
      rrow(j) = 1.0;
      rows.push_back(rrow);
      hrows.push_back(cfloor[static_cast<std::size_t>(j)]);
    }
  }
  const auto nc = static_cast<Eigen::Index>(rows.size());
  MatX G(nc, N);
  VecX h(nc);
  for (Eigen::Index i = 0; i < nc; ++i) {
    G.row(i) = rows[static_cast<std::size_t>(i)].transpose();
    h(i) = hrows[static_cast<std::size_t>(i)];
  }

  // Strictly feasible start: a quadratic decreasing from cmax to a small floor —
  // strictly convex, decreasing, positive, so no constraint is active initially.
  double cmax = 0.0;
  for (Eigen::Index i = 0; i < M; ++i) {
    cmax = std::max(cmax, co(i));
  }
  const double span = un(N - 1) - un(0);
  const double floor = 1.0e-6 * (cmax + 1.0);
  VecX x0(N);
  for (Eigen::Index j = 0; j < N; ++j) {
    const double t = (span > 0.0) ? (un(N - 1) - un(j)) / span : 0.0;
    double v = floor + cmax * t * t;
    if (has_floor[static_cast<std::size_t>(j)]) {
      // max with the (convex, decreasing, positive) floor curve + strict margin.
      v = std::max(v, cfloor[static_cast<std::size_t>(j)] * (1.0 + 1.0e-6) + 1.0e-9);
    }
    x0(j) = v;
  }

  // The historical start is retained bit-for-bit whenever it is feasible. A
  // hostile price scale can make its quadratic slope steeper than -df when that
  // optional bound is enabled; in that case replace only the START (not the QP)
  // with a high, gently decreasing, strictly convex curve above every calendar
  // floor. The optimizer and its converged node values remain unchanged.
  double start_violation = scaled_primal_violation(G, h, x0);
  if (!std::isfinite(start_violation) || start_violation > kQpStartTol) {
    double level = cmax;
    for (Eigen::Index j = 0; j < N; ++j) {
      if (has_floor[static_cast<std::size_t>(j)]) {
        level = std::max(level, cfloor[static_cast<std::size_t>(j)]);
      }
    }
    const double shape_height = 0.25 * df * span;
    const double margin = 1.0e-9 * (1.0 + level + shape_height);
    if (!std::isfinite(level) || !std::isfinite(shape_height) || !std::isfinite(margin)) {
      return Err(ErrorCode::Internal, "fit_convex_slice: cannot construct finite feasible start");
    }
    for (Eigen::Index j = 0; j < N; ++j) {
      const double t = (span > 0.0) ? (un(N - 1) - un(j)) / span : 0.0;
      x0(j) = level + margin + shape_height * (0.5 * t + 0.5 * t * t);
    }
    start_violation = scaled_primal_violation(G, h, x0);
  }
  if (!std::isfinite(start_violation) || start_violation > kQpStartTol) {
    return Err(ErrorCode::Internal, "fit_convex_slice: failed to construct feasible start");
  }

  // ── Optional interval (band) loss ─────────────────────────────────────────
  // Instead of fitting each price to its MID, fit it into the bid-ask BAND
  // [c_bid, c_ask] with ZERO residual inside the band and a quadratic penalty
  // OUTSIDE — the exact interval loss, still a convex QP via slack variables.
  // The variable vector widens to z = [g (N nodes); s⁺ (M); s⁻ (M)] (size N+2M):
  //
  //   minimize  ½ Σ_i w_i (s⁺_i² + s⁻_i²) + λ Σ (Δ³g)²
  //   s.t.      (cone + slope-below + calendar-floor rows on g, reused verbatim)
  //             −(Bg)_i + s⁺_i ≥ −c_ask_i        (price − c_ask ≤ s⁺)
  //              (Bg)_i + s⁻_i ≥  c_bid_i         (c_bid − price ≤ s⁻)
  //             s⁺_i ≥ 0,  s⁻_i ≥ 0
  //
  // At the optimum s⁺_i = max(0, price−c_ask_i), s⁻_i = max(0, c_bid_i−price):
  // zero when the price is inside the band, its overshoot otherwise. The g-block
  // reuses the SAME `rows`/`hrows` and feasible start `x0` assembled above (each
  // g-block row embedded into the wider z layout by zero-padding the 2M slack
  // columns, RHS unchanged) — so interval loss composes with the A3 slope-below
  // and A4 calendar-floor constraints. The Mid branch below is untouched.
  if (opts.loss == CalibLossKind::Interval) {
    const Eigen::Index Nz = N + 2 * M;

    // Objective ½zᵀHz z (qz = 0): the g sub-block carries the SAME third-
    // difference roughness + conditioning ridge as the Mid path (NO data term —
    // the band slacks carry the data fit); each slack gets a 2·w_i diagonal so
    // its squared penalty is w_i·s².
    MatX Hz = MatX::Zero(Nz, Nz);
    const double lam_iv = opts.lambda * w_mean;
    for (Eigen::Index i = 0; i + 3 < N; ++i) {
      const double rrow[4] = {-1.0, 3.0, -3.0, 1.0}; // 3rd difference
      for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
          Hz(i + a, i + b) += 2.0 * lam_iv * rrow[a] * rrow[b];
        }
      }
    }
    Hz.topLeftCorner(N, N) = (0.5 * (Hz.topLeftCorner(N, N) + Hz.topLeftCorner(N, N).transpose()))
                                 .eval(); // exact symmetry for solve_spd
    for (Eigen::Index i = 0; i < N; ++i) {
      Hz(i, i) += 1.0e-9 * w_mean + 1.0e-15; // conditioning ridge (matches Mid)
    }
    for (Eigen::Index i = 0; i < M; ++i) {
      Hz(N + i, N + i) = 2.0 * wo(i);         // penalty w_i·(s⁺_i)²
      Hz(N + M + i, N + M + i) = 2.0 * wo(i); // penalty w_i·(s⁻_i)²
    }
    // Rescale the objective by 1/w_mean (a positive constant ⇒ the argmin is
    // unchanged). The data weights w_i can be enormous when spreads collapse, so
    // this brings Hz to O(1) — matching the O(1) constraint rows — which keeps
    // the augmented KKT matrix well balanced and the solve accurate. (The Mid
    // path is naturally balanced by its BᵀWB data term, so it needs no rescale.)
    Hz *= 1.0 / w_mean;
    const VecX qz = VecX::Zero(Nz);

    // Band edges per merged obs (call-folded): the half-spread is invariant under
    // put-call parity, so c_ask = co + spread/2, c_bid = max(0, co − spread/2).
    VecX cask(M), cbid(M);
    for (Eigen::Index i = 0; i < M; ++i) {
      const double half = 0.5 * merged[static_cast<std::size_t>(i)].s;
      cask(i) = co(i) + half;
      cbid(i) = std::max(0.0, co(i) - half);
    }

    // Constraints Gz z ≥ hz: the reused g-block rows (zero-padded slack columns),
    // then band-upper, band-lower, s⁺ ≥ 0, s⁻ ≥ 0.
    const auto ncg = static_cast<Eigen::Index>(rows.size());
    const Eigen::Index ncz = ncg + 4 * M;
    MatX Gz = MatX::Zero(ncz, Nz);
    VecX hz = VecX::Zero(ncz);
    for (Eigen::Index i = 0; i < ncg; ++i) {
      Gz.block(i, 0, 1, N) = rows[static_cast<std::size_t>(i)].transpose();
      hz(i) = hrows[static_cast<std::size_t>(i)];
    }
    Eigen::Index rr = ncg;
    for (Eigen::Index i = 0; i < M; ++i) { // −(Bg)_i + s⁺_i ≥ −c_ask_i
      Gz.block(rr, 0, 1, N) = -B.row(i);
      Gz(rr, N + i) = 1.0;
      hz(rr) = -cask(i);
      ++rr;
    }
    for (Eigen::Index i = 0; i < M; ++i) { // (Bg)_i + s⁻_i ≥ c_bid_i
      Gz.block(rr, 0, 1, N) = B.row(i);
      Gz(rr, N + M + i) = 1.0;
      hz(rr) = cbid(i);
      ++rr;
    }
    for (Eigen::Index i = 0; i < M; ++i) { // s⁺_i ≥ 0
      Gz(rr, N + i) = 1.0;
      ++rr;
    }
    for (Eigen::Index i = 0; i < M; ++i) { // s⁻_i ≥ 0
      Gz(rr, N + M + i) = 1.0;
      ++rr;
    }

    // Strictly feasible start: g = the Mid-path x0 (already lifted to the
    // calendar floor, so the g-block rows are already strict); each slack set
    // just past its band residual so every band + non-negativity row is strict.
    const VecX Bx0 = B * x0;
    VecX z0 = VecX::Zero(Nz);
    z0.head(N) = x0;
    constexpr double eps = 1.0e-6;
    for (Eigen::Index i = 0; i < M; ++i) {
      z0(N + i) = std::max(0.0, Bx0(i) - cask(i)) + eps;
      z0(N + M + i) = std::max(0.0, cbid(i) - Bx0(i)) + eps;
    }
    const double interval_start_violation = scaled_primal_violation(Gz, hz, z0);
    if (!std::isfinite(interval_start_violation) || interval_start_violation > kQpStartTol) {
      return Err(ErrorCode::Internal, "fit_convex_slice: infeasible interval-loss start");
    }

    ATX_TRY(QpSolveResult solved, qp_active_set(Hz, qz, Gz, hz, z0, opts.max_iter));
    if (!solved.converged) {
      return Err(ErrorCode::Internal, "fit_convex_slice: QP failed KKT certification");
    }

    // Recover node prices from the g sub-block; build the fit as the Mid path does.
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = F;
    fit.df = df;
    fit.u.resize(static_cast<std::size_t>(N));
    fit.C.resize(static_cast<std::size_t>(N));
    for (Eigen::Index j = 0; j < N; ++j) {
      fit.u[static_cast<std::size_t>(j)] = un(j);
      fit.C[static_cast<std::size_t>(j)] = solved.x(j);
    }
    const VecX pred = B * solved.x.head(N);
    double wsse = 0.0, wtot = 0.0;
    for (Eigen::Index i = 0; i < M; ++i) {
      const double res = pred(i) - co(i);
      wsse += wo(i) * res * res;
      wtot += wo(i);
    }
    fit.rmse_price = (wtot > 0.0) ? std::sqrt(wsse / wtot) : 0.0;
    if (!std::isfinite(fit.rmse_price)) {
      return Err(ErrorCode::Internal, "fit_convex_slice: non-finite fit diagnostics");
    }
    fit.n_obs = static_cast<std::size_t>(M);
    fit.n_active = solved.active_count;
    fit.qp_iterations = static_cast<std::size_t>(solved.iterations);
    fit.qp_stationarity = solved.stationarity;
    fit.qp_primal_violation = solved.primal_violation;
    fit.qp_complementarity = solved.complementarity;
    fit.qp_dual_violation = solved.dual_violation;
    return Ok(std::move(fit));
  }

  ATX_TRY(QpSolveResult solved, qp_active_set(H, q, G, h, x0, opts.max_iter));
  if (!solved.converged) {
    return Err(ErrorCode::Internal, "fit_convex_slice: QP failed KKT certification");
  }

  ConvexSliceFit fit;
  fit.T = T;
  fit.F = F;
  fit.df = df;
  fit.u.resize(static_cast<std::size_t>(N));
  fit.C.resize(static_cast<std::size_t>(N));
  for (Eigen::Index j = 0; j < N; ++j) {
    fit.u[static_cast<std::size_t>(j)] = un(j);
    fit.C[static_cast<std::size_t>(j)] = solved.x(j);
  }
  const VecX pred = B * solved.x;
  double wsse = 0.0, wtot = 0.0;
  for (Eigen::Index i = 0; i < M; ++i) {
    const double rres = pred(i) - co(i);
    wsse += wo(i) * rres * rres;
    wtot += wo(i);
  }
  fit.rmse_price = (wtot > 0.0) ? std::sqrt(wsse / wtot) : 0.0; // vega-wtd price RMSE
  if (!std::isfinite(fit.rmse_price)) {
    return Err(ErrorCode::Internal, "fit_convex_slice: non-finite fit diagnostics");
  }
  fit.n_obs = static_cast<std::size_t>(M);
  fit.n_active = solved.active_count;
  fit.qp_iterations = static_cast<std::size_t>(solved.iterations);
  fit.qp_stationarity = solved.stationarity;
  fit.qp_primal_violation = solved.primal_violation;
  fit.qp_complementarity = solved.complementarity;
  fit.qp_dual_violation = solved.dual_violation;
  return Ok(std::move(fit));
}

} // namespace atx::vol
