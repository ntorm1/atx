#pragma once

// Andersen-Lake test/measurement-only internals split out of the public
// american.hpp API surface (Task 5, atx-vol API restructure): every symbol
// here is used only by american.cpp (production) and by american_test.cpp /
// american_bulk_rung_test.cpp (direct unit-test seams) or bench/*.cpp
// (al_preset_ladder_bench.cpp, american_shootout_bench.cpp) measurement
// tooling, never by any other production TU.

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol::detail {

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

// A6 (PR-P2) test seam. Bind a workspace for the internal-put contract
// (K, T, sigma, r, q, opts) and audit the HOISTED sweep-invariant barycentric table
// entry by entry against the inline formula al_cheb_eval_t evaluated on every sweep
// before the hoist.
//
//   `specialized` — the scheme takes the hoisted kernel at all (a generic AlOpts
//                   does not, and then nothing is bound and nothing is audited).
//   `entries`     — (collocation node, quad node) pairs the bind populated. ZERO is
//                   how A6's absence is detectable: no table, no hoist. Note that at
//                   A6's parent commit `entries` is 0 because THIS FUNCTION does not
//                   exist there, so that RED is a self-referential absence signal
//                   rather than an independent observable (REVWSA finding 3).
//   `mismatches`  — entries where any stored quotient, the stored denominator sum, or
//                   the stored exact-node hit differs from the inline computation by
//                   a single bit. Must be 0, which is what makes the BIND a hoist.
//
// SCOPE — what this audit does NOT prove (REVWSA finding 2). It recomputes using the
// BIND's own index arithmetic, in the same TU, with the same nb, duplicating the
// bind's own skip conditions, and it never calls al_cheb_eval_hoisted. So it cannot
// catch an index or stride disagreement, and it says nothing about the KERNEL's read
// stride or its `num` accumulation order. Those are covered — and the bit-identity
// claim is actually carried — by the PRE-EXISTING
// BoundaryHoist.SpecializedMatchesGeneric, which compares end prices out of the
// hoisted and generic kernels across all three specialized schemes. Cite that test,
// not this one's entry count, when the hoist's bit-identity is the question.
//
// Not a production entry point.
struct AlBaryHoistAudit {
  std::size_t entries = 0;
  std::size_t mismatches = 0;
  bool specialized = false;
};
[[nodiscard]] AlBaryHoistAudit al_bary_hoist_audit(double K, double T, double sigma, double r,
                                                   double q,
                                                   const std::optional<AlOpts> &opts) noexcept;

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

// A6 bench/measurement seam. Prices exactly like `andersen_lake` but FORCES the
// cold boundary seed (`seed`) and, when `n_quad_price != 0`, overrides the premium
// Gauss-Legendre order — so american_shootout_bench can A/B the QD+ seed and a
// trimmed premium quadrature against the BAW/16 fast-tier baseline in a single
// build. Production `andersen_lake` calls are unaffected (they never route here).
[[nodiscard]] Result<double> andersen_lake_seeded(double S, double K, double T, double sigma,
                                                  double r, double q, Side side,
                                                  const std::optional<AlOpts> &opts,
                                                  AlSeedMode seed,
                                                  std::uint16_t n_quad_price = 0);

// ── A1 test seam: BAW smooth-pasting critical-price root-find ─────────────
//
// The Barone-Adesi-Whaley smooth-pasting residual and its analytic derivative
// (put_residual/put_residual_deriv, call_residual/call_residual_deriv) are
// file-static in american.cpp. These seams expose them for the A1 FD-parity and
// convergence tests WITHOUT widening the production surface. Not production
// entry points.

// Evaluate the BAW smooth-pasting residual `f` and its analytic derivative
// `fprime = df/dSx` at a trial critical price Sx, with the quadratic exponent
// q1 (put) / q2 (call) derived internally from (K,T,sigma,r,q) exactly as
// baw_american does. `ok == false` on the European / degenerate / no-valid-
// exponent corners (f, fprime, q_exp left 0). Lets the test central-difference
// `f` and pin the analytic derivative's sign and magnitude (finding 1).
struct BawResidualEval {
  double f = 0.0;
  double fprime = 0.0;
  double q_exp = 0.0; // q1 (put) or q2 (call)
  bool ok = false;
};
[[nodiscard]] BawResidualEval baw_residual_eval(double Sx, double K, double T, double sigma,
                                                double r, double q, Side side) noexcept;

// Run the safeguarded critical-price Newton (newton_critical_put/call) and report
// its convergence contract (finding 8): `iters` executed, `converged` == a
// Newton/step tolerance test fired INSIDE the loop (NOT max_iter bisection
// exhaustion), `residual` == the signed residual f at the returned Sx. `ok ==
// false` on the European / degenerate corners with no interior early-exercise
// boundary.
struct BawCriticalSolve {
  double Sx = 0.0;
  double residual = 0.0;
  std::uint16_t iters = 0;
  bool converged = false;
  bool ok = false;
};
[[nodiscard]] BawCriticalSolve baw_critical_solve(double K, double T, double sigma, double r,
                                                  double q, Side side, std::uint16_t max_iter,
                                                  double tol) noexcept;

} // namespace atx::vol::detail
