// AVX2 (4-lane f64) batched second-order Taylor P&L-explain.
//
// Built with -mavx2 -mfma (see atx-vol/CMakeLists.txt). Called only when the
// dispatch layer confirms AVX2+FMA at runtime. Lane-parallel across 4 positions;
// the n % 4 tail runs the identical scalar decomposition. Pure arithmetic — no
// transcendentals, no cross-position reduction — so the kernel is loads,
// multiplies, and adds in EXACTLY the scalar source of truth's association tree
// (pnl_batch.cpp), and reproduces it BIT-FOR-BIT. Deliberately no FMA here: fusing
// would drop the rounding of each product, which is both a route-dependent and a
// batch-index-dependent answer for the same position.
//
// Per 4-lane pass, per share (then weighted by w = qty, or 1.0 when qty is null):
//   pd = delta·dS   pg = ½gamma·dS²   pv = vega·dSigma   pvol = ½volga·dSigma²
//   pvanna = vanna·dS·dSigma   pth = theta·dt   prho = rho·dr   pcharm = charm·dS·dt
//   total = pd + pg + pv + pvol + pvanna + pth + prho + pcharm

#include "pnl_batch_avx2.hpp"

#include <immintrin.h>

namespace atx::vol::simd::detail {

namespace {

// Scalar decomposition for the n % 4 tail — matches pnl_batch.cpp term-for-term.
void pnl_row_scalar(const PnlExplainInputs& in, const PnlExplainOutputs& out,
                    std::size_t i) noexcept {
    const double w = (in.qty != nullptr) ? in.qty[i] : 1.0;
    const double dS = in.dS[i];
    const double dSigma = in.dSigma[i];
    const double dt = in.dt[i];
    const double dr = in.dr[i];

    const double pd = in.delta[i] * dS;
    const double pg = 0.5 * in.gamma[i] * dS * dS;
    const double pv = in.vega[i] * dSigma;
    const double pvol = 0.5 * in.volga[i] * dSigma * dSigma;
    const double pvanna = in.vanna[i] * dS * dSigma;
    const double pth = in.theta[i] * dt;
    const double prho = in.rho[i] * dr;
    const double pcharm = in.charm[i] * dS * dt;
    const double explained = pd + pg + pv + pvol + pvanna + pth + prho + pcharm;

    out.delta_pnl[i] = w * pd;
    out.gamma_pnl[i] = w * pg;
    out.vega_pnl[i] = w * pv;
    out.volga_pnl[i] = w * pvol;
    out.vanna_pnl[i] = w * pvanna;
    out.theta_pnl[i] = w * pth;
    out.rho_pnl[i] = w * prho;
    out.charm_pnl[i] = w * pcharm;
    out.total[i] = w * explained;
}

} // namespace

void pnl_taylor_explain_batch_avx2(const PnlExplainInputs& in,
                                   const PnlExplainOutputs& out,
                                   std::size_t n) noexcept {
    const bool has_qty = in.qty != nullptr;
    const __m256d half = _mm256_set1_pd(0.5);
    const __m256d one = _mm256_set1_pd(1.0);

    std::size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        const __m256d w = has_qty ? _mm256_loadu_pd(in.qty + i) : one;
        const __m256d dS = _mm256_loadu_pd(in.dS + i);
        const __m256d dSig = _mm256_loadu_pd(in.dSigma + i);
        const __m256d dt = _mm256_loadu_pd(in.dt + i);
        const __m256d dr = _mm256_loadu_pd(in.dr + i);

        const __m256d delta = _mm256_loadu_pd(in.delta + i);
        const __m256d gamma = _mm256_loadu_pd(in.gamma + i);
        const __m256d vega = _mm256_loadu_pd(in.vega + i);
        const __m256d volga = _mm256_loadu_pd(in.volga + i);
        const __m256d vanna = _mm256_loadu_pd(in.vanna + i);
        const __m256d theta = _mm256_loadu_pd(in.theta + i);
        const __m256d rho = _mm256_loadu_pd(in.rho + i);
        const __m256d charm = _mm256_loadu_pd(in.charm + i);

        // Per-share components, in the SCALAR kernel's association tree. Every
        // product is left-to-right — ((½·gamma)·dS)·dS, not (½·gamma)·(dS·dS) —
        // so each lane rounds exactly where pnl_batch.cpp's scalar loop rounds.
        // Grouping the shared move products (dS·dS, dS·dSigma, dS·dt) instead was
        // one multiply cheaper but rounded elsewhere, which made a position's P&L
        // depend on whether it landed in a vector lane or in the n % 4 scalar tail.
        const __m256d hg = _mm256_mul_pd(half, gamma);
        const __m256d hv = _mm256_mul_pd(half, volga);
        const __m256d pd = _mm256_mul_pd(delta, dS);
        const __m256d pg = _mm256_mul_pd(_mm256_mul_pd(hg, dS), dS);
        const __m256d pv = _mm256_mul_pd(vega, dSig);
        const __m256d pvol = _mm256_mul_pd(_mm256_mul_pd(hv, dSig), dSig);
        const __m256d pvanna = _mm256_mul_pd(_mm256_mul_pd(vanna, dS), dSig);
        const __m256d pth = _mm256_mul_pd(theta, dt);
        const __m256d prho = _mm256_mul_pd(rho, dr);
        const __m256d pcharm = _mm256_mul_pd(_mm256_mul_pd(charm, dS), dt);

        // Explained total: the same left-to-right eight-term sum of the components
        // stored below, NOT an FMA chain over the (coefficient, move) products. An
        // FMA feeds each product into the sum UNROUNDED, so `total` stopped being
        // the sum of the very numbers written to the component columns — breaking
        // the pnl_batch.hpp contract on the AVX2 route only. Each add is its own
        // statement so /fp:precise (-ffp-contract=on) cannot re-fuse them.
        __m256d acc = _mm256_add_pd(pd, pg);
        acc = _mm256_add_pd(acc, pv);
        acc = _mm256_add_pd(acc, pvol);
        acc = _mm256_add_pd(acc, pvanna);
        acc = _mm256_add_pd(acc, pth);
        acc = _mm256_add_pd(acc, prho);
        acc = _mm256_add_pd(acc, pcharm);

        _mm256_storeu_pd(out.delta_pnl + i, _mm256_mul_pd(w, pd));
        _mm256_storeu_pd(out.gamma_pnl + i, _mm256_mul_pd(w, pg));
        _mm256_storeu_pd(out.vega_pnl + i, _mm256_mul_pd(w, pv));
        _mm256_storeu_pd(out.volga_pnl + i, _mm256_mul_pd(w, pvol));
        _mm256_storeu_pd(out.vanna_pnl + i, _mm256_mul_pd(w, pvanna));
        _mm256_storeu_pd(out.theta_pnl + i, _mm256_mul_pd(w, pth));
        _mm256_storeu_pd(out.rho_pnl + i, _mm256_mul_pd(w, prho));
        _mm256_storeu_pd(out.charm_pnl + i, _mm256_mul_pd(w, pcharm));
        _mm256_storeu_pd(out.total + i, _mm256_mul_pd(w, acc));
    }
    for (; i < n; ++i) {
        pnl_row_scalar(in, out, i);
    }
}

} // namespace atx::vol::simd::detail
