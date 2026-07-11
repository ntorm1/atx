#include "atx/vol/simd/greeks_batch.hpp"

#include "atx/vol/greeks.hpp"
#include "atx/vol/simd/cpu.hpp"

#include "greeks_batch_avx2.hpp"

namespace atx::vol::simd {

namespace {

void greeks_batch_scalar(const double* F, const double* K, const double* T,
                         const double* sigma, const double* r, const double* df,
                         const Side* side, Greeks* greeks_out,
                         double* price_out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const Black76Greeks g =
            black76_greeks(F[i], K[i], T[i], sigma[i], r[i], df[i], side[i]);
        greeks_out[i] = g.greeks;
        price_out[i] = g.price;
    }
}

} // namespace

void black76_greeks_batch(const double* F, const double* K, const double* T,
                          const double* sigma, const double* r,
                          const double* df, const Side* side,
                          Greeks* greeks_out, double* price_out,
                          std::size_t n) noexcept {
    if (have_avx2()) {
        detail::black76_greeks_batch_avx2(F, K, T, sigma, r, df, side,
                                          greeks_out, price_out, n);
    } else {
        greeks_batch_scalar(F, K, T, sigma, r, df, side, greeks_out, price_out,
                            n);
    }
}

} // namespace atx::vol::simd
