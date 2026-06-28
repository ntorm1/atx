#include <gtest/gtest.h>
#include <cmath>
#include <optional>
#include <vector>
#include "atx/engine/eval/deflated_sharpe.hpp"

namespace atxtest_eval_deflated_sharpe_test {

using namespace atx::engine::eval;
TEST(EvalDsr, N1_EqualsProbabilisticSharpeAtZero) {
  auto d = deflated_sharpe(0.10, 250, 0.0, 0.0, 1, std::nullopt);
  EXPECT_NEAR(d.dsr, probabilistic_sharpe(0.10, 0.0, 250, 0.0, 0.0), 1e-12);
}
TEST(EvalDsr, SelectionPenaltyBites_DecreasingInN) {
  double prev = 1.0;
  for (std::size_t N : {1U, 10U, 100U, 1000U, 100000U}) {
    auto d = deflated_sharpe(0.12, 500, -0.2, 3.0, N, std::nullopt);
    EXPECT_LE(d.dsr, prev + 1e-12); prev = d.dsr;
  }
  EXPECT_LT(deflated_sharpe(0.12,500,-0.2,3.0,100000U,std::nullopt).dsr, 0.5);
}
TEST(EvalDsr, IncreasingInObservedSharpe) {
  EXPECT_LT(deflated_sharpe(0.05,500,0,0,100,std::nullopt).dsr, deflated_sharpe(0.20,500,0,0,100,std::nullopt).dsr);
}
TEST(EvalDsr, PsrAtBenchmarkEqualSharpeIsHalf) {
  EXPECT_NEAR(probabilistic_sharpe(0.10, 0.10, 250, 0.0, 0.0), 0.5, 1e-9);
}
TEST(EvalDsr, Haircut_NonNegativeAndBelowSharpe) {
  auto d = deflated_sharpe(0.15, 750, -0.1, 2.0, 500, std::nullopt);
  EXPECT_GE(d.haircut_sharpe, 0.0); EXPECT_LE(d.haircut_sharpe, 0.15);
}
TEST(EvalDsr, ReferenceValue_CitedConstant) {
  // Reference computed offline from the Bailey-Lopez de Prado (2014) DSR formula
  // (§3.3), using an independent probit (NormalDist.inv_cdf / .cdf), NOT this
  // library — so the expected value is not a tautology. Full arithmetic:
  //   Inputs: SR=0.0808, T=120, gamma3=-0.5, kappa(exkurt)=3.0, N=100, var=nullopt.
  //   inner   = 1 - gamma3*SR + ((kappa+2)/4)*SR^2
  //           = 1 - (-0.5)(0.0808) + ((5)/4)(0.0808^2)
  //           = 1 + 0.0404 + 1.25*0.00652864 = 1.0485608
  //   V_hat   = inner / T = 1.0485608 / 120        = 0.008738006666666666
  //   sqrtV   = sqrt(V_hat)                          = 0.09347730562370027
  //   z1 = ppf(1 - 1/N)      = ppf(0.99)             = 2.3263478740408408
  //   z2 = ppf(1 - 1/(N*e))  = ppf(0.9963212055...)  = 2.680210444966887
  //   SR*_100 = sqrtV * [ (1-gammaE)*z1 + gammaE*z2 ]   (gammaE = 0.5772156649015329)
  //           = 0.09347730562370027 * [0.4227843350984671*2.3263478740408408
  //                                    + 0.5772156649015329*2.680210444966887]
  //           = 0.236553940060034
  //   denom   = sqrt(inner) = sqrt(1.0485608)        = 1.0239925780981032
  //   z       = (SR - SR*_100)*sqrt(T-1) / denom
  //           = (0.0808 - 0.236553940060034)*sqrt(119)/1.0239925780981032
  //           = -1.6592648513047705
  //   DSR     = Phi(z) = norm_cdf(-1.6592648513047705) = 0.04853121733576854
  auto d = deflated_sharpe(0.0808, 120, -0.5, 3.0, 100, std::nullopt);
  EXPECT_NEAR(d.dsr, /*REF=*/0.0485312173357685, 1e-6);
}
TEST(EvalDsr, DegenerateT_ReturnsNaN) {
  EXPECT_TRUE(std::isnan(probabilistic_sharpe(0.10, 0.0, 1, 0.0, 0.0)));  // T<2 degenerate
  EXPECT_TRUE(std::isnan(probabilistic_sharpe(0.10, 0.0, 0, 0.0, 0.0)));
}

// ---------------------------------------------------------------------------
//  S4-4: deflated_sharpe_net_cost tests.
// ---------------------------------------------------------------------------

// Helper: build a synthetic PnL series of length n with constant per-period
// return mu and zero-mean noise, along with a constant turnover series.
// We use a simple alternating-sign noise so stats are well-defined and the
// gross DSR is positive and meaningful.
static std::vector<atx::f64> make_pnl(atx::usize n, atx::f64 mu, atx::f64 noise_amp) {
  std::vector<atx::f64> out(n);
  if (n == 0U) { return out; }
  out[0] = 0.0; // structural zero
  for (atx::usize t = 1U; t < n; ++t) {
    // alternating noise so mean ≈ mu; individual values mu ± noise_amp
    const atx::f64 sign = ((t % 2U) == 0U) ? 1.0 : -1.0;
    out[t] = mu + sign * noise_amp;
  }
  return out;
}

static std::vector<atx::f64> make_turnover(atx::usize n, atx::f64 u) {
  return std::vector<atx::f64>(n, u);
}

TEST(EvalDsrNetCost, HighTurnoverCostReducesDSR) {
  // High turnover (0.8) + rt_cost_bps=10.0 drains enough from gross returns
  // that the net-cost DSR is strictly less than the gross DSR (on the same
  // gross series). We construct a MARGINAL case so neither DSR saturates to 1:
  // small gross mu (0.001) and large noise (0.010) keep the gross SR low; high
  // turnover with 10 bps cost adds a material per-period drag of ~0.0008/period.
  // T_obs=19 (20-1) is small so neither DSR saturates, keeping both below 1.
  constexpr atx::usize N_PERIODS = 20U;
  constexpr atx::f64 MU = 0.001;
  constexpr atx::f64 NOISE = 0.010;
  constexpr atx::f64 U = 0.8;
  constexpr atx::f64 COST_BPS = 10.0;
  constexpr atx::usize N_TRIALS = 5U;

  const auto pnl      = make_pnl(N_PERIODS, MU, NOISE);
  const auto turnover = make_turnover(N_PERIODS, U);

  // Gross DSR: compute gross SR/moments over pnl[1..T) directly.
  // T_obs = N_PERIODS - 1 = 19 observations.
  const auto pnl_span = std::span<const atx::f64>{pnl}.subspan(1);
  const atx::usize T_obs = pnl_span.size();
  const MeanStd ms = mean_std_pop(pnl_span);
  const atx::f64 sr_gross = (ms.std > 0.0) ? ms.mean / ms.std : 0.0;
  const atx::f64 skew_gross = skewness(pnl_span);
  const atx::f64 exkurt_gross = excess_kurtosis(pnl_span);
  const auto dsr_gross = deflated_sharpe(sr_gross, T_obs, skew_gross, exkurt_gross, N_TRIALS, std::nullopt);

  // Net-cost DSR via the new helper.
  const auto dsr_net = deflated_sharpe_net_cost(pnl, turnover, COST_BPS, N_TRIALS, std::nullopt);

  // With high turnover and non-zero cost the net DSR must be strictly less.
  EXPECT_LT(dsr_net.dsr, dsr_gross.dsr);
}

TEST(EvalDsrNetCost, ZeroCostEqualsGrossDSR) {
  // rt_cost_bps = 0.0: r_net[t] == r_gross[t], so the net DSR must equal the
  // gross DSR within floating-point epsilon (same arithmetic path).
  constexpr atx::usize N_PERIODS = 200U;
  constexpr atx::f64 MU = 0.003;
  constexpr atx::f64 NOISE = 0.002;
  constexpr atx::f64 U = 0.8;
  constexpr atx::usize N_TRIALS = 30U;

  const auto pnl      = make_pnl(N_PERIODS, MU, NOISE);
  const auto turnover = make_turnover(N_PERIODS, U);

  // Gross DSR (observation count = N_PERIODS - 1 = 199).
  const auto pnl_span = std::span<const atx::f64>{pnl}.subspan(1);
  const atx::usize T_obs = pnl_span.size();
  const MeanStd ms = mean_std_pop(pnl_span);
  const atx::f64 sr_gross = (ms.std > 0.0) ? ms.mean / ms.std : 0.0;
  const atx::f64 skew_gross = skewness(pnl_span);
  const atx::f64 exkurt_gross = excess_kurtosis(pnl_span);
  const auto dsr_gross = deflated_sharpe(sr_gross, T_obs, skew_gross, exkurt_gross, N_TRIALS, std::nullopt);

  const auto dsr_net = deflated_sharpe_net_cost(pnl, turnover, /*rt_cost_bps=*/0.0, N_TRIALS, std::nullopt);

  // Both should agree to within floating-point round-off.
  EXPECT_NEAR(dsr_net.dsr, dsr_gross.dsr, 1e-12);
}

TEST(EvalDsrNetCost, MismatchedSpanLengthsReturnNaN) {
  // Release-safe guard: a size mismatch must NOT read turnover out of bounds — it
  // fails safe to a NaN DsrResult (the assert is debug-only; this covers NDEBUG).
  const std::vector<atx::f64> pnl{0.0, 0.01, -0.02, 0.03, 0.00};
  const std::vector<atx::f64> turnover{0.5, 0.5, 0.5}; // shorter than pnl
  const auto d = deflated_sharpe_net_cost(pnl, turnover, 10.0, 5U, std::nullopt);
  EXPECT_TRUE(std::isnan(d.dsr));
  EXPECT_TRUE(std::isnan(d.psr));
  EXPECT_TRUE(std::isnan(d.sr_star));
  EXPECT_TRUE(std::isnan(d.haircut_sharpe));
}

TEST(EvalDsrNetCost, DegenerateShortSeriesReturnsNaN) {
  // Fewer than 1 return observation (length-1 stream: only the structural zero)
  // is degenerate -> NaN DsrResult (mirrors compute_metrics's 0-observation policy).
  const std::vector<atx::f64> pnl{0.0};
  const std::vector<atx::f64> turnover{0.8};
  const auto d = deflated_sharpe_net_cost(pnl, turnover, 10.0, 5U, std::nullopt);
  EXPECT_TRUE(std::isnan(d.dsr));
}


}  // namespace atxtest_eval_deflated_sharpe_test
