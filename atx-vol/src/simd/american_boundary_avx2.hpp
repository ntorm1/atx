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

} // namespace atx::vol::simd::detail
