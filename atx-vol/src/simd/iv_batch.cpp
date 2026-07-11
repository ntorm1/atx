#include "atx/vol/simd/iv_batch.hpp"

#include "atx/vol/implied_vol.hpp"
#include "atx/vol/simd/cpu.hpp"

#include "iv_batch_avx2.hpp"

#include <limits>

namespace atx::vol::simd {

namespace {

// Unpack a scalar implied_vol Result into the (iv, ok) SoA outputs: the value
// with ok = 1 on success, NaN with ok = 0 on any failure (non-finite input,
// out-of-band price, or non-convergence). This is the numerical source of truth
// the AVX2 kernel patches through.
void iv_batch_scalar(const double* price, const double* F, const double* K,
                     const double* T, const double* df, const Side* side,
                     double* iv_out, std::uint8_t* ok_out,
                     std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const Result<double> r =
            implied_vol(price[i], F[i], K[i], T[i], df[i], side[i]);
        if (r) {
            iv_out[i] = *r;
            ok_out[i] = 1;
        } else {
            iv_out[i] = std::numeric_limits<double>::quiet_NaN();
            ok_out[i] = 0;
        }
    }
}

} // namespace

void implied_vol_batch(const double* price, const double* F, const double* K,
                       const double* T, const double* df, const Side* side,
                       double* iv_out, std::uint8_t* ok_out,
                       std::size_t n) noexcept {
    if (have_avx2()) {
        detail::implied_vol_batch_avx2(price, F, K, T, df, side, iv_out, ok_out,
                                       n);
    } else {
        iv_batch_scalar(price, F, K, T, df, side, iv_out, ok_out, n);
    }
}

} // namespace atx::vol::simd
