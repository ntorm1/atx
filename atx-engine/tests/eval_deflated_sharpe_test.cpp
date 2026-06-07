#include <gtest/gtest.h>
#include <cmath>
#include <optional>
#include "atx/engine/eval/deflated_sharpe.hpp"
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
