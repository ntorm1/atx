#pragma once

// Internal declaration for the AVX2 Black-76 Greeks batch kernel. Defined in
// greeks_batch_avx2.cpp (built with -mavx2 -mfma) and called only from
// greeks_batch.cpp behind an atx::vol::simd::have_avx2() guard. Not a public
// header: callers use atx/vol/simd/greeks_batch.hpp.

#include <cstddef>

#include "atx/vol/greeks.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol::simd::detail {

void black76_greeks_batch_avx2(const double* F, const double* K, const double* T,
                               const double* sigma, const double* r,
                               const double* df, const Side* side,
                               Greeks* greeks_out, double* price_out,
                               std::size_t n) noexcept;

} // namespace atx::vol::simd::detail
