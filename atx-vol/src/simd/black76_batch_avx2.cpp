// AVX2 (4-lane f64) batched Black-76 pricer + fused vega.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 contracts;
// the n % 4 tail and any degenerate lanes fall through to the exact scalar
// kernels in atx/vol/black76.hpp. Φ is the full-range Cody rational-erfc form
// (detail/vector_math.hpp), machine-accurate on the entire real line, so no |d|
// wing patch is needed (K2). The PRICE kernel computes the put leg from
// Φ(−d1),Φ(−d2) directly, matching the scalar black76_price and tracking it to
// ≈1e-16 RELATIVE across the whole line, deep wings included (A8). The value+vega
// kernel deliberately keeps the 1−Φ(d) complement for the put leg to stay
// bit-consistent with its scalar ref black76_value_and_vega — absolute-accurate
// everywhere but, like that ref, only absolute (not relative) accurate deep in the
// put wing where 1−Φ(d) cancels.
//
// Per 4-lane pass:
//   v    = σ·√T,  d1 = (ln(F/K) + ½v²)/v,  d2 = d1 - v
//   Φ(·) = ½·erfc(−·/√2), Cody rational erfc (detail/vector_math.hpp)
//   C    = df·(F·Φ(d1) - K·Φ(d2))
//   P    = df·(K·Φ(−d2) - F·Φ(−d1))       [price kernel]
//        = df·(K·(1-Φ(d2)) - F·(1-Φ(d1))) [value+vega kernel, complement — see above]
//   vega = F·df·φ(d1)·√T                       (same for calls and puts)
// Degenerate lanes (T ≤ 0 or σ ≤ 0) get dummy finite inputs to stay branchless,
// then are patched, as are the rare NaN-d lanes (R-22).

#include "black76_batch_avx2.hpp"

#include "atx/vol/black76.hpp"
#include "atx/vol/detail/vector_math.hpp"

#include <cmath>
#include <immintrin.h>
#include <limits>

namespace atx::vol::simd::detail {

// Pull in the shared 4-lane transcendentals (log_pd/exp_pd/norm_cdf_erfc_pd2/
// norm_pdf_pd), which live in atx::vol::detail.
using namespace atx::vol::detail;

namespace {

// Per-lane sign-bit mask for blendv: Put → -1.0 (select put), Call → 0.0.
ATX_FORCE_INLINE __m256d side_blend_mask(const Side *side, std::size_t i) noexcept {
  alignas(32) double s[4];
  s[0] = (side[i + 0] != Side::Call) ? -1.0 : 0.0;
  s[1] = (side[i + 1] != Side::Call) ? -1.0 : 0.0;
  s[2] = (side[i + 2] != Side::Call) ? -1.0 : 0.0;
  s[3] = (side[i + 3] != Side::Call) ? -1.0 : 0.0;
  return _mm256_load_pd(s);
}

ATX_FORCE_INLINE __m256d nonfinite_mask(__m256d value) noexcept {
  const __m256d abs_mask = _mm256_set1_pd(-0.0);
  const __m256d max_finite = _mm256_set1_pd(std::numeric_limits<double>::max());
  const __m256d magnitude = _mm256_andnot_pd(abs_mask, value);
  return _mm256_cmp_pd(magnitude, max_finite, _CMP_NLE_UQ);
}

ATX_FORCE_INLINE __m256d input_patch_mask(__m256d F, __m256d K, __m256d T, __m256d sigma,
                                          __m256d df, __m256d zero) noexcept {
  __m256d patch = nonfinite_mask(F);
  patch = _mm256_or_pd(patch, nonfinite_mask(K));
  patch = _mm256_or_pd(patch, nonfinite_mask(T));
  patch = _mm256_or_pd(patch, nonfinite_mask(sigma));
  patch = _mm256_or_pd(patch, nonfinite_mask(df));
  patch = _mm256_or_pd(patch, _mm256_cmp_pd(F, zero, _CMP_LE_OQ));
  patch = _mm256_or_pd(patch, _mm256_cmp_pd(K, zero, _CMP_LE_OQ));
  patch = _mm256_or_pd(patch, _mm256_cmp_pd(T, zero, _CMP_LE_OQ));
  patch = _mm256_or_pd(patch, _mm256_cmp_pd(sigma, zero, _CMP_LE_OQ));
  return _mm256_or_pd(patch, _mm256_cmp_pd(df, zero, _CMP_LE_OQ));
}

// Lanes needing the exact scalar path: degenerate (T ≤ 0 or σ ≤ 0) OR a
// NON-FINITE d (±inf or NaN). Returns a 4-bit movemask.
//
// K2 wing-patch removal (accuracy-improving): the deep-wing |d| > kNormCdfWing
// escape for FINITE d is GONE. It existed only because the degree-48
// Chebyshev–Clenshaw Φ lost accuracy past |d| ≈ 6 and the F·Φ(d1)−K·Φ(d2)
// cancellation amplified that absolute error. The Cody rational-erfc Φ
// (norm_cdf_erfc_pd2) that now feeds this kernel is full double precision across
// the ENTIRE real line — correct denormal wings included, and exp_pd flushes its
// deep-underflow tail to 0 — so a finite deep-wing lane prices on the vector path
// to ≈1e-16 instead of detouring through scalar. tests/simd_norm_cdf_erfc_test
// gates the Φ accuracy and tests/simd_greeks_test the wing Greeks.
//
// Non-finite d (retained, correctness — NOT accuracy): d can be NaN (R-22: FINITE
// F/K under/overflow with a σ²T overflow → ±inf cancellation) or ±inf (log_pd of
// an under/overflowed F/K plus a ½v² overflow). The retired ORDERED wing compares
// caught the ±inf case but not NaN; an unordered self-compare caught NaN but not
// ±inf. `nonfinite_mask` (magnitude > DBL_MAX, unordered-true) catches BOTH in one
// test, routing such a lane to the scalar source of truth for an exact match —
// independent of Φ accuracy, so it stays now the finite-wing patch is gone.
//
// A9 (simd-review finding 4): log_pd assumes a positive-NORMAL argument, so an F/K
// ratio that underflows to a denormal/0 or overflows to +inf decodes to FINITE
// garbage near ±709 (log of the min/max normal) instead of ±inf — which nonfinite_
// mask(d) then cannot catch. `|lnFK| >= 708` brackets exactly that garbage band (and
// any genuine deep wing, which is Φ-saturated and priced exactly by scalar anyway),
// routing the lane to the scalar source of truth.
ATX_FORCE_INLINE int patch_bits(__m256d degen, __m256d lnfk, __m256d d1, __m256d d2) noexcept {
  const __m256d abs_mask = _mm256_set1_pd(-0.0);
  const __m256d abs_lnfk = _mm256_andnot_pd(abs_mask, lnfk);
  const __m256d lnfk_escape = _mm256_cmp_pd(abs_lnfk, _mm256_set1_pd(708.0), _CMP_GE_OQ);
  const __m256d nonfinite_d = _mm256_or_pd(nonfinite_mask(d1), nonfinite_mask(d2));
  return _mm256_movemask_pd(_mm256_or_pd(degen, _mm256_or_pd(lnfk_escape, nonfinite_d)));
}

struct PerLaneExpiry {
  const double *values;

  [[nodiscard]] ATX_FORCE_INLINE __m256d load(std::size_t i) const noexcept {
    return _mm256_loadu_pd(values + i);
  }
  [[nodiscard]] ATX_FORCE_INLINE __m256d root(__m256d safe_t) const noexcept {
    return _mm256_sqrt_pd(safe_t);
  }
  [[nodiscard]] Black76ValueVega scalar(const double *F, const double *K, const double *sigma,
                                        const double *df, const Side *side,
                                        std::size_t i) const noexcept {
    return black76_value_and_vega(F[i], K[i], values[i], sigma[i], df[i], side[i]);
  }
};

struct SharedExpiry {
  double value;
  double root_in;
  double root_value;

  [[nodiscard]] ATX_FORCE_INLINE __m256d load(std::size_t) const noexcept {
    return _mm256_set1_pd(value);
  }
  [[nodiscard]] ATX_FORCE_INLINE __m256d root(__m256d) const noexcept {
    return _mm256_set1_pd(root_value);
  }
  [[nodiscard]] Black76ValueVega scalar(const double *F, const double *K, const double *sigma,
                                        const double *df, const Side *side,
                                        std::size_t i) const noexcept {
    return black76_value_and_vega(F[i], K[i], value, sigma[i], df[i], side[i], root_in);
  }
};

template <typename Expiry>
void value_vega_batch_avx2_impl(const double *F, const double *K, const double *sigma,
                                const double *df, const Side *side, double *price_out,
                                double *vega_out, std::size_t n, const Expiry &expiry) noexcept {
  const __m256d half = _mm256_set1_pd(0.5);
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d zero = _mm256_setzero_pd();

  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    const __m256d Fv = _mm256_loadu_pd(F + i);
    const __m256d Kv = _mm256_loadu_pd(K + i);
    const __m256d Tv = expiry.load(i);
    const __m256d sv = _mm256_loadu_pd(sigma + i);
    const __m256d dfv = _mm256_loadu_pd(df + i);

    const __m256d input_patch = input_patch_mask(Fv, Kv, Tv, sv, dfv, zero);
    const __m256d safe_f = _mm256_blendv_pd(Fv, one, input_patch);
    const __m256d safe_k = _mm256_blendv_pd(Kv, one, input_patch);
    const __m256d safe_t = _mm256_blendv_pd(Tv, one, input_patch);
    const __m256d safe_s = _mm256_blendv_pd(sv, one, input_patch);
    const __m256d safe_df = _mm256_blendv_pd(dfv, one, input_patch);
    const __m256d sqrt_t = expiry.root(safe_t);
    const __m256d v = _mm256_mul_pd(safe_s, sqrt_t);
    const __m256d ln_fk = log_pd(_mm256_div_pd(safe_f, safe_k));
    const __m256d d1 =
        _mm256_div_pd(_mm256_add_pd(ln_fk, _mm256_mul_pd(half, _mm256_mul_pd(v, v))), v);
    const __m256d d2 = _mm256_sub_pd(d1, v);

    __m256d nd1, nd2;
    norm_cdf_erfc_pd2(d1, d2, nd1, nd2); // K2: full-range Cody erfc Φ (see price batch)
    const __m256d call = _mm256_mul_pd(
        safe_df, _mm256_sub_pd(_mm256_mul_pd(safe_f, nd1), _mm256_mul_pd(safe_k, nd2)));
    // A8 (simd-review finding 1): this value+vega kernel deliberately KEEPS the
    // 1−Φ(d) complement for the put leg (unlike the price kernel above, which uses
    // Φ(−d)) to stay bit-consistent with its scalar ref black76_value_and_vega,
    // which also uses the complement — so batch and scalar agree EXACTLY (both 0.0)
    // deep in the put wing rather than one being accurate and one not.
    const __m256d put =
        _mm256_mul_pd(safe_df, _mm256_sub_pd(_mm256_mul_pd(safe_k, _mm256_sub_pd(one, nd2)),
                                             _mm256_mul_pd(safe_f, _mm256_sub_pd(one, nd1))));
    __m256d price = _mm256_blendv_pd(call, put, side_blend_mask(side, i));
    __m256d vega =
        _mm256_mul_pd(_mm256_mul_pd(_mm256_mul_pd(safe_f, safe_df), norm_pdf_pd(d1)), sqrt_t);

    const int patch = patch_bits(input_patch, ln_fk, d1, d2);
    if (patch != 0) {
      alignas(32) double price_buffer[4];
      alignas(32) double vega_buffer[4];
      _mm256_store_pd(price_buffer, price);
      _mm256_store_pd(vega_buffer, vega);
      for (int lane = 0; lane < 4; ++lane) {
        if (patch & (1 << lane)) {
          const std::size_t index = i + static_cast<std::size_t>(lane);
          const Black76ValueVega scalar = expiry.scalar(F, K, sigma, df, side, index);
          price_buffer[lane] = scalar.price;
          vega_buffer[lane] = scalar.vega;
        }
      }
      price = _mm256_load_pd(price_buffer);
      vega = _mm256_load_pd(vega_buffer);
    }
    _mm256_storeu_pd(price_out + i, price);
    _mm256_storeu_pd(vega_out + i, vega);
  }
  for (; i < n; ++i) {
    const Black76ValueVega scalar = expiry.scalar(F, K, sigma, df, side, i);
    price_out[i] = scalar.price;
    vega_out[i] = scalar.vega;
  }
}

} // namespace

void black76_price_batch_avx2(const double *F, const double *K, const double *T,
                              const double *sigma, const double *df, const Side *side,
                              double *price_out, std::size_t n) noexcept {
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

    const __m256d input_patch = input_patch_mask(Fv, Kv, Tv, sv, dfv, zero);
    const __m256d safeF = _mm256_blendv_pd(Fv, one, input_patch);
    const __m256d safeK = _mm256_blendv_pd(Kv, one, input_patch);
    const __m256d safeT = _mm256_blendv_pd(Tv, one, input_patch);
    const __m256d safeS = _mm256_blendv_pd(sv, one, input_patch);
    const __m256d safeDf = _mm256_blendv_pd(dfv, one, input_patch);

    const __m256d v = _mm256_mul_pd(safeS, _mm256_sqrt_pd(safeT));
    const __m256d lnFK = log_pd(_mm256_div_pd(safeF, safeK));
    const __m256d d1 =
        _mm256_div_pd(_mm256_add_pd(lnFK, _mm256_mul_pd(half, _mm256_mul_pd(v, v))), v);
    const __m256d d2 = _mm256_sub_pd(d1, v);

    __m256d Nd1, Nd2;
    // K2 (accuracy-improving): full-range Cody rational-erfc Φ (≈1e-16, correct
    // wings) replaces the degree-48 Chebyshev–Clenshaw (~1e-11, |d|≤7 only), so
    // the deep-wing |d| escape is gone — only degenerate and NaN-d lanes patch to
    // scalar now (see patch_bits). Deep-wing lanes price on this vector path.
    norm_cdf_erfc_pd2(d1, d2, Nd1, Nd2);

    // A8 (simd-review finding 1): compute the put leg from Φ(−d1),Φ(−d2) DIRECTLY
    // — negate the args, the Cody erfc kernel is symmetric and accurate for
    // negatives — matching the scalar black76_price (which uses Φ(−d)). The
    // 1−Φ(d) complement suffers catastrophic cancellation deep in the put wing
    // (d ≫ 0 ⇒ Φ(d) rounds to exactly 1.0 ⇒ 1−Φ(d) = 0.0), zeroing a genuine
    // ~1e-26 premium; Φ(−d) resolves it to full RELATIVE precision. The call leg
    // keeps Φ(d1),Φ(d2) (no cancellation there).
    __m256d Nm1, Nm2;
    norm_cdf_erfc_pd2(_mm256_sub_pd(zero, d1), _mm256_sub_pd(zero, d2), Nm1, Nm2);

    const __m256d call =
        _mm256_mul_pd(safeDf, _mm256_sub_pd(_mm256_mul_pd(safeF, Nd1), _mm256_mul_pd(safeK, Nd2)));
    const __m256d put =
        _mm256_mul_pd(safeDf, _mm256_sub_pd(_mm256_mul_pd(safeK, Nm2), _mm256_mul_pd(safeF, Nm1)));
    __m256d price = _mm256_blendv_pd(call, put, side_blend_mask(side, i));

    const int patch = patch_bits(input_patch, lnFK, d1, d2);
    if (patch != 0) {
      alignas(32) double pb[4];
      _mm256_store_pd(pb, price);
      for (int j = 0; j < 4; ++j) {
        if (patch & (1 << j)) {
          pb[j] = black76_price(F[i + j], K[i + j], T[i + j], sigma[i + j], df[i + j], side[i + j]);
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

void black76_value_vega_batch_avx2(const double *F, const double *K, const double *T,
                                   const double *sigma, const double *df, const Side *side,
                                   double *price_out, double *vega_out, std::size_t n) noexcept {
  value_vega_batch_avx2_impl(F, K, sigma, df, side, price_out, vega_out, n, PerLaneExpiry{T});
}

void black76_value_vega_shared_t_batch_avx2(const double *F, const double *K, double T,
                                            double sqrt_t_in, const double *sigma, const double *df,
                                            const Side *side, double *price_out, double *vega_out,
                                            std::size_t n) noexcept {
  const double root = (sqrt_t_in >= 0.0) ? sqrt_t_in : std::sqrt(T);
  if (sqrt_t_in == 0.0 || !std::isfinite(root)) {
    for (std::size_t i = 0; i < n; ++i) {
      const Black76ValueVega scalar =
          black76_value_and_vega(F[i], K[i], T, sigma[i], df[i], side[i], sqrt_t_in);
      price_out[i] = scalar.price;
      vega_out[i] = scalar.vega;
    }
    return;
  }
  value_vega_batch_avx2_impl(F, K, sigma, df, side, price_out, vega_out, n,
                             SharedExpiry{T, sqrt_t_in, root});
}

} // namespace atx::vol::simd::detail
