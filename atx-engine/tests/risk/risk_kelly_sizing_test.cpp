// risk_kelly_sizing_test.cpp — S10-2: fractional-Kelly conviction-scaled sizing.
//
// kelly_size computes the full-Kelly target f* = V^{-1} mu over the FACTORED
// covariance (FactorModel::apply_inverse, Woodbury — never an MxM inverse), scales
// it by cfg.kelly_fraction (default quarter-Kelly), scales each name by its
// conviction in [0,1], then clamps gross leverage Sum|w| to cfg.max_gross.
//
// These tests pin the algorithm end-to-end with HAND-COMPUTABLE oracles: a D-only
// (pure-specific-variance) FactorModel makes V = diag(D), so V^{-1}mu = mu_i / D_i.
//   * Full-Kelly closed form (frac=1, conviction=1, clamp off) == V^{-1}mu.
//   * frac=0.25 scales every weight by exactly 0.25 vs frac=1.
//   * a zero-conviction name => exactly 0.0 weight, others unaffected.
//   * gross clamp binds: realized gross == max_gross, scale_applied == max_gross/pre.
//   * gross clamp slack: scale_applied == 1.0.
//   * deterministic: two runs byte-identical.
//   * death: a NaN expected_alpha aborts (fail-closed finite guard).
// Caught by `ctest -R KellySizing`.

#include <cmath>   // std::fabs
#include <limits>  // std::numeric_limits (NaN literal for the death test)
#include <span>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include "atx/core/linalg/linalg.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/kelly_sizing.hpp"

namespace atxtest_risk_kelly_sizing_test {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::FactorModel;
using atx::engine::risk::kelly_size;
using atx::engine::risk::KellyConfig;
using atx::engine::risk::KellyWeights;

// A D-ONLY FactorModel: X all-zero so X F Xᵀ vanishes and V = diag(D). Then
// V^{-1}mu = mu_i / D_i — a hand-computable closed form for every assertion below.
// K=1 (one dummy factor column of zeros) is the minimal SPD F the model accepts.
[[nodiscard]] FactorModel diag_model(const VecX &d) {
  const Eigen::Index m = d.size();
  const MatX x = MatX::Zero(m, 1); // zero exposures ⇒ no factor contribution to V
  MatX f(1, 1);
  f << 1.0; // SPD; irrelevant since X is zero (X F Xᵀ == 0)
  auto r = FactorModel::create(x, f, d, 0U, 1U);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().to_string());
  return std::move(*r);
}

[[nodiscard]] VecX vec(std::initializer_list<f64> xs) {
  VecX v(static_cast<Eigen::Index>(xs.size()));
  Eigen::Index i = 0;
  for (const f64 x : xs) {
    v[i++] = x;
  }
  return v;
}

// 1. Full-Kelly closed form: D-only model, frac=1, conviction all 1, clamp off ⇒
//    weights == V^{-1}mu (verified BOTH against mu_i/D_i and the model's own apply).
TEST(KellySizing, FullKellyClosedForm) {
  const VecX d = vec({0.10, 0.20, 0.05});
  const VecX mu = vec({0.02, -0.04, 0.01});
  const VecX conv = vec({1.0, 1.0, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig cfg;
  cfg.kelly_fraction = 1.0;
  cfg.max_gross = -1.0; // <= 0 disables the clamp
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  // Hand oracle: V = diag(D) ⇒ w_i = mu_i / D_i.
  ASSERT_EQ(kw.weights.size(), 3);
  EXPECT_NEAR(kw.weights[0], 0.02 / 0.10, 1e-9);
  EXPECT_NEAR(kw.weights[1], -0.04 / 0.20, 1e-9);
  EXPECT_NEAR(kw.weights[2], 0.01 / 0.05, 1e-9);

  // Cross-check against the model's own apply_inverse on mu (same Woodbury path).
  VecX fstar(3);
  m.apply_inverse(std::span<const f64>(mu.data(), static_cast<std::size_t>(mu.size())),
                  std::span<f64>(fstar.data(), static_cast<std::size_t>(fstar.size())));
  for (Eigen::Index i = 0; i < 3; ++i) {
    EXPECT_NEAR(kw.weights[i], fstar[i], 1e-9);
  }
  EXPECT_DOUBLE_EQ(kw.scale_applied, 1.0); // clamp disabled ⇒ no scaling
}

// 2. kelly_fraction=0.25 scales every weight to exactly 0.25x the full-Kelly weight.
TEST(KellySizing, QuarterKellyScalesByFraction) {
  const VecX d = vec({0.10, 0.20, 0.05});
  const VecX mu = vec({0.02, -0.04, 0.01});
  const VecX conv = vec({1.0, 1.0, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig full;
  full.kelly_fraction = 1.0;
  full.max_gross = -1.0;
  KellyConfig quarter;
  quarter.kelly_fraction = 0.25;
  quarter.max_gross = -1.0;

  const KellyWeights kf = kelly_size(mu, m, conv, full);
  const KellyWeights kq = kelly_size(mu, m, conv, quarter);
  ASSERT_EQ(kf.weights.size(), kq.weights.size());
  for (Eigen::Index i = 0; i < kf.weights.size(); ++i) {
    EXPECT_NEAR(kq.weights[i], 0.25 * kf.weights[i], 1e-12);
  }
}

// 3. A zero-conviction name ⇒ that name's weight is EXACTLY 0.0; others unaffected.
TEST(KellySizing, ZeroConvictionNameIsExactlyZero) {
  const VecX d = vec({0.10, 0.20, 0.05});
  const VecX mu = vec({0.02, -0.04, 0.01});
  const VecX conv = vec({1.0, 0.0, 1.0}); // name 1 has zero conviction
  const FactorModel m = diag_model(d);

  KellyConfig cfg;
  cfg.kelly_fraction = 1.0;
  cfg.max_gross = -1.0;
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  EXPECT_EQ(kw.weights[1], 0.0); // exact zero, not merely near
  EXPECT_NEAR(kw.weights[0], 0.02 / 0.10, 1e-9);
  EXPECT_NEAR(kw.weights[2], 0.01 / 0.05, 1e-9);
}

// 4. Gross clamp binds: pre-clamp gross > max_gross ⇒ realized gross == max_gross and
//    scale_applied == max_gross / pre_gross (< 1).
TEST(KellySizing, GrossClampBinds) {
  const VecX d = vec({0.10, 0.20});
  const VecX mu = vec({0.50, -0.40}); // large alpha ⇒ large pre-clamp gross
  const VecX conv = vec({1.0, 1.0});
  const FactorModel m = diag_model(d);

  // Pre-clamp full-Kelly weights: 0.50/0.10 = 5.0, -0.40/0.20 = -2.0 ⇒ gross = 7.0.
  const f64 pre_gross = std::fabs(0.50 / 0.10) + std::fabs(-0.40 / 0.20);
  ASSERT_DOUBLE_EQ(pre_gross, 7.0);

  KellyConfig cfg;
  cfg.kelly_fraction = 1.0;
  cfg.max_gross = 1.0; // far below pre_gross ⇒ clamp binds
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  EXPECT_NEAR(kw.gross, 1.0, 1e-12);
  EXPECT_LT(kw.scale_applied, 1.0);
  EXPECT_NEAR(kw.scale_applied, 1.0 / pre_gross, 1e-12);
  // Relative tilt preserved: each weight is pre_weight * scale_applied.
  EXPECT_NEAR(kw.weights[0], (0.50 / 0.10) * kw.scale_applied, 1e-12);
  EXPECT_NEAR(kw.weights[1], (-0.40 / 0.20) * kw.scale_applied, 1e-12);
}

// 5. Gross clamp slack: tiny alpha ⇒ pre-clamp gross < max_gross ⇒ scale_applied == 1.
TEST(KellySizing, GrossClampNotBinding) {
  const VecX d = vec({1.0, 1.0});
  const VecX mu = vec({0.01, -0.02}); // V^{-1}mu = mu (D=1) ⇒ gross = 0.03 << 1
  const VecX conv = vec({1.0, 1.0});
  const FactorModel m = diag_model(d);

  KellyConfig cfg;
  cfg.kelly_fraction = 1.0;
  cfg.max_gross = 1.0;
  const KellyWeights kw = kelly_size(mu, m, conv, cfg);

  EXPECT_DOUBLE_EQ(kw.scale_applied, 1.0);
  EXPECT_NEAR(kw.gross, 0.03, 1e-12);
  EXPECT_NEAR(kw.weights[0], 0.01, 1e-9);
  EXPECT_NEAR(kw.weights[1], -0.02, 1e-9);
}

// 6. Deterministic: same inputs ⇒ byte-identical weights + gross across two runs.
TEST(KellySizing, DeterministicTwoRunsEqual) {
  const VecX d = vec({0.10, 0.20, 0.05, 0.15});
  const VecX mu = vec({0.03, -0.05, 0.02, 0.04});
  const VecX conv = vec({0.8, 0.3, 1.0, 0.5});
  const FactorModel m = diag_model(d);

  KellyConfig cfg; // defaults (quarter-Kelly, max_gross 1.0)
  const KellyWeights a = kelly_size(mu, m, conv, cfg);
  const KellyWeights b = kelly_size(mu, m, conv, cfg);

  ASSERT_EQ(a.weights.size(), b.weights.size());
  for (Eigen::Index i = 0; i < a.weights.size(); ++i) {
    EXPECT_EQ(a.weights[i], b.weights[i]); // exact, element-wise
  }
  EXPECT_EQ(a.gross, b.gross);
  EXPECT_EQ(a.scale_applied, b.scale_applied);
}

// 7. Fail-closed: a NaN in expected_alpha aborts (the debug finite guard). Cast to
//    void — the result is [[nodiscard]] and the call aborts before returning.
TEST(KellySizingDeathTest, NanAlphaAborts) {
  const VecX d = vec({0.10, 0.20});
  const VecX bad = vec({std::numeric_limits<f64>::quiet_NaN(), 0.01});
  const VecX conv = vec({1.0, 1.0});
  const FactorModel m = diag_model(d);
  EXPECT_DEATH((void)kelly_size(bad, m, conv, {}), ".*");
}

// 8. Fail-closed: an out-of-range conviction (> 1) aborts (the [0,1] domain guard).
TEST(KellySizingDeathTest, ConvictionAboveOneAborts) {
  const VecX d = vec({0.10, 0.20});
  const VecX mu = vec({0.02, 0.01});
  const VecX bad_conv = vec({1.5, 1.0}); // 1.5 ∉ [0,1]
  const FactorModel m = diag_model(d);
  EXPECT_DEATH((void)kelly_size(mu, m, bad_conv, {}), ".*");
}

} // namespace atxtest_risk_kelly_sizing_test
