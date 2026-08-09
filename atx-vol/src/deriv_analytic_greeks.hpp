#pragma once

// deriv_analytic_greeks.hpp — Task P-4 (GK-P): closed-form variance-swap
// strip greeks. INTERNAL to atx-vol's derivatives.cpp -- lives in src/, not
// include/atx/vol/, carries no stability promise, and must not be included
// from any public header. Backs the public opt-in knob
// `DerivGreekMethod::AnalyticStrip` (derivatives.hpp); derivatives.cpp's
// `deriv_greeks` is the only production caller.
//
// ─────────────────────────────────────────────────────────────────────────
// WHY A CLOSED FORM EXISTS AT ALL (GK-P finding)
// ─────────────────────────────────────────────────────────────────────────
// K_var(T), the uncapped variance swap's fair strike, is the model-free strip
//
//   K_var = (2/T) * integral[ OTM(K) / (df*K^2) ] dK
//         = (2/T) * integral[ OTM(K) / (df*K)   ] dx     (K = F*e^x)
//
// quadratured by composite Simpson over a FIXED grid {x_i} with FIXED
// weights w_i on a (per-panel) spacing dx -- derivatives.cpp's
// `accumulate_strip` / `node_x`, C-3's kink-aligned panel split
// (`strip::plan_strip_split`, detail/strip_grid.hpp). That makes K_var a
// LINEAR functional of the N Black-76 prices {P_i} at those nodes, with
// coefficients c_i = w_i * dx_panel / (3 * df * K_i) that are fixed AT THE
// CENTER (u = 0) but NOT along the spot-bump path below -- K_i(u) moves
// with u, and so does c_i's own 1/K_i factor:
//
//   K_var = (2/T) * sum_i c_i * P_i(F, K_i, T, sigma_i)     (at u = 0 only)
//
// The quadrature weights {c_i} are exactly the "adjoint" the GK-P finding
// names: differentiating a LINEAR functional of N Black-76 prices costs the
// SAME N Black-76 *Greeks* (vega_i, volga_i below), not N repricings of the
// whole N-node strip integral -- which is what re-running
// `var_swap_fair_strike` under a finite difference (the pre-P-4 path) pays,
// once per bump, ~13-16 times per `deriv_greeks` call. That asymmetry is the
// entire performance argument; everything below is the calculus that turns
// "differentiate the sum" into formulas over the surface's own smile
// derivatives sigma'(k) / sigma''(k), which this file samples via a handful
// of extra batched read vectors rather than repricing anything. (The DELTA/
// GAMMA/VANNA sections below differentiate along u, so they work with
// q_i = P_i/K_i directly rather than c_i*P_i -- see each section for why.)
//
// Scope: `DerivKind::VarSwap` ONLY, uncapped, any age, AND
// `DerivConfig::discrete_correction_mode == DerivDiscreteCorrection::None`.
// The future leg IS this strip (`k_var_future_dec` in `price_var_swap`); the
// accrued leg is a contract-fixed constant no bump touches. `VolSwap`
// (Carr-Lee / the lognormal RV model) and both capped kinds price through a
// genuinely NONLINEAR model layer on top of the strip (a straddle formula, a
// displaced-lognormal expectation, a split-domain quadrature) -- none of
// those admit the same "differentiate the linear functional" shortcut, so
// they stay FD. The `Diffusion1OverN` discrete-monitoring correction adds
// `(T/n_rem)*((r-q) - K_var/2)^2` to `k_var_future_dec` BEFORE it becomes the
// quantity PV is linear in (`price_var_swap`) -- QUADRATIC in the very K_var
// this file differentiates, so every sensitivity below would need an extra
// `(1 - (T/n_rem)*mu)` chain-rule factor (plus a volga/vanna cross term) this
// file does not compute; a VarSwap priced with that correction ON also stays
// FD (Review fix round 1, C-1 -- the raw strip's own closed form silently
// disagreed with the corrected PV by orders of the parity gate).
// `derivatives.cpp` enforces both halves of the scope check (kind ==
// VarSwap AND discrete_correction_mode == None); this file assumes them,
// not re-derives them.
//
// ─────────────────────────────────────────────────────────────────────────
// NOTATION
// ─────────────────────────────────────────────────────────────────────────
// F, S      forward and spot at the contract's own T (curves.forward /
//           curves.spot); df the discount factor at T.
// x_i       the strip's pinned log-forward-moneyness grid (this file reuses
//           the CENTER quote's own resolved k_lo/k_hi/n_nodes/wing_band --
//           see `analytic_strip_greeks`'s parameters -- so it walks the
//           IDENTICAL panels/nodes `var_swap_fair_strike` priced).
// K_i       F * exp(x_i) -- the UNCLAMPED node position; matches
//           `price_node`'s own K exactly (only the SURFACE READ clamps to
//           the wing-trust band, never the strike itself -- see "wing clamp"
//           below).
// sigma_i   the surface's implied vol at (the wing-clamped) x_i, T.
// P_i       black76_price(F, K_i, T, sigma_i, df, side_i) -- side_i = Put
//           for x_i < 0, Call otherwise (put-call parity kink at x = 0,
//           C-3's own panel boundary).
// vega_i    d P_i / d sigma_i = F*df*phi(d1_i)*sqrt(T)   (side-independent).
// volga_i   d vega_i / d sigma_i = vega_i * d1_i * d2_i / sigma_i (standard
//           Black-Scholes/76 identity -- derived below).
// sigma'(x_i), sigma''(x_i)  the surface's own first/second derivative in
//           log-forward-moneyness at x_i, read via four extra batched
//           surface queries at x_i +/- delta and x_i +/- 2*delta -- see
//           "smile derivatives via FD" below. NOT a repricing: these
//           differentiate the SMILE, a deterministic function of the
//           ALREADY-FITTED surface, not the option payoff.
//
// The spot bump this file differentiates against is the SAME sticky-strike
// convention `deriv_greeks`' finite-difference stencils use (see that
// function's own header doc in derivatives.hpp): S and every ForwardPoint::F
// scale together, so d(ln F) = d(ln S) along the bump path, and the surface
// is always re-read at ln(K_i / F_orig) = x_i + ln(F/F_orig) -- i.e. the vol
// assigned to whichever absolute strike a grid index lands on is drawn from
// the SPOT-INDEPENDENT (sticky) map. Writing u = ln(F / F_center), every
// derivative below is first taken in u (where the algebra is clean) and
// converted to a spot derivative at the end via d/dS = (1/S) d/du (chain
// rule, S(u) = S_center * e^u along the same path).
//
// ─────────────────────────────────────────────────────────────────────────
// VEGA (parallel vol shift v, sigma_i -> sigma_i + v for every node)
// ─────────────────────────────────────────────────────────────────────────
// K_i is untouched by v; only sigma_i moves, at rate 1. Straight chain rule:
//
//   d P_i / dv = vega_i     =>     dK_var/dv = (2/T) * sum_i c_i * vega_i
//
// ─────────────────────────────────────────────────────────────────────────
// DELTA (spot bump, sticky-strike): B76 HOMOGENEITY
// ─────────────────────────────────────────────────────────────────────────
// Along the bump path, F(u) = F*e^u and (because the grid {x_i} is pinned)
// K_i(u) = F(u)*e^{x_i} = K_i(0)*e^u -- F and K_i scale by the IDENTICAL
// factor as u moves, so their ratio (and hence d1_i, d2_i at FIXED sigma)
// never changes with u; only sigma_i(u) = sigma_center(x_i) + [sticky-strike
// re-read] moves, at rate sigma_i'(u)|_{u=0} = sigma'(x_i).
//
// Black-76's undiscounted price is HOMOGENEOUS OF DEGREE 1 in (F, K) at
// fixed (sigma, T) -- Euler's theorem gives F*dP/dF + K*dP/dK = P. Since
// dF/du = F(u) and dK_i/du = K_i(u) (both scale at rate 1 in u), the total
// derivative collapses to
//
//   dP_i/du = [F*dP_i/dF + K_i*dP_i/dK] + vega_i*sigma_i'(u)
//           = P_i(u) + vega_i(u)*sigma_i'(u)                            (*)
//
// an identity that holds for ALL u, not just u = 0 -- it is reused below for
// gamma and vanna. Now differentiate K_i's contribution to K_var,
// q_i(u) = P_i(u)/K_i(u), using dK_i/du = K_i(u):
//
//   q_i'(u) = [P_i'(u)*K_i(u) - P_i(u)*K_i(u)] / K_i(u)^2
//           = [P_i'(u) - P_i(u)] / K_i(u)
//
// Substituting (*) at u = 0, the P_i(0) terms cancel EXACTLY, leaving only
// the smile-slope term -- the "B76 homogeneity => spot acts only through the
// smile re-read" the brief names:
//
//   q_i'(0) = vega_i * sigma'(x_i) / K_i(0)
//
// `c_i*K_i(0)` cancels exactly ONE of c_i's two `1/K_i` factors (c_i =
// w_i*dx/(3*df*K_i(0))), leaving the second one carried by q_i'(0) itself --
// Review fix round 1, I-1: this line previously dropped that surviving
// `1/K_i`, which the code (`inv_dfk = 1/(df*K)`, applied once per node) never
// did:
//
//   dK_var/du = (2/T) * sum_i c_i * K_i * q_i'(0)
//             = (2/T) * sum_i [w_i*dx/3 / (df*K_i)] * vega_i * sigma'(x_i)
//
// delta = (1/S) * dK_var/du (chain rule above).
//
// ─────────────────────────────────────────────────────────────────────────
// VOLGA: the standard Black-Scholes/76 identity
// ─────────────────────────────────────────────────────────────────────────
// d1 = [ln(F/K) + sigma^2 T / 2] / (sigma*sqrt(T)), d2 = d1 - sigma*sqrt(T).
// Differentiating d1 = ln(F/K)/(sigma*sqrt(T)) + sigma*sqrt(T)/2 in sigma
// gives d(d1)/dsigma = -ln(F/K)/(sigma^2*sqrt(T)) + sqrt(T)/2, which is
// EXACTLY -d2/sigma (expand d2 the same way and compare term by term). Then
//
//   d(vega)/dsigma = d[F*df*phi(d1)*sqrt(T)]/dsigma
//                  = -d1 * vega * d(d1)/dsigma       (phi'(x) = -x*phi(x))
//                  = -d1 * vega * (-d2/sigma) = vega * d1 * d2 / sigma
//
// so volga_i = vega_i * d1_i * d2_i / sigma_i, and (K_i, sigma_i untouched
// by a parallel-only second differentiation of vega's own formula):
//
//   d^2 K_var / dv^2 = (2/T) * sum_i c_i * volga_i
//
// ─────────────────────────────────────────────────────────────────────────
// GAMMA: differentiate (*) again
// ─────────────────────────────────────────────────────────────────────────
// vega itself is ALSO homogeneous degree 1 in (F, K) at fixed sigma (it is
// F*df*phi(d1)*sqrt(T), and d1 depends on F, K only via ln(F/K), which is
// homogeneous degree 0 -- so vega inherits F's own degree-1 scaling). The
// SAME Euler argument applied to vega gives an identical recursion:
//
//   vega_i'(u) = vega_i(u) + volga_i(u) * sigma_i'(u)
//
// Differentiate (*) once more (P_i''(u) = d/du[P_i(u) + vega_i(u)*sigma_i'(u)],
// the identity itself, not a fresh Euler application) and substitute both
// recursions at u = 0 (sigma_i''(0) = sigma''(x_i), the surface's own
// log-moneyness curvature):
//
//   P_i''(0) = P_i'(0) + vega_i'(0)*sigma'(x_i) + vega_i(0)*sigma''(x_i)
//            = [P_i(0) + vega_i*sigma'(x_i)]
//                     + [vega_i + volga_i*sigma'(x_i)]*sigma'(x_i)
//                     + vega_i*sigma''(x_i)
//            = P_i(0) + 2*vega_i*sigma'(x_i) + volga_i*sigma'(x_i)^2
//                     + vega_i*sigma''(x_i)
//
// and (mirroring q_i'(0)'s derivation, now to second order):
//
//   q_i''(0) = [P_i''(0) - 2*P_i'(0) + P_i(0)] / K_i(0)
//
// Substituting P_i(0), P_i'(0) = P_i(0) + vega_i*sigma'(x_i), and P_i''(0)
// above: the P_i(0) terms cancel (coefficients 1 - 2 + 1 = 0) and so does
// the 2*vega_i*sigma'(x_i) term against -2*P_i'(0)'s own vega_i*sigma'(x_i)
// piece (2 - 2 = 0 exactly, no leftover), leaving the clean smile-only
// result:
//
//   q_i''(0) = [vega_i*sigma''(x_i) + volga_i*sigma'(x_i)^2] / K_i(0)
//
// As in delta's derivation above, `c_i*K_i(0)` cancels only ONE of c_i's two
// `1/K_i` factors -- the second survives inside q_i''(0) itself (Review fix
// round 1, I-1: this line previously dropped it):
//
//   d^2 K_var / du^2 = (2/T) * sum_i c_i * K_i * q_i''(0)
//                    = (2/T) * sum_i [w_i*dx/3/(df*K_i)] *
//                          [vega_i*sigma''(x_i) + volga_i*sigma'(x_i)^2]
//
// SANITY COROLLARY (regression-worthy): on a genuinely FLAT smile
// (sigma'(x_i) = sigma''(x_i) = 0 at every node), delta AND gamma are
// EXACTLY zero -- the log-contract replication that makes K_var model-free
// is, by construction, insensitive to spot when there is no skew to move
// through. This is the same "PV does not depend on spot at all" property
// `HighVolRegimeGridPinKeepsSecondOrderSane` (deriv_greeks_test.cpp) already
// uses as its own FD contamination tripwire.
//
// Converting the u-derivatives to a spot derivative needs one more step than
// delta's, because d/dS = (1/S) d/du has an EXPLICIT 1/S factor of its own:
// d^2/dS^2 = d/dS[(1/S) dK_var/du] = -(1/S^2) dK_var/du + (1/S^2)
// d^2K_var/du^2, i.e.
//
//   gamma = (1/S^2) * [d^2K_var/du^2 - dK_var/du]
//
// ─────────────────────────────────────────────────────────────────────────
// VANNA: the (u, v) cross-partial
// ─────────────────────────────────────────────────────────────────────────
// Let r_i(u) = vega_i(u) / K_i(u) at v = 0 (the same object delta's own
// derivative differentiates the PRICE analogue of). Using dK_i/du = K_i(u)
// and vega_i'(u) = vega_i(u) + volga_i(u)*sigma_i'(u) (above):
//
//   r_i'(u) = [vega_i'(u)*K_i(u) - vega_i(u)*K_i(u)] / K_i(u)^2
//           = [vega_i'(u) - vega_i(u)] / K_i(u) = volga_i(u)*sigma_i'(u)/K_i(u)
//
// dK_var/dv, as a function of u (vega_Kvar(u) = (2/T)*sum_i c_i*K_i*r_i(u)),
// differentiated once more in u at u = 0 gives -- again, `c_i*K_i(0)` cancels
// only ONE of c_i's two `1/K_i` factors, the second surviving inside r_i'(0)
// itself (Review fix round 1, I-1: this line previously dropped it):
//
//   d^2K_var/(du dv) = (2/T) * sum_i c_i * K_i * r_i'(0)
//                    = (2/T) * sum_i [w_i*dx/3/(df*K_i)] * volga_i*sigma'(x_i)
//
// vanna = (1/S) * d^2K_var/(du dv) -- a SINGLE 1/S factor: this is a first
// spot-derivative of a (vol-differentiated) quantity, not a second one, so
// gamma's -1/S^2 correction term does not apply here.
//
// ─────────────────────────────────────────────────────────────────────────
// IMPLEMENTATION NOTES
// ─────────────────────────────────────────────────────────────────────────
// Wing clamp: CLAMP THE GRID POSITION ONCE, THEN SHIFT -- exactly mirroring
// `CachedBumpView::iv` (derivatives.cpp), the same machinery the FD greeks
// this file is validated against actually run through: `sigma_at` clamps a
// node's TRUE log-moneyness x_i to the resolved wing-trust band FIRST
// (`x_read = clamp(x_i, -band, band)`), and only THEN is any shift added
// (`CachedBumpView::iv`'s `k_shift`, there; this file's `kSmileDerivStep`,
// here) -- the shifted result is never re-clamped. Getting this order
// backwards (clamping x_i +/- delta directly, as an earlier version of this
// file did) reproduces the exact SAME band-edge read for every clamped node
// regardless of delta -- a STRUCTURAL error invariant to the step size
// (refining `kSmileDerivStep` cannot fix it, which is exactly the symptom
// that exposed it against the parity suite), not a precision one: the true
// surface slope AT the band edge is real and nonzero, and every node beyond
// the band shares it (all clamped reads sit at the SAME position, so the
// clamped nodes' sigma'/sigma'' are IDENTICAL to each other and to the
// unclamped node closest to the band, not zero).
//
// Smile derivatives via FD: sigma'(x_i) and sigma''(x_i) come from FOUR
// extra read vectors at (the already-clamped) x_i +/- `kSmileDerivStep` and
// x_i +/- 2*`kSmileDerivStep`, combined via the standard 5-point central
// stencils (O(delta^4) truncation, not the naive 2-point/O(delta^2) pair a
// single +/-delta read would give). This is the ONE place this file is
// still "finite difference" -- but it differentiates the FITTED SMILE (a
// cheap, already-parametrized function), never the option payoff or the
// strip integral, so it costs 4 extra batched surface reads total (not 4
// extra STRIPS), independent of how many greeks are requested. The 5-point
// upgrade is load-bearing, not a nicety: a 2-point stencil's O(delta^2)
// truncation and O(eps/delta^2) roundoff trade off at a delta too coarse for
// this file's parity gate (empirically, shrinking `kSmileDerivStep` from
// 1e-3 to 1e-4 made the 2-point gamma's FD gap WORSE, the signature of an
// already roundoff-dominated regime -- refining a 4th-order stencil's delta
// has far more room before roundoff dominates). `kSmileDerivStep = 1e-3`
// is comfortably inside that room; see `AnalyticGreeks.*`
// (deriv_greeks_test.cpp) for the empirical confirmation.
//
// Batched reads reuse `iv_batch` exactly like `var_swap_fair_strike`'s own
// node loop (has_analytic_iv_batch, mirroring that function's
// `has_strip_iv_batch`): PricedSurfaceStripView / CachedBumpView<...> expose
// it; the legacy VolSurface/EssviSurface/SviSurface and SurfaceRefStripView
// containers do not and fall through to the per-node scalar `iv()` loop --
// same dual path, same reason, no second batch-read mechanism invented.
//
// "Bad" node convention: a node whose surface read is non-finite or <= 0
// contributes ZERO to every sum, mirroring `price_node`'s own "bad" handling
// in `var_swap_fair_strike` exactly (never propagates a NaN into the total).
// A NON-FINITE +/-delta read (but a perfectly good center read) degrades
// sigma'/sigma'' to 0 rather than poisoning the sums with NaN -- "no usable
// local slope" is read as "locally flat", the same fail-safe direction the
// wing clamp's own flat tail already takes.

#include <algorithm>  // std::clamp (Review fix round 1, I-3 -- was only transitive)
#include <cassert>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

#include "atx/vol/detail/strip_grid.hpp"

namespace atx::vol::detail {

// Central-difference step (log-forward-moneyness units) for the smile's own
// sigma'/sigma'' read vectors -- see "smile derivatives via FD" above.
inline constexpr double kSmileDerivStep = 1.0e-3;

// Fused Black-76 vega + volga (defined in deriv_analytic_greeks.cpp -- pure
// function of (F, K, T, sigma, df), no SurfaceT dependency, so it does not
// need to live in this template-instantiating header). Mirrors
// `black76_value_and_vega`'s exact formula/degenerate-branch convention
// (T <= 0 or sigma <= 0 -> both fields 0.0, matching `price_node`'s "bad
// node contributes 0"), with volga's d1*d2/sigma computed alongside so
// neither vega nor volga pays for a second, independent d1/d2 resolution.
struct B76VegaVolga {
  double vega = 0.0;
  double volga = 0.0;
};

[[nodiscard]] B76VegaVolga black76_vega_volga(double F, double K, double T, double sigma,
                                              double df) noexcept;

// K_var's own sensitivities (decimal-variance units), ALREADY converted to
// spot-derivative form (the 1/S, 1/S^2 factors from "IMPLEMENTATION NOTES"
// above are applied here) -- the caller (`deriv_greeks`, derivatives.cpp)
// only has to multiply by the contract's `df*N*w_future` scale factor (see
// `price_var_swap`: `pv = df*N*(total - strike)`, `total` linear in
// `k_var_future_dec` with slope `w_future`) to get PV-level greeks.
struct AnalyticStripGreeks {
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double vanna = 0.0;
  double volga = 0.0;
};

// Structural detection of a batched `iv_batch(x, T, out)` -- mirrors
// derivatives.cpp's own `has_strip_iv_batch` (a separate, file-local copy
// because that one lives in an anonymous namespace of a different
// translation unit; both source the same requires-clause, so the two cannot
// silently drift on WHICH surfaces qualify without a diff showing up on
// both sides at once).
template <class SurfaceT>
[[nodiscard]] constexpr bool has_analytic_iv_batch() noexcept {
  return requires(const SurfaceT& s, std::span<const double> x, double t, std::span<double> out) {
    s.iv_batch(x, t, out);
  };
}

// Node-position formula, IDENTICAL to derivatives.cpp's own `node_x`
// (Review fix round 1, I-6): panel ends are the kink abscissae verbatim, not
// a rounded `k_lo + i*dx`, so no rounding step can drift a kink off the node
// it must sit on. Duplicated (not shared across TUs) for the same reason
// `has_analytic_iv_batch` above is: it closes over nothing but its own
// arguments, so any future drift between the two copies is a 3-line diff to
// eyeball, not a divergent reimplementation of `plan_strip_split` itself
// (the one shared source both walk).
[[nodiscard]] inline double analytic_node_x(const strip::StripPanel& panel, std::size_t np,
                                            double dx, std::size_t i) noexcept {
  return (i == 0)          ? panel.k_lo
        : (i + 1 == np)    ? panel.k_hi
                           : panel.k_lo + dx * static_cast<double>(i);
}

// Computes the block above on the EXACT pinned grid `var_swap_fair_strike`
// resolved for the center quote: `k_lo`/`k_hi`/`n_nodes` (that quote's own
// `strip_k_lo_used`/`strip_k_hi_used`/`strip_nodes_used`) and `wing_band`
// (its `resolved_wing_clamp`, 0.0 for "clamp off", never NaN -- the caller
// only reaches this function once a strip has actually run, which every
// non-fully-aged VarSwap dispatch guarantees).
//
// VarSwap ONLY (uncapped, any age) -- see the file header for why the other
// three kinds cannot share this closed form. `F`, `S`, `df` are the SAME
// forward/spot/discount `var_swap_fair_strike` resolved for this `T` (the
// caller reads them off the same `curves` at the same `T`, so they agree by
// construction, not by re-derivation here).
//
// Preconditions (asserted, not `Result`-checked): every one of them is
// already guaranteed by the caller having successfully priced the center
// quote through this exact (curves, T) pair -- there is no NEW way for a
// caller of THIS function to violate them that `deriv_price` would not
// already have rejected first.
template <class SurfaceT>
[[nodiscard]] AnalyticStripGreeks analytic_strip_greeks(const SurfaceT& surface, double F,
                                                        double S, double T, double df,
                                                        double k_lo, double k_hi,
                                                        std::size_t n_nodes,
                                                        double wing_band) noexcept {
  assert(F > 0.0 && S > 0.0 && T > 0.0 && df > 0.0 && "resolved by a successful center price");
  assert(k_hi > k_lo && n_nodes >= 3 && "resolved strip grid from a successful center price");

  const strip::StripSplit split = strip::plan_strip_split(k_lo, k_hi, n_nodes, wing_band);
  const std::size_t n = n_nodes;

  const auto clamp_read = [wing_band](double x) noexcept {
    return wing_band > 0.0 ? std::clamp(x, -wing_band, wing_band) : x;
  };

  // Gather pass: `x_true` is the UNCLAMPED node position (K_i = F*exp(x_true)
  // always reads this, matching `price_node`); `buf_c`/`buf_dn`/`buf_up`/
  // `buf_dn2`/`buf_up2` start life holding the CLAMPED read positions
  // (x, x -/+ delta, x -/+ 2*delta) and are overwritten IN PLACE with the
  // sigma values `iv_batch` writes back -- the same aliased-span idiom
  // `PricedSurfaceStripView::iv_batch` and derivatives.cpp's own gather pass
  // already rely on. Five read vectors total (the center plus four shifted):
  // a plain 2-point central difference's O(delta^2) truncation could not be
  // driven low enough by shrinking delta alone before roundoff (amplified
  // ~1/delta^2 for the curvature term, and further amplified by this sum's
  // own O(hundreds) of vega/volga-weighted nodes) took back everything
  // truncation gave up -- empirically confirmed against the parity suite
  // (`AnalyticGreeks.*`, deriv_greeks_test.cpp): shrinking `kSmileDerivStep`
  // from 1e-3 to 1e-4 made gamma's parity gap WORSE, not better, the
  // signature of a roundoff-dominated regime, not a truncation-dominated
  // one. The standard fix is a higher-order (5-point) stencil, O(delta^4)
  // truncation, so a roundoff-safe `kSmileDerivStep` no longer trades away
  // precision.
  std::vector<double> x_true(n);
  std::vector<double> buf_c(n);
  std::vector<double> buf_dn(n);
  std::vector<double> buf_up(n);
  std::vector<double> buf_dn2(n);
  std::vector<double> buf_up2(n);

  std::size_t idx = 0;
  for (std::size_t p = 0; p < split.count; ++p) {
    const strip::StripPanel& panel = split.panels[p];
    const std::size_t np = panel.n_nodes;
    const double dx = (panel.k_hi - panel.k_lo) / static_cast<double>(np - 1);
    const std::size_t i_begin = (p == 0) ? 0u : 1u;  // shared boundary node, visited once
    for (std::size_t i = i_begin; i < np; ++i) {
      const double x = analytic_node_x(panel, np, dx, i);
      x_true[idx] = x;
      // CLAMP ONCE, THEN SHIFT -- matching `CachedBumpView::iv` EXACTLY
      // (`x_read = clamp(raw_x, ...)`, THEN `x_read + k_shift`, never
      // re-clamped): the strip's own wing-clamp band gates the GRID
      // position alone, and a spot/smile shift applied afterward walks
      // off the band edge freely. Clamping each SHIFTED probe
      // independently (`clamp(x +/- delta)`) would instead reproduce the
      // band edge at every clamped node regardless of delta -- a
      // STRUCTURAL error, not a precision one (sig_slope/sig_curv would
      // read exactly 0 for every one of the (typically many) clamped
      // nodes, when the true surface slope AT the band edge is real and
      // nonzero) -- caught by `AnalyticGreeks.MatchesFD*`
      // (deriv_greeks_test.cpp) independently of `kSmileDerivStep`, since
      // refining delta cannot fix an error that has no delta-dependence.
      const double x_read = clamp_read(x);
      buf_c[idx] = x_read;
      buf_dn[idx] = x_read - kSmileDerivStep;
      buf_up[idx] = x_read + kSmileDerivStep;
      buf_dn2[idx] = x_read - 2.0 * kSmileDerivStep;
      buf_up2[idx] = x_read + 2.0 * kSmileDerivStep;
      ++idx;
    }
  }
  assert(idx == n && "plan_strip_split's own distinct-node-count contract");

  if constexpr (has_analytic_iv_batch<SurfaceT>()) {
    surface.iv_batch(std::span<const double>(buf_c.data(), n), T,
                     std::span<double>(buf_c.data(), n));
    surface.iv_batch(std::span<const double>(buf_dn.data(), n), T,
                     std::span<double>(buf_dn.data(), n));
    surface.iv_batch(std::span<const double>(buf_up.data(), n), T,
                     std::span<double>(buf_up.data(), n));
    surface.iv_batch(std::span<const double>(buf_dn2.data(), n), T,
                     std::span<double>(buf_dn2.data(), n));
    surface.iv_batch(std::span<const double>(buf_up2.data(), n), T,
                     std::span<double>(buf_up2.data(), n));
  } else {
    for (std::size_t i = 0; i < n; ++i) {
      buf_c[i] = surface.iv(buf_c[i], T);
      buf_dn[i] = surface.iv(buf_dn[i], T);
      buf_up[i] = surface.iv(buf_up[i], T);
      buf_dn2[i] = surface.iv(buf_dn2[i], T);
      buf_up2[i] = surface.iv(buf_up2[i], T);
    }
  }

  // Accumulate pass: walks the FULL local index range 0..np-1 of EVERY panel
  // (unlike the gather pass above), mirroring `accumulate_strip`'s own
  // `shared` variable exactly -- a node shared by two panels is READ once
  // (the gather pass's job) but WEIGHTED AND SUMMED once PER PANEL IT
  // BORDERS, each panel contributing its own `dx/3` scale. This is the
  // standard composite-Simpson aggregation across panels of differing
  // spacing (the same reason a shared node's total weight in a trapezoidal
  // composite rule is (dx_left + dx_right)/2, not dx_left/2 alone) -- an
  // ordinary `i_begin = (p == 0) ? 0 : 1` skip here would silently drop the
  // second panel's `dx/3` contribution at every interior panel boundary
  // (k = 0 and, when the wing clamp engages, +/-wing_band too), undercounting
  // K_var's own derivative by several percent -- caught by
  // `AnalyticGreeks.MatchesFD*` (deriv_greeks_test.cpp) against the FD
  // oracle before this fix.
  double total_vega = 0.0, total_delta_u = 0.0, total_gamma_u = 0.0;
  double total_vanna_u = 0.0, total_volga = 0.0;

  idx = 0;
  double shared_K = 0.0, shared_sigma = 0.0;
  double shared_sigma_dn = 0.0, shared_sigma_up = 0.0;
  double shared_sigma_dn2 = 0.0, shared_sigma_up2 = 0.0;
  for (std::size_t p = 0; p < split.count; ++p) {
    const strip::StripPanel& panel = split.panels[p];
    const std::size_t np = panel.n_nodes;
    const double dx = (panel.k_hi - panel.k_lo) / static_cast<double>(np - 1);
    double sum_vega = 0.0, sum_delta = 0.0, sum_gamma = 0.0, sum_vanna = 0.0, sum_volga = 0.0;
    for (std::size_t i = 0; i < np; ++i) {
      double K, sigma, sigma_dn, sigma_up, sigma_dn2, sigma_up2;
      if (p == 0 || i != 0) {
        K = F * std::exp(x_true[idx]);
        sigma = buf_c[idx];
        sigma_dn = buf_dn[idx];
        sigma_up = buf_up[idx];
        sigma_dn2 = buf_dn2[idx];
        sigma_up2 = buf_up2[idx];
        ++idx;
      } else {
        // The node this panel shares with the PREVIOUS panel's own last
        // index -- reuse the values gathered (and, below, the same
        // Black-76 evaluation) there rather than re-deriving them, exactly
        // as `accumulate_strip`'s `shared` carries a price forward instead
        // of recomputing it.
        K = shared_K;
        sigma = shared_sigma;
        sigma_dn = shared_sigma_dn;
        sigma_up = shared_sigma_up;
        sigma_dn2 = shared_sigma_dn2;
        sigma_up2 = shared_sigma_up2;
      }

      const double w = strip::simpson_weight(i, np);
      const B76VegaVolga bv = black76_vega_volga(F, K, T, sigma, df);
      const double inv_dfk = 1.0 / (df * K);

      // A non-finite +/-delta or +/-2*delta read degrades the local slope/
      // curvature to 0 ("no usable smile-derivative info here" -> "locally
      // flat") rather than poisoning an otherwise-good center node with a
      // NaN -- see the file header's "bad node" convention.
      const bool slope_usable = std::isfinite(sigma_dn) && std::isfinite(sigma_up) &&
                                std::isfinite(sigma_dn2) && std::isfinite(sigma_up2);
      // 5-point central-difference stencils, O(delta^4) truncation -- see
      // the gather pass's own comment for why the plain 2-point/O(delta^2)
      // stencil could not reach this file's precision target.
      const double sig_slope = slope_usable
                                   ? (-sigma_up2 + 8.0 * sigma_up - 8.0 * sigma_dn + sigma_dn2) /
                                         (12.0 * kSmileDerivStep)
                                   : 0.0;
      const double sig_curv =
          slope_usable
              ? (-sigma_up2 + 16.0 * sigma_up - 30.0 * sigma + 16.0 * sigma_dn - sigma_dn2) /
                    (12.0 * kSmileDerivStep * kSmileDerivStep)
              : 0.0;

      sum_vega += w * bv.vega * inv_dfk;
      sum_delta += w * bv.vega * sig_slope * inv_dfk;
      sum_gamma += w * (bv.vega * sig_curv + bv.volga * sig_slope * sig_slope) * inv_dfk;
      sum_vanna += w * bv.volga * sig_slope * inv_dfk;
      sum_volga += w * bv.volga * inv_dfk;

      if (i + 1 == np) {
        shared_K = K;
        shared_sigma = sigma;
        shared_sigma_dn = sigma_dn;
        shared_sigma_up = sigma_up;
        shared_sigma_dn2 = sigma_dn2;
        shared_sigma_up2 = sigma_up2;
      }
    }
    const double panel_scale = dx / 3.0;
    total_vega += sum_vega * panel_scale;
    total_delta_u += sum_delta * panel_scale;
    total_gamma_u += sum_gamma * panel_scale;
    total_vanna_u += sum_vanna * panel_scale;
    total_volga += sum_volga * panel_scale;
  }
  assert(idx == n && "plan_strip_split's own distinct-node-count contract");

  const double scale = 2.0 / T;
  const double vega_kvar = scale * total_vega;
  const double delta_dlnF = scale * total_delta_u;
  const double gamma_dlnF2 = scale * total_gamma_u;
  const double vanna_dlnFv = scale * total_vanna_u;
  const double volga_kvar = scale * total_volga;

  AnalyticStripGreeks out{};
  out.vega = vega_kvar;
  out.delta = delta_dlnF / S;
  out.gamma = (gamma_dlnF2 - delta_dlnF) / (S * S);
  out.vanna = vanna_dlnFv / S;
  out.volga = volga_kvar;
  return out;
}

}  // namespace atx::vol::detail
