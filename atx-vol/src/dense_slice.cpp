#include "atx/vol/dense_slice.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/linalg/linalg.hpp"  // MatX, VecX
#include "atx/core/linalg/solve.hpp"   // solve, solve_spd
#include "atx/vol/black76.hpp"         // black76_price, black76_value_and_vega

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
[[nodiscard]] atx::core::Result<VecX> qp_active_set(const MatX& H, const VecX& q,
                                                    const MatX& G, const VecX& h,
                                                    VecX x, int max_iter) {
  const Eigen::Index n = H.rows();
  const Eigen::Index nc = G.rows();
  std::vector<char> in_w(static_cast<std::size_t>(nc), 0);  // working-set membership
  std::vector<Eigen::Index> wset;                            // active row indices

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
        return Ok(std::move(x));  // KKT-optimal
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
        const double gix = G.row(i).dot(x) - h(i);   // residual to the RHS
        const double ai = -gix / gip;                 // >= 0 since gix >= 0
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
  return Ok(std::move(x));  // hit the iteration cap: return best feasible point
}

// One (strike, target call price, weight) fit row, after PCP-folding puts to calls
// and merging duplicate strikes.
struct Node {
  double u{};   // strike
  double c{};   // weighted-mean target European call price
  double w{};   // total weight
  double s{};   // weighted-mean bid-ask band width (call-invariant; interval loss)
};

}  // namespace

double ConvexSliceFit::call_price(double K) const noexcept {
  if (u.empty() || C.size() != u.size() || !(K > 0.0) || !(F > 0.0) || !(df > 0.0)) {
    return kNaN;
  }
  if (K <= u.front()) {
    if (u.size() < 2) {
      return C.front();
    }
    // Left wing in PUT space. P(0)=0 and P=P0*(K/K0)^a is positive,
    // increasing and convex for a>=1. Match the fitted edge derivative when
    // feasible; the a>=1 projection is the no-arbitrage bound implied by a
    // convex put through the origin. C = df(F-K)+P is therefore bounded,
    // decreasing, convex and C1 at the splice.
    const double K0 = u.front();
    const double intrinsic0 = df * (F - K0);
    const double P0 = std::max(C.front() - intrinsic0, 0.0);
    if (!(P0 > 0.0)) {
      return std::max(df * (F - K), 0.0);
    }
    const double slope = (C[1] - C[0]) / (u[1] - u[0]);
    const double put_slope = std::clamp(slope + df, 0.0, df);
    const double exponent = std::max(1.0, put_slope * K0 / P0);
    const double put = P0 * std::pow(K / K0, exponent);
    return df * (F - K) + put;
  }
  if (K >= u.back()) {
    if (u.size() < 2 || !(C.back() > 0.0)) {
      return std::max(C.back(), 0.0);
    }
    // Right wing in CALL space. A power tail matches the edge slope while
    // remaining positive, decreasing and convex, and tends to zero instead of
    // flat-clamping a non-zero option price indefinitely.
    const std::size_t n = u.size();
    const double Kn = u.back();
    const double slope = (C[n - 1] - C[n - 2]) / (u[n - 1] - u[n - 2]);
    const double exponent = std::max(0.0, -slope * Kn / C.back());
    return C.back() * std::pow(K / Kn, -exponent);
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
  if (!std::isfinite(c)) {
    return kNaN;
  }
  // Price-space convex wings legitimately approach intrinsic/zero so closely
  // that a finite-vol root is lost to floating-point cancellation. Project the
  // price into Black's open interval by taking the maximum with a parallel
  // intrinsic+epsilon line (left) / epsilon floor (right). The maximum of these
  // convex functions remains convex, so numerical invertibility does not trade
  // away the very shape guarantee the dense fit provides.
  const double lower = df * std::max(F - K, 0.0);
  const double upper = df * F;
  const double gap = upper - lower;
  const double epsilon = std::min(1.0e-6 * std::max(1.0, upper), 0.25 * gap);
  const double safe_price =
      std::min(std::max(c, lower + epsilon), std::nextafter(upper, lower));
  if (!(safe_price > lower) || !(safe_price < upper)) {
    return kNaN;
  }
  // The generic IV helper stops at a price tolerance appropriate for market
  // quotes. A risk curve is differentiated twice, so that residual can become
  // visible as false negative density. Polish monotonically by bisection to a
  // near-machine price bracket; this is deterministic and remains reliable
  // when vega collapses in a wing.
  // Do not inherit the quote-inversion 0.5% IV floor here: in a deep wing even
  // a healthy convex price can imply a smaller numerical sigma. Flooring sigma
  // would move the served price off the convex curve and create false density.
  double lo = 1.0e-10;
  double hi = kIvMax;
  if (black76_price(F, K, T, hi, df, Side::Call) < safe_price) {
    return kNaN;
  }
  for (int iter = 0; iter < 64; ++iter) {
    const double mid = 0.5 * (lo + hi);
    const double model = black76_price(F, K, T, mid, df, Side::Call);
    if (model < safe_price) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return 0.5 * (lo + hi);
}

Result<ConvexSliceFit> fit_convex_slice(std::span<const FitObs> obs, double F,
                                        double T, double df,
                                        const ConvexFitOpts& opts,
                                        const std::function<double(double)>& w_prev,
                                        const ConvexFitContext& context) {
  if (!(F > 0.0) || !(T > 0.0) || !(df > 0.0)) {
    return Err(ErrorCode::InvalidArgument,
               "fit_convex_slice: F/T/df must be positive");
  }

  // Fold every quote to an equivalent European CALL price via put-call parity
  // (C = P + df·(F − K)); weight by vega²/spread² (the w-space fit weight). Merge
  // duplicate strikes by weighted mean.
  std::vector<Node> nodes;
  nodes.reserve(obs.size());
  for (const FitObs& o : obs) {
    if (!(o.K > 0.0) || !std::isfinite(o.mid)) {
      continue;
    }
    const double call = (o.side == Side::Call) ? o.mid : o.mid + df * (F - o.K);
    if (!std::isfinite(call) || call < 0.0) {
      continue;
    }
    const double spread = (o.spread > 1.0e-9) ? o.spread : 1.0e-9;
    const double vega = (o.vega > 0.0) ? o.vega : 1.0e-6;
    const double w = (vega * vega) / (spread * spread);
    // Raw (unclamped, call-invariant under put-call parity) band width for the
    // optional interval loss; the Mid data term does not read it.
    const double band = (o.spread > 0.0) ? o.spread : 0.0;
    nodes.push_back(Node{o.K, call, w, band});
  }
  if (nodes.size() < 3) {
    return Err(ErrorCode::InvalidArgument,
               "fit_convex_slice: fewer than 3 usable strikes");
  }
  std::sort(nodes.begin(), nodes.end(),
            [](const Node& a, const Node& b) { return a.u < b.u; });
  // Merge duplicate strikes (weighted mean of the call target).
  std::vector<Node> merged;
  merged.reserve(nodes.size());
  for (const Node& nd : nodes) {
    if (!merged.empty() && std::fabs(nd.u - merged.back().u) < 1.0e-9) {
      Node& m = merged.back();
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
    return Err(ErrorCode::InvalidArgument,
               "fit_convex_slice: fewer than 3 distinct strikes");
  }

  // Observations: one per distinct strike (Ko, target call co, weight wo).
  const Eigen::Index M = m;
  VecX Ko(M), co(M), wo(M);
  double wsum = 0.0;
  for (Eigen::Index i = 0; i < M; ++i) {
    const Node& nd = merged[static_cast<std::size_t>(i)];
    Ko(i) = nd.u;
    co(i) = nd.c;
    wo(i) = nd.w;
    wsum += nd.w;
  }
  const double w_mean = wsum / static_cast<double>(M);

  // Robust quote uncertainty in volatility units. build_observations_european
  // populates noise_sigma; direct/synthetic callers may omit it, in which case
  // spread/vega is the identical first-order estimate. A deterministic median
  // prevents one stale/wide wing from changing the whole slice's smoothness.
  std::vector<double> sigma_errors;
  sigma_errors.reserve(obs.size());
  for (const FitObs& o : obs) {
    double e = o.noise_sigma;
    if (!(e > 0.0) || !std::isfinite(e)) {
      e = (o.spread > 0.0 && o.vega > 1.0e-12) ? o.spread / o.vega : 0.0;
    }
    if (e > 0.0 && std::isfinite(e)) {
      sigma_errors.push_back(e);
    }
  }
  double noise_scale = 1.0;
  if (context.noise_aware_regularization && !sigma_errors.empty()) {
    const std::size_t mid = sigma_errors.size() / 2;
    std::nth_element(sigma_errors.begin(), sigma_errors.begin() + mid,
                     sigma_errors.end());
    const double median = sigma_errors[mid];
    // 0.01 is one absolute volatility point. This is a stable, explicit
    // discrepancy rule: larger measurement error permits proportionally more
    // curvature suppression, while very clean boards remain near-interpolating.
    noise_scale = std::clamp(median / 0.01, 0.25, 16.0);
  }

  // Node grid: at most node_cap QP variables. Small boards use the strikes
  // directly; wider boards use a uniform-in-log-moneyness grid spanning the strike
  // range. The design matrix B (linear-in-K interpolation, matching call_price)
  // maps node values to observation prices, so the fit near-interpolates the smile
  // with far fewer variables than strikes — the QP stays O(node_cap³) per step.
  const int cap = (opts.node_cap >= 4) ? opts.node_cap : 40;
  const Eigen::Index Nbase = std::min<Eigen::Index>(M, static_cast<Eigen::Index>(cap));
  VecX base(Nbase);
  if (Nbase == M) {
    base = Ko;
  } else {
    // Node grid CLUSTERED near the money (y = 0), where vega is largest and the
    // penny bid-ask band is tightest — so the fit resolves the ATM smile finely
    // (the metric-critical region) without spending nodes on the flat deep wings.
    // A |t|^warp warp of a uniform parameter t ∈ [−1,1] does the clustering.
    const double ylo = std::log(Ko(0) / F);
    const double yhi = std::log(Ko(M - 1) / F);
    constexpr double warp = 1.7;
    for (Eigen::Index j = 0; j < Nbase; ++j) {
      const double t = -1.0 + 2.0 * static_cast<double>(j) /
                                  static_cast<double>(Nbase - 1);
      const double wv = std::copysign(std::pow(std::fabs(t), warp), t);
      const double y = (wv < 0.0) ? (-wv) * ylo : wv * yhi;
      base(j) = F * std::exp(y);
    }
  }

  // Calendar/admission knots are exact QP nodes. They are additive to the
  // market candidate cap by design: node_cap is a latency hint, never authority
  // to drop a correctness constraint. Sorting and near-equality de-duplication
  // make the result deterministic regardless of violation discovery order.
  std::vector<double> node_strikes;
  node_strikes.reserve(static_cast<std::size_t>(Nbase) + context.required_k.size());
  for (Eigen::Index j = 0; j < Nbase; ++j) {
    node_strikes.push_back(base(j));
  }
  for (const double k : context.required_k) {
    if (std::isfinite(k) && std::fabs(k) <= 3.0) {
      node_strikes.push_back(F * std::exp(k));
    }
  }
  std::sort(node_strikes.begin(), node_strikes.end());
  node_strikes.erase(
      std::unique(node_strikes.begin(), node_strikes.end(), [](double a, double b) {
        return std::fabs(a - b) <= 1.0e-11 * std::max({1.0, std::fabs(a), std::fabs(b)});
      }),
      node_strikes.end());
  const Eigen::Index N = static_cast<Eigen::Index>(node_strikes.size());
  VecX un(N);
  for (Eigen::Index j = 0; j < N; ++j) {
    un(j) = node_strikes[static_cast<std::size_t>(j)];
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
  const MatX BtW = B.transpose() * wo.asDiagonal();  // N×M
  MatX H = 2.0 * (BtW * B);                           // N×N (symmetric)
  const double effective_lambda = opts.lambda * noise_scale;
  const double lam = effective_lambda * w_mean;
  for (Eigen::Index i = 0; i + 3 < N; ++i) {
    const double row[4] = {-1.0, 3.0, -3.0, 1.0};  // 3rd difference
    for (int a = 0; a < 4; ++a) {
      for (int b = 0; b < 4; ++b) {
        H(i + a, i + b) += 2.0 * lam * row[a] * row[b];
      }
    }
  }
  H = 0.5 * (H + H.transpose()).eval();  // enforce exact symmetry for solve_spd
  for (Eigen::Index i = 0; i < N; ++i) {
    H(i, i) += 1.0e-9 * w_mean + 1.0e-15;  // conditioning ridge
  }
  const VecX q = -2.0 * (BtW * co);  // N

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

  // Constraints G g >= h on the node values: discounted intrinsic/forward
  // price bounds (2N), monotone non-increasing (N−1), convexity via divided
  // second differences (N−2),
  // (optionally) the slope-below bound (N−1), and (optionally) the calendar
  // floor (<= N). `hrows` carries each row's RHS in parallel with `rows` — 0
  // for the homogeneous positivity/monotone/convexity rows, −df·Δ for the
  // slope-below rows, cfloor_j for the calendar-floor rows.
  std::vector<VecX> rows;
  std::vector<double> hrows;
  rows.reserve(static_cast<std::size_t>(6 * N));
  hrows.reserve(static_cast<std::size_t>(6 * N));
  // Keep fitted nodes numerically inside Black's open no-arbitrage interval.
  // A value exactly on intrinsic has a mathematically valid zero-vol limit but
  // cannot be inverted to a positive risk volatility, especially in deep ITM
  // wings. One hundredth of a cent on a $100 forward is still economically
  // negligible while remaining well above double/root-solver cancellation.
  const double price_epsilon = 1.0e-6 * std::max(1.0, df * F);
  for (Eigen::Index i = 0; i < N; ++i) {  // g_i >= discounted intrinsic
    VecX rrow = VecX::Zero(N);
    rrow(i) = 1.0;
    rows.push_back(rrow);
    const double intrinsic = df * std::max(F - un(i), 0.0);
    hrows.push_back(std::min(intrinsic + price_epsilon,
                             std::nextafter(df * F, 0.0)));
  }
  for (Eigen::Index i = 0; i < N; ++i) {  // g_i <= discounted forward
    VecX rrow = VecX::Zero(N);
    rrow(i) = -1.0;
    rows.push_back(rrow);
    hrows.push_back(-(df * F - price_epsilon));
  }
  for (Eigen::Index i = 0; i + 1 < N; ++i) {  // g_i - g_{i+1} >= 0
    VecX rrow = VecX::Zero(N);
    rrow(i) = 1.0;
    rrow(i + 1) = -1.0;
    rows.push_back(rrow);
    hrows.push_back(0.0);
  }
  for (Eigen::Index i = 1; i + 1 < N; ++i) {  // convexity (divided 2nd difference)
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
    for (Eigen::Index i = 0; i + 1 < N; ++i) {  // (g_{i+1} - g_i) >= -df*(u_{i+1}-u_i)
      VecX rrow = VecX::Zero(N);
      rrow(i + 1) = 1.0;
      rrow(i) = -1.0;
      rows.push_back(rrow);
      hrows.push_back(-df * (un(i + 1) - un(i)));
    }
  }
  if (N >= 2) {
    // Convex extension through the left origin in PUT space. At K0 a convex
    // put with P(0)=0 must have P'(K0) >= P(K0)/K0. Without this endpoint
    // inequality, projecting the power-wing exponent to a>=1 can make the wing
    // derivative exceed the first interior derivative, creating a density kink
    // even though every interior divided difference is non-negative.
    const double K0 = un(0);
    const double delta = un(1) - K0;
    VecX rrow = VecX::Zero(N);
    rrow(0) = -(1.0 / delta + 1.0 / K0);
    rrow(1) = 1.0 / delta;
    rows.push_back(rrow);
    hrows.push_back(-df * F / K0);
  }
  for (Eigen::Index j = 0; j < N; ++j) {  // calendar floor: g_j >= cfloor_j
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

  // Strictly feasible start: a high finite-vol Black curve satisfies both price
  // bounds, both slope bounds, monotonicity and convexity. A calendar floor is
  // lifted by a tiny margin without crossing the discounted-forward ceiling.
  VecX x0(N);
  for (Eigen::Index j = 0; j < N; ++j) {
    double v = black76_price(F, un(j), T, 2.0, df, Side::Call);
    if (has_floor[static_cast<std::size_t>(j)]) {
      const double upper = df * F;
      const double lifted = cfloor[static_cast<std::size_t>(j)] +
                            1.0e-9 * std::max(1.0, upper);
      v = std::max(v, std::min(lifted, std::nextafter(upper, 0.0)));
    }
    x0(j) = v;
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
    const double lam_iv = effective_lambda * w_mean;
    for (Eigen::Index i = 0; i + 3 < N; ++i) {
      const double rrow[4] = {-1.0, 3.0, -3.0, 1.0};  // 3rd difference
      for (int a = 0; a < 4; ++a) {
        for (int b = 0; b < 4; ++b) {
          Hz(i + a, i + b) += 2.0 * lam_iv * rrow[a] * rrow[b];
        }
      }
    }
    Hz.topLeftCorner(N, N) =
        (0.5 * (Hz.topLeftCorner(N, N) +
                Hz.topLeftCorner(N, N).transpose()))
            .eval();  // exact symmetry for solve_spd
    for (Eigen::Index i = 0; i < N; ++i) {
      Hz(i, i) += 1.0e-9 * w_mean + 1.0e-15;  // conditioning ridge (matches Mid)
    }
    for (Eigen::Index i = 0; i < M; ++i) {
      Hz(N + i, N + i) = 2.0 * wo(i);          // penalty w_i·(s⁺_i)²
      Hz(N + M + i, N + M + i) = 2.0 * wo(i);  // penalty w_i·(s⁻_i)²
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
    for (Eigen::Index i = 0; i < M; ++i) {  // −(Bg)_i + s⁺_i ≥ −c_ask_i
      Gz.block(rr, 0, 1, N) = -B.row(i);
      Gz(rr, N + i) = 1.0;
      hz(rr) = -cask(i);
      ++rr;
    }
    for (Eigen::Index i = 0; i < M; ++i) {  // (Bg)_i + s⁻_i ≥ c_bid_i
      Gz.block(rr, 0, 1, N) = B.row(i);
      Gz(rr, N + M + i) = 1.0;
      hz(rr) = cbid(i);
      ++rr;
    }
    for (Eigen::Index i = 0; i < M; ++i) {  // s⁺_i ≥ 0
      Gz(rr, N + i) = 1.0;
      ++rr;
    }
    for (Eigen::Index i = 0; i < M; ++i) {  // s⁻_i ≥ 0
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

    ATX_TRY(VecX z, qp_active_set(Hz, qz, Gz, hz, z0, opts.max_iter));

    // Recover node prices from the g sub-block; build the fit as the Mid path does.
    ConvexSliceFit fit;
    fit.T = T;
    fit.F = F;
    fit.df = df;
    fit.u.resize(static_cast<std::size_t>(N));
    fit.C.resize(static_cast<std::size_t>(N));
    for (Eigen::Index j = 0; j < N; ++j) {
      fit.u[static_cast<std::size_t>(j)] = un(j);
      fit.C[static_cast<std::size_t>(j)] = z(j);
    }
    const VecX pred = B * z.head(N);
    double wsse = 0.0, wtot = 0.0;
    for (Eigen::Index i = 0; i < M; ++i) {
      const double res = pred(i) - co(i);
      wsse += wo(i) * res * res;
      wtot += wo(i);
    }
    fit.rmse_price = (wtot > 0.0) ? std::sqrt(wsse / wtot) : 0.0;
    fit.n_obs = static_cast<std::size_t>(M);
    const VecX slack = Gz * z - hz;
    fit.n_active = static_cast<std::size_t>(
        (slack.array().abs() <= 1.0e-7).count());
    fit.effective_lambda = effective_lambda;
    fit.noise_scale = noise_scale;
    return Ok(std::move(fit));
  }

  ATX_TRY(VecX gn, qp_active_set(H, q, G, h, x0, opts.max_iter));

  ConvexSliceFit fit;
  fit.T = T;
  fit.F = F;
  fit.df = df;
  fit.u.resize(static_cast<std::size_t>(N));
  fit.C.resize(static_cast<std::size_t>(N));
  for (Eigen::Index j = 0; j < N; ++j) {
    fit.u[static_cast<std::size_t>(j)] = un(j);
    fit.C[static_cast<std::size_t>(j)] = gn(j);
  }
  const VecX pred = B * gn;
  double wsse = 0.0, wtot = 0.0;
  for (Eigen::Index i = 0; i < M; ++i) {
    const double rres = pred(i) - co(i);
    wsse += wo(i) * rres * rres;
    wtot += wo(i);
  }
  fit.rmse_price = (wtot > 0.0) ? std::sqrt(wsse / wtot) : 0.0;  // vega-wtd price RMSE
  fit.n_obs = static_cast<std::size_t>(M);
  const VecX slack = G * gn - h;
  fit.n_active = static_cast<std::size_t>(
      (slack.array().abs() <= 1.0e-7).count());
  fit.effective_lambda = effective_lambda;
  fit.noise_scale = noise_scale;
  return Ok(std::move(fit));
}

}  // namespace atx::vol
