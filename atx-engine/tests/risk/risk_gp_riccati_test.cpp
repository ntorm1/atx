// RiskGpRiccati_* — Lane 6 multi-period Gârleanu-Pedersen policy (dense Riccati + the
// factor-space closed form).

#include <cmath>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/linalg/linalg.hpp"
#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/gp_riccati.hpp"

namespace atx_test_l6_optim_gp_riccati {
namespace risk = atx::engine::risk;
namespace la = atx::core::linalg;
using atx::f64;
using atx::usize;

// Two correlated assets through one common factor.
risk::FactorModel two_asset_model() {
  la::MatX x(2, 1);
  x << 1.0, 0.6;
  la::MatX f(1, 1);
  f << 0.04;
  la::VecX d(2);
  d << 0.02, 0.03;
  auto m = risk::FactorModel::create(x, f, d, 0, 1);
  EXPECT_TRUE(m.has_value());
  return std::move(*m);
}

la::MatX two_signal_loadings() {
  la::MatX b(2, 2);
  b << 0.010, -0.004, 0.003, 0.008;
  return b;
}

la::MatX sigma_of(const risk::FactorModel &v) {
  la::MatX s = v.exposures() * v.factor_cov() * v.exposures().transpose();
  s.diagonal() += v.specific_var();
  return s;
}

TEST(RiskGpRiccati, DenseProportionalMatchesGpClosedFormRateAndAim) {
  const auto v = two_asset_model();
  const auto b = two_signal_loadings();
  const std::vector<f64> phi{0.05, 0.4};
  const f64 gamma = 2.0;
  const f64 lambda = 3.0;
  const f64 rho = 0.01;
  risk::GpRiccatiCfg cfg;
  cfg.rho = rho;
  cfg.force_dense = true;
  const auto pol = risk::gp_riccati(v, gamma, {{}, lambda}, phi, b, cfg);
  ASSERT_TRUE(pol) << pol.error().message();
  ASSERT_TRUE(pol->converged);

  const f64 rb = 1.0 - rho;
  const f64 bb = gamma * rb + lambda * rho;
  const f64 a = (-bb + std::sqrt(bb * bb + 4.0 * gamma * lambda * rb * rb)) / (2.0 * rb);
  // Trade rate: I − keep == (a/λ)·I.
  const la::MatX rate = la::MatX::Identity(2, 2) - pol->keep;
  EXPECT_NEAR(rate(0, 0), a / lambda, 1e-8);
  EXPECT_NEAR(rate(1, 1), a / lambda, 1e-8);
  EXPECT_NEAR(rate(0, 1), 0.0, 1e-8);
  EXPECT_NEAR(rate(1, 0), 0.0, 1e-8);
  // Aim: (γΣ)⁻¹ B (I + aΦ/γ)⁻¹ f.
  const std::vector<f64> f{1.3, -0.7};
  la::VecX shrunk(2);
  shrunk << f[0] / (1.0 + a * phi[0] / gamma), f[1] / (1.0 + a * phi[1] / gamma);
  const la::VecX expected = (gamma * sigma_of(v)).ldlt().solve(b * shrunk);
  const auto aim = pol->aim(f);
  EXPECT_NEAR(aim[0], expected[0], 1e-8);
  EXPECT_NEAR(aim[1], expected[1], 1e-8);
}

TEST(RiskGpRiccati, FactorSpacePathMatchesDenseOracle) {
  const auto v = two_asset_model();
  const auto b = two_signal_loadings();
  const std::vector<f64> phi{0.1, 0.25};
  risk::GpRiccatiCfg dense_cfg;
  dense_cfg.rho = 0.02;
  dense_cfg.force_dense = true;
  risk::GpRiccatiCfg fast_cfg;
  fast_cfg.rho = 0.02;
  const auto dense = risk::gp_riccati(v, 1.5, {{}, 4.0}, phi, b, dense_cfg);
  const auto fast = risk::gp_riccati(v, 1.5, {{}, 4.0}, phi, b, fast_cfg);
  ASSERT_TRUE(dense);
  ASSERT_TRUE(fast);
  EXPECT_TRUE(fast->factor_space);
  const std::vector<f64> x_prev{0.2, -0.1};
  const std::vector<f64> f{0.5, 1.0};
  const auto xd = dense->step(x_prev, f);
  const auto xf = fast->step(x_prev, f);
  EXPECT_NEAR(xd[0], xf[0], 1e-9);
  EXPECT_NEAR(xd[1], xf[1], 1e-9);
}

TEST(RiskGpRiccati, HorizonOneIsTheMyopicCostedTrade) {
  const auto v = two_asset_model();
  const auto b = two_signal_loadings();
  const std::vector<f64> phi{0.1, 0.3};
  const std::vector<f64> lam{0.5, 0.8};
  risk::GpRiccatiCfg cfg;
  cfg.horizon = 1;
  cfg.rho = 0.05;
  const auto pol = risk::gp_riccati(v, 2.0, {lam, 0.0}, phi, b, cfg);
  ASSERT_TRUE(pol) << pol.error().message();
  EXPECT_EQ(pol->iterations, 1U);
  // x = (ρ̄γΣ + Λ)⁻¹(Λ x_prev + ρ̄ B f)
  const f64 rb = 0.95;
  la::MatX lm = la::MatX::Zero(2, 2);
  lm(0, 0) = lam[0];
  lm(1, 1) = lam[1];
  const la::MatX j = rb * 2.0 * sigma_of(v) + lm;
  la::VecX xp(2);
  xp << 0.3, -0.2;
  la::VecX fv(2);
  fv << 0.7, 0.2;
  const la::VecX expected = j.llt().solve(lm * xp + rb * b * fv);
  const auto x = pol->step(std::vector<f64>{0.3, -0.2}, std::vector<f64>{0.7, 0.2});
  EXPECT_NEAR(x[0], expected[0], 1e-14);
  EXPECT_NEAR(x[1], expected[1], 1e-14);
}

TEST(RiskGpRiccati, LongHorizonConvergesToSteadyStateAndSatisfiesRiccati) {
  const auto v = two_asset_model();
  const auto b = two_signal_loadings();
  const std::vector<f64> phi{0.1, 0.3};
  const std::vector<f64> lam{0.5, 0.8};
  risk::GpRiccatiCfg inf_cfg;
  inf_cfg.rho = 0.01;
  const auto inf = risk::gp_riccati(v, 2.0, {lam, 0.0}, phi, b, inf_cfg);
  ASSERT_TRUE(inf);
  ASSERT_TRUE(inf->converged);
  // Fixed-point residual of the Riccati map.
  const f64 rb = 0.99;
  const la::MatX sigma = sigma_of(v);
  la::MatX lm = la::MatX::Zero(2, 2);
  lm(0, 0) = lam[0];
  lm(1, 1) = lam[1];
  const la::MatX j = rb * 2.0 * sigma + lm + rb * inf->A_xx;
  const la::MatX axx_next = lm - lm * j.llt().solve(lm);
  EXPECT_LE((axx_next - inf->A_xx).cwiseAbs().maxCoeff(), 1e-12);
  // A long finite horizon approaches the steady state.
  risk::GpRiccatiCfg h_cfg = inf_cfg;
  h_cfg.horizon = 4000;
  const auto hor = risk::gp_riccati(v, 2.0, {lam, 0.0}, phi, b, h_cfg);
  ASSERT_TRUE(hor);
  EXPECT_LE((hor->keep - inf->keep).cwiseAbs().maxCoeff(), 1e-9);
  // The forward-looking policy values the position's future alpha, so it keeps less of
  // the old book than the myopic costed trade (GP: rate a/λ > γ/(γ+λ)).
  risk::GpRiccatiCfg one = inf_cfg;
  one.horizon = 1;
  const auto myo = risk::gp_riccati(v, 2.0, {lam, 0.0}, phi, b, one);
  ASSERT_TRUE(myo);
  EXPECT_LT(inf->keep.trace(), myo->keep.trace());
}

TEST(RiskGpRiccati, RejectsBadInputs) {
  const auto v = two_asset_model();
  const auto b = two_signal_loadings();
  const std::vector<f64> phi{0.1, 0.3};
  const std::vector<f64> lam{0.5, 0.8};
  EXPECT_FALSE(risk::gp_riccati(v, 0.0, {lam, 0.0}, phi, b));
  EXPECT_FALSE(risk::gp_riccati(v, 1.0, {{}, 0.0}, phi, b));
  const std::vector<f64> bad_phi{0.1, 1.5};
  EXPECT_FALSE(risk::gp_riccati(v, 1.0, {lam, 0.0}, bad_phi, b));
  risk::GpRiccatiCfg cfg;
  cfg.rho = 1.0;
  EXPECT_FALSE(risk::gp_riccati(v, 1.0, {lam, 0.0}, phi, b, cfg));
  const la::MatX bad_b = la::MatX::Zero(3, 2);
  EXPECT_FALSE(risk::gp_riccati(v, 1.0, {lam, 0.0}, phi, bad_b));
}

} // namespace atx_test_l6_optim_gp_riccati
