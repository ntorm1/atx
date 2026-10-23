// K2 (r5, 2026-10-22): the 4-lane AVX2 fused (σ, q) family price + vega (contract in
// simd/american_iv_batch_avx2.hpp). Built with -mavx2 -mfma (the *_avx2.cpp glob in
// atx-vol/CMakeLists.txt); called only behind simd::have_avx2().

#include "simd/american_iv_batch_avx2.hpp"

#include <immintrin.h>

#include <cmath>
#include <cstddef>
#include <limits>

#include "pricing/american_boundary.hpp"     // amer::al_xmax_put
#include "pricing/american_sigma_family.hpp" // sigma_family_detail::diff_tables
#include "pricing/boundary_interp.hpp"       // detail::internal_put_coords
#include "simd/vector_math.hpp"

namespace atx::vol::simd::iv_batch_avx2 {

namespace {

namespace sq = sigma_q_family_detail;
using sigma_family_detail::diff_tables;
using sigma_family_detail::DiffTables;

// Φ(x) and φ(x) from one exponential: erfc_nonneg_pd's regions (simd/vector_math.hpp, the
// Cody CALERF coefficients) with e = exp(−y²), y = |x|/√2, returned as well — that e IS
// e^{−x²/2}, so φ(x) = e / √(2π). The asymptotic region (y > 4) is computed only when a
// lane needs it; every selected lane's value is the full-range kernel's.
ATX_FORCE_INLINE void cdf_pdf_pd(__m256d x, __m256d &cdf, __m256d &pdf) noexcept {
  using namespace atx::vol::detail;
  const __m256d abs_x = _mm256_andnot_pd(_mm256_set1_pd(-0.0), x);
  const __m256d y = _mm256_mul_pd(abs_x, _mm256_set1_pd(1.0 / kSqrt2));
  const __m256d ysq = _mm256_mul_pd(y, y);
  const __m256d e = exp_pd(_mm256_sub_pd(_mm256_setzero_pd(), ysq));
  const __m256d in_lo = _mm256_cmp_pd(y, _mm256_set1_pd(kCodyThresh), _CMP_LE_OQ);
  __m256d num1 = _mm256_set1_pd(kCodyA[4]);
  num1 = _mm256_fmadd_pd(num1, ysq, _mm256_set1_pd(kCodyA[0]));
  num1 = _mm256_fmadd_pd(num1, ysq, _mm256_set1_pd(kCodyA[1]));
  num1 = _mm256_fmadd_pd(num1, ysq, _mm256_set1_pd(kCodyA[2]));
  num1 = _mm256_fmadd_pd(num1, ysq, _mm256_set1_pd(kCodyA[3]));
  __m256d den1 = _mm256_set1_pd(1.0);
  den1 = _mm256_fmadd_pd(den1, ysq, _mm256_set1_pd(kCodyB[0]));
  den1 = _mm256_fmadd_pd(den1, ysq, _mm256_set1_pd(kCodyB[1]));
  den1 = _mm256_fmadd_pd(den1, ysq, _mm256_set1_pd(kCodyB[2]));
  den1 = _mm256_fmadd_pd(den1, ysq, _mm256_set1_pd(kCodyB[3]));
  __m256d num2 = _mm256_set1_pd(kCodyC[8]);
  __m256d den2 = _mm256_set1_pd(1.0);
  for (int j = 0; j < 8; ++j) {
    num2 = _mm256_fmadd_pd(num2, y, _mm256_set1_pd(kCodyC[j]));
    den2 = _mm256_fmadd_pd(den2, y, _mm256_set1_pd(kCodyD[j]));
  }
  const __m256d num12 = _mm256_blendv_pd(num2, _mm256_mul_pd(y, num1), in_lo);
  const __m256d den12 = _mm256_blendv_pd(den2, den1, in_lo);
  const __m256d q12 = _mm256_div_pd(num12, den12);
  const __m256d r_lo = _mm256_sub_pd(_mm256_set1_pd(1.0), q12);
  const __m256d r_mid = _mm256_mul_pd(e, q12);
  const __m256d in_hi = _mm256_cmp_pd(y, _mm256_set1_pd(4.0), _CMP_GT_OQ);
  __m256d erfc = r_mid;
  if (_mm256_movemask_pd(in_hi) != 0) {
    const __m256d w = _mm256_div_pd(_mm256_set1_pd(1.0), ysq);
    __m256d num3 = _mm256_set1_pd(kCodyP[5]);
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[0]));
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[1]));
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[2]));
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[3]));
    num3 = _mm256_fmadd_pd(num3, w, _mm256_set1_pd(kCodyP[4]));
    __m256d den3 = _mm256_set1_pd(1.0);
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[0]));
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[1]));
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[2]));
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[3]));
    den3 = _mm256_fmadd_pd(den3, w, _mm256_set1_pd(kCodyQ[4]));
    __m256d r_hi = _mm256_mul_pd(w, _mm256_div_pd(num3, den3));
    r_hi = _mm256_div_pd(_mm256_sub_pd(_mm256_set1_pd(kCodySqrtPiInv), r_hi), y);
    r_hi = _mm256_mul_pd(e, r_hi);
    erfc = _mm256_blendv_pd(r_mid, r_hi, in_hi);
  }
  erfc = _mm256_blendv_pd(erfc, r_lo, in_lo);
  const __m256d half_ea = _mm256_mul_pd(_mm256_set1_pd(0.5), erfc);
  const __m256d hi = _mm256_sub_pd(_mm256_set1_pd(1.0), half_ea);
  const __m256d nonneg = _mm256_cmp_pd(x, _mm256_setzero_pd(), _CMP_GE_OQ);
  cdf = _mm256_blendv_pd(half_ea, hi, nonneg);
  pdf = _mm256_mul_pd(_mm256_set1_pd(kInvSqrt2Pi), e);
}

ATX_FORCE_INLINE __m256d ld4(const double *p) noexcept { return _mm256_load_pd(p); }

// The normalised barycentric σ basis b and its nodal derivative c = bᵀD1, four lanes
// (sigma_q_family_detail::bary_basis / bary_basis_derivative per lane; a lane on an exact
// node takes the scalar bary_basis itself — the unit vector on its first hit node).
void sigma_basis(const sq::PremiumTable &t, const double *z, __m256d *b, __m256d *c) noexcept {
  const unsigned ns = t.n_sigma;
  const __m256d zq = ld4(z);
  __m256d den = _mm256_setzero_pd();
  __m256d hit_any = _mm256_setzero_pd();
  for (unsigned s = 0; s < ns; ++s) {
    const __m256d dz = _mm256_sub_pd(zq, _mm256_set1_pd(t.sz[s]));
    hit_any = _mm256_or_pd(hit_any, _mm256_cmp_pd(dz, _mm256_setzero_pd(), _CMP_EQ_OQ));
    b[s] = _mm256_div_pd(_mm256_set1_pd(t.sw[s]), dz);
    den = _mm256_add_pd(den, b[s]);
  }
  const __m256d inv = _mm256_div_pd(_mm256_set1_pd(1.0), den);
  for (unsigned s = 0; s < ns; ++s) {
    b[s] = _mm256_mul_pd(b[s], inv);
  }
  if (_mm256_movemask_pd(hit_any) != 0) {
    alignas(32) double bl[kSigmaNodes][kLanes];
    for (unsigned s = 0; s < ns; ++s) {
      _mm256_store_pd(bl[s], b[s]);
    }
    for (std::size_t k = 0; k < kLanes; ++k) {
      bool hit = false;
      for (unsigned s = 0; s < ns; ++s) {
        hit = hit || (z[k] - t.sz[s] == 0.0);
      }
      if (hit) {
        double one[kSigmaNodes] = {};
        sq::bary_basis(t.sz, t.sw, ns, z[k], one);
        for (unsigned u = 0; u < ns; ++u) {
          bl[u][k] = one[u];
        }
      }
    }
    for (unsigned s = 0; s < ns; ++s) {
      b[s] = _mm256_load_pd(bl[s]);
    }
  }
  const DiffTables &dt = diff_tables(ns);
  for (unsigned u = 0; u < ns; ++u) {
    c[u] = _mm256_setzero_pd();
  }
  for (unsigned s = 0; s < ns; ++s) {
    for (unsigned u = 0; u < ns; ++u) {
      c[u] = _mm256_fmadd_pd(b[s], _mm256_set1_pd(dt.d1[s][u]), c[u]);
    }
  }
}

} // namespace

void load_strike(const sq::PremiumTable &t, double S, double K, std::size_t k,
                 Slots &s) noexcept {
  const detail::InternalPutCoords ic = detail::internal_put_coords(t.side, S, K);
  const double rp = s.rp[k];
  const double qp = s.qp[k];
  const double xmax = amer::al_xmax_put(ic.Kp, rp, qp);
  s.Sp[k] = ic.Sp;
  s.Kp[k] = ic.Kp;
  s.xmax[k] = xmax;
  s.lsx[k] = std::log(ic.Sp / xmax);
  s.lsk[k] = std::log(ic.Sp / ic.Kp);
  s.drift[k] = rp - qp;
  s.ak[k] = rp * ic.Kp;
  s.bs[k] = qp * ic.Sp;
  // The European put leg's forward and discount: euro_leg's expressions.
  s.F[k] = ic.Sp * std::exp((rp - qp) * t.nodes.T);
  s.df[k] = std::exp(-rp * t.nodes.T);
  s.lfk[k] = std::log(s.F[k] / ic.Kp);
  s.intr[k] = ic.Kp - ic.Sp;
}

void load_slot(const sq::PremiumTable &t, const sq::QTile &tile, double S, double K,
               std::size_t k, Slots &s) noexcept {
  s.rp[k] = tile.carry.rp;
  s.qp[k] = tile.carry.qp;
  const unsigned n = t.nodes.n;
  for (unsigned i = 0; i < n; ++i) {
    s.dq[i][k] = tile.carry.dq[i];
    s.dr[i][k] = tile.carry.dr[i];
  }
  const std::size_t cells = static_cast<std::size_t>(n + 1u) * t.n_sigma;
  for (std::size_t j = 0; j < cells; ++j) {
    s.g[j][k] = tile.g[j];
  }
  load_strike(t, S, K, k, s);
}

void price_vega4(const sq::PremiumTable &t, const Slots &s, Result &out) noexcept {
  using namespace atx::vol::detail;
  const sq::PremiumNodes &pn = t.nodes;
  const unsigned n = pn.n;
  const unsigned ns = t.n_sigma;
  // σ -> the axis coordinate z and dz/dσ per lane (the family's own sigma_z).
  alignas(32) double z[kLanes];
  alignas(32) double dzs[kLanes];
  for (std::size_t k = 0; k < kLanes; ++k) {
    const sq::SigmaZ az = sq::sigma_z(t.axis, t.u_lo, t.u_hi, s.sigma[k]);
    z[k] = az.z;
    dzs[k] = az.dz;
  }
  __m256d b[kSigmaNodes];
  __m256d c[kSigmaNodes];
  sigma_basis(t, z, b, c);
  const __m256d dz = ld4(dzs);
  // The boundary at the premium nodes: two n_sigma-dots per row (boundary_from_tile).
  alignas(32) double y[sq::kMaxPremiumNodes][kLanes];
  alignas(32) double dy[sq::kMaxPremiumNodes][kLanes];
  __m256d y_now = _mm256_setzero_pd();
  for (unsigned i = 0; i <= n; ++i) {
    const double(*row)[kLanes] = &s.g[static_cast<std::size_t>(i) * ns];
    __m256d a = _mm256_setzero_pd();
    __m256d d = _mm256_setzero_pd();
    for (unsigned j = 0; j < ns; ++j) {
      const __m256d gv = ld4(row[j]);
      a = _mm256_fmadd_pd(b[j], gv, a);
      d = _mm256_fmadd_pd(c[j], gv, d);
    }
    if (i < n) {
      _mm256_store_pd(y[i], a);
      _mm256_store_pd(dy[i], _mm256_mul_pd(d, dz));
    } else {
      y_now = a;
    }
  }
  // finish_node_boundary: the kink scale, s = √max(y, 0), ds = y'/(2s), the skips.
  const __m256d sign = _mm256_set1_pd(-0.0);
  const __m256d zero = _mm256_setzero_pd();
  __m256d ymax = _mm256_andnot_pd(sign, y_now);
  for (unsigned i = 0; i < n; ++i) {
    if (pn.kind[i] == sq::PremiumNodeKind::Boundary) {
      ymax = _mm256_max_pd(ymax, _mm256_andnot_pd(sign, ld4(y[i])));
    }
  }
  const __m256d kink = _mm256_mul_pd(_mm256_set1_pd(sq::kFusedKinkRel), ymax);
  __m256d degenerate = zero;
  const __m256d sigma = ld4(s.sigma);
  const __m256d inv_sigma = _mm256_div_pd(_mm256_set1_pd(1.0), sigma);
  const __m256d lsx = ld4(s.lsx);
  const __m256d drift = ld4(s.drift);
  const __m256d ak = ld4(s.ak);
  const __m256d bs = ld4(s.bs);
  const __m256d half = _mm256_set1_pd(0.5);
  __m256d total = zero;
  __m256d asig = zero;
  for (unsigned i = 0; i < n; ++i) {
    const sq::PremiumNodeKind kind = pn.kind[i];
    if (kind == sq::PremiumNodeKind::Dead) {
      continue;
    }
    const __m256d v = _mm256_mul_pd(sigma, _mm256_set1_pd(pn.sqrt_t[i]));
    const __m256d inv_v = _mm256_mul_pd(inv_sigma, _mm256_set1_pd(1.0 / pn.sqrt_t[i]));
    const __m256d dq = ld4(s.dq[i]);
    const __m256d dr = ld4(s.dr[i]);
    __m256d ld;
    __m256d ds = zero;
    __m256d live = _mm256_castsi256_pd(_mm256_set1_epi64x(-1));
    if (kind == sq::PremiumNodeKind::Boundary) {
      const __m256d yi = ld4(y[i]);
      const __m256d dyi = ld4(dy[i]);
      const __m256d sv = _mm256_sqrt_pd(_mm256_max_pd(yi, zero));
      live = _mm256_cmp_pd(sv, _mm256_set1_pd(745.0), _CMP_LT_OQ);
      const __m256d pos = _mm256_cmp_pd(yi, zero, _CMP_GT_OQ);
      ds = _mm256_and_pd(pos, _mm256_div_pd(dyi, _mm256_add_pd(sv, sv)));
      const __m256d near = _mm256_cmp_pd(_mm256_andnot_pd(sign, yi), kink, _CMP_LE_OQ);
      const __m256d moving = _mm256_cmp_pd(dyi, zero, _CMP_NEQ_UQ);
      degenerate = _mm256_or_pd(degenerate, _mm256_and_pd(near, moving));
      ld = _mm256_fmadd_pd(drift, _mm256_set1_pd(pn.t[i]), _mm256_add_pd(lsx, sv));
    } else { // Strike: B = K (unreachable for interior Gauss-Legendre nodes; mirrored)
      ld = log_pd(_mm256_div_pd(_mm256_mul_pd(ld4(s.Sp), dq), _mm256_mul_pd(ld4(s.Kp), dr)));
    }
    const __m256d dp = _mm256_fmadd_pd(ld, inv_v, _mm256_mul_pd(half, v));
    const __m256d dm = _mm256_sub_pd(dp, v);
    const __m256d a = _mm256_mul_pd(ak, dr);
    const __m256d bb = _mm256_mul_pd(bs, dq);
    __m256d cm;
    __m256d phm;
    __m256d cp;
    __m256d php;
    cdf_pdf_pd(_mm256_sub_pd(zero, dm), cm, phm);
    cdf_pdf_pd(_mm256_sub_pd(zero, dp), cp, php);
    const __m256d w = _mm256_set1_pd(pn.wz2[i]);
    const __m256d term = _mm256_mul_pd(w, _mm256_fmsub_pd(a, cm, _mm256_mul_pd(bb, cp)));
    total = _mm256_add_pd(total, _mm256_and_pd(live, term));
    const __m256d am = _mm256_mul_pd(a, phm);
    const __m256d bp = _mm256_mul_pd(bb, php);
    const __m256d expl = _mm256_mul_pd(_mm256_fmsub_pd(_mm256_mul_pd(a, dp), phm,
                                                       _mm256_mul_pd(_mm256_mul_pd(bb, dm), php)),
                                       inv_sigma);
    const __m256d motion = _mm256_mul_pd(_mm256_mul_pd(_mm256_sub_pd(am, bp), ds), inv_v);
    asig = _mm256_add_pd(asig, _mm256_and_pd(live, _mm256_mul_pd(w, _mm256_sub_pd(expl, motion))));
  }
  const __m256d half_sqrt_t = _mm256_set1_pd(pn.half_sqrtT);
  total = _mm256_mul_pd(total, half_sqrt_t);
  // The European put leg (black76_value_and_vega's expressions).
  const __m256d sqrt_tt = _mm256_set1_pd(pn.sqrtT);
  const __m256d ve = _mm256_mul_pd(sigma, sqrt_tt);
  const __m256d inv_ve = _mm256_div_pd(_mm256_set1_pd(1.0), ve);
  const __m256d d1 =
      _mm256_mul_pd(_mm256_fmadd_pd(_mm256_mul_pd(half, ve), ve, ld4(s.lfk)), inv_ve);
  const __m256d d2 = _mm256_sub_pd(d1, ve);
  __m256d c1;
  __m256d p1;
  __m256d c2;
  __m256d p2;
  cdf_pdf_pd(_mm256_sub_pd(zero, d1), c1, p1);
  cdf_pdf_pd(_mm256_sub_pd(zero, d2), c2, p2);
  const __m256d F = ld4(s.F);
  const __m256d df = ld4(s.df);
  const __m256d euro =
      _mm256_mul_pd(df, _mm256_fmsub_pd(ld4(s.Kp), c2, _mm256_mul_pd(F, c1)));
  const __m256d euro_vega = _mm256_mul_pd(_mm256_mul_pd(_mm256_mul_pd(F, df), p1), sqrt_tt);
  alignas(32) double tot[kLanes];
  alignas(32) double as[kLanes];
  alignas(32) double eu[kLanes];
  alignas(32) double ev[kLanes];
  alignas(32) double yn[kLanes];
  alignas(32) double dg[kLanes];
  _mm256_store_pd(tot, total);
  _mm256_store_pd(as, asig);
  _mm256_store_pd(eu, euro);
  _mm256_store_pd(ev, euro_vega);
  _mm256_store_pd(yn, y_now);
  _mm256_store_pd(dg, degenerate);
  // The al_put_price_from_boundary clamp order and the exercise rule, per lane (scalar).
  for (std::size_t k = 0; k < kLanes; ++k) {
    const bool floored = !(tot[k] > 0.0);
    const double prem = floored ? 0.0 : tot[k];
    double price = eu[k] + prem;
    const double intr = s.intr[k];
    bool clamped = false;
    if (intr > price) {
      price = intr;
      clamped = true;
    }
    if (eu[k] > price) {
      price = eu[k];
    }
    if (price < 0.0) {
      price = 0.0;
      clamped = true;
    }
    const double now_factor = std::exp(-std::sqrt((yn[k] > 0.0) ? yn[k] : 0.0));
    const bool deg = dg[k] != 0.0;
    out.price[k] = price;
    out.exercised[k] = (clamped || (intr > 0.0 && price <= intr + 1.0e-9 * s.Kp[k]) ||
                        s.Sp[k] <= s.xmax[k] * now_factor)
                           ? 1u
                           : 0u;
    out.degenerate[k] = (!clamped && deg) ? 1u : 0u;
    if (clamped) {
      out.vega[k] = 0.0;
    } else if (deg) {
      out.vega[k] = std::numeric_limits<double>::quiet_NaN();
    } else {
      out.vega[k] = ev[k] + (floored ? 0.0 : pn.half_sqrtT * as[k]);
    }
  }
}

} // namespace atx::vol::simd::iv_batch_avx2
