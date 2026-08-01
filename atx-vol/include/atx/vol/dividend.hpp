#pragma once

// Hybrid discrete/proportional dividend forward + per-term borrow-cost
// implication for equity options — the "European-equivalent forward" builder
// that American-equity surface fitting sits on top of.
//
// ## The Vola Dynamics hybrid dividend model (Klassen 2017)
//
// The observed stock decomposes as  S_t = S~_t + D_t, where S~_t is a pure
// geometric Brownian motion and D_t is a deterministic shift equal to the
// present value of the remaining cash dividends. Discrete *cash* dividends
// dominate the short end (the escrowed-cash / Battig-Jarrow forward already
// implemented as `forward_div_corrected` in rates_curve.hpp); a *proportional*
// (continuous-yield) treatment dominates the long end, where the escrowed
// model over-suppresses variance. Klassen's practical construction blends
// the two with a single dimensionless parameter (the three named variants
// FHM/PHM/SKA differ only in how that blend and the yield are parameterised;
// this module exposes the blend directly).
//
// ## Forward construction (exact identity used below)
//
// With  q = prop_div_yield,  b = borrow,  β = blend ∈ [0,1],  T = year-frac,
// and  F_cash = forward_div_corrected(S, r, T, cash_divs, expiry, now)
//              = (S − Σ Dᵢ·e^{−r·tᵢ})·e^{rT}   (pure escrowed cash):
//
//     F(β,b,q) = e^{−b·T} · [ β · S · e^{(r − β·q)·T}
//                           + (1−β) · F_cash · e^{−β·q·T} ]
//
// This is algebraically  F = escrowed_or_blended · e^{(r − q_eff − b)·T}  with
// the escrowed base  S − (1−β)·Σ PVᵢ  and effective proportional yield
// q_eff = β·q, rearranged so `forward_div_corrected` is reused verbatim.
//
//   • β = 0            → pure escrowed cash: the (1−β) term is F_cash, the β
//                        term vanishes. With b = 0 the result is
//                        `forward_div_corrected` BIT-FOR-BIT (see test), for
//                        any q — q and b only ever enter via the carry.
//   • β = 1            → pure proportional: F = S·e^{(r − q − b)·T}.
//   • 0 < β < 1        → smooth cash→proportional transition.
//
// ## Sign conventions (documented, load-bearing)
//
//   r  > 0  raises the forward   (cost of carry, factor e^{+rT}).
//   q  > 0  lowers  the forward   (continuous dividend yield, e^{−β·q·T}).
//   b  > 0  lowers  the forward   (borrow cost / hard-to-borrow drag,
//                                  e^{−b·T}) — i.e. a positive borrow behaves
//                                  exactly like an extra dividend yield. A
//                                  hard-to-borrow name (negative rebate) has
//                                  b > 0 and a depressed forward.
//   Cash dividends with ex-date < now (already paid) or > expiry (after the
//   option) are ignored, mirroring `forward_div_corrected`.
//
// ## PORT NOTE — European vs American put-call parity
//
// `imply_borrow_european_pcp` inverts the *European* PCP *equality*
//     C − P = e^{−rT}·(F(b) − K)
// which is exact for European options and therefore self-contained (no
// American pricer needed). For American equity options PCP is only a
// *band*, not an equality, so a borrow implied here is biased by the early-
// exercise premium. De-Americanising the mids first (converting American
// call/put prices to their European equivalents via the American pricer)
// and only then calling this function is the correct pipeline; that
// de-Americanisation step lives in the de-Americanization module, which owns
// the dependency on the American pricer. This module deliberately stays on
// the European side of that seam.

#include <cstddef>
#include <cstdint>
#include <span>

#include "atx/vol/rates_curve.hpp" // DividendEvent, forward_div_corrected, kQuietNaN
#include "atx/vol/types.hpp" // Result / ErrorCode (atx::core, re-exported)

namespace atx::vol {

// Blend/yield parameters for the hybrid dividend forward. Defaults reproduce
// the pure escrowed-cash model (`forward_div_corrected`).
struct HybridDivParams {
  // Proportional (continuous) dividend yield used at the long end, e.g. 0.015
  // for 1.5%/yr. Only enters scaled by `blend`.
  double prop_div_yield = 0.0;
  // Cash→proportional blend. 0 = pure escrowed cash (reproduces
  // `forward_div_corrected` exactly when borrow == 0), 1 = pure proportional.
  // Expected in [0, 1]; the formula is well-defined for any finite value but
  // only [0, 1] is a meaningful convex blend.
  double blend = 0.0;
};

// Borrow-independent factor G in the hybrid forward F(b) = G*exp(-b*T).
// Bind it once per expiry/carry strip and reuse it for every trial borrow.
//
// @return G, or NaN under the same invalid-input contract as hybrid_forward.
[[nodiscard]] double hybrid_forward_base(double S, double r, double T,
                                         std::span<const DividendEvent> cash_divs,
                                         std::int64_t expiry_ns, std::int64_t now_ts_ns,
                                         const HybridDivParams &hyb) noexcept;

// Apply a borrow to a previously bound hybrid-forward base.
//
// @param base       finite borrow-independent factor returned above
// @param borrow,T   finite continuous borrow and positive year-fraction
// @return           base*exp(-borrow*T), or NaN on invalid input
[[nodiscard]] double hybrid_forward_from_base(double base, double borrow, double T) noexcept;

// Hybrid dividend forward (see header formula and sign conventions above).
//
// `borrow` enters as an extra continuous carry: borrow > 0 lowers the forward
// like a dividend yield (factor e^{−borrow·T}). The escrowed-cash component is
// `forward_div_corrected` reused verbatim, blended toward the proportional
// forward S·e^{(r−q)·T} by `hyb.blend`.
//
// @param S          spot (> 0)
// @param r          continuously-compounded rate for [now, T]
// @param borrow     continuous borrow cost (> 0 hard-to-borrow, lowers F)
// @param T          year-fraction to expiry (> 0)
// @param cash_divs  discrete cash dividends; ex-dates outside [now, expiry]
//                   are ignored (mirrors forward_div_corrected)
// @param expiry_ns  option expiry, epoch nanoseconds
// @param now_ts_ns  valuation timestamp, epoch nanoseconds
// @param hyb        blend + proportional-yield parameters
// @return           the hybrid forward, strictly decreasing in `borrow`; NaN
//                   if S <= 0, T <= 0, or any scalar input is non-finite.
//                   With hyb.blend == 0 and borrow == 0 this equals
//                   forward_div_corrected(S, r, T, cash_divs, expiry_ns,
//                   now_ts_ns) bit-for-bit.
[[nodiscard]] double hybrid_forward(double S, double r, double borrow, double T,
                                    std::span<const DividendEvent> cash_divs,
                                    std::int64_t expiry_ns, std::int64_t now_ts_ns,
                                    const HybridDivParams &hyb) noexcept;

// Analytic Jacobian ∂F/∂D_i of the escrowed hybrid forward w.r.t. each cash-
// dividend AMOUNT. Cash dividends enter F only through the escrowed base
// F_cash (LINEARLY: F_cash = (S − Σ Dᵢ·e^{−r·tᵢ})·e^{rT}), so for an event whose
// ex-date lies in [now, expiry] — the SAME window `forward_div_corrected` sums —
//   ∂F/∂Dᵢ = −(1 − blend)·e^{−borrow·T}·e^{−blend·prop_div_yield·T}·e^{r·(T − tᵢ)}
// with tᵢ = (ex_date_ns − now_ts_ns) in 365.25-day years; out-of-window events get
// 0. The coefficient is independent of S and of the amounts themselves (the map is
// linear in each Dᵢ), so `S` is not a parameter. This is the analytic ground truth
// a central-difference bump of `hybrid_forward` converges to, and the ∂F/∂Div leg
// of the American ∂P/∂Div chain rule (see american.hpp
// `american_dividend_sensitivities`).
//
// @param dF_dDiv_out written elementwise for each cash_divs[i] up to the smaller of
//        the two spans; a non-finite scalar input writes NaN into every touched slot.
void hybrid_forward_div_jacobian(double r, double borrow, double T,
                                 std::span<const DividendEvent> cash_divs,
                                 std::int64_t expiry_ns, std::int64_t now_ts_ns,
                                 const HybridDivParams &hyb,
                                 std::span<double> dF_dDiv_out) noexcept;

// Imply the per-term borrow cost from a co-terminal (same K, T) European
// call/put pair via European put-call parity:
//
//     C − P = e^{−rT}·(F(b) − K),   F(b) = hybrid_forward(..., borrow = b, hyb)
//
// F(b) is strictly monotone (decreasing) in b — F(b) = G·e^{−b·T}, G > 0
// independent of b — so PCP has the direct solution
//
//     b = -ln(((C-P)·e^{rT} + K) / G) / T.
//
// @param call_price European call price/mid at (K, T)
// @param put_price  European put price/mid at (K, T)
// @param S,K,T,r    spot, strike, year-fraction (all > 0), cc rate (finite)
// @param cash_divs  cash dividend schedule (as in hybrid_forward)
// @param hyb        blend + proportional-yield parameters (borrow is solved)
// @param b_lo,b_hi  accepted range for the borrow (finite, b_lo < b_hi)
// @param tol        VESTIGIAL (R-26): validated finite-positive for API
//                   compatibility, but the closed-form PCP inversion is exact and
//                   does NOT consume it. A root at the bracket edge is admitted
//                   only within a fixed machine-roundoff slack, never within
//                   `tol` — a root genuinely outside [b_lo, b_hi] is rejected
//                   even when |root - edge| < tol. Retained so the former
//                   bisection's signature stays call-compatible.
// @return           the implied borrow b, or:
//                     InvalidArgument — bad scalar inputs / bracket / tol,
//                                       or a non-finite forward factor
//                     OutOfRange      — the implied borrow lies outside
//                                       [b_lo, b_hi] (no sign change)
//
// See the PORT NOTE above: this is the European *equality*; American mids must
// be de-Americanised upstream before it applies.
[[nodiscard]] atx::core::Result<double> imply_borrow_european_pcp(
    double call_price, double put_price, double S, double K, double T, double r,
    std::span<const DividendEvent> cash_divs, std::int64_t expiry_ns, std::int64_t now_ts_ns,
    const HybridDivParams &hyb, double b_lo = -0.5, double b_hi = 0.5, double tol = 1e-8) noexcept;

// Closed-form PCP inversion using a pre-bound G. This is the carry-loop entry:
// callers that solve several co-terminal pairs bind the expiry geometry once
// with hybrid_forward_base rather than rescanning dividends per fixed-point
// iteration and PCP solve.
//
// @param base       finite, positive borrow-independent hybrid-forward factor
// @return           same result/error contract as imply_borrow_european_pcp
[[nodiscard]] atx::core::Result<double>
imply_borrow_european_pcp_from_base(double call_price, double put_price, double K, double T,
                                    double r, double base, double b_lo = -0.5, double b_hi = 0.5,
                                    double tol = 1e-8) noexcept;

// One co-terminal call/put strike's mids, for the strip convenience below.
struct CoTermQuote {
  double strike = 0.0;
  double call_mid = 0.0;
  double put_mid = 0.0;
};

// Convenience (optional): the dividend/borrow-adjusted forward implied by a
// whole strip of co-terminal call/put mids, via robust near-ATM averaging.
//
// Each strike gives a *model-free* PCP forward  Fᵢ = (Cᵢ − Pᵢ)·e^{rT} + Kᵢ
// (this already embeds whatever dividends and borrow the market is pricing);
// the estimate is the simple mean of Fᵢ over the `n_atm` strikes whose strike
// is nearest to `S`. Averaging near ATM avoids the wide-wing strikes where a
// small mid error is amplified by the C−P difference. The returned forward can
// be compared against `hybrid_forward`, or fed strike-by-strike into
// `imply_borrow_european_pcp`.
//
// @param quotes non-empty co-terminal strip
// @param S      spot / ATM reference (> 0)
// @param T,r    year-fraction (> 0), cc rate (finite)
// @param n_atm  number of nearest-to-ATM strikes to average (>= 1; clamped to
//               the strip size)
// @return       the mean near-ATM PCP forward, or InvalidArgument on bad input.
[[nodiscard]] atx::core::Result<double> imply_forward_atm_pcp(std::span<const CoTermQuote> quotes,
                                                              double S, double T, double r,
                                                              std::size_t n_atm = 3);

} // namespace atx::vol
