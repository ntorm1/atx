#pragma once

// ── Adjoint (AAD) American / European greeks — WS-P P2 + P3-pre ───────────
//
// Hand-coded adjoint algorithmic differentiation of the Andersen-Lake American
// pricer. All 8 greeks (delta, gamma, vega, theta, rho, vanna, volga, charm) from
// ONE taped forward solve plus a reverse tangent through the early-exercise
// boundary. P2 differentiated the exact fixed point via the implicit-function
// theorem (mark-consistent only on the ~1/12 well-converged subset); P3-pre
// switches the boundary sensitivities to Christianson (1994) reverse-accumulation
// through the ACTUAL budget-limited iteration, so the greek matches the served mark
// derivative on the wide domain (~83% of a realistic grid). Machine-precise vs a
// central-difference reference. Design + primary-source citations:
// docs/adjoint_greeks_design.md.
//
// This is the pricing lever that replaces american_greeks/fd_warm (the ~1.5 ms,
// ~7-boundary-solve finite-difference bundle). The existing FD path is UNTOUCHED
// and remains the fallback for every regime this kernel does not claim.
//
// Scalar-first; SIMD is out of scope this sprint (sprint §Carry-forward).
// Pure functions (no globals/statics mutation) so live == backtest bit-for-bit.

#include <optional>

#include "atx/vol/american.hpp" // AmericanGreeks, AlOpts, Result, Side

namespace atx::vol::detail {

// European (Black-Scholes-Merton spot form, continuous yield q) greeks. First
// order (delta, vega, rho, theta) via a hand-coded REVERSE sweep of the price
// graph — the adjoint architecture, exposing the full direct-input gradient
// [∂P/∂S, ∂P/∂K, ∂P/∂T, ∂P/∂σ, ∂P/∂r, ∂P/∂q]; second order (gamma, vanna, volga,
// charm) via the exact BSM closed forms. theta is calendar-time (-∂P/∂T). This
// is the TDD rung 1 for the adjoint machinery AND the exact American price in the
// no-early-exercise regime (American == European). Always succeeds for positive
// inputs; degenerate T~0 / σ~0 collapses to intrinsic greeks.
//
// @param dP_dq optional out: the reverse sweep's ∂P/∂q carry sensitivity — the
//        6th gradient component the returned AmericanGreeks has no field for (G2).
//        Written iff non-null; the machine-exact BSM ∂P/∂q (= -T·S·e^{-qT}·Φ(±d1))
//        on the non-degenerate branch, 0 in the intrinsic limit. Pure (writes only
//        through the caller's pointer).
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
