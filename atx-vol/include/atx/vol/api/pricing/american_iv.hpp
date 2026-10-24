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
// monotone increasing in sigma, so a bracketing root-find is robust (except inside
// the Andersen-Lake kernel's exercise region close to the zero-volatility value,
// where it is not: see "Typed entry point with the opt-in non-monotone refusal"). The
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
#include <limits>
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

// ── Typed entry point with the opt-in non-monotone refusal (L1c, 2026-10-18) ──
//
// The cold Andersen-Lake map P(sigma) is NOT monotone everywhere (region map:
// sprints/2026-10-18-L1c-al-nonmonotone-sprint.md). Every decreasing step of it lies
// in the kernel's OWN exercise region at t = 0: spot on the exercise side of
// exercise_boundary(K, T, sigma, r, q, side, opts) (put S <= B, call S >= B). There
// the exact American value is the intrinsic, but the kernel prices euro + premium
// floored only at the intrinsic, so it prints a premium artifact that rises and falls
// with sigma (measured up to 1.2e-6 * K * max(1, T)^2 above the zero-volatility value
// on the accurate preset and its Fast profile, 9.1e-5 on al_fast_opts). A target that
// close to the zero-volatility value can have several roots, and an inverter returns
// whichever its path meets: Ok answers 0.005-0.18 vol apart on the same target.
//
// IvOpts::refuse_non_monotone (default false: every output is bit-identical to the
// positional overload above) types those answers as IvStatus::NonMonotoneRegion on the
// cold Andersen-Lake route (method AndersenLake and no usable correction cache; the
// BAW and cached maps never refuse). An Ok answer x is refused when the target lies
// in the band price - zero_vol <= al_non_monotone_gate(K, T, opts) and EITHER x lies
// in the kernel's exercise region (al_non_monotone_region), OR x is above kIvMin and
// not certified on the cold map: the chord slope over [x (1 - 1e-4), x] is <= 1e-8 K,
// or the Newton correction |P(x) - price| / slope exceeds 1e-4 vol. Targets above the
// band, and the kIvMin floor clamp outside the exercise region, keep today's answer.

enum class IvStatus : std::uint8_t {
  Ok = 0,
  InvalidArgument = 1,   // S/K/T <= 0 (ErrorCode::InvalidArgument)
  OutOfRange = 2,        // non-finite input or price outside the identifiable band
  NoConvergence = 3,     // root-find did not converge in max_iter (ErrorCode::Unavailable)
  Unpriceable = 4,       // the pricer refused (NotImplemented regimes) or allocation failed
  NonMonotoneRegion = 5, // opt-in refusal: see above
};

// ── Opt-in de-Americanised inversion strategy (L2e, 2026-10-19) ──
//
// Search (the default) is the safeguarded Newton above, bit for bit. Premium inverts
// by de-Americanisation: P_am(sigma) = A(sigma) + D(sigma) with a cheap seed leg A
// (Barone-Adesi-Whaley where the contract has an exercise region, Black-76 elsewhere)
// whose remainder D varies slowly in sigma. The leg inverted at the quote gives sigma0;
// one cold Andersen-Lake solve gives D(sigma0); the leg inverted at `price - D(sigma0)`
// gives sigma1, and each further iterate solves the leg plus the D chord through the
// last two cold points exactly (no American solve), confirmed by one cold solve. A
// root is ACCEPTED only when its Newton distance on the cold map, |P_am(sigma) - price|
// / slope (slope: the leg's slope plus that D chord), is <= tol: the vol-unit
// tolerance the Search route's bracket and polish stop on. The accepted value is that
// Newton-corrected sigma.
// Everything else FALLS BACK to the Search route unchanged (bit-identical answer): any
// input the Search route refuses, a price at the identifiable-range edges or inside
// the non-monotone band (al_non_monotone_gate), no European seed, a chord that is not
// an increasing slope, a root at the volatility floor or above the search ceiling, a
// pricer failure, or no acceptance within 4 cold solves. Premium applies only on the
// cold Andersen-Lake route with warm_start 0 and price_tol 0 (any other route runs
// Search), and refuse_non_monotone acts on its answer exactly as on Search.
enum class IvStrategy : std::uint8_t {
  Search = 0,  // the default safeguarded Newton (historical, bit for bit)
  Premium = 1, // OPT-IN: early-exercise-premium inversion with a Search fallback
};

// The positional overload's controls as one struct, plus the opt-in refusal. Same
// meanings and defaults as american_implied_vol above (al_opts is its `opts`).
struct IvOpts {
  AmericanMethod method = AmericanMethod::AndersenLake;
  double tol = 1.0e-7;
  std::uint16_t max_iter = 64;
  std::optional<AlOpts> al_opts = std::nullopt;
  const CorrectionCache *correction = nullptr;
  double warm_start = 0.0;
  double price_tol = 0.0;
  bool refuse_non_monotone = false;         // OPT-IN (L1c): see the section comment above
  IvStrategy strategy = IvStrategy::Search; // OPT-IN (L2e): see IvStrategy
  // Premium only: the kernel profile of the iterate (seed) solves. Unset (the default)
  // or al_opts' own profile: every solve is on al_opts, the accurate kernel the Search
  // route inverts on. Another profile: iterates run al_opts' scheme on that profile and
  // the accepted root is confirmed by one solve on al_opts (Newton distance <= tol).
  // AlProfile::Screen (SPY1s Phase 1, 2026-10-20) selects the MEASURED Screen tier
  // al_screen_opts() (american.hpp: 7 nodes, l 16, 5 sweeps, p 48 - the only Screen
  // scheme with a tested bound), not al_opts' scheme on the Screen arithmetic. A price
  // inside Screen's wide non-monotone gate (al_non_monotone_gate of al_screen_opts())
  // runs its iterates on al_opts instead (the accurate kernel; no confirm needed), so a
  // Screen refusal never becomes a quote refusal and Screen is never classified as the
  // accurate scheme (L2f note section 4).
  std::optional<AlProfile> seed_profile = std::nullopt;
};

struct IvResult {
  double iv = std::numeric_limits<double>::quiet_NaN(); // the vol on Ok, NaN otherwise
  IvStatus status = IvStatus::InvalidArgument;
};

// american_implied_vol with typed statuses. With refuse_non_monotone false and the
// Search strategy, iv equals the positional overload's value bit for bit and status
// maps its Error code (InvalidArgument, OutOfRange, Unavailable -> NoConvergence,
// anything else -> Unpriceable). A Premium fallback returns exactly that as well.
[[nodiscard]] IvResult american_implied_vol(double price, double S, double K, double T,
                                            double r, double q, Side side,
                                            const IvOpts &opts) noexcept;

// Width of the non-monotone band above the zero-volatility value:
// kappa * K * max(1, T)^2 with kappa = 5e-6 for the accurate scheme (nullopt, or its
// explicit equivalent al_fast_profile(nullopt) under either AlProfile) and 4e-4 for
// any other scheme. kappa is ~4x the largest exercise-region artifact the map measured
// for that scheme class (accurate / Fast profile 1.2e-6; al_fast_opts 9.1e-5,
// al_bulk_opts 3.2e-5); unmapped schemes take the wide value. NaN for non-finite or
// non-positive K / T.
[[nodiscard]] double al_non_monotone_gate(double K, double T,
                                          const std::optional<AlOpts> &opts) noexcept;

// True iff `price` lies in the non-monotone band (price - zero_vol <= the gate) AND the
// spot lies in the kernel's exercise region at `sigma` (one exercise_boundary solve,
// paid only inside the band). A boundary the kernel refuses (the European regime has
// none) is outside the region; an allocation failure answers true (refuse is the safe
// side). The predicate the batch routes apply to their Ok rows (batch_chain.hpp).
[[nodiscard]] bool al_non_monotone_region(double price, double S, double K, double T, double r,
                                          double q, Side side, double sigma,
                                          const std::optional<AlOpts> &opts) noexcept;

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
