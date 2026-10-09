#pragma once

// Internal declaration for the AVX2 American-put boundary batch kernel. Defined in
// american_boundary_avx2.cpp (built with -mavx2 -mfma via the src/simd/*_avx2.cpp
// CMake glob) and called only from american_boundary_batch.cpp behind an
// atx::vol::simd::use_avx2() guard. Not a public header: callers use
// atx/vol/simd/american_boundary_batch.hpp.

#include <cstddef>
#include <optional>

#include "atx/vol/api/pricing/american.hpp"

namespace atx::vol::simd::detail {

// Price n American puts under one homogeneous configured scheme into price_out,
// 4 lanes at a time on AVX2. Degenerate / non-American / collapse / deep-wing /
// non-finite lanes and the n % 4 tail patch through the scalar andersen_lake, so
// the result matches the scalar batch to the P3 accuracy gate.
void american_put_boundary_batch_avx2(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n,
                                      const std::optional<AlOpts>& opts) noexcept;

// TEST PROBE for the shared 4-lane barycentric interpolant cheb_eval_pd
// (american_boundary_avx2_kernel.hpp). The interpolant is an inline detail of two
// hot kernels, and its exact-node (dz == 0) behaviour — the one case where the AVX2
// copy used to diverge from the scalar al_cheb_eval_t / bary_eval — is not otherwise
// reachable from any public entry point in a way that survives the downstream masks.
// This thin wrapper evaluates it directly so that contract can be gated.
//
// `znodes`/`wbary`/`y` are nb-long and shared by all four lanes; `zq` and `out` are
// exactly 4 long. AVX2-ONLY: call behind atx::vol::simd::have_avx2().
void bary_eval_pack_avx2(const double* znodes, const double* wbary, const double* y,
                         unsigned nb, const double* zq, double* out) noexcept;

} // namespace atx::vol::simd::detail
