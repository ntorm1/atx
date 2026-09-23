// risk_attribution_test.cpp — L7: realized factor P&L attribution + TE check.
//
// Covers risk/attribution.hpp: the factor / specific / cost / borrow components sum
// to the total P&L within 1e-12; per-factor P&L is (Xᵀw)_k·f_k; the ledger folds
// days; risk_split matches FactorModel::risk; the realized-vs-predicted TE check is
// in band for a calibrated forecast and out of band for an understated one.

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/random.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/risk/attribution.hpp"
#include "atx/engine/risk/factor_model.hpp"

namespace atx_test_l7_riskmodel_attribution {

using atx::f64;
using atx::usize;
using atx::core::linalg::MatX;
using atx::core::linalg::VecX;
using atx::engine::risk::attribute;
using atx::engine::risk::Attribution;
using atx::engine::risk::AttributionLedger;
using atx::engine::risk::FactorModel;
using atx::engine::risk::risk_split;
using atx::engine::risk::TrackingErrorCheck;

struct Day {
  MatX x;
  std::vector<f64> w;
  std::vector<f64> f;
  std::vector<f64> r;
};

Day make_day(usize m, usize k, std::uint64_t seed) {
  atx::core::Xoshiro256pp rng{seed};
  Day d{MatX(static_cast<Eigen::Index>(m), static_cast<Eigen::Index>(k)), {}, {}, {}};
  for (usize c = 0; c < k; ++c) {
    d.f.push_back(0.01 * rng.normal());
  }
  for (usize i = 0; i < m; ++i) {
    f64 fit = 0.0;
    for (usize c = 0; c < k; ++c) {
      const f64 v = rng.normal();
      d.x(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(c)) = v;
      fit += v * d.f[c];
    }
    d.r.push_back(fit + 0.02 * rng.normal());
    d.w.push_back(rng.normal() / static_cast<f64>(m));
  }
  return d;
}

TEST(RiskAttribution, ComponentsSumToTotalWithin1e12) {
  for (std::uint64_t seed = 1U; seed <= 25U; ++seed) {
    const Day d = make_day(500, 12, seed);
    const Attribution a = attribute(d.w, d.x, d.f, d.r, 1.3e-4, 0.7e-4);
    EXPECT_NEAR(a.component_sum(), a.total, 1e-12) << "seed " << seed;
    f64 gross = 0.0;
    for (usize i = 0; i < d.w.size(); ++i) {
      gross += d.w[i] * d.r[i];
    }
    EXPECT_NEAR(a.gross, gross, 1e-15);
    EXPECT_NEAR(a.total, gross - 1.3e-4 - 0.7e-4, 1e-15);
  }
}

TEST(RiskAttribution, PerFactorPnlIsExposureTimesReturn) {
  const Day d = make_day(40, 3, 77U);
  const Attribution a = attribute(d.w, d.x, d.f, d.r, 0.0, 0.0);
  ASSERT_EQ(a.factor.size(), 3U);
  for (usize c = 0; c < 3U; ++c) {
    f64 e = 0.0;
    for (usize i = 0; i < 40U; ++i) {
      e += d.x(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(c)) * d.w[i];
    }
    EXPECT_NEAR(a.factor[c], e * d.f[c], 1e-15);
  }
  // A pure factor book with zero residual has zero specific P&L.
  Day z = d;
  for (usize i = 0; i < 40U; ++i) {
    f64 fit = 0.0;
    for (usize c = 0; c < 3U; ++c) {
      fit += z.x(static_cast<Eigen::Index>(i), static_cast<Eigen::Index>(c)) * z.f[c];
    }
    z.r[i] = fit;
  }
  EXPECT_NEAR(attribute(z.w, z.x, z.f, z.r, 0.0, 0.0).specific, 0.0, 1e-16);
}

TEST(RiskAttribution, HaltedNameContributesNothing) {
  Day d = make_day(10, 2, 5U);
  const Attribution before = attribute(d.w, d.x, d.f, d.r, 0.0, 0.0);
  d.r[4] = std::numeric_limits<f64>::quiet_NaN();
  const Attribution after = attribute(d.w, d.x, d.f, d.r, 0.0, 0.0);
  EXPECT_FALSE(std::isnan(after.total));
  EXPECT_NE(before.total, after.total);
  EXPECT_NEAR(after.component_sum(), after.total, 1e-15);
}

TEST(RiskAttribution, LedgerFoldsDays) {
  AttributionLedger led;
  f64 tot = 0.0;
  f64 f0 = 0.0;
  for (std::uint64_t s = 1U; s <= 30U; ++s) {
    const Day d = make_day(50, 4, 100U + s);
    const Attribution a = attribute(d.w, d.x, d.f, d.r, 1e-5, 2e-6);
    tot += a.total;
    f0 += a.factor[0];
    led.add(a);
  }
  EXPECT_EQ(led.days(), 30U);
  EXPECT_NEAR(led.cumulative().total, tot, 1e-15);
  EXPECT_NEAR(led.cumulative().factor[0], f0, 1e-15);
  EXPECT_NEAR(led.cumulative().component_sum(), led.cumulative().total, 1e-12);
  EXPECT_NEAR(led.cumulative().cost, 30.0 * 1e-5, 1e-15);
}

TEST(RiskAttribution, RiskSplitMatchesModelRisk) {
  atx::core::Xoshiro256pp rng{9U};
  const Eigen::Index m = 60;
  const Eigen::Index k = 4;
  MatX x(m, k);
  VecX d(m);
  for (Eigen::Index i = 0; i < m; ++i) {
    for (Eigen::Index c = 0; c < k; ++c) {
      x(i, c) = rng.normal();
    }
    d[i] = 1e-4 * (1.0 + rng.uniform01());
  }
  MatX a(k, k);
  for (Eigen::Index r = 0; r < k; ++r) {
    for (Eigen::Index c = 0; c < k; ++c) {
      a(r, c) = 0.01 * rng.normal();
    }
  }
  const MatX f = a * a.transpose() + 1e-5 * MatX::Identity(k, k);
  const auto model = FactorModel::create(x, f, d, 0U, 1U);
  ASSERT_TRUE(model);
  std::vector<f64> w(static_cast<usize>(m));
  for (f64 &v : w) {
    v = rng.normal();
  }
  const auto s = risk_split(*model, w);
  EXPECT_NEAR(s.total_var(), model->risk(w), 1e-14 * model->risk(w));
  EXPECT_GT(s.factor_var, 0.0);
  EXPECT_GT(s.specific_var, 0.0);
}

TEST(RiskAttribution, TrackingErrorCheckBand) {
  atx::core::Xoshiro256pp rng{11U};
  TrackingErrorCheck good;
  TrackingErrorCheck under;
  for (int t = 0; t < 1000; ++t) {
    const f64 sigma = 0.01 * (1.0 + 0.5 * std::sin(0.01 * t)); // time-varying TE
    const f64 r = sigma * rng.normal();
    good.add(r, sigma);
    under.add(r, 0.6 * sigma); // model understates risk by 40%
  }
  EXPECT_EQ(good.n(), 1000U);
  EXPECT_TRUE(good.in_band()) << good.bias();
  EXPECT_FALSE(under.in_band());
  EXPECT_NEAR(under.bias(), 1.0 / 0.6, 0.1);
  EXPECT_NEAR(good.band_half_width(), std::sqrt(2.0 / 1000.0), 1e-15);
  good.add(1.0, 0.0); // ignored (non-positive σ̂)
  EXPECT_EQ(good.n(), 1000U);
}

} // namespace atx_test_l7_riskmodel_attribution
