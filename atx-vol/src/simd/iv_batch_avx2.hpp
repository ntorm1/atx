#pragma once

// Internal declaration for the AVX2 implied-volatility batch kernel. Defined in
// iv_batch_avx2.cpp (built with -mavx2 -mfma) and called only from iv_batch.cpp
// behind an atx::vol::simd::have_avx2() guard. Not a public header: callers use
// atx/vol/simd/iv_batch.hpp.

#include <cstddef>
#include <cstdint>

#include "atx/vol/api/core/types.hpp"

namespace atx::vol::simd::detail {

void implied_vol_batch_avx2(const double* price, const double* F,
                            const double* K, const double* T, const double* df,
                            const Side* side, double* iv_out,
                            std::uint8_t* ok_out, std::size_t n) noexcept;

} // namespace atx::vol::simd::detail
