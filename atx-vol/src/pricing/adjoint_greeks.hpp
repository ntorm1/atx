#pragma once

// ── American / European greeks via a taped boundary tangent — WS-P P2 + P3-pre ──
//
// NAME WARNING (L5 T6, 2026-08-23). "Adjoint" in this file's name and in
// `american_greeks_adjoint` is HISTORICAL. This header used to describe a
// kernel that was never built; what ships is:
//
//   * European (`european_greeks_adjoint`): a genuine hand-coded REVERSE sweep
//     of the BSM price graph, FIRST ORDER ONLY (`euro_reverse`,
//     adjoint_greeks.cpp). Second order comes from the closed BSM forms.
//   * American (`american_greeks_adjoint`): FORWARD-MODE tangent propagation
//     through the taped boundary iteration —
//     ẏ⁰ = 0, ẏᵏ = ∂G_k/∂y·ẏᵏ⁻¹ + ∂G_k/∂θ — with BOTH Jacobian-vector products
//     obtained by FINITE-DIFFERENCING `al_apply_boundary_sweep`. No adjoint
//     variable λ, no transposed solve Jᵀλ = (∂P/∂y)ᵀ, no reverse sweep anywhere
//     on the American path. The tangent runs ONCE PER PARAMETER (σ, then r), so
//     its cost scales with the parameter count — precisely what an adjoint
//     exists to eliminate.
//
// Cost of one American bundle, counted from the code: 3 boundary solves (one
// taped, plus two cold σ± re-solves for volga), 2 tangent passes (each applying
// the boundary sweep twice per taped iteration), and 15 price evaluations off a
// boundary. A genuine Christianson/Giles-Glasserman adjoint would return the
// whole greek row for ~3-4x ONE price REGARDLESS of how many upstream
// parameters feed it (Giles-Glasserman, "Smoking Adjoints", RISK 2006;
// NA-05-15). That gap is real and unrealised — a Phase-2 item, not something
// this file does. `docs/adjoint_greeks_design.md` §4-§5 specify the adjoint
// that was DESIGNED; read them as the target, not as a description of the code.
//
// What the kernel does deliver: all 8 greeks (delta, gamma, vega, theta, rho,
// vanna, volga, charm) from ONE taped forward solve instead of the FD bundle's
// seven cold ones, with the boundary sensitivities differentiated through the
// ACTUAL budget-limited iteration (Christianson 1994's attractive-fixed-point
// result is what licenses dropping the seed tangent ẏ⁰), so the greek matches
// the SERVED mark derivative on the wide domain (~83% of a realistic grid)
// rather than the ~1/12 well-converged subset the earlier exact-fixed-point IFT
// claimed.
//
// Accuracy is NOT "machine-precise vs a central-difference reference", as this
// header used to claim. The European closed forms are exact; the American
// bundle is finite-difference throughout, and its own acceptance guard is 3% of
// the sum of two independent vega estimates plus 1e-3 absolute (the
// self-consistency check in adjoint_greeks.cpp). Measured max gaps vs a
// Richardson reference over the 189-point `DiagnosticGaps` grid: vega 9.5e-3,
// vanna 1.8e-3, volga 2.4.
//
// This is the pricing lever that replaces american_greeks/fd_warm (the ~1.5 ms,
// ~7-boundary-solve finite-difference bundle). The existing FD path is UNTOUCHED
// and remains the fallback for every regime this kernel does not claim.
//
// Scalar-first; SIMD is out of scope this sprint (sprint §Carry-forward).
// Pure functions (no globals/statics mutation) so live == backtest bit-for-bit.

#include <optional>

#include "atx/vol/api/pricing/american.hpp" // AmericanGreeks, AlOpts, Result, Side

namespace atx::vol::detail {

// European (Black-Scholes-Merton spot form, continuous yield q) greeks. First
// order (delta, vega, rho, theta) via a hand-coded REVERSE sweep of the price
// graph — the adjoint architecture, exposing the full direct-input gradient
// [∂P/∂S, ∂P/∂K, ∂P/∂T, ∂P/∂σ, ∂P/∂r, ∂P/∂q]; second order (gamma, vanna, volga,
// charm) via the exact BSM closed forms. theta is calendar-time (-∂P/∂T). This
// is the one genuinely reverse-mode piece of the kernel AND the exact American
// price in the no-early-exercise regime (American == European). Always succeeds
// for positive inputs.
//
// Degenerate limits, in `andersen_lake_core`'s own order:
//   T ~ 0    -> the SPOT intrinsic, delta ±1 (no time left).
//   σ ~ 0    -> the EUROPEAN σ->0 limit df·max(sgn·(F-K), 0), F = S·e^{(r-q)T} —
//               the discounted FORWARD intrinsic, with delta = sgn·e^{-qT}, and
//               rho/theta/charm/∂P∂q filled from the same closed form (L5 T3;
//               this arm used to return the bare spot intrinsic with delta ±1,
//               dropping the forward AND the discount factor). Second-order
//               greeks are exactly 0: no optionality is left.
//
// @param dP_dq optional out: ∂P/∂q, the carry sensitivity `AmericanGreeks` has
//        no field for (G2). Written iff non-null; the reverse sweep's exact BSM
//        ∂P/∂q (= -T·S·e^{-qT}·Φ(±d1)) on the non-degenerate branch,
//        -sgn·T·S·e^{-qT} on the ITM σ~0 arm, 0 at T~0 and out of the money.
//        Pure (writes only through the caller's pointer).
[[nodiscard]] AmericanGreeks european_greeks_adjoint(double S, double K, double T, double sigma,
                                                     double r, double q, Side side,
                                                     double *dP_dq = nullptr) noexcept;

// American greeks via the Christianson through-iterations adjoint. Claims genuine
// early-exercise PUTS only (r > 0, non-degenerate, single-boundary American
// regime): delta/gamma from frozen-base-boundary spot stencils (boundary is
// spot-independent — exact, NOT the Γ=0 trap); vega/rho from the boundary tangent
// dy*/dσ, dy*/dr differentiated THROUGH the actual budget-limited Andersen-Lake
// iteration the pricer ran (Christianson 1994 reverse-accumulation of iterated
// maps — matches the served mark derivative on the wide domain, not just the
// well-converged fixed-point subset the P2 IFT claimed); vanna from the first-order
// boundary tangent y_σ; volga from a COLD σ± boundary re-solve 2nd difference (a
// warm re-solve's residual is amplified by 1/h² and blows up); theta/charm from the
// continuation-region Black-Scholes PDE identity.
//
// Every other regime falls back to american_greeks_fd (the untouched FD
// reference): calls, the European-exact regime (American == European), degenerate
// T~0/σ~0, the negative-carry / double-continuation corners, and any bumped
// boundary that fails to solve. The result is therefore a drop-in for
// american_greeks_fd on the whole domain, exact on the fallback regimes and
// adjoint-accelerated on the genuine-early-exercise put hot path.
//
// @param took_adjoint_path optional out: set true iff the genuine IFT-adjoint path
//        produced the result (false on every FD/European fallback). Lets tests and
//        callers confirm WHICH path ran behind a reliability claim (a fallback also
//        returns has_value(), so the value alone is ambiguous). Pure: writes only
//        through the caller's pointer, no global state.
// @return InvalidArgument on non-positive S/K/T/σ (matches american_greeks_fd);
//         otherwise the 8 greeks + price, with price == the andersen_lake mark.
[[nodiscard]] Result<AmericanGreeks>
american_greeks_adjoint(double S, double K, double T, double sigma, double r, double q, Side side,
                        const std::optional<AlOpts> &opts = std::nullopt,
                        bool *took_adjoint_path = nullptr);

} // namespace atx::vol::detail
