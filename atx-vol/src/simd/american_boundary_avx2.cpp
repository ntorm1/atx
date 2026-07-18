// AVX2 (4-lane f64) batched Andersen-Lake AMERICAN-PUT boundary solve + price.
//
// Built with -mavx2 -mfma (src/simd/*_avx2.cpp CMake glob). Called only when the
// dispatch layer confirms use_avx2() at runtime. P3.2 greenfield: this is the
// FIRST vectorization of the ITERATIVE AL exercise-boundary solver — 4 INDEPENDENT
// puts (AoSoA<4>) run their Jacobi-Newton + fixed-point sweeps and premium
// quadrature in lockstep, sharing the hand-tuned AVX2 transcendentals in
// detail/vector_math.hpp. Mirrors the scalar generic kernel (american.cpp
// al_jn_sweep_impl / al_fp_sweep_impl / eqn_b_ND_impl <0,0> + al_put_premium +
// al_put_price_from_boundary) per lane, so the ONLY Reference-vs-AVX2 gap is the
// vector transcendentals (log/exp/Φ/φ).
//
// Design decisions (see at-task-13 brief):
//   * The BAW seed stays SCALAR and per-lane — it is the exact cold reference
//     seed (al_solve_put_boundary with the sweep budget zeroed), so the pack
//     starts from the bit-identical y[] the scalar solver would. Only the
//     transcendental-bound sweep + premium are vectorized.
//   * An ACTIVE MASK is maintained across sweeps: a lane that hits the residual
//     tol is frozen (never updated again), reproducing the scalar per-solve early
//     exit; frozen/ineligible lanes never corrupt live lanes (all ops lane-wise).
//   * Each lane keeps its own quadrature reduction order (serial add over the
//     shared GL nodes), matching the scalar accumulation order per lane.
//   * PATCH lanes to the exact scalar andersen_lake — degenerate (T≤1e-12 ∨
//     σ≤1e-8), non-American regime, boundary-collapse, and any non-finite
//     result — exactly the idiom every *_batch_avx2.cpp uses for its scalar tail.
//     A4 [S1]: Φ is now the full-range Cody rational-erfc norm_cdf_erfc_pd2
//     (~1e-16 across the whole line, saturating to exactly 1.0/0.0 in the deep
//     wings), so NO deep-wing (|d|>kNormCdfWing) patch is needed — the earlier
//     degree-48 Chebyshev-Φ, accurate only on |x|≤~7, is no longer used here and
//     the boundary path is single-source with the scalar andersen_lake's erfc Φ.

#include "american_boundary_avx2.hpp"

#include "../american_boundary.hpp" // amer:: seam (AlScheme/AlBoundary/al_solve_put_boundary...)
#include "atx/vol/american.hpp"     // andersen_lake, classify_regime, ExerciseRegime
#include "atx/vol/detail/vector_math.hpp" // log_pd/exp_pd/norm_cdf_erfc_pd2/norm_pdf_pd (+ __AVX2__ guard)

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

#if !defined(__AVX2__) || !defined(__FMA__)
#  error "american_boundary_avx2.cpp requires -mavx2 -mfma (build via src/simd/*_avx2.cpp)"
#endif

#include <immintrin.h>

namespace atx::vol::simd::detail {

using atx::vol::detail::exp_pd;
using atx::vol::detail::log_pd;
using atx::vol::detail::norm_cdf_erfc_pd2;
using atx::vol::detail::norm_pdf_pd;

namespace {

namespace amer = atx::vol::amer;

// select: return t where mask's sign bit is set, else f (blendv order is (f,t,mask)).
ATX_FORCE_INLINE __m256d sel(__m256d mask, __m256d t, __m256d f) noexcept {
    return _mm256_blendv_pd(f, t, mask);
}

// Scalar cold reference for a single put lane (the patch target).
[[nodiscard]] double scalar_put(double S, double K, double T, double sigma,
                                double r, double q,
                                const std::optional<AlOpts>& opts) noexcept {
    const Result<double> res = andersen_lake(S, K, T, sigma, r, q, Side::Put, opts);
    return res.has_value() ? *res : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

void american_put_boundary_batch_avx2(const double* S, const double* K,
                                      const double* T, const double* sigma,
                                      const double* r, const double* q,
                                      double* price_out, std::size_t n,
                                      const std::optional<AlOpts>& opts) noexcept {
    const amer::AlScheme sch = amer::scheme_from_opts(opts);
    // A4 [S1] accuracy-improving: Φ(d1),Φ(d2) come from the full-range Cody
    // rational-erfc norm_cdf_erfc_pd2 (vector_math.hpp) instead of the degree-48
    // Chebyshev norm_cdf_pd2 that clamped |x|>7 and needed a Φ-coefficient table.
    // Numerically Φ improves from ~1e-11 interior / clamped wings to ~1e-16 full
    // range; the boundary PRICE stays within the economic parity gate (kNormalGate
    // 1e-6, stress 1e-3) — the swept price output moved by ≤ a few 1e-9 (see
    // AvxBoundary parity tests) — and the AVX2 Φ is now single-source with the
    // scalar andersen_lake's erfc Φ, the prerequisite for A1's de-Am vectorization.

    // ── Broadcast constants ──────────────────────────────────────────────
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

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        // ── 1. Per-lane SCALAR seed (init nodes + BAW seed, no sweeps) ────
        // Task A1: the seed lays down node geometry + quadrature pointers + the cold
        // BAW y[] via al_seed_put_boundary, which SKIPS al_bind_geometry — the
        // sweep-invariant geometry precompute this kernel never reads (it recomputes
        // every geometry term inline below). Dropping that per-lane bind removes the
        // dominant scalar serialization on the seed while leaving the seed y[]
        // bit-identical, so the vectorized sweeps + price are unchanged (parity held).
        amer::AlBoundary bnd[4];
        amer::AlWorkspace ws[4];
        bool eligible[4];
        int ref = -1;
        for (int l = 0; l < 4; ++l) {
            const std::size_t idx = i + static_cast<std::size_t>(l);
            const bool degen = (T[idx] <= 1.0e-12) || (sigma[idx] <= 1.0e-8);
            const bool american =
                atx::vol::detail::classify_regime(r[idx], q[idx]) ==
                atx::vol::detail::ExerciseRegime::American;
            bool ok = !degen && american && std::isfinite(r[idx]) &&
                      std::isfinite(q[idx]) && K[idx] > 0.0 && S[idx] > 0.0;
            if (ok) {
                const amer::AlSolveStatus st = amer::al_seed_put_boundary(
                    K[idx], T[idx], sigma[idx], r[idx], q[idx], sch, bnd[l], ws[l]);
                ok = (st == amer::AlSolveStatus::Ok);
            }
            eligible[l] = ok;
            if (ok && ref < 0) {
                ref = l;
            }
        }

        // All four lanes ineligible → nothing to vectorize; patch each.
        if (ref < 0) {
            for (int l = 0; l < 4; ++l) {
                const std::size_t idx = i + static_cast<std::size_t>(l);
                price_out[idx] =
                    scalar_put(S[idx], K[idx], T[idx], sigma[idx], r[idx], q[idx], opts);
            }
            continue;
        }

        // ── 2. Load the AoSoA<4> pack ────────────────────────────────────
        const unsigned nb = bnd[ref].n;                  // boundary collocation nodes
        const unsigned nq = ws[ref].n_quad_fp;           // fixed-point GL order
        const unsigned np = ws[ref].n_quad_price;        // premium GL order
        const double* znodes = bnd[ref].z.data();        // scheme-fixed (same all lanes)
        const double* wbary = bnd[ref].wbary.data();     // scheme-fixed
        const double* qx_fp = ws[ref].qx_fp;
        const double* qw_fp = ws[ref].qw_fp;
        const double* qx_price = ws[ref].qx_price;
        const double* qw_price = ws[ref].qw_price;

        __m256d Y[amer::kAlMaxNodes];
        __m256d TAU[amer::kAlMaxNodes];
        for (unsigned j = 0; j < nb; ++j) {
            alignas(32) double yv[4];
            alignas(32) double tv[4];
            for (int l = 0; l < 4; ++l) {
                yv[l] = eligible[l] ? bnd[l].y[j] : 0.0;
                tv[l] = eligible[l] ? bnd[l].tau[j] : 0.0;
            }
            Y[j] = _mm256_load_pd(yv);
            TAU[j] = _mm256_load_pd(tv);
        }

        alignas(32) double xmv[4], kv[4], sv[4], rv[4], qv[4], tvv[4], spv[4], av[4];
        for (int l = 0; l < 4; ++l) {
            const std::size_t idx = i + static_cast<std::size_t>(l);
            xmv[l] = eligible[l] ? bnd[l].xmax : 1.0;
            kv[l] = K[idx];
            sv[l] = sigma[idx];
            rv[l] = r[idx];
            qv[l] = q[idx];
            tvv[l] = T[idx];
            spv[l] = S[idx];
            av[l] = eligible[l] ? 1.0 : 0.0;
        }
        const __m256d XMAX = _mm256_load_pd(xmv);
        const __m256d Kv = _mm256_load_pd(kv);
        const __m256d SIG = _mm256_load_pd(sv);
        const __m256d Rv = _mm256_load_pd(rv);
        const __m256d Qv = _mm256_load_pd(qv);
        const __m256d Tv = _mm256_load_pd(tvv);
        const __m256d Sv = _mm256_load_pd(spv);
        const __m256d RmQ = _mm256_sub_pd(Rv, Qv);
        __m256d active = _mm256_cmp_pd(_mm256_load_pd(av), half, _CMP_GT_OQ);

        // ── boundary math helpers (lane-wise, mirror american.cpp) ───────
        auto b_from_y_pd = [&](__m256d y, __m256d xmax) {
            const __m256d yv2 = _mm256_max_pd(y, zero);
            return _mm256_mul_pd(
                xmax, exp_pd(_mm256_sub_pd(zero, _mm256_sqrt_pd(yv2))));
        };
        auto y_from_b_pd = [&](__m256d b, __m256d xmax) {
            const __m256d ok =
                _mm256_and_pd(_mm256_cmp_pd(b, zero, _CMP_GT_OQ),
                              _mm256_cmp_pd(xmax, zero, _CMP_GT_OQ));
            const __m256d lg =
                log_pd(_mm256_div_pd(sel(ok, b, one), sel(ok, xmax, one)));
            return _mm256_and_pd(_mm256_mul_pd(lg, lg), ok);
        };
        // Barycentric cheb interp of the pack boundary at zc (nodes shared, y per-lane).
        auto cheb_eval = [&](__m256d zc) {
            __m256d num = zero;
            __m256d den = zero;
            for (unsigned j = 0; j < nb; ++j) {
                const __m256d dz = _mm256_sub_pd(zc, _mm256_set1_pd(znodes[j]));
                const __m256d qq = _mm256_div_pd(_mm256_set1_pd(wbary[j]), dz);
                num = _mm256_add_pd(num, _mm256_mul_pd(qq, Y[j]));
                den = _mm256_add_pd(den, qq);
            }
            return _mm256_div_pd(num, den);
        };
        // eqn_b_ND generic (american.cpp:797-838), 4-lane. safe_tau assumed > 0.
        auto eqn_b_ND = [&](__m256d tau, __m256d b_val, __m256d& N, __m256d& D) {
            const __m256d v_tip = _mm256_mul_pd(SIG, _mm256_sqrt_pd(tau));
            const __m256d base_tip = _mm256_div_pd(
                _mm256_add_pd(log_pd(_mm256_div_pd(b_val, Kv)),
                              _mm256_mul_pd(RmQ, tau)),
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
                    _mm256_mul_pd(two,
                                  _mm256_sqrt_pd(_mm256_div_pd(u_eff, Tv))),
                    one);
                const __m256d bu = b_from_y_pd(cheb_eval(clamp01(zz)), XMAX);
                const __m256d bu_gt = _mm256_cmp_pd(bu, zero, _CMP_GT_OQ);
                const __m256d qact = _mm256_and_pd(tu_gt, bu_gt);
                const __m256d safe_tu = sel(tu_gt, t_u, one);
                const __m256d z = _mm256_div_pd(b_val, sel(bu_gt, bu, one));
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
            N = _mm256_add_pd(tip_m,
                              _mm256_mul_pd(Rv, _mm256_mul_pd(n_int, half_tau)));
            D = _mm256_add_pd(tip_p,
                              _mm256_mul_pd(Qv, _mm256_mul_pd(d_int, half_tau)));
        };
        // ∂N/∂b, ∂D/∂b (american.cpp:841-854), 4-lane.
        auto eqn_b_NDd = [&](__m256d tau, __m256d b_val, __m256d& Nd, __m256d& Dd) {
            const __m256d v = _mm256_mul_pd(SIG, _mm256_sqrt_pd(tau));
            const __m256d base = _mm256_div_pd(
                _mm256_add_pd(log_pd(_mm256_div_pd(b_val, Kv)),
                              _mm256_mul_pd(RmQ, tau)),
                v);
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
                const __m256d safe_tau = sel(tau_gt, tau, one);
                const __m256d b_val = b_from_y_pd(Y[nodei], XMAX);
                __m256d N, D;
                eqn_b_ND(safe_tau, b_val, N, D);
                const __m256d Dvalid = _mm256_cmp_pd(D, D300, _CMP_GT_OQ);
                const __m256d safeD = sel(Dvalid, D, one);
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
                    const __m256d big =
                        _mm256_cmp_pd(abs_pd(denom), DEN12, _CMP_GT_OQ);
                    const __m256d newton_b = _mm256_sub_pd(
                        b_val, _mm256_div_pd(_mm256_sub_pd(f, b_val),
                                             sel(big, denom, one)));
                    b_new = sel(big, newton_b, f);
                } else {
                    b_new = f;
                }
                b_new = _mm256_min_pd(b_new, XMAX);
                const __m256d bpos = _mm256_cmp_pd(b_new, zero, _CMP_GT_OQ);
                b_new = sel(bpos, b_new, _mm256_mul_pd(K1e6, Kv));
                const __m256d y_new = y_from_b_pd(b_new, XMAX);

                // Δy contributes only where tau>tiny AND D valid AND lane active.
                __m256d dy = abs_pd(_mm256_sub_pd(y_new, Y[nodei]));
                dy = _mm256_and_pd(dy, _mm256_and_pd(tau_gt, Dvalid));
                dy = _mm256_and_pd(dy, active);
                maxdy = _mm256_max_pd(maxdy, dy);

                // next_y = tau≤tiny ? 0 : (D valid ? y_new : y_old); frozen lanes keep y.
                const __m256d val =
                    sel(tau_gt, sel(Dvalid, y_new, Y[nodei]), zero);
                nextY[nodei] = sel(active, val, Y[nodei]);
            }
            for (unsigned j = 0; j < nb; ++j) {
                Y[j] = nextY[j];
            }
            return maxdy;
        };

        // Cascade: n_iter_jn Jacobi-Newton sweeps, then n_iter_fp fixed-point
        // sweeps on the still-active lanes. Freeze a lane once its residual ≤ tol.
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
                active = _mm256_andnot_pd(_mm256_cmp_pd(maxdy, TOL, _CMP_LE_OQ),
                                          active);
                if (_mm256_movemask_pd(active) == 0) {
                    break;
                }
            }
        }

        // ── 4. Vector price: euro (Black-76 put) + AL premium + clamps ────
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
            df, _mm256_sub_pd(
                    _mm256_mul_pd(Kv, _mm256_sub_pd(one, Nd2)),
                    _mm256_mul_pd(F, _mm256_sub_pd(one, Nd1))));

        const __m256d sqrtT = _mm256_sqrt_pd(Tv);
        const __m256d half_sqrtT = _mm256_mul_pd(half, sqrtT);
        __m256d total = zero;
        for (unsigned pi = 0; pi < np; ++pi) {
            const __m256d xs = _mm256_set1_pd(qx_price[pi]);
            const __m256d wv = _mm256_set1_pd(qw_price[pi]);
            const __m256d zi = _mm256_mul_pd(half_sqrtT, _mm256_add_pd(one, xs));
            const __m256d t = _mm256_mul_pd(zi, zi);
            const __m256d t_gt = _mm256_cmp_pd(t, TINY, _CMP_GT_OQ);
            const __m256d rem = _mm256_sub_pd(Tv, t);
            const __m256d rem_gt = _mm256_cmp_pd(rem, zero, _CMP_GT_OQ);
            // b_t = rem>0 ? al_boundary_at(rem) : K
            const __m256d u_eff = _mm256_min_pd(sel(rem_gt, rem, Tv), Tv);
            const __m256d zz = _mm256_sub_pd(
                _mm256_mul_pd(two, _mm256_sqrt_pd(_mm256_div_pd(u_eff, Tv))), one);
            const __m256d bnd_at = b_from_y_pd(cheb_eval(clamp01(zz)), XMAX);
            const __m256d b_t = sel(rem_gt, bnd_at, Kv);
            const __m256d bt_gt = _mm256_cmp_pd(b_t, zero, _CMP_GT_OQ);
            const __m256d iact = _mm256_and_pd(t_gt, bt_gt);
            const __m256d v = _mm256_mul_pd(SIG, _mm256_sqrt_pd(sel(t_gt, t, one)));
            const __m256d dq = exp_pd(_mm256_sub_pd(zero, _mm256_mul_pd(Qv, t)));
            const __m256d dr = exp_pd(_mm256_sub_pd(zero, _mm256_mul_pd(Rv, t)));
            const __m256d ratio = _mm256_div_pd(_mm256_mul_pd(Sv, dq),
                                                _mm256_mul_pd(sel(bt_gt, b_t, one), dr));
            const __m256d dp = _mm256_add_pd(
                _mm256_div_pd(log_pd(ratio), v), _mm256_mul_pd(half, v));
            const __m256d arg1 = _mm256_add_pd(_mm256_sub_pd(zero, dp), v);
            const __m256d arg2 = _mm256_sub_pd(zero, dp);
            __m256d P1, P2;
            norm_cdf_erfc_pd2(arg1, arg2, P1, P2);
            const __m256d termA =
                _mm256_mul_pd(_mm256_mul_pd(Rv, Kv), _mm256_mul_pd(dr, P1));
            const __m256d termB =
                _mm256_mul_pd(_mm256_mul_pd(Qv, Sv), _mm256_mul_pd(dq, P2));
            __m256d integ = _mm256_mul_pd(_mm256_mul_pd(two, zi),
                                          _mm256_sub_pd(termA, termB));
            integ = _mm256_and_pd(integ, iact);
            total = _mm256_add_pd(total, _mm256_mul_pd(wv, integ));
        }
        const __m256d prem = _mm256_max_pd(_mm256_mul_pd(total, half_sqrtT), zero);
        __m256d price = _mm256_add_pd(euro, prem);
        price = _mm256_max_pd(price, _mm256_sub_pd(Kv, Sv)); // intrinsic
        price = _mm256_max_pd(price, euro);                  // euro floor
        price = _mm256_max_pd(price, zero);

        // ── 5. Store + patch edge lanes through the exact scalar path ─────
        alignas(32) double pr[4];
        _mm256_store_pd(pr, price);
        for (int l = 0; l < 4; ++l) {
            const std::size_t idx = i + static_cast<std::size_t>(l);
            const bool patch = !eligible[l] || !std::isfinite(pr[l]);
            price_out[idx] =
                patch ? scalar_put(S[idx], K[idx], T[idx], sigma[idx], r[idx],
                                   q[idx], opts)
                      : pr[l];
        }
    }

    // Scalar tail (n % 4): exact scalar path, matching the *_batch_avx2 idiom.
    for (; i < n; ++i) {
        price_out[i] =
            scalar_put(S[i], K[i], T[i], sigma[i], r[i], q[i], opts);
    }
}

} // namespace atx::vol::simd::detail
