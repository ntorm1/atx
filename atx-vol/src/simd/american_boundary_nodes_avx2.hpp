#pragma once

// L2h-c (2026-10-21): the AVX2 boundary kernel as a 4-lane NODE solver — for a caller
// that keeps the converged dimensionless boundaries (a family's tensor nodes), not one
// price per lane. Defined in american_boundary_nodes_avx2.cpp (built with -mavx2 -mfma
// by the src/simd/*_avx2.cpp glob); call it only when atx::vol::simd::use_avx2() is
// true. Additive: the put batch driver (american_boundary_avx2.cpp) and the kernel
// header are untouched; this TU rides solve_put_boundary_pack_avx2 for the init and the
// 4-wide Barone-Adesi-Whaley seed and runs the SAME Jacobi-Newton / fixed-point sweeps
// with the sweep-invariant quadrature geometry hoisted out of the sweep loop (the
// scalar solve's al_bind_geometry_static idea): the barycentric terms wbary_m /
// (zc - z_m) of every (collocation node, quadrature node) pair are lane-uniform (the
// pack shares T, so the node grid and the quadrature times are common), and
// qw·exp(r u), qw·exp(q u), sigma·sqrt(tau - u) are bound once per pack instead of once
// per sweep. Every hoisted value is formed by the kernel's own intrinsic sequence on the
// same inputs and consumed in the kernel's order, so the boundary is the kernel's BIT
// FOR BIT; only the work moves.
//
// NOT a public header. Not installed.

#include <array>
#include <cstdint>

#include "pricing/american_boundary.hpp" // amer::AlScheme / AlBoundary / AlWorkspace

namespace atx::vol::simd::detail {

// Caller-owned scratch for retained sweep geometry (~130 KB: heap it). Sized
// for n_boundary <= kMaxNodes and n_quad_fp <= kMaxQuad — the Reference scheme is
// (12, 24); a larger scheme runs the kernel's own inline-geometry sweeps instead.
struct NodePackScratch {
  static constexpr unsigned kMaxNodes = 16;
  static constexpr unsigned kMaxQuad = 32;
  static constexpr unsigned kMaxPairs = (kMaxNodes - 1u) * kMaxQuad;
  // Local to one family preparation. Compare the actual initialized grid/tables,
  // not their addresses, before retaining invariant coefficients across packs.
  struct GeometryKey {
    double T = 0.0;
    unsigned nb = 0;
    unsigned nq = 0;
    std::array<double, kMaxNodes> tau{}, z{}, wbary{};
    std::array<double, kMaxQuad> qx{}, qw{};
  } geometry_key;
  bool geometry_ready = false;
  bool carry_ready = false;
  std::array<double, 4> carry_r{}, carry_q{};
  std::uint32_t geometry_builds = 0; // diagnostic work counts for this scratch lifetime
  std::uint32_t carry_builds = 0;
  double u[kMaxPairs];
  double weight[kMaxPairs];
  double sqrt_tu[kMaxPairs];
  double qq[kMaxPairs * kMaxNodes];  // wbary_m / (zc_p - z_m), pair-major
  double den[kMaxPairs];             // sum_m of the above, the kernel's order
  double safe_tu[kMaxPairs];         // tau_i - u_ik where > 1e-14, else 1
  std::int8_t hit[kMaxPairs];        // exact collocation-node hit of zc_p, -1 = none
  std::uint8_t tu_gt[kMaxPairs];     // tau_i - u_ik > 1e-14
  alignas(32) double er[kMaxPairs * 4]; // per lane: qw_k * exp(r u_ik)
  alignas(32) double eq[kMaxPairs * 4]; // per lane: qw_k * exp(q u_ik)
  alignas(32) double vq[kMaxPairs * 4]; // per lane: sigma * sqrt(safe_tu)
};

// Solve the four internal-put boundaries (K[l], T, sigma[l], r[l], q[l]) of one pack
// under `sch` with the AVX2 kernel (init + 4-wide Barone-Adesi-Whaley seed + lockstep
// Jacobi-Newton / fixed-point sweeps, sch's budget and tol). `bnd` / `ws` are four
// caller-owned scratch slots, `geo` the hoisted-geometry scratch. Returns the lane mask
// (bit l) of the boundaries solved: for such a lane bnd[l] holds the node grid, K, xmax
// and the converged y[] (finite). A clear bit is a lane the kernel declines —
// degenerate, non-American, collapsed, frozen sweep or non-finite — which the caller
// solves on the scalar path (it reports the scalar status). Counts one boundary solve
// per returned lane in the solve ledger, as the put batch does.
[[nodiscard]] unsigned solve_put_boundaries4_avx2(const double *K, double T,
                                                  const double *sigma, const double *r,
                                                  const double *q, const amer::AlScheme &sch,
                                                  amer::AlBoundary *bnd, amer::AlWorkspace *ws,
                                                  NodePackScratch &geo) noexcept;

} // namespace atx::vol::simd::detail
