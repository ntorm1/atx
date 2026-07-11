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
#include <array>
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

// Fused eSSVI backbone w + natural gradient {∂w/∂θ, ∂w/∂φ, ∂w/∂ρ}, sharing the
// backbone subexpression tree (pk / a / inner / sqrt) between w and the three
// partials — one evaluation, versus the scalar path's essvi_backbone_w THEN
// essvi_w_grad3 each rebuilding the same tree. The partials are formed from the
// SAME closed forms essvi_w_grad3 uses (vol_surface.cpp:120-132), op-for-op, so
// the vector result matches the scalar reference to ~1e-12:
//   w      = ½θ·(1 + rho·pk + r)          ∂w/∂θ = ½·(1 + rho·pk + r)   (shared term)
//   ∂w/∂φ  = ½θ·(rho·k + (a·k)/r)
//   ∂w/∂ρ  = ½θ·(pk + (a - rho)/r)        [a - rho formed explicitly, matching
//                                          the scalar op — NOT reusing pk]
// The w column is bit-identical to essvi_backbone_w_batch_avx2 (same helper ops).
void essvi_backbone_w_grad_batch_avx2(const EssviParams& slice,
                                      const double* k_log, double* w_out,
                                      double* dw_dtheta, double* dw_dphi,
                                      double* dw_drho, std::size_t n) noexcept {
    if (blend_active(slice)) {
        for (std::size_t i = 0; i < n; ++i) {
            w_out[i] = essvi_backbone_w(slice, k_log[i]);
            const std::array<double, 3> g = essvi_w_grad3(slice, k_log[i]);
            dw_dtheta[i] = g[0];
            dw_dphi[i] = g[1];
            dw_drho[i] = g[2];
        }
        return;
    }

    const double rho = slice.rho;
    const __m256d phi_v = _mm256_set1_pd(slice.phi);
    const __m256d rho_v = _mm256_set1_pd(rho);
    const __m256d c_v = _mm256_set1_pd(1.0 - rho * rho);
    const __m256d halfTheta_v = _mm256_set1_pd(0.5 * slice.theta);
    const __m256d half_v = _mm256_set1_pd(0.5);
    const __m256d one = _mm256_set1_pd(1.0);

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d kv = _mm256_loadu_pd(k_log + i);
        const __m256d pk = _mm256_mul_pd(phi_v, kv);
        const __m256d a = _mm256_add_pd(pk, rho_v);
        const __m256d inner = _mm256_fmadd_pd(a, a, c_v); // a² + (1 - rho²)
        const __m256d r = _mm256_sqrt_pd(inner);
        // Shared term (1 + rho·pk + r): drives both w (·½θ) and ∂w/∂θ (·½).
        const __m256d rho_pk = _mm256_mul_pd(rho_v, pk);
        const __m256d term = _mm256_add_pd(_mm256_add_pd(one, rho_pk), r);
        _mm256_storeu_pd(w_out + i, _mm256_mul_pd(halfTheta_v, term));
        _mm256_storeu_pd(dw_dtheta + i, _mm256_mul_pd(half_v, term));
        // ∂w/∂φ = ½θ·(rho·k + (a·k)/r).
        const __m256d rho_k = _mm256_mul_pd(rho_v, kv);
        const __m256d ak = _mm256_mul_pd(a, kv);
        const __m256d ak_r = _mm256_div_pd(ak, r);
        const __m256d in_phi = _mm256_add_pd(rho_k, ak_r);
        _mm256_storeu_pd(dw_dphi + i, _mm256_mul_pd(halfTheta_v, in_phi));
        // ∂w/∂ρ = ½θ·(pk + (a - rho)/r).
        const __m256d amr = _mm256_sub_pd(a, rho_v);
        const __m256d amr_r = _mm256_div_pd(amr, r);
        const __m256d in_rho = _mm256_add_pd(pk, amr_r);
        _mm256_storeu_pd(dw_drho + i, _mm256_mul_pd(halfTheta_v, in_rho));
    }
    for (; i < n; ++i) {
        w_out[i] = essvi_backbone_w(slice, k_log[i]);
        const std::array<double, 3> g = essvi_w_grad3(slice, k_log[i]);
        dw_dtheta[i] = g[0];
        dw_dphi[i] = g[1];
        dw_drho[i] = g[2];
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

// Raw-SVI quasi-explicit rotated basis, per strike k at fixed (m, sigma):
//   y = (k - m) / sigma,  z = sqrt(y² + 1)
//   u = (y + z) / sqrt(2),  v = (z - y) / sqrt(2)
// Op-for-op with the scalar source of truth (svi_calib.cpp build_and_solve_normal
// / svi_qe_sse): the y²+1 step is a mul THEN add (NOT fmadd), so it matches the
// scalar two-rounding sequence — div and sqrt are correctly-rounded IEEE — and
// parity is bit-exact (the VALUES-ONLY rule keeps the fit's H/g accumulation a
// scalar loop in the caller). Always vectorized; n % 4 tail via the exact ops.
void svi_qe_basis_batch_avx2(double m, double sigma, const double* k,
                             double* u_out, double* v_out,
                             std::size_t n) noexcept {
    constexpr double kInvSqrt2 = 0.70710678118654752440;
    const __m256d m_v = _mm256_set1_pd(m);
    const __m256d sigma_v = _mm256_set1_pd(sigma);
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d invsqrt2_v = _mm256_set1_pd(kInvSqrt2);

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d kv = _mm256_loadu_pd(k + i);
        const __m256d dk = _mm256_sub_pd(kv, m_v);
        const __m256d y = _mm256_div_pd(dk, sigma_v);      // (k - m) / sigma
        const __m256d yy = _mm256_mul_pd(y, y);
        const __m256d t = _mm256_add_pd(yy, one);          // y² + 1 (no fma)
        const __m256d z = _mm256_sqrt_pd(t);
        const __m256d u = _mm256_mul_pd(_mm256_add_pd(y, z), invsqrt2_v);
        const __m256d v = _mm256_mul_pd(_mm256_sub_pd(z, y), invsqrt2_v);
        _mm256_storeu_pd(u_out + i, u);
        _mm256_storeu_pd(v_out + i, v);
    }
    for (; i < n; ++i) {
        const double y = (k[i] - m) / sigma;
        const double z = std::sqrt(y * y + 1.0);
        u_out[i] = (y + z) * kInvSqrt2;
        v_out[i] = (z - y) * kInvSqrt2;
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
