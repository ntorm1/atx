#pragma once

// Internal declarations for the AVX2 Black-76 batch kernels. Defined in
// black76_batch_avx2.cpp (built with -mavx2 -mfma) and called only from
// black76_batch.cpp behind an atx::vol::simd::have_avx2() guard. Not a public
// header: callers use atx/vol/simd/black76_batch.hpp.

#include <cstddef>

#include "atx/vol/types.hpp"

namespace atx::vol::simd::detail {

void black76_price_batch_avx2(const double* F, const double* K, const double* T,
                              const double* sigma, const double* df,
                              const Side* side, double* price_out,
                              std::size_t n) noexcept;

void black76_value_vega_batch_avx2(const double* F, const double* K,
                                   const double* T, const double* sigma,
                                   const double* df, const Side* side,
                                   double* price_out, double* vega_out,
                                   std::size_t n) noexcept;

} // namespace atx::vol::simd::detail
