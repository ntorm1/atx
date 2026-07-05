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
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "atx/vol/types.hpp"

namespace atx::vol {

// Defined in correction.hpp — observed (non-owning) by the cached pricer/Greeks.
class CorrectionCache;

// ── Andersen-Lake tuning knobs ──────────────────────────────────────────
//
// Mirrors the C `AtsVolALOpts`. The public knobs are mapped internally to a
// QuantLib-style accuracy preset (boundary nodes, fixed-point / premium
// quadrature orders, and sweep counts).
struct AlOpts {
  std::uint16_t n_collocation = 12;   // Chebyshev boundary nodes
  std::uint16_t n_quadrature = 24;    // Gauss-Legendre fixed-point order
  std::uint16_t max_newton_iter = 8;  // total Jacobi-Newton + fixed-point sweeps
  double tol = 1.0e-10;               // convergence tol on the boundary residual
};

// The C `ats_pricer_al_default_opts()`: {12, 24, 8, 1e-10}.
[[nodiscard]] AlOpts al_default_opts() noexcept;

// Fast ALO preset for the surface-fit hot path: {7, 16, 6, 1e-8}. Maps to a
// 7-node boundary, order-16 Gauss-Legendre for BOTH the fixed-point and premium
// integrals, and 2 Jacobi-Newton + 4 fixed-point sweeps. ~3-6x cheaper per solve
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
//                NotImplemented  — negative-rate corner where the asymptotic
//                                  boundary collapses (xmax <= 0)
//                Internal        — quadrature-table construction failed
[[nodiscard]] Result<double> andersen_lake(double S, double K, double T,
                                           double sigma, double r, double q,
                                           Side side,
                                           const std::optional<AlOpts>& opts = std::nullopt);

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
// corner (q <= 0) writes the European call per strike.
//
// @return InvalidArgument — S <= 0, negative T/sigma, a non-positive strike, or a
//                           span-length mismatch
//         NotImplemented  — the negative-carry corner where the asymptotic
//                           boundary collapses (matches `andersen_lake`)
[[nodiscard]] Status andersen_lake_call_slice(
    double S, std::span<const double> strikes, double T, double sigma, double r,
    double q, std::span<double> price_out,
    const std::optional<AlOpts>& opts = std::nullopt);

// Barone-Adesi-Whaley American approximation.
//
// @param max_iter critical-price root-find iteration cap (0 -> 16)
// @param tol      critical-price convergence tolerance (<= 0 -> 1e-8)
// @return         the American premium, or an Error:
//                   InvalidArgument — S/K <= 0, or negative T or sigma
//                   Unavailable     — the critical-price root-find diverged
[[nodiscard]] Result<double> baw_american(double S, double K, double T,
                                          double sigma, double r, double q,
                                          Side side, std::uint16_t max_iter = 16,
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
// Mutable warm state — NOT thread-safe; construct one per inversion. A large
// sigma jump (or the first call) transparently falls back to a cold seed.
class AloPricer {
 public:
  AloPricer(double S, double K, double T, double r, double q, Side side,
            const std::optional<AlOpts>& opts = std::nullopt);
  ~AloPricer();
  AloPricer(AloPricer&&) noexcept;
  AloPricer& operator=(AloPricer&&) noexcept;
  AloPricer(const AloPricer&) = delete;
  AloPricer& operator=(const AloPricer&) = delete;

  // American price at this contract and `sigma` (>= 0). Warm-starts the boundary
  // from the previous call. Returns NaN only on the negative-rate/carry corner
  // where the asymptotic boundary collapses (andersen_lake returns NotImplemented
  // there); a degenerate sigma ~ 0 or T ~ 0 returns intrinsic.
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
[[nodiscard]] Result<double> american_price(double S, double K, double T,
                                            double sigma, double r, double q,
                                            Side side,
                                            AmericanMethod method = AmericanMethod::AndersenLake,
                                            const std::optional<AlOpts>& opts = std::nullopt);

// Hot-path cached American price: Black-76 + F·correction(k_log, T, sigma) with
// F = S·e^{(r-q)T} and k_log = ln(K/F). A null (or unpopulated) correction falls
// back to the cold Andersen-Lake path, returning NaN if that solver fails.
//
// Mirrors the C `ats_pricer_american_routed(..., correction, NULL, NULL)`.
[[nodiscard]] double american_price_cached(double S, double K, double T,
                                           double sigma, double r, double q,
                                           Side side,
                                           const CorrectionCache* correction);

// ── American Greeks ─────────────────────────────────────────────────────
//
// First-order (delta, vega, rho, theta) are exact chain-rule on the Black-76
// closed forms plus the analytic first-order correction partials. Second-order
// (gamma, vanna, volga, charm) use central finite differences on the analytic
// first-order correction gradient — the "FD wins" fallback the C library flags,
// validated to ~1e-6 absolute against full FD.
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
};

// American Greeks + price. A null `correction` degrades to the Black-76 leg
// (American -> European), exactly like the underlying cached pricer.
//
// @return InvalidArgument if any of S, K, T, sigma is non-positive.
[[nodiscard]] Result<AmericanGreeks> american_greeks(double S, double K, double T,
                                                     double sigma, double r, double q,
                                                     Side side,
                                                     const CorrectionCache* correction);

// American vega ONLY (∂price/∂sigma) — the single first-order sensitivity the IV
// inverter's Newton step needs, WITHOUT the full `american_greeks` bundle's
// second-order finite-difference correction partials (gamma/vanna/volga/charm,
// which cost ~6 extra cache evaluations). It is the Black-76 vega plus
// F·∂correction/∂sigma (one cache `eval_grad`). A null `correction` gives the
// Black-76 (European-leg) vega. Returns 0 on a degenerate / non-positive input
// (the inverter reads 0 as "force bisection").
[[nodiscard]] double american_vega(double S, double K, double T, double sigma,
                                   double r, double q, Side side,
                                   const CorrectionCache* correction) noexcept;

namespace detail {

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

}  // namespace detail

}  // namespace atx::vol
