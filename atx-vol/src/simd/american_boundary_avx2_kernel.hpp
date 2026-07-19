#pragma once

// AVX2 (4-lane f64) Andersen-Lake AMERICAN-PUT boundary primitives, shared by the
// boundary batch (K2, american_boundary_avx2.cpp) and the laned greeks bundle (K3,
// american_greeks_avx2.cpp). Built ONLY inside src/simd/*_avx2.cpp (-mavx2 -mfma).
// NOT a public header.
//
// The two primitives factor the K2 monolithic kernel so K3 rides its PROVEN solve +
// price code (the AvxBoundary + SchemeMapping parity tests validate this refactor as
// behavior-preserving):
//   * solve_put_boundary_pack_avx2 — init nodes + 4-wide BAW seed + lockstep JN/FP
//     sweeps for a pack of up-to-4 puts; leaves the converged dimensionless boundary
//     Y[] and the (params, geometry) it was solved at in a PutPackBoundary.
//   * price_put_pack_avx2 — euro (Black-76 put) + AL premium quadrature at an
//     ARBITRARY spot vector against a solved PutPackBoundary. The boundary is
//     spot-independent, so ONE solve serves every spot stencil (the K3 lever).
//
// The math is a line-for-line port of american_boundary_avx2.cpp's steps 1-4; the
// ONLY difference is the price spot became a parameter (was the contract's own S).

#include "../american_boundary.hpp" // amer:: seam (AlScheme/AlBoundary/al_init_put_boundary...)
#include "atx/vol/american.hpp"     // classify_regime, ExerciseRegime
#include "atx/vol/detail/vector_math.hpp" // log_pd/exp_pd/norm_cdf_erfc_pd2/norm_pdf_pd (+ __AVX2__ guard)

#include <cstddef>
#include <cmath>

#if !defined(__AVX2__) || !defined(__FMA__)
#  error "american_boundary_avx2_kernel.hpp requires -mavx2 -mfma (include only from src/simd/*_avx2.cpp)"
#endif

#include <immintrin.h>

namespace atx::vol::simd::detail {

using atx::vol::detail::exp_pd;
using atx::vol::detail::log_pd;
using atx::vol::detail::norm_cdf_erfc_pd2;
using atx::vol::detail::norm_pdf_pd;

namespace amer = atx::vol::amer;

// select: return t where mask's sign bit is set, else f (blendv order is (f,t,mask)).
ATX_FORCE_INLINE __m256d k_sel(__m256d mask, __m256d t, __m256d f) noexcept {
    return _mm256_blendv_pd(f, t, mask);
}

// The K2 BAW critical-price Newton cap (see american_boundary_avx2.cpp lever-1 audit:
// a static sub-16 cap breaks the economic parity gate — the movemask early-exit is the
// adaptive trim). Shared so both kernels seed identically.
inline constexpr int kBawSeedIters = 16;

// A converged 4-lane put boundary + the (params, geometry) it was solved at. The
// geometry pointers (znodes/wbary/qx_*) borrow from the caller-owned AlBoundary/
// AlWorkspace scratch and the static Gauss-Legendre tables, so that scratch must
// outlive every price_put_pack_avx2 call against this boundary.
struct PutPackBoundary {
    unsigned nb = 0, nq = 0, np = 0;
    const double* znodes = nullptr;   // scheme-fixed collocation nodes (all lanes)
    const double* wbary = nullptr;    // scheme-fixed barycentric weights
    const double* qx_price = nullptr; // premium GL nodes
    const double* qw_price = nullptr; // premium GL weights
    __m256d Y[amer::kAlMaxNodes];     // converged dimensionless boundary, per lane
    __m256d XMAX{};                   // K-dependent asymptotic level, per lane
    __m256d Kv{}, SIG{}, Rv{}, Qv{}, Tv{}, RmQ{};
    __m256d active{}; // lanes that were eligible (subset stays live through pricing)
};

// Solve a pack of up-to-4 puts. `bnd`/`ws` are caller-owned scratch (>=4 each) whose
// geometry `out` borrows. `eligible[l]` (out) is false for a degenerate / non-American
// / collapse lane the caller must patch to scalar; `ref` (out) is the first eligible
// lane index, or -1 if none (then `out` is not filled and the caller patches all 4).
// NOT force-inline: the K3 greeks kernel calls this 5x + price 13x per pack; forcing
// all 18 into one frame overflows the Debug stack. Plain `inline` (ODR across the two
// *_avx2.cpp TUs) lets Debug keep them as real calls with reused frames.
inline void solve_put_boundary_pack_avx2(
    const double* S_unused, const double* K, const double* T, const double* sigma,
    const double* r, const double* q, std::size_t n, const amer::AlScheme& sch,
    amer::AlBoundary* bnd, amer::AlWorkspace* ws, PutPackBoundary& out,
    bool eligible[4], int& ref) noexcept {
    (void)S_unused; // spot is not needed to solve the boundary (it is spot-independent)
    // ── 1. Per-lane node INIT ONLY (grid + quadrature, no seed) ───────
    ref = -1;
    for (std::size_t l = 0; l < 4; ++l) {
        eligible[l] = false;
    }
    for (std::size_t l = 0; l < n; ++l) {
        const bool degen = (T[l] <= 1.0e-12) || (sigma[l] <= 1.0e-8);
        const bool american =
            atx::vol::detail::classify_regime(r[l], q[l]) ==
            atx::vol::detail::ExerciseRegime::American;
        bool ok = !degen && american && std::isfinite(r[l]) && std::isfinite(q[l]) &&
                  K[l] > 0.0 && S_unused[l] > 0.0;
        if (ok) {
            const amer::AlSolveStatus st =
                amer::al_init_put_boundary(K[l], T[l], r[l], q[l], sch, bnd[l], ws[l]);
            ok = (st == amer::AlSolveStatus::Ok);
        }
        eligible[l] = ok;
        if (ok && ref < 0) {
            ref = static_cast<int>(l);
        }
    }
    if (ref < 0) {
        return;
    }

    // ── Broadcast constants ──────────────────────────────────────────
    const __m256d zero = _mm256_setzero_pd();
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d two = _mm256_set1_pd(2.0);
    const __m256d neg_one = _mm256_set1_pd(-1.0);
    const __m256d half = _mm256_set1_pd(0.5);
    const __m256d signmask = _mm256_set1_pd(-0.0);
    const __m256d TINY = _mm256_set1_pd(1.0e-14);
    const __m256d D300 = _mm256_set1_pd(1.0e-300);
    const __m256d DEN12 = _mm256_set1_pd(1.0e-12);
    const __m256d K1e6 = _mm256_set1_pd(1.0e-6);
    const __m256d TOL = _mm256_set1_pd(sch.tol);
    auto abs_pd = [&](__m256d x) { return _mm256_andnot_pd(signmask, x); };
    auto clamp01 = [&](__m256d x) {
        return _mm256_min_pd(_mm256_max_pd(x, neg_one), one);
    };

    // ── 2. Load the AoSoA<4> pack ────────────────────────────────────
    const unsigned nb = bnd[ref].n;
    const unsigned nq = ws[ref].n_quad_fp;
    const unsigned np = ws[ref].n_quad_price;
    out.nb = nb;
    out.nq = nq;
    out.np = np;
    out.znodes = bnd[ref].z.data();
    out.wbary = bnd[ref].wbary.data();
    out.qx_price = ws[ref].qx_price;
    out.qw_price = ws[ref].qw_price;
    const double* qx_fp = ws[ref].qx_fp;
    const double* qw_fp = ws[ref].qw_fp;

    __m256d* Y = out.Y;
    __m256d TAU[amer::kAlMaxNodes];
    for (unsigned j = 0; j < nb; ++j) {
        alignas(32) double yv[4] = {0, 0, 0, 0};
        alignas(32) double tv[4] = {0, 0, 0, 0};
        for (std::size_t l = 0; l < n; ++l) {
            yv[l] = eligible[l] ? bnd[l].y[j] : 0.0;
            tv[l] = eligible[l] ? bnd[l].tau[j] : 0.0;
        }
        Y[j] = _mm256_load_pd(yv);
        TAU[j] = _mm256_load_pd(tv);
    }

    alignas(32) double xmv[4] = {1, 1, 1, 1}, kv[4] = {1, 1, 1, 1}, sv[4] = {0, 0, 0, 0};
    alignas(32) double rv[4] = {0, 0, 0, 0}, qv[4] = {0, 0, 0, 0}, tvv[4] = {0, 0, 0, 0};
    alignas(32) double av[4] = {0, 0, 0, 0};
    for (std::size_t l = 0; l < n; ++l) {
        xmv[l] = eligible[l] ? bnd[l].xmax : 1.0;
        kv[l] = K[l];
        sv[l] = sigma[l];
        rv[l] = r[l];
        qv[l] = q[l];
        tvv[l] = T[l];
        av[l] = eligible[l] ? 1.0 : 0.0;
    }
    const __m256d XMAX = _mm256_load_pd(xmv);
    const __m256d Kv = _mm256_load_pd(kv);
    const __m256d SIG = _mm256_load_pd(sv);
    const __m256d Rv = _mm256_load_pd(rv);
    const __m256d Qv = _mm256_load_pd(qv);
    const __m256d Tv = _mm256_load_pd(tvv);
    const __m256d RmQ = _mm256_sub_pd(Rv, Qv);
    __m256d active = _mm256_cmp_pd(_mm256_load_pd(av), half, _CMP_GT_OQ);

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

    // ── 2.5 Vector Barone-Adesi-Whaley critical-price seed ──
    {
        const __m256d ALL = _mm256_castsi256_pd(_mm256_set1_epi64x(-1));
        auto notm = [&](__m256d m) { return _mm256_andnot_pd(m, ALL); };
        const __m256d sig2 = _mm256_mul_pd(SIG, SIG);
        const __m256d Mv = _mm256_div_pd(_mm256_mul_pd(two, Rv), sig2);
        const __m256d Nv = _mm256_div_pd(_mm256_mul_pd(two, RmQ), sig2);
        const __m256d Nm1 = _mm256_sub_pd(Nv, one);
        const __m256d four = _mm256_set1_pd(4.0);
        const __m256d loK = _mm256_mul_pd(_mm256_set1_pd(1.0e-3), Kv);
        const __m256d hiK = _mm256_mul_pd(_mm256_set1_pd(1.0 - 1.0e-6), Kv);
        const __m256d stolK = _mm256_mul_pd(_mm256_set1_pd(1.0e-10), Kv);
        const __m256d fpEPS = _mm256_set1_pd(1.0e-15);
        const __m256d fb03 = _mm256_set1_pd(0.3);
        const __m256d rpos = _mm256_cmp_pd(Rv, zero, _CMP_GT_OQ);
        auto res_deriv = [&](__m256d Sx, __m256d tau, __m256d q1, __m256d& f, __m256d& fp) {
            const __m256d v = _mm256_mul_pd(SIG, _mm256_sqrt_pd(tau));
            const __m256d F = _mm256_mul_pd(Sx, exp_pd(_mm256_mul_pd(RmQ, tau)));
            const __m256d df = exp_pd(_mm256_sub_pd(zero, _mm256_mul_pd(Rv, tau)));
            const __m256d dq = exp_pd(_mm256_sub_pd(zero, _mm256_mul_pd(Qv, tau)));
            const __m256d d1 = _mm256_div_pd(
                _mm256_add_pd(log_pd(_mm256_div_pd(F, Kv)),
                              _mm256_mul_pd(half, _mm256_mul_pd(v, v))),
                v);
            const __m256d d2 = _mm256_sub_pd(d1, v);
            __m256d Nnd1, Nnd2;
            norm_cdf_erfc_pd2(_mm256_sub_pd(zero, d1), _mm256_sub_pd(zero, d2), Nnd1, Nnd2);
            const __m256d phim = norm_pdf_pd(_mm256_sub_pd(zero, d1));
            const __m256d pE = _mm256_mul_pd(
                df, _mm256_sub_pd(_mm256_mul_pd(Kv, Nnd2), _mm256_mul_pd(F, Nnd1)));
            const __m256d dqN = _mm256_mul_pd(dq, Nnd1);
            const __m256d bit = _mm256_sub_pd(one, dqN);
            f = _mm256_add_pd(_mm256_sub_pd(_mm256_sub_pd(Kv, Sx), pE),
                              _mm256_div_pd(_mm256_mul_pd(Sx, bit), q1));
            fp = _mm256_add_pd(
                _mm256_add_pd(neg_one, dqN),
                _mm256_sub_pd(_mm256_div_pd(bit, q1),
                              _mm256_div_pd(_mm256_mul_pd(dq, phim),
                                            _mm256_mul_pd(q1, v))));
        };
        for (unsigned nodei = 1; nodei < nb; ++nodei) {
            const __m256d tau = TAU[nodei];
            const __m256d tau_gt = _mm256_cmp_pd(tau, TINY, _CMP_GT_OQ);
            const __m256d h =
                _mm256_sub_pd(one, exp_pd(_mm256_sub_pd(zero, _mm256_mul_pd(Rv, tau))));
            const __m256d hpos = _mm256_cmp_pd(h, zero, _CMP_GT_OQ);
            const __m256d disc = _mm256_add_pd(
                _mm256_mul_pd(Nm1, Nm1),
                _mm256_div_pd(_mm256_mul_pd(four, Mv), k_sel(hpos, h, one)));
            const __m256d dpos = _mm256_cmp_pd(disc, zero, _CMP_GE_OQ);
            const __m256d sqrt_disc = _mm256_sqrt_pd(_mm256_max_pd(disc, zero));
            const __m256d q1 = _mm256_mul_pd(
                half, _mm256_sub_pd(_mm256_sub_pd(zero, Nm1), sqrt_disc));
            const __m256d nl = _mm256_and_pd(_mm256_and_pd(tau_gt, rpos),
                                             _mm256_and_pd(hpos, dpos));
            const __m256d Sx0 = _mm256_div_pd(_mm256_mul_pd(Kv, q1), _mm256_sub_pd(q1, one));
            const __m256d mid0 = _mm256_mul_pd(half, _mm256_add_pd(loK, hiK));
            const __m256d in_br = _mm256_and_pd(_mm256_cmp_pd(Sx0, loK, _CMP_GT_OQ),
                                                _mm256_cmp_pd(Sx0, hiK, _CMP_LT_OQ));
            __m256d Sx = k_sel(nl, k_sel(in_br, Sx0, mid0), Kv);
            __m256d lo = loK;
            __m256d hi = hiK;
            __m256d done = notm(nl);
            for (int it = 0; it < kBawSeedIters; ++it) {
                if (_mm256_movemask_pd(done) == 0xF) {
                    break;
                }
                __m256d f, fp;
                res_deriv(Sx, tau, q1, f, fp);
                const __m256d act = notm(done);
                const __m256d fpos = _mm256_cmp_pd(f, zero, _CMP_GT_OQ);
                lo = k_sel(_mm256_and_pd(act, fpos), Sx, lo);
                hi = k_sel(_mm256_and_pd(act, notm(fpos)), Sx, hi);
                const __m256d mid = _mm256_mul_pd(half, _mm256_add_pd(lo, hi));
                const __m256d conv_f =
                    _mm256_and_pd(act, _mm256_cmp_pd(abs_pd(f), stolK, _CMP_LT_OQ));
                const __m256d fp_ok = _mm256_cmp_pd(abs_pd(fp), fpEPS, _CMP_GT_OQ);
                __m256d Sxn = k_sel(fp_ok, _mm256_sub_pd(Sx, _mm256_div_pd(f, fp)), mid);
                const __m256d outb = _mm256_or_pd(_mm256_cmp_pd(Sxn, lo, _CMP_LE_OQ),
                                                  _mm256_cmp_pd(Sxn, hi, _CMP_GE_OQ));
                Sxn = k_sel(outb, mid, Sxn);
                const __m256d dS = _mm256_sub_pd(Sxn, Sx);
                const __m256d conv_dS = _mm256_and_pd(
                    _mm256_and_pd(act, notm(conv_f)),
                    _mm256_cmp_pd(abs_pd(dS), stolK, _CMP_LT_OQ));
                Sx = k_sel(_mm256_and_pd(act, notm(conv_f)), Sxn, Sx);
                done = _mm256_or_pd(done, _mm256_or_pd(conv_f, conv_dS));
            }
            const __m256d baw_ok = _mm256_and_pd(_mm256_cmp_pd(Sx, zero, _CMP_GT_OQ),
                                                 _mm256_cmp_pd(Sx, Kv, _CMP_LE_OQ));
            const __m256d Sfb = _mm256_mul_pd(
                Kv, _mm256_sub_pd(one, _mm256_mul_pd(
                                           fb03, _mm256_sqrt_pd(_mm256_div_pd(tau, Tv)))));
            Sx = k_sel(baw_ok, Sx, Sfb);
            Sx = _mm256_min_pd(Sx, XMAX);
            Sx = k_sel(_mm256_cmp_pd(Sx, zero, _CMP_GT_OQ), Sx, _mm256_mul_pd(K1e6, Kv));
            Y[nodei] = k_sel(tau_gt, y_from_b_pd(Sx, XMAX), zero);
        }
    }

    auto cheb_eval = [&](__m256d zc) {
        __m256d num = zero;
        __m256d den = zero;
        for (unsigned j = 0; j < nb; ++j) {
            const __m256d dz = _mm256_sub_pd(zc, _mm256_set1_pd(out.znodes[j]));
            const __m256d qq = _mm256_div_pd(_mm256_set1_pd(out.wbary[j]), dz);
            num = _mm256_add_pd(num, _mm256_mul_pd(qq, Y[j]));
            den = _mm256_add_pd(den, qq);
        }
        return _mm256_div_pd(num, den);
    };
    auto eqn_b_ND = [&](__m256d tau, __m256d b_val, __m256d& N, __m256d& D) {
        const __m256d v_tip = _mm256_mul_pd(SIG, _mm256_sqrt_pd(tau));
        const __m256d base_tip = _mm256_div_pd(
            _mm256_add_pd(log_pd(_mm256_div_pd(b_val, Kv)), _mm256_mul_pd(RmQ, tau)),
            v_tip);
        const __m256d hv_tip = _mm256_mul_pd(half, v_tip);
        const __m256d dpv_tip = _mm256_add_pd(base_tip, hv_tip);
        const __m256d dmv_tip = _mm256_sub_pd(base_tip, hv_tip);
        __m256d tip_p, tip_m;
        norm_cdf_erfc_pd2(dpv_tip, dmv_tip, tip_p, tip_m);
        const __m256d half_tau = _mm256_mul_pd(half, tau);
        __m256d n_int = zero;
        __m256d d_int = zero;
        for (unsigned qi = 0; qi < nq; ++qi) {
            const __m256d xs = _mm256_set1_pd(qx_fp[qi]);
            const __m256d wv = _mm256_set1_pd(qw_fp[qi]);
            const __m256d u = _mm256_mul_pd(half_tau, _mm256_add_pd(one, xs));
            const __m256d t_u = _mm256_sub_pd(tau, u);
            const __m256d tu_gt = _mm256_cmp_pd(t_u, TINY, _CMP_GT_OQ);
            const __m256d u_eff = _mm256_min_pd(u, Tv);
            const __m256d zz = _mm256_sub_pd(
                _mm256_mul_pd(two, _mm256_sqrt_pd(_mm256_div_pd(u_eff, Tv))), one);
            const __m256d bu = b_from_y_pd(cheb_eval(clamp01(zz)), XMAX);
            const __m256d bu_gt = _mm256_cmp_pd(bu, zero, _CMP_GT_OQ);
            const __m256d qact = _mm256_and_pd(tu_gt, bu_gt);
            const __m256d safe_tu = k_sel(tu_gt, t_u, one);
            const __m256d z = _mm256_div_pd(b_val, k_sel(bu_gt, bu, one));
            const __m256d vq = _mm256_mul_pd(SIG, _mm256_sqrt_pd(safe_tu));
            const __m256d base = _mm256_div_pd(
                _mm256_add_pd(log_pd(z), _mm256_mul_pd(RmQ, safe_tu)), vq);
            const __m256d hvq = _mm256_mul_pd(half, vq);
            const __m256d dpv = _mm256_add_pd(base, hvq);
            const __m256d dmv = _mm256_sub_pd(base, hvq);
            __m256d ncdf_p, ncdf_m;
            norm_cdf_erfc_pd2(dpv, dmv, ncdf_p, ncdf_m);
            __m256d term_n = _mm256_mul_pd(
                _mm256_mul_pd(wv, exp_pd(_mm256_mul_pd(Rv, u))), ncdf_m);
            __m256d term_d = _mm256_mul_pd(
                _mm256_mul_pd(wv, exp_pd(_mm256_mul_pd(Qv, u))), ncdf_p);
            n_int = _mm256_add_pd(n_int, _mm256_and_pd(term_n, qact));
            d_int = _mm256_add_pd(d_int, _mm256_and_pd(term_d, qact));
        }
        N = _mm256_add_pd(tip_m, _mm256_mul_pd(Rv, _mm256_mul_pd(n_int, half_tau)));
        D = _mm256_add_pd(tip_p, _mm256_mul_pd(Qv, _mm256_mul_pd(d_int, half_tau)));
    };
    auto eqn_b_NDd = [&](__m256d tau, __m256d b_val, __m256d& Nd, __m256d& Dd) {
        const __m256d v = _mm256_mul_pd(SIG, _mm256_sqrt_pd(tau));
        const __m256d base = _mm256_div_pd(
            _mm256_add_pd(log_pd(_mm256_div_pd(b_val, Kv)), _mm256_mul_pd(RmQ, tau)), v);
        const __m256d hv = _mm256_mul_pd(half, v);
        const __m256d bv = _mm256_mul_pd(b_val, v);
        Nd = _mm256_div_pd(norm_pdf_pd(_mm256_sub_pd(base, hv)), bv);
        Dd = _mm256_div_pd(norm_pdf_pd(_mm256_add_pd(base, hv)), bv);
    };

    // ── 3. Vector sweeps: one JN/FP pass → per-lane max|Δy| ───────────
    __m256d nextY[amer::kAlMaxNodes];
    auto do_sweep = [&](bool newton) {
        __m256d maxdy = zero;
        nextY[0] = Y[0];
        for (unsigned nodei = 1; nodei < nb; ++nodei) {
            const __m256d tau = TAU[nodei];
            const __m256d tau_gt = _mm256_cmp_pd(tau, TINY, _CMP_GT_OQ);
            const __m256d safe_tau = k_sel(tau_gt, tau, one);
            const __m256d b_val = b_from_y_pd(Y[nodei], XMAX);
            __m256d N, D;
            eqn_b_ND(safe_tau, b_val, N, D);
            const __m256d Dvalid = _mm256_cmp_pd(D, D300, _CMP_GT_OQ);
            const __m256d safeD = k_sel(Dvalid, D, one);
            const __m256d alpha = _mm256_mul_pd(
                Kv, exp_pd(_mm256_mul_pd(_mm256_sub_pd(Qv, Rv), safe_tau)));
            const __m256d f = _mm256_div_pd(_mm256_mul_pd(alpha, N), safeD);
            __m256d b_new;
            if (newton) {
                __m256d Nd, Dd;
                eqn_b_NDd(safe_tau, b_val, Nd, Dd);
                const __m256d fprime = _mm256_mul_pd(
                    alpha,
                    _mm256_sub_pd(
                        _mm256_div_pd(Nd, safeD),
                        _mm256_div_pd(_mm256_mul_pd(Dd, N),
                                      _mm256_mul_pd(safeD, safeD))));
                const __m256d denom = _mm256_sub_pd(fprime, one);
                const __m256d big = _mm256_cmp_pd(abs_pd(denom), DEN12, _CMP_GT_OQ);
                const __m256d newton_b = _mm256_sub_pd(
                    b_val, _mm256_div_pd(_mm256_sub_pd(f, b_val), k_sel(big, denom, one)));
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
            const __m256d val = k_sel(tau_gt, k_sel(Dvalid, y_new, Y[nodei]), zero);
            nextY[nodei] = k_sel(active, val, Y[nodei]);
        }
        for (unsigned j = 0; j < nb; ++j) {
            Y[j] = nextY[j];
        }
        return maxdy;
    };

    for (std::uint16_t k = 0; k < sch.n_iter_jn; ++k) {
        const __m256d maxdy = do_sweep(/*newton=*/true);
        active = _mm256_andnot_pd(_mm256_cmp_pd(maxdy, TOL, _CMP_LE_OQ), active);
        if (_mm256_movemask_pd(active) == 0) {
            break;
        }
    }
    if (_mm256_movemask_pd(active) != 0) {
        for (std::uint16_t k = 0; k < sch.n_iter_fp; ++k) {
            const __m256d maxdy = do_sweep(/*newton=*/false);
            active = _mm256_andnot_pd(_mm256_cmp_pd(maxdy, TOL, _CMP_LE_OQ), active);
            if (_mm256_movemask_pd(active) == 0) {
                break;
            }
        }
    }

    out.XMAX = XMAX;
    out.Kv = Kv;
    out.SIG = SIG;
    out.Rv = Rv;
    out.Qv = Qv;
    out.Tv = Tv;
    out.RmQ = RmQ;
    // `active` at this point marks lanes still iterating; every eligible lane (whether
    // converged early or not) carries a valid boundary. Re-derive the eligible mask.
    alignas(32) double elig[4] = {0, 0, 0, 0};
    for (std::size_t l = 0; l < n; ++l) {
        elig[l] = eligible[l] ? 1.0 : 0.0;
    }
    out.active = _mm256_cmp_pd(_mm256_load_pd(elig), _mm256_set1_pd(0.5), _CMP_GT_OQ);
}

// Price a pack against the solved boundary `b` at an arbitrary (`spot`, `strike`,
// `xmax`) triple. Returns the 4-wide American put price (euro floor + intrinsic +
// non-negativity clamps), a line-for-line port of the K2 kernel's step 4 with S,
// K, and xmax all generalised to explicit vectors.
//
// The put-greeks kernel prices spot stencils against the FIXED contract strike
// (b.Kv) and xmax (b.XMAX) — see the price_put_pack_avx2 wrapper below. The
// call-greeks kernel exploits the McDonald-Schroder map C(S,K,r,q)=P(K,S,q,r): a
// call spot stencil is priced as the internal put at internal-spot=K_call (fixed),
// internal-strike=S_stencil (the varying call spot), and xmax rescaled by strike
// homogeneity (al_xmax_put is linear in strike, so xmax(S2)=XMAX_base·S2/S_base is
// exact math; ~1 ULP from the scalar al_xmax_put(S2,q,r) direct evaluation, deep
// inside the economic gate). One solved boundary therefore serves every stencil on
// EITHER side — the K3 lever, unchanged.
inline __m256d price_put_pack_at_avx2(const PutPackBoundary& b, __m256d spot,
                                      __m256d strike, __m256d xmax) noexcept {
    const __m256d zero = _mm256_setzero_pd();
    const __m256d one = _mm256_set1_pd(1.0);
    const __m256d two = _mm256_set1_pd(2.0);
    const __m256d neg_one = _mm256_set1_pd(-1.0);
    const __m256d half = _mm256_set1_pd(0.5);
    const __m256d TINY = _mm256_set1_pd(1.0e-14);
    const __m256d Kv = strike, SIG = b.SIG, Rv = b.Rv, Qv = b.Qv, Tv = b.Tv, RmQ = b.RmQ;
    const __m256d XMAX = xmax;
    const unsigned nb = b.nb, np = b.np;
    const __m256d* Y = b.Y;

    auto clamp01 = [&](__m256d x) {
        return _mm256_min_pd(_mm256_max_pd(x, neg_one), one);
    };
    auto b_from_y_pd = [&](__m256d y, __m256d xmax) {
        const __m256d yv2 = _mm256_max_pd(y, zero);
        return _mm256_mul_pd(xmax, exp_pd(_mm256_sub_pd(zero, _mm256_sqrt_pd(yv2))));
    };
    auto cheb_eval = [&](__m256d zc) {
        __m256d num = zero;
        __m256d den = zero;
        for (unsigned j = 0; j < nb; ++j) {
            const __m256d dz = _mm256_sub_pd(zc, _mm256_set1_pd(b.znodes[j]));
            const __m256d qq = _mm256_div_pd(_mm256_set1_pd(b.wbary[j]), dz);
            num = _mm256_add_pd(num, _mm256_mul_pd(qq, Y[j]));
            den = _mm256_add_pd(den, qq);
        }
        return _mm256_div_pd(num, den);
    };

    const __m256d Sv = spot;
    const __m256d F = _mm256_mul_pd(Sv, exp_pd(_mm256_mul_pd(RmQ, Tv)));
    const __m256d df = exp_pd(_mm256_sub_pd(zero, _mm256_mul_pd(Rv, Tv)));
    const __m256d vT = _mm256_mul_pd(SIG, _mm256_sqrt_pd(Tv));
    const __m256d d1 = _mm256_div_pd(
        _mm256_add_pd(log_pd(_mm256_div_pd(F, Kv)),
                      _mm256_mul_pd(half, _mm256_mul_pd(vT, vT))),
        vT);
    const __m256d d2 = _mm256_sub_pd(d1, vT);
    __m256d Nd1, Nd2;
    norm_cdf_erfc_pd2(d1, d2, Nd1, Nd2);
    const __m256d euro = _mm256_mul_pd(
        df, _mm256_sub_pd(_mm256_mul_pd(Kv, _mm256_sub_pd(one, Nd2)),
                          _mm256_mul_pd(F, _mm256_sub_pd(one, Nd1))));

    const __m256d sqrtT = _mm256_sqrt_pd(Tv);
    const __m256d half_sqrtT = _mm256_mul_pd(half, sqrtT);
    __m256d total = zero;
    for (unsigned pi = 0; pi < np; ++pi) {
        const __m256d xs = _mm256_set1_pd(b.qx_price[pi]);
        const __m256d wv = _mm256_set1_pd(b.qw_price[pi]);
        const __m256d zi = _mm256_mul_pd(half_sqrtT, _mm256_add_pd(one, xs));
        const __m256d t = _mm256_mul_pd(zi, zi);
        const __m256d t_gt = _mm256_cmp_pd(t, TINY, _CMP_GT_OQ);
        const __m256d rem = _mm256_sub_pd(Tv, t);
        const __m256d rem_gt = _mm256_cmp_pd(rem, zero, _CMP_GT_OQ);
        const __m256d u_eff = _mm256_min_pd(k_sel(rem_gt, rem, Tv), Tv);
        const __m256d zz = _mm256_sub_pd(
            _mm256_mul_pd(two, _mm256_sqrt_pd(_mm256_div_pd(u_eff, Tv))), one);
        const __m256d bnd_at = b_from_y_pd(cheb_eval(clamp01(zz)), XMAX);
        const __m256d b_t = k_sel(rem_gt, bnd_at, Kv);
        const __m256d bt_gt = _mm256_cmp_pd(b_t, zero, _CMP_GT_OQ);
        const __m256d iact = _mm256_and_pd(t_gt, bt_gt);
        const __m256d v = _mm256_mul_pd(SIG, _mm256_sqrt_pd(k_sel(t_gt, t, one)));
        const __m256d dq = exp_pd(_mm256_sub_pd(zero, _mm256_mul_pd(Qv, t)));
        const __m256d dr = exp_pd(_mm256_sub_pd(zero, _mm256_mul_pd(Rv, t)));
        const __m256d ratio = _mm256_div_pd(_mm256_mul_pd(Sv, dq),
                                            _mm256_mul_pd(k_sel(bt_gt, b_t, one), dr));
        const __m256d dp = _mm256_add_pd(_mm256_div_pd(log_pd(ratio), v),
                                         _mm256_mul_pd(half, v));
        const __m256d arg1 = _mm256_add_pd(_mm256_sub_pd(zero, dp), v);
        const __m256d arg2 = _mm256_sub_pd(zero, dp);
        __m256d P1, P2;
        norm_cdf_erfc_pd2(arg1, arg2, P1, P2);
        const __m256d termA = _mm256_mul_pd(_mm256_mul_pd(Rv, Kv), _mm256_mul_pd(dr, P1));
        const __m256d termB = _mm256_mul_pd(_mm256_mul_pd(Qv, Sv), _mm256_mul_pd(dq, P2));
        __m256d integ = _mm256_mul_pd(_mm256_mul_pd(two, zi), _mm256_sub_pd(termA, termB));
        integ = _mm256_and_pd(integ, iact);
        total = _mm256_add_pd(total, _mm256_mul_pd(wv, integ));
    }
    const __m256d prem = _mm256_max_pd(_mm256_mul_pd(total, half_sqrtT), zero);
    __m256d price = _mm256_add_pd(euro, prem);
    price = _mm256_max_pd(price, _mm256_sub_pd(Kv, Sv)); // intrinsic
    price = _mm256_max_pd(price, euro);                  // euro floor
    price = _mm256_max_pd(price, zero);
    return price;
}

// Put spot-stencil pricer: the contract strike (b.Kv) and asymptotic level (b.XMAX)
// are FIXED; only the spot varies. Bit-identical to the pre-refactor kernel (same ops,
// same operands) — the K2/K3 put path is unchanged.
inline __m256d price_put_pack_avx2(const PutPackBoundary& b, __m256d spot) noexcept {
    return price_put_pack_at_avx2(b, spot, b.Kv, b.XMAX);
}

} // namespace atx::vol::simd::detail
