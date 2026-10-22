// L2h-c (2026-10-21): 4-lane AVX2 node solver over the shared boundary kernel — the
// AlSigmaQFamily build's batched path. See american_boundary_nodes_avx2.hpp.
//
// Built with -mavx2 -mfma (src/simd/*_avx2.cpp CMake glob).

#include "american_boundary_nodes_avx2.hpp"

#include "american_boundary_avx2_kernel.hpp"

#include "fitting/counters.hpp" // BoundarySolves / AlBoundarySolves — per-solve ledger

#include <cmath>
#include <cstddef>

#if !defined(__AVX2__) || !defined(__FMA__)
#  error "american_boundary_nodes_avx2.cpp requires -mavx2 -mfma (build via src/simd/*_avx2.cpp)"
#endif

#include <immintrin.h>

namespace atx::vol::simd::detail {

unsigned solve_put_boundaries4_avx2(const double *K, double T, const double *sigma,
                                    const double *r, const double *q, const amer::AlScheme &sch,
                                    amer::AlBoundary *bnd, amer::AlWorkspace *ws) noexcept {
  const double Tv[4] = {T, T, T, T};
  PutPackBoundary pack;
  bool eligible[4] = {false, false, false, false};
  int ref = -1;
  // The kernel's spot argument only gates eligibility (S > 0); the boundary is
  // spot-independent, so the strikes stand in for it.
  solve_put_boundary_pack_avx2(K, K, Tv, sigma, r, q, /*n=*/4, sch, bnd, ws, pack, eligible,
                               ref);
  if (ref < 0) {
    return 0u;
  }
  unsigned mask = 0u;
  for (unsigned l = 0; l < 4; ++l) {
    mask |= eligible[l] ? (1u << l) : 0u;
  }
  alignas(32) double lane[4] = {0.0, 0.0, 0.0, 0.0};
  for (unsigned j = 0; j < pack.nb; ++j) {
    _mm256_store_pd(lane, pack.Y[j]);
    for (unsigned l = 0; l < 4; ++l) {
      if ((mask & (1u << l)) == 0u) {
        continue;
      }
      bnd[l].y[j] = lane[l];
      if (!std::isfinite(lane[l])) {
        mask &= ~(1u << l); // the scalar path decides this lane
      }
    }
  }
  for (unsigned l = 0; l < 4; ++l) {
    if ((mask & (1u << l)) != 0u) {
      // One boundary solve per lane, as the put batch counts its vector lanes, so the
      // solve ledger reads the same whichever route built the nodes.
      ATX_VOL_COUNT(BoundarySolves);
      counters::ledger::bump(counters::ledger::Solve::AlBoundarySolves);
    }
  }
  return mask;
}

} // namespace atx::vol::simd::detail
