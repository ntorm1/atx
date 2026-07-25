#pragma once

// ── Internal seam: Andersen-Lake dimensionless-boundary primitives ────────
//
// A NARROW, library-private declaration surface exposing the small set of
// file-local Andersen-Lake helpers that american.cpp defines so a SECOND
// translation unit (boundary_interp.cpp, the σ-axis Chebyshev boundary
// interpolant, Task 11 / §P2.5) can reuse them WITHOUT re-deriving the boundary
// math. The DEFINITIONS stay in american.cpp; only the types/constants and the
// declarations of the entry points move here. Signatures and numerics are
// unchanged — american.cpp defines these exact symbols inside `namespace amer`.
//
// Everything lives in `atx::vol::amer` (internal detail). american.cpp brings it
// into scope with a single `using namespace amer;` so its existing unqualified
// call sites are undisturbed; boundary_interp.cpp qualifies with `amer::`.
//
// NOT a public API. Not installed. No stability guarantees.

#include <array>
#include <cstdint>
#include <optional>

#include "atx/vol/american.hpp"  // AlOpts (scheme_from_opts input)

namespace atx::vol::amer {

// Hard limits — every per-solve buffer is stack-bounded (matches C ATS_AL_*).
inline constexpr std::uint16_t kAlMaxNodes = 32;

// P2.2 sweep-invariant geometry precompute sizing (mirrors american.cpp).
inline constexpr unsigned kGeoNodeMax = 16;     // >= max specialized n_boundary (12)
inline constexpr unsigned kGeoQuadStride = 32;  // >= max specialized n_quad_fp (24)
inline constexpr unsigned kGeoSize = kGeoNodeMax * kGeoQuadStride;  // 512 doubles

// ── A6 (PR-P2) sweep-invariant BARYCENTRIC precompute sizing ──────────────
//
// al_cheb_eval_t's inner loop recomputes, on EVERY Jacobi-Newton and fixed-point
// sweep, the quotients wbary[k] / (zc_ji - z[k]) and their running sum `den`, for
// every (collocation node j, fixed-point quad node i) pair. zc_ji, wbary[] and z[]
// are ALL fixed for the whole solve — only bnd.y[] moves between sweeps — so the
// entire denominator half of the barycentric formula is sweep-invariant. Hoisting it
// removes nb divisions + nb subtractions + nb adds per (j, i) per sweep and leaves
// only the sweep-VARYING dot product num = sum_k qq[k] * y[k].
//
// The hoist is BIT-IDENTICAL, not merely close: qq[k] is formed from the same two
// doubles by the same operator, `den` is summed left to right over the same values,
// and `num` accumulates left to right over the same products, so num/den reproduces
// al_cheb_eval_t's result exactly. The inline `dz == 0` early return is preserved as
// an explicit hit index rather than dropped.
//
// Layout is PACKED and in the kernel's own access order: bidx = (j-1)*nq + i indexes
// den/hit, and bidx*nb + k indexes the quotients. Only the SPECIALIZED schemes
// {(7,8), (7,16), (12,24)} take the hoisted kernel, so the table needs the max over
// those of (nb-1)*nq*nb = 11*24*12 = 3168 doubles. Total workspace growth ~27 KB.
//
// THROUGHPUT GATE, RE-SPECIFIED. A6's plan text gates this hoist on ">= 8% on the
// bench sweep row". No such row exists: no name registered in atx-vol/bench/*.cpp
// contains "sweep" (the only "sweep" artifact in the tree is an unrelated
// fit-worker-count baseline FILE, i7-1260p-clang18-avx2-e2e-fitworkers-sweep.json),
// so the gate as written is unfalsifiable. The rows that actually measure the
// quantity this hoist changes — a cold AL boundary solve, which is all JN+FP sweep
// and nothing else — already exist in american_shootout_bench.cpp:
//
//   american/boundary_batch/scalar          {12,24}, ForceScalar  <- PRIMARY
//   american/boundary_batch/scalar_qlfast   {7,8},   ForceScalar
//
// 4096 puts, every lane taking the full sweep budget, no premium quadrature to
// dilute the signal and ForceScalar so no AVX2 lane confounds it. Both names are
// machine-pinned by the atx-vol-american-shootout-name-coverage CTest, so this
// re-specification cannot rot the way the original did. Corroborating (whole cold
// price, so the sweep win arrives diluted): american/price/accurate, american/price/fast.
//
// NOT MEASURED HERE. This host is shared with other build/test sessions for the
// whole of this sprint; every throughput figure taken on it is non-citable, so the
// >= 8% half of the gate is deferred to a quiet-window re-run against the rows
// above. What IS proved is the half that makes the hoist legitimate at all:
// bit-identity, by BoundaryHoist.HoistedBaryTableMatchesInlineFormula.
inline constexpr unsigned kGeoBaryNodeMax = 12;  // max specialized n_boundary
inline constexpr unsigned kGeoBaryQuadMax = 24;  // max specialized n_quad_fp
inline constexpr unsigned kGeoBaryPairs = (kGeoBaryNodeMax - 1u) * kGeoBaryQuadMax;  // 264
inline constexpr unsigned kGeoBarySize = kGeoBaryPairs * kGeoBaryNodeMax;            // 3168

// ── AL boundary state + scheme ──────────────────────────────────────────

struct AlScheme {
  std::uint16_t n_boundary = 12;
  std::uint16_t n_quad_fp = 24;
  std::uint16_t n_quad_price = 48;
  std::uint16_t n_iter_jn = 2;
  std::uint16_t n_iter_fp = 4;
  double tol = 1.0e-10;
  // Cold critical-boundary seed the solve lays down before the JN/FP sweeps.
  // Baw (Barone-Adesi-Whaley) is the default; QdPlus is the Li (2010) "+"
  // refinement, kept SELECTABLE for measurement. Task A6 A/B'd QdPlus on the
  // shootout and it REGRESSED fast-tier accuracy (max abs err 1.44e-3 → 4.62e-3
  // under the truncated fast sweep budget), so every production scheme stays Baw;
  // see the qdplus_critical_put note in american.cpp. Defaulting to Baw leaves the
  // accurate / reference / boundary_interp schemes byte-unchanged.
  detail::AlSeedMode seed = detail::AlSeedMode::Baw;
};

struct AlBoundary {
  std::array<double, kAlMaxNodes> z{};
  std::array<double, kAlMaxNodes> wbary{};  // 2nd-kind barycentric weights (fixed)
  std::array<double, kAlMaxNodes> x{};
  std::array<double, kAlMaxNodes> tau{};
  std::array<double, kAlMaxNodes> y{};  // H(τ) values — the live state
  std::uint16_t n = 0;
  double T = 0.0;
  double K = 0.0;
  double xmax = 0.0;  // asymptotic boundary B(∞)
};

struct AlWorkspace {
  const double* qx_fp = nullptr;
  const double* qw_fp = nullptr;
  unsigned n_quad_fp = 0;
  const double* qx_price = nullptr;
  const double* qw_price = nullptr;
  unsigned n_quad_price = 0;
  std::array<double, kAlMaxNodes> next_y{};  // iteration scratch
  // Force the generic (runtime trip-count) kernel even for a specialized scheme —
  // the test seam behind detail::andersen_lake_generic_kernel. Default true so
  // every production solve specializes.
  bool specialize = true;
  // True only after the sigma-independent geometry below has been bound for the
  // current (T,r,q,node-grid) contract. Retained pricers clear it on reset.
  bool geo_static_bound = false;
  // Sweep-invariant geometry for eqn_b_ND. zc/weru/wequ are bound once per
  // contract; geo_v is rebound for each sigma. NOT zero-init'd (active elements
  // are filled before read), so stacked workspaces do not pay four 512-double
  // memsets.
  std::array<double, kGeoSize> geo_zc;    // clamped Chebyshev argument z_ji
  std::array<double, kGeoSize> geo_v;     // sigma * sqrt(t_u_ji)
  std::array<double, kGeoSize> geo_weru;  // qw_fp[i] * exp(r * u_ji)
  std::array<double, kGeoSize> geo_wequ;  // qw_fp[i] * exp(q * u_ji)
  // A6: the sweep-invariant barycentric table (see kGeoBarySize above). Bound by the
  // SAME static bind as geo_zc — it depends only on (T, tau_j, xs_i, node grid) —
  // so a retained workspace carries it across a sigma sweep exactly as it does zc.
  std::array<double, kGeoBarySize> geo_bary;      // wbary[k] / (zc_ji - z[k])
  std::array<double, kGeoBaryPairs> geo_bary_den; // sum_k of the above, same order
  std::array<std::int8_t, kGeoBaryPairs> geo_bary_hit;  // exact-node index; -1 = none
#ifndef NDEBUG
  // R-30: Debug-only bind key naming the contract the sweep-invariant static
  // geometry above (geo_zc/geo_weru/geo_wequ) was bound for. Written when
  // al_bind_geometry_static binds; asserted on every reuse in al_bind_geometry_sigma
  // so a retained workspace can never silently consume geometry from a different
  // (T, r, q, node-grid) contract (the obs-23864 revalidation-trust regression shape).
  struct GeoBindKey {
    double T = 0.0;
    double r = 0.0;
    double q = 0.0;
    std::uint16_t n = 0;   // matches AlBoundary::n
    unsigned nq = 0;       // matches AlWorkspace::n_quad_fp
    bool set = false;
  } geo_bind_key{};
#endif
};

// Boundary solve status (S-independence seam). Ok => bnd/ws hold a converged
// boundary ready for al_put_price_from_boundary.
enum class AlSolveStatus { Ok, Collapsed, TableMissing };

// ── Declarations (definitions live in american.cpp, namespace amer) ──────

// Homogeneity scale K·min(1, r/q) (with the negative-carry corners). > 0 iff the
// asymptotic boundary exists (American regime).
[[nodiscard]] double al_xmax_put(double K, double r, double q) noexcept;

// ACCURATE preset when opts == nullopt; otherwise map the public knobs.
[[nodiscard]] AlScheme scheme_from_opts(const std::optional<AlOpts>& opts) noexcept;

// S-independent: init nodes, bind quadrature, seed + iterate the boundary. On Ok,
// bnd/ws hold a converged boundary ready for al_put_price_from_boundary.
[[nodiscard]] AlSolveStatus al_solve_put_boundary(double K, double T, double sigma,
                                                  double r, double q,
                                                  const AlScheme& sch,
                                                  AlBoundary& bnd, AlWorkspace& ws,
                                                  bool specialize = true) noexcept;

// Init-ONLY variant for the AVX2 boundary batch (Task A5; supersedes A1's
// al_seed_put_boundary). Runs al_solve_put_boundary's pre-sweep prefix — init the
// node grid + bind the Gauss-Legendre quadrature pointers — but does NEITHER the
// cold Barone-Adesi-Whaley seed NOR al_bind_geometry (the kernel recomputes geometry
// inline and lays down the BAW seed 4-wide itself). Leaves bnd.y[] at 0 for the
// caller's vector seed; ws.specialize is false. No sigma argument (the seed is the
// only sigma-dependent step, and the caller owns it).
[[nodiscard]] AlSolveStatus al_init_put_boundary(double K, double T, double r, double q,
                                                 const AlScheme& sch, AlBoundary& bnd,
                                                 AlWorkspace& ws) noexcept;

// Warm variant: seed from an already-converged boundary a small (sigma,r,T) bump
// away instead of a cold Barone-Adesi-Whaley re-seed.
[[nodiscard]] AlSolveStatus al_solve_put_boundary_warm(double K, double T, double sigma,
                                                       double r, double q,
                                                       const AlScheme& sch,
                                                       const AlBoundary& seed,
                                                       AlBoundary& bnd,
                                                       AlWorkspace& ws) noexcept;

// Put price at spot S from a solved (or interpolated) boundary. Runs the SAME
// euro + premium + clamp path as a full cold solve, so a freshly-solved bnd is
// bit-identical to andersen_lake's American arm.
[[nodiscard]] double al_put_price_from_boundary(const AlBoundary& bnd,
                                                const AlWorkspace& ws, double S,
                                                double K, double T, double sigma,
                                                double r, double q) noexcept;

// ── P2 (WS-P) seam: the PURE collocation residual R(y; sigma, r) ──────────
//
// Adjoint / implicit-function-theorem greeks (detail/adjoint_greeks.cpp) need the
// residual as a linkable symbol to form the Jacobian J = dR/dy and the parameter
// sensitivities R_sigma/R_r. This entry point exposes the pure fixed-point residual
// R(y; sigma, r) over the file-static kernel eqn_b_ND_impl<0,0>, with no change
// to any existing behaviour: it is a PURE function of (y, sigma, r) given an
// already-initialised bnd (node grid / xmax / K / T fixed) and its bound ws.
//
// Writes R_out[0..bnd.n-1]. Node 0 is pinned (R_out[0] = 0); interior node i gives
//   R_out[i] = y[i] - y_from_b( clamp( alpha * N(tau_i,b_i) / D(tau_i,b_i) ) )
// with b_i = xmax*exp(-sqrt(y[i])), alpha = K*exp(-(r-q)*tau_i). The generic
// (inline-geometry) kernel is used, so ws need not have its sweep-invariant
// geometry re-bound for a perturbed (sigma, r, q). Does not mutate bnd. q is an
// explicit argument (not read from bnd) so R_r/R_sigma central differences bump
// exactly one parameter while holding the node grid (bnd.tau/xmax/K) fixed.
void al_put_boundary_residual(const AlBoundary& bnd, const AlWorkspace& ws,
                              const double* y, double sigma, double r, double q,
                              double* R_out) noexcept;

// ── P3-pre (WS-P) seam: reverse-accumulation-through-iterations (Christianson) ──
//
// The IFT-adjoint (P2) differentiates the EXACT fixed point y_fp(θ); the production
// Andersen-Lake solve is BUDGET-LIMITED (2 JN + 4 FP, early-exit at tol) so it
// returns an under-converged y*(θ) ≠ y_fp and its mark is price(y*;θ). fd/al
// differentiate that ACTUAL mark (dy*/dθ); the IFT does not, so it is mark-consistent
// only on the well-converged subset (~14% of a realistic grid — measured). To match
// the budget-limited mark on the WHOLE domain we differentiate THROUGH THE ITERATION
// ACTUALLY RUN — Christianson, "Reverse accumulation and attractive fixed points"
// (Optimization Methods & Software 3 (1994) 311–326): tape the iterate sequence
// (seed y⁰, y¹=G₁(y⁰;θ), …, yᴺ=Gₙ(yᴺ⁻¹;θ)) and propagate the tangent through it.
// These three primitives expose exactly what the adjoint TU needs to tape + replay.

// Max sweeps the tape can hold (ACCURATE preset is 2 JN + 4 FP = 6). A scheme whose
// n_iter_jn + n_iter_fp exceeds this makes al_solve_put_boundary_tape return
// TableMissing so the adjoint caller falls back to the FD bundle.
inline constexpr std::uint16_t kAlMaxTapeSweeps = 16;

// Records the seed + every swept iterate + the sweep kind (JN vs FP) at each step
// (after the actual early-exit budget). y_iter[0] = seed; y_iter[k] = boundary after
// sweep k for k = 1..n_steps. Stack-bounded (mirrors AlBoundary::y sizing).
struct AlSolveTape {
  std::uint16_t n_steps = 0;
  std::array<bool, kAlMaxTapeSweeps> is_jn{};
  std::array<std::array<double, kAlMaxNodes>, kAlMaxTapeSweeps + 1> y_iter{};
};

// Same seed, same JN/FP schedule, same tol early-exit as al_solve_put_boundary, but
// runs the GENERIC (specialize=false) kernel — so it can be replayed bit-for-bit by
// al_apply_boundary_sweep for the Christianson tangent (below) — and ALSO records
// `tape`. NOT bit-identical to production al_solve_put_boundary, which defaults to the
// specialized kernel; the two agree only to the "pure hoist" tolerance the delta/price
// tests bound to ~1e-9, NOT to 0 ULP. So do NOT treat the taped boundary as the served
// mark or as a bit-exact FD-parity oracle. Mark safety in the portfolio path comes from
// evaluate_batch (or, under the I-2 fuse, from american_greeks_adjoint's own AL price),
// never from this boundary being bit-identical to production. Collapsed/TableMissing
// exactly as the base solve (plus TableMissing if the budget exceeds kAlMaxTapeSweeps).
[[nodiscard]] AlSolveStatus al_solve_put_boundary_tape(double K, double T, double sigma, double r,
                                                       double q, const AlScheme& sch,
                                                       AlBoundary& bnd, AlWorkspace& ws,
                                                       AlSolveTape& tape) noexcept;

// Apply ONE boundary sweep (Jacobi-Newton if is_jn, else fixed-point) of the GENERIC
// inline-geometry kernel to y_in at (sigma,r,q), writing the swept boundary to y_out.
// Pure: does not mutate bnd/ws. The generic kernel recomputes geometry inline, so a
// PERTURBED sigma/r needs no geometry rebind. The load-bearing guarantee: at the taped
// inputs it reproduces the tape's GENERIC sweep BIT-FOR-BIT (al_solve_put_boundary_tape
// runs the same specialize=false kernel) — that is what makes y_iter[k+1] the exact
// forward-difference anchor. (It matches the SPECIALIZED production sweep only to the
// pure-hoist ~1e-9 tolerance, which is NOT relied on here.) This is Gₖ(·;θ) for the
// Christianson tangent's directional differences.
void al_apply_boundary_sweep(const AlBoundary& bnd, const AlWorkspace& ws, const double* y_in,
                             double sigma, double r, double q, bool is_jn, double* y_out) noexcept;

// The cold Barone-Adesi-Whaley boundary seed (al_seed_boundary) written to y_out
// WITHOUT mutating bnd — the tape's y⁰ and the source of the seed tangent ∂y⁰/∂θ.
void al_seed_boundary_into(const AlBoundary& bnd, double sigma, double r, double q,
                           double* y_out) noexcept;

}  // namespace atx::vol::amer
