#pragma once

// Internal declaration for the AVX2 Black-76 Greeks batch kernel. Defined in
// greeks_batch_avx2.cpp (built with -mavx2 -mfma) and called only from
// greeks_batch.cpp behind an atx::vol::simd::have_avx2() guard. Not a public
// header: callers use atx/vol/simd/greeks_batch.hpp.

#include <cstddef>

#include "atx/vol/api/pricing/greeks.hpp"
#include "atx/vol/api/simd/greeks_batch.hpp" // GreeksBatchSoA
#include "atx/vol/api/core/types.hpp"

namespace atx::vol::simd::detail {

// AoS scatter: {greeks_out[i], price_out[i]}; null price_out skips premium.
void black76_greeks_batch_avx2(const double *F, const double *K, const double *T,
                               const double *sigma, const double *r, const double *df,
                               const Side *side, Greeks *greeks_out, double *price_out,
                               std::size_t n) noexcept;

// SoA scatter into the per-greek columns of `out` (null columns skipped). Shares
// the identical vector math with the AoS kernel above, so it is bit-for-bit equal
// field-for-field.
void black76_greeks_batch_soa_avx2(const double *F, const double *K, const double *T,
                                   const double *sigma, const double *r, const double *df,
                                   const Side *side, const GreeksBatchSoA &out,
                                   std::size_t n) noexcept;

} // namespace atx::vol::simd::detail
