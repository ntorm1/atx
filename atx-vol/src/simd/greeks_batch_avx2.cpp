// AVX2 (4-lane f64) batched Black-76 Greeks + price.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 contracts;
// the n % 4 tail and any degenerate/deep-wing lanes fall through to the exact
// scalar kernel atx::vol::black76_greeks, which keeps the batch bit-for-bit with
// the scalar source of truth wherever the Chebyshev-Φ core would lose accuracy.
//
// ── One vector core, two scatters (P3.4) ──────────────────────────────────
// The 4-lane math is computed ONCE per block into stack columns (greeks_block);
// a Sink then places each lane either into the AoS `Greeks[]` (AoSSink) or into
// the per-greek SoA columns (SoASink). Because both scatters read the SAME stack
// doubles and both patch through the SAME scalar black76_greeks, the AoS and SoA
// public entry points are bit-for-bit identical field-for-field, and the AoS
// output is byte-for-byte the pre-SoA kernel's.
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
// Degenerate lanes (T ≤ 0 or σ ≤ 0) get dummy finite inputs to stay branchless,
// then are patched; deep-wing lanes (|d1|,|d2| > kNormCdfWing) are patched too.

#include "greeks_batch_avx2.hpp"

#include "atx/vol/detail/norm_cdf_cheb.hpp"
#include "atx/vol/detail/vector_math.hpp"
#include "atx/vol/greeks.hpp"
#include "atx/vol/simd/greeks_batch.hpp"

#include <immintrin.h>

#include <limits>

namespace atx::vol::simd::detail {

// Pull in the shared 4-lane transcendentals (log_pd/exp_pd/norm_cdf_pd2/
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

ATX_FORCE_INLINE __m256d input_patch_mask(__m256d F, __m256d K, __m256d T, __m256d sigma, __m256d r,
                                          __m256d df, __m256d zero) noexcept {
  __m256d patch = nonfinite_mask(F);
  patch = _mm256_or_pd(patch, nonfinite_mask(K));
  patch = _mm256_or_pd(patch, nonfinite_mask(T));
  patch = _mm256_or_pd(patch, nonfinite_mask(sigma));
  patch = _mm256_or_pd(patch, nonfinite_mask(r));
  patch = _mm256_or_pd(patch, nonfinite_mask(df));
  patch = _mm256_or_pd(patch, _mm256_cmp_pd(F, zero, _CMP_LE_OQ));
  patch = _mm256_or_pd(patch, _mm256_cmp_pd(K, zero, _CMP_LE_OQ));
  patch = _mm256_or_pd(patch, _mm256_cmp_pd(T, zero, _CMP_LE_OQ));
  patch = _mm256_or_pd(patch, _mm256_cmp_pd(sigma, zero, _CMP_LE_OQ));
  return _mm256_or_pd(patch, _mm256_cmp_pd(df, zero, _CMP_LE_OQ));
}

// Lanes needing the exact scalar path: degenerate (T ≤ 0 or σ ≤ 0) OR deep-wing
// (either d exceeds the Chebyshev-accurate interior). Returns a 4-bit movemask.
ATX_FORCE_INLINE int patch_bits(__m256d degen, __m256d d1, __m256d d2) noexcept {
  const __m256d w = _mm256_set1_pd(kNormCdfWing);
  const __m256d nw = _mm256_set1_pd(-kNormCdfWing);
  const __m256d wing = _mm256_or_pd(
      _mm256_or_pd(_mm256_cmp_pd(d1, w, _CMP_GT_OQ), _mm256_cmp_pd(d1, nw, _CMP_LT_OQ)),
      _mm256_or_pd(_mm256_cmp_pd(d2, w, _CMP_GT_OQ), _mm256_cmp_pd(d2, nw, _CMP_LT_OQ)));
  return _mm256_movemask_pd(_mm256_or_pd(degen, wing));
}

// One 4-lane block, computed into aligned stack columns. The eight Greeks + price
// for lanes [i, i+4). Returns the patch movemask (lanes to recompute scalar).
struct BlockOut {
  alignas(32) double dl[4], gm[4], vg[4], th[4], rh[4], vn[4], vl[4], cm[4];
  alignas(32) double pr[4];
};

ATX_FORCE_INLINE int greeks_block(const double *F, const double *K, const double *T,
                                  const double *sigma, const double *r, const double *df,
                                  const Side *side, std::size_t i, BlockOut &b) noexcept {
  const double *coefs = norm_cdf_cheb_coefs().data();
  const __m256d half = _mm256_set1_pd(0.5);
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d two = _mm256_set1_pd(2.0);
  const __m256d zero = _mm256_setzero_pd();

  const __m256d Fv = _mm256_loadu_pd(F + i);
  const __m256d Kv = _mm256_loadu_pd(K + i);
  const __m256d Tv = _mm256_loadu_pd(T + i);
  const __m256d sv = _mm256_loadu_pd(sigma + i);
  const __m256d rv = _mm256_loadu_pd(r + i);
  const __m256d dfv = _mm256_loadu_pd(df + i);

  const __m256d input_patch = input_patch_mask(Fv, Kv, Tv, sv, rv, dfv, zero);
  const __m256d safeT = _mm256_blendv_pd(Tv, one, input_patch);
  const __m256d safeS = _mm256_blendv_pd(sv, one, input_patch);

  const __m256d sqrtT = _mm256_sqrt_pd(safeT);
  const __m256d v = _mm256_mul_pd(safeS, sqrtT);
  const __m256d lnFK = log_pd(_mm256_div_pd(Fv, Kv));
  const __m256d d1 =
      _mm256_div_pd(_mm256_add_pd(lnFK, _mm256_mul_pd(half, _mm256_mul_pd(v, v))), v);
  const __m256d d2 = _mm256_sub_pd(d1, v);

  __m256d Nd1, Nd2;
  norm_cdf_pd2(d1, d2, coefs, Nd1, Nd2); // fused: hides Clenshaw latency
  const __m256d phi = norm_pdf_pd(d1);   // φ(d1), shared across Greeks

  // Prices (call & put); side blend selects per lane.
  const __m256d cNd1 = _mm256_sub_pd(one, Nd1);
  const __m256d cNd2 = _mm256_sub_pd(one, Nd2);
  const __m256d call =
      _mm256_mul_pd(dfv, _mm256_sub_pd(_mm256_mul_pd(Fv, Nd1), _mm256_mul_pd(Kv, Nd2)));
  const __m256d put =
      _mm256_mul_pd(dfv, _mm256_sub_pd(_mm256_mul_pd(Kv, cNd2), _mm256_mul_pd(Fv, cNd1)));
  const __m256d smask = side_blend_mask(side, i);
  const __m256d price = _mm256_blendv_pd(call, put, smask);

  // delta: call = df·Φ(d1); put = -df·(1-Φ(d1)).
  const __m256d delta_call = _mm256_mul_pd(dfv, Nd1);
  const __m256d delta_put = _mm256_sub_pd(zero, _mm256_mul_pd(dfv, cNd1));
  const __m256d delta = _mm256_blendv_pd(delta_call, delta_put, smask);

  // Call/put-symmetric Greeks (match src/greeks.cpp term-for-term).
  const __m256d df_phi = _mm256_mul_pd(dfv, phi);
  const __m256d gamma = _mm256_div_pd(df_phi, _mm256_mul_pd(Fv, v));    // df·φ/(F·v)
  const __m256d vega = _mm256_mul_pd(_mm256_mul_pd(df_phi, Fv), sqrtT); // df·F·φ·√T
  const __m256d vanna =
      _mm256_div_pd(_mm256_sub_pd(zero, _mm256_mul_pd(df_phi, d2)), safeS); // -df·φ·d2/σ
  const __m256d volga =
      _mm256_div_pd(_mm256_mul_pd(_mm256_mul_pd(vega, d1), d2), safeS); // vega·d1·d2/σ
  // theta = r·price - df·F·φ·σ/(2√T)  (calendar-time).
  const __m256d theta = _mm256_sub_pd(
      _mm256_mul_pd(rv, price),
      _mm256_div_pd(_mm256_mul_pd(_mm256_mul_pd(df_phi, Fv), safeS), _mm256_mul_pd(two, sqrtT)));
  // charm = r·delta + df·φ·d2/(2T).
  const __m256d charm =
      _mm256_add_pd(_mm256_mul_pd(rv, delta),
                    _mm256_div_pd(_mm256_mul_pd(df_phi, d2), _mm256_mul_pd(two, safeT)));
  // rho = -T·price (same form for call and put).
  const __m256d rho = _mm256_mul_pd(_mm256_sub_pd(zero, Tv), price);

  _mm256_store_pd(b.dl, delta);
  _mm256_store_pd(b.gm, gamma);
  _mm256_store_pd(b.vg, vega);
  _mm256_store_pd(b.th, theta);
  _mm256_store_pd(b.rh, rho);
  _mm256_store_pd(b.vn, vanna);
  _mm256_store_pd(b.vl, volga);
  _mm256_store_pd(b.cm, charm);
  _mm256_store_pd(b.pr, price);

  return patch_bits(input_patch, d1, d2);
}

// AoS scatter sink: writes lane j into greeks_out[idx] / price_out[idx].
struct AoSSink {
  const double *F, *K, *T, *sigma, *r, *df;
  const Side *side;
  Greeks *g;
  double *px;
  ATX_FORCE_INLINE void vec(std::size_t idx, int j, const BlockOut &b) noexcept {
    Greeks &o = g[idx];
    o.delta = b.dl[j];
    o.gamma = b.gm[j];
    o.vega = b.vg[j];
    o.theta = b.th[j];
    o.rho = b.rh[j];
    o.vanna = b.vn[j];
    o.volga = b.vl[j];
    o.charm = b.cm[j];
    if (px != nullptr) {
      px[idx] = b.pr[j];
    }
  }
  ATX_FORCE_INLINE void scalar(std::size_t idx) noexcept {
    const Black76Greeks bg =
        black76_greeks(F[idx], K[idx], T[idx], sigma[idx], r[idx], df[idx], side[idx]);
    g[idx] = bg.greeks;
    if (px != nullptr) {
      px[idx] = bg.price;
    }
  }
};

// SoA scatter sink: writes each non-null column at index idx.
struct SoASink {
  const double *F, *K, *T, *sigma, *r, *df;
  const Side *side;
  GreeksBatchSoA out;
  ATX_FORCE_INLINE void store(std::size_t idx, const Greeks &gg, double p) noexcept {
    if (out.delta)
      out.delta[idx] = gg.delta;
    if (out.gamma)
      out.gamma[idx] = gg.gamma;
    if (out.vega)
      out.vega[idx] = gg.vega;
    if (out.theta)
      out.theta[idx] = gg.theta;
    if (out.rho)
      out.rho[idx] = gg.rho;
    if (out.vanna)
      out.vanna[idx] = gg.vanna;
    if (out.volga)
      out.volga[idx] = gg.volga;
    if (out.charm)
      out.charm[idx] = gg.charm;
    if (out.price)
      out.price[idx] = p;
  }
  ATX_FORCE_INLINE void vec(std::size_t idx, int j, const BlockOut &b) noexcept {
    if (out.delta)
      out.delta[idx] = b.dl[j];
    if (out.gamma)
      out.gamma[idx] = b.gm[j];
    if (out.vega)
      out.vega[idx] = b.vg[j];
    if (out.theta)
      out.theta[idx] = b.th[j];
    if (out.rho)
      out.rho[idx] = b.rh[j];
    if (out.vanna)
      out.vanna[idx] = b.vn[j];
    if (out.volga)
      out.volga[idx] = b.vl[j];
    if (out.charm)
      out.charm[idx] = b.cm[j];
    if (out.price)
      out.price[idx] = b.pr[j];
  }
  ATX_FORCE_INLINE void scalar(std::size_t idx) noexcept {
    const Black76Greeks bg =
        black76_greeks(F[idx], K[idx], T[idx], sigma[idx], r[idx], df[idx], side[idx]);
    store(idx, bg.greeks, bg.price);
  }
};

// Shared driver: 4-lane blocks through greeks_block + Sink, scalar tail + patched
// lanes through Sink::scalar. One copy of the math for both AoS and SoA entries.
template <class Sink>
ATX_FORCE_INLINE void greeks_batch_drive(const double *F, const double *K, const double *T,
                                         const double *sigma, const double *r, const double *df,
                                         const Side *side, std::size_t n, Sink sink) noexcept {
  std::size_t i = 0;
  for (; i + 4 <= n; i += 4) {
    BlockOut b;
    const int patch = greeks_block(F, K, T, sigma, r, df, side, i, b);
    for (int j = 0; j < 4; ++j) {
      if (patch & (1 << j)) {
        sink.scalar(i + static_cast<std::size_t>(j));
      } else {
        sink.vec(i + static_cast<std::size_t>(j), j, b);
      }
    }
  }
  for (; i < n; ++i) {
    sink.scalar(i);
  }
}

} // namespace

void black76_greeks_batch_avx2(const double *F, const double *K, const double *T,
                               const double *sigma, const double *r, const double *df,
                               const Side *side, Greeks *greeks_out, double *price_out,
                               std::size_t n) noexcept {
  greeks_batch_drive(F, K, T, sigma, r, df, side, n,
                     AoSSink{F, K, T, sigma, r, df, side, greeks_out, price_out});
}

void black76_greeks_batch_soa_avx2(const double *F, const double *K, const double *T,
                                   const double *sigma, const double *r, const double *df,
                                   const Side *side, const GreeksBatchSoA &out,
                                   std::size_t n) noexcept {
  greeks_batch_drive(F, K, T, sigma, r, df, side, n, SoASink{F, K, T, sigma, r, df, side, out});
}

} // namespace atx::vol::simd::detail
