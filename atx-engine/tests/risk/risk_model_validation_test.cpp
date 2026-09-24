// risk_model_validation_test.cpp — L7: the risk-model validation scorecard.
//
// Covers risk/model_validation.hpp: on a correctly specified simulated model the bias
// statistics fall inside the 95% band (equal-weight, random books, min-variance,
// asset level) and Q ≈ 1 + ln2 + γ; an understated model is flagged; the JSON
// scorecard is deterministic; three candidate hybrid configs each produce a scorecard,
// and on a world with a planted latent factor a latent-mimicking book is badly
// understated by the fundamental-only model but calibrated under the hybrid models.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/factor_model.hpp"
#include "atx/engine/risk/model_validation.hpp"

namespace atx_test_l7_riskmodel_validation {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using namespace atx::engine::risk; // NOLINT(google-build-using-namespace) test-local

struct TrueWorld {
  MatX x;
  MatX f;
  VecX d;
  ReturnPanel ret;
};

// Returns drawn i.i.d. from V = X F Xᵀ + D.
TrueWorld true_world(usize n, usize k, usize t, std::uint64_t seed) {
  atx::core::Xoshiro256pp rng{seed};
  TrueWorld w;
  w.x.resize(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(k));
  w.d.resize(static_cast<Eigen::Index>(n));
  for (Eigen::Index i = 0; i < w.x.rows(); ++i) {
    w.x(i, 0) = 1.0;
    for (Eigen::Index c = 1; c < w.x.cols(); ++c) {
      w.x(i, c) = rng.normal();
    }
    const f64 s = 0.01 + 0.02 * rng.uniform01();
    w.d[i] = s * s;
  }
  MatX a(static_cast<Eigen::Index>(k), static_cast<Eigen::Index>(k));
  for (Eigen::Index r = 0; r < a.rows(); ++r) {
    for (Eigen::Index c = 0; c < a.cols(); ++c) {
      a(r, c) = 0.005 * rng.normal();
    }
  }
  w.f = a * a.transpose() + 2.5e-5 * MatX::Identity(a.rows(), a.cols());
  const MatX l = w.f.llt().matrixL();
  w.ret.r.resize(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(n));
  for (Eigen::Index d = 0; d < w.ret.r.rows(); ++d) {
    VecX z(static_cast<Eigen::Index>(k));
    for (Eigen::Index c = 0; c < z.size(); ++c) {
      z[c] = rng.normal();
    }
    const VecX fr = l * z;
    for (Eigen::Index i = 0; i < w.x.rows(); ++i) {
      w.ret.r(d, i) = w.x.row(i).dot(fr) + std::sqrt(w.d[i]) * rng.normal();
    }
  }
  return w;
}

RiskModelFactory fixed_factory(const TrueWorld &w, f64 scale) {
  return [&w, scale](usize) -> atx::core::Result<ModelSnapshot> {
    auto m = FactorModel::create(w.x, w.f * scale, w.d * scale, 0U, 1U);
    if (!m) {
      return atx::core::Err(m.error().code(), "fixed_factory");
    }
    std::vector<usize> assets(static_cast<usize>(w.x.rows()));
    for (usize i = 0; i < assets.size(); ++i) {
      assets[i] = i;
    }
    return atx::core::Ok(ModelSnapshot{std::move(*m), std::move(assets)});
  };
}

TEST(RiskModelValidation, CorrectlySpecifiedModelIsInBand) {
  const TrueWorld w = true_world(150, 4, 420, 17U);
  ValidationCfg cfg;
  cfg.first_as_of = 1U;
  cfg.n_periods = 400U;
  cfg.n_random = 40U;
  const auto sc = validate_risk_model(fixed_factory(w, 1.0), w.ret, cfg);
  ASSERT_TRUE(sc) << sc.error().message();
  EXPECT_EQ(sc->n_periods, 400U);
  EXPECT_GE(sc->equal_weight.bias, sc->band_lo);
  EXPECT_LE(sc->equal_weight.bias, sc->band_hi);
  EXPECT_TRUE(sc->equal_weight.in_band);
  EXPECT_TRUE(sc->min_variance.in_band) << sc->min_variance.bias;
  EXPECT_GE(sc->random_in_band_frac, 0.85);
  EXPECT_NEAR(sc->random_bias_mean, 1.0, 0.03);
  EXPECT_NEAR(sc->random_q_mean, 1.0 + std::log(2.0) + 0.5772156649, 0.12);
  EXPECT_EQ(sc->n_assets_scored, 150U);
  EXPECT_NEAR(sc->asset_bias_mean, 1.0, 0.02);
  EXPECT_LT(sc->asset_mrad, 0.06);
  EXPECT_DOUBLE_EQ(sc->mean_factors, 4.0);
}

TEST(RiskModelValidation, UnderstatedModelIsFlagged) {
  const TrueWorld w = true_world(120, 3, 320, 23U);
  ValidationCfg cfg;
  cfg.n_periods = 300U;
  const auto sc = validate_risk_model(fixed_factory(w, 0.5), w.ret, cfg);
  ASSERT_TRUE(sc);
  EXPECT_FALSE(sc->equal_weight.in_band);
  EXPECT_NEAR(sc->equal_weight.bias, std::sqrt(2.0), 0.15);
  EXPECT_LT(sc->random_in_band_frac, 0.1);
  EXPECT_NEAR(sc->asset_bias_mean, std::sqrt(2.0), 0.05);
}

TEST(RiskModelValidation, JsonScorecardIsDeterministicAndComplete) {
  const TrueWorld w = true_world(60, 3, 120, 29U);
  ValidationCfg cfg;
  cfg.label = "truth";
  cfg.n_periods = 100U;
  const auto a = validate_risk_model(fixed_factory(w, 1.0), w.ret, cfg);
  const auto b = validate_risk_model(fixed_factory(w, 1.0), w.ret, cfg);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  const std::string ja = a->to_json();
  EXPECT_EQ(ja, b->to_json());
  for (const char *key : {"\"label\":\"truth\"", "\"n_periods\":100", "\"band\":[",
                          "\"equal_weight\":{", "\"min_variance\":{", "\"random\":{",
                          "\"asset\":{", "\"mean_factors\":", "\"q\":"}) {
    EXPECT_NE(ja.find(key), std::string::npos) << key << " in " << ja;
  }
  EXPECT_EQ(ja.find("null"), std::string::npos);
  EXPECT_EQ(ja.front(), '{');
  EXPECT_EQ(ja.back(), '}');
}

TEST(RiskModelValidation, JsonStringsAreEscaped) {
  EXPECT_EQ(json_quote("C:\\atx\\data"), "\"C:\\\\atx\\\\data\"");
  EXPECT_EQ(json_quote("a\"b"), "\"a\\\"b\"");
  EXPECT_EQ(json_quote("x\ny\t\x01"), "\"x\\ny\\t\\u0001\"");
  const TrueWorld w = true_world(30, 2, 40, 37U);
  ValidationCfg cfg;
  cfg.label = "C:\\runs\\\"q\"";
  cfg.n_periods = 20U;
  const auto sc = validate_risk_model(fixed_factory(w, 1.0), w.ret, cfg);
  ASSERT_TRUE(sc);
  EXPECT_NE(sc->to_json().find("\"label\":\"C:\\\\runs\\\\\\\"q\\\"\""), std::string::npos)
      << sc->to_json();
}

// A name with no realized return at a−1 (it left the universe) is dropped from the
// books, so the equal-weight book scores exactly like a model without that name.
TEST(RiskModelValidation, NanReturnNamesAreDroppedFromBooks) {
  TrueWorld w = true_world(80, 3, 140, 43U);
  for (Eigen::Index d = 0; d < w.ret.r.rows(); ++d) {
    w.ret.r(d, 0) = std::numeric_limits<f64>::quiet_NaN();
  }
  TrueWorld sub;
  sub.x = w.x.bottomRows(79);
  sub.f = w.f;
  sub.d = w.d.tail(79);
  sub.ret.r = w.ret.r.rightCols(79);
  ValidationCfg cfg;
  cfg.n_periods = 120U;
  cfg.n_random = 5U;
  cfg.n_optimized = 5U;
  const auto full = validate_risk_model(fixed_factory(w, 1.0), w.ret, cfg);
  const auto ref = validate_risk_model(fixed_factory(sub, 1.0), sub.ret, cfg);
  ASSERT_TRUE(full);
  ASSERT_TRUE(ref);
  EXPECT_DOUBLE_EQ(full->mean_excluded, 1.0);
  EXPECT_DOUBLE_EQ(ref->mean_excluded, 0.0);
  EXPECT_EQ(full->equal_weight.n, ref->equal_weight.n);
  EXPECT_NEAR(full->equal_weight.bias, ref->equal_weight.bias, 1e-9);
  EXPECT_NEAR(full->equal_weight.mean_pred_vol, ref->equal_weight.mean_pred_vol, 1e-12);
  EXPECT_NE(full->to_json().find("\"mean_excluded\":1"), std::string::npos);
}

TEST(RiskModelValidation, RejectsBadConfigAndPropagatesFactoryErrors) {
  const TrueWorld w = true_world(20, 2, 30, 31U);
  ValidationCfg cfg;
  cfg.first_as_of = 0U;
  EXPECT_FALSE(validate_risk_model(fixed_factory(w, 1.0), w.ret, cfg));
  cfg.first_as_of = 1U;
  cfg.n_periods = 30U; // last as_of == 30 >= 30 dates
  EXPECT_FALSE(validate_risk_model(fixed_factory(w, 1.0), w.ret, cfg));
  cfg.n_periods = 5U;
  const RiskModelFactory failing = [](usize) -> atx::core::Result<ModelSnapshot> {
    return atx::core::Err(atx::core::ErrorCode::Internal, "boom");
  };
  EXPECT_FALSE(validate_risk_model(failing, w.ret, cfg));
}

// A fundamental world with a planted latent factor (hybrid acceptance).
struct LatentWorld {
  ReturnPanel ret;
  ExposureSeries exp;
};

LatentWorld latent_world(usize n, usize t, std::uint64_t seed, VecX &v) {
  atx::core::Xoshiro256pp rng{seed};
  LatentWorld w;
  w.exp.market = true;
  w.exp.n_industries = 4U;
  MatX style(static_cast<Eigen::Index>(n), 2);
  v.resize(static_cast<Eigen::Index>(n));
  for (usize i = 0; i < n; ++i) {
    w.exp.industry.push_back(static_cast<u32>(i % 4U));
    w.exp.cap.push_back(1e9 * std::exp(rng.normal()));
    style(static_cast<Eigen::Index>(i), 0) = rng.normal();
    style(static_cast<Eigen::Index>(i), 1) = rng.normal();
    v[static_cast<Eigen::Index>(i)] = rng.normal();
  }
  w.exp.style.push_back(style);
  w.ret.r.resize(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(n));
  for (Eigen::Index d = 0; d < w.ret.r.rows(); ++d) {
    const f64 fm = 0.01 * rng.normal();
    f64 fi[4];
    for (f64 &x : fi) {
      x = 0.005 * rng.normal();
    }
    const f64 fs0 = 0.004 * rng.normal();
    const f64 fs1 = 0.004 * rng.normal();
    const f64 h = 0.012 * rng.normal();
    for (Eigen::Index i = 0; i < w.ret.r.cols(); ++i) {
      w.ret.r(d, i) = fm + fi[static_cast<usize>(i) % 4U] + style(i, 0) * fs0 +
                      style(i, 1) * fs1 + v[i] * h + 0.015 * rng.normal();
    }
  }
  return w;
}

TEST(RiskModelValidation, ThreeCandidateConfigsProduceScorecards) {
  VecX v;
  const LatentWorld w = latent_world(150, 150, 41U, v);
  HybridCfg fund_only;
  fund_only.window = 80U;
  fund_only.select = StatFactorSelect::Fixed;
  fund_only.n_stat_fixed = 0U;
  HybridCfg hybrid = fund_only;
  hybrid.select = StatFactorSelect::BaiNgIc2;
  HybridCfg hybrid_ewma = hybrid;
  hybrid_ewma.vol_halflife = 40U;
  hybrid_ewma.corr_halflife = 80U;
  hybrid_ewma.spec_halflife = 40U;
  // A latent-mimicking book (w ∝ v): the fundamental-only model books its latent risk
  // as diversifiable noise and understates it; the hybrid sees the factor.
  NamedBook mimic{"latent_mimic", std::vector<f64>(v.data(), v.data() + v.size())};
  std::vector<ValidationScorecard> cards;
  const std::vector<std::pair<std::string, HybridCfg>> configs = {
      {"fundamental", fund_only}, {"hybrid_baing", hybrid}, {"hybrid_ewma", hybrid_ewma}};
  for (const auto &[label, hc] : configs) {
    ValidationCfg vc;
    vc.label = label;
    vc.first_as_of = 1U;
    vc.n_periods = 40U;
    vc.n_random = 20U;
    vc.n_optimized = 10U;
    vc.books = {mimic};
    const auto sc = validate_risk_model(hybrid_model_factory(w.ret, w.exp, hc), w.ret, vc);
    ASSERT_TRUE(sc) << label << ": " << sc.error().message();
    const std::string js = sc->to_json();
    EXPECT_NE(js.find("\"label\":\"" + label + "\""), std::string::npos);
    EXPECT_NE(js.find("\"latent_mimic\""), std::string::npos);
    ASSERT_EQ(sc->books.size(), 1U);
    cards.push_back(*sc);
  }
  EXPECT_DOUBLE_EQ(cards[0].mean_factors, 1.0 + 3.0 + 2.0);
  EXPECT_GT(cards[1].mean_factors, cards[0].mean_factors); // latent factor found
  EXPECT_GT(cards[0].books[0].bias, 2.0);                  // understated latent risk
  EXPECT_LT(std::abs(cards[1].books[0].bias - 1.0), 0.35);
  EXPECT_LT(std::abs(cards[2].books[0].bias - 1.0), 0.35);
}

} // namespace atx_test_l7_riskmodel_validation
