// AVX2 (4-lane f64) batched eSSVI/SVI total-variance evaluators.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 strikes;
// the n % 4 tail falls through to the exact scalar kernels in
// atx/vol/vol_surface.hpp, keeping the batch bit-for-bit with the scalar source
// of truth on the tail.
//
// eSSVI backbone, per strike k, with a single slice-constant rho:
//   pk    = phi·k
//   inner = (pk + rho)² + (1 - rho²)
//   w     = ½·theta·(1 + rho·pk + sqrt(inner))
// (1 - rho²) and ½·theta are strike-independent, so they are broadcast once
// outside the loop.
//
// ARMED asymmetric-rho blend (rho_scale > 0 AND rho_R != rho): the blend was
// RETIRED (T9) -- there is no strike-dependent rho and no tanh anywhere in this
// library any more, and `essvi_backbone_w` returns NaN for an armed slice
// (`essvi_rho_blend_armed`, vol_surface.cpp:47-54). `blend_active` below is
// therefore a REFUSAL predicate, not a vectorization-capability one: it routes
// an armed slice to the scalar kernel precisely so this path refuses it
// identically to the scalar one, instead of vectorizing a slice the scalar
// source of truth declines to evaluate. rho_R / rho_scale survive only as
// reserved-zero wire vocabulary (vol_surface.hpp:101).
//
// Raw SVI, per strike k:
//   dk = k - m,  w = a + b·(rho·dk + sqrt(dk² + sigma²))
// Pure arithmetic + one sqrt; always vectorized.

#include "essvi_batch_avx2.hpp"

#include "atx/core/macro.hpp" // ATX_FORCE_INLINE
#include "atx/vol/api/fitting/vol_surface.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include <immintrin.h>

namespace atx::vol::simd::detail {

namespace {

// True iff the slice carries an ARMED asymmetric-rho blend, which the scalar
// evaluator refuses with NaN. Mirrors `essvi_rho_blend_armed` (vol_surface.cpp)
// exactly so the two lanes agree on what they will not evaluate.
[[nodiscard]] ATX_FORCE_INLINE bool blend_active(const EssviParams &s) noexcept {
  return (s.rho_scale > 0.0) && (s.rho_R != s.rho);
}

[[nodiscard]] bool slice_vector_admissible(const EssviParams &s) noexcept {
  return std::isfinite(s.theta) && std::isfinite(s.phi) && std::isfinite(s.rho) && s.theta > 0.0 &&
         s.phi > 0.0 && std::abs(s.rho) < 1.0;
}

ATX_FORCE_INLINE __m256d nonfinite_mask(__m256d value) noexcept {
  const __m256d abs_mask = _mm256_set1_pd(-0.0);
  const __m256d max_finite = _mm256_set1_pd(std::numeric_limits<double>::max());
  const __m256d magnitude = _mm256_andnot_pd(abs_mask, value);
  return _mm256_cmp_pd(magnitude, max_finite, _CMP_NLE_UQ);
}

// One 4-lane eSSVI backbone step at constant effective rho.
//   c_v        = 1 - rho²          (broadcast)
//   halfTheta_v= ½·theta           (broadcast)
//
// A9 (simd-review finding 6) — fma policy, DOCUMENTED not changed: the a² + (1-rho²)
// step below (and svi_total_w_batch_avx2's a + b·(…)) use _mm256_fmadd_pd where the
// scalar source (vol_surface.cpp) rounds mul-then-add, so w can diverge from scalar
// by <=1 ulp across hosts. This is accepted for the eSSVI/SVI value batches (unlike
// svi_qe_basis_batch_avx2, which deliberately forbids fma for bit-exact fit parity);
// the per-strike ~1e-12 parity gate already tolerates it. Left as-is by design.
ATX_FORCE_INLINE __m256d essvi_backbone_w4(__m256d kv, __m256d phi_v, __m256d rho_v, __m256d c_v,
                                           __m256d halfTheta_v, __m256d one) noexcept {
  const __m256d pk = _mm256_mul_pd(phi_v, kv);
  const __m256d a = _mm256_add_pd(pk, rho_v);
  const __m256d inner = _mm256_fmadd_pd(a, a, c_v); // a² + (1 - rho²)
  const __m256d r = _mm256_sqrt_pd(inner);
  const __m256d rho_pk = _mm256_mul_pd(rho_v, pk);
  const __m256d term = _mm256_add_pd(_mm256_add_pd(one, rho_pk), r);
  return _mm256_mul_pd(halfTheta_v, term);
}

} // namespace

void essvi_backbone_w_batch_avx2(const EssviParams &slice, const double *k_log, double *w_out,
                                 std::size_t n) noexcept {
  if (blend_active(slice) || !slice_vector_admissible(slice)) {
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
  const __m256d zero = _mm256_setzero_pd();

  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m256d kv = _mm256_loadu_pd(k_log + i);
    const __m256d invalid = nonfinite_mask(kv);
    const __m256d safe_k = _mm256_blendv_pd(kv, zero, invalid);
    __m256d w = essvi_backbone_w4(safe_k, phi_v, rho_v, c_v, halfTheta_v, one);
    const int patch = _mm256_movemask_pd(invalid);
    if (patch != 0) {
      alignas(32) double values[4];
      _mm256_store_pd(values, w);
      for (int lane = 0; lane < 4; ++lane) {
        if (patch & (1 << lane)) {
          const std::size_t index = i + static_cast<std::size_t>(lane);
          values[lane] = essvi_backbone_w(slice, k_log[index]);
        }
      }
      w = _mm256_load_pd(values);
    }
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
void essvi_backbone_w_grad_batch_avx2(const EssviParams &slice, const double *k_log, double *w_out,
                                      double *dw_dtheta, double *dw_dphi, double *dw_drho,
                                      std::size_t n) noexcept {
  // A9 (simd-review finding 6): mirror the w-batch's refusal — a non-admissible
  // slice (|rho| >= 1, non-positive theta/phi, non-finite params) would propagate
  // NaN through the vector path only by accident, so route the WHOLE batch to the
  // exact scalar kernels, exactly as essvi_backbone_w_batch_avx2 does.
  if (blend_active(slice) || !slice_vector_admissible(slice)) {
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
  const __m256d zero = _mm256_setzero_pd();

  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m256d kv = _mm256_loadu_pd(k_log + i);
    // A9 (simd-review finding 6): non-finite k lanes patched from scalar (mirrors
    // the w-batch). safe_k == kv on every finite lane, so those stay bit-identical.
    const __m256d invalid = nonfinite_mask(kv);
    const __m256d safe_k = _mm256_blendv_pd(kv, zero, invalid);
    const __m256d pk = _mm256_mul_pd(phi_v, safe_k);
    const __m256d a = _mm256_add_pd(pk, rho_v);
    const __m256d inner = _mm256_fmadd_pd(a, a, c_v); // a² + (1 - rho²)
    const __m256d r = _mm256_sqrt_pd(inner);
    // Shared term (1 + rho·pk + r): drives both w (·½θ) and ∂w/∂θ (·½).
    const __m256d rho_pk = _mm256_mul_pd(rho_v, pk);
    const __m256d term = _mm256_add_pd(_mm256_add_pd(one, rho_pk), r);
    __m256d w = _mm256_mul_pd(halfTheta_v, term);
    __m256d dth = _mm256_mul_pd(half_v, term);
    // ∂w/∂φ = ½θ·(rho·k + (a·k)/r).
    const __m256d rho_k = _mm256_mul_pd(rho_v, safe_k);
    const __m256d ak = _mm256_mul_pd(a, safe_k);
    const __m256d ak_r = _mm256_div_pd(ak, r);
    const __m256d in_phi = _mm256_add_pd(rho_k, ak_r);
    __m256d dph = _mm256_mul_pd(halfTheta_v, in_phi);
    // ∂w/∂ρ = ½θ·(pk + (a - rho)/r).
    const __m256d amr = _mm256_sub_pd(a, rho_v);
    const __m256d amr_r = _mm256_div_pd(amr, r);
    const __m256d in_rho = _mm256_add_pd(pk, amr_r);
    __m256d drh = _mm256_mul_pd(halfTheta_v, in_rho);
    const int patch = _mm256_movemask_pd(invalid);
    if (patch != 0) {
      alignas(32) double wv[4], dthv[4], dphv[4], drhv[4];
      _mm256_store_pd(wv, w);
      _mm256_store_pd(dthv, dth);
      _mm256_store_pd(dphv, dph);
      _mm256_store_pd(drhv, drh);
      for (int lane = 0; lane < 4; ++lane) {
        if (patch & (1 << lane)) {
          const std::size_t index = i + static_cast<std::size_t>(lane);
          wv[lane] = essvi_backbone_w(slice, k_log[index]);
          const std::array<double, 3> g = essvi_w_grad3(slice, k_log[index]);
          dthv[lane] = g[0];
          dphv[lane] = g[1];
          drhv[lane] = g[2];
        }
      }
      w = _mm256_load_pd(wv);
      dth = _mm256_load_pd(dthv);
      dph = _mm256_load_pd(dphv);
      drh = _mm256_load_pd(drhv);
    }
    _mm256_storeu_pd(w_out + i, w);
    _mm256_storeu_pd(dw_dtheta + i, dth);
    _mm256_storeu_pd(dw_dphi + i, dph);
    _mm256_storeu_pd(dw_drho + i, drh);
  }
  for (; i < n; ++i) {
    w_out[i] = essvi_backbone_w(slice, k_log[i]);
    const std::array<double, 3> g = essvi_w_grad3(slice, k_log[i]);
    dw_dtheta[i] = g[0];
    dw_dphi[i] = g[1];
    dw_drho[i] = g[2];
  }
}

void svi_total_w_batch_avx2(const SviParams &slice, const double *k_log, double *w_out,
                            std::size_t n) noexcept {
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
    const __m256d s = _mm256_add_pd(rho_dk, r);     // rho·dk + r
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
void svi_qe_basis_batch_avx2(double m, double sigma, const double *k, double *u_out, double *v_out,
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
    const __m256d y = _mm256_div_pd(dk, sigma_v); // (k - m) / sigma
    const __m256d yy = _mm256_mul_pd(y, y);
    const __m256d t = _mm256_add_pd(yy, one); // y² + 1 (no fma)
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

void essvi_backbone_sigma_batch_avx2(const EssviParams &slice, const double *k_log,
                                     double *sigma_out, std::size_t n) noexcept {
  // The time axis is the DIVISOR here, and an unusable one is a refusal, not a
  // fallback: routing to the scalar loop would divide by the same T. See
  // essvi_slice_time_valid (essvi_batch_avx2.hpp) for what went out un-flagged before.
  if (!essvi_slice_time_valid(slice)) {
    for (std::size_t i = 0; i < n; ++i) {
      sigma_out[i] = std::numeric_limits<double>::quiet_NaN();
    }
    return;
  }
  // A9 (simd-review finding 6): mirror the w-batch's admissibility refusal (see
  // essvi_backbone_w_grad_batch_avx2) so a |rho| >= 1 / non-positive-theta/phi
  // slice routes to the exact scalar kernel instead of leaking a NaN by accident.
  if (blend_active(slice) || !slice_vector_admissible(slice)) {
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
    // A9 (simd-review finding 6): non-finite k lanes patched from scalar (mirrors
    // the w-batch). safe_k == kv on every finite lane, so those stay bit-identical.
    const __m256d invalid = nonfinite_mask(kv);
    const __m256d safe_k = _mm256_blendv_pd(kv, zero, invalid);
    const __m256d w = essvi_backbone_w4(safe_k, phi_v, rho_v, c_v, halfTheta_v, one);
    const __m256d wmax = _mm256_max_pd(w, zero);
    __m256d sig = _mm256_sqrt_pd(_mm256_div_pd(wmax, T_v));
    const int patch = _mm256_movemask_pd(invalid);
    if (patch != 0) {
      alignas(32) double values[4];
      _mm256_store_pd(values, sig);
      for (int lane = 0; lane < 4; ++lane) {
        if (patch & (1 << lane)) {
          const std::size_t index = i + static_cast<std::size_t>(lane);
          const double w_s = essvi_backbone_w(slice, k_log[index]);
          values[lane] = std::sqrt(std::max(w_s, 0.0) / slice.T);
        }
      }
      sig = _mm256_load_pd(values);
    }
    _mm256_storeu_pd(sigma_out + i, sig);
  }
  for (; i < n; ++i) {
    const double w = essvi_backbone_w(slice, k_log[i]);
    sigma_out[i] = std::sqrt(std::max(w, 0.0) / slice.T);
  }
}

} // namespace atx::vol::simd::detail
