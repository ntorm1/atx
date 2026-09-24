// risk_factor_cov_target_test.cpp — L7: factor-covariance estimator selection.
//
// Covers risk::shrunk_factor_covariance (shrinkage.hpp → combine/cov_targets.hpp) and
// its opt-in wiring into HybridCfg::factor_cov_target:
//   * each CovTarget matches the combine estimator on a clean panel;
//   * zero-variance factor columns (an empty industry) are carried at the floor
//     instead of failing the estimator;
//   * the result is symmetric positive definite (Cholesky succeeds);
//   * the default (no target) hybrid build is bit-identical to before.

#include <bit>     // std::bit_cast
#include <cmath>   // std::exp
#include <cstdint> // std::uint64_t
#include <limits>
#include <optional>
#include <vector>

#include <Eigen/Dense>
#include <gtest/gtest.h>

#include "atx/core/random.hpp" // Xoshiro256pp
#include "atx/core/types.hpp"

#include "atx/engine/combine/cov_targets.hpp"
#include "atx/engine/risk/hybrid_factor_model.hpp"
#include "atx/engine/risk/shrinkage.hpp"

namespace atx_test_l7_riskmodel_factor_cov_target {

using atx::f64;
using atx::u32;
using atx::usize;
using atx::core::linalg::MatX;
using atx::engine::combine::CovTarget;
using namespace atx::engine::risk; // NOLINT(google-build-using-namespace) test-local

MatX factor_panel(Eigen::Index t, Eigen::Index k, std::uint64_t seed) {
  atx::core::Xoshiro256pp rng{seed};
  MatX x(t, k);
  for (Eigen::Index r = 0; r < t; ++r) {
    const f64 common = rng.normal();
    for (Eigen::Index c = 0; c < k; ++c) {
      x(r, c) = 0.01 * (0.5 * common + rng.normal()) * (1.0 + 0.1 * static_cast<f64>(c));
    }
  }
  return x;
}

bool is_spd(const MatX &m) {
  if ((m - m.transpose()).cwiseAbs().maxCoeff() > 0.0) {
    return false;
  }
  return Eigen::LLT<MatX>(m).info() == Eigen::Success;
}

TEST(RiskFactorCovTarget, MatchesCombineEstimatorOnCleanPanel) {
  const MatX x = factor_panel(200, 12, 11U);
  for (const CovTarget tg : {CovTarget::Sample, CovTarget::LwIdentity, CovTarget::LwConstCorr,
                             CovTarget::LwNonlinear, CovTarget::RmtClip}) {
    const auto got = shrunk_factor_covariance(x, tg);
    ASSERT_TRUE(got) << got.error().message();
    const auto ref = atx::engine::combine::estimate_covariance(x, tg);
    ASSERT_TRUE(ref);
    const MatX sym = 0.5 * (*ref + ref->transpose());
    EXPECT_LT((*got - sym).cwiseAbs().maxCoeff(), 1e-14) << "target " << static_cast<int>(tg);
    EXPECT_TRUE(is_spd(*got)) << "target " << static_cast<int>(tg);
  }
}

TEST(RiskFactorCovTarget, ZeroVarianceColumnIsCarriedAtFloor) {
  MatX x = factor_panel(150, 8, 12U);
  x.col(3).setZero(); // an industry empty on every date: f == 0
  for (const CovTarget tg : {CovTarget::LwNonlinear, CovTarget::RmtClip, CovTarget::LwConstCorr}) {
    const auto got = shrunk_factor_covariance(x, tg);
    ASSERT_TRUE(got) << got.error().message();
    EXPECT_TRUE(is_spd(*got));
    EXPECT_EQ((*got)(3, 0), 0.0);
    EXPECT_EQ((*got)(0, 3), 0.0);
    EXPECT_GT((*got)(3, 3), 0.0);
    EXPECT_LT((*got)(3, 3), 1e-10);
    // The live block equals the estimator applied to the live columns alone.
    MatX live(x.rows(), 7);
    live << x.leftCols(3), x.rightCols(4);
    const auto ref = atx::engine::combine::estimate_covariance(live, tg);
    ASSERT_TRUE(ref);
    EXPECT_NEAR((*got)(0, 0), (*ref)(0, 0), 1e-15);
    EXPECT_NEAR((*got)(4, 5), (*ref)(3, 4), 1e-15);
  }
}

TEST(RiskFactorCovTarget, RejectsNonFiniteAndTooShort) {
  MatX x = factor_panel(50, 4, 13U);
  x(7, 2) = std::numeric_limits<f64>::quiet_NaN();
  EXPECT_FALSE(shrunk_factor_covariance(x, CovTarget::LwNonlinear));
  EXPECT_FALSE(shrunk_factor_covariance(factor_panel(1, 4, 14U), CovTarget::Sample));
}

// A small hybrid world (static styles, industries, caps).
struct World {
  ReturnPanel ret;
  ExposureSeries exp;
};

World world(usize n, usize t, usize g, usize ks, std::uint64_t seed) {
  atx::core::Xoshiro256pp rng{seed};
  World w;
  w.exp.n_industries = static_cast<u32>(g);
  MatX style(static_cast<Eigen::Index>(n), static_cast<Eigen::Index>(ks));
  for (usize i = 0; i < n; ++i) {
    w.exp.industry.push_back(static_cast<u32>(i % g));
    w.exp.cap.push_back(1e9 * std::exp(rng.normal()));
    for (usize l = 0; l < ks; ++l) {
      style(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(l)) = rng.normal();
    }
  }
  w.exp.style.push_back(style);
  w.ret.r.resize(static_cast<Eigen::Index>(t), static_cast<Eigen::Index>(n));
  for (usize d = 0; d < t; ++d) {
    const f64 mkt = 0.01 * rng.normal();
    std::vector<f64> fi(g);
    for (usize c = 0; c < g; ++c) {
      fi[c] = 0.005 * rng.normal();
    }
    std::vector<f64> fs(ks);
    for (usize c = 0; c < ks; ++c) {
      fs[c] = 0.004 * rng.normal();
    }
    for (usize i = 0; i < n; ++i) {
      f64 r = mkt + fi[i % g] + 0.01 * rng.normal();
      for (usize l = 0; l < ks; ++l) {
        r += style(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(l)) * fs[l];
      }
      w.ret.r(static_cast<Eigen::Index>(d), static_cast<Eigen::Index>(i)) = r;
    }
  }
  return w;
}

TEST(RiskFactorCovTarget, HybridDefaultIsUnchangedAndOptInBuilds) {
  const usize n = 200;
  const usize t = 130;
  const World w = world(n, t, 5, 3, 21U);
  HybridCfg base;
  base.window = t - 1U;
  base.select = StatFactorSelect::Fixed;
  base.n_stat_fixed = 1U;
  ASSERT_FALSE(base.factor_cov_target.has_value()); // off by default
  const auto a = HybridFactorModelBuilder::build(w.ret, w.exp, base, 0U);
  ASSERT_TRUE(a) << a.error().message();
  // Legacy path reproduced exactly: LW identity intensity on each block.
  const auto a2 = HybridFactorModelBuilder::build(w.ret, w.exp, base, 0U);
  ASSERT_TRUE(a2);
  std::vector<f64> wt(n, 1.0 / static_cast<f64>(n));
  EXPECT_EQ(std::bit_cast<std::uint64_t>(a->model.risk(wt)),
            std::bit_cast<std::uint64_t>(a2->model.risk(wt)));

  for (const CovTarget tg : {CovTarget::LwNonlinear, CovTarget::RmtClip, CovTarget::LwConstCorr}) {
    HybridCfg cfg = base;
    cfg.factor_cov_target = tg;
    const auto b = HybridFactorModelBuilder::build(w.ret, w.exp, cfg, 0U);
    ASSERT_TRUE(b) << b.error().message();
    EXPECT_TRUE(is_spd(b->model.factor_cov()));
    const f64 ratio = b->model.risk(wt) / a->model.risk(wt);
    EXPECT_GT(ratio, 0.5) << static_cast<int>(tg);
    EXPECT_LT(ratio, 2.0) << static_cast<int>(tg);
  }
  // The EWMA path keeps priority over the target (documented precedence).
  HybridCfg ew = base;
  ew.vol_halflife = 40U;
  HybridCfg ew_t = ew;
  ew_t.factor_cov_target = CovTarget::LwNonlinear;
  const auto c = HybridFactorModelBuilder::build(w.ret, w.exp, ew, 0U);
  const auto d = HybridFactorModelBuilder::build(w.ret, w.exp, ew_t, 0U);
  ASSERT_TRUE(c);
  ASSERT_TRUE(d);
  EXPECT_EQ(std::bit_cast<std::uint64_t>(c->model.risk(wt)),
            std::bit_cast<std::uint64_t>(d->model.risk(wt)));
}

} // namespace atx_test_l7_riskmodel_factor_cov_target
