// AVX2 (4-lane f64) batched implied-volatility inversion.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 contracts;
// the n % 4 tail and any lane the vector path cannot invert to full accuracy
// fall through to the exact scalar atx::vol::implied_vol, which keeps the batch
// bit-for-bit with the scalar source of truth on those lanes.
//
// Algorithm per 4-lane pass (mirrors the scalar implied_vol):
//
//   1. Vectorized Choi-2023 L₃ closed-form seed (seed_choi_l3_pd — the same seed
//      the scalar path uses): one vector log + a few vector exp, branchless; a
//      validity mask flags lanes where the closed form degenerates.
//   2. Three Halley (order-3) steps on the (price, side): fused
//      norm_cdf_erfc_pd2 for Φ(d1),Φ(d2) — full-range Cody rational erfc — and
//      norm_pdf_pd for φ(d1). f = model-price, f' = vega = df·F·φ(d1)·√T,
//      f'' = volga = vega·d1·d2/σ; step bounded to [-σ/2, σ] then σ clamped to
//      [kIvMin, kIvMax] — identical to the scalar. The final step also returns
//      (vega, resid), so the accept gate needs no separate Φ/φ pass.
//   3. Per-lane patch to scalar for: degenerate inputs (T/F/K/df ≤ 0), an
//      invalid seed, a non-finite result, an ill-conditioned lane (vega below the
//      notional-scaled floor, so the price→σ map is too flat), or a non-converged
//      residual. There is NO wing patch: the Cody erfc Φ is full double precision
//      into the deep wings, so finite wing lanes stay on the vector path. Patched
//      lanes call implied_vol, matching scalar exactly; accepted lanes keep the
//      vector σ (within ~1e-9 of scalar).

#include "iv_batch_avx2.hpp"

#include "simd/vector_math.hpp"
#include "atx/vol/api/pricing/implied_vol.hpp"

#include <immintrin.h>

#include <limits>

namespace atx::vol::simd::detail {

// Pull in the shared 4-lane transcendentals (log_pd/exp_pd/norm_cdf_erfc_pd2/
// norm_pdf_pd) — all in atx::vol::detail.
using namespace atx::vol::detail;

namespace {

// Accept threshold for the FUSED probe residual (K3). The batch runs 3 Halley
// steps and folds the accept-gate Φ/φ evaluation into the LAST step, so the gate
// residual is evaluated at the σ BEFORE that step — one step stale (larger than
// the emitted σ's residual). A well-conditioned accepted lane (vega ≥ the ill-cond
// floor — low-vega / short-dated lanes are already routed to scalar) whose
// pre-step residual is below this is driven to machine precision by the final
// step, so the emitted σ tracks the scalar inverter far inside the 1e-8 parity
// bound the simd_iv suite asserts. A converged interior lane sits orders below it;
// a lane above it is genuinely non-converged (seed pathology / near-arb edge).
constexpr double kIvProbeResidTol = 1.0e-4;

// Conditioning gate. σ-parity to scalar ≈ (Φ price bias)/vega, with the price
// bias ≈ εΦ·(F+K)·df. K3: Φ is now the full-range Cody rational-erfc (εΦ ≈ 1e-16,
// machine), NOT the old degree-48 Chebyshev (εΦ ≈ few·1e-11), so requiring
// vega ≥ coef·(F+K)·df keeps σ-parity ≲ εΦ/coef ≈ 1e-16/coef — orders below the
// 1e-8 suite bound for any coef ≳ 1e-8. The floor's job is therefore now purely
// numerical-stability: guard the Halley step's 1/vega² against near-zero-vega
// (deep-wing / near-expiry) lanes, which patch to the exact scalar inverter. The
// old 0.05 (sized for the Chebyshev bias) needlessly patched entire short-dated
// ATM columns whose σ the vector path now recovers to machine precision (e.g.
// T = 1/365 ATM vega ≈ 0.021·F < 0.05·(F+K)·df). 0.005 keeps ~2e-14 σ-parity
// headroom while returning those lanes to the vector path (K3 patch-rate cut).
constexpr double kIvVegaFloorCoef = 0.005;

// Peter J. Acklam's rational inverse-normal CDF Φ⁻¹(p), 4-lane. Central rational
// body + both symmetric tails, selected branchlessly (all three computed, blended
// by p). Relative error < 1.15e-9 — far more than a seed needs. Vector sibling of
// implied_vol.cpp::norm_ppf. Primary source: P. J. Acklam, "An algorithm for
// computing the inverse normal cumulative distribution function" (2003).
ATX_FORCE_INLINE __m256d norm_ppf_pd(__m256d p) noexcept {
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d a0 = _mm256_set1_pd(-3.969683028665376e+01);
  const __m256d a1 = _mm256_set1_pd(2.209460984245205e+02);
  const __m256d a2 = _mm256_set1_pd(-2.759285104469687e+02);
  const __m256d a3 = _mm256_set1_pd(1.383577518672690e+02);
  const __m256d a4 = _mm256_set1_pd(-3.066479806614716e+01);
  const __m256d a5 = _mm256_set1_pd(2.506628277459239e+00);
  const __m256d b0 = _mm256_set1_pd(-5.447609879822406e+01);
  const __m256d b1 = _mm256_set1_pd(1.615858368580409e+02);
  const __m256d b2 = _mm256_set1_pd(-1.556989798598866e+02);
  const __m256d b3 = _mm256_set1_pd(6.680131188771972e+01);
  const __m256d b4 = _mm256_set1_pd(-1.328068155288572e+01);
  const __m256d c0 = _mm256_set1_pd(-7.784894002430293e-03);
  const __m256d c1 = _mm256_set1_pd(-3.223964580411365e-01);
  const __m256d c2 = _mm256_set1_pd(-2.400758277161838e+00);
  const __m256d c3 = _mm256_set1_pd(-2.549732539343734e+00);
  const __m256d c4 = _mm256_set1_pd(4.374664141464968e+00);
  const __m256d c5 = _mm256_set1_pd(2.938163982698783e+00);
  const __m256d d0 = _mm256_set1_pd(7.784695709041462e-03);
  const __m256d d1 = _mm256_set1_pd(3.224671290700398e-01);
  const __m256d d2 = _mm256_set1_pd(2.445134137142996e+00);
  const __m256d d3 = _mm256_set1_pd(3.754408661907416e+00);

  // Central body: q = p − 0.5, r = q².
  const __m256d q = _mm256_sub_pd(p, _mm256_set1_pd(0.5));
  const __m256d r = _mm256_mul_pd(q, q);
  __m256d cnum = a0;
  cnum = _mm256_fmadd_pd(cnum, r, a1);
  cnum = _mm256_fmadd_pd(cnum, r, a2);
  cnum = _mm256_fmadd_pd(cnum, r, a3);
  cnum = _mm256_fmadd_pd(cnum, r, a4);
  cnum = _mm256_fmadd_pd(cnum, r, a5);
  cnum = _mm256_mul_pd(cnum, q);
  __m256d cden = b0;
  cden = _mm256_fmadd_pd(cden, r, b1);
  cden = _mm256_fmadd_pd(cden, r, b2);
  cden = _mm256_fmadd_pd(cden, r, b3);
  cden = _mm256_fmadd_pd(cden, r, b4);
  cden = _mm256_fmadd_pd(cden, r, one);
  const __m256d x_central = _mm256_div_pd(cnum, cden);

  const __m256d neg2 = _mm256_set1_pd(-2.0);
  // Lower tail (p < 0.02425): ql = √(−2 ln p).
  const __m256d ql = _mm256_sqrt_pd(_mm256_mul_pd(neg2, log_pd(p)));
  __m256d lnum = c0;
  lnum = _mm256_fmadd_pd(lnum, ql, c1);
  lnum = _mm256_fmadd_pd(lnum, ql, c2);
  lnum = _mm256_fmadd_pd(lnum, ql, c3);
  lnum = _mm256_fmadd_pd(lnum, ql, c4);
  lnum = _mm256_fmadd_pd(lnum, ql, c5);
  __m256d lden = d0;
  lden = _mm256_fmadd_pd(lden, ql, d1);
  lden = _mm256_fmadd_pd(lden, ql, d2);
  lden = _mm256_fmadd_pd(lden, ql, d3);
  lden = _mm256_fmadd_pd(lden, ql, one);
  const __m256d x_lower = _mm256_div_pd(lnum, lden);

  // Upper tail (p > 0.97575): qu = √(−2 ln(1−p)); x = −(rational).
  const __m256d qu = _mm256_sqrt_pd(_mm256_mul_pd(neg2, log_pd(_mm256_sub_pd(one, p))));
  __m256d unum = c0;
  unum = _mm256_fmadd_pd(unum, qu, c1);
  unum = _mm256_fmadd_pd(unum, qu, c2);
  unum = _mm256_fmadd_pd(unum, qu, c3);
  unum = _mm256_fmadd_pd(unum, qu, c4);
  unum = _mm256_fmadd_pd(unum, qu, c5);
  __m256d uden = d0;
  uden = _mm256_fmadd_pd(uden, qu, d1);
  uden = _mm256_fmadd_pd(uden, qu, d2);
  uden = _mm256_fmadd_pd(uden, qu, d3);
  uden = _mm256_fmadd_pd(uden, qu, one);
  const __m256d x_upper = _mm256_sub_pd(_mm256_setzero_pd(), _mm256_div_pd(unum, uden));

  __m256d x = x_central;
  x = _mm256_blendv_pd(x, x_lower, _mm256_cmp_pd(p, _mm256_set1_pd(0.02425), _CMP_LT_OQ));
  x = _mm256_blendv_pd(x, x_upper, _mm256_cmp_pd(p, _mm256_set1_pd(0.97575), _CMP_GT_OQ));
  return x;
}

// Vectorized Choi-Kim-Kwak (2023) tighter LOWER bound L₃ IV seed (arXiv:2302.08758,
// Cor. 5.2) — the vector sibling of implied_vol.cpp::seed_choi_l3, and the exact
// same seed the scalar hot path now uses (K2). Exact ATM, tight in the wings, so
// the Halley loop starts in the cubic basin. CHEAP: reuses the block's lnFK,
// gets eᵏ = max/min by DIVISION (no libm exp — the old SR seed spent 4 vector
// exp + 2 vector log here), so its only transcendental is the one rational-plus-
// log Acklam probit. `valid` is set where the standardized time value c > 0 and
// σ > 0; degenerate lanes fall through to the scalar patch (Choi→SR corner seed).
ATX_FORCE_INLINE __m256d seed_choi_l3_pd(__m256d price, __m256d F, __m256d K, __m256d df,
                                         __m256d is_put, __m256d lnFK, __m256d sqrtT,
                                         __m256d &valid) noexcept {
  const __m256d zero = _mm256_setzero_pd();
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d m = _mm256_min_pd(F, K);
  const __m256d fwd_price = _mm256_div_pd(price, df);
  const __m256d intr_call = _mm256_max_pd(_mm256_sub_pd(F, K), zero);
  const __m256d intr_put = _mm256_max_pd(_mm256_sub_pd(K, F), zero);
  const __m256d intr = _mm256_blendv_pd(intr_call, intr_put, is_put);
  const __m256d c = _mm256_div_pd(_mm256_sub_pd(fwd_price, intr), m);

  const __m256d k = _mm256_andnot_pd(_mm256_set1_pd(-0.0), lnFK); // |ln(F/K)|
  const __m256d ek = _mm256_div_pd(_mm256_max_pd(F, K), m);       // eᵏ = max/min, no exp
  const __m256d argnum = _mm256_mul_pd(c, _mm256_add_pd(c, ek));
  const __m256d argden = _mm256_sub_pd(_mm256_add_pd(_mm256_add_pd(c, c), ek), one);
  __m256d arg = _mm256_div_pd(argnum, argden);
  // Φ⁻¹ needs an argument in (0,1); clamp the degenerate edges.
  arg = _mm256_min_pd(_mm256_max_pd(arg, _mm256_set1_pd(1.0e-300)),
                      _mm256_set1_pd(1.0 - 1.0e-16));
  const __m256d x = norm_ppf_pd(arg);
  // s = d₁⁻¹(x) = x + √(x² + 2k) = total vol σ·√T.
  const __m256d s = _mm256_add_pd(x, _mm256_sqrt_pd(_mm256_fmadd_pd(x, x, _mm256_add_pd(k, k))));
  const __m256d sigma = _mm256_div_pd(s, sqrtT);

  const __m256d vc = _mm256_cmp_pd(c, zero, _CMP_GT_OQ);
  const __m256d vs = _mm256_cmp_pd(sigma, zero, _CMP_GT_OQ);
  valid = _mm256_and_pd(vc, vs);

  const __m256d s_min = _mm256_set1_pd(kIvMin);
  const __m256d s_max = _mm256_set1_pd(kIvMax);
  return _mm256_min_pd(_mm256_max_pd(sigma, s_min), s_max);
}

// One Halley (order-3) step on the *original* (price, side). Returns the updated
// σ (bounded + clamped exactly as the scalar). Pre-computed sqrtT / lnFK are
// shared across the passes.
ATX_FORCE_INLINE __m256d iv_halley_step_pd(__m256d sigma, __m256d price, __m256d F, __m256d K,
                                           __m256d df, __m256d is_put, __m256d sqrtT,
                                           __m256d lnFK) noexcept {
  const __m256d zero = _mm256_setzero_pd();
  const __m256d half = _mm256_set1_pd(0.5);
  const __m256d two = _mm256_set1_pd(2.0);

  const __m256d v = _mm256_mul_pd(sigma, sqrtT);
  const __m256d half_v2 = _mm256_mul_pd(half, _mm256_mul_pd(v, v));
  const __m256d d1 = _mm256_div_pd(_mm256_add_pd(lnFK, half_v2), v);
  const __m256d d2 = _mm256_sub_pd(d1, v);
  __m256d Nd1, Nd2;
  // K2 (accuracy-improving): full-range Cody rational-erfc Φ replaces the
  // degree-48 Chebyshev–Clenshaw. The lane accept/patch gate below is unchanged.
  norm_cdf_erfc_pd2(d1, d2, Nd1, Nd2);
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

// Combined FINAL Halley step + accept-gate probe (K3). Evaluates Φ/φ ONCE at the
// input σ and returns BOTH the stepped σ (one further Halley refinement) AND the
// vega + price residual AT THE INPUT σ for the accept/patch gate. Folding the gate
// evaluation into the last step removes a whole redundant Φ/φ evaluation pass —
// the batch's dominant cost is these erfc/exp passes, so 4 passes (3 steps + a
// separate evaluate) drop to 3. The returned residual therefore bounds the
// PRE-step σ; the caller's accept threshold is loosened accordingly
// (kIvProbeResidTol): a well-conditioned lane (vega ≥ the ill-cond floor, so the
// risky low-vega / short-dated lanes are already routed to scalar) whose pre-step
// residual is under it is driven to machine precision by this step, so the emitted
// post-step σ tracks scalar far inside the 1e-8 parity bound.
ATX_FORCE_INLINE __m256d iv_halley_step_probe_pd(__m256d sigma, __m256d price, __m256d F, __m256d K,
                                                 __m256d df, __m256d is_put, __m256d sqrtT,
                                                 __m256d lnFK, __m256d &vegao,
                                                 __m256d &resido) noexcept {
  const __m256d zero = _mm256_setzero_pd();
  const __m256d half = _mm256_set1_pd(0.5);
  const __m256d two = _mm256_set1_pd(2.0);
  const __m256d v = _mm256_mul_pd(sigma, sqrtT);
  const __m256d half_v2 = _mm256_mul_pd(half, _mm256_mul_pd(v, v));
  const __m256d d1 = _mm256_div_pd(_mm256_add_pd(lnFK, half_v2), v);
  const __m256d d2 = _mm256_sub_pd(d1, v);
  __m256d Nd1, Nd2;
  norm_cdf_erfc_pd2(d1, d2, Nd1, Nd2); // full-range Cody erfc Φ (accurate in the wings)
  const __m256d phi1 = norm_pdf_pd(d1);
  const __m256d call_pr =
      _mm256_mul_pd(df, _mm256_sub_pd(_mm256_mul_pd(F, Nd1), _mm256_mul_pd(K, Nd2)));
  const __m256d put_pr = _mm256_fmadd_pd(df, _mm256_sub_pd(K, F), call_pr);
  const __m256d price_model = _mm256_blendv_pd(call_pr, put_pr, is_put);
  const __m256d resid = _mm256_sub_pd(price_model, price);
  const __m256d vega = _mm256_mul_pd(_mm256_mul_pd(df, F), _mm256_mul_pd(phi1, sqrtT));
  const __m256d volga =
      _mm256_div_pd(_mm256_mul_pd(vega, _mm256_mul_pd(d1, d2)), sigma); // vega·d1·d2/σ
  vegao = vega;
  resido = resid;

  // Halley step (bound + clamp identical to iv_halley_step_pd).
  const __m256d num = _mm256_mul_pd(two, _mm256_mul_pd(resid, vega));
  const __m256d two_v2 = _mm256_mul_pd(two, _mm256_mul_pd(vega, vega));
  const __m256d den = _mm256_fnmadd_pd(resid, volga, two_v2); // 2v² − resid·volga
  const __m256d step = _mm256_div_pd(_mm256_sub_pd(zero, num), den);
  const __m256d s_lo = _mm256_sub_pd(zero, _mm256_mul_pd(half, sigma));
  const __m256d step_b = _mm256_min_pd(_mm256_max_pd(step, s_lo), sigma);
  const __m256d sigma_new = _mm256_add_pd(sigma, step_b);
  const __m256d s_min = _mm256_set1_pd(kIvMin);
  const __m256d s_max = _mm256_set1_pd(kIvMax);
  return _mm256_min_pd(_mm256_max_pd(sigma_new, s_min), s_max);
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
  const __m256d zero = _mm256_setzero_pd();
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d all_ones = _mm256_cmp_pd(zero, zero, _CMP_EQ_OQ);
  const __m256d abs_mask = _mm256_set1_pd(-0.0);
  const __m256d resid_tol = _mm256_set1_pd(kIvProbeResidTol);
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
    // ONE convention for the log argument across the three batch kernels: the PATCHED
    // copies, as black76_batch_avx2 already does. safeF/safeK equal Fv/Kv on every lane
    // that is not already in `bad`, so every SERVED lane is bit-identical; the change is
    // that a degenerate lane can no longer feed log_pd a 0 or a negative and have the
    // decode hand back finite garbage that the later masks reason about.
    const __m256d safeF = _mm256_blendv_pd(Fv, one, bad);
    const __m256d safeK = _mm256_blendv_pd(Kv, one, bad);

    const __m256d sqrtT = _mm256_sqrt_pd(safeT);
    const __m256d lnFK = log_pd(_mm256_div_pd(safeF, safeK));

    // K3: the cheap Choi-2023 L₃ seed (same as the scalar K2 seed) starts inside
    // the Halley cubic basin, so THREE Halley steps drive the lanes to machine
    // precision on-vector — no |d| wing patch needed (the evaluate uses full-range
    // Cody erfc Φ, accurate in the deep wings), so wing lanes now stay vector
    // instead of paying a scalar detour. The 3rd step is what lets the tighter
    // seed reach the accept residual on the harder wing/deep lanes.
    __m256d seed_valid;
    __m256d sigma = seed_choi_l3_pd(Pv, Fv, Kv, dfv, is_put, lnFK, sqrtT, seed_valid);
    sigma = iv_halley_step_pd(sigma, Pv, Fv, Kv, dfv, is_put, sqrtT, lnFK);
    sigma = iv_halley_step_pd(sigma, Pv, Fv, Kv, dfv, is_put, sqrtT, lnFK);
    // Final step fuses the accept-gate evaluation: it returns σ₃ AND (vega, resid)
    // at σ₂, so no separate 4th Φ/φ pass is needed (K3 — the batch's dominant cost).
    __m256d vega, resid;
    sigma = iv_halley_step_probe_pd(sigma, Pv, Fv, Kv, dfv, is_put, sqrtT, lnFK, vega, resid);

    // Assemble the patch mask (true → this lane goes to scalar). Genuinely
    // ill-conditioned (tiny vega) or non-converged lanes still patch to the exact
    // scalar inverter for full parity; the wing patch is retired (Cody erfc Φ). The
    // residual gate uses the (one-step-stale) probe residual, so its threshold is
    // kIvProbeResidTol; the emitted σ has had one more Halley step past it.
    const __m256d abs_resid = _mm256_andnot_pd(abs_mask, resid);
    const __m256d not_conv = _mm256_cmp_pd(abs_resid, resid_tol, _CMP_GE_OQ);
    const __m256d vfloor = _mm256_mul_pd(_mm256_mul_pd(vfloor_coef, _mm256_add_pd(Fv, Kv)), dfv);
    // UNORDERED-true compare: an ORDERED `vega < vfloor` reads a NaN vega as
    // well-conditioned and declines to patch the lane. _CMP_NGE_UQ is identical to
    // _CMP_LT_OQ for every ordered pair and true when either side is NaN.
    const __m256d ill_cond = _mm256_cmp_pd(vega, vfloor, _CMP_NGE_UQ);
    // The |ln(F/K)| >= 708 escape both sibling kernels carry and both document as
    // required (black76_batch_avx2.cpp:104, greeks_batch_avx2.cpp:110). log_pd assumes a
    // positive NORMAL argument, so an F/K ratio that underflows to a denormal/zero or
    // overflows to +inf decodes to FINITE garbage near ±709 — which nonfinite_mask
    // cannot see — and that garbage lnFK feeds d1 in all four Halley steps.
    //
    // The escape is REDUNDANT here — the ill-conditioning floor already patches every
    // lane in this band — but not by accident, and the proof needs BOTH signs of lnFK,
    // by two DIFFERENT arguments. (An earlier version of this comment used the AM-GM
    // argument for both signs. It is false for lnFK < 0: at v = sqrt(2·708) ≈ 37.63,
    // d1 = lnFK/v + v/2 = 0 EXACTLY and φ(d1) = 0.3989, not ≤ 1e-308.)
    //
    //   lnFK >= +708 (F/K overflowed): AM-GM is valid because both terms are positive,
    //     so d1 = lnFK/v + v/2 >= sqrt(2·708) = 37.63 for ANY v > 0, giving
    //     φ(d1) <= 1.32e-308 and vega/vfloor <= φ(d1)·√T/0.005 <= 2.64e-306·√T.
    //   lnFK <= -708 (F/K underflowed to a denormal/zero — the reachable sign): AM-GM
    //     gives nothing, but the NOTIONAL scaling does. vfloor is 0.005·(F+K)·df, which
    //     grows with max(F, K), while vega grows with F alone; and lnFK <= -708 means
    //     F/K <= e^-708 = 3.31e-308. So
    //     vega/vfloor <= (0.3989/0.005)·√T·F/(F+K) <= 79.79·√T·3.31e-308.
    //
    // Both bounds are under 1 for any T this library will ever see, so the lane is
    // already ill-conditioned. The escape is kept so the three kernels state the same
    // precondition in the same place, not because a served number moves.
    const __m256d abs_lnfk = _mm256_andnot_pd(abs_mask, lnFK);
    const __m256d lnfk_escape = _mm256_cmp_pd(abs_lnfk, _mm256_set1_pd(708.0), _CMP_GE_OQ);
    const __m256d not_valid = _mm256_andnot_pd(seed_valid, all_ones);
    const __m256d nan_out = _mm256_or_pd(_mm256_cmp_pd(sigma, sigma, _CMP_UNORD_Q),
                                         _mm256_cmp_pd(resid, resid, _CMP_UNORD_Q));

    __m256d patch = _mm256_or_pd(bad, not_valid);
    patch = _mm256_or_pd(patch, ill_cond);
    patch = _mm256_or_pd(patch, lnfk_escape);
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
