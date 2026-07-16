#pragma once

// American option pricing.
//
// Ported from the C `ats-vol` library (ats_amer.h, ats_pricer_al*.c,
// ats_pricer_baw_american.c, ats_greeks_american.c). Two cold-path pricers plus
// a chain-rule Greeks layer:
//
//   1. Andersen-Lake-Offengenden spectral collocation (`andersen_lake`) — the
//      high-accuracy cold pricer (~10-11 sig figs). A Chebyshev-in-sqrt-time
//      early-exercise boundary is solved by damped Jacobi-Newton + naive
//      fixed-point sweeps; the early-exercise premium is a Gauss-Legendre
//      quadrature. Calls are priced from puts via McDonald-Schroder symmetry.
//
//   2. Barone-Adesi-Whaley (`baw_american`) — the quadratic analytic
//      approximation. Fast, 3-4 sig figs. Also seeds the AL boundary iteration.
//
// The hot path (`american_price_cached`) is Black-76 plus a cached Chebyshev
// correction (see correction.hpp); `american_greeks` differentiates that graph.
//
// The European base is the already-ported Black-76 kernel (black76.hpp): the
// American price is European + early-exercise premium/correction. Normal CDF/PDF
// come from atx::core (math.hpp).
//
// The C library shipped AVX2 batch variants; those are deferred in this port.
// Stateless and pure — every entry is safe to call concurrently from any thread.

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/types.hpp"

namespace atx::vol {

// Defined in correction.hpp — observed (non-owning) by the cached pricer/Greeks.
class CorrectionCache;
struct CorrectionBlend;

// ── Andersen-Lake tuning knobs ──────────────────────────────────────────
//
// Mirrors the C `AtsVolALOpts`. The public knobs are mapped internally to a
// QuantLib-style accuracy preset (boundary nodes, fixed-point / premium
// quadrature orders, and sweep counts).
struct AlOpts {
  std::uint16_t n_collocation = 12;  // Chebyshev boundary nodes
  std::uint16_t n_quadrature = 24;   // Gauss-Legendre fixed-point order
  std::uint16_t max_newton_iter = 8; // total Jacobi-Newton + fixed-point sweeps
  double tol = 1.0e-10;              // convergence tol on the boundary residual
};

// The C `ats_pricer_al_default_opts()`: {12, 24, 8, 1e-10}.
//
// NOTE: passing al_default_opts() EXPLICITLY is NOT equivalent to passing
// std::nullopt. std::nullopt selects the internal ACCURATE preset
// {n_boundary=12, n_quad_fp=24, n_quad_price=48, n_iter_jn=2, n_iter_fp=4,
// tol=1e-10}. An explicit AlOpts instead drives the premium quadrature off
// n_quadrature (scheme_from_opts sets n_quad_price = n_quad_fp), so this preset
// yields n_quad_price=24 (not 48), and max_newton_iter=8 maps to n_iter_jn=2,
// n_iter_fp=6. Same name, a different (cheaper) cost/accuracy point.
[[nodiscard]] AlOpts al_default_opts() noexcept;

// Fast ALO preset for the surface-fit hot path: {7, 16, 4, 1e-8}. Maps to a
// 7-node boundary, order-16 Gauss-Legendre for BOTH the fixed-point and premium
// integrals, and 2 Jacobi-Newton + 2 fixed-point sweeps. ~3-6x cheaper per solve
// than the ACCURATE (nullopt) preset while holding price accuracy to ~1e-4 — the
// regime American-IV inversion and correction-cache sampling actually need
// (surface RMSE is ~1e-2). This is the ALO "fast config" analogue; the session
// de-Americanizes and samples its correction cache with it by default.
[[nodiscard]] AlOpts al_fast_opts() noexcept;

// Andersen-Lake American price under Black-76 dynamics with continuous dividend
// yield q (cash divs folded into the forward beforehand).
//
// @param S,K   spot and strike (> 0)
// @param T     year-fraction to expiry (>= 0; T ~ 0 collapses to intrinsic)
// @param sigma annualized lognormal vol (>= 0; sigma ~ 0 collapses to intrinsic)
// @param r,q   continuously-compounded rate and dividend yield
// @param side  Call or Put
// @param opts  nullopt selects the ACCURATE preset (matches C `..., NULL, ...`)
// @return      the American premium, or an Error:
//                InvalidArgument — S/K <= 0, or non-finite/negative T or sigma
//                NotImplemented  — the double-continuation regime (put q < r <= 0
//                                  / call r < q <= 0), where early exercise is
//                                  possible but a second exercise boundary appears
//                                  that the single-boundary ALO scheme cannot
//                                  represent; also the negative-carry corner where
//                                  the asymptotic boundary collapses (xmax <= 0)
//                Internal        — quadrature-table construction failed
[[nodiscard]] Result<double> andersen_lake(double S, double K, double T, double sigma, double r,
                                           double q, Side side,
                                           const std::optional<AlOpts> &opts = std::nullopt);

// ── Cross-strike call-slice pricer (one boundary, many strikes) ──────────
//
// Price MANY American CALL strikes at a fixed (S, T, r, q, sigma) reusing a
// SINGLE early-exercise boundary solve. A call C(S,K,r,q) = P(K,S,q,r)
// (McDonald-Schroder), so its internal put has strike Kp = S (the fixed spot)
// and spot Sp = K_i. The Andersen-Lake boundary depends on the internal STRIKE
// (Kp = S, T, rp = q, qp = r, sigma) — identical across the call strikes — while
// K_i enters only the premium quadrature. One cold boundary solve therefore
// serves the whole slice, and `price_out[i]` is BIT-IDENTICAL to
// `andersen_lake(S, strikes[i], T, sigma, r, q, Side::Call, opts)`.
//
// This is the batch-level throughput lever for anything that prices many call
// strikes at one sigma (correction-cache sampling, scenario/what-if pricing at a
// common vol): an n_strikes-fold cut in boundary solves.
//
// Degenerate T ~ 0 / sigma ~ 0 writes intrinsic per strike; the no-early-exercise
// European corner (q <= 0 && q <= r) writes the European call per strike.
//
// @return InvalidArgument — S <= 0, negative T/sigma, a non-positive strike, or a
//                           span-length mismatch
//         NotImplemented  — the double-continuation regime (r < q <= 0), or the
//                           negative-carry corner where the asymptotic boundary
//                           collapses (matches `andersen_lake`)
[[nodiscard]] Status andersen_lake_call_slice(double S, std::span<const double> strikes, double T,
                                              double sigma, double r, double q,
                                              std::span<double> price_out,
                                              const std::optional<AlOpts> &opts = std::nullopt);

// ── Cross-strike put-slice pricer (one boundary, many strikes) ───────────
//
// Price MANY American PUT strikes at a fixed (S, T, r, q, sigma) reusing a
// SINGLE early-exercise boundary solve, via strike HOMOGENEITY. The American
// put is homogeneous of degree one in (S, K): P(S,K,...) = K·P(S/K,1,...), and
// its exercise boundary scales linearly with strike, b(τ;K) = K·b̃(τ). The
// solver stores the boundary in the K-independent coordinate y = (log(b/xmax))²
// with xmax = K·min(1,r/q), so a boundary solved at ONE reference strike is
// reused for every K_i by rescaling only (K, xmax) and re-running the cheap
// premium quadrature + European leg. One cold boundary solve serves the slice.
//
// UNLIKE the call slice (whose internal put has a FIXED strike Kp = S, so it is
// BIT-IDENTICAL to per-strike andersen_lake), the put boundary is only
// homogeneity-invariant in EXACT arithmetic: the sweep kernels carry b.K in
// absolute (non-ratio) terms, so a reference-strike boundary reused at another
// strike differs from a fresh per-strike solve by a few ULP (measured — see
// american.cpp). price_out[i] therefore matches andersen_lake(S, strikes[i], T,
// sigma, r, q, Side::Put, opts) to ~a few ULP (bit-identical AT the reference
// strike), NOT bit-for-bit across the whole ladder. Regimes route identically
// to andersen_lake (degenerate T~0/sigma~0 -> put intrinsic max(K_i-S,0);
// European r<=0&&r<=q -> Black-76 put per strike; Unsupported -> NotImplemented).
//
// This is the batch throughput lever for anything that prices many put strikes
// at one sigma (put portfolio ladders, the sigma-Chebyshev boundary primitive):
// an n_strikes-fold cut in boundary solves.
//
// @return InvalidArgument — S <= 0, negative T/sigma, a non-positive strike, or a
//                           span-length mismatch
//         NotImplemented  — the double-continuation regime (q < r <= 0), or the
//                           negative-carry corner where the asymptotic boundary
//                           collapses (matches `andersen_lake`)
[[nodiscard]] Status andersen_lake_put_slice(double S, std::span<const double> strikes, double T,
                                             double sigma, double r, double q,
                                             std::span<double> price_out,
                                             const std::optional<AlOpts> &opts = std::nullopt);

// ── σ-axis Chebyshev boundary interpolation (fitted-smile board, P2.5) ───
//
// A fitted-smile slice carries a DIFFERENT σ per strike, so andersen_lake_*_slice
// (one σ, one boundary) does not apply — today it costs one cold Andersen-Lake
// boundary solve per strike. The *dimensionless* boundary y[] depends only on
// (σ, r, q, τ), NOT on strike, and is smooth in σ. andersen_lake_{put,call}_slice
// _sigma therefore builds ONE σ-Chebyshev interpolant of y[] per (expiry, r, q)
// group — n_σ cold solves shared by the whole ladder — and prices each strike by
// interpolating y[] at that strike's σ, rescaling to its K (strike homogeneity),
// and running the existing premium quadrature. Calls go through the same object
// via the McDonald-Schroder internal-put map.
//
// GATED (Task 11 / sprint §P2.5): the interpolant introduces a boundary
// approximation, so it is OPT-IN. With `use_sigma_boundary_interp == false` (the
// default) every strike takes the cold per-strike solve — BIT-IDENTICAL to
// andersen_lake(S, K_i, T, σ_i, r, q, side) — so the flag-off route is exactly
// the scalar reference. With the flag ON, strikes whose σ clears the box + guards
// take the interpolant; any strike outside the σ box, below the small-σ guard, or
// (whole slice) below the near-expiry τ guard takes the cold solve, tagged
// ColdFallback (bit-identical to the direct solve). See at-task-11-report.md for
// the measured accuracy/throughput and the ship/flag decision.
struct SigmaInterpOptions {
  // Master flag. SHIPPED default ON (Task 11): the interpolant cleared the §P2.5
  // ship gate on a real SPY board — 8.1x board-price throughput, all §9 gates
  // green (max price gap 3.8e-5 vs cold over 51,755 corpus strikes, δ gap 3e-7,
  // 0% ColdFallback). OFF forces the cold per-strike solve for every strike —
  // BIT-IDENTICAL to andersen_lake(S, K_i, T, σ_i, r, q, side) — the reference.
  bool use_sigma_boundary_interp = true;
  std::uint16_t n_sigma = 8;    // σ Chebyshev-Lobatto nodes (cold solves)
  double sigma_lo = 0.0;        // σ box lower bound; <= 0 => auto = min over in-guard σ
  double sigma_hi = 0.0;        // σ box upper bound; <= 0 => auto = max over in-guard σ
  double min_tau = 3.0 / 365.0; // near-expiry guard: whole slice cold below this τ
  double min_sigma = 0.01;      // small-σ guard: that strike cold below this σ
};

// Diagnostics from a *_slice_sigma call (optional out-param).
struct SigmaSliceStats {
  std::size_t n_strikes = 0;
  std::size_t n_boundary_solves = 0; // n_σ build solves (+ one per ColdFallback)
  std::size_t n_cold_fallback = 0;   // strikes routed to the cold solve
  std::size_t n_interp = 0;          // strikes priced through the interpolant
  std::uint16_t n_sigma = 0;         // σ-nodes actually used (0 when not built)
  double sigma_lo = 0.0;
  double sigma_hi = 0.0;
  bool used_interp = false; // the interpolant was built for this slice
};

// Price MANY American PUT strikes, each at its OWN σ_i, at a fixed (S, T, r, q).
// See SigmaInterpOptions. Regimes/degenerate handling route exactly like
// andersen_lake_put_slice (degenerate T~0/σ~0 -> intrinsic; European r<=0&&r<=q
// -> Black-76 put per strike; Unsupported -> NotImplemented).
//
// @return InvalidArgument — S <= 0, negative T, a non-positive strike, a negative
//                           σ, a span-length mismatch, or non-finite r/q
//         NotImplemented  — the double-continuation corner, or a boundary collapse
[[nodiscard]] Status andersen_lake_put_slice_sigma(double S, std::span<const double> strikes,
                                                   std::span<const double> sigmas, double T,
                                                   double r, double q, std::span<double> price_out,
                                                   const SigmaInterpOptions &sopts = {},
                                                   const std::optional<AlOpts> &opts = std::nullopt,
                                                   SigmaSliceStats *stats = nullptr);

// Price MANY American CALL strikes, each at its OWN σ_i, via the McDonald-Schroder
// internal-put map. See andersen_lake_put_slice_sigma / SigmaInterpOptions.
[[nodiscard]] Status andersen_lake_call_slice_sigma(
    double S, std::span<const double> strikes, std::span<const double> sigmas, double T, double r,
    double q, std::span<double> price_out, const SigmaInterpOptions &sopts = {},
    const std::optional<AlOpts> &opts = std::nullopt, SigmaSliceStats *stats = nullptr);

// Barone-Adesi-Whaley American approximation.
//
// @param max_iter critical-price root-find iteration cap (0 -> 16)
// @param tol      critical-price convergence tolerance (<= 0 -> 1e-8)
// @return         the American premium, or an Error:
//                   InvalidArgument — S/K <= 0, or negative T or sigma
//                   Unavailable     — the critical-price root-find diverged
[[nodiscard]] Result<double> baw_american(double S, double K, double T, double sigma, double r,
                                          double q, Side side, std::uint16_t max_iter = 16,
                                          double tol = 1.0e-8);

// ── Warm-started ALO pricer (fixed contract, sigma sweep) ────────────────
//
// The throughput lever for American-IV inversion. The Andersen-Lake
// early-exercise boundary depends on (K, T, r, q, sigma) but NOT on spot S (S
// enters only the premium quadrature), and the node grid + Gauss-Legendre tables
// depend on none of (S, sigma). So for a FIXED contract (S, K, T, r, q, side) an
// implied-vol root-find sweeps only sigma: the node grid is built ONCE, and each
// price at a new sigma warm-starts the boundary from the previous sigma — one or
// two Jacobi-Newton sweeps instead of a cold solve (a fresh `al_seed_boundary` is
// 12 Barone-Adesi-Whaley critical-price root-finds plus ~6 sweeps). This is the
// single biggest cost in the inversion, so reusing it is a multiple-x speedup at
// identical output.
//
// `price(sigma)` reproduces `andersen_lake(S, K, T, sigma, r, q, side, opts)` to
// the boundary tolerance (a warm and a cold solve converge to the same boundary).
// Mutable warm state — NOT thread-safe; retain one per thread/inversion. A large
// sigma jump (or the first call) transparently falls back to a cold seed.
class AloPricer {
public:
  AloPricer(double S, double K, double T, double r, double q, Side side,
            const std::optional<AlOpts> &opts = std::nullopt);
  ~AloPricer();
  AloPricer(AloPricer &&) noexcept;
  AloPricer &operator=(AloPricer &&) noexcept;
  AloPricer(const AloPricer &) = delete;
  AloPricer &operator=(const AloPricer &) = delete;

  // Rebind this retained state to another fixed contract without allocating.
  // The next price() starts from a cold boundary seed; no state from the prior
  // contract or accuracy scheme is consumed. Not thread-safe, like price().
  void reset(double S, double K, double T, double r, double q, Side side,
             const std::optional<AlOpts> &opts = std::nullopt) noexcept;

  // American price at this contract and `sigma` (>= 0). Warm-starts the boundary
  // from the previous call. Returns NaN on the negative-rate/carry corners where
  // andersen_lake returns NotImplemented: the double-continuation regime
  // (put q < r <= 0 / call r < q <= 0) and the asymptotic boundary collapse.
  // A degenerate sigma ~ 0 or T ~ 0 returns intrinsic regardless of regime.
  [[nodiscard]] double price(double sigma) noexcept;

private:
  struct State;
  std::unique_ptr<State> st_;
};

// Cold-path method selector for the unified entry point.
enum class AmericanMethod : std::uint8_t {
  AndersenLake = 0,
  Baw = 1,
};

// Unified cold pricer. `opts` is honoured only for the Andersen-Lake method.
[[nodiscard]] Result<double> american_price(double S, double K, double T, double sigma, double r,
                                            double q, Side side,
                                            AmericanMethod method = AmericanMethod::AndersenLake,
                                            const std::optional<AlOpts> &opts = std::nullopt);

// Hot-path cached American price: Black-76 + F·correction(k_log, T, sigma) with
// F = S·e^{(r-q)T} and k_log = ln(K/F). A null, unpopulated, or opposite-side
// correction falls back to the cold Andersen-Lake path, returning NaN if that
// solver fails.
//
// Mirrors the C `ats_pricer_american_routed(..., correction, NULL, NULL)`.
[[nodiscard]] double american_price_cached(double S, double K, double T, double sigma, double r,
                                           double q, Side side, const CorrectionCache *correction);

// Same cached graph with an explicit call-constant blend of two fixed-carry
// correction caches. Invalid blends degrade to the cold Andersen-Lake route;
// exact weights 0/1 evaluate only the selected endpoint cache.
[[nodiscard]] double american_price_cached(double S, double K, double T, double sigma, double r,
                                           double q, Side side, const CorrectionBlend &correction);

// ── American Greeks ─────────────────────────────────────────────────────
//
// First-order (delta, vega, rho, theta) are exact chain-rule on the Black-76
// closed forms plus analytic correction partials. Second-order
// (gamma, vanna, volga, charm) differentiate the correction interpolant in the
// same fused Clenshaw traversal, with no off-point finite-difference stencil.
//
// theta uses the calendar-time convention: ∂P/∂t = -∂P/∂T.
struct AmericanGreeks {
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0;
  double rho = 0.0;
  double vanna = 0.0;
  double volga = 0.0;
  double charm = 0.0;
  double price = 0.0;

  [[nodiscard]] bool operator==(const AmericanGreeks &) const = default;
};

// American Greeks + price. A null, unpopulated, or opposite-side `correction`
// degrades to the Black-76 leg (American -> European).
//
// Degenerate-input policy: this SURFACES an error, deliberately asymmetric with
// `american_vega`, which returns a 0.0 sentinel on the same input. The
// difference is intentional — `american_greeks` has no sentinel consumer, so it
// reports the error; `american_vega`'s 0 is a signal the IV inverter depends on.
//
// @return InvalidArgument if any of S, K, T, sigma is non-positive.
[[nodiscard]] Result<AmericanGreeks> american_greeks(double S, double K, double T, double sigma,
                                                     double r, double q, Side side,
                                                     const CorrectionCache *correction);

// Blended fixed-carry cache route. `correction.upper_weight` remains constant
// through every derivative so theta/charm do not absorb carry-curve slope.
[[nodiscard]] Result<AmericanGreeks> american_greeks(double S, double K, double T, double sigma,
                                                     double r, double q, Side side,
                                                     const CorrectionBlend &correction);

// American Greeks via central finite differences on the cold `american_price`
// (same method/opts the mark is priced with). Used on the null-correction-cache
// path so that greeks().price == fair_value() bit-identical and the coefficients
// are American (not European). InvalidArgument on non-positive S/K/T/sigma;
// propagates any american_price error (e.g. NotImplemented negative-carry corner).
//
// The AndersenLake put path exploits the spot-independence of the exercise boundary:
// the 17 stencils collapse to the 7 unique (sigma,r,T) boundaries. With
// `warm_start` the 6 bumped boundaries are seeded from the converged base boundary
// (skipping the dominant cold Barone-Adesi-Whaley re-seed) and reconverge to the
// same tol — same greeks to ~tol, several-fold faster. `warm_start = false` keeps
// every boundary cold (the exact FD reference; greeks().price stays bit-identical
// to fair_value()). Calls and the BAW method always take the cold 17-solve path.
[[nodiscard]] Result<AmericanGreeks>
american_greeks_fd(double S, double K, double T, double sigma, double r, double q, Side side,
                   AmericanMethod method = AmericanMethod::AndersenLake,
                   const std::optional<AlOpts> &opts = std::nullopt, bool warm_start = false);

// American Greeks in FIVE Andersen-Lake boundary solves instead of the FD path's
// seven. The exercise boundary is spot-independent, so delta/gamma are exact finite
// differences over the base boundary (no extra solve); vega/rho/vanna/volga re-solve
// the sigma+/- and r+/- boundaries (the boundary genuinely moves with sigma/r — the
// frozen-boundary/envelope shortcut is NOT valid for the AL premium decomposition);
// theta/charm come from the continuation-region Black-Scholes PDE
// (theta = rV - (r-q)S*delta - 0.5*sigma^2*S^2*gamma; charm = d theta/dS), which
// DROPS the two time-bumped solves and is more accurate (no time-bump truncation).
// A pure function of the inputs, so bit-reproducible across a surface-archive
// round-trip.
//
// vs american_greeks_fd: price + delta/gamma/vega/rho/vanna/volga are bit-identical
// (same boundaries), theta/charm differ (the PDE is the exact continuation-region
// value, gated close to the FD reference). Puts with genuine early exercise (r>0,
// non-degenerate) only; calls, the r<=0 European put, the degenerate corners, and
// any bumped-boundary collapse defer to american_greeks_fd. greeks().price ==
// fair_value(). InvalidArgument on non-positive S/K/T/sigma.
[[nodiscard]] Result<AmericanGreeks>
american_greeks_al(double S, double K, double T, double sigma, double r, double q, Side side,
                   const std::optional<AlOpts> &opts = std::nullopt);

// American delta ONLY (∂price/∂S) — the single sensitivity the strike-from-delta
// solver's bisection consumes, WITHOUT the full american_greeks_fd bundle's other
// eight axes (which cost 17 boundary solves). The put/AndersenLake fast lane
// exploits the spot-independent exercise boundary: ONE boundary solve + two
// price-from-boundary spot stencils, BIT-IDENTICAL to american_greeks_fd's delta.
// Calls / BAW / degenerate corners take a two-evaluation central difference on the
// cold american_price — the same stencil (and value) american_greeks_fd uses, at
// two solves instead of seventeen. So a delta-driven root-find (resolve_strike_by_
// delta) that repriced full greeks per candidate now solves ~1-2 boundaries per
// candidate at an unchanged strike. InvalidArgument on non-positive S/K/T/sigma;
// propagates any american_price error.
[[nodiscard]] Result<double> american_delta(double S, double K, double T, double sigma, double r,
                                            double q, Side side,
                                            AmericanMethod method = AmericanMethod::AndersenLake,
                                            const std::optional<AlOpts> &opts = std::nullopt);

// Cached delta-only route: Black-76 forward delta plus the correction's value
// and k_log derivative. It avoids constructing the full second-order Greek jet
// consumed by american_greeks and keeps blend weights call-constant.
[[nodiscard]] Result<double> american_delta(double S, double K, double T, double sigma, double r,
                                            double q, Side side, const CorrectionBlend &correction);

// American vega ONLY (∂price/∂sigma) — the single first-order sensitivity the IV
// inverter's Newton step needs, WITHOUT the full `american_greeks` bundle's
// second-order correction jet (gamma/vanna/volga/charm). It is the Black-76
// vega plus F·∂correction/∂sigma (one cache `eval_grad`).
// Null, unpopulated, and opposite-side caches give the Black-76 (European-leg)
// vega. Returns 0 on a degenerate / non-positive input
// (the inverter reads 0 as "force bisection"). This 0 sentinel is LOAD-BEARING
// and is why this function differs from `american_greeks`, which surfaces
// InvalidArgument on the same input rather than a sentinel.
[[nodiscard]] double american_vega(double S, double K, double T, double sigma, double r, double q,
                                   Side side, const CorrectionCache *correction) noexcept;

// Call-constant two-cache blend variant. Uses only each endpoint's sigma
// partial; exact endpoints preserve the single-cache path.
[[nodiscard]] double american_vega(double S, double K, double T, double sigma, double r, double q,
                                   Side side, const CorrectionBlend &correction) noexcept;

// ─────────────────────────────────────────────────────────────────────────
// C1.7 (additive-only; see FILE-OWNERSHIP RULE in the 07-09 sprint doc) — a
// vega-ONLY entry point for `PricedSurface::vega`, bit-identical to
// `american_greeks_al(...).vega` on every input where the AL bundle takes its
// native analytic route (and to `american_greeks_fd(...).vega` on every input
// where the bundle falls back to FD), at 0-2 boundary solves instead of the
// bundle's 5 (see american_greeks_al's cost comment above). `american_vega`
// (just above) is NOT reusable here: it is the Black-76-plus-correction-cache
// hot-path vega built for the IV inverter's 0.0-sentinel contract, a
// DIFFERENT numerical procedure (no AL boundary re-solve at all) from the
// bundle's re-solved-boundary finite difference, and it returns a plain
// double with a load-bearing 0-on-degenerate sentinel rather than a `Result`
// with delta()'s InvalidArgument contract. See american.cpp for the
// boundary-re-solve mirror of american_greeks_al's vega branch.
//
// @return InvalidArgument on non-positive S/K/T/sigma (delta()'s error
//         contract, NOT american_vega's 0.0 sentinel); otherwise the vega,
//         bit-identical to greeks_analytic(...).vega for the same inputs.
[[nodiscard]] Result<double> american_vega_al(double S, double K, double T, double sigma, double r,
                                              double q, Side side,
                                              const std::optional<AlOpts> &opts = std::nullopt);
// ─────────────────────────────────────────────────────────────────────────

namespace detail {

// ── Early-exercise regime classification (Healy 2021 §2.2) ───────────────
//
// SINGLE SOURCE OF TRUTH for the negative-rate early-exercise regime table.
// Every pricing entry point in the library (american.cpp) and the
// correction-cache populator (correction.cpp) classifies through this one
// function so the regime boundaries are encoded in exactly one place.
//
// Under the McDonald-Schroder map C(S,K,r,q) = P(K,S,q,r), a call delegates to
// the internal put with (rate=q, yield=r), so BOTH sides reduce to an internal
// put characterized purely by its (rate, yield):
//   - European    : early exercise is never optimal, so American == European
//                   EXACTLY (rate <= 0 && rate <= yield).
//   - Unsupported : early exercise IS possible but a double continuation region
//                   appears (rate <= 0 && rate > yield, i.e. yield < rate <= 0);
//                   the single-boundary ALO scheme cannot represent two exercise
//                   boundaries (Battauz-De Donno-Sbuelz 2015, Mgmt Sci 61(5);
//                   Andersen-Lake 2021). Callers return NotImplemented / NaN
//                   rather than a silently-wrong European price.
//   - American    : the standard single-boundary early-exercise regime (rate > 0).
enum class ExerciseRegime : std::uint8_t { European, Unsupported, American };

[[nodiscard]] inline ExerciseRegime classify_regime(double rate, double yield) noexcept {
  if (rate > 0.0) {
    return ExerciseRegime::American;
  }
  return (rate <= yield) ? ExerciseRegime::European : ExerciseRegime::Unsupported;
}

// Max Gauss-Legendre order the AL quadrature supports (matches C ATS_AL_MAX_QUAD).
inline constexpr unsigned kMaxQuadNodes = 64;

// Gauss-Legendre nodes/weights on [-1, 1], generated via Golub-Welsch
// (eigendecomposition of the symmetric tridiagonal Jacobi matrix). Exposed so
// the quadrature constants can be locked directly in tests. Supported orders:
// {8, 16, 24, 32, 48, 64}; an unsupported order returns `ok == false`.
struct GaussLegendre {
  std::array<double, kMaxQuadNodes> nodes{};
  std::array<double, kMaxQuadNodes> weights{};
  unsigned n = 0;
  bool ok = false;
};

[[nodiscard]] GaussLegendre gauss_legendre(unsigned n);

// Test-only seam (P2.2 §3). Identical to `andersen_lake` but forces the GENERIC
// runtime-trip-count boundary kernel even for a scheme that has a specialized
// compile-time-trip-count instantiation. Used by BoundaryHoist_SpecializedMatches
// Generic to prove the specialized fixed-scheme kernel is bit-identical to the
// generic path. Not part of the production API.
[[nodiscard]] Result<double>
andersen_lake_generic_kernel(double S, double K, double T, double sigma, double r, double q,
                             Side side, const std::optional<AlOpts> &opts = std::nullopt);

// P2.2b spike seed for al_boundary_jn_sweeps_to_converge.
enum class AlSeedMode : std::uint8_t { Baw = 0, QdPlus = 1, Oracle = 2 };

// Test/measurement-only (P2.2b). Cold-solve the put boundary for (K,T,sigma,r,q)
// with the requested seed, then count Jacobi-Newton sweeps until the boundary
// residual (max |Δy|) first falls to <= tol, capped at max_sweeps. Returns the
// sweep count, or -1 if the boundary collapses / a table is missing. Used by the
// QD+ vs BAW seed-count spike; NOT a production entry point.
[[nodiscard]] int al_boundary_jn_sweeps_to_converge(double K, double T, double sigma, double r,
                                                    double q, const std::optional<AlOpts> &opts,
                                                    AlSeedMode seed, double tol, int max_sweeps);

// ── P2.3 (research spike): warm-start the boundary ACROSS TIME ────────────
//
// Price ONE American PUT contract (fixed strike K, spot S, yield q) over a temporal
// sequence of `n_snap` snapshots: T decays by `dT` and (sigma, r) drift by
// (dsigma, dr) per step from (T0, sigma0, r0). Each snapshot solves the boundary
// TWICE and prices from each:
//   * COLD  — a fresh Barone-Adesi-Whaley reseed every snapshot (the reference).
//   * WARM  — seeded from the PREVIOUS snapshot's converged dimensionless boundary
//             (remapped across the new residual T by al_boundary_at), UNLESS the
//             move guard |dsigma/sigma| or |dT/T| exceeds `move_guard_frac` (or the
//             warm seed's first-sweep residual fails the trend check), which forces
//             a cold reseed so warm is never worse than cold.
// Both runs execute the production two-stage sweep sequence (JN then FP) with an
// early-exit at the scheme tol; the per-stage caps are the scheme's fixed budget
// when `converge_to_tol` is false, or raised to `max_sweeps` when true (so the cost
// of reaching tol from each seed is visible). The per-snapshot EXECUTED JN+FP sweep
// counts are appended to `cold_sweeps` / `warm_sweeps`; `warm_hits` / `cold_reseeds`
// accumulate; `max_price_gap` tracks max |warm_price − cold_price| over the sequence.
// Returns false (nothing appended) for a collapsed / European / degenerate corner.
[[nodiscard]] bool al_temporal_warm_probe(double S, double K, double q, double T0, double sigma0,
                                          double r0, double dT, double dsigma, double dr,
                                          int n_snap, const std::optional<AlOpts> &opts,
                                          bool converge_to_tol, int max_sweeps,
                                          double move_guard_frac, std::vector<int> &cold_sweeps,
                                          std::vector<int> &warm_sweeps, int &warm_hits,
                                          int &cold_reseeds, double &max_price_gap) noexcept;

// ── P2.4 (research spike): implicit boundary-differentiation greeks ───────
//
// American PUT greeks from ONE converged early-exercise boundary by differentiating
// the collocation fixed point implicitly. At convergence the pure collocation
// residual R(y; θ) = 0 (θ = σ, r); J = ∂R/∂y (n×n) and R_θ = ∂R/∂θ are formed, and
// J·y_θ = −R_θ is solved by dense pivoted LU. The boundary sensitivities y_σ, y_r
// are propagated through the SAME premium quadrature (a moving-boundary central
// difference — the boundary is DIFFERENTIATED, never frozen) to get vega / rho;
// theta comes from the continuation-region Black-Scholes PDE (as american_greeks_al).
// delta / gamma are exact spot stencils on the (spot-independent) base boundary.
//
// The Jacobian is built by central differences of the pure residual R with a checked
// step (per the brief). When `validate` is true the base boundary is converged
// tightly and the implicit-diff sensitivity y_σ is cross-checked against a
// finite-difference of the RE-SOLVED boundary (the ground-truth ∂y/∂σ); the max
// relative gap is returned in `j_max_rel_err` (0 when not validating — the cheap
// production path). `cost_passes` returns the number of full-node residual-equivalent
// passes the implicit-diff step costs ON TOP of the base solve (the boundary-
// equivalent numerator, validation re-solves excluded); `base_sweeps` is the base
// solve's executed JN+FP sweeps. Genuine early-exercise puts only (r > 0,
// non-degenerate); `ok == false` on any other corner.
struct ImplicitDiffGreeks {
  double price = 0.0;
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double rho = 0.0;
  double theta = 0.0;
  double vanna = 0.0;
  double charm = 0.0;
  double volga = 0.0;
  int cost_passes = 0; // residual-equivalent passes beyond the base solve
  int base_sweeps = 0; // base solve executed JN+FP sweeps
  int n_boundary = 0;  // collocation nodes (LU dimension)
  bool ok = false;
};
[[nodiscard]] ImplicitDiffGreeks al_implicit_diff_put_greeks(double S, double K, double T,
                                                             double sigma, double r, double q,
                                                             const std::optional<AlOpts> &opts,
                                                             bool validate,
                                                             double &j_max_rel_err) noexcept;

} // namespace detail

} // namespace atx::vol
