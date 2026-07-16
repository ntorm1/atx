// AVX2 (4-lane f64) batched implied-volatility inversion.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 contracts;
// the n % 4 tail and any lane the vector path cannot invert to full accuracy
// fall through to the exact scalar atx::vol::implied_vol, which keeps the batch
// bit-for-bit with the scalar source of truth on those lanes.
//
// Algorithm per 4-lane pass (mirrors the scalar implied_vol / the C ats-vol
// ats_pricer_iv_avx2.c):
//
//   1. Vectorized put-call parity: ITM lanes are rewritten to their OTM
//      equivalent (price_eff, eps_eff) before the seed — SR-2017's Polya-CDF
//      form is well conditioned only on the OTM tail.
//   2. Vectorized Stefanica-Radoicic (2017) closed-form seed: one vector log +
//      four vector exp, branchless; a validity mask (β>0, γ≥|y|, σ>0) flags
//      lanes where the quadratic degenerates.
//   3. Two Halley (order-3) steps on the *original* (price, side): fused
//      norm_cdf_pd2 for Φ(d1),Φ(d2) and norm_pdf_pd for φ(d1). f = model-price,
//      f' = vega = df·F·φ(d1)·√T, f'' = volga = vega·d1·d2/σ; step bounded to
//      [-σ/2, σ] then σ clamped to [kIvMin, kIvMax] — identical to the scalar.
//   4. Per-lane patch to scalar for: degenerate inputs (T/F/K/df ≤ 0), an
//      invalid seed, a non-finite result, deep-wing d (|d| > kNormCdfWing, where
//      Cheb-Φ loses accuracy), an ill-conditioned lane (vega too small, so the
//      Cheb-Φ price bias would move σ more than the round-trip tolerance), or a
//      non-converged residual. Patched lanes call implied_vol, matching scalar
//      exactly; accepted lanes keep the vector σ (within ~1e-9 of scalar).

#include "iv_batch_avx2.hpp"

#include "atx/vol/detail/norm_cdf_cheb.hpp"
#include "atx/vol/detail/vector_math.hpp"
#include "atx/vol/implied_vol.hpp"

#include <immintrin.h>

#include <limits>

namespace atx::vol::simd::detail {

// Pull in the shared 4-lane transcendentals (log_pd/exp_pd/norm_cdf_pd2/
// norm_pdf_pd), the Chebyshev table, and kNormCdfWing — all in atx::vol::detail.
using namespace atx::vol::detail;

namespace {

// High-precision π (matches the scalar seed's Polya factor 2/π to the last ULP).
constexpr double kPi = 3.141592653589793238462643383279502884;
constexpr double kTwoOverPi = 2.0 / kPi;
constexpr double kOneMinusKp = 1.0 - kTwoOverPi;

// Accept a lane's vector σ only if its Halley residual (computed with Cheb-Φ)
// falls under this. Cheb-Φ has ~1e-11 uniform absolute error, so a converged
// interior lane sits far below 1e-6; a lane that stays above it is genuinely
// non-converged (seed pathology / near-arb edge) and patches to scalar.
constexpr double kIvResidTol = 1.0e-6;

// Conditioning gate. σ-parity to scalar ≈ (Cheb-Φ price bias)/vega, with the
// price bias ≈ εΦ·(F+K)·df and εΦ ≈ few·1e-11. Requiring vega ≥ coef·(F+K)·df
// keeps that ratio ≲ εΦ/coef ≈ 1e-9 for accepted lanes (F cancels, so the gate
// is forward-scale invariant); lower-vega lanes patch to scalar. coef = 0.05
// leaves ~10× headroom under the round-trip tolerance.
constexpr double kIvVegaFloorCoef = 0.05;

// Vectorized SR-2017 closed-form IV seed. Returns σ clamped to [kIvMin, kIvMax];
// `valid` receives an all-ones lane where the closed form is well posed. See
// implied_vol.cpp::seed_sr2017_core for the scalar reference and derivation.
ATX_FORCE_INLINE __m256d sr2017_seed_pd(__m256d price, __m256d F, __m256d K, __m256d T, __m256d df,
                                        __m256d eps, __m256d &valid) noexcept {
  const __m256d zero = _mm256_setzero_pd();
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d two = _mm256_set1_pd(2.0);
  const __m256d four = _mm256_set1_pd(4.0);
  const __m256d kp = _mm256_set1_pd(kTwoOverPi);
  const __m256d one_m_kp = _mm256_set1_pd(kOneMinusKp);

  // Put-call parity flip ITM → OTM. ITM iff eps·(F − K) > 0.
  const __m256d FmK = _mm256_sub_pd(F, K);
  const __m256d itm = _mm256_cmp_pd(_mm256_mul_pd(eps, FmK), zero, _CMP_GT_OQ);
  const __m256d delta = _mm256_mul_pd(df, FmK);
  const __m256d adj = _mm256_blendv_pd(zero, _mm256_mul_pd(eps, delta), itm);
  __m256d price_eff = _mm256_sub_pd(price, adj);
  price_eff = _mm256_max_pd(price_eff, _mm256_set1_pd(1.0e-15));
  const __m256d eps_eff = _mm256_blendv_pd(eps, _mm256_sub_pd(zero, eps), itm);

  // SR-2017 main formula (paper Table 3).
  const __m256d y = log_pd(_mm256_div_pd(F, K));
  const __m256d Q = _mm256_div_pd(price_eff, _mm256_mul_pd(df, K));
  const __m256d f = _mm256_div_pd(F, K); // exact F/K, no exp(log) round-trip
  const __m256d f_m1 = _mm256_sub_pd(f, one);
  const __m256d R = _mm256_sub_pd(_mm256_mul_pd(two, Q), _mm256_mul_pd(eps_eff, f_m1));

  const __m256d pky = _mm256_mul_pd(kp, y);
  const __m256d e_pky = exp_pd(pky);
  const __m256d e_mky = exp_pd(_mm256_sub_pd(zero, pky));
  const __m256d y_h = _mm256_mul_pd(one_m_kp, y);
  const __m256d e_y_h = exp_pd(y_h);
  const __m256d e_y_l = exp_pd(_mm256_sub_pd(zero, y_h));

  const __m256d diff = _mm256_sub_pd(e_y_h, e_y_l);
  const __m256d A = _mm256_mul_pd(diff, diff);
  const __m256d sum_h = _mm256_add_pd(e_y_h, e_y_l);
  const __m256d f2 = _mm256_mul_pd(f, f);
  const __m256d f2_R2 = _mm256_sub_pd(f2, _mm256_mul_pd(R, R));
  // B = 4(e_mky + e_pky) − 2 sum_h (1 + f² − R²)/f.
  const __m256d term1 = _mm256_mul_pd(four, _mm256_add_pd(e_mky, e_pky));
  const __m256d inner = _mm256_add_pd(one, f2_R2);
  const __m256d term2 = _mm256_div_pd(_mm256_mul_pd(_mm256_mul_pd(two, sum_h), inner), f);
  const __m256d B = _mm256_sub_pd(term1, term2);

  // C = (4Q/f²)(Q − ε(f−1))((f+1)−R)((f+1)+R).
  const __m256d Q_minus = _mm256_sub_pd(Q, _mm256_mul_pd(eps_eff, f_m1));
  const __m256d f_plus = _mm256_add_pd(f, one);
  const __m256d Cf1 = _mm256_sub_pd(f_plus, R);
  const __m256d Cf2 = _mm256_add_pd(f_plus, R);
  const __m256d coef = _mm256_div_pd(_mm256_mul_pd(four, Q), f2);
  const __m256d Cc = _mm256_mul_pd(_mm256_mul_pd(coef, Q_minus), _mm256_mul_pd(Cf1, Cf2));

  // disc = max(B² + 4AC, 0); β = 2C / (B + √disc) (stable, avoids B−√ cancel).
  __m256d disc = _mm256_fmadd_pd(four, _mm256_mul_pd(A, Cc), _mm256_mul_pd(B, B));
  disc = _mm256_max_pd(disc, zero);
  const __m256d denom = _mm256_add_pd(B, _mm256_sqrt_pd(disc));
  const __m256d beta = _mm256_div_pd(_mm256_mul_pd(two, Cc), denom);

  // γ = −log(β)/k. Clamp β to a tiny positive floor so a bad lane yields a
  // finite (invalid-flagged) value rather than NaN.
  const __m256d beta_safe = _mm256_max_pd(beta, _mm256_set1_pd(1.0e-300));
  const __m256d gamma = _mm256_div_pd(_mm256_sub_pd(zero, log_pd(beta_safe)), kp);

  // σ√T = √(γ+y) + √(γ−y); clamp operands at 0 to keep √ finite on bad lanes.
  const __m256d gpy = _mm256_max_pd(_mm256_add_pd(gamma, y), zero);
  const __m256d gmy = _mm256_max_pd(_mm256_sub_pd(gamma, y), zero);
  const __m256d sqrt_v = _mm256_add_pd(_mm256_sqrt_pd(gpy), _mm256_sqrt_pd(gmy));
  const __m256d sigma = _mm256_div_pd(sqrt_v, _mm256_sqrt_pd(T));

  // Validity: β > 0, γ ≥ |y|, σ > 0 (each false on any NaN lane).
  const __m256d abs_y = _mm256_andnot_pd(_mm256_set1_pd(-0.0), y);
  const __m256d v1 = _mm256_cmp_pd(beta, zero, _CMP_GT_OQ);
  const __m256d v2 = _mm256_cmp_pd(gamma, abs_y, _CMP_GE_OQ);
  const __m256d v3 = _mm256_cmp_pd(sigma, zero, _CMP_GT_OQ);
  valid = _mm256_and_pd(v1, _mm256_and_pd(v2, v3));

  const __m256d s_min = _mm256_set1_pd(kIvMin);
  const __m256d s_max = _mm256_set1_pd(kIvMax);
  return _mm256_min_pd(_mm256_max_pd(sigma, s_min), s_max);
}

// One Halley (order-3) step on the *original* (price, side). Returns the updated
// σ (bounded + clamped exactly as the scalar). Pre-computed sqrtT / lnFK are
// shared across the two passes.
ATX_FORCE_INLINE __m256d iv_halley_step_pd(__m256d sigma, __m256d price, __m256d F, __m256d K,
                                           __m256d df, __m256d is_put, __m256d sqrtT, __m256d lnFK,
                                           const double *coefs) noexcept {
  const __m256d zero = _mm256_setzero_pd();
  const __m256d half = _mm256_set1_pd(0.5);
  const __m256d two = _mm256_set1_pd(2.0);

  const __m256d v = _mm256_mul_pd(sigma, sqrtT);
  const __m256d half_v2 = _mm256_mul_pd(half, _mm256_mul_pd(v, v));
  const __m256d d1 = _mm256_div_pd(_mm256_add_pd(lnFK, half_v2), v);
  const __m256d d2 = _mm256_sub_pd(d1, v);
  __m256d Nd1, Nd2;
  norm_cdf_pd2(d1, d2, coefs, Nd1, Nd2);
  const __m256d phi1 = norm_pdf_pd(d1);

  // Call: df·(F·Φ(d1) − K·Φ(d2)). Put = call + df·(K − F) (put-call parity).
  const __m256d call_pr =
      _mm256_mul_pd(df, _mm256_sub_pd(_mm256_mul_pd(F, Nd1), _mm256_mul_pd(K, Nd2)));
  const __m256d put_pr = _mm256_fmadd_pd(df, _mm256_sub_pd(K, F), call_pr);
  const __m256d price_model = _mm256_blendv_pd(call_pr, put_pr, is_put);
  const __m256d resid = _mm256_sub_pd(price_model, price);

  const __m256d vega = _mm256_mul_pd(_mm256_mul_pd(df, F), _mm256_mul_pd(phi1, sqrtT));
  const __m256d volga =
      _mm256_div_pd(_mm256_mul_pd(vega, _mm256_mul_pd(d1, d2)), sigma); // vega·d1·d2/σ

  // Halley: step = −2 f f' / (2 f'² − f f''). f=resid, f'=vega, f''=volga.
  const __m256d num = _mm256_mul_pd(two, _mm256_mul_pd(resid, vega));
  const __m256d two_v2 = _mm256_mul_pd(two, _mm256_mul_pd(vega, vega));
  const __m256d den = _mm256_fnmadd_pd(resid, volga, two_v2); // 2v² − resid·volga
  const __m256d step = _mm256_div_pd(_mm256_sub_pd(zero, num), den);

  // Multiplicative bound step ∈ [−σ/2, σ] (an inf/NaN step collapses to a
  // bounded value here, so σ_new stays finite), then clamp to [kIvMin, kIvMax].
  const __m256d s_lo = _mm256_sub_pd(zero, _mm256_mul_pd(half, sigma));
  const __m256d step_b = _mm256_min_pd(_mm256_max_pd(step, s_lo), sigma);
  const __m256d sigma_new = _mm256_add_pd(sigma, step_b);
  const __m256d s_min = _mm256_set1_pd(kIvMin);
  const __m256d s_max = _mm256_set1_pd(kIvMax);
  return _mm256_min_pd(_mm256_max_pd(sigma_new, s_min), s_max);
}

// Evaluate d1, d2, vega and the price residual at the final σ, for the accept /
// patch gate (the residual from the last Halley step is one σ stale).
ATX_FORCE_INLINE void iv_evaluate_pd(__m256d sigma, __m256d price, __m256d F, __m256d K, __m256d df,
                                     __m256d is_put, __m256d sqrtT, __m256d lnFK,
                                     const double *coefs, __m256d &d1o, __m256d &d2o,
                                     __m256d &vegao, __m256d &resido) noexcept {
  const __m256d half = _mm256_set1_pd(0.5);
  const __m256d v = _mm256_mul_pd(sigma, sqrtT);
  const __m256d half_v2 = _mm256_mul_pd(half, _mm256_mul_pd(v, v));
  const __m256d d1 = _mm256_div_pd(_mm256_add_pd(lnFK, half_v2), v);
  const __m256d d2 = _mm256_sub_pd(d1, v);
  __m256d Nd1, Nd2;
  norm_cdf_pd2(d1, d2, coefs, Nd1, Nd2);
  const __m256d phi1 = norm_pdf_pd(d1);
  const __m256d call_pr =
      _mm256_mul_pd(df, _mm256_sub_pd(_mm256_mul_pd(F, Nd1), _mm256_mul_pd(K, Nd2)));
  const __m256d put_pr = _mm256_fmadd_pd(df, _mm256_sub_pd(K, F), call_pr);
  const __m256d price_model = _mm256_blendv_pd(call_pr, put_pr, is_put);

  d1o = d1;
  d2o = d2;
  vegao = _mm256_mul_pd(_mm256_mul_pd(df, F), _mm256_mul_pd(phi1, sqrtT));
  resido = _mm256_sub_pd(price_model, price);
}

// Patch a single lane through the exact scalar inverter, writing (iv, ok).
ATX_FORCE_INLINE void patch_scalar(const double *price, const double *F, const double *K,
                                   const double *T, const double *df, const Side *side,
                                   double *iv_out, std::uint8_t *ok_out, std::size_t i) noexcept {
  const Result<double> r = implied_vol(price[i], F[i], K[i], T[i], df[i], side[i]);
  if (r) {
    iv_out[i] = *r;
    ok_out[i] = 1;
  } else {
    iv_out[i] = std::numeric_limits<double>::quiet_NaN();
    ok_out[i] = 0;
  }
}

ATX_FORCE_INLINE __m256d nonfinite_mask(__m256d value) noexcept {
  const __m256d abs_mask = _mm256_set1_pd(-0.0);
  const __m256d max_finite = _mm256_set1_pd(std::numeric_limits<double>::max());
  const __m256d magnitude = _mm256_andnot_pd(abs_mask, value);
  return _mm256_cmp_pd(magnitude, max_finite, _CMP_NLE_UQ);
}

} // namespace

void implied_vol_batch_avx2(const double *price, const double *F, const double *K, const double *T,
                            const double *df, const Side *side, double *iv_out,
                            std::uint8_t *ok_out, std::size_t n) noexcept {
  const double *coefs = norm_cdf_cheb_coefs().data();
  const __m256d zero = _mm256_setzero_pd();
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d all_ones = _mm256_cmp_pd(zero, zero, _CMP_EQ_OQ);
  const __m256d abs_mask = _mm256_set1_pd(-0.0);
  const __m256d wing = _mm256_set1_pd(kNormCdfWing);
  const __m256d resid_tol = _mm256_set1_pd(kIvResidTol);
  const __m256d vfloor_coef = _mm256_set1_pd(kIvVegaFloorCoef);

  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m256d Pv = _mm256_loadu_pd(price + i);
    const __m256d Fv = _mm256_loadu_pd(F + i);
    const __m256d Kv = _mm256_loadu_pd(K + i);
    const __m256d Tv = _mm256_loadu_pd(T + i);
    const __m256d dfv = _mm256_loadu_pd(df + i);

    alignas(32) double eps_buf[4];
    eps_buf[0] = (side[i + 0] != Side::Call) ? -1.0 : 1.0;
    eps_buf[1] = (side[i + 1] != Side::Call) ? -1.0 : 1.0;
    eps_buf[2] = (side[i + 2] != Side::Call) ? -1.0 : 1.0;
    eps_buf[3] = (side[i + 3] != Side::Call) ? -1.0 : 1.0;
    const __m256d eps = _mm256_load_pd(eps_buf);
    const __m256d is_put = _mm256_cmp_pd(eps, zero, _CMP_LT_OQ);

    // Degenerate inputs (T/F/K/df ≤ 0): force scalar patch. safeT keeps the
    // vector √T finite so a bad lane cannot spill NaN into the seed maths.
    __m256d bad = nonfinite_mask(Pv);
    bad = _mm256_or_pd(bad, nonfinite_mask(Fv));
    bad = _mm256_or_pd(bad, nonfinite_mask(Kv));
    bad = _mm256_or_pd(bad, nonfinite_mask(Tv));
    bad = _mm256_or_pd(bad, nonfinite_mask(dfv));
    bad = _mm256_or_pd(
        bad, _mm256_or_pd(_mm256_cmp_pd(Tv, zero, _CMP_LE_OQ),
                          _mm256_or_pd(_mm256_cmp_pd(Fv, zero, _CMP_LE_OQ),
                                       _mm256_or_pd(_mm256_cmp_pd(Kv, zero, _CMP_LE_OQ),
                                                    _mm256_cmp_pd(dfv, zero, _CMP_LE_OQ)))));
    const __m256d safeT = _mm256_blendv_pd(Tv, one, bad);

    __m256d seed_valid;
    __m256d sigma = sr2017_seed_pd(Pv, Fv, Kv, safeT, dfv, eps, seed_valid);

    const __m256d sqrtT = _mm256_sqrt_pd(safeT);
    const __m256d lnFK = log_pd(_mm256_div_pd(Fv, Kv));
    sigma = iv_halley_step_pd(sigma, Pv, Fv, Kv, dfv, is_put, sqrtT, lnFK, coefs);
    sigma = iv_halley_step_pd(sigma, Pv, Fv, Kv, dfv, is_put, sqrtT, lnFK, coefs);

    __m256d d1, d2, vega, resid;
    iv_evaluate_pd(sigma, Pv, Fv, Kv, dfv, is_put, sqrtT, lnFK, coefs, d1, d2, vega, resid);

    // Assemble the patch mask (true → this lane goes to scalar).
    const __m256d abs_d1 = _mm256_andnot_pd(abs_mask, d1);
    const __m256d abs_d2 = _mm256_andnot_pd(abs_mask, d2);
    const __m256d wing_bad = _mm256_or_pd(_mm256_cmp_pd(abs_d1, wing, _CMP_GT_OQ),
                                          _mm256_cmp_pd(abs_d2, wing, _CMP_GT_OQ));
    const __m256d abs_resid = _mm256_andnot_pd(abs_mask, resid);
    const __m256d not_conv = _mm256_cmp_pd(abs_resid, resid_tol, _CMP_GE_OQ);
    const __m256d vfloor = _mm256_mul_pd(_mm256_mul_pd(vfloor_coef, _mm256_add_pd(Fv, Kv)), dfv);
    const __m256d ill_cond = _mm256_cmp_pd(vega, vfloor, _CMP_LT_OQ);
    const __m256d not_valid = _mm256_andnot_pd(seed_valid, all_ones);
    const __m256d nan_out = _mm256_or_pd(_mm256_cmp_pd(sigma, sigma, _CMP_UNORD_Q),
                                         _mm256_cmp_pd(resid, resid, _CMP_UNORD_Q));

    __m256d patch = _mm256_or_pd(bad, not_valid);
    patch = _mm256_or_pd(patch, wing_bad);
    patch = _mm256_or_pd(patch, ill_cond);
    patch = _mm256_or_pd(patch, not_conv);
    patch = _mm256_or_pd(patch, nan_out);
    const int pbits = _mm256_movemask_pd(patch);

    alignas(32) double sb[4];
    _mm256_store_pd(sb, sigma);
    for (int j = 0; j < 4; ++j) {
      if (pbits & (1 << j)) {
        patch_scalar(price, F, K, T, df, side, iv_out, ok_out, i + j);
      } else {
        iv_out[i + j] = sb[j];
        ok_out[i + j] = 1;
      }
    }
  }
  for (; i < n; ++i) {
    patch_scalar(price, F, K, T, df, side, iv_out, ok_out, i);
  }
}

} // namespace atx::vol::simd::detail
