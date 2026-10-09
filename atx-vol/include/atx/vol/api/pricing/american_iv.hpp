#pragma once

// American-option implied-volatility inversion.
//
// Inverts an observed AMERICAN option price back to the lognormal volatility
// that reproduces it under the library's American pricer (american.hpp). This
// is the load-bearing primitive for "de-Americanization": American market
// quotes are inverted here into an American-consistent sigma, which downstream
// is converted to a European-equivalent implied vol before the surface fitter
// ever sees it.
//
// The forward map P(sigma) = american_price(..., sigma, ..., method, opts) is
// monotone increasing in sigma, so a bracketing root-find is robust. The
// inverter is a safeguarded Newton (Numerical Recipes `rtsafe`): a Newton step
// driven by the American vega (american_greeks().vega) accelerates convergence,
// while a maintained [lo, hi] bracket with a forced-bisection fallback
// guarantees the iterate never leaves the sign-change interval and that the
// bracket width shrinks every step. The European implied vol (implied_vol.hpp)
// seeds the initial guess whenever it is available.
//
// Because the SAME pricer is used as the forward map, a price produced by
// `american_price(sigma, method)` inverts back to `sigma` regardless of that
// method's absolute accuracy — the round-trip is self-consistent.
//
// Observably pure and safe to call concurrently. Each thread retains private
// Andersen-Lake scratch between calls; no pricing state is shared across threads.
// This remains a cold-path routine (surface-fit cadence), not a per-tick kernel.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "atx/vol/api/pricing/american.hpp" // AmericanMethod, AlOpts
#include "atx/vol/api/core/types.hpp"    // Side, Result, Status

namespace atx::vol {

// Invert an American option price to its implied volatility.
//
// @param price    observed American premium. Must lie inside the no-arbitrage
//                 band (American intrinsic, upper]; a price at intrinsic clamps
//                 to kIvMin and succeeds (sigma is not identifiable there).
// @param S,K      spot and strike (> 0)
// @param T        year-fraction to expiry (> 0)
// @param r,q      continuously-compounded rate and dividend yield (any sign)
// @param side     Call or Put
// @param method   cold pricer used as the forward map — must match the pricer
//                 the quote was produced with for an exact round-trip
// @param tol      convergence tolerance in volatility units (bracket half-width)
// @param max_iter iteration cap (bounded-loop guard, JPL Rule 2)
// @param opts     Andersen-Lake accuracy preset (honoured only for AndersenLake)
// @param correction  optional hot-path correction cache. When non-null, populated,
//                 and built for THIS `side`, the forward map and Newton vega use
//                 the cached pricer (`american_price_cached`) instead of the cold
//                 Andersen-Lake path — orders of magnitude faster, and the
//                 inversion is self-consistent with any re-pricing through the
//                 same cache. A null/mismatched/unpopulated cache falls back to
//                 the cold `method` path (bit-identical to the pre-cache result).
// @param warm_start  optional Newton seed (a prior nearby sigma). When > 0 and
//                 inside the bracket it takes priority over the European seed —
//                 e.g. a borrow fixed-point re-inverting the same leg as q_eff
//                 nudges converges in ~1 step from the previous iterate. 0 (the
//                 default) means "no warm start; use the European seed". The
//                 result is unchanged — only the iteration count differs.
// @param price_tol  optional PRICE-unit convergence demand on the cold
//                 Andersen-Lake reference map (the map `american_greeks_al`
//                 re-prices with). 0 (the default) reproduces the sigma-unit
//                 polish behaviour bit-for-bit. When > 0 the cold-AL polish
//                 additionally verifies |cold(sigma) - price| <= price_tol and,
//                 where the warm search map's seed-dependent boundary left the
//                 root a multiple of `tol` away from the cold root (measured
//                 1e-5..6.2e-5 sigma at long-dated deep-ITM high-vega corners,
//                 2026-08-23), continues the cold-map Newton past the sigma
//                 drift cap — keeping that cap's collapsed-vega protection by
//                 accepting only iterates that REDUCE the cold-map residual.
//                 Bounded: at most 4 cold solves total. Only the cold
//                 AndersenLake route reads it; cached/BAW routes ignore it.
// @return         the implied volatility, or an Error:
//                   InvalidArgument — S/K/T <= 0
//                   OutOfRange      — non-finite input, or price outside the
//                                     no-arbitrage band [intrinsic, upper]
//                   Unavailable     — root-find did not converge in max_iter
//                   Internal        — allocation failure (the message is empty:
//                                     building one would allocate again). These
//                                     entry points are noexcept, so the
//                                     inversion's allocations — the ~46 KB
//                                     Andersen-Lake pricer state, the Error
//                                     message strings — are contained here
//                                     rather than terminating the process.
//                 Any pricer Error (e.g. Andersen-Lake NotImplemented on the
//                 negative-rate corner) is propagated unchanged.
[[nodiscard]] Result<double>
american_implied_vol(double price, double S, double K, double T, double r, double q, Side side,
                     AmericanMethod method = AmericanMethod::AndersenLake, double tol = 1.0e-7,
                     std::uint16_t max_iter = 64, const std::optional<AlOpts> &opts = std::nullopt,
                     const CorrectionCache *correction = nullptr, double warm_start = 0.0,
                     double price_tol = 0.0) noexcept;

// Fixed-weight two-cache forward map. The blend is placed before the optional
// solver controls so callers cannot accidentally bind it to the legacy cache
// pointer slot. Exact endpoint weights preserve the single-cache route.
[[nodiscard]] Result<double>
american_implied_vol(double price, double S, double K, double T, double r, double q, Side side,
                     const CorrectionBlend &correction,
                     AmericanMethod method = AmericanMethod::AndersenLake, double tol = 1.0e-7,
                     std::uint16_t max_iter = 64, const std::optional<AlOpts> &opts = std::nullopt,
                     double warm_start = 0.0, double price_tol = 0.0) noexcept;

// Batch inversion over a strike axis (scalar-backed; mirrors the library's
// *_batch shape and the parallel-status failure convention of
// implied_vol_batch). S, T, r, q, side, method, tol, max_iter, opts are shared
// across lanes; `price` and `K` vary per lane. Each `iv_out[i]` receives the
// inverted vol (or NaN on a lane failure) and each `status_out[i]` the per-lane
// Status. `price`, `K`, `iv_out`, and `status_out` must all have equal length.
//
// @param warm_start_chain  P6 / perf F9. When true, each lane seeds its Newton
//                 search from the previous lane's converged root (all lanes share
//                 one side), falling back to the per-quote European seed when the
//                 previous lane failed or its strike is more than a documented
//                 log-moneyness step away. On a strike-sorted CACHED batch this
//                 chains near-equal adjacent-strike IVs and cuts residual evals
//                 materially (measured 65 -> 42 on an 11-lane put ladder). The seed
//                 only shifts the Newton PATH: `iv_out` is economic-parity to the
//                 false (cold) path — bounded by the inverter's warm_start
//                 invariance (< 1e-9 on the cached map, ~1e-6 on the cold
//                 Andersen-Lake map, whose 2-step polish is seed-dependent), far
//                 below any economic budget. Default false.
//
// Error model (4.3): the entry reports through `Result<std::size_t>` — the
// library's one error channel — carrying the LANE COUNT it wrote on success.
// The outputs stay caller-owned spans, so the value in the Result is the count,
// never a freshly allocated container. That count is the length of the parallel
// `status_out` channel a caller must walk, and it is only defined when the call
// succeeded — exactly the property the old `Status` shape could not express.
//
// @return the number of lanes written (every lane is written; a lane's own
//         failure lives in status_out[i], not in this return), or
//         InvalidArgument on a span-length mismatch.
[[nodiscard]] Result<std::size_t> american_implied_vol_batch(
    std::span<const double> price, double S, std::span<const double> K, double T, double r,
    double q, Side side, std::span<double> iv_out, std::span<Status> status_out,
    AmericanMethod method = AmericanMethod::AndersenLake, double tol = 1.0e-7,
    std::uint16_t max_iter = 64, const std::optional<AlOpts> &opts = std::nullopt,
    const CorrectionCache *correction = nullptr, bool warm_start_chain = false);

} // namespace atx::vol
