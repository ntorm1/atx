// AVX2 (4-lane f64) batched Black-76 Greeks + price.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 contracts;
// the n % 4 tail and any degenerate/deep-wing lanes fall through to the exact
// scalar kernel atx::vol::black76_greeks, which keeps the batch bit-for-bit with
// the scalar source of truth wherever the Chebyshev-Φ core would lose accuracy.
//
// Per 4-lane pass (shares the d1/d2/Φ math with the pricer):
//   v    = σ·√T,  d1 = (ln(F/K) + ½v²)/v,  d2 = d1 - v
//   Φ(·) = Chebyshev-Clenshaw (fused Φ(d1)+Φ(d2)),  φ(d1) = vectorized pdf
//   price/delta are side-dependent; the remaining seven Greeks are call/put
//   symmetric. Field formulas match src/greeks.cpp exactly, including the
//   calendar-time theta (∂P/∂t = -∂P/∂T) and rho = -T·price sign conventions:
//     gamma = df·φ(d1)/(F·v)          vega  = df·F·φ(d1)·√T
//     vanna = -df·φ(d1)·d2/σ          volga = vega·d1·d2/σ
//     theta = r·price - df·F·φ(d1)·σ/(2√T)
//     charm = r·delta + df·φ(d1)·d2/(2T)   rho = -T·price
// Results are scattered to the AoS Greeks[] via aligned stores to stack buffers
// then a per-lane copy. Degenerate lanes (T ≤ 0 or σ ≤ 0) get dummy finite
// inputs to stay branchless, then are patched; deep-wing lanes (|d1|,|d2| >
// kNormCdfWing) are patched too.

#include "greeks_batch_avx2.hpp"

#include "atx/vol/detail/norm_cdf_cheb.hpp"
#include "atx/vol/detail/vector_math.hpp"
#include "atx/vol/greeks.hpp"

#include <immintrin.h>

namespace atx::vol::simd::detail {

// Pull in the shared 4-lane transcendentals (log_pd/exp_pd/norm_cdf_pd2/
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

void black76_greeks_batch_avx2(const double* F, const double* K, const double* T,
                               const double* sigma, const double* r,
                               const double* df, const Side* side,
                               Greeks* greeks_out, double* price_out,
                               std::size_t n) noexcept {
    const double* coefs = norm_cdf_cheb_coefs().data();
    const __m256d half = _mm256_set1_pd(0.5);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d two = _mm256_set1_pd(2.0);
    const __m256d zero = _mm256_setzero_pd();

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d Fv = _mm256_loadu_pd(F + i);
        const __m256d Kv = _mm256_loadu_pd(K + i);
        const __m256d Tv = _mm256_loadu_pd(T + i);
        const __m256d sv = _mm256_loadu_pd(sigma + i);
        const __m256d rv = _mm256_loadu_pd(r + i);
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
        const __m256d phi = norm_pdf_pd(d1);   // φ(d1), shared across Greeks

        // Prices (call & put); side blend selects per lane.
        const __m256d cNd1 = _mm256_sub_pd(one, Nd1);
        const __m256d cNd2 = _mm256_sub_pd(one, Nd2);
        const __m256d call = _mm256_mul_pd(
            dfv, _mm256_sub_pd(_mm256_mul_pd(Fv, Nd1), _mm256_mul_pd(Kv, Nd2)));
        const __m256d put = _mm256_mul_pd(
            dfv, _mm256_sub_pd(_mm256_mul_pd(Kv, cNd2), _mm256_mul_pd(Fv, cNd1)));
        const __m256d smask = side_blend_mask(side, i);
        const __m256d price = _mm256_blendv_pd(call, put, smask);

        // delta: call = df·Φ(d1); put = -df·(1-Φ(d1)).
        const __m256d delta_call = _mm256_mul_pd(dfv, Nd1);
        const __m256d delta_put = _mm256_sub_pd(zero, _mm256_mul_pd(dfv, cNd1));
        const __m256d delta = _mm256_blendv_pd(delta_call, delta_put, smask);

        // Call/put-symmetric Greeks (match src/greeks.cpp term-for-term).
        const __m256d df_phi = _mm256_mul_pd(dfv, phi);
        const __m256d gamma =
            _mm256_div_pd(df_phi, _mm256_mul_pd(Fv, v)); // df·φ/(F·v)
        const __m256d vega =
            _mm256_mul_pd(_mm256_mul_pd(df_phi, Fv), sqrtT); // df·F·φ·√T
        const __m256d vanna = _mm256_div_pd(
            _mm256_sub_pd(zero, _mm256_mul_pd(df_phi, d2)), safeS); // -df·φ·d2/σ
        const __m256d volga = _mm256_div_pd(
            _mm256_mul_pd(_mm256_mul_pd(vega, d1), d2), safeS); // vega·d1·d2/σ
        // theta = r·price - df·F·φ·σ/(2√T)  (calendar-time).
        const __m256d theta = _mm256_sub_pd(
            _mm256_mul_pd(rv, price),
            _mm256_div_pd(_mm256_mul_pd(_mm256_mul_pd(df_phi, Fv), safeS),
                          _mm256_mul_pd(two, sqrtT)));
        // charm = r·delta + df·φ·d2/(2T).
        const __m256d charm = _mm256_add_pd(
            _mm256_mul_pd(rv, delta),
            _mm256_div_pd(_mm256_mul_pd(df_phi, d2), _mm256_mul_pd(two, safeT)));
        // rho = -T·price (same form for call and put).
        const __m256d rho = _mm256_mul_pd(_mm256_sub_pd(zero, Tv), price);

        // Scatter to the AoS Greeks[] via aligned stores then a per-lane copy;
        // patched lanes recompute through the exact scalar kernel instead.
        alignas(32) double dl[4], gm[4], vg[4], th[4], rh[4], vn[4], vl[4], cm[4];
        alignas(32) double pr[4];
        _mm256_store_pd(dl, delta);
        _mm256_store_pd(gm, gamma);
        _mm256_store_pd(vg, vega);
        _mm256_store_pd(th, theta);
        _mm256_store_pd(rh, rho);
        _mm256_store_pd(vn, vanna);
        _mm256_store_pd(vl, volga);
        _mm256_store_pd(cm, charm);
        _mm256_store_pd(pr, price);

        const int patch = patch_bits(degen, d1, d2);
        for (int j = 0; j < 4; ++j) {
            if (patch & (1 << j)) {
                const Black76Greeks g =
                    black76_greeks(F[i + j], K[i + j], T[i + j], sigma[i + j],
                                   r[i + j], df[i + j], side[i + j]);
                greeks_out[i + j] = g.greeks;
                price_out[i + j] = g.price;
            } else {
                Greeks& g = greeks_out[i + j];
                g.delta = dl[j];
                g.gamma = gm[j];
                g.vega = vg[j];
                g.theta = th[j];
                g.rho = rh[j];
                g.vanna = vn[j];
                g.volga = vl[j];
                g.charm = cm[j];
                price_out[i + j] = pr[j];
            }
        }
    }
    for (; i < n; ++i) {
        const Black76Greeks g =
            black76_greeks(F[i], K[i], T[i], sigma[i], r[i], df[i], side[i]);
        greeks_out[i] = g.greeks;
        price_out[i] = g.price;
    }
}

} // namespace atx::vol::simd::detail
