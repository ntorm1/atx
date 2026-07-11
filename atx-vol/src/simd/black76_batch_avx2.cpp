// AVX2 (4-lane f64) batched Black-76 pricer + fused vega.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 contracts;
// the n % 4 tail and any degenerate/deep-wing lanes fall through to the exact
// scalar kernels in atx/vol/black76.hpp, which keeps the batch bit-for-bit with
// the scalar source of truth wherever the Chebyshev-Φ core would lose accuracy.
//
// Per 4-lane pass:
//   v    = σ·√T,  d1 = (ln(F/K) + ½v²)/v,  d2 = d1 - v
//   Φ(·) = Chebyshev-Clenshaw (detail/vector_math.hpp)
//   C    = df·(F·Φ(d1) - K·Φ(d2));  P = df·(K·(1-Φ(d2)) - F·(1-Φ(d1)))
//   vega = F·df·φ(d1)·√T                       (same for calls and puts)
// Degenerate lanes (T ≤ 0 or σ ≤ 0) get dummy finite inputs to stay branchless,
// then are patched; deep-wing lanes (|d1|,|d2| > kNormCdfWing) are patched too.

#include "black76_batch_avx2.hpp"

#include "atx/vol/black76.hpp"
#include "atx/vol/detail/norm_cdf_cheb.hpp"
#include "atx/vol/detail/vector_math.hpp"

#include <immintrin.h>

namespace atx::vol::simd::detail {

// Pull in the shared 4-lane transcendentals (log_pd/exp_pd/norm_cdf_pd/
// norm_pdf_pd) and Chebyshev table, which live in atx::vol::detail.
using namespace atx::vol::detail;

namespace {

// Per-lane sign-bit mask for blendv: Put → -1.0 (select put), Call → 0.0.
ATX_FORCE_INLINE __m256d side_blend_mask(const Side* side, std::size_t i) noexcept {
    alignas(32) double s[4];
    s[0] = (side[i + 0] == Side::Put) ? -1.0 : 0.0;
    s[1] = (side[i + 1] == Side::Put) ? -1.0 : 0.0;
    s[2] = (side[i + 2] == Side::Put) ? -1.0 : 0.0;
    s[3] = (side[i + 3] == Side::Put) ? -1.0 : 0.0;
    return _mm256_load_pd(s);
}

// Lanes needing the exact scalar path: degenerate (T ≤ 0 or σ ≤ 0) OR deep-wing
// (either d exceeds the Chebyshev-accurate interior). Returns a 4-bit movemask.
ATX_FORCE_INLINE int patch_bits(__m256d degen, __m256d d1, __m256d d2) noexcept {
    const __m256d w = _mm256_set1_pd(kNormCdfWing);
    const __m256d nw = _mm256_set1_pd(-kNormCdfWing);
    const __m256d wing = _mm256_or_pd(
        _mm256_or_pd(_mm256_cmp_pd(d1, w, _CMP_GT_OQ),
                     _mm256_cmp_pd(d1, nw, _CMP_LT_OQ)),
        _mm256_or_pd(_mm256_cmp_pd(d2, w, _CMP_GT_OQ),
                     _mm256_cmp_pd(d2, nw, _CMP_LT_OQ)));
    return _mm256_movemask_pd(_mm256_or_pd(degen, wing));
}

} // namespace

void black76_price_batch_avx2(const double* F, const double* K, const double* T,
                              const double* sigma, const double* df,
                              const Side* side, double* price_out,
                              std::size_t n) noexcept {
    const double* coefs = norm_cdf_cheb_coefs().data();
    const __m256d half = _mm256_set1_pd(0.5);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d zero = _mm256_setzero_pd();

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d Fv = _mm256_loadu_pd(F + i);
        const __m256d Kv = _mm256_loadu_pd(K + i);
        const __m256d Tv = _mm256_loadu_pd(T + i);
        const __m256d sv = _mm256_loadu_pd(sigma + i);
        const __m256d dfv = _mm256_loadu_pd(df + i);

        const __m256d degen = _mm256_or_pd(_mm256_cmp_pd(Tv, zero, _CMP_LE_OQ),
                                           _mm256_cmp_pd(sv, zero, _CMP_LE_OQ));
        const __m256d safeT = _mm256_blendv_pd(Tv, one, degen);
        const __m256d safeS = _mm256_blendv_pd(sv, one, degen);

        const __m256d v = _mm256_mul_pd(safeS, _mm256_sqrt_pd(safeT));
        const __m256d lnFK = log_pd(_mm256_div_pd(Fv, Kv));
        const __m256d d1 = _mm256_div_pd(
            _mm256_add_pd(lnFK, _mm256_mul_pd(half, _mm256_mul_pd(v, v))), v);
        const __m256d d2 = _mm256_sub_pd(d1, v);

        __m256d Nd1, Nd2;
        norm_cdf_pd2(d1, d2, coefs, Nd1, Nd2); // fused: hides Clenshaw latency

        const __m256d call = _mm256_mul_pd(
            dfv, _mm256_sub_pd(_mm256_mul_pd(Fv, Nd1), _mm256_mul_pd(Kv, Nd2)));
        const __m256d put = _mm256_mul_pd(
            dfv, _mm256_sub_pd(_mm256_mul_pd(Kv, _mm256_sub_pd(one, Nd2)),
                               _mm256_mul_pd(Fv, _mm256_sub_pd(one, Nd1))));
        __m256d price = _mm256_blendv_pd(call, put, side_blend_mask(side, i));

        const int patch = patch_bits(degen, d1, d2);
        if (patch != 0) {
            alignas(32) double pb[4];
            _mm256_store_pd(pb, price);
            for (int j = 0; j < 4; ++j) {
                if (patch & (1 << j)) {
                    pb[j] = black76_price(F[i + j], K[i + j], T[i + j],
                                          sigma[i + j], df[i + j], side[i + j]);
                }
            }
            price = _mm256_load_pd(pb);
        }
        _mm256_storeu_pd(price_out + i, price);
    }
    for (; i < n; ++i) {
        price_out[i] = black76_price(F[i], K[i], T[i], sigma[i], df[i], side[i]);
    }
}

void black76_value_vega_batch_avx2(const double* F, const double* K,
                                   const double* T, const double* sigma,
                                   const double* df, const Side* side,
                                   double* price_out, double* vega_out,
                                   std::size_t n) noexcept {
    const double* coefs = norm_cdf_cheb_coefs().data();
    const __m256d half = _mm256_set1_pd(0.5);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d zero = _mm256_setzero_pd();

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d Fv = _mm256_loadu_pd(F + i);
        const __m256d Kv = _mm256_loadu_pd(K + i);
        const __m256d Tv = _mm256_loadu_pd(T + i);
        const __m256d sv = _mm256_loadu_pd(sigma + i);
        const __m256d dfv = _mm256_loadu_pd(df + i);

        const __m256d degen = _mm256_or_pd(_mm256_cmp_pd(Tv, zero, _CMP_LE_OQ),
                                           _mm256_cmp_pd(sv, zero, _CMP_LE_OQ));
        const __m256d safeT = _mm256_blendv_pd(Tv, one, degen);
        const __m256d safeS = _mm256_blendv_pd(sv, one, degen);

        const __m256d sqrtT = _mm256_sqrt_pd(safeT);
        const __m256d v = _mm256_mul_pd(safeS, sqrtT);
        const __m256d lnFK = log_pd(_mm256_div_pd(Fv, Kv));
        const __m256d d1 = _mm256_div_pd(
            _mm256_add_pd(lnFK, _mm256_mul_pd(half, _mm256_mul_pd(v, v))), v);
        const __m256d d2 = _mm256_sub_pd(d1, v);

        __m256d Nd1, Nd2;
        norm_cdf_pd2(d1, d2, coefs, Nd1, Nd2); // fused: hides Clenshaw latency

        const __m256d call = _mm256_mul_pd(
            dfv, _mm256_sub_pd(_mm256_mul_pd(Fv, Nd1), _mm256_mul_pd(Kv, Nd2)));
        const __m256d put = _mm256_mul_pd(
            dfv, _mm256_sub_pd(_mm256_mul_pd(Kv, _mm256_sub_pd(one, Nd2)),
                               _mm256_mul_pd(Fv, _mm256_sub_pd(one, Nd1))));
        __m256d price = _mm256_blendv_pd(call, put, side_blend_mask(side, i));

        // vega = F·df·φ(d1)·√T (identical for call/put).
        __m256d vega = _mm256_mul_pd(
            _mm256_mul_pd(_mm256_mul_pd(Fv, dfv), norm_pdf_pd(d1)), sqrtT);

        const int patch = patch_bits(degen, d1, d2);
        if (patch != 0) {
            alignas(32) double pb[4];
            alignas(32) double vb[4];
            _mm256_store_pd(pb, price);
            _mm256_store_pd(vb, vega);
            for (int j = 0; j < 4; ++j) {
                if (patch & (1 << j)) {
                    const Black76ValueVega r =
                        black76_value_and_vega(F[i + j], K[i + j], T[i + j],
                                               sigma[i + j], df[i + j],
                                               side[i + j]);
                    pb[j] = r.price;
                    vb[j] = r.vega;
                }
            }
            price = _mm256_load_pd(pb);
            vega = _mm256_load_pd(vb);
        }
        _mm256_storeu_pd(price_out + i, price);
        _mm256_storeu_pd(vega_out + i, vega);
    }
    for (; i < n; ++i) {
        const Black76ValueVega r =
            black76_value_and_vega(F[i], K[i], T[i], sigma[i], df[i], side[i]);
        price_out[i] = r.price;
        vega_out[i] = r.vega;
    }
}

} // namespace atx::vol::simd::detail
