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

#include <cmath>
#include <immintrin.h>
#include <limits>

namespace atx::vol::simd::detail {

// Pull in the shared 4-lane transcendentals (log_pd/exp_pd/norm_cdf_pd/
// norm_pdf_pd) and Chebyshev table, which live in atx::vol::detail.
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

// Lanes needing the exact scalar path: degenerate (T ≤ 0 or σ ≤ 0), deep-wing
// (either d exceeds the accurate interior), OR a non-finite d. Returns a 4-bit
// movemask. R-22: the wing compares are ORDERED (false for NaN), so a NaN d
// produced by finite inputs (F/K under/overflow with a σ²T overflow → ±inf
// cancellation) escaped them. An unordered self-compare (NaN iff x != x) routes
// such a lane to the scalar kernel, which returns NaN, matching it exactly. (K2's
// erfc Φ also propagates NaN rather than clamping it, so the two agree either
// way; this makes the routing explicit and robust to future Φ changes.)
ATX_FORCE_INLINE int patch_bits(__m256d degen, __m256d d1, __m256d d2) noexcept {
  const __m256d w = _mm256_set1_pd(kNormCdfWing);
  const __m256d nw = _mm256_set1_pd(-kNormCdfWing);
  const __m256d wing = _mm256_or_pd(
      _mm256_or_pd(_mm256_cmp_pd(d1, w, _CMP_GT_OQ), _mm256_cmp_pd(d1, nw, _CMP_LT_OQ)),
      _mm256_or_pd(_mm256_cmp_pd(d2, w, _CMP_GT_OQ), _mm256_cmp_pd(d2, nw, _CMP_LT_OQ)));
  const __m256d nan_d = _mm256_or_pd(_mm256_cmp_pd(d1, d1, _CMP_UNORD_Q),
                                     _mm256_cmp_pd(d2, d2, _CMP_UNORD_Q));
  return _mm256_movemask_pd(_mm256_or_pd(_mm256_or_pd(degen, wing), nan_d));
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
    const __m256d put =
        _mm256_mul_pd(safe_df, _mm256_sub_pd(_mm256_mul_pd(safe_k, _mm256_sub_pd(one, nd2)),
                                             _mm256_mul_pd(safe_f, _mm256_sub_pd(one, nd1))));
    __m256d price = _mm256_blendv_pd(call, put, side_blend_mask(side, i));
    __m256d vega =
        _mm256_mul_pd(_mm256_mul_pd(_mm256_mul_pd(safe_f, safe_df), norm_pdf_pd(d1)), sqrt_t);

    const int patch = patch_bits(input_patch, d1, d2);
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
    // wings) replaces the degree-48 Chebyshev–Clenshaw (~1e-11, |d|≤7 only). The
    // patch mask below is retained unchanged (degenerate + |d|>kNormCdfWing lanes
    // still route to the scalar kernel), so patched-lane parity is preserved.
    norm_cdf_erfc_pd2(d1, d2, Nd1, Nd2);

    const __m256d call =
        _mm256_mul_pd(safeDf, _mm256_sub_pd(_mm256_mul_pd(safeF, Nd1), _mm256_mul_pd(safeK, Nd2)));
    const __m256d put =
        _mm256_mul_pd(safeDf, _mm256_sub_pd(_mm256_mul_pd(safeK, _mm256_sub_pd(one, Nd2)),
                                            _mm256_mul_pd(safeF, _mm256_sub_pd(one, Nd1))));
    __m256d price = _mm256_blendv_pd(call, put, side_blend_mask(side, i));

    const int patch = patch_bits(input_patch, d1, d2);
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
