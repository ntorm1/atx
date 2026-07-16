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

// ── AL boundary state + scheme ──────────────────────────────────────────

struct AlScheme {
  std::uint16_t n_boundary = 12;
  std::uint16_t n_quad_fp = 24;
  std::uint16_t n_quad_price = 48;
  std::uint16_t n_iter_jn = 2;
  std::uint16_t n_iter_fp = 4;
  double tol = 1.0e-10;
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

}  // namespace atx::vol::amer
