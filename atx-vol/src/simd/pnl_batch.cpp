#include "atx/vol/simd/pnl_batch.hpp"

#include "atx/vol/simd/cpu.hpp"

#include "pnl_batch_avx2.hpp"

namespace atx::vol::simd {

namespace {

// Scalar per-position Taylor P&L-explain — the numerical source of truth. Mirrors
// PortfolioPricer's scatter_pnl_rows / reduce_pnl_totals (portfolio_pricer.cpp)
// term-for-term and in the same op order, so this batch is the vectorized image
// of the exact decomposition the portfolio pricer already ships.
void pnl_taylor_explain_batch_scalar(const PnlExplainInputs& in,
                                     const PnlExplainOutputs& out,
                                     std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
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
