#pragma once

// atx::engine::book -- target tracking: trade a book toward a target book in risk units,
// paying for trading and borrow. There is no alpha vector: the only reason to trade is the
// risk-weighted gap to the target (platform v8 R-6; review S-8, research recipe 1A).
//
// ===========================================================================
//  The problem (one period, n names)
// ===========================================================================
//      minimize_w   (gamma/2) (w - a)' Sigma (w - a)                          tracking
//                 + sum_i [ s_i |w_i - w0_i| + eta_i |w_i - w0_i|^{3/2} ]     trading
//                 + sum_i b_i max(-w_i, 0)                                    borrow
//      subject to   lower_i <= w_i <= upper_i                                 holding box
//                   |w_i - w0_i| <= t_i                                       trade limit
//                   net.lo  <= 1'w    <= net.hi                               net limit
//                   beta.lo <= beta'w <= beta.hi                              beta limit
//
//  a = target, w0 = current book, s / eta / b = linear cost, 3/2-power impact and borrow
//  rate (per unit, per period: a caller amortizing a trade's cost over H periods divides s
//  and eta by H), t = trade limit (inf: none). The holding box carries a locate mask as
//  lower_i = min(w0_i, 0). A box emptied by the trade limit (w0 outside [lower, upper] by
//  more than t) becomes the point w0 -+ t toward the box.
//
//  Sigma = B F B' + diag(d) is never formed. B has three blocks of columns: column 0 an
//  intercept (every name loads 1; zero its row and column of F to drop it), columns
//  1..groups one-hot groups (a name loads 1 on at most one), then `styles` dense columns.
//  Positions held outside the problem enter only through their factor exposure: with e =
//  B'(w - a) over the problem's names and e_out = external_gap = B'(w - a) over the others,
//  the tracking variance is (e + e_out)' F (e + e_out) + sum_i d_i (w_i - a_i)^2 (the outside
//  names' own specific variance is a constant and is left out).
//
// ===========================================================================
//  Method: over-relaxed ADMM in the metric R = diag(rho), rho_i = 10 gamma d_i
// ===========================================================================
//  Split min f(x) + g(z) s.t. x = z with f = tracking + the indicator of the two limits and
//  g = trading + borrow + the box (separable). Per iteration, with scaled dual u:
//    x = argmin f(x) + 1/2 ||x - z + u||_R^2   the exact QP: (gamma Sigma + R) x = q by
//        Woodbury through the K x K capacitance I + W' B' Delta^{-1} B W (W W' = gamma F,
//        Delta = gamma d + rho), factored once per solve; the two limit rows by an exact
//        active-set choice among <= 9 cases (each a 0/1/2-row equality solve);
//    xh = alpha x + (1 - alpha) z               over-relaxation, alpha = 1.6;
//    z = prox_{g, R}(xh + u)                    closed form per name: soft threshold with the
//        3/2 power (a quadratic in sqrt|z - w0|), the borrow kink at 0, clamp to the box;
//    u = u + xh - z.
//  Convergence: f and g are closed proper convex, so ADMM with a fixed positive diagonal
//  metric and alpha in (0, 2) converges to a KKT point whenever one exists (Eckstein-
//  Bertsekas 1992; the constraints are polyhedral, so a nonempty feasible set suffices).
//  Stop when the primal residual ||x - z||_inf and the dual residual ||rho (z - z_prev) /
//  (gamma d)||_inf (the dual residual as a weight step in each name's own curvature) are both
//  <= tolerance, or at the iteration cap. Then a restoration puts the returned z (always
//  inside its box) exactly on both limits: bounded Newton passes on the names strictly
//  inside their box and away from their kinks (w0 and 0), moving each by at most the ADMM's
//  residual order.
//
//  F is used through a PSD root: its eigenvalues below 0 (a covariance with zeroed entries
//  can be slightly indefinite) are set to 0 and counted. Deterministic: order-fixed loops, a
//  fixed metric, no RNG, no clock, no threads; same inputs give the same bytes. Allocation:
//  O(n + K^2) workspace allocated once before the loop; the loop allocates nothing.

#include <limits>
#include <span>
#include <vector>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::engine::book {

// Solver constants (declared, never fitted).
inline constexpr atx::usize tracking_max_iterations = 2000; // iteration cap
inline constexpr atx::f64 tracking_tolerance = 1e-9;        // residuals, weight units
inline constexpr atx::f64 tracking_penalty_scale = 10.0;    // rho_i = 10 gamma d_i
inline constexpr atx::f64 tracking_relaxation = 1.6;        // alpha in (0, 2)
inline constexpr atx::usize tracking_restore_passes = 16;   // limit restoration passes
inline constexpr atx::f64 tracking_limit_tolerance = 1e-12; // net / beta after restoration

// The exposure layout of B: 1 + groups + styles columns (see the header).
struct TrackingFactors {
  atx::usize groups{}, styles{};
  [[nodiscard]] atx::usize count() const noexcept { return 1 + groups + styles; }
};
// A band lo <= a'w <= hi (lo == hi: an equality; infinite ends: open). Default: open.
struct TrackingLimit {
  atx::f64 lo{-std::numeric_limits<atx::f64>::infinity()};
  atx::f64 hi{std::numeric_limits<atx::f64>::infinity()};
};

struct TrackingProblem {
  TrackingFactors factors;
  atx::usize n{};
  std::vector<atx::u32> group;        // per name: group column in [1, groups], 0 = none
  std::vector<atx::f64> styles;       // n x styles, row-major
  std::vector<atx::f64> covariance;   // F: K x K row-major, symmetric (PSD up to rounding)
  std::vector<atx::f64> specific;     // d_i, finite > 0
  std::vector<atx::f64> external_gap; // K: B'(w - a) of the positions outside the problem
  std::vector<atx::f64> target;       // a
  std::vector<atx::f64> current;      // w0
  std::vector<atx::f64> linear_cost;  // s_i >= 0
  std::vector<atx::f64> impact_cost;  // eta_i >= 0
  std::vector<atx::f64> borrow_cost;  // b_i >= 0
  std::vector<atx::f64> trade_limit;  // t_i >= 0 (inf: none)
  std::vector<atx::f64> lower, upper; // holding box, lower <= upper (may be -inf / +inf)
  std::vector<atx::f64> beta;         // the beta limit's row
  atx::f64 gamma{1.0};                // > 0
  TrackingLimit net{}, beta_limit{};
};
// Sizes, finiteness, signs and orderings; InvalidArgument naming the first defect.
[[nodiscard]] atx::core::Status validate_tracking_problem(const TrackingProblem& p);

struct TrackingOptions {
  atx::usize max_iterations{tracking_max_iterations};
  atx::f64 tolerance{tracking_tolerance};
};

// The objective's parts at w (F as given). tracking_variance is per period.
struct TrackingTerms {
  atx::f64 tracking_variance{}, tracking{}, trade_cost{}, borrow{}, objective{};
};
[[nodiscard]] atx::core::Result<TrackingTerms> tracking_terms(const TrackingProblem& p,
                                                              std::span<const atx::f64> w);

struct TrackingSolution {
  std::vector<atx::f64> w;    // inside every box and trade limit
  std::vector<atx::f64> dual; // rho u (gradient units): a later solve's warm_dual
  atx::usize iterations{};
  bool converged{};           // both residuals <= tolerance before the cap
  bool limits_met{};          // net and beta within tracking_limit_tolerance of their bands
  atx::f64 primal_residual{}, dual_residual{}; // at the last iteration, weight units
  atx::f64 limit_violation{}; // after the restoration
  atx::usize restore_passes{};
  atx::usize clipped_eigenvalues{}; // eigenvalues of F below -1e-10 x its largest, set to 0
  atx::f64 net_multiplier{}, beta_multiplier{}; // the x-update's limit multipliers
  TrackingTerms terms;        // at w
  atx::f64 tracking_error{};  // sqrt(terms.tracking_variance), per period
  atx::usize no_trade{};      // w_i == w0_i
  atx::usize at_trade_limit{}; // 0 < t_i < inf and |w_i - w0_i| >= t_i (1 - 1e-9)
  atx::f64 trade_limit_share{}; // at_trade_limit / n (0 when n == 0)
};

// Solves the problem from the current book clipped to its box. warm_dual: empty (a cold
// start, u = 0) or n finite values from an earlier solution's `dual` mapped to this
// problem's names (u = warm_dual / rho). InvalidArgument on a malformed problem or warm
// dual; Internal if the capacitance is not positive definite (a non-finite covariance root).
[[nodiscard]] atx::core::Result<TrackingSolution>
solve_tracking(const TrackingProblem& p, const TrackingOptions& options = {},
               std::span<const atx::f64> warm_dual = {});

} // namespace atx::engine::book
