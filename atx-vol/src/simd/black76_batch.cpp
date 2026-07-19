#include "atx/vol/simd/black76_batch.hpp"

#include "atx/vol/black76.hpp"
#include "atx/vol/simd/cpu.hpp"

#include "black76_batch_avx2.hpp"

namespace atx::vol::simd {

namespace {

void price_batch_scalar(const double* F, const double* K, const double* T,
                        const double* sigma, const double* df, const Side* side,
                        double* price_out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        price_out[i] = black76_price(F[i], K[i], T[i], sigma[i], df[i], side[i]);
    }
}

void value_vega_batch_scalar(const double* F, const double* K, const double* T,
                             const double* sigma, const double* df,
                             const Side* side, double* price_out,
                             double* vega_out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const Black76ValueVega r =
            black76_value_and_vega(F[i], K[i], T[i], sigma[i], df[i], side[i]);
        price_out[i] = r.price;
        vega_out[i] = r.vega;
    }
}

} // namespace

void black76_price_batch(const double* F, const double* K, const double* T,
                         const double* sigma, const double* df,
                         const Side* side, double* price_out,
                         std::size_t n) noexcept {
    if (use_avx2()) {
        detail::black76_price_batch_avx2(F, K, T, sigma, df, side, price_out, n);
    } else {
        price_batch_scalar(F, K, T, sigma, df, side, price_out, n);
    }
}

void black76_value_vega_batch(const double* F, const double* K, const double* T,
                              const double* sigma, const double* df,
                              const Side* side, double* price_out,
                              double* vega_out, std::size_t n) noexcept {
    if (use_avx2()) {
        detail::black76_value_vega_batch_avx2(F, K, T, sigma, df, side,
                                              price_out, vega_out, n);
    } else {
        value_vega_batch_scalar(F, K, T, sigma, df, side, price_out, vega_out, n);
    }
}

} // namespace atx::vol::simd
