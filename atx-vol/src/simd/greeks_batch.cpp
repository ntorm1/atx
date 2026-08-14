#include "atx/vol/api/simd/greeks_batch.hpp"

#include "atx/vol/api/pricing/greeks.hpp"
#include "atx/vol/api/simd/cpu.hpp"

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
        // A9 (simd-review finding 8): null-check price_out like the AVX2 AoS sink
        // (greeks_batch_avx2.cpp). A nullptr price_out is a valid "greeks only"
        // request; dereferencing it unconditionally crashed on a non-AVX2 host.
        if (price_out != nullptr) {
            price_out[i] = g.price;
        }
    }
}

// SoA scalar reference: identical numbers to the AoS loop, scattered per column
// (null columns skipped). The numerical source of truth the AVX2 SoA path grades
// against and the fallback on a non-AVX2 host.
void greeks_batch_soa_scalar(const double* F, const double* K, const double* T,
                             const double* sigma, const double* r,
                             const double* df, const Side* side,
                             const GreeksBatchSoA& out, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        const Black76Greeks g =
            black76_greeks(F[i], K[i], T[i], sigma[i], r[i], df[i], side[i]);
        if (out.delta) out.delta[i] = g.greeks.delta;
        if (out.gamma) out.gamma[i] = g.greeks.gamma;
        if (out.vega) out.vega[i] = g.greeks.vega;
        if (out.theta) out.theta[i] = g.greeks.theta;
        if (out.rho) out.rho[i] = g.greeks.rho;
        if (out.vanna) out.vanna[i] = g.greeks.vanna;
        if (out.volga) out.volga[i] = g.greeks.volga;
        if (out.charm) out.charm[i] = g.greeks.charm;
        if (out.price) out.price[i] = g.price;
    }
}

} // namespace

void black76_greeks_batch_soa(const double* F, const double* K, const double* T,
                              const double* sigma, const double* r,
                              const double* df, const Side* side,
                              const GreeksBatchSoA& out, std::size_t n) noexcept {
    if (use_avx2()) {
        detail::black76_greeks_batch_soa_avx2(F, K, T, sigma, r, df, side, out,
                                              n);
    } else {
        greeks_batch_soa_scalar(F, K, T, sigma, r, df, side, out, n);
    }
}

void black76_greeks_batch(const double* F, const double* K, const double* T,
                          const double* sigma, const double* r,
                          const double* df, const Side* side,
                          Greeks* greeks_out, double* price_out,
                          std::size_t n) noexcept {
    if (use_avx2()) {
        detail::black76_greeks_batch_avx2(F, K, T, sigma, r, df, side,
                                          greeks_out, price_out, n);
    } else {
        greeks_batch_scalar(F, K, T, sigma, r, df, side, greeks_out, price_out,
                            n);
    }
}

} // namespace atx::vol::simd
