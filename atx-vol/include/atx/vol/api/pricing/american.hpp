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
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "atx/vol/api/fitting/aggregate_arity.hpp" // AlOpts field-count drift pin
#include "atx/vol/api/core/types.hpp"

namespace atx::vol {

// Defined in correction.hpp — observed (non-owning) by the cached pricer/Greeks.
class CorrectionCache;
struct CorrectionBlend;

// ── Andersen-Lake tuning knobs ──────────────────────────────────────────
//
// Mirrors the C `AtsVolALOpts`. The public knobs are mapped internally to a
// QuantLib-style accuracy preset (boundary nodes, fixed-point / premium
// quadrature orders, and sweep counts).
//
// CONSTRUCTION CONTRACT (v1, plan item 4.2) — DESIGNATED INITIALIZERS ONLY:
//
//   AlOpts{.n_collocation = 7, .n_quadrature = 8, .n_quad_price = 32,
//          .max_newton_iter = 2, .tol = 1.0e-8}
//
// Positional aggregate init (`AlOpts{7, 8, 2, 1.0e-8}`) is no longer a supported
// form. `n_quad_price` used to be pinned to the END of the struct purely so
// existing 4-argument positional initializers kept compiling — which made
// declaration ORDER part of the public API and condemned every future knob to
// live away from the field it belongs with. It now sits beside the fixed-point
// order it decouples from, and the field-count `static_assert` below turns a
// silent re-append into a compile error.
//
// Tier-A (frozen v1 surface): this reorder is the LAST layout change allowed.
// After v1 the order is fixed and new knobs append at the end WITHOUT any
// positional-compatibility promise — designated init is the only contract.

// Andersen-Lake kernel arithmetic profile (al-speed lane, 2026-10-17).
//
//   Reference  the shipped kernel; every pinned hash (dev200 c89d0739 etc.) is on it.
//   Fast       OPT-IN. The SAME scheme — collocation nodes, quadrature orders, sweep
//              schedule, BAW seed, every guard and clamp — with three substitutions
//              that are identities in exact arithmetic:
//                (1) Phi / phi by the validated scalar Cody rational kernels
//                    (pricing/scalar_erfc.hpp, ~1e-16 vs std::erfc) instead of
//                    std::erfc, which costs ~20 ns/call on the reference host;
//                (2) the boundary ratio in log space: with b = xmax*exp(-sqrt(y)),
//                    log(b_i/b(u)) = sqrt(y(u)) - sqrt(y_i) and
//                    log(S/b(t)) = log(S/xmax) + sqrt(y(t)) — one exp and one log
//                    fewer per quadrature node;
//                (3) the BAW seed shares one d1 between its Black-76 leg and its
//                    smooth-pasting term (2 Phi per Newton step instead of 3).
//              The differences from Reference are rounding-level. MEASURED, Fast vs
//              Reference on the SAME scheme (accurate / fast / bulk), worst case over
//              the AL ladder grid, a 6,000-contract random set and 10,694 dev200
//              contracts (bench rows american/fast_profile_error/*; numbers in
//              sprints/2026-10-17-al-speed-sprint.md). Convention: each value is the
//              measured worst case (max, or p99 where stated) to 5 significant
//              figures; the BOUND in brackets is its next 2-figure round-up, so every
//              bound is >= its measurement:
//                price   max |dP| 7.9581e-13 abs [8.0e-13]; max relative (P >= 0.01)
//                        1.8534e-13 [1.9e-13]
//                IV      max 5.3660e-3 bp [0.0054]; p99 1.9638e-8 bp [2.0e-8] (one
//                        target price inverted under each profile, tol 1e-10)
//                Greeks  p99 |d| (american_greeks_al): delta 1.7941e-13 [1.8e-13],
//                        gamma 1.6880e-11 [1.7e-11], vega 1.4211e-10 [1.5e-10],
//                        theta 5.4645e-8 [5.5e-8], rho 1.4921e-9 [1.5e-9],
//                        vanna 1.2206e-10 [1.3e-10], volga 5.1159e-7 [5.2e-7],
//                        charm 1.4977e-7 [1.5e-7]
//              TESTED bound (AlFastProfile.* in tests/american_test.cpp):
//                |dP| <= 1e-12 * max(P, 1); |dIV| <= 0.01 bp on vega-bearing
//                contracts; per-Greek p99 bounds ~100x the measured p99; the call
//                slice stays bit-identical to the scalar Fast call.
//              Not adopted by the AVX2 batch kernel (its own vector math) or by the
//              adjoint tape (it differentiates the Reference map; its bumped
//              re-solves pin Reference too).
//   Screen     OPT-IN, never a default (L2f al-fast-tier, 2026-10-19). A cheap SCREENING
//              tier for inversion seeds / iterations that an accurate confirm finishes:
//              the Fast arithmetic above on a SMALL scheme. It is not a rounding-level
//              twin of Reference — it truncates the scheme — so its contract is the
//              measured truncation bound documented on al_screen_opts() and pinned by
//              AlScreenProfile.* (tests/american_test.cpp). Under Screen the explicit
//              knobs are honoured (scheme_from_opts) with ONE difference: the boundary
//              node floor is 4 instead of 6, so a caller may ask for an even cheaper
//              screen (3 interior nodes) than the measured one; only al_screen_opts()
//              carries a measured bound. Same exclusions as Fast (AVX2 batch, adjoint
//              tape); the carry memo and the de-Am IV cache key already include it.
enum class AlProfile : std::uint8_t {
  Reference = 0,
  Fast = 1,
  Screen = 2,
};

struct AlOpts {
  std::uint16_t n_collocation = 12; // Chebyshev boundary nodes
  std::uint16_t n_quadrature = 24;  // Gauss-Legendre fixed-point order
  // Premium (pricing) Gauss-Legendre order, DECOUPLED from the fixed-point order.
  // 0 (the default) ties it to n_quadrature — the historical behavior, so every
  // existing in-memory / serialized AlOpts resolves to the SAME scheme (K2,
  // class: pure-refactor). A non-zero value expresses QuantLib QdFpAmericanEngine's
  // l != p axis: a cheap boundary-locating fixed-point quadrature (l = n_quadrature)
  // paired with a rich final pricing quadrature (p = n_quad_price). The fixed-point
  // integral runs l*n*sweeps times per solve while the pricing integral runs once,
  // so pricing accuracy is cheap and fixed-point cost is where the time is — the
  // "ql_fast" rung (n_quadrature=8, n_quad_price=32) is ~1.8x cheaper than the
  // tied fast preset at equal accuracy. Quantized to {8,16,24,32,48,64}. See
  // docs/al-preset-ladder.md (QuantLib QdFpLegendreScheme; ALO SSRN 2547027).
  // NOTE: the surface archive decomposes AlOpts field-by-field, so this knob
  // round-trips only through archives/manifests that carry `al_n_quad_price`;
  // anything written before that column existed reads back as 0 (tied).
  std::uint16_t n_quad_price = 0;
  std::uint16_t max_newton_iter = 8; // total Jacobi-Newton + fixed-point sweeps
  double tol = 1.0e-10;              // convergence tol on the boundary residual
  // Kernel arithmetic profile (al-speed, 2026-10-17; appended per the v1 contract
  // above). Reference — the default — is the shipped arithmetic, byte for byte.
  // Fast is OPT-IN: see AlProfile for what it changes and its measured bound.
  // Like n_quad_price before it, this knob is runtime-only: no AlOpts record format
  // (surface_db / surface_archive / priced_surface_view) persists it, so a stored
  // config always reads back Reference.
  AlProfile profile = AlProfile::Reference;
};

// Drift pin (plan item 4.2). AlOpts has exactly SIX fields (the sixth, `profile`,
// appended 2026-10-17). Adding, removing or splitting one breaks this line, which
// is the point: it forces whoever changes the struct to read the construction
// contract above instead of appending a knob "for compatibility" with positional
// initializers that are no longer part of the API.
static_assert(detail::aggregate_arity_is_v<AlOpts, 6>,
              "AlOpts field count changed: update this pin, and confirm every "
              "construction site still uses designated initializers.");

// `opts` resolved to an EXPLICIT AlOpts that selects the same scheme with
// `profile = AlProfile::Fast`. nullopt (the ACCURATE preset) resolves to its
// explicit equivalent {12, 24, price 48, 6 sweeps, 1e-10}, which maps to exactly
// the nullopt scheme, so this is the one way to ask for the accurate preset on the
// fast kernel. The opt-in "policy key" for callers that thread AlOpts.
[[nodiscard]] AlOpts al_fast_profile(const std::optional<AlOpts> &opts) noexcept;

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

// The `ql_fast` rung of docs/al-preset-ladder.md §4: {7, 8, 2, 1e-8} with the
// DECOUPLED premium order n_quad_price = 32. A cheap boundary-locating fixed-point
// quadrature (l = 8, half of al_fast_opts's 16) over only 2 sweeps (vs 4) drops the
// dominant n_quad_fp x n_boundary x n_sweeps work from 448 node evaluations per
// solve to 112 (-75%), while the rich decoupled premium (p = 32) holds price
// accuracy at ~1.0e-3 — statistically the same as al_fast_opts's measured 9.7e-4.
// The ladder measured it at 25.8 us/op vs al_fast_opts's 46.7 us/op (1.81x) while
// still paying the generic-kernel tax; (7,8) is specialized in `al_fp_specialized`
// now, so the realized gap is wider.
//
// The ladder's §5 tier policy names this rung for the fit-de-Americanization /
// IV-inversion tier and for correction-cache sampling. It is NOT a serving or
// oracle preset, and — because `n_quad_price` is not persisted by any of the three
// AlOpts record formats — it must never be baked into a stored pricing config; see
// DeAmOptions::serve_al_opts (deamer.hpp).
[[nodiscard]] AlOpts al_bulk_opts() noexcept;

// The measured AlProfile::Screen tier (L2f al-fast-tier, 2026-10-19). OPT-IN only: no
// default, preset or stored config selects it. See AlProfile::Screen.
//
// Scheme: 7 Chebyshev nodes, fixed-point Gauss-Legendre l = 16, 5 sweeps (2 Jacobi-Newton
// + 3 fixed-point, early exit at tol 1e-8), premium Gauss-Legendre p = 48, on the Fast
// arithmetic. Chosen from a 27-point (nodes, l, sweeps, p, tol) scan (bench rows
// american/screen_scan/*; sprints/2026-10-19-L2f-al-fast-tier-sprint.md) as the most
// accurate point under ~20 us per solve + price; the accurate Reference preset is
// {12, 24, 6 sweeps, 48}.
//
// MEASURED vs the ACCURATE Reference (nullopt), |P_screen - P_ref| (USD of the set's spot;
// relative on P_ref >= 0.01):
//   SPY 2026-11-06 chain (23 d, 317 quotes, sigma = each mid's Reference IV):
//       max abs 2.65e-5, max rel 1.17e-6; IV p99 0.018 bp, max 0.028 bp
//   dev200 kernel inputs (10,694 contracts):
//       max abs 8.70e-3 (put S 1711.8, K 2900, T 2.17), max rel 1.48e-5;
//       IV p99 0.043 bp, max 0.73 bp (vega-bearing)
//   AL ladder grid (104): max abs 2.06e-5, max rel 2.56e-6
//   6,000-contract random set (T to 5 y, q to 25 %, sigma to 1.5):
//       max abs 4.86e-3, max rel 2.0e-3 (on P >= 0.01), p99 rel 6.8e-5
// TESTED bound (AlScreenProfile.* in tests/american_test.cpp, 1,404-contract grid):
//   |dP| <= 7.8e-4 * max(P, 1) everywhere, <= 4.6e-6 * max(P, 1) for T <= 0.1;
//   |dIV| <= 0.48 bp on vega-bearing contracts.
// A caller using Screen for an inversion seed / screen must finish with an accurate
// confirm (or widen any certificate by at least the bound above): Screen is NOT a
// rounding-level twin of Reference the way Fast is.
[[nodiscard]] AlOpts al_screen_opts() noexcept;

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

// ── Shared expiry geometry (L2b al-geometry, 2026-10-18) ─────────────────
//
// Every cold Andersen-Lake solve rebuilds its expiry's Chebyshev node grid and the
// sweep-invariant quadrature geometry (the Gauss-Legendre node/weight products,
// the barycentric table, exp(r u) / exp(q u) weights), although none of it depends
// on the strike or the volatility. An AlExpiryGeometry prepares that state ONCE per
// (T, r, q, opts) — for the puts' orientation (rate r, yield q) and for the calls'
// McDonald-Schroder internal put (rate q, yield r) — and the overloads below reuse
// it. They are BIT-IDENTICAL to the fresh entry points: same validation, same
// errors, same regimes, and every value the kernel reads is computed from the same
// operands by the same expressions (one multiply by the strike replaces the bind's
// own). Covered by tests AlExpiryGeometry.* / AlBoundaryHandle.* / AlPricePack.*
// (tests/american_geometry_test.cpp).
//
// The cache key is the WHOLE tuple (T, r, q, opts — scheme and AlProfile
// included): a geometry is never consulted for another expiry, carry or kernel
// profile, and a prepare() rebuilds it completely.
//
// OWNERSHIP / THREADS. Move-only owner of ~90 KB of prepared tables. Immutable
// between prepare() calls: any number of threads may price through one const
// geometry concurrently; prepare() and destruction need exclusive access. No global
// mutable table is involved (the Gauss-Legendre tables are process-wide constants).
class AlExpiryGeometry {
public:
  AlExpiryGeometry();
  ~AlExpiryGeometry();
  AlExpiryGeometry(AlExpiryGeometry &&) noexcept;
  AlExpiryGeometry &operator=(AlExpiryGeometry &&) noexcept;
  AlExpiryGeometry(const AlExpiryGeometry &) = delete;
  AlExpiryGeometry &operator=(const AlExpiryGeometry &) = delete;

  // (Re)prepare for (T, r, q, opts). Allocates only when the storage is missing
  // (a moved-from geometry); a prepared geometry is rebuilt in place. Never fails:
  // inputs andersen_lake would refuse (non-finite r/q, negative T) are recorded and
  // refused again by the pricing overloads with andersen_lake's own error.
  void prepare(double T, double r, double q, const std::optional<AlOpts> &opts = std::nullopt);

  [[nodiscard]] bool prepared() const noexcept;
  [[nodiscard]] double T() const noexcept;
  [[nodiscard]] double r() const noexcept;
  [[nodiscard]] double q() const noexcept;

  // Implementation access for the pricing sources; the type is opaque here.
  struct Impl;
  [[nodiscard]] const Impl *impl() const noexcept { return impl_.get(); }

private:
  std::unique_ptr<Impl> impl_;
};

// A geometry prepared for (T, r, q, opts).
[[nodiscard]] AlExpiryGeometry prepare_al_geometry(double T, double r, double q,
                                                   const std::optional<AlOpts> &opts = std::nullopt);

// Bit-identical to andersen_lake(S, K, g.T(), sigma, g.r(), g.q(), side, opts-of-g).
// InvalidArgument when `g` is not prepared.
[[nodiscard]] Result<double> andersen_lake(const AlExpiryGeometry &g, double S, double K,
                                           double sigma, Side side);

// ── Boundary handle: one solved boundary, many prices ────────────────────
//
// The exercise boundary of a call depends on its internal-put strike, which is the
// SPOT (McDonald-Schroder), not the call strike: one solve at (spot, sigma) serves
// every call strike of the expiry at that sigma. A put's boundary depends on its own
// strike in absolute terms inside the kernel (alpha = K e^{-(r-q)tau}, the clamp
// xmax = K min(1, r/q)), so a strike-rescaled reuse is NOT bit-identical (see
// andersen_lake_put_slice); a put handle therefore serves one strike at any spot.
//
// The handle is bound to (geometry, side, anchor) with anchor = the internal-put
// strike: the spot for Side::Call, the strike for Side::Put. solve(sigma) solves
// that boundary once (or records a closed-form regime); price_from_boundary(S, K)
// then prices any contract whose internal-put strike equals the anchor (call: S ==
// anchor; put: K == anchor) BIT-IDENTICALLY to andersen_lake(S, K, T, sigma, r, q,
// side, opts) — and returns NaN for any other contract, before solve(), after a
// failed solve, or where andersen_lake would return an error.
//
// OWNERSHIP / THREADS. Mutable per-thread state (~46 KB, allocated at construction);
// observes the geometry, which must outlive the handle. Enforced (L2f, from the L2b
// review): a temporary geometry cannot bind (the rvalue constructor is deleted), and a
// geometry re-prepared after solve() makes price_from_boundary return NaN until the
// handle is solved again. Not thread-safe; const price_from_boundary calls may run
// concurrently with each other but not with solve() or the geometry's prepare().
class AlBoundaryHandle {
public:
  AlBoundaryHandle(const AlExpiryGeometry &geometry, Side side, double anchor);
  AlBoundaryHandle(const AlExpiryGeometry &&geometry, Side side, double anchor) = delete;
  ~AlBoundaryHandle();
  AlBoundaryHandle(AlBoundaryHandle &&) noexcept;
  AlBoundaryHandle &operator=(AlBoundaryHandle &&) noexcept;
  AlBoundaryHandle(const AlBoundaryHandle &) = delete;
  AlBoundaryHandle &operator=(const AlBoundaryHandle &) = delete;

  // Solve (or classify) at sigma. Errors are andersen_lake's for this anchor: an
  // unprepared geometry, invalid T / sigma / r / q / anchor, the double-continuation
  // regime, a collapsed or unsolvable boundary.
  [[nodiscard]] Status solve(double sigma);
  [[nodiscard]] double price_from_boundary(double S, double K) const noexcept;

private:
  struct State;
  std::unique_ptr<State> st_;
};

// ── Price pack: one expiry, many contracts, price + exact Newton chord ───
//
// One contract of an al_price_pack call. Every contract shares the geometry's
// (T, r, q, opts).
struct AlContract {
  double S{};
  double K{};
  Side side{Side::Call};
};

// The sigma step of the pack's bumped price, in absolute vol units.
inline constexpr double kAlPackSigmaBump = 1.0e-4;

// price[i] = andersen_lake(g, S_i, K_i, sigma[i], side_i) and, when
// price_sigma_bumped is non-empty, price_sigma_bumped[i] = the same at sigma[i] +
// kAlPackSigmaBump — both BIT-IDENTICAL to the fresh andersen_lake (NaN where it
// errors). (bumped - price) / kAlPackSigmaBump is therefore the exact chord of the
// served map, the slope a Newton / secant step on these prices needs. It differs from
// the central difference by (P(s+h) - 2 P(s) + P(s-h)) / (2h) ~ (h / 2) volga, h =
// kAlPackSigmaBump; the tested bound is |chord - central| <= h * |volga| + 1e-8
// (AlPricePack.ChordSlopeMatchesTheCentralDifferenceWithinTheVolgaBound, volga from
// american_greeks_al). Shares the geometry across the pack,
// and one call boundary across consecutive calls at the same spot and sigma (puts
// re-solve per strike, see AlBoundaryHandle). Scalar per contract — not an AVX2 lane
// kernel, which would not be bit-identical. Single-thread; allocates one ~185 KB
// block of solve scratch per call. Pure in its inputs: concurrent calls on one const
// geometry are safe with disjoint outputs.
//
// @return InvalidArgument when `g` is not prepared or the spans' lengths differ
//         (nothing is written); otherwise Ok.
[[nodiscard]] Status al_price_pack(const AlExpiryGeometry &g, std::span<const AlContract> contracts,
                                   std::span<const double> sigma, std::span<double> price,
                                   std::span<double> price_sigma_bumped);

// ── σ-Chebyshev boundary family: one expiry, one side, every (K, σ) ──────
//
// L2h sigma-family (2026-10-19). The dimensionless Andersen-Lake boundary y[] of an
// internal put depends only on (σ, r, q, τ) — never on the strike — and is smooth in
// σ. A family solves that boundary on n_σ Chebyshev-Lobatto σ-nodes of a box
// [σ_lo, σ_hi] (n_σ Reference-accuracy solves when the geometry is Reference; a
// Fast / Screen geometry gives its tier's nodes) and then prices ANY contract of the
// (expiry, side) at ANY σ inside the box with no solve at all: interpolate y[] at σ
// (2nd-kind barycentric, the scheme the P2.5 slice `andersen_lake_*_slice_sigma`
// shipped on — the evaluator is duplicated in american_sigma_family.cpp with a
// comment naming its twin so the slice stays bit-identical), rescale the boundary to
// the contract's internal strike (T8 homogeneity: y[] is strike-free, only K and
// xmax = K min(1, r/q) change), and run the premium quadrature at the geometry's p.
// Cost: n_σ solves to build, then one quadrature (a few µs) per price.
//
// ACCURACY CONTRACT (PM ruling (a): distance to the accurate root, not "equal to
// the old path"). At a σ-node and at the anchor strike a family price equals the
// cold solve to rounding (~1e-14 relative); between nodes the σ-interpolation error
// enters (exponentially small in n_σ); at another strike the homogeneity rescale is
// a few ULP from a fresh solve (the kernel carries K in absolute terms; LEDGER
// 2026-10-19 "a put boundary is reusable only at its own strike" — reusable to a
// few ULP, which this contract accepts). Measured against the cold Reference and
// pinned by AlSigmaFamily.* (tests/american_geometry_test.cpp); the numbers are in
// sprints/2026-10-19-L2h-sigma-family-sprint.md. NOTHING default selects a family:
// no fitting path, preset, policy key or record format constructs one.
//
// THE ANCHOR. `anchor` is the internal-put strike the node solves run at: the
// reference strike of a put family, the spot of a call family (McDonald-Schroder:
// C(S, K; r, q) = P(K, S; q, r), internal strike = S). Any other internal strike is
// served by the rescale, so a call family built at spot S0 still prices the chain
// after the spot moves to S1 — at the rescale's few-ULP distance from a fresh solve
// at S1, NOT bit-identically; the tests measure a 1 % spot move. One family per
// (expiry, side) is the intended shape; rebuilding on a spot move is unnecessary.
//
// OWNERSHIP / THREADS. Move-only; observes the geometry (which must outlive it; a
// temporary cannot bind) and allocates its ~6 KB of state once at construction.
// prepare() needs exclusive access. After prepare() the family is IMMUTABLE and
// price() / price_and_chord() are const, allocation-free and safe to call from any
// number of threads at once on one const family (scratch lives on the caller's
// stack; the premium quadrature reads only const tables) — the shape the
// certification fan-out and the rung memo need. A geometry re-prepared after
// prepare() makes every price refuse (GeometryRePrepared) until the family is
// prepared again.
//
// REFUSALS are typed and allocation-free on every path (price() never returns NaN
// on a refusal: `status != Ok` and `price` is NaN together). The L1c non-monotone
// refusal is NOT the family's to make — it is keyed on an inversion TARGET, which a
// pricer does not see — so a family-based inversion must test the band with
// al_non_monotone_gate (american_iv.hpp) BEFORE iterating on the family and route
// band rows to the accurate cold inversion (sprint note §"What L2i must assume").
enum class AlSigmaFamilyStatus : std::uint8_t {
  Ok = 0,
  NotPrepared = 1,         // prepare() has not succeeded (or the family is moved-from)
  GeometryRePrepared = 2,  // the geometry was re-prepared after prepare()
  InvalidArgument = 3,     // S or K not > 0 / not finite, or sigma not finite
  SigmaBelowGuard = 4,     // sigma < kAlSigmaFamilyMinSigma (the small-sigma guard)
  SigmaOutsideBox = 5,     // sigma outside [sigma_lo, sigma_hi] (never extrapolated)
};

struct AlSigmaFamilyPrice {
  double price = std::numeric_limits<double>::quiet_NaN(); // NaN unless status == Ok
  AlSigmaFamilyStatus status = AlSigmaFamilyStatus::NotPrepared;
};

struct AlSigmaFamilyChord {
  double price = std::numeric_limits<double>::quiet_NaN();              // at sigma
  double price_sigma_bumped = std::numeric_limits<double>::quiet_NaN(); // at sigma + h
  AlSigmaFamilyStatus status = AlSigmaFamilyStatus::NotPrepared;
};

// The σ-axis the Chebyshev nodes are laid on: log σ (the nodes then cluster toward
// the low end of the box in σ, where the boundary moves fastest) or σ itself. Both
// are measured in the sprint note (§4): at 12 nodes LogSigma is the tighter axis on
// every set (SPY 23 d chain 1.5e-10 vs 1.8e-8 max relative; the 1,350-contract grid's
// [0.08, 0.7] box 1.8e-6 vs 5.4e-5), at 8 nodes Sigma is — so LogSigma is the default
// and Sigma stays selectable for a measured reason.
enum class AlSigmaFamilyAxis : std::uint8_t {
  LogSigma = 0,
  Sigma = 1,
};

// Guards shared with the P2.5 slice (SigmaInterpOptions): no family below this
// expiry (the whole expiry prices cold), no price below this σ.
inline constexpr double kAlSigmaFamilyMinTau = 3.0 / 365.0;
inline constexpr double kAlSigmaFamilyMinSigma = 0.01;
inline constexpr std::uint16_t kAlSigmaFamilyMinNodes = 4;
inline constexpr std::uint16_t kAlSigmaFamilyMaxNodes = 16;

// L3g family-greeks (2026-10-20): the analytics TU's (american_family_greeks.cpp) key
// to the prepared state — the one friend of the family; defined in that TU only.
namespace sigma_family_detail {
struct FamilyAccess;
}

class AlSigmaFamily {
public:
  AlSigmaFamily(const AlExpiryGeometry &geometry, Side side, double anchor);
  AlSigmaFamily(const AlExpiryGeometry &&geometry, Side side, double anchor) = delete;
  ~AlSigmaFamily();
  AlSigmaFamily(AlSigmaFamily &&) noexcept;
  AlSigmaFamily &operator=(AlSigmaFamily &&) noexcept;
  AlSigmaFamily(const AlSigmaFamily &) = delete;
  AlSigmaFamily &operator=(const AlSigmaFamily &) = delete;

  // Solve the n_sigma node boundaries over [sigma_lo, sigma_hi]. Refuses — leaving
  // the family UNPREPARED, never half-built — with InvalidArgument when the geometry
  // is not prepared, the box is degenerate (sigma_lo < kAlSigmaFamilyMinSigma, or
  // sigma_hi - sigma_lo <= 1e-9, or non-finite), n_sigma is outside
  // [kAlSigmaFamilyMinNodes, kAlSigmaFamilyMaxNodes], the anchor is not > 0, or the
  // expiry is below kAlSigmaFamilyMinTau; with NotImplemented when the side is not
  // in the American regime on this geometry (closed-form / double-continuation
  // expiries have no boundary family), a node boundary collapses (xmax <= 0), or a
  // node's boundary sweep cannot move (the kernel's NotConverged, mapped exactly as
  // andersen_lake's al_solve_status_error maps it); with Internal only when a node
  // solve reports TableMissing (Gauss-Legendre table unavailable). Strong guarantee:
  // on any refusal the previous preparation, if any, is discarded (prepared() == false).
  [[nodiscard]] Status prepare(double sigma_lo, double sigma_hi, std::uint16_t n_sigma,
                               AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma);

  [[nodiscard]] bool prepared() const noexcept;
  [[nodiscard]] double sigma_lo() const noexcept;
  [[nodiscard]] double sigma_hi() const noexcept;
  [[nodiscard]] std::uint16_t n_sigma() const noexcept;
  [[nodiscard]] double anchor() const noexcept;
  [[nodiscard]] Side side() const noexcept;

  // The (S, K) contract of this family's side at sigma. Ok iff S, K > 0 and finite,
  // sigma finite, sigma >= kAlSigmaFamilyMinSigma and inside the box; otherwise the
  // typed refusal with a NaN price. Const, allocation-free, thread-safe (above).
  [[nodiscard]] AlSigmaFamilyPrice price(double S, double K, double sigma) const noexcept;

  // price(S, K, sigma) and price(S, K, sigma + kAlPackSigmaBump) from the same
  // boundary family: (bumped - price) / kAlPackSigmaBump is the forward chord of the
  // family's map, the Newton slope an inversion on the family needs. Refuses as
  // price() does for EITHER sigma (so the box needs kAlPackSigmaBump of headroom
  // above the highest query); nothing is written on a refusal but the status.
  [[nodiscard]] AlSigmaFamilyChord price_and_chord(double S, double K, double sigma) const noexcept;

  // Bytes of state this family owns (one heap block; for the memory budget).
  [[nodiscard]] static std::size_t state_bytes() noexcept;

private:
  struct State;
  std::unique_ptr<State> st_;
  friend struct sigma_family_detail::FamilyAccess; // L3g: the Greeks TU reads State (const)
};

// ── σ-family analytics: analytic Greeks, the exact Newton slope, chains, shocks ──
//
// L3g family-greeks (2026-10-20). The family price is one closed-form map of
// (S, K, σ): P = clamp(E(S, K, σ) + Π(S, K, σ; y(σ))) with E the European put and Π
// the early-exercise premium quadrature over the boundary B_j(σ) = xmax · exp(−√y_j(σ)),
// y_j the τ-barycentric value of the σ-interpolated series. Every Greek below is an
// EXACT derivative of that served map, computed in ONE fused pass over the premium
// nodes (no stencil, no re-solve), so the Greeks always belong to the served price:
//   Δ, Γ (and speed)  — ∂/∂S of the integrand at FIXED boundary (the internal put's
//                       boundary does not depend on the spot: exact);
//   calls             — through the McDonald-Schroder map C(S, K; r, q) = P(K, S; q, r)
//                       and the homogeneity identities of the internal put
//                       (∂P/∂K_int = (P − S_int ∂P/∂S_int)/K_int, ∂²P/∂K_int² =
//                       (S_int/K_int)² ∂²P/∂S_int²): the call's spot is the internal
//                       strike, which the family serves by its T8 rescale;
//   vega, vanna, volga — the explicit σ-terms PLUS the boundary term dy_k/dσ (and d²)
//                       from the σ-derivative of the Chebyshev interpolant (nodal
//                       differentiation matrices D⁽¹⁾ / D⁽²⁾ applied to the series,
//                       then interpolated — exact for the polynomial, stable at and
//                       near a node; american_family_greeks.cpp cites the formulas);
//                       `boundary_term_included` records that these carry it;
//   θ (calendar, dV/dt) — the continuation-region PDE identity
//                       θ = rV − (r − q) S Δ − ½ σ² S² Γ, the american_greeks_al /
//                       RecordGreeks definition: the family has NO τ axis (one geometry
//                       per expiry), so an FD-in-T theta does not exist here. NOT usable
//                       next to the exercise edge: within ~0.03 σ√T of it this θ was
//                       measured 25 – 138 % off the converged calendar difference, once
//                       with the wrong sign (A0 r6 slow suite; review-A0 m2). This bundle
//                       has no per-Greek reason, so nothing marks those rows — the (σ, q)
//                       bundle (A0 block below) types the band NearExerciseEdge; review-L3g
//                       m5's in-band part is OPEN for this bundle's owner;
//   charm             — ∂θ/∂S from the same identity with the analytic speed;
//   rho, phi (∂P/∂r, ∂P/∂q) — ONLY with an AlSigmaFamilyCarryBumps (below): a σ-family
//                       has no r / q axis, and its frozen-boundary derivative (y held)
//                       is up to 2.4× off the re-solved rho on SPY 23 d (sprint note
//                       §4), so it is NEVER served (PM ruling, L3g resume r4). The
//                       carry set holds the σ-series of SECOND families built at r ± h
//                       and q ± h (h = kAlSigmaFamilyCarryBump, the bundle's own bump);
//                       rho / phi are then the frozen-y terms PLUS the boundary term
//                       Σ ∂P/∂y_k · dy_k/dr (dq) with dy_k/dr the central difference of
//                       the bumped series — the same fused pass, no extra quadrature.
//                       Without a carry set a requested rho / phi is NaN with
//                       `carry == AlSigmaFamilyCarryStatus::NoCarryBumps` (REFUSED).
// Regime flags mirror RecordGreeks (fitting/expiry_record.hpp): when the spot is on
// the exercise side of the family's current edge (B at τ = T), or the served price is
// intrinsic to the bundle's own tolerance (≤ intrinsic + 1e-9 K — the kernel's clamp,
// or the premium decomposition reproducing K − S to quadrature noise, which is what
// it does in and just above the exercise region), the EXERCISE Greeks are served with
// the unchanged price (Δ = −1 put / +1 call, every other Greek 0, `exercised`);
// `floored` marks that regime on the CONTINUATION side of the edge (the band where
// the served max(decomposition, intrinsic) has no smooth derivative), so both flags
// belong to the served price.
// Accuracy: measured, not "equal to the old path" (PM ruling (a)): the sprint note
// 2026-10-20-L3g-family-greeks-sprint.md tabulates every Greek against the Reference
// bundle on the same geometry AND a Richardson reference; the AlSigmaFamilyGreeks.*
// tests pin 2-figure round-ups. Cost: price + 8 Greeks in one quadrature pass (a few
// µs; the note's µs table). Every entry refuses exactly as AlSigmaFamily::price()
// does (same admit tree, typed status, NaN with the status, never NaN alone), is
// const, allocation-free and thread-safe on one const family (stack scratch only).
// NOTHING default selects any of this: no fitting path, preset, policy key or record
// constructs a family or calls these (L2i adopts family_price_and_vega opt-in; L3h
// consumes the chain / shock entries).

// Which Greeks a call computes. Price is always computed; an unrequested Greek is
// left 0.0 (the K4 tier convention of american_greeks_al). Price-only costs exactly
// AlSigmaFamily::price() (it IS that call).
struct AlSigmaFamilyGreeksMask {
  static constexpr std::uint16_t kDelta = 1u << 0;
  static constexpr std::uint16_t kGamma = 1u << 1;
  static constexpr std::uint16_t kVega = 1u << 2;
  static constexpr std::uint16_t kTheta = 1u << 3;
  static constexpr std::uint16_t kRho = 1u << 4;
  static constexpr std::uint16_t kPhi = 1u << 5;
  static constexpr std::uint16_t kVanna = 1u << 6;
  static constexpr std::uint16_t kVolga = 1u << 7;
  static constexpr std::uint16_t kCharm = 1u << 8;

  std::uint16_t bits = 0;

  [[nodiscard]] static constexpr AlSigmaFamilyGreeksMask price_only() noexcept { return {0}; }
  // Δ, Γ, vega, θ — the hedge set ("+4").
  [[nodiscard]] static constexpr AlSigmaFamilyGreeksMask hedge() noexcept {
    return {kDelta | kGamma | kVega | kTheta};
  }
  // Δ, Γ, vega, θ, rho, phi, vanna, volga ("+8").
  [[nodiscard]] static constexpr AlSigmaFamilyGreeksMask eight() noexcept {
    return {kDelta | kGamma | kVega | kTheta | kRho | kPhi | kVanna | kVolga};
  }
  // The eight plus charm.
  [[nodiscard]] static constexpr AlSigmaFamilyGreeksMask all() noexcept {
    return {eight().bits | kCharm};
  }
  [[nodiscard]] constexpr bool has(std::uint16_t bit) const noexcept { return (bits & bit) != 0; }
};

// How a requested rho / phi was served (L3g resume r4). All-or-nothing: either every
// requested rho / phi is served (Ok) or every requested one is NaN with the reason.
enum class AlSigmaFamilyCarryStatus : std::uint8_t {
  NotRequested = 0,       // the mask asked for neither rho nor phi
  Ok = 1,                 // served from the carry set's bumped families (boundary term in)
  NoCarryBumps = 2,       // REFUSED: no carry set on this entry (the frozen boundary is not served)
  CarryBumpsMismatch = 3, // REFUSED: the carry set is unprepared, lacks a requested axis,
                          // or was built from a different family
};

struct AlSigmaFamilyGreeks {
  double price = std::numeric_limits<double>::quiet_NaN(); // NaN unless status == Ok
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0; // calendar dV/dt per year (PDE identity; NOT near the edge, above)
  double rho = 0.0;   // ∂P/∂r: carry set only (else NaN, `carry` says why)
  double phi = 0.0;   // ∂P/∂q: carry set only (else NaN, `carry` says why)
  double vanna = 0.0; // ∂²P/∂S∂σ
  double volga = 0.0; // ∂²P/∂σ²
  double charm = 0.0; // ∂Δ/∂t = ∂θ/∂S
  AlSigmaFamilyStatus status = AlSigmaFamilyStatus::NotPrepared;
  AlSigmaFamilyCarryStatus carry = AlSigmaFamilyCarryStatus::NotRequested;
  bool exercised = false; // intrinsic served with the exercise Greeks
  bool floored = false;   // exercised on the continuation side of the current edge
  // True when vega / vanna / volga were computed: they carry the σ-boundary term
  // dy/dσ. A served rho / phi (carry == Ok) carries its (r, q) boundary term.
  bool boundary_term_included = false;
  // On a refusal every requested Greek is NaN together with the price.
};

// Price + the requested Greeks of the (S, K) contract of the family's side at sigma.
// Refuses exactly as AlSigmaFamily::price(). Const, allocation-free, thread-safe.
// A requested rho / phi is NaN with carry == NoCarryBumps (use the overload below).
[[nodiscard]] AlSigmaFamilyGreeks family_greeks(const AlSigmaFamily &fam, double S, double K,
                                                double sigma,
                                                AlSigmaFamilyGreeksMask mask) noexcept;

// ── rho / phi: the carry set (second families at r ± h, q ± h) ──
//
// The bump of the second families: the Reference bundle's own (american_greeks_al's
// r ± 1e-4, american_carry_greeks_al's q ± 1e-4), so the two are compared like for like.
inline constexpr double kAlSigmaFamilyCarryBump = 1.0e-4;

// The (r, q)-sensitivity of ONE prepared family's boundary series. prepare(base, ...)
// builds, on geometries at (T, r ± h, q) and / or (T, r, q ± h) with the base
// geometry's own AlOpts, families with the base's side, anchor, σ-box, node count and
// axis, and keeps ONLY the central differences of their σ-series (dy_{k,s}/dr,
// dy_{k,s}/dq; the bumped geometries and families are released before prepare()
// returns). When the DOWN-bumped family refuses with NotImplemented the forward
// difference against the base series is kept instead and *_one_sided() says so — the
// A5 rule of american_greeks_al / american_carry_greeks_al, whose intended case is a
// down bump that leaves the American regime (a put with r − h <= 0, a call with
// q − h <= 0). The rule keys on the error CODE, not on the regime: every other
// NotImplemented refusal of the down bump (a collapsed node, a sweep that cannot move,
// a collocation grid that differs from the base's) takes the same one-sided route
// (A0 r6 rewording, review-L3g m7). Cost: 2 family builds per axis
// (≈ the base family's build each, plus a geometry prepare); the per-contract rho /
// phi then costs one more τ-barycentric accumulator in the same fused pass.
// Refuses (InvalidArgument) when the base is not prepared or its geometry was
// re-prepared, with the bumped family's own refusal when an UP-bumped family refuses
// (or a down-bumped one with any code but NotImplemented), (NotImplemented) when the
// UP-bumped family's collocation grid differs from the base's. Strong
// guarantee: on a refusal the set is unprepared.
// Move-only, immutable after prepare(), thread-safe for every const use; it does NOT
// observe the base family (identity is checked by value on every call).
class AlSigmaFamilyCarryBumps {
public:
  AlSigmaFamilyCarryBumps();
  ~AlSigmaFamilyCarryBumps();
  AlSigmaFamilyCarryBumps(AlSigmaFamilyCarryBumps &&) noexcept;
  AlSigmaFamilyCarryBumps &operator=(AlSigmaFamilyCarryBumps &&) noexcept;
  AlSigmaFamilyCarryBumps(const AlSigmaFamilyCarryBumps &) = delete;
  AlSigmaFamilyCarryBumps &operator=(const AlSigmaFamilyCarryBumps &) = delete;

  // `axes` selects the axes to build: kRho -> r ± h, kPhi -> q ± h (other bits ignored;
  // neither bit -> InvalidArgument).
  [[nodiscard]] Status prepare(const AlSigmaFamily &base, AlSigmaFamilyGreeksMask axes);

  [[nodiscard]] bool prepared() const noexcept;
  [[nodiscard]] bool has_rho() const noexcept;
  [[nodiscard]] bool has_phi() const noexcept;
  [[nodiscard]] bool rho_one_sided() const noexcept;
  [[nodiscard]] bool phi_one_sided() const noexcept;
  [[nodiscard]] static std::size_t state_bytes() noexcept;

  struct State;

private:
  std::unique_ptr<State> st_;
  friend struct sigma_family_detail::FamilyAccess;
};

// family_greeks(fam, …) with rho / phi served from `carry` (carry == Ok), which must
// have been prepared from `fam` (same side, anchor, (T, r, q), box, nodes, axis,
// profile) with every requested axis; otherwise the requested rho / phi are NaN with
// carry == CarryBumpsMismatch and every other field is family_greeks(fam, …)'s.
// Exercised rows serve rho = phi = 0 (intrinsic does not move with r or q).
[[nodiscard]] AlSigmaFamilyGreeks family_greeks(const AlSigmaFamily &fam,
                                                const AlSigmaFamilyCarryBumps &carry, double S,
                                                double K, double sigma,
                                                AlSigmaFamilyGreeksMask mask) noexcept;

struct AlSigmaFamilyPriceVega {
  double price = std::numeric_limits<double>::quiet_NaN();
  double vega = std::numeric_limits<double>::quiet_NaN(); // dP/dσ of the family map
  AlSigmaFamilyStatus status = AlSigmaFamilyStatus::NotPrepared;
  // A0 r6 (review-L3g m4): the row is in the exercise regime (family_greeks' `exercised`
  // rule). vega is then exactly 0.0 BY CONSTRUCTION (intrinsic does not move with σ), not
  // a measured slope: an inversion must treat it as "no slope here", never divide by it.
  bool exercised = false;
};

// The exact Newton slope for a family-based inversion: price(S, K, sigma) and the
// analytic dP/dσ of the SAME map in one fused pass — replaces price_and_chord's two
// quadratures and its O(h/2 · volga) chord bias (the sprint note §"What L2i must
// assume" gives the one-line opt-in). Refuses as price() does; needs NO headroom
// above sigma (the chord's kAlPackSigmaBump requirement goes away).
[[nodiscard]] AlSigmaFamilyPriceVega family_price_and_vega(const AlSigmaFamily &fam, double S,
                                                           double K, double sigma) noexcept;

// Caller-owned output columns for the chain / shock entries. A column is written
// iff it is non-empty, and then its length must equal the row count; the mask
// decides what is COMPUTED, the spans decide what is STORED (a requested Greek with
// an empty span is computed and dropped; an unrequested Greek with a span gets 0.0).
// `flags` (optional) receives bit 0 = exercised, bit 1 = floored, bit 2 =
// boundary_term_included, bit 3 = a requested rho / phi was REFUSED (NaN; carry
// status NoCarryBumps or CarryBumpsMismatch) per row.
struct AlSigmaFamilyGreekColumns {
  std::span<double> price{};
  std::span<double> delta{};
  std::span<double> gamma{};
  std::span<double> vega{};
  std::span<double> theta{};
  std::span<double> rho{};
  std::span<double> phi{};
  std::span<double> vanna{};
  std::span<double> volga{};
  std::span<double> charm{};
  std::span<std::uint8_t> flags{};
};

// One expiry's chain at spot S: row i is (K[i], sigma[i], side[i]) priced through
// `put` or `call` by its side. A row whose family pointer is null carries
// NotPrepared; a refused row carries its status; both write NaN to every stored
// column. rho / phi rows are served through `put_carry` / `call_carry` (the carry
// set of that side's family, family_greeks' carry overload); a null carry set
// refuses them (NaN, flags bit 3). Allocation-free, const on every input,
// single-thread per call (rows may be split across threads by the caller with
// disjoint spans).
// @return InvalidArgument when the span lengths disagree (nothing is written);
//         otherwise Ok — per-row outcomes are in `status`.
[[nodiscard]] Status family_chain_greeks(const AlSigmaFamily *put, const AlSigmaFamily *call,
                                         double S, std::span<const double> K,
                                         std::span<const double> sigma,
                                         std::span<const Side> side, AlSigmaFamilyGreeksMask mask,
                                         const AlSigmaFamilyGreekColumns &out,
                                         std::span<AlSigmaFamilyStatus> status,
                                         const AlSigmaFamilyCarryBumps *put_carry = nullptr,
                                         const AlSigmaFamilyCarryBumps *call_carry = nullptr);

// Exact reprice of ONE contract over a (spot shock × vol shock) grid through the same
// family: cell (i, j), stored at index i * vol_bump.size() + j, is the contract at
// spot S · (1 + spot_pct[i]) and sigma + vol_bump[j] (absolute vol units). A spot
// shock is the family's homogeneity rescale; a vol shock moves σ inside the box —
// a σ outside it refuses that cell with SigmaOutsideBox (never extrapolated), a
// non-positive shocked spot with InvalidArgument. dt and dr / dq are NOT axes of a
// σ-family (one geometry per (T, r, q)), so this API takes none: a time or rate
// shock needs another geometry; a requested rho / phi is refused per cell (NaN,
// flags bit 3 — no carry set here). Columns / status hold n_spot × n_vol entries.
// @return InvalidArgument when a non-empty column or `status` has the wrong length
//         (nothing is written); otherwise Ok.
[[nodiscard]] Status family_shock_grid(const AlSigmaFamily &fam, double S, double K, double sigma,
                                       std::span<const double> spot_pct,
                                       std::span<const double> vol_bump,
                                       AlSigmaFamilyGreeksMask mask,
                                       const AlSigmaFamilyGreekColumns &out,
                                       std::span<AlSigmaFamilyStatus> status);

// ── Detached serve copy: the family without its geometry ────────────────────
//
// A prepared family observes its AlExpiryGeometry (~90 KB per expiry, both
// orientations) for the premium quadrature's workspace header (qx/qw_price,
// n_quad_price, fast) and the re-prepare stamp. A detached copy owns everything
// the fused kernel reads (the σ-series, the node grid, the quadrature pointers —
// process-wide Gauss-Legendre tables — and the flags: `state_bytes()`), so the
// geometry may be released after detach() while the copy keeps serving price and
// Greeks. Its price is the family's price to the bit (the fused kernel mirrors the
// premium quadrature expression for expression; pinned by the tests); its Greeks
// are family_greeks' bits (a requested rho / phi is refused, NoCarryBumps, as on
// the family without a carry set). No generation stamp is checked (the copy cannot
// go stale); the admit tree is otherwise the family's. Move-only, immutable,
// thread-safe for every const call.
class AlSigmaFamilyDetached {
public:
  AlSigmaFamilyDetached();
  ~AlSigmaFamilyDetached();
  AlSigmaFamilyDetached(AlSigmaFamilyDetached &&) noexcept;
  AlSigmaFamilyDetached &operator=(AlSigmaFamilyDetached &&) noexcept;
  AlSigmaFamilyDetached(const AlSigmaFamilyDetached &) = delete;
  AlSigmaFamilyDetached &operator=(const AlSigmaFamilyDetached &) = delete;

  [[nodiscard]] bool prepared() const noexcept;
  [[nodiscard]] double sigma_lo() const noexcept;
  [[nodiscard]] double sigma_hi() const noexcept;
  [[nodiscard]] Side side() const noexcept;

  [[nodiscard]] AlSigmaFamilyPrice price(double S, double K, double sigma) const noexcept;
  [[nodiscard]] AlSigmaFamilyGreeks greeks(double S, double K, double sigma,
                                           AlSigmaFamilyGreeksMask mask) const noexcept;
  [[nodiscard]] AlSigmaFamilyPriceVega price_and_vega(double S, double K,
                                                      double sigma) const noexcept;

  [[nodiscard]] static std::size_t state_bytes() noexcept;

  struct State;

private:
  friend AlSigmaFamilyDetached family_detach(const AlSigmaFamily &fam);
  std::unique_ptr<State> st_;
};

// The detached copy of `fam`. When `fam` is not prepared (or its geometry was
// re-prepared) the copy is unprepared: prepared() == false and every call refuses
// with NotPrepared. Allocates the copy's one heap block.
[[nodiscard]] AlSigmaFamilyDetached family_detach(const AlSigmaFamily &fam);

// ── end of the L3g family-greeks sub-block (union-merge marker: L2h-b / L3k add theirs) ──

// ── L2h-b sigma-q family (2026-10-21): (σ, q) boundary family, q a second axis ──
//
// L2h-b (2026-10-21). Under a per-strike effective yield (the funding-pv carry: the
// SPY cohort5 board carries 1-144 distinct q per expiry) an AlSigmaFamily built at ONE
// q misprices every other strike by about T·S·Δq/2 — 2-6 orders above its own bound.
// The dimensionless boundary y[] is as smooth in the internal put's carry as in σ, so
// this family lays a TENSOR of Chebyshev-Lobatto nodes over (σ, q): the σ axis exactly
// as AlSigmaFamily's (n_σ nodes, LogSigma / Sigma, same box rule) times n_q nodes
// linear in q over the caller's [q_lo, q_hi] (the expiry's measured yield range).
// Each node is one prepared boundary solve at the anchor with that node's (rate,
// yield) — n_σ · n_q solves to build, at the geometry's own AlOpts. A price
// interpolates y[] at (σ, q) — 2nd-kind barycentric in σ, then in q — and runs the
// European leg and the premium quadrature at the strike's OWN q: the boundary is the
// only interpolated quantity (the premium is first-order insensitive to a boundary
// error, the reason the σ-family reaches ~1e-10). Measured against the cold Reference
// per n_q in sprints/2026-10-21-L2hb-sigma-q-family-sprint.md; pinned by
// AlSigmaQFamily.* (tests/american_sigma_q_family_test.cpp).
//
// THE BUILD (L2h-c, 2026-10-21; sprints/2026-10-21-L2hc-family-build-speed-sprint.md).
// On an AVX2 host (simd::use_avx2(), i.e. unless SimdIsa::ForceScalar) under the
// Reference profile and the Baw seed, the n_σ·n_q node boundaries are solved FOUR AT A
// TIME through the AVX2 boundary-batch kernel (the one andersen_lake's put batch rides;
// same scheme, same sweep budget and tol), a remainder < 4 and any lane the kernel
// hands back on the scalar prepared solve. The AVX2 boundary differs from the scalar one
// at rounding level (the batch's AvxBoundary parity, <= ~4e-13), so an AVX2-built family
// is NOT bit-identical to a scalar-built one; every accuracy pin holds on both builds.
// Under ForceScalar (or another profile / seed) every node is the scalar prepared solve.
//
// THE q BOX. q_hi - q_lo <= kAlSigmaQFamilyDegenerateWidth collapses the axis to ONE
// node at the box midpoint: the family is then AlSigmaFamily at that q — BIT FOR BIT
// on the scalar build, within the pinned rounding bound on the AVX2 build. Otherwise
// n_q in [kAlSigmaQFamilyMinQNodes, kAlSigmaQFamilyMaxQNodes]. A q outside [q_lo,
// q_hi] refuses (YieldOutsideBox; never extrapolated). prepare() refuses with
// NotImplemented a box on which the side is not American at both ends (a call needs
// q > 0 — a q <= 0 call never exercises early: price it in closed form), one that
// straddles q = r (the boundary's scale K·min(1, r/q) has a kink there and the
// near-expiry boundary changes form — split the box at r), and — the ACCURACY GUARD —
// a box too wide for its distance to q = r (the singular point of y(q) on both sides):
// with a = |box centre − r| / box half-width and the Bernstein-ellipse factor
// rho = a + sqrt(a² − 1), a box is refused when rho^(−n_q) >
// kAlSigmaQFamilyMaxQAxisEstimate (a box touching r has rho = 1). Measured, the q-axis
// price error is 0.008-0.5 × rho^(−n_q); SPY's boxes (<= 1.8e-3 wide, >= 4 % from r:
// rho >= 116) pass at n_q >= 3, a 1 %-wide box centred 3 % from r needs n_q >= 6, a
// 4 %-wide box near r is refused at every n_q — split it into narrower boxes (or raise
// n_q) until each passes.
// (q = 0 is singular for calls too, but the call's exercise premium vanishes there —
// review-L2hb's probe priced q in [2e-5, 9e-4] calls at 0 USD error — so the guard
// measures r only.)
//
// THE GEOMETRY supplies T, r, the AL scheme and the premium quadrature's tables; its
// own q is used by no node and no price. The quadrature header the price path reads
// (Gauss-Legendre pointers + flags) is q- and side-free, so the family takes it from
// the geometry's orientation for `side` or, when that side is European at the
// geometry's q (a q > 0 call box on an expiry whose geometry q <= 0), from the other
// side's orientation — the same scheme, the same tables (review-L2hb m1). Anchor,
// ownership, threads (const, allocation-free, thread-safe price calls after
// prepare()), the re-prepare stamp, the L1c band rule and every other refusal are
// AlSigmaFamily's (above). Nothing default selects it.

// AlSigmaFamilyStatus plus YieldOutsideBox; the shared enumerators carry the SAME
// values (a caller may map one onto the other by value).
enum class AlSigmaQFamilyStatus : std::uint8_t {
  Ok = 0,
  NotPrepared = 1,         // prepare() has not succeeded (or the family is moved-from)
  GeometryRePrepared = 2,  // the geometry was re-prepared after prepare()
  InvalidArgument = 3,     // S or K not > 0 / not finite, or sigma / q not finite
  SigmaBelowGuard = 4,     // sigma < kAlSigmaFamilyMinSigma (the small-sigma guard)
  SigmaOutsideBox = 5,     // sigma outside [sigma_lo, sigma_hi] (never extrapolated)
  YieldOutsideBox = 6,     // q outside [q_lo, q_hi] (never extrapolated)
};

struct AlSigmaQFamilyPrice {
  double price = std::numeric_limits<double>::quiet_NaN(); // NaN unless status == Ok
  AlSigmaQFamilyStatus status = AlSigmaQFamilyStatus::NotPrepared;
};

struct AlSigmaQFamilyChord {
  double price = std::numeric_limits<double>::quiet_NaN();              // at sigma
  double price_sigma_bumped = std::numeric_limits<double>::quiet_NaN(); // at sigma + h
  AlSigmaQFamilyStatus status = AlSigmaQFamilyStatus::NotPrepared;
};

inline constexpr std::uint16_t kAlSigmaQFamilyMinQNodes = 2;
inline constexpr std::uint16_t kAlSigmaQFamilyMaxQNodes = 8;
inline constexpr double kAlSigmaQFamilyDegenerateWidth = 1.0e-12;
// The accuracy guard's ceiling on rho^(−n_q) (THE q BOX above).
inline constexpr double kAlSigmaQFamilyMaxQAxisEstimate = 1.0e-6;

// prepare()'s optional self-check (opt-in: it costs four scalar Reference boundary
// solves + 24 quadratures, ≈ 0.6-0.8 ms per family).
enum class AlSigmaQFamilyCheck : std::uint8_t {
  None = 0,    // no check (self_check() reports NotPrepared)
  OffNode = 1, // measure the family against the cold Reference off its nodes
};

// A SAMPLED ESTIMATE of the family's error per (expiry, side), NOT a bound: the maxima
// over 12 prices, measured at 0.1-0.4x of a chain's worst (review L2h-c n1), so a
// certificate must not cite max_* as its error bar without its own margin. Four
// check points sit at the midpoints of the first and the central cell of each axis
// (the σ axis on its LogSigma / Sigma coordinate; on a collapsed q box the q node, so
// the points pair up); at each, the cold Reference boundary is solved by the scalar
// prepared solve at the anchor (bit-identical to the cold al_solve_put_boundary) and
// the family's price is compared with the price from that boundary at internal
// moneyness S/K in {0.8, 1.0, 1.25} (K = the anchor; the price is homogeneous of
// degree 1, so max_abs_per_strike × K estimates |dP| in USD at that moneyness). An
// estimate, not an enclosure: the error between the check points is not bounded by it
// (the sprint note compares it with each chain's measured worst).
struct AlSigmaQFamilySelfCheck {
  double max_abs_per_strike = std::numeric_limits<double>::quiet_NaN(); // estimate: max |dP| / K
  double max_rel = std::numeric_limits<double>::quiet_NaN(); // estimate: max |dP| / P, P >= 1e-4 K
  std::uint16_t prices = 0; // price pairs compared (0 when no check point solved)
  AlSigmaQFamilyStatus status = AlSigmaQFamilyStatus::NotPrepared; // Ok = measured, not passed
};

namespace sigma_q_family_detail {
struct QFamilyAccess; // the internal accessor (src/pricing/american_sigma_q_family.hpp)
} // namespace sigma_q_family_detail

class AlSigmaQFamily {
public:
  AlSigmaQFamily(const AlExpiryGeometry &geometry, Side side, double anchor);
  AlSigmaQFamily(const AlExpiryGeometry &&geometry, Side side, double anchor) = delete;
  ~AlSigmaQFamily();
  AlSigmaQFamily(AlSigmaQFamily &&) noexcept;
  AlSigmaQFamily &operator=(AlSigmaQFamily &&) noexcept;
  AlSigmaQFamily(const AlSigmaQFamily &) = delete;
  AlSigmaQFamily &operator=(const AlSigmaQFamily &) = delete;

  // Solve the n_sigma x n_q node boundaries over [sigma_lo, sigma_hi] x [q_lo, q_hi]
  // (THE BUILD above). Refuses — leaving the family UNPREPARED, never half-built — with
  // InvalidArgument for every AlSigmaFamily::prepare InvalidArgument case, a non-finite
  // or inverted q box (q_lo > q_hi), or n_q outside [kAlSigmaQFamilyMinQNodes,
  // kAlSigmaQFamilyMaxQNodes] on a non-degenerate box (n_q is ignored on a degenerate
  // one); with NotImplemented when the geometry has no American orientation on either
  // side, the side is not American at q_lo or q_hi, the box straddles q = r, the box
  // fails the accuracy guard, a node boundary collapses or a node sweep cannot move
  // (the kernel's NotConverged); with Internal when the Gauss-Legendre tables are
  // missing. `check` == OffNode also runs the self-check (self_check()). Allocates its
  // solve scratch (≈ 0.3 MB: one node orientation, the retained scalar solve state and
  // one 4-lane AVX2 pack) for its duration and the node tensor (sized from (n_boundary,
  // n_q, n_sigma); kept across a re-prepare of the same shape); the price path
  // allocates nothing. On any refusal the previous preparation, if any, is discarded
  // (prepared() == false).
  [[nodiscard]] Status prepare(double sigma_lo, double sigma_hi, std::uint16_t n_sigma, double q_lo,
                               double q_hi, std::uint16_t n_q,
                               AlSigmaFamilyAxis axis = AlSigmaFamilyAxis::LogSigma,
                               AlSigmaQFamilyCheck check = AlSigmaQFamilyCheck::None);

  [[nodiscard]] bool prepared() const noexcept;
  [[nodiscard]] double sigma_lo() const noexcept;
  [[nodiscard]] double sigma_hi() const noexcept;
  [[nodiscard]] double q_lo() const noexcept;
  [[nodiscard]] double q_hi() const noexcept;
  [[nodiscard]] std::uint16_t n_sigma() const noexcept;
  // The q nodes actually laid down: 1 on a degenerate box, else prepare()'s n_q.
  [[nodiscard]] std::uint16_t n_q() const noexcept;
  [[nodiscard]] double anchor() const noexcept;
  [[nodiscard]] Side side() const noexcept;

  // The (S, K) contract of this family's side at (sigma, q), q = the strike's own
  // effective yield. Ok iff S, K > 0 and finite, sigma and q finite, sigma >=
  // kAlSigmaFamilyMinSigma, sigma inside the σ box and q inside the q box; otherwise
  // the typed refusal with a NaN price. Const, allocation-free, thread-safe.
  [[nodiscard]] AlSigmaQFamilyPrice price(double S, double K, double sigma,
                                          double q) const noexcept;

  // price(S, K, sigma, q) and price(S, K, sigma + kAlPackSigmaBump, q) from the same
  // family (the forward chord, the Newton slope of a family inversion). Refuses as
  // price() does for EITHER sigma; nothing is written on a refusal but the status.
  [[nodiscard]] AlSigmaQFamilyChord price_and_chord(double S, double K, double sigma,
                                                    double q) const noexcept;

  // L2h-c: the strikes K[0..n) of this family's side at ONE (sigma, q) into out[i] —
  // the boundary is interpolated ONCE, then each strike runs its rescale + European leg
  // + premium quadrature; out[i] == price(S, K[i], sigma, q).price to the bit. K and out
  // must have equal lengths, else InvalidArgument with nothing written. The (S, sigma,
  // q) refusals are price()'s, with every out[i] NaN; a strike that is not finite and
  // > 0 gets NaN while the rest are priced, and the call then returns InvalidArgument.
  // Const, allocation-free, thread-safe.
  [[nodiscard]] AlSigmaQFamilyStatus price_strikes(double S, std::span<const double> K,
                                                   double sigma, double q,
                                                   std::span<double> out) const noexcept;

  // L2h-c: the self-check prepare(…, AlSigmaQFamilyCheck::OffNode) measured (status Ok),
  // or the reason there is none (NotPrepared: not prepared, or prepared without the
  // check; GeometryRePrepared). Reads stored numbers: const, allocation-free. A sampled
  // estimate, not a bound (see AlSigmaQFamilySelfCheck).
  [[nodiscard]] AlSigmaQFamilySelfCheck self_check() const noexcept;

  // Heap bytes this family owns: the fixed state block (allocated at construction) plus
  // the node tensor sized at prepare() from (n_boundary, n_q, n_sigma) — n_boundary ·
  // n_q · n_sigma doubles; 0 tensor bytes while unprepared.
  [[nodiscard]] std::size_t state_bytes() const noexcept;

private:
  struct State; // defined in src/pricing/american_sigma_q_family.hpp (not public)
  friend struct sigma_q_family_detail::QFamilyAccess; // L2h-c: internal const accessor
  std::unique_ptr<State> st_;
};
// ── end L2h-b sigma-q family ──

// ── A0 sigma-q family Greeks (r6, 2026-10-23): the primary bundle on AlSigmaQFamily ──
//
// The (σ, q) family's price is one closed-form map of (S, K, σ, q) — K1's fused map
// (src/pricing/american_sigma_q_family.hpp): P = clamp(E + Π), Π the premium quadrature
// over the boundary B_i = xmax · exp(−√y_i), y interpolated tensor-barycentric in
// (σ, q) and barycentric in τ. Δ, Γ, vega, phi and rho below are EXACT derivatives of
// that served map, from ONE fused premium pass (no stencil, no re-solve): the family's
// OWN polynomial map, NOT the converged Andersen-Lake model (deep review F11; the
// distance to the converged model is measured in docs/sigma-q-family-greeks.md). θ is
// NOT a derivative of the map (the family has no τ axis): it is the model's
// continuation-region PDE identity evaluated with the map's V, Δ, Γ, so its error is not
// bounded by the map's accuracy (review-A0 m4) — and next to the exercise edge it is
// not served at all (the near-edge band below).
//   Δ, Γ   ∂/∂S of the integrand at FIXED boundary (the internal put's boundary does
//          not depend on its spot); calls through McDonald-Schroder and homogeneity,
//          as family_greeks on the σ family (above);
//   vega   K1's fused dP/dσ: the explicit σ-terms plus the boundary motion dy/dσ;
//   θ      the continuation-region PDE identity θ = rV − (r − q)SΔ − ½σ²S²Γ (NOT a
//          derivative of the map): calendar dV/dt per unit of the geometry's T;
//   phi    ∂P/∂q: the frozen-boundary terms PLUS the boundary motion dy/dq through the
//          tensor's q axis (nodal differentiation, exact for the interpolant); a
//          collapsed q box (n_q() == 1) has no q coordinate: NoQAxis;
//   rho    ∂P/∂r: the family has no r axis, so ONLY with an AlSigmaQFamilyRateBumps
//          (second families at r ± h): the frozen terms plus Σ ∂P/∂y · dy/dr in the
//          same pass; without one NaN with NoCarryBumps (the frozen-boundary rho is
//          never served — the L3g r4 ruling).
// Every requested Greek carries a typed AlFamilyGreekReason, NEVER a silent 0.0 marked
// served: the exercise regime (the σ-family rule — the spot on the exercise side of
// the current edge, or the served price within 1e-9 K of intrinsic) serves the
// exercise Greeks (Δ = −1 put / +1 call, the rest 0.0) with reason Exercised; a
// derivative the map does not have (the transform s = √y at its zero with a nonzero
// slope in that parameter) is NaN with NearNodeDegenerate; an unimplemented held-fixed
// convention is NaN with ConventionUnsupported; θ and Γ next to the exercise edge are
// NaN with NearExerciseEdge (below). Validity: S, K > 0, σ inside the σ box,
// q inside the q box; otherwise the row refuses with price()'s typed status and every
// requested Greek is NaN with Refused. The price field is the fused pass's price —
// price()'s map to rounding, not price()'s bits — whatever the mask. Accuracy (vs the
// family map, vs finite differences of the converged model inside / outside the
// near-boundary band, AVX2- vs scalar-built families) and cost are measured in
// docs/sigma-q-family-greeks.md. Const, allocation-free, thread-safe on a const family
// (stack scratch only). Nothing default selects any of this.
//
// THE NEAR-EDGE BAND (review-A0 MJ1). edge_sd = ln(S' / B'_now) / (σ√T) is the contract's
// distance to the family's CURRENT exercise edge in standard deviations, computed in the
// internal put's coordinates: S' the internal spot, B'_now = xmax · exp(−√y) at τ = T and
// the contract's (σ, q) — the very edge the Exercised rule tests (S' <= B'_now). For a put
// that is ln(S / S*) / (σ√T); for a call (internal S' = K, K' = S, homogeneity) it is
// ln(S* / S) / (σ√T), S* the option's own edge. It is > 0 on the continuation side, <= 0
// on the exercise side, NaN on a refused row. A continuation row with
// edge_sd < kAlFamilyGreekEdgeBand (0.25 σ√T) is IN THE BAND: θ and Γ are NaN with
// NearExerciseEdge (`near_edge`, flags bit 2). There θ → 0 is a cancellation of O(rK)
// terms of the identity, which amplifies the discretised map's Γ error near the edge
// (its approximate smooth pasting): the identity θ was measured up to 138 % off the
// converged calendar difference with the WRONG sign 0.007 σ√T from the edge, 25 % off at
// 0.016 σ√T (the L3g σ family's θ is the same number — the identity, not the (σ, q)
// interpolant), and Γ is not validated there. Δ, vega, phi and rho stay served (their
// in-band accuracy is in the docs).

// The Greeks of the bundle, in the order of AlFamilyGreekReasons.
enum class AlFamilyGreek : std::uint8_t {
  Delta = 0,
  Gamma = 1,
  Vega = 2,
  Theta = 3,
  Rho = 4,
  Phi = 5,
};
inline constexpr std::size_t kAlFamilyGreekCount = 6;

// The near-edge band's half-width in σ√T (the band defined above).
inline constexpr double kAlFamilyGreekEdgeBand = 0.25;

// Which Greeks a call computes (the price always is); an unrequested Greek is left 0.0
// with reason NotRequested.
struct AlFamilyGreekMask {
  std::uint8_t bits = 0;

  [[nodiscard]] static constexpr std::uint8_t bit(AlFamilyGreek g) noexcept {
    return static_cast<std::uint8_t>(1u << static_cast<unsigned>(g));
  }
  [[nodiscard]] static constexpr AlFamilyGreekMask none() noexcept { return {0}; }
  // Δ, Γ, vega, θ, phi — everything the family serves on its own (no second family).
  [[nodiscard]] static constexpr AlFamilyGreekMask primary() noexcept {
    return {static_cast<std::uint8_t>(bit(AlFamilyGreek::Delta) | bit(AlFamilyGreek::Gamma) |
                                      bit(AlFamilyGreek::Vega) | bit(AlFamilyGreek::Theta) |
                                      bit(AlFamilyGreek::Phi))};
  }
  // primary() plus rho (served only with a rate-bump set).
  [[nodiscard]] static constexpr AlFamilyGreekMask all() noexcept {
    return {static_cast<std::uint8_t>(primary().bits | bit(AlFamilyGreek::Rho))};
  }
  [[nodiscard]] constexpr bool has(AlFamilyGreek g) const noexcept {
    return (bits & bit(g)) != 0;
  }
  [[nodiscard]] constexpr AlFamilyGreekMask with(AlFamilyGreek g) const noexcept {
    return {static_cast<std::uint8_t>(bits | bit(g))};
  }
};

// Why a Greek holds what it holds. Served and Exercised carry a usable number; every
// other reason but NotRequested carries NaN. A zero-initialised reason is NotRequested.
enum class AlFamilyGreekReason : std::uint8_t {
  NotRequested = 0,          // not in the request mask: 0.0
  Served = 1,                // the derivative of the family's map (the convention below)
  Exercised = 2,             // the exercise regime: intrinsic's derivative BY CONSTRUCTION
                             // (Δ = −1 put / +1 call, every other Greek 0.0) — not the
                             // continuation map's slope
  Refused = 3,               // the row refused (status != Ok): NaN
  NoCarryBumps = 4,          // rho without a rate-bump set (the family has no r axis): NaN
  CarryBumpRefused = 5,      // rho with a rate-bump set that is unprepared or was built
                             // from another family: NaN
  NoQAxis = 6,               // phi on a collapsed q box (no q coordinate): NaN
  NearNodeDegenerate = 7,    // the boundary transform s = √y sits at its zero at a premium
                             // node with a nonzero slope in this Greek's parameter: NaN
  ConventionUnsupported = 8, // the requested held-fixed convention is not implemented: NaN
  NearExerciseEdge = 9,      // θ and Γ on a continuation row inside the near-edge band
                             // (edge_sd < kAlFamilyGreekEdgeBand): NaN
};
using AlFamilyGreekReasons = std::array<AlFamilyGreekReason, kAlFamilyGreekCount>;

// What a Greek holds fixed when its argument moves.
enum class AlFamilyGreekHeldFixed : std::uint8_t {
  ContractSigma = 0,   // the contract's σ (the call's sigma argument) held: implemented
  StickyStrike = 1,    // the surface's σ(K) at the contract's strike held: on a family
                       // priced at that σ these are the ContractSigma numbers: implemented
  StickyMoneyness = 2, // σ moves with K / S (needs dσ/dS of a fitted surface — A1 / H0):
                       // NOT implemented, every requested Greek ConventionUnsupported
  RefittedSurface = 3, // σ re-fit after the move: NOT implemented (ConventionUnsupported)
};

// Whose derivatives these are: the family's own polynomial map (K1's), boundary motion
// included — not the converged model's (F11).
enum class AlFamilyGreekModel : std::uint8_t {
  SigmaQFamily = 0,
};

// The units of θ / rho / phi: θ is dV/dt per unit of the geometry's T (the caller's year
// fraction on whatever day count it prepared the geometry with); rho / phi per unit of
// rate / yield. A time roll through expiry, an earnings date or a dividend is an event
// operation, not θ.
enum class AlFamilyGreekClock : std::uint8_t {
  GeometryTime = 0,
};

// Populated on every bundle the entries return (refusals included).
struct AlFamilyGreekConvention {
  AlFamilyGreekHeldFixed held_fixed = AlFamilyGreekHeldFixed::ContractSigma; // as requested
  AlFamilyGreekModel model = AlFamilyGreekModel::SigmaQFamily;
  AlFamilyGreekClock clock = AlFamilyGreekClock::GeometryTime;
  std::uint64_t surface_version = 0; // the caller's tag, echoed (a family carries none)
};

struct AlSigmaQGreeksRequest {
  AlFamilyGreekMask mask = AlFamilyGreekMask::primary();
  AlFamilyGreekHeldFixed held_fixed = AlFamilyGreekHeldFixed::ContractSigma;
  std::uint64_t surface_version = 0;
};

struct AlSigmaQFamilyGreeks {
  double price = std::numeric_limits<double>::quiet_NaN(); // NaN unless status == Ok
  double delta = 0.0;
  double gamma = 0.0;
  double vega = 0.0;
  double theta = 0.0; // calendar dV/dt (PDE identity), per unit of the geometry's T
  double rho = 0.0;   // ∂P/∂r: rate-bump set only
  double phi = 0.0;   // ∂P/∂q: the tensor's q axis
  AlSigmaQFamilyStatus status = AlSigmaQFamilyStatus::NotPrepared;
  AlFamilyGreekMask requested{};
  AlFamilyGreekMask supported{}; // requested Greeks holding a usable number (Served /
                                 // Exercised)
  AlFamilyGreekReasons reason{}; // per AlFamilyGreek (unrequested: NotRequested)
  bool exercised = false;        // the exercise Greeks were served
  bool floored = false;          // exercised on the CONTINUATION side of the current edge
  bool near_edge = false;        // a continuation row in the near-edge band: θ and Γ are
                                 // NearExerciseEdge
  double edge_sd = std::numeric_limits<double>::quiet_NaN(); // the distance to the current
                                 // exercise edge in σ√T (the band above); NaN unless Ok
  AlFamilyGreekConvention convention{};
  [[nodiscard]] AlFamilyGreekReason reason_of(AlFamilyGreek g) const noexcept {
    return reason[static_cast<std::size_t>(g)];
  }
};

// ── rho: the rate-bump set (second families at r ± h) ──
//
// The r-sensitivity of ONE prepared (σ, q) family's node tensor. prepare(base) builds, on
// geometries at (T, r ± h, q_geometry) with the base geometry's own AlOpts, two families
// with the base's side, anchor, σ box, q box, node counts and axis (under the CURRENT
// SimdIsa route), and keeps ONLY the central difference dY/dr of their tensors
// (n_boundary · n_q · n_sigma doubles; the bumped geometries and families are released
// before prepare() returns). h = kAlSigmaFamilyCarryBump. NO one-sided fallback: a
// refusal of either bumped family (leaving the American regime, the q-box accuracy guard
// at r ± h, a node that cannot move) is returned as is, (NotImplemented) a bumped family
// whose collocation grid differs from the base's; (InvalidArgument) an unprepared base
// or one whose geometry was re-prepared. Strong guarantee: on any refusal the set is
// unprepared. Cost: two family builds plus two geometry prepares — the reason rho is
// opt-in. Move-only, immutable after prepare(), thread-safe for every const use; it does
// NOT observe the base (identity is checked by value on every call: CarryBumpRefused on
// a mismatch).
class AlSigmaQFamilyRateBumps {
public:
  AlSigmaQFamilyRateBumps();
  ~AlSigmaQFamilyRateBumps();
  AlSigmaQFamilyRateBumps(AlSigmaQFamilyRateBumps &&) noexcept;
  AlSigmaQFamilyRateBumps &operator=(AlSigmaQFamilyRateBumps &&) noexcept;
  AlSigmaQFamilyRateBumps(const AlSigmaQFamilyRateBumps &) = delete;
  AlSigmaQFamilyRateBumps &operator=(const AlSigmaQFamilyRateBumps &) = delete;

  [[nodiscard]] Status prepare(const AlSigmaQFamily &base);
  [[nodiscard]] bool prepared() const noexcept;
  // Heap bytes this set owns (its state block plus the dY/dr tensor; 0 when empty).
  [[nodiscard]] std::size_t state_bytes() const noexcept;

private:
  struct State; // defined in src/pricing/american_sigma_q_family_greeks.cpp
  friend struct sigma_q_family_detail::QFamilyAccess; // A0: the Greeks TU's const reads
  std::unique_ptr<State> st_;
};

// Price + the requested Greeks of the (S, K) contract of the family's side at (sigma, q)
// (the contract's own effective yield). Refuses exactly as AlSigmaQFamily::price().
// A requested rho is NaN with NoCarryBumps (use the overload below).
[[nodiscard]] AlSigmaQFamilyGreeks family_greeks(const AlSigmaQFamily &fam, double S, double K,
                                                 double sigma, double q,
                                                 const AlSigmaQGreeksRequest &req = {}) noexcept;

// family_greeks(fam, …) with rho served from `rate`, which must have been prepared from
// `fam` (same side, anchor, (T, r), boxes, nodes, axis, quadrature, node tensor);
// otherwise a requested rho is NaN with CarryBumpRefused and every other field is the
// overload above's.
[[nodiscard]] AlSigmaQFamilyGreeks family_greeks(const AlSigmaQFamily &fam,
                                                 const AlSigmaQFamilyRateBumps &rate, double S,
                                                 double K, double sigma, double q,
                                                 const AlSigmaQGreeksRequest &req = {}) noexcept;

// Caller-owned structure-of-arrays outputs of family_greeks_many. A column is written iff
// it is non-empty, and then its length must equal the lane count; the mask decides what
// is COMPUTED, the spans what is STORED. `flags`: bit 0 exercised, bit 1 floored, bit 2
// near_edge; `edge_sd`: the bundle's edge_sd.
struct AlSigmaQGreekColumns {
  std::span<double> price{};
  std::span<double> delta{};
  std::span<double> gamma{};
  std::span<double> vega{};
  std::span<double> theta{};
  std::span<double> rho{};
  std::span<double> phi{};
  std::span<AlSigmaQFamilyStatus> status{};
  std::span<AlFamilyGreekMask> supported{};
  std::span<AlFamilyGreekReasons> reason{};
  std::span<std::uint8_t> flags{};
  std::span<double> edge_sd{};
};

// A chain on ONE family at spot S, lane i = (K[i], sigma[i], q[i]) (per-lane σ and q:
// a fitter's iterates, funding-pv yields). `active` empty = every lane; otherwise
// active[i] == 0 retires lane i: its inputs are not read and nothing of it is written.
// Every active lane is family_greeks(fam[, *rate], S, K[i], sigma[i], q[i], req) to the
// bit (a refused lane carries its status and NaN; it never poisons another lane); the
// strike-, σ- and q-free node data are laid once per call and the carry is reused while
// q repeats. `rate` null: a requested rho is NoCarryBumps. Allocation-free — the
// InvalidArgument refusal too (its message fits the small-string buffer; review-A0 m5) —
// const on every input, single-thread per call (lanes may be split across threads by
// the caller with disjoint spans).
// @return InvalidArgument when K / sigma / q / a non-empty `active` or column disagree
//         in length (nothing is written); otherwise Ok — per-lane outcomes in `status`.
[[nodiscard]] Status family_greeks_many(const AlSigmaQFamily &fam, double S,
                                        std::span<const double> K,
                                        std::span<const double> sigma,
                                        std::span<const double> q,
                                        std::span<const std::uint8_t> active,
                                        const AlSigmaQGreekColumns &out,
                                        const AlSigmaQGreeksRequest &req = {},
                                        const AlSigmaQFamilyRateBumps *rate = nullptr);
// ── end A0 sigma-q family Greeks ──

// ── Boundary-convergence evidence (what `andersen_lake` does NOT tell you) ──
//
// The Andersen-Lake exercise boundary is solved by a FIXED sweep budget with an
// early exit once the boundary residual max |Δy| falls to `AlOpts::tol`. Reaching
// tol is NOT a precondition of pricing: the solve stops when the budget is spent
// and returns the boundary it has, so `andersen_lake` succeeds either way. That is
// deliberate — a truncated budget is the fast tiers' normal operating point, and
// hard-failing it would turn every fast-tier mark into an error — but it does mean
// a successful price carries NO convergence claim of its own, and `tol` alone tells
// you what was ASKED for, never what was achieved.
//
// Measured 2026-08-23 on a 64-cell production-shaped grid (S=K=100, T in
// {0.5,1,2,5}, sigma in {0.2,0.4,0.6,0.9}, r in {0.03,0.08}, q in {0,0.02}, put):
// `al_fast_opts()` (tol 1e-8, 2 JN + 2 FP sweeps) converged on ZERO of them — pure
// Jacobi-Newton needs 17-24 sweeps to reach that tol there — while `andersen_lake`
// returned a price on all 64.
//
// This entry point reports the residual the solve ACTUALLY achieved for one cell,
// so a caller whose accuracy budget depends on it can gate instead of assume. It
// runs the identical validation / degenerate / regime path as `andersen_lake` and
// then the identical boundary solve, so its verdict describes the very solve that
// price would have used (Side::Call reports the McDonald-Schroder internal put's
// solve — strike S, rate q, yield r — which is the boundary a call actually rides).
struct AlBoundaryConvergence {
  double resid = 0.0;              // achieved max |Δy| of the last sweep run
  double tol = 0.0;                // the scheme tolerance the budget was asked to reach
  bool converged = false;          // resid <= tol, and a sweep actually measured it
  // Sweeps that actually ran and produced a residual. 0 means NOTHING was measured
  // and `resid` is not a measurement — which is the case for every regime that
  // prices without solving a boundary (below). Read it before trusting `resid`.
  std::uint16_t sweeps_run = 0;
  std::uint16_t sweep_budget = 0;  // sweeps the scheme affords (n_iter_jn + n_iter_fp)
};

// Convergence evidence for the boundary solve behind
// `andersen_lake(S, K, T, sigma, r, q, side, opts)`.
//
// @return the report, or the SAME Error `andersen_lake` would return for these
//         arguments (InvalidArgument / NotImplemented / Internal) — so a caller
//         needs no second regime classification of its own.
//
// Regimes that price without solving a boundary at all (degenerate T ~ 0 or
// sigma ~ 0, and the European no-early-exercise corner) report
// {resid = 0, converged = true, sweeps_run = 0, sweep_budget = 0}: nothing was
// left unconverged because nothing was solved.
[[nodiscard]] Result<AlBoundaryConvergence>
andersen_lake_convergence(double S, double K, double T, double sigma, double r, double q,
                          Side side, const std::optional<AlOpts> &opts = std::nullopt);

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
// ColdFallback (bit-identical to the direct solve). The measured
// accuracy/throughput and the ship decision are on `use_sigma_boundary_interp`
// immediately below, which carries the §P2.5 gate numbers it shipped on.
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

// R-30 observability: number of times the Andersen-Lake specialized sigma-bind hit
// its safety fallback (retained static geometry unexpectedly unbound, forcing the
// generic runtime-trip-count kernel). Monotonic, process-wide, thread-safe. Expected
// to stay 0 on every production flow; a non-zero value flags a retained-workspace
// lifecycle bug. Available in all build configs (the Debug bind-key assert is the
// louder Debug-only counterpart).
[[nodiscard]] std::uint64_t al_geometry_specialize_off_fallback_count() noexcept;

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

// Fused cached price + American vega for the IV-inversion Newton step (perf
// review F1 + F8). The residual and the vega both need the correction at the SAME
// (k_log, T, sigma), so this shares ONE correction value traversal between them:
//   price = american_price_cached(...)                              (bit-identical)
//   vega  = black76_value_and_vega(...).vega + F * ∂correction/∂σ    (F8 cheap leg)
// cutting the Newton step's tensor traversals from 3 (price eval + vega eval_grad's
// eval + its dsigma partial) to 2 in stage (a), and to ~1 once eval_value_and_dsigma
// fuses the value+∂σ pass in stage (b).
//
// The price leg is byte-for-byte american_price_cached (same Φ(−d) direct legs,
// same intrinsic floor, same NaN on the double-continuation regime / cold
// fallback). The vega leg is american_vega's Black-76 leg plus the served
// correction's gated sigma partial; because the shared value is taken at the price
// path's k_log = -ln(F/K), the vega's correction partial is evaluated at that same
// point (a sub-ULP shift from the standalone american_vega's ln(K/F)), making the
// American price and its vega mutually consistent. Vega carries the 0-on-degenerate
// scale the inverter reads as "force bisection".
struct AmericanPriceVega {
  double price;
  double vega;
};

[[nodiscard]] AmericanPriceVega american_price_and_vega_cached(double S, double K, double T,
                                                              double sigma, double r, double q,
                                                              Side side,
                                                              const CorrectionCache *correction);

[[nodiscard]] AmericanPriceVega american_price_and_vega_cached(double S, double K, double T,
                                                              double sigma, double r, double q,
                                                              Side side,
                                                              const CorrectionBlend &correction);

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
//
// K4 first-order tier: need_vega/need_rho/need_charm skip the boundary solves the
// requested greeks don't need — price+delta+gamma+theta ride the BASE solve alone;
// vega/volga/vanna gate the sigma+/- solves; rho the r+/- solves; charm the wide speed
// stencils. A hedge caller ({delta}) does ONE boundary solve (1 BoundarySolves ledger
// count) instead of five; unrequested greeks are left 0. Defaults keep the full 5-solve
// bundle, so every existing call site is byte-unchanged. The columns a reduced request
// returns are BIT-IDENTICAL to the full-bundle run (same base boundary + stencils).
[[nodiscard]] Result<AmericanGreeks>
american_greeks_al(double S, double K, double T, double sigma, double r, double q, Side side,
                   const std::optional<AlOpts> &opts = std::nullopt, bool need_vega = true,
                   bool need_rho = true, bool need_charm = true);

// Bit-identical to american_greeks_al(S, K, g.T(), sigma, g.r(), g.q(), side,
// opts-of-g, need_vega, need_rho, need_charm). The base and sigma +/- boundaries
// reuse the prepared geometry; the rate-bumped boundaries (another orientation) and
// every fallback path solve fresh. InvalidArgument when `g` is not prepared.
[[nodiscard]] Result<AmericanGreeks> american_greeks_al(const AlExpiryGeometry &g, double S,
                                                        double K, double sigma, Side side,
                                                        bool need_vega = true, bool need_rho = true,
                                                        bool need_charm = true);

// American delta ONLY (∂price/∂S) — the single sensitivity the strike-from-delta
// solver's bisection consumes, WITHOUT the full american_greeks_fd bundle's other
// eight axes (7 unique boundaries / 17 stencils, measured 7.07x). The
// put/AndersenLake fast lane
// exploits the spot-independent exercise boundary: ONE boundary solve + two
// price-from-boundary spot stencils, BIT-IDENTICAL to american_greeks_fd's delta.
// Calls / BAW / degenerate corners take a two-evaluation central difference on the
// cold american_price — the same stencil (and value) american_greeks_fd uses, at
// two solves instead of seven unique boundaries. So a delta-driven root-find
// (`resolve_strike_by_delta`) that repriced full greeks per candidate now solves
// ~1-2 boundaries per candidate at an unchanged strike. InvalidArgument on
// non-positive S/K/T/sigma;
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

// ── Carry / dividend sensitivities (G2, gaps-review finding 2) ────────────
//
// The carry axis `AmericanGreeks` omits: ∂P/∂q (continuous-yield sensitivity,
// "q-rho") and the per-event ∂P/∂Div vector. Kept in a SEPARATE struct rather
// than widening `AmericanGreeks` — that 8-greek SoA struct threads a large
// consumer ecosystem (surface archive fingerprints, laned SIMD greek kernels,
// the python binding, bit-pinned `operator==` comparisons), none of which the
// carry axis belongs in, and the ∂P/∂Div vector is variable-length.
struct CarryGreeks {
  double price = 0.0;       // the mark == american_greeks(...).price / fair_value
  double dP_dq = 0.0;       // ∂P/∂q  (continuous dividend-yield / carry sensitivity)
  bool q_one_sided = false; // A5: the q down-bump used a one-sided forward stencil
};

// American ∂P/∂q via q± early-exercise boundary re-solves — the analytic AL tier,
// symmetric to `american_greeks_al`'s r± (rho) machinery under the McDonald-
// Schroder map. For a PUT, q is the internal-put YIELD (internal rate = r > 0 is
// held fixed), so the q-stencil is ALWAYS central. For a CALL, q is the internal-
// put RATE; when the down-bump q - hq leaves the American regime the A5 pattern
// switches to a one-sided FORWARD stencil (mirroring `american_greeks_fd`'s
// rho_forward and near-expiry theta) rather than solving an unpriceable boundary.
// Because carry greeks bump only q (no spot stencil), NO homogeneity rescale is
// needed on either side — the result is bit-identical to `american_carry_greeks_fd`
// (both sides). `price` == `american_greeks_al(...).price`. Degenerate corners
// (T~0/σ~0) and the no-early-exercise regime defer to the exact FD reference.
// InvalidArgument on non-positive S/K/T/sigma; propagates any pricing error.
[[nodiscard]] Result<CarryGreeks>
american_carry_greeks_al(double S, double K, double T, double sigma, double r, double q, Side side,
                         const std::optional<AlOpts> &opts = std::nullopt);

// FD reference for ∂P/∂q: central (A5 one-sided) q± bumps of the cold
// `american_price` (the same method/opts the mark is priced with) — the q-analogue
// of `american_greeks_fd`'s rho and the FD-parity reference for the AL tier.
// `price` == fair_value(). InvalidArgument on non-positive S/K/T/sigma; propagates
// any american_price error (e.g. NotImplemented double-continuation corner).
[[nodiscard]] Result<CarryGreeks>
american_carry_greeks_fd(double S, double K, double T, double sigma, double r, double q, Side side,
                         AmericanMethod method = AmericanMethod::AndersenLake,
                         const std::optional<AlOpts> &opts = std::nullopt);

// Cached fixed-carry ∂P/∂q. The correction term is held FIXED across the carry
// bump exactly as the cached rho does (`american_greeks`): analytically
//   ∂P/∂q = -T·F·D,   D = ∂P/∂F = gB.delta + c - dc_dk   (the cached forward delta),
// which is the through-forward part of the fixed-carry rho with q's sign and
// without rho's discount-factor leg (q does not enter the discount). A null,
// unpopulated, or opposite-side cache degrades to the Black-76 (European) leg.
// Same InvalidArgument / double-continuation NotImplemented contract as
// `american_greeks`.
[[nodiscard]] Result<CarryGreeks>
american_carry_greeks(double S, double K, double T, double sigma, double r, double q, Side side,
                      const CorrectionCache *correction);
[[nodiscard]] Result<CarryGreeks>
american_carry_greeks(double S, double K, double T, double sigma, double r, double q, Side side,
                      const CorrectionBlend &correction);

// Compose per-event dividend sensitivities ∂P/∂D_i from the carry sensitivity and
// the escrowed-forward Jacobian, via the q_eff-bridge chain rule the discrete-
// dividend fit uses (discrete cash divs fold into F, absorbed as an effective
// continuous carry q_eff = r - ln(F/S)/T):
//   ∂P/∂D_i = (∂P/∂q)·(∂q_eff/∂D_i) = (-∂P/∂q / (F·T))·(∂F/∂D_i).
// @param dP_dq   american_carry_greeks_*(...).dP_dq evaluated at that q_eff.
// @param F, T    the forward the option was priced on and its year-fraction (> 0).
// @param dF_dDiv ∂F/∂D_i per event (dividend.hpp `hybrid_forward_div_jacobian`).
// @param dP_dDiv_out written out[i] = (-dP_dq/(F·T))·dF_dDiv[i]; must match dF_dDiv
//        in size. A non-finite / zero F·T writes zeros (no sensitivity available).
void american_dividend_sensitivities(double dP_dq, double F, double T,
                                     std::span<const double> dF_dDiv,
                                     std::span<double> dP_dDiv_out) noexcept;

// ── Early-exercise boundary + assignment-risk screen (G4, gaps finding 5) ─
//
// The Andersen-Lake early-exercise (critical) price B(T): the spot at which
// immediate exercise first becomes optimal for an option with time-to-expiry T.
// A PUT is exercised when spot <= B; a CALL when spot >= B. This exposes the SAME
// retained boundary state the pricer solves — the put boundary directly, the call
// boundary via the McDonald-Schroder reflection
//   B_call(T; K, r, q) = K^2 / B_put(T; K, q, r)   (swap rate<->yield on the
// internal put; verified: B_call * B_put(swapped) == K^2). The value returned is
// the boundary evaluated at the FULL time-to-expiry T (the internal Chebyshev
// node z = +1), so andersen_lake(B, K, T, sigma, r, q, side) sits exactly on the
// smooth-paste seam between the exercise and continuation regions.
//
// The near-expiry limit is the homogeneity scale the solver already carries
// (al_xmax_put); DERIVED FROM THE CODE'S regime math, NOT a textbook mnemonic:
//   put : B(0+) = K*min(1, r/q)   == K when r >= q,  else K*(r/q) < K
//   call: B(0+) = K*max(1, r/q)   == K when r <= q,  else K*(r/q) > K
// B(T) is monotone in T (DECREASING for the put toward the perpetual level B∞,
// INCREASING for the call). As T -> infinity B(T) approaches the perpetual
// boundary K*γ/(γ-1) (γ the in-regime root of the characteristic quadratic).
//
// @param K,T,sigma  strike / time-to-expiry / vol (K > 0; T,sigma ~ 0 collapse to
//                   the near-expiry limit above)
// @param r,q        continuously-compounded rate and dividend yield (finite)
// @return the critical price, or an Error:
//   InvalidArgument — K <= 0, non-finite/negative T or sigma, or non-finite r/q
//   OutOfRange      — the EUROPEAN regime (put r<=0 && r<=q / call q<=0 && q<=r):
//                     early exercise is never optimal, so NO finite boundary exists
//                     (it sits at S=0 for the put / S=+inf for the call). This is a
//                     documented sentinel: no fabricated price is returned.
//   NotImplemented  — the double-continuation regime (put q<r<=0 / call r<q<=0),
//                     matching andersen_lake, or an asymptotic-boundary collapse
//   Internal        — the Gauss-Legendre quadrature table was unavailable
[[nodiscard]] Result<double> exercise_boundary(double K, double T, double sigma, double r, double q,
                                               Side side,
                                               const std::optional<AlOpts> &opts = std::nullopt);

// A fast HEURISTIC screen (NOT a pricing statement) for whether a deep-ITM American
// option is a candidate for early exercise / assignment. It compares the carry
// BENEFIT of exercising now against the remaining time (extrinsic) value forfeited:
//   deep-ITM CALL: benefit = pending dividend income  q*S*T (an option holder
//                  forgoes dividends the stock pays; exercising captures them)
//   deep-ITM PUT : benefit = interest on the strike    r*K*T (received early on
//                  exercise and reinvested)
// `at_risk` is set when `carry_benefit > time_value` AND the option is in the money.
// This is a COARSE screen: it uses the linear (undiscounted) carry term r*K*T /
// q*S*T over the remaining life T and ignores the second-order carry cross term —
// for the exact early-exercise decision compare the spot to `exercise_boundary()`.
// `time_value` is measured against the cold Andersen-Lake mark (the same method
// `american_price` serves).
struct AssignmentRisk {
  bool at_risk = false;       // carry_benefit > time_value, and the option is ITM
  double margin = 0.0;        // carry_benefit - time_value  (signed; > 0 when flagged)
  double carry_benefit = 0.0; // q*S*T (call) / r*K*T (put) — the linear carry term
  double time_value = 0.0;    // american_price - intrinsic (>= 0)
};

// @return the assignment-risk screen, or the same error `american_price` surfaces
//         for these inputs (InvalidArgument on non-positive S/K/T/sigma;
//         NotImplemented on the double-continuation corner; etc.).
[[nodiscard]] Result<AssignmentRisk>
assignment_risk(double S, double K, double T, double sigma, double r, double q, Side side,
                const std::optional<AlOpts> &opts = std::nullopt);

// ── Discrete CASH dividends: the Vellekoop-Nieuwenhuis spliced CRR lattice ──
//
// Everything above prices on a CONTINUOUS carry: cash dividends, if they enter
// at all, do so escrowed out of spot (`forward_div_corrected`, dividend.hpp) or
// absorbed into an effective yield. That is a model choice, and on an underlier
// whose ex-dates land ON its option expiries it is a large one — measured on
// 9,155 SPY rows with `ddiv > 0`, escrowing costs 142.60 ticks of price MAE
// against the vendor mark where the spliced lattice below costs 6.96 (2.29 once
// the two far-dated merged quarters are placed on their true ex-dates). On the
// 5,202 `ddiv == 0` rows the same lattice sits at 0.09 ticks, which is what
// establishes that the rate / vol / year-fraction conventions were never the
// problem — only the dividend treatment was.
//
// The method is Vellekoop & Nieuwenhuis (2006), "Efficient Pricing of Derivatives
// on Assets with Discrete Dividends", Applied Mathematical Finance 13(3): ONE
// recombining Cox-Ross-Rubinstein lattice is built on the dividend-FREE process,
// and each ex-date is handled during the rollback by shifting the stock levels
// down by the cash amount, interpolating the continuation value back onto the
// unshifted recombining grid, and continuing. The tree therefore stays
// recombining (O(N^2) nodes) instead of splitting into a 2^d-branch forest.
//
// Three details are load-bearing; each was measured, and each is stated where it
// is implemented as well as here:
//
//   1. A dividend landing exactly ON the terminal step is applied to the
//      ANALYTIC payoff, never interpolated. The terminal payoff has a kink at
//      the strike and linear interpolation across it is the one place this
//      scheme carries avoidable O(grid-spacing) error. Applying the payoff
//      directly took a European check from 0.042 error to BIT-EXACT agreement
//      with the equivalent-strike lattice. It matters here rather than in
//      theory because SPY's ex-dates ARE expiry dates.
//   2. The American exercise test at an ex-step is applied against BOTH stock
//      levels — the CUM-dividend level and the post-dividend one — and the value
//      is the better of the two against continuation. A holder standing at the
//      ex-step may exercise before the cash comes off or an instant after it,
//      and the classical result (Roll 1977 / Geske 1979 / Whaley 1981;
//      re-confirmed numerically by Itkin, arXiv:2510.18159 §6.4) is that the
//      instant BEFORE an ex-date is the only place an American call exercises
//      early at all. Testing `post` alone understates a call's exercise value by
//      exactly the dividend wherever exercise binds; it is a no-op on a put,
//      whose cum-dividend exercise is dominated by the same exercise a moment
//      later. An earlier revision claimed admitting the cum-dividend test "made
//      agreement with the vendor mark strictly worse" — that is evidence about a
//      vendor, not about the option, and the omission was never free: the
//      exercise opportunity was recovered one lattice step early, so the price
//      converged from BELOW with an O(dt) bias. Measured against the exact
//      quadrature decomposition of this same model (S=100, K=90, T=0.5,
//      sigma=0.20, r=0.04, one 5.00 dividend at tau=0.25; exact
//      11.663913846210640) the error fell from -1.4539e-2 / -6.0211e-3 /
//      -2.7829e-3 to -2.1011e-3 / -1.3008e-3 / -9.4645e-4 at 301 / 601 / 1201
//      steps. The same right applies on the TERMINAL step, where no later step
//      can recover it, and its consequence there is exact rather than O(dt): a
//      dividend landing on an American CALL's own expiry cannot move its price.
//   3. The post-dividend level is floored at 0: a cash amount exceeding the
//      stock level at a low node would otherwise put the lattice at a negative
//      stock price and hand back a nonsense payoff. (Under a continuous yield
//      `q` the model-consistent present value of the removed cash is
//      `sum_i D_i * exp(-r*tau_i) * exp(-q*(T - tau_i))` — `q` accrues on the
//      escrowed cash too. That is a caller's parity identity, not this
//      function's output, but getting it wrong reads as a constant offset.)
//      Distinct from that floor, and equally load-bearing: below the LOWEST grid
//      node the continuation value is EXTRAPOLATED — quadratically through the
//      three lowest nodes, or linearly when only two survive — and never held
//      flat, and never clamped. `post = level[i] - amount` sits under `level[0]`
//      for the low nodes at EVERY splice, so this is the common case and not a
//      corner; a flat clamp says the option stops responding to spot there,
//      which an American put's exercise floor hides and a European put does not.
//      Measured against the exact European decomposition of this same model, one
//      6.00 dividend three steps into a 300-step year on a 100 stock: the flat
//      clamp cost +0.8753 on the call and -0.8787 on the put at the money, a
//      linear extrapolation -0.0944 on both, the quadratic -0.0073 on both. At
//      SPY scale (775.8 spot, 2.15 of cash on step 1 of 300) the flat clamp cost
//      +63.18 ticks on the call and -43.45 on the put where the extrapolation
//      costs -3.64 on each. It was also pinning the ruinous-dividend European put
//      at 96.146830 where the only defensible value is K*exp(-r*T) = 97.044553.
//
//      NOTHING is clamped in that branch, and the reason is a property of the
//      splice rather than a preference: it applies ONE LINEAR OPERATOR to the
//      value vector, and `cont_call - cont_put` is affine in the stock level, so
//      any linear extrapolation reproduces the parity difference EXACTLY while
//      giving both legs the identical absolute error. A per-node
//      `max(extrapolated, 0)` is not linear, desynchronises the two legs, and is
//      the only thing that can make European put-call parity fail here — it
//      measured a 4.96e-05 residual that vanishes to 1.68e-12 without it. The
//      corollary is the trap this scheme sets for its own tests: PUT-CALL PARITY
//      CANNOT SEE THIS BRANCH AT ALL. Both legs are wrong by the same amount and
//      the residual stays at machine precision, so only an ABSOLUTE reference
//      measures it (`vn_european_one_dividend` in the tests).
//
//      Quadratic rather than linear because option value is CONVEX in spot, so a
//      linear extension from the lowest segment is a rigorous lower bound and
//      undershoots — one-sidedly, by 12x more than the quadratic at an early
//      ex-date. The quadratic's divided-difference term is >= 0 on convex data,
//      so it only ever adds to the linear bound; where the true curvature decays
//      faster than quadratic it can overshoot slightly (measured +0.043 against
//      the linear -0.302 at K=140), which is a 7x improvement in magnitude and
//      the reason it is not guarded.
//
// THREE entry points, in increasing cost. `american_discrete_div_price` is one
// rollback. `american_discrete_div_greeks` is the SAME rollback, additionally
// reporting the delta and gamma its first two time steps already carry — both
// free. `american_discrete_div_greek_bundle` is the nine greeks the oracle
// scores, at EIGHT rollbacks rather than the nineteen a naive two-sided bump of
// every axis would cost; its accounting, its bump sizes and its two theta
// conventions are documented at its own declaration below.

// One discrete CASH dividend on the option's own clock.
//
// `tau` is a year-fraction measured from the SAME valuation instant as the
// option's `T`, so a schedule is reusable across every expiry of one chain.
// Dividends outside `(0, T]` are IGNORED, not rejected — already-paid and
// after-expiry events are a normal property of a shared chain-wide schedule.
// That is the TAU-SPACE analogue of `forward_div_corrected`'s window, not the
// same test: this type carries no instants, so `(0, T]` is the only window
// expressible here, and it is sound ONLY while `tau` is on the option's own
// clock as the paragraph above requires. `forward_div_corrected` compares
// INSTANTS, which is clock-independent, and no longer screens on `T` at all. A
// `tau` marginally past `T` (within `kTauAtExpiryRelTol`) still counts as
// landing AT expiry, so a schedule whose ex-date was reconstructed from the same
// year-fraction column as `T` is not silently dropped by one ulp.
struct CashDividend {
  double tau = 0.0;    // ex-date, year-fraction from valuation; admitted on (0, T]
  double amount = 0.0; // cash per share, finite and >= 0 (0 is a no-op)
};

// The vendor documents 301 steps for this method, and 301 is what reproduces
// its marks: on the dividend-free control rows the 301-vs-601 spread is 0.040
// ticks, ~58x smaller than the residual the dividend treatment itself carries,
// so the step count is not the binding constraint on agreement.
inline constexpr int kDiscreteDivDefaultSteps = 301;

// A `tau` this far past `T` (relatively) is still treated as landing AT expiry.
// PUBLIC because `discrete_div_route` (dividend.hpp) has to apply the SAME
// window this engine applies — its whole contract is that the two routes see the
// same cash — and a duplicated literal is how that quietly stops being true.
inline constexpr double kTauAtExpiryRelTol = 1.0e-12;

// Bounded-loop / allocation guard (JPL rule 2). The rollback is O(steps^2) and
// each lattice buffer is `steps + 1` doubles; a caller asking for more than this
// gets InvalidArgument rather than an unbounded loop.
inline constexpr int kDiscreteDivMaxSteps = 20001;

// ── Lattice scheme: strike smoothing and the escrowed variant (opt-in) ──────
//
// WHY THIS EXISTS. A strike-by-strike lattice price must pass
// `AmericanPriceSlice` certification (fixed 1e-8 + 1e-9*max(S, K) tolerance)
// when each strike is priced at its OWN fitted vol. Two separate defects stop the
// spliced CRR doing that on dividend-paying chains (per-strike diagnosis in
// sprints/2026-10-17-smooth-div-sprint.md):
//   1. LATTICE NOISE. A plain CRR price is piecewise LINEAR in strike between
//      terminal nodes (the terminal distribution is a staircase), and per-strike
//      vols move the nodes between neighbouring strikes: concave kinks of 1e-2
//      to 1e-1 on dev200 ASML, refused at 301 to 4801 steps alike.
//   2. MODEL MISMATCH. The vol curve is fitted convex in BLACK76 PRICE space at
//      the dated-cash forward, and a dense convex fit is TIGHT (zero butterflies
//      on runs of strikes). The spliced lattice's European is not Black76: the
//      cash-dividend distribution differs from the lognormal forward by a smooth
//      term (0.02-0.30 on ASML 2027-04-16) that is concave in the wings, so the
//      re-priced slice is non-convex on every tight run — still at 4801 steps
//      with Richardson. No lattice refinement removes it.
// The BBS final step removes (1). Only a scheme whose European converges to
// Black76 at the dated-cash forward removes (2), and only one whose European is
// ALSO accurate to well under the tolerance at the served step count survives
// the tightest runs: the escrowed Leisen-Reimer tree below. (Control variates
// against Black76 were built and measured too — they fix the dense chains and
// break tightly LINEAR deep-in-the-money runs, which they re-bend with the
// European's residual error — and were dropped; see the sprint note.)
//
//   SplicedCrr  the V&N spliced CRR rollback exactly as documented above.
//         DEFAULT, and bit-identical to every price this function returned
//         before the option existed.
//   SplicedBbs  Broadie-Detemple (1996): the continuation value at the LAST
//         step before expiry is the analytic Black-Scholes value over that
//         final step instead of the rollback of the kinked payoff, so each node
//         at step N-1 spreads a lognormal over the last step and the price is a
//         smooth function of (strike, sigma). Cash on the terminal (expiry)
//         step stays analytic: the European payoff becomes a call struck at
//         K + D, or a put struck at K + D minus a put struck at D (exact,
//         including the zero floor on the post-dividend level); an AMERICAN
//         call may exercise cum-dividend at expiry, which makes its payoff the
//         undiluted (S - K)+, so the dividend cannot move it (the ledger's
//         cum-dividend rule, now analytic). Cash landing ON step N-1 is applied
//         to that analytic continuation at the post-dividend level directly, so
//         that splice interpolates nothing and needs no grid-bottom
//         extrapolation; every earlier ex-step splices exactly as in SplicedCrr
//         (same interpolation, same quadratic grid-bottom rule, same cum- and
//         post-dividend exercise test).
//   SplicedBbsr  SplicedBbs plus two-point Richardson extrapolation in the step
//         count: (N*P_N - M*P_M) / (N - M) with M = N / 2 (integer division),
//         the generalisation of 2*P_N - P_{N/2} to odd N. An American price is
//         floored at its intrinsic value afterwards (extrapolation can step
//         under an exercise plateau). One extra rollback at a quarter of the
//         work; `steps >= 2`. It AMPLIFIES the exercise-boundary oscillation
//         (measured), so it is an accuracy option, not a smoothing one.
//   EscrowedBbs  A DIFFERENT MODEL, stated as such: the lattice carries the
//         escrowed stock X = S - sum D_i e^{-(r-q) tau_i} (cash still to come,
//         discounted at the carry), lognormal with vol `sigma`, and the exercise
//         test values the stock as X + escrow(t), stepping down at each ex-date
//         — dated exercise, cum- and post-dividend at an ex-step exactly as the
//         splice tests it, but nothing interpolated and no grid bottom. Its
//         European converges to Black76 at the SAME dated-cash forward
//         F = S e^{(r-q)T} - sum D_i e^{(r-q)(T - tau_i)}, so the curve's
//         convexity carries over, and the BBS final step makes it smooth in
//         (strike, sigma). The vol is the escrowed stock's, not the spot's (the
//         classical escrowed-model distinction); a quote fit absorbs that
//         because it inverts and reprices in one model. Its residual O(1/N)
//         European error is smooth but still bends the tightest runs (measured).
//         OutOfRange when the cash exhausts the spot.
//   EscrowedLeisenReimer  The escrowed model on a Leisen-Reimer (1996) tree:
//         Peizer-Pratt inversion makes the up probability N(d2) and the up
//         factor N(d1)/N(d2) AT THIS STRIKE, so the tree is centred on K — the
//         strike-aligned lattice, moving continuously with (K, sigma) — and its
//         European converges smoothly at O(1/n^2) to the same Black76. Plain
//         terminal payoff (the centring makes a BBS step unnecessary). An even
//         `steps` runs at `steps + 1` (Leisen-Reimer's odd-n convention). Where
//         the strike is so far from the money that a Peizer-Pratt probability
//         rounds to exactly 0 or 1 (|d1| or |d2| beyond about 104 at 301 steps,
//         207 at 1201; the bound grows like sqrt(n)), the centred tree does not
//         exist and the CRR geometry prices that strike (it is worth its
//         forward intrinsic on any tree). OutOfRange when the cash exhausts the
//         spot.
//
//         The escrowed schemes are a different MODEL, so a quote slice fitted
//         under them reports the ESCROWED stock's volatility (the stock net of
//         the cash in the option's life), not a spot volatility: comparable
//         with another escrowed fit, not one for one with a spot-vol mark.
//
// The scheme is a PRICE option: `american_discrete_div_greeks`, the greek bundle
// and the dividend sensitivities keep the spliced CRR rollback. Every scheme
// uses the caller's step count for its (fine) lattice and the same per-call
// lattice workspace; BBSR's coarse rollback reuses the fine one's buffers.
enum class DiscreteDivScheme : std::uint8_t {
  SplicedCrr,
  SplicedBbs,
  SplicedBbsr,
  EscrowedBbs,
  EscrowedLeisenReimer,
};

// Stable names for provenance and policy files: "spliced", "spliced-bbs",
// "spliced-bbsr", "escrowed-bbs", "escrowed-lr".
[[nodiscard]] const char *to_string(DiscreteDivScheme value) noexcept;

// True when `scheme` names a scheme and `steps` can carry it: Richardson needs a
// coarse lattice of at least one step, so `steps >= 2`. The step RANGE itself
// is `american_discrete_div_price`'s own validation.
[[nodiscard]] constexpr bool discrete_div_scheme_admits(DiscreteDivScheme scheme,
                                                        int steps) noexcept {
  switch (scheme) {
  case DiscreteDivScheme::SplicedCrr:
  case DiscreteDivScheme::SplicedBbs:
  case DiscreteDivScheme::EscrowedBbs:
  case DiscreteDivScheme::EscrowedLeisenReimer:
    return steps >= 1;
  case DiscreteDivScheme::SplicedBbsr:
    return steps >= 2;
  }
  return false;
}

// The coarse step count BBSR extrapolates against (`steps / 2`).
[[nodiscard]] constexpr int discrete_div_richardson_coarse_steps(int steps) noexcept {
  return steps / 2;
}

// The step count whose time step bounds the CRR probability window for a
// scheme: the coarse lattice under BBSR, the caller's otherwise. PUBLIC because
// a caller that brackets an implied volatility must keep the CRR probability
// inside (0, 1) on EVERY lattice the scheme runs.
[[nodiscard]] constexpr int discrete_div_coarsest_steps(int steps,
                                                        DiscreteDivScheme scheme) noexcept {
  return scheme == DiscreteDivScheme::SplicedBbsr ? discrete_div_richardson_coarse_steps(steps)
                                                  : steps;
}

// Price plus the two FREE lattice-derived greeks — the cheap tier. Deliberately
// not `AmericanGreeks`; the wide bundle is `DiscreteDivGreekBundle` below, and
// it costs seven more rollbacks.
struct DiscreteDivGreeks {
  double price = 0.0; // == american_discrete_div_price(...) for the same inputs
  double delta = 0.0; // (V1[1] - V1[0]) / (S1[1] - S1[0])           — step 1
  double gamma = 0.0; // central second difference over step 2's three nodes
};

// V&N spliced-CRR price with discrete cash dividends.
//
// @param S,K      spot and strike (> 0)
// @param T        year-fraction to expiry (> 0)
// @param sigma    annualized lognormal vol (> 0)
// @param r,q      continuously-compounded rate and residual continuous yield
//                 (finite). `q` is the yield that remains AFTER the discrete
//                 cash schedule — passing a yield that already contains the
//                 cash dividends double-counts them.
// @param side     Call or Put
// @param dividends discrete cash schedule; any order, duplicates allowed
//                 (amounts landing on one lattice step are summed). Events
//                 outside (0, T] are ignored. An EMPTY span reduces BIT-EXACTLY
//                 to the plain CRR lattice — the no-dividend path is the same
//                 loop with an empty step map, not a second implementation.
// @param steps    lattice steps, [1, kDiscreteDivMaxSteps]
// @param exercise American (default) or European. European exists because the
//                 identities that VALIDATE this engine — put-call parity and
//                 Black-Scholes convergence — are European statements.
// @param scheme   lattice scheme (`DiscreteDivScheme` above). SplicedCrr, the
//                 default, is the historical rollback bit for bit.
// @return the option premium, or an Error:
//   InvalidArgument — non-positive S/K/T/sigma, non-finite r/q, `steps` outside
//                     its range, a non-finite / negative dividend field, or a
//                     scheme `discrete_div_scheme_admits` refuses
//   OutOfRange      — the CRR risk-neutral probability left (0, 1): the step is
//                     too coarse for this (r - q, sigma, T). Raise `steps`.
//                     Under SplicedBbsr the COARSE lattice's step decides it
//                     (`discrete_div_coarsest_steps`). Also: under the escrowed
//                     schemes, cash that exhausts the spot.
[[nodiscard]] Result<double>
american_discrete_div_price(double S, double K, double T, double sigma, double r, double q,
                            Side side, std::span<const CashDividend> dividends,
                            int steps = kDiscreteDivDefaultSteps,
                            ExerciseStyle exercise = ExerciseStyle::American,
                            DiscreteDivScheme scheme = DiscreteDivScheme::SplicedCrr);

// The same lattice, additionally reporting the delta and gamma its first two
// time steps already carry. `price` is bit-identical to
// `american_discrete_div_price` for the same inputs (one shared kernel).
//
// Both greeks are with respect to the DIVIDEND-FREE lattice level, which at
// step 0 is `S` itself — the splice always interpolates back onto the
// unshifted grid, so the step-1 and step-2 nodes are the plain CRR levels.
//
// Same error contract as above, plus: InvalidArgument when `steps < 2` (gamma
// needs three step-2 nodes).
[[nodiscard]] Result<DiscreteDivGreeks>
american_discrete_div_greeks(double S, double K, double T, double sigma, double r, double q,
                             Side side, std::span<const CashDividend> dividends,
                             int steps = kDiscreteDivDefaultSteps,
                             ExerciseStyle exercise = ExerciseStyle::American);

// ── The nine-greek bundle ────────────────────────────────────────────────
//
// The oracle scores NINE greeks, so the price-plus-two tier above cannot
// replace the escrowed-spot path it beats on price. EIGHT rollbacks buy all
// nine, versus the nineteen (one base + two per axis, plus two spot x vol and
// two spot x time cross stencils) a naive bump-everything bundle would spend:
//
//   1 base solve       price, delta, gamma, theta, charm. FIVE outputs from one
//                      rollback: delta off step 1, gamma off step 2, theta off
//                      the step-2 node that sits at the ROOT's own stock level
//                      (u*d == 1), charm off the step-1 and step-3 deltas, which
//                      straddle that same node pair for the same reason.
//   2 sigma solves     vega  — central first  difference of the PRICE
//                      volga — central second difference of the PRICE
//                      vanna — central first  difference of the lattice DELTA,
//                              because vanna IS d(delta)/d(sigma) and each
//                              sigma-bumped rollback already carries its delta.
//                      Three greeks, one bump pair, no spot x vol cross stencil.
//   2 rate solves      rho = dP/dr
//   2 yield solves     phi = dP/dq
//   1 maturity solve   the SECANT theta (below). SKIPPED — 7 solves, not 8 —
//                      when `T - horizon <= 0` collapses the bumped leg to the
//                      intrinsic.
//
// TWO thetas, deliberately, because the oracle is deciding between them on
// measured evidence and a bundle that ships only one pre-empts that decision:
//
//   `theta`        the lattice's own time derivative, in the SAME calendar
//                  convention and PER-YEAR units as `AmericanGreeks::theta`
//                  (dP/dt = -dP/dT, so decay is NEGATIVE). Read as
//                  (V[2][1] - V[0][0]) / (2*dt): node (2,1) is the same stock
//                  level as the root, two steps later.
//   `theta_secant` SpiderRock's documented definition — "the difference between
//                  the current option price and the option price calculated
//                  with volatility time decreased by 1/252 years", reported
//                  POSITIVE as decay. It is P(T) - P(T - horizon), and it is
//                  ALREADY a one-period quantity: dividing it by a day count a
//                  second time is the double-scaling trap this field exists to
//                  make visible, and the tests pin it.
//
// The two are not interchangeable and must not be averaged: `theta` is a rate
// per year, `theta_secant` is a dollar amount per `horizon`, and the secant
// carries the curvature of P in T across a whole day where the tangent does not.
//
// Dividend RE-ANCHORING under the maturity bump: the bumped leg re-solves at
// `T - horizon` and passes the SAME schedule, so the engine's own `(0, T]`
// window drops every ex-date past the bumped expiry. Dropped, never clipped
// onto the new terminal step — a dividend the option no longer lives to receive
// contributes nothing, and moving it to `T - horizon` would instead price a
// cash flow that does not exist.
struct DiscreteDivGreekBundle {
  double price = 0.0; // == american_discrete_div_price(...), bit-identical
  double delta = 0.0; // == american_discrete_div_greeks(...).delta
  double gamma = 0.0; // == american_discrete_div_greeks(...).gamma
  double vega = 0.0;  // dP/dsigma, per 1.0 of vol (NOT per vol point)
  double theta = 0.0; // dP/dt, calendar convention, per YEAR (decay < 0)
  double rho = 0.0;   // dP/dr
  double phi = 0.0;   // dP/dq — the axis `AmericanGreeks` omits (see CarryGreeks)
  double vanna = 0.0; // d(delta)/d(sigma) == d^2P/dS dsigma
  double volga = 0.0; // d^2P/dsigma^2
  double charm = 0.0; // d(delta)/dt == -d^2P/dS dT, calendar (delta decay)
  double theta_secant = 0.0; // P(T) - P(T - horizon), POSITIVE as decay

  [[nodiscard]] bool operator==(const DiscreteDivGreekBundle &) const = default;
};

// The sigma bump the vega / volga / vanna triple is differenced over, as a
// FRACTION of sigma. It is deliberately two orders of magnitude coarser than
// american.cpp's analytic-pricer `hv = 1e-3`, and the reason is the lattice:
// a CRR price carries an O(S/steps) node-quantization ripple in sigma (the
// strike's position between terminal nodes moves as the node spacing
// sigma*sqrt(dt) moves), which a 1e-3 bump would divide by 2e-3 and report as
// vega. A 10% RELATIVE bump keeps the central-difference truncation error
// (O(h^2)) far below that ripple on every greek that divides by h, is
// scale-free across the vol range, and can never drive `sigma - h` non-positive.
// Second differences still divide the same ripple by h^2 — see the accuracy note
// on `volga` in the tests: away from the money it is the noisy member of the
// bundle, and that is a property of the lattice, not of the bump.
//
// This constant is NOT a tuning knob, and the tests pin why. Swept over 2%-90%
// of sigma at two step counts and five strikes, the volga error versus bump is
// not the U-shape a truncation/noise trade-off would give — it is the ripple's
// phase, so the best bump is 0.05 at the money, 0.30 one strike below and 0.90
// one strike above, and none of it transfers. Widening to 30% costs the
// AT-THE-MONEY volga 11.6x and vanna, which shares these two solves, 10-100x.
inline constexpr double kDiscreteDivSigmaBumpRel = 0.10;

// Rate / yield bumps, matching american_greeks_fd's `hr`/`hq`. Small is safe
// here in a way it is not for sigma: `r` and `q` do not move a single lattice
// node (levels are S*exp(sigma*sqrt(dt)*rung)), so they cannot move the ripple.
inline constexpr double kDiscreteDivRateBump = 1.0e-4;
inline constexpr double kDiscreteDivYieldBump = 1.0e-4;

// One trading day of 252 — the horizon in SpiderRock's own theta definition.
inline constexpr double kDiscreteDivThetaSecantHorizon = 1.0 / 252.0;

// The absolute sigma bump for `sigma`. Exposed so a caller (or a test) can
// reproduce the bundle's stencil exactly rather than re-deriving the rule.
[[nodiscard]] constexpr double discrete_div_sigma_bump(double sigma) noexcept {
  return kDiscreteDivSigmaBumpRel * sigma;
}

// The nine oracle greeks off the V&N spliced lattice. Same (S, K, T, sigma, r,
// q, side, dividends, steps, exercise) contract as the two entries above, so
// `price`, `delta` and `gamma` are bit-identical to theirs.
//
// @param theta_secant_horizon  the SpiderRock secant's time step, in years
//        (> 0, finite). Defaults to 1/252. When `T - theta_secant_horizon <= 0`
//        the bumped leg is NOT clamped to an epsilon maturity: SpiderRock's
//        companion rule (American -> European with rate = sdiv = carry = 0)
//        taken to its limit at zero remaining time IS the intrinsic payoff, so
//        the intrinsic is what the leg uses, and its at-the-money kink resolves
//        to the OUT-of-the-money side (max(sgn*(S - K), 0) is 0 at S == K).
//
// @return the bundle, or an Error:
//   InvalidArgument — everything `american_discrete_div_price` rejects, plus
//                     `steps < 3` (charm reads a step-3 delta) and a
//                     non-finite / non-positive `theta_secant_horizon`
//   OutOfRange      — propagated from ANY of the eight solves, including a
//                     bumped one: a (r ± h, q ± h) state can leave the CRR
//                     probability's (0, 1) window where the base state did not.
[[nodiscard]] Result<DiscreteDivGreekBundle> american_discrete_div_greek_bundle(
    double S, double K, double T, double sigma, double r, double q, Side side,
    std::span<const CashDividend> dividends, int steps = kDiscreteDivDefaultSteps,
    ExerciseStyle exercise = ExerciseStyle::American,
    double theta_secant_horizon = kDiscreteDivThetaSecantHorizon);

// ── Per-event dividend sensitivities off the lattice ─────────────────────
//
// dP/dD_i for EACH cash event, priced where the event actually lands.
//
// The escrowed route cannot do this, and the reason is structural rather than a
// question of accuracy. It folds the whole schedule into one scalar carry
// (q_eff = r - ln(F/S)/T) before any early-exercise boundary is touched, so an
// individual event's only channel is the forward, and
// `american_dividend_sensitivities` above accordingly composes
//   dP/dD_i = (-dP/dq / (F*T)) * dF/dD_i,   dF/dD_i = -(1-blend)*exp(r*(T-t_i))
// — a FIXED scalar times a pure discount factor. The ratio between any two
// events is then exp(r*(t_j - t_i)) and NOTHING else: not the side, not the
// strike, not whether one of them triggers early exercise. Two schedules with
// the same present value and different timing get identical answers. Measured
// on S=K=100, T=1, sigma=0.25, r=0.05 with 6.00 at tau=0.10 and 6.00 at
// tau=0.90, the lattice ranks the early event 1.7509x the late one for a CALL
// and 0.8214x for a PUT — opposite orderings on one schedule — where the chain
// rule says 1.0408 for both.
//
// Cost: TWO rollbacks per in-window event, central-differenced in the amount.
// The ex-date's lattice step does not move with the bump, so the step-rounding
// error is common to both legs and cancels.
//
// @param dP_dDiv_out one slot per `dividends` element, same order; sizes must
//        match exactly. An event outside (0, T] — which the engine drops from
//        the price — reports exactly 0, the same convention
//        `hybrid_forward_div_jacobian` uses for its own window.
// @return Ok, or everything `american_discrete_div_price` rejects, plus
//         InvalidArgument on a size mismatch.
[[nodiscard]] Status american_discrete_div_dividend_sensitivities(
    double S, double K, double T, double sigma, double r, double q, Side side,
    std::span<const CashDividend> dividends, std::span<double> dP_dDiv_out,
    int steps = kDiscreteDivDefaultSteps, ExerciseStyle exercise = ExerciseStyle::American);

// The bump each amount is central-differenced over: a fraction of the amount,
// with an absolute floor so a zero (or tiny) event still gets a usable step.
// Unlike the sigma bump there is no lattice ripple to dodge here — a dividend
// amount moves the interpolation ARGUMENT, not the node positions — so this is
// an ordinary truncation-versus-roundoff choice and small is safe.
inline constexpr double kDiscreteDivAmountBumpRel = 1.0e-3;
inline constexpr double kDiscreteDivAmountBumpAbs = 1.0e-4;

// Exposed so a caller (or a test) can reproduce the stencil exactly rather than
// re-deriving the rule. A down-leg that would go negative is clamped at 0 and
// the difference becomes one-sided over the true interval; that is why callers
// must not assume the denominator is 2h.
[[nodiscard]] constexpr double discrete_div_amount_bump(double amount) noexcept {
  const double rel = kDiscreteDivAmountBumpRel * amount;
  return (rel > kDiscreteDivAmountBumpAbs) ? rel : kDiscreteDivAmountBumpAbs;
}

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
//                   EXACTLY (rate < 0 && rate <= yield, or rate == 0 && yield >= 0).
//   - Unsupported : early exercise IS possible but a double continuation region
//                   appears (yield < rate < 0 — STRICTLY negative rate);
//                   the single-boundary ALO scheme cannot represent two exercise
//                   boundaries (Battauz-De Donno-Sbuelz 2015, Mgmt Sci 61(5);
//                   Andersen-Lake 2021). Callers return NotImplemented / NaN
//                   rather than a silently-wrong European price.
//   - American    : the standard single-boundary early-exercise regime (rate > 0),
//                   PLUS the rate == 0 && yield < 0 edge. The second boundary
//                   exists only because a STRICTLY negative rate makes the
//                   early-received strike decay, so waiting deep ITM regains
//                   value; at rate exactly 0 the negative yield only drifts the
//                   internal-put spot UP, the exercise region stays
//                   downward-connected, and al_xmax_put(K, r=0, q<0) == K
//                   already encodes the single boundary. This row used to be
//                   lumped into Unsupported, which NaN-killed every r=0
//                   de-Americanization whose PCP borrow iterate landed at
//                   q_eff = -eps (whole boards died "no expiry produced a
//                   usable eSSVI slice"). Gate:
//                   NegRateDomainMap.ZeroRateNegativeYield_IsSingleBoundaryAmerican.
//
// THE SERVED OUTPUT IS DISCONTINUOUS IN `rate` AT 0 FOR yield < 0, and that is
// a property of what this library can COMPUTE, not of the option's value. Hold
// `yield < 0` and sweep `rate` upward through 0: at `yield < rate < 0` the cell
// is Unsupported and every pricing entry returns NotImplemented / NaN; at
// `rate == 0` it is American and a price is served; for `rate > 0` it stays
// American. So an arbitrarily small change in `rate` across 0 flips "no answer"
// to "an answer" — a jump in SERVABILITY. The true American value is continuous
// across that boundary; the double-continuation region simply has a second
// exercise boundary the single-boundary ALO scheme cannot represent, so the
// library refuses rather than serving a silently-wrong single-boundary number.
//
// Consequences for callers, all deliberate: a carry solve or root-find that
// walks `rate` through 0 with a negative yield sees its objective become
// undefined on one side of 0 and defined AT 0 (do not assume a continuous
// objective there); and a sensitivity taken by bumping `rate` around 0 in that
// corner will bump into the refusal rather than a number. Widening the served
// set to `rate < 0` needs a two-boundary scheme, not a predicate change — this
// note documents the edge, it does not propose moving it.
enum class ExerciseRegime : std::uint8_t { European, Unsupported, American };

[[nodiscard]] inline ExerciseRegime classify_regime(double rate, double yield) noexcept {
  if (rate > 0.0) {
    return ExerciseRegime::American;
  }
  if (rate == 0.0) {
    return (yield < 0.0) ? ExerciseRegime::American : ExerciseRegime::European;
  }
  return (rate <= yield) ? ExerciseRegime::European : ExerciseRegime::Unsupported;
}

} // namespace detail

} // namespace atx::vol
