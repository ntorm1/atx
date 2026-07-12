#pragma once

// Arbitrage-constrained DENSE per-slice fit — Phase 1 of the dense-surface design
// (docs/superpowers/specs/2026-07-05-arb-constrained-dense-surface-design.md).
//
// Where eSSVI (3 DoF) and the C2 wing/dense residual are smooth parametric curves
// that under-fit a penny-tight index board, this fits the smile DENSELY (one node
// per liquid strike) and NEAR-INTERPOLATINGLY, with butterfly no-arbitrage
// enforced as a HARD CONSTRAINT of the optimizer rather than repaired afterwards.
//
// ## Why call-price space
//
// Butterfly no-arbitrage is exactly convexity of the European call price in
// strike, ∂²C/∂K² >= 0 — a LINEAR inequality on the fitted node prices. Together
// with monotonicity (−df <= ∂C/∂K <= 0) and positivity, the per-slice fit is a
// convex quadratic program
//
//     minimize  Σ_i w_i (C(K_i) − c_i)²  +  λ · Σ (Δ³C)²
//     s.t.      C convex, non-increasing, non-negative in K
//
// solved to optimality by a primal active-set method (KKT solves via atx-core
// `solve`). c_i is the de-Americanized European call price at strike K_i and
// w_i ≈ vega²/spread². The feasible set IS the arbitrage-free cone, so the fitted
// smile is butterfly-arb-free by construction at ANY λ (λ→0 near-interpolates).
//
// Calendar no-arbitrage across expiries and full session integration are later
// phases; this header is the per-slice primitive, measured side-by-side with the
// shipped fit before any integration.
//
// ## Thread-safety
//
// `fit_convex_slice` is a pure function of its inputs. `ConvexSliceFit` is an
// immutable value (sorted node prices); concurrent `iv`/`call_price` reads are
// safe.

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "atx/vol/calib.hpp" // FitObs
#include "atx/vol/types.hpp" // Result, Side

namespace atx::vol {

// A fitted arbitrage-free convex call-price smile for one expiry. Holds the node
// strikes (ascending) and the fitted European call prices at them; queries
// interpolate the convex node prices and invert Black-76 for the implied vol.
struct ConvexSliceFit {
  double T{0.0};                   // year-fraction to expiry
  double F{0.0};                   // forward
  double df{0.0};                  // discount factor exp(-rT)
  std::vector<double> u;           // node strikes, strictly ascending
  std::vector<double> C;           // fitted European call price at each node (convex)
  double rmse_price{0.0};          // vega/spread-weighted price RMSE in half-spread units
  std::size_t n_obs{0};            // observations fit
  std::size_t n_active{0};         // constraints active at the optimum (diagnostic)
  std::size_t qp_iterations{0};    // active-set iterations used to certify convergence
  double qp_stationarity{0.0};     // scaled infinity norm of the KKT dual residual
  double qp_primal_violation{0.0}; // maximum scaled constraint violation
  double qp_complementarity{0.0};  // maximum scaled complementarity residual
  double qp_dual_violation{0.0};   // maximum scaled negative active multiplier

  // European call price at strike K by convexity-preserving interpolation of the
  // node prices; flat-extrapolated (clamped) outside [u.front(), u.back()].
  [[nodiscard]] double call_price(double K) const noexcept;

  // Black-76 implied vol at log-moneyness k = log(K / F). Inverts call_price(K).
  // NaN if K/T non-positive or the price is outside the no-arb (intrinsic, F·df)
  // band where an inversion does not exist.
  [[nodiscard]] double iv(double k_log) const noexcept;
};

// Options for the per-slice convex fit.
struct ConvexFitOpts {
  // Roughness penalty on the third difference of node prices (smooths curvature
  // CHANGES, so it does not fight convexity). Small => near-interpolation. Scaled
  // internally by the data-weight trace, so this is a dimensionless strength.
  double lambda{1.0e-3};
  // Enforce the call-slope lower bound ∂C/∂K >= −df (in addition to <= 0). Off by
  // default: the monotone (<= 0) + convex constraints already bound the smile, and
  // the extra bound occasionally over-constrains a sparse deep-ITM node.
  bool bound_slope_below{false};
  // Cap on the number of spline NODES (QP variables). A board with more distinct
  // strikes is fit on a uniform-in-log-moneyness node grid via a design matrix, so
  // the QP stays small (O(node_cap³) per active-set step) regardless of board
  // width — and per-strike penny noise is not over-fit. ~40 captures an index
  // smile densely; raise for extreme boards, lower for latency.
  int node_cap{40};
  // Max active-set iterations (safety cap). Zero permits no solve step and the
  // fit reports Internal rather than returning the feasible initial point.
  int max_iter{200};
  // Data-fit loss. Mid (default) fits each price to its symmetric MIDPOINT (a
  // point target). Interval fits it into the bid-ask BAND [bid, ask] with ZERO
  // residual inside the band and a quadratic penalty outside — the exact
  // interval loss, still a convex QP (solved via slack variables). Mid keeps
  // production fits byte-identical; the Interval branch composes with the
  // slope-below (`bound_slope_below`) and calendar-floor (`w_prev`) constraints.
  CalibLossKind loss{CalibLossKind::Mid};
};

// Capacity guard for the Interval (band) loss. That path widens the QP variable
// vector to N + 2M (N spline nodes bounded by node_cap, plus M+M band slacks)
// and materializes DENSE (N+2M)x(N+2M) system matrices — O(M^2) in the
// distinct-strike count M, an unbounded memory cliff for a pathologically wide
// board. fit_convex_slice rejects a slack system larger than this many rows with
// InvalidArgument BEFORE allocating, so the cliff surfaces as a loud, bounded
// error rather than an allocation death. Interval is opt-in (loss defaults to
// Mid); 2048 rows comfortably covers any real board a node_cap-bounded fit sees.
inline constexpr int kMaxIntervalSlackRows = 2048;

// Fit an arbitrage-free convex call-price smile to one expiry's filtered
// observation set. `obs` are the `build_observations` survivors for the slice
// (each carries its strike K, mid price, spread, side, and Black-76 vega at the
// market IV); `F`/`T`/`df` are the slice's forward / year-fraction / discount.
//
// Both call and put quotes are used: each is converted to the EQUIVALENT European
// CALL price via put-call parity (C = P + df·(F − K)), so the whole strike range
// contributes to one convex call curve. Duplicate strikes are merged
// (weight-combined). The convex QP is then solved to optimality.
//
// `w_prev`, if set, is the PREVIOUS (earlier-T) expiry's total-variance-by-
// log-moneyness callback w_prev(k). When present, the fit adds a per-node
// CALENDAR FLOOR: g_j >= black76_call(F, u_j, T, sqrt(w_prev(k_j)/T), df), for
// every node where w_prev(k_j) is finite and positive — so this slice's total
// variance cannot dip below the previous expiry's at the nodes (calendar
// no-arbitrage). Left empty (the default), the floor is skipped entirely and
// behavior is bit-identical to the unconstrained fit.
//
// @return InvalidArgument if F/T/df, consumed observations, or options are
//         non-finite/invalid, or fewer than 3 distinct strikes survive; Internal
//         if the QP solver cannot produce a finite KKT-certified solution;
//         otherwise the fitted convex smile.
[[nodiscard]] Result<ConvexSliceFit>
fit_convex_slice(std::span<const FitObs> obs, double F, double T, double df,
                 const ConvexFitOpts &opts = {}, const std::function<double(double)> &w_prev = {});

} // namespace atx::vol
