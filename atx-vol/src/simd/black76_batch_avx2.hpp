#pragma once

// Internal declarations for the AVX2 Black-76 batch kernels. Defined in
// black76_batch_avx2.cpp (built with -mavx2 -mfma) and called only from
// black76_batch.cpp behind an atx::vol::simd::have_avx2() guard. Not a public
// header: callers use atx/vol/simd/black76_batch.hpp.

#include <cstddef>

#include "atx/vol/types.hpp"

namespace atx::vol::simd::detail {

void black76_price_batch_avx2(const double *F, const double *K, const double *T,
                              const double *sigma, const double *df, const Side *side,
                              double *price_out, std::size_t n) noexcept;

void black76_value_vega_batch_avx2(const double *F, const double *K, const double *T,
                                   const double *sigma, const double *df, const Side *side,
                                   double *price_out, double *vega_out, std::size_t n) noexcept;

// Slice-shaped overload: one T (and optional precomputed sqrt(T)) is shared by
// every lane. `sqrt_t_in < 0` computes sqrt(T); non-negative values are used
// exactly as supplied, matching black76_value_and_vega.
void black76_value_vega_shared_t_batch_avx2(const double *F, const double *K, double T,
                                            double sqrt_t_in, const double *sigma, const double *df,
                                            const Side *side, double *price_out, double *vega_out,
                                            std::size_t n) noexcept;

} // namespace atx::vol::simd::detail
