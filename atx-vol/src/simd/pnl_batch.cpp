#include "simd/pnl_batch.hpp"

#include "atx/vol/api/simd/cpu.hpp"

#include "pnl_batch_avx2.hpp"

namespace atx::vol::simd {

namespace {

// Scalar per-position Taylor P&L-explain — the numerical source of truth. Mirrors
// PortfolioPricer's scatter_pnl_rows / reduce_pnl_totals (portfolio_pricer.cpp)
// term-for-term and in the same op order, so this batch is the vectorized image
// of the exact decomposition the portfolio pricer already ships.
// SAFETY (__restrict, T9): the pointers below are declared non-aliasing. This is not
// a new promise — every one of these headers already states that the output arrays
// must not alias the inputs, and the SoA columns are documented distinct — but nothing
// in the code SAID so, and there is no `restrict` anywhere else under src/simd or
// src/pricing. Restrict only constrains objects that are MODIFIED in the scope, so two
// read-only input columns pointing at the same array remain well defined; what it rules
// out is an output column overlapping an input or another output, which the API forbids
// already. Applied ONLY to the file-local scalar fallbacks — the AVX2 kernels use
// explicit loads/stores and are unaffected — so no intrinsic path and no number moves.
//
// This loop is the reason the item exists: docs/simd_fastpath.md:82-86 records
// scalar_novec == scalar_autovec in the bench, i.e. clang did NOT auto-vectorize it,
// and 13 input plus 9 output pointers with no non-aliasing promise is why. The columns
// are re-bound to restrict-qualified locals rather than annotating the two structs, so
// the public layout types are untouched. The association tree is unchanged, so the
// BIT-EXACT scalar-vs-AVX2 identity SimdPnlWiring gates still holds either way.
void pnl_taylor_explain_batch_scalar(const PnlExplainInputs& in,
                                     const PnlExplainOutputs& out,
                                     std::size_t n) noexcept {
    const double* __restrict delta = in.delta;
    const double* __restrict gamma = in.gamma;
    const double* __restrict vega = in.vega;
    const double* __restrict volga = in.volga;
    const double* __restrict vanna = in.vanna;
    const double* __restrict theta = in.theta;
    const double* __restrict rho = in.rho;
    const double* __restrict charm = in.charm;
    const double* __restrict qty = in.qty;
    const double* __restrict dS_c = in.dS;
    const double* __restrict dSigma_c = in.dSigma;
    const double* __restrict dt_c = in.dt;
    const double* __restrict dr_c = in.dr;
    double* __restrict o_delta = out.delta_pnl;
    double* __restrict o_gamma = out.gamma_pnl;
    double* __restrict o_vega = out.vega_pnl;
    double* __restrict o_volga = out.volga_pnl;
    double* __restrict o_vanna = out.vanna_pnl;
    double* __restrict o_theta = out.theta_pnl;
    double* __restrict o_rho = out.rho_pnl;
    double* __restrict o_charm = out.charm_pnl;
    double* __restrict o_total = out.total;

    for (std::size_t i = 0; i < n; ++i) {
        const double w = (qty != nullptr) ? qty[i] : 1.0;
        const double dS = dS_c[i];
        const double dSigma = dSigma_c[i];
        const double dt = dt_c[i];
        const double dr = dr_c[i];

        const double pd = delta[i] * dS;
        const double pg = 0.5 * gamma[i] * dS * dS;
        const double pv = vega[i] * dSigma;
        const double pvol = 0.5 * volga[i] * dSigma * dSigma;
        const double pvanna = vanna[i] * dS * dSigma;
        const double pth = theta[i] * dt;
        const double prho = rho[i] * dr;
        const double pcharm = charm[i] * dS * dt;
        const double explained = pd + pg + pv + pvol + pvanna + pth + prho + pcharm;

        o_delta[i] = w * pd;
        o_gamma[i] = w * pg;
        o_vega[i] = w * pv;
        o_volga[i] = w * pvol;
        o_vanna[i] = w * pvanna;
        o_theta[i] = w * pth;
        o_rho[i] = w * prho;
        o_charm[i] = w * pcharm;
        o_total[i] = w * explained;
    }
}

} // namespace

void pnl_taylor_explain_batch(const PnlExplainInputs& in,
                              const PnlExplainOutputs& out,
                              std::size_t n) noexcept {
    if (use_avx2()) {
        detail::pnl_taylor_explain_batch_avx2(in, out, n);
    } else {
        pnl_taylor_explain_batch_scalar(in, out, n);
    }
}

} // namespace atx::vol::simd
