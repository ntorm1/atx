// L2h-c (2026-10-21): 4-lane AVX2 node solver over the shared boundary kernel — the
// AlSigmaQFamily build's batched path. See american_boundary_nodes_avx2.hpp.
//
// Built with -mavx2 -mfma (src/simd/*_avx2.cpp CMake glob).
//
// Structure: solve_put_boundary_pack_avx2 runs with a ZERO sweep budget (init + the
// 4-wide Barone-Adesi-Whaley seed, exactly as it seeds a full solve), then this TU runs
// the kernel's Jacobi-Newton / fixed-point sweep loop (american_boundary_avx2_kernel.hpp
// step 3, transcribed operation for operation) with eqn_b_ND's sweep-invariant
// quadrature geometry read from NodePackScratch instead of recomputed per sweep. The
// hoisted values come from the kernel's own intrinsic sequence on the same inputs (no
// scalar re-expression, so no FP-contraction difference) and enter the kernel's
// accumulations in the kernel's order: the converged boundary is bit-identical to the
// kernel's (pinned by the family tests' AVX2-vs-scalar numbers, which do not move).

#include "american_boundary_nodes_avx2.hpp"

#include "american_boundary_avx2_kernel.hpp"

#include "fitting/counters.hpp" // BoundarySolves / AlBoundarySolves — per-solve ledger

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if !defined(__AVX2__) || !defined(__FMA__)
#  error "american_boundary_nodes_avx2.cpp requires -mavx2 -mfma (build via src/simd/*_avx2.cpp)"
#endif

#include <immintrin.h>

namespace atx::vol::simd::detail {

namespace {

[[nodiscard]] bool same_bits(double a, double b) noexcept {
  return std::bit_cast<std::uint64_t>(a) == std::bit_cast<std::uint64_t>(b);
}

[[nodiscard]] bool same_geometry(const PutPackBoundary &pk, const amer::AlBoundary &bnd,
                                  const amer::AlWorkspace &ws,
                                  const NodePackScratch &geo) noexcept {
  const auto &key = geo.geometry_key;
  if (!geo.geometry_ready || key.nb != pk.nb || key.nq != pk.nq || !same_bits(key.T, bnd.T)) {
    return false;
  }
  for (unsigned i = 0; i < pk.nb; ++i) {
    if (!same_bits(key.tau[i], bnd.tau[i]) || !same_bits(key.z[i], pk.znodes[i]) ||
        !same_bits(key.wbary[i], pk.wbary[i])) return false;
  }
  for (unsigned i = 0; i < pk.nq; ++i) {
    if (!same_bits(key.qx[i], ws.qx_fp[i]) || !same_bits(key.qw[i], ws.qw_fp[i])) return false;
  }
  return true;
}

// These intrinsic operations are unchanged. Only their lifetime moves from one
// sigma pack to one exact initialized time/grid/quadrature key.
void hoist_invariant_geometry(const PutPackBoundary &pk, const amer::AlBoundary &ref_bnd,
                               const amer::AlWorkspace &ref_ws, NodePackScratch &geo) noexcept {
  const __m256d zero = _mm256_setzero_pd();
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d two = _mm256_set1_pd(2.0);
  const __m256d neg_one = _mm256_set1_pd(-1.0);
  const __m256d half = _mm256_set1_pd(0.5);
  const __m256d TINY = _mm256_set1_pd(1.0e-14);
  const unsigned nb = pk.nb;
  const unsigned nq = pk.nq;
  for (unsigned i = 1; i < nb; ++i) {
    // safe_tau == tau for every collocation node past the first (tau_i > 0).
    const __m256d tau = _mm256_set1_pd(ref_bnd.tau[i]);
    const __m256d half_tau = _mm256_mul_pd(half, tau);
    for (unsigned k = 0; k < nq; ++k) {
      const unsigned p = (i - 1u) * nq + k;
      const __m256d xs = _mm256_set1_pd(ref_ws.qx_fp[k]);
      const __m256d wv = _mm256_set1_pd(ref_ws.qw_fp[k]);
      const __m256d u = _mm256_mul_pd(half_tau, _mm256_add_pd(one, xs));
      const __m256d t_u = _mm256_sub_pd(tau, u);
      const __m256d tu_gt = _mm256_cmp_pd(t_u, TINY, _CMP_GT_OQ);
      const __m256d u_eff = _mm256_min_pd(u, pk.Tv);
      const __m256d zz = _mm256_sub_pd(
          _mm256_mul_pd(two, _mm256_sqrt_pd(_mm256_div_pd(u_eff, pk.Tv))), one);
      const __m256d zc = _mm256_min_pd(_mm256_max_pd(zz, neg_one), one);
      // cheb_eval_pd's per-term quantities, in its order.
      __m256d den = zero;
      int hit_at = -1;
      for (unsigned m = 0; m < nb; ++m) {
        const __m256d dz = _mm256_sub_pd(zc, _mm256_set1_pd(pk.znodes[m]));
        const __m256d hit = _mm256_cmp_pd(dz, zero, _CMP_EQ_OQ);
        const __m256d safe_dz = _mm256_blendv_pd(dz, one, hit);
        const __m256d qq = _mm256_div_pd(_mm256_set1_pd(pk.wbary[m]), safe_dz);
        den = _mm256_add_pd(den, qq);
        geo.qq[static_cast<std::size_t>(p) * NodePackScratch::kMaxNodes + m] =
            _mm256_cvtsd_f64(qq);
        if ((_mm256_movemask_pd(hit) & 1) != 0) {
          hit_at = static_cast<int>(m);
        }
      }
      geo.den[p] = _mm256_cvtsd_f64(den);
      geo.hit[p] = static_cast<std::int8_t>(hit_at);
      geo.tu_gt[p] = static_cast<std::uint8_t>((_mm256_movemask_pd(tu_gt) & 1) != 0 ? 1 : 0);
      const __m256d safe_tu = k_sel(tu_gt, t_u, one);
      geo.safe_tu[p] = _mm256_cvtsd_f64(safe_tu);
      geo.u[p] = _mm256_cvtsd_f64(u);
      geo.weight[p] = _mm256_cvtsd_f64(wv);
      geo.sqrt_tu[p] = _mm256_cvtsd_f64(_mm256_sqrt_pd(safe_tu));
    }
  }
  auto &key = geo.geometry_key;
  key.T = ref_bnd.T;
  key.nb = pk.nb;
  key.nq = pk.nq;
  for (unsigned i = 0; i < pk.nb; ++i) {
    key.tau[i] = ref_bnd.tau[i];
    key.z[i] = pk.znodes[i];
    key.wbary[i] = pk.wbary[i];
  }
  for (unsigned i = 0; i < pk.nq; ++i) {
    key.qx[i] = ref_ws.qx_fp[i];
    key.qw[i] = ref_ws.qw_fp[i];
  }
  geo.geometry_ready = true;
  geo.carry_ready = false;
  ++geo.geometry_builds;
}

void hoist_geometry(const PutPackBoundary &pk, const amer::AlBoundary &ref_bnd,
                    const amer::AlWorkspace &ref_ws, NodePackScratch &geo) noexcept {
  if (!same_geometry(pk, ref_bnd, ref_ws, geo)) {
    hoist_invariant_geometry(pk, ref_bnd, ref_ws, geo);
  }
  std::array<double, 4> rate{}, yield{};
  _mm256_storeu_pd(rate.data(), pk.Rv);
  _mm256_storeu_pd(yield.data(), pk.Qv);
  bool carry_matches = geo.carry_ready;
  for (unsigned lane = 0; lane < 4; ++lane) {
    carry_matches = carry_matches && same_bits(rate[lane], geo.carry_r[lane]) &&
                    same_bits(yield[lane], geo.carry_q[lane]);
  }
  const unsigned pairs = (pk.nb - 1u) * pk.nq;
  for (unsigned p = 0; p < pairs; ++p) {
    if (!carry_matches) {
      const __m256d u = _mm256_set1_pd(geo.u[p]);
      const __m256d wv = _mm256_set1_pd(geo.weight[p]);
      _mm256_store_pd(&geo.er[static_cast<std::size_t>(p) * 4u],
                      _mm256_mul_pd(wv, exp_pd(_mm256_mul_pd(pk.Rv, u))));
      _mm256_store_pd(&geo.eq[static_cast<std::size_t>(p) * 4u],
                      _mm256_mul_pd(wv, exp_pd(_mm256_mul_pd(pk.Qv, u))));
    }
    _mm256_store_pd(&geo.vq[static_cast<std::size_t>(p) * 4u],
                    _mm256_mul_pd(pk.SIG, _mm256_set1_pd(geo.sqrt_tu[p])));
  }
  if (!carry_matches) {
    geo.carry_r = rate;
    geo.carry_q = yield;
    geo.carry_ready = true;
    ++geo.carry_builds;
  }
}

// The kernel's step-3 sweep loop on a seeded pack, eqn_b_ND reading `geo`. `Y` / `active`
// / `eligible` as the kernel holds them; TAU the per-lane collocation times.
void sweep_hoisted(PutPackBoundary &pk, const __m256d *TAU, const amer::AlScheme &sch,
                   const NodePackScratch &geo, bool eligible[4]) noexcept {
  const __m256d zero = _mm256_setzero_pd();
  const __m256d one = _mm256_set1_pd(1.0);
  const __m256d half = _mm256_set1_pd(0.5);
  const __m256d signmask = _mm256_set1_pd(-0.0);
  const __m256d TINY = _mm256_set1_pd(1.0e-14);
  const __m256d D300 = _mm256_set1_pd(1.0e-300);
  const __m256d DEN12 = _mm256_set1_pd(1.0e-12);
  const __m256d K1e6 = _mm256_set1_pd(1.0e-6);
  const __m256d TOL = _mm256_set1_pd(sch.tol);
  const unsigned nb = pk.nb;
  const unsigned nq = pk.nq;
  __m256d *Y = pk.Y;
  const __m256d XMAX = pk.XMAX;
  const __m256d Kv = pk.Kv;
  const __m256d SIG = pk.SIG;
  const __m256d Rv = pk.Rv;
  const __m256d Qv = pk.Qv;
  const __m256d RmQ = pk.RmQ;
  __m256d active = pk.active;
  auto abs_pd = [&](__m256d x) { return _mm256_andnot_pd(signmask, x); };
  auto b_from_y_pd = [&](__m256d y, __m256d xmax) {
    const __m256d yv2 = _mm256_max_pd(y, zero);
    return _mm256_mul_pd(xmax, exp_pd(_mm256_sub_pd(zero, _mm256_sqrt_pd(yv2))));
  };
  auto y_from_b_pd = [&](__m256d b, __m256d xmax) {
    const __m256d ok = _mm256_and_pd(_mm256_cmp_pd(b, zero, _CMP_GT_OQ),
                                     _mm256_cmp_pd(xmax, zero, _CMP_GT_OQ));
    const __m256d lg = log_pd(_mm256_div_pd(k_sel(ok, b, one), k_sel(ok, xmax, one)));
    return _mm256_and_pd(_mm256_mul_pd(lg, lg), ok);
  };
  // cheb_eval_pd at pair p from the hoisted terms: num accumulated in the kernel's order,
  // the same num / den, the exact-node blend reduced to the (lane-uniform) hit.
  auto cheb_at = [&](unsigned p) {
    const int h = geo.hit[p];
    if (h >= 0) {
      return Y[h];
    }
    const double *qq = &geo.qq[static_cast<std::size_t>(p) * NodePackScratch::kMaxNodes];
    __m256d num = zero;
    for (unsigned m = 0; m < nb; ++m) {
      num = _mm256_add_pd(num, _mm256_mul_pd(_mm256_set1_pd(qq[m]), Y[m]));
    }
    return _mm256_div_pd(num, _mm256_set1_pd(geo.den[p]));
  };
  auto eqn_b_ND = [&](unsigned nodei, __m256d tau, __m256d b_val, __m256d &N, __m256d &D) {
    const __m256d v_tip = _mm256_mul_pd(SIG, _mm256_sqrt_pd(tau));
    const __m256d base_tip = _mm256_div_pd(
        _mm256_add_pd(log_pd(_mm256_div_pd(b_val, Kv)), _mm256_mul_pd(RmQ, tau)), v_tip);
    const __m256d hv_tip = _mm256_mul_pd(half, v_tip);
    const __m256d dpv_tip = _mm256_add_pd(base_tip, hv_tip);
    const __m256d dmv_tip = _mm256_sub_pd(base_tip, hv_tip);
    __m256d tip_p, tip_m;
    norm_cdf_erfc_pd2(dpv_tip, dmv_tip, tip_p, tip_m);
    const __m256d half_tau = _mm256_mul_pd(half, tau);
    __m256d n_int = zero;
    __m256d d_int = zero;
    for (unsigned qi = 0; qi < nq; ++qi) {
      const unsigned p = (nodei - 1u) * nq + qi;
      const __m256d bu = b_from_y_pd(cheb_at(p), XMAX);
      const __m256d bu_gt = _mm256_cmp_pd(bu, zero, _CMP_GT_OQ);
      const __m256d tu_gt = (geo.tu_gt[p] != 0) ? _mm256_castsi256_pd(_mm256_set1_epi64x(-1))
                                                : zero;
      const __m256d qact = _mm256_and_pd(tu_gt, bu_gt);
      const __m256d safe_tu = _mm256_set1_pd(geo.safe_tu[p]);
      const __m256d z = _mm256_div_pd(b_val, k_sel(bu_gt, bu, one));
      const __m256d vq = _mm256_load_pd(&geo.vq[static_cast<std::size_t>(p) * 4u]);
      const __m256d base =
          _mm256_div_pd(_mm256_add_pd(log_pd(z), _mm256_mul_pd(RmQ, safe_tu)), vq);
      const __m256d hvq = _mm256_mul_pd(half, vq);
      const __m256d dpv = _mm256_add_pd(base, hvq);
      const __m256d dmv = _mm256_sub_pd(base, hvq);
      __m256d ncdf_p, ncdf_m;
      norm_cdf_erfc_pd2(dpv, dmv, ncdf_p, ncdf_m);
      const __m256d term_n =
          _mm256_mul_pd(_mm256_load_pd(&geo.er[static_cast<std::size_t>(p) * 4u]), ncdf_m);
      const __m256d term_d =
          _mm256_mul_pd(_mm256_load_pd(&geo.eq[static_cast<std::size_t>(p) * 4u]), ncdf_p);
      n_int = _mm256_add_pd(n_int, _mm256_and_pd(term_n, qact));
      d_int = _mm256_add_pd(d_int, _mm256_and_pd(term_d, qact));
    }
    N = _mm256_add_pd(tip_m, _mm256_mul_pd(Rv, _mm256_mul_pd(n_int, half_tau)));
    D = _mm256_add_pd(tip_p, _mm256_mul_pd(Qv, _mm256_mul_pd(d_int, half_tau)));
  };
  auto eqn_b_NDd = [&](__m256d tau, __m256d b_val, __m256d &Nd, __m256d &Dd) {
    const __m256d v = _mm256_mul_pd(SIG, _mm256_sqrt_pd(tau));
    const __m256d base = _mm256_div_pd(
        _mm256_add_pd(log_pd(_mm256_div_pd(b_val, Kv)), _mm256_mul_pd(RmQ, tau)), v);
    const __m256d hv = _mm256_mul_pd(half, v);
    const __m256d bv = _mm256_mul_pd(b_val, v);
    Nd = _mm256_div_pd(norm_pdf_pd(_mm256_sub_pd(base, hv)), bv);
    Dd = _mm256_div_pd(norm_pdf_pd(_mm256_add_pd(base, hv)), bv);
  };
  __m256d nextY[amer::kAlMaxNodes];
  auto do_sweep = [&](bool newton, __m256d &all_frozen_out) {
    __m256d maxdy = zero;
    __m256d n_movable = zero;
    __m256d n_frozen = zero;
    nextY[0] = Y[0];
    for (unsigned nodei = 1; nodei < nb; ++nodei) {
      const __m256d tau = TAU[nodei];
      const __m256d tau_gt = _mm256_cmp_pd(tau, TINY, _CMP_GT_OQ);
      const __m256d safe_tau = k_sel(tau_gt, tau, one);
      const __m256d b_val = b_from_y_pd(Y[nodei], XMAX);
      __m256d N, D;
      eqn_b_ND(nodei, safe_tau, b_val, N, D);
      const __m256d Dvalid = _mm256_cmp_pd(D, D300, _CMP_GT_OQ);
      const __m256d safeD = k_sel(Dvalid, D, one);
      const __m256d alpha =
          _mm256_mul_pd(Kv, exp_pd(_mm256_mul_pd(_mm256_sub_pd(Qv, Rv), safe_tau)));
      const __m256d f = _mm256_div_pd(_mm256_mul_pd(alpha, N), safeD);
      __m256d b_new;
      if (newton) {
        __m256d Nd, Dd;
        eqn_b_NDd(safe_tau, b_val, Nd, Dd);
        const __m256d fprime = _mm256_mul_pd(
            alpha, _mm256_sub_pd(_mm256_div_pd(Nd, safeD),
                                 _mm256_div_pd(_mm256_mul_pd(Dd, N), _mm256_mul_pd(safeD, safeD))));
        const __m256d denom = _mm256_sub_pd(fprime, one);
        const __m256d big = _mm256_cmp_pd(abs_pd(denom), DEN12, _CMP_GT_OQ);
        const __m256d newton_b =
            _mm256_sub_pd(b_val, _mm256_div_pd(_mm256_sub_pd(f, b_val), k_sel(big, denom, one)));
        b_new = k_sel(big, newton_b, f);
      } else {
        b_new = f;
      }
      b_new = _mm256_min_pd(b_new, XMAX);
      const __m256d bpos = _mm256_cmp_pd(b_new, zero, _CMP_GT_OQ);
      b_new = k_sel(bpos, b_new, _mm256_mul_pd(K1e6, Kv));
      const __m256d y_new = y_from_b_pd(b_new, XMAX);
      __m256d dy = abs_pd(_mm256_sub_pd(y_new, Y[nodei]));
      dy = _mm256_and_pd(dy, _mm256_and_pd(tau_gt, Dvalid));
      dy = _mm256_and_pd(dy, active);
      maxdy = _mm256_max_pd(maxdy, dy);
      n_movable = _mm256_add_pd(n_movable, _mm256_and_pd(one, tau_gt));
      n_frozen =
          _mm256_add_pd(n_frozen, _mm256_and_pd(one, _mm256_andnot_pd(Dvalid, tau_gt)));
      const __m256d val = k_sel(tau_gt, k_sel(Dvalid, y_new, Y[nodei]), zero);
      nextY[nodei] = k_sel(active, val, Y[nodei]);
    }
    for (unsigned j = 0; j < nb; ++j) {
      Y[j] = nextY[j];
    }
    all_frozen_out = _mm256_and_pd(_mm256_cmp_pd(n_movable, zero, _CMP_GT_OQ),
                                   _mm256_cmp_pd(n_frozen, n_movable, _CMP_EQ_OQ));
    return maxdy;
  };
  __m256d frozen_fail = zero;
  for (std::uint16_t k = 0; k < sch.n_iter_jn; ++k) {
    __m256d all_frozen = zero;
    const __m256d maxdy = do_sweep(/*newton=*/true, all_frozen);
    const __m256d froze_now = _mm256_and_pd(all_frozen, active);
    frozen_fail = _mm256_or_pd(frozen_fail, froze_now);
    active = _mm256_andnot_pd(froze_now, active);
    active = _mm256_andnot_pd(_mm256_cmp_pd(maxdy, TOL, _CMP_LE_OQ), active);
    if (_mm256_movemask_pd(active) == 0) {
      break;
    }
  }
  if (_mm256_movemask_pd(active) != 0) {
    for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
      __m256d all_frozen = zero;
      const __m256d maxdy = do_sweep(/*newton=*/false, all_frozen);
      const __m256d froze_now = _mm256_and_pd(all_frozen, active);
      frozen_fail = _mm256_or_pd(frozen_fail, froze_now);
      active = _mm256_andnot_pd(froze_now, active);
      active = _mm256_andnot_pd(_mm256_cmp_pd(maxdy, TOL, _CMP_LE_OQ), active);
      if (_mm256_movemask_pd(active) == 0) {
        break;
      }
    }
  }
  const int frozen_bits = _mm256_movemask_pd(frozen_fail);
  for (unsigned l = 0; l < 4; ++l) {
    if ((frozen_bits & (1 << l)) != 0) {
      eligible[l] = false;
    }
  }
}

} // namespace

unsigned solve_put_boundaries4_avx2(const double *K, double T, const double *sigma,
                                    const double *r, const double *q, const amer::AlScheme &sch,
                                    amer::AlBoundary *bnd, amer::AlWorkspace *ws,
                                    NodePackScratch &geo) noexcept {
  const double Tv[4] = {T, T, T, T};
  PutPackBoundary pack;
  bool eligible[4] = {false, false, false, false};
  int ref = -1;
  const bool hoist =
      sch.n_boundary <= NodePackScratch::kMaxNodes && sch.n_quad_fp <= NodePackScratch::kMaxQuad;
  // The kernel's spot argument only gates eligibility (S > 0); the boundary is
  // spot-independent, so the strikes stand in for it. With the hoisted sweeps the kernel
  // runs a zero sweep budget: init + seed only.
  amer::AlScheme seed_only = sch;
  if (hoist) {
    seed_only.n_iter_jn = 0;
    seed_only.n_iter_fp = 0;
  }
  solve_put_boundary_pack_avx2(K, K, Tv, sigma, r, q, /*n=*/4, seed_only, bnd, ws, pack, eligible,
                               ref);
  if (ref < 0) {
    return 0u;
  }
  if (hoist) {
    const amer::AlBoundary &rb = bnd[ref];
    __m256d TAU[amer::kAlMaxNodes];
    for (unsigned j = 0; j < pack.nb; ++j) {
      alignas(32) double tv[4] = {0.0, 0.0, 0.0, 0.0};
      for (unsigned l = 0; l < 4; ++l) {
        tv[l] = eligible[l] ? bnd[l].tau[j] : 0.0;
      }
      TAU[j] = _mm256_load_pd(tv);
    }
    hoist_geometry(pack, rb, ws[ref], geo);
    sweep_hoisted(pack, TAU, sch, geo, eligible);
  }
  unsigned mask = 0u;
  for (unsigned l = 0; l < 4; ++l) {
    mask |= eligible[l] ? (1u << l) : 0u;
  }
  alignas(32) double lane[4] = {0.0, 0.0, 0.0, 0.0};
  for (unsigned j = 0; j < pack.nb; ++j) {
    _mm256_store_pd(lane, pack.Y[j]);
    for (unsigned l = 0; l < 4; ++l) {
      if ((mask & (1u << l)) == 0u) {
        continue;
      }
      bnd[l].y[j] = lane[l];
      if (!std::isfinite(lane[l])) {
        mask &= ~(1u << l); // the scalar path decides this lane
      }
    }
  }
  for (unsigned l = 0; l < 4; ++l) {
    if ((mask & (1u << l)) != 0u) {
      // One boundary solve per lane, as the put batch counts its vector lanes, so the
      // solve ledger reads the same whichever route built the nodes.
      ATX_VOL_COUNT(BoundarySolves);
      counters::ledger::bump(counters::ledger::Solve::AlBoundarySolves);
    }
  }
  return mask;
}

} // namespace atx::vol::simd::detail
