// AVX2 (4-lane f64) batched eSSVI/SVI total-variance evaluators.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 strikes;
// the n % 4 tail falls through to the exact scalar kernels in
// atx/vol/vol_surface.hpp, keeping the batch bit-for-bit with the scalar source
// of truth on the tail.
//
// eSSVI backbone (symmetric effective rho), per strike k:
//   pk    = phi·k
//   inner = (pk + rho)² + (1 - rho²)
//   w     = ½·theta·(1 + rho·pk + sqrt(inner))
// This vectorizes with a constant effective rho == slice.rho, which is exactly
// what the scalar rho_eff() returns whenever the asymmetric blend is inactive,
// i.e. rho_scale <= 0 OR rho_R == rho. (1 - rho²) and ½·theta are strike-
// independent, so they are broadcast once outside the loop.
//
// Asymmetric-rho blend (rho_scale > 0 AND rho_R != rho): the scalar effective
// rho is rho + (rho_R - rho)·½(1 + tanh(k/rho_scale)), i.e. rho becomes strike-
// dependent through tanh. There is no bit-exact 4-lane tanh here (vector_math's
// transcendentals are approximations and would break the ~1e-12 per-strike
// parity), so the WHOLE batch falls back to the scalar essvi_backbone_w in that
// regime. The common symmetric path stays fully vectorized.
//
// Raw SVI, per strike k:
//   dk = k - m,  w = a + b·(rho·dk + sqrt(dk² + sigma²))
// Pure arithmetic + one sqrt; always vectorized.

#include "essvi_batch_avx2.hpp"

#include "atx/core/macro.hpp" // ATX_FORCE_INLINE
#include "atx/vol/vol_surface.hpp"

#include <algorithm>
#include <cmath>

#include <immintrin.h>

namespace atx::vol::simd::detail {

namespace {

// True iff the scalar rho_eff() would vary with strike (the tanh blend path).
// Mirrors vol_surface.cpp: constant rho when rho_scale <= 0 or rho_R == rho.
[[nodiscard]] ATX_FORCE_INLINE bool blend_active(const EssviParams& s) noexcept {
    return (s.rho_scale > 0.0) && (s.rho_R != s.rho);
}

// One 4-lane eSSVI backbone step at constant effective rho.
//   c_v        = 1 - rho²          (broadcast)
//   halfTheta_v= ½·theta           (broadcast)
ATX_FORCE_INLINE __m256d essvi_backbone_w4(__m256d kv, __m256d phi_v,
                                           __m256d rho_v, __m256d c_v,
                                           __m256d halfTheta_v,
                                           __m256d one) noexcept {
    const __m256d pk = _mm256_mul_pd(phi_v, kv);
    const __m256d a = _mm256_add_pd(pk, rho_v);
    const __m256d inner = _mm256_fmadd_pd(a, a, c_v); // a² + (1 - rho²)
    const __m256d r = _mm256_sqrt_pd(inner);
    const __m256d rho_pk = _mm256_mul_pd(rho_v, pk);
    const __m256d term = _mm256_add_pd(_mm256_add_pd(one, rho_pk), r);
    return _mm256_mul_pd(halfTheta_v, term);
}

} // namespace

void essvi_backbone_w_batch_avx2(const EssviParams& slice, const double* k_log,
                                 double* w_out, std::size_t n) noexcept {
    if (blend_active(slice)) {
        for (std::size_t i = 0; i < n; ++i) {
            w_out[i] = essvi_backbone_w(slice, k_log[i]);
        }
        return;
    }

    const double rho = slice.rho;
    const __m256d phi_v = _mm256_set1_pd(slice.phi);
    const __m256d rho_v = _mm256_set1_pd(rho);
    const __m256d c_v = _mm256_set1_pd(1.0 - rho * rho);
    const __m256d halfTheta_v = _mm256_set1_pd(0.5 * slice.theta);
    const __m256d one = _mm256_set1_pd(1.0);

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d kv = _mm256_loadu_pd(k_log + i);
        const __m256d w =
            essvi_backbone_w4(kv, phi_v, rho_v, c_v, halfTheta_v, one);
        _mm256_storeu_pd(w_out + i, w);
    }
    for (; i < n; ++i) {
        w_out[i] = essvi_backbone_w(slice, k_log[i]);
    }
}

void svi_total_w_batch_avx2(const SviParams& slice, const double* k_log,
                            double* w_out, std::size_t n) noexcept {
    const __m256d a_v = _mm256_set1_pd(slice.a);
    const __m256d b_v = _mm256_set1_pd(slice.b);
    const __m256d rho_v = _mm256_set1_pd(slice.rho);
    const __m256d m_v = _mm256_set1_pd(slice.m);
    const __m256d sig2_v = _mm256_set1_pd(slice.sigma * slice.sigma);

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d kv = _mm256_loadu_pd(k_log + i);
        const __m256d dk = _mm256_sub_pd(kv, m_v);
        const __m256d inner = _mm256_fmadd_pd(dk, dk, sig2_v); // dk² + sigma²
        const __m256d r = _mm256_sqrt_pd(inner);
        const __m256d rho_dk = _mm256_mul_pd(rho_v, dk);
        const __m256d s = _mm256_add_pd(rho_dk, r);   // rho·dk + r
        const __m256d w = _mm256_fmadd_pd(b_v, s, a_v); // a + b·(…)
        _mm256_storeu_pd(w_out + i, w);
    }
    for (; i < n; ++i) {
        w_out[i] = svi_total_w(slice, k_log[i]);
    }
}

void essvi_backbone_sigma_batch_avx2(const EssviParams& slice,
                                     const double* k_log, double* sigma_out,
                                     std::size_t n) noexcept {
    if (blend_active(slice)) {
        for (std::size_t i = 0; i < n; ++i) {
            const double w = essvi_backbone_w(slice, k_log[i]);
            sigma_out[i] = std::sqrt(std::max(w, 0.0) / slice.T);
        }
        return;
    }

    const double rho = slice.rho;
    const __m256d phi_v = _mm256_set1_pd(slice.phi);
    const __m256d rho_v = _mm256_set1_pd(rho);
    const __m256d c_v = _mm256_set1_pd(1.0 - rho * rho);
    const __m256d halfTheta_v = _mm256_set1_pd(0.5 * slice.theta);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d zero = _mm256_setzero_pd();
    const __m256d T_v = _mm256_set1_pd(slice.T); // divisor (matches scalar)

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d kv = _mm256_loadu_pd(k_log + i);
        const __m256d w =
            essvi_backbone_w4(kv, phi_v, rho_v, c_v, halfTheta_v, one);
        const __m256d wmax = _mm256_max_pd(w, zero);
        const __m256d sig = _mm256_sqrt_pd(_mm256_div_pd(wmax, T_v));
        _mm256_storeu_pd(sigma_out + i, sig);
    }
    for (; i < n; ++i) {
        const double w = essvi_backbone_w(slice, k_log[i]);
        sigma_out[i] = std::sqrt(std::max(w, 0.0) / slice.T);
    }
}

} // namespace atx::vol::simd::detail
