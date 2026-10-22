#pragma once

// L2h-c (2026-10-21): the AVX2 boundary kernel as a 4-lane NODE solver — for a caller
// that keeps the converged dimensionless boundaries (a family's tensor nodes), not one
// price per lane. Defined in american_boundary_nodes_avx2.cpp (built with -mavx2 -mfma
// by the src/simd/*_avx2.cpp glob); call it only when atx::vol::simd::use_avx2() is
// true. Additive: the put batch driver (american_boundary_avx2.cpp) and the kernel
// header are untouched; this TU rides solve_put_boundary_pack_avx2 as they do.
//
// NOT a public header. Not installed.

#include "pricing/american_boundary.hpp" // amer::AlScheme / AlBoundary / AlWorkspace

namespace atx::vol::simd::detail {

// Solve the four internal-put boundaries (K[l], T, sigma[l], r[l], q[l]) of one pack
// under `sch` with the AVX2 kernel (init + 4-wide Barone-Adesi-Whaley seed + lockstep
// Jacobi-Newton / fixed-point sweeps, sch's budget and tol). `bnd` / `ws` are four
// caller-owned scratch slots. Returns the lane mask (bit l) of the boundaries solved:
// for such a lane bnd[l] holds the node grid, K, xmax and the converged y[] (finite).
// A clear bit is a lane the kernel declines — degenerate, non-American, collapsed,
// frozen sweep or non-finite — which the caller solves on the scalar path (it reports
// the scalar status). Counts one boundary solve per returned lane in the solve ledger,
// as the put batch does.
[[nodiscard]] unsigned solve_put_boundaries4_avx2(const double *K, double T,
                                                  const double *sigma, const double *r,
                                                  const double *q, const amer::AlScheme &sch,
                                                  amer::AlBoundary *bnd,
                                                  amer::AlWorkspace *ws) noexcept;

} // namespace atx::vol::simd::detail
