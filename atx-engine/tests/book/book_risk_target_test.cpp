// book_risk_target_test.cpp — platform v8 R-8: the ex-ante risk target risk-target-v1
// (atx::engine::book, risk_target.hpp).
//
//   factor_variance against the dense w'(B F B' + D)w; sigma_hat (gross_one_vol) and L_t in
//   closed form on a synthetic covariance, free of the book's scale and scaled down by the gross
//   of names the model does not price; the clip [.8 L, 1.25 L] at both ends and inside; the
//   cadence (first estimable decision, held for 20 sessions, re-estimated on the 21st, a due
//   decision with a flat book keeps the clock); parameter and model refusals.
//
// Suite: BookRiskTarget

#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/risk_target.hpp"

namespace atx_test_v8_book_risk_target {

using atx::f64;
using atx::u32;
using atx::usize;
namespace eb = atx::engine::book;

constexpr f64 kNaN = std::numeric_limits<f64>::quiet_NaN();

// A factor model held by value; view() lends it to the engine.
struct Model {
  usize groups{};
  usize styles{};
  std::vector<u32> group;
  std::vector<f64> exposures, covariance, specific;
  [[nodiscard]] eb::FactorRiskView view() const {
    return eb::FactorRiskView{groups, styles, group, exposures, covariance, specific};
  }
};

// Intercept only (K = 1, market variance 1e-4), two names with specific variance 1e-5.
Model market_model() {
  Model m;
  m.group = {0U, 0U};
  m.covariance = {1e-4};
  m.specific = {1e-5, 1e-5};
  return m;
}

// Four names, two groups, one style: K = 4, F = A A' + diag (symmetric positive definite).
Model dense_model() {
  Model m;
  m.groups = 2U;
  m.styles = 1U;
  m.group = {1U, 2U, 0U, 1U};
  m.exposures = {0.5, -1.0, 2.0, 0.25};
  m.specific = {1e-4, 2e-4, 3e-4, 4e-4};
  const f64 a[4][4] = {{1e-2, 2e-3, -1e-3, 5e-4},
                       {2e-3, 8e-3, 1e-3, -2e-3},
                       {-1e-3, 1e-3, 6e-3, 1e-3},
                       {5e-4, -2e-3, 1e-3, 4e-3}};
  m.covariance.assign(16U, 0.0);
  for (usize r = 0U; r < 4U; ++r) {
    for (usize c = 0U; c < 4U; ++c) {
      f64 v = r == c ? 1e-6 : 0.0;
      for (usize k = 0U; k < 4U; ++k) {
        v += a[r][k] * a[c][k];
      }
      m.covariance[r * 4U + c] = v;
    }
  }
  return m;
}

// w'(B F B' + D)w with B formed densely: row i = [1, group == 1, group == 2, x_i].
f64 dense_variance(const Model &m, const std::vector<f64> &w) {
  const usize n = w.size();
  const usize k = 1U + m.groups + m.styles;
  std::vector<f64> b(n * k, 0.0);
  for (usize i = 0U; i < n; ++i) {
    b[i * k] = 1.0;
    if (m.group[i] != 0U) {
      b[i * k + m.group[i]] = 1.0;
    }
    for (usize c = 0U; c < m.styles; ++c) {
      b[i * k + 1U + m.groups + c] = m.exposures[i * m.styles + c];
    }
  }
  f64 total = 0.0;
  for (usize i = 0U; i < n; ++i) {
    for (usize j = 0U; j < n; ++j) {
      f64 sigma = i == j ? m.specific[i] : 0.0;
      for (usize p = 0U; p < k; ++p) {
        for (usize q = 0U; q < k; ++q) {
          sigma += b[i * k + p] * m.covariance[p * k + q] * b[j * k + q];
        }
      }
      total += w[i] * sigma * w[j];
    }
  }
  return total;
}

TEST(BookRiskTarget, FactorVarianceEqualsTheDenseQuadraticForm) {
  const Model m = dense_model();
  const std::vector<f64> w{0.3, -0.2, 0.1, -0.4};
  const auto v = eb::factor_variance(m.view(), w);
  ASSERT_TRUE(v) << v.error().to_string();
  const f64 dense = dense_variance(m, w);
  ASSERT_GT(dense, 0.0);
  EXPECT_NEAR(*v, dense, 1e-12 * dense);
  // A zero weight adds nothing (skipped), the rest unchanged.
  const std::vector<f64> sparse{0.3, 0.0, 0.1, -0.4};
  const auto s = eb::factor_variance(m.view(), sparse);
  ASSERT_TRUE(s);
  EXPECT_NEAR(*s, dense_variance(m, sparse), 1e-12 * dense);
}

// The scaler on a synthetic covariance, in closed form: w = (.5, -.5) has no market exposure,
// so w'Sigma w = 1e-5 (.25 + .25); sigma_hat = sqrt(252 x 5e-6) (~ .0355) and, at S .05, b
// 1.15 and L 1.2, L_t = S / (b sigma_hat) (~ 1.225) inside [.96, 1.5].
TEST(BookRiskTarget, SigmaHatAndLeverageInClosedForm) {
  const Model m = market_model();
  const std::vector<f64> w{0.5, -0.5};
  const auto sigma = eb::gross_one_vol(m.view(), w, 1.0);
  ASSERT_TRUE(sigma) << sigma.error().to_string();
  const f64 expected = std::sqrt(252.0 * (1e-5 * 0.25 + 1e-5 * 0.25));
  EXPECT_DOUBLE_EQ(*sigma, expected);
  const eb::RiskTargetParams p{0.05, 1.15, 21U};
  const auto at = eb::risk_target_leverage(p, *sigma, 1.2);
  EXPECT_DOUBLE_EQ(at.raw, 0.05 / (1.15 * expected));
  EXPECT_EQ(at.clip, eb::RiskTargetClip::None);
  EXPECT_EQ(at.leverage, at.raw);
  EXPECT_GT(at.leverage, 0.8 * 1.2);
  EXPECT_LT(at.leverage, 1.25 * 1.2);
  // With market exposure: w = (.6, -.2) at gross .8 is (.75, -.25) at gross 1, e = .5.
  const std::vector<f64> tilted{0.6, -0.2};
  const auto tilted_sigma = eb::gross_one_vol(m.view(), tilted, 0.8);
  ASSERT_TRUE(tilted_sigma);
  const f64 variance = 0.5 * 1e-4 * 0.5 + 1e-5 * (0.75 * 0.75 + 0.25 * 0.25);
  EXPECT_NEAR(*tilted_sigma, std::sqrt(252.0 * variance), 1e-14);
}

// The book's scale does not move sigma_hat; gross held in names the model does not price
// scales the priced book down (here by 2: half the gross is unpriced).
TEST(BookRiskTarget, SigmaHatIsScaleFreeAndReadsTheWholeBookGross) {
  const Model m = market_model();
  const std::vector<f64> unit{0.5, -0.5}, triple{1.5, -1.5};
  const auto a = eb::gross_one_vol(m.view(), unit, 1.0);
  const auto b = eb::gross_one_vol(m.view(), triple, 3.0);
  const auto half = eb::gross_one_vol(m.view(), unit, 2.0);
  ASSERT_TRUE(a);
  ASSERT_TRUE(b);
  ASSERT_TRUE(half);
  EXPECT_EQ(*a, *b);
  EXPECT_DOUBLE_EQ(*half, 0.5 * *a);
}

TEST(BookRiskTarget, ClipHoldsTheLeverageInsidePoint8ToOnePoint25L) {
  const eb::RiskTargetParams p{0.05, 1.15, 21U};
  const f64 base = 1.247;
  const auto high = eb::risk_target_leverage(p, 1e-4, base); // raw ~ 435 L
  EXPECT_EQ(high.clip, eb::RiskTargetClip::High);
  EXPECT_EQ(high.leverage, eb::risk_target_clip_hi * base);
  EXPECT_GT(high.raw, high.leverage);
  const auto low = eb::risk_target_leverage(p, 10.0, base); // raw ~ .004
  EXPECT_EQ(low.clip, eb::RiskTargetClip::Low);
  EXPECT_EQ(low.leverage, eb::risk_target_clip_lo * base);
  EXPECT_LT(low.raw, low.leverage);
  // At the lower bound's sigma_hat the leverage is the bound (to rounding), clipped or not.
  const f64 at_lo = p.sigma_star / (p.bias * eb::risk_target_clip_lo * base);
  const auto edge = eb::risk_target_leverage(p, at_lo, base);
  EXPECT_NEAR(edge.leverage, eb::risk_target_clip_lo * base, 1e-14);
  // A sweep: always inside [lo L, hi L], non-increasing in sigma_hat, the raw value inside.
  f64 previous = std::numeric_limits<f64>::infinity();
  for (f64 sigma = 1e-3; sigma < 1.0; sigma *= 1.1) {
    const auto at = eb::risk_target_leverage(p, sigma, base);
    EXPECT_GE(at.leverage, eb::risk_target_clip_lo * base) << sigma;
    EXPECT_LE(at.leverage, eb::risk_target_clip_hi * base) << sigma;
    EXPECT_LE(at.leverage, previous) << sigma;
    if (at.clip == eb::RiskTargetClip::None) {
      EXPECT_EQ(at.leverage, at.raw) << sigma;
    }
    previous = at.leverage;
  }
}

// The cadence: no estimate on a flat book (L in force, the clock not started), the first on the
// next decision with a book, held for 20 sessions whatever the book does, re-estimated on the
// 21st; a due decision with a flat book keeps the clock, so the next one estimates.
TEST(BookRiskTarget, CadenceEstimatesEvery21SessionsFromTheFirstEstimableDecision) {
  const Model m = market_model();
  const eb::RiskTargetParams p{0.05, 1.15, 21U};
  const f64 base = 1.2;
  const std::vector<f64> flat{0.0, 0.0}, book{0.5, -0.5}, tilted{0.6, -0.2};
  eb::RiskTargetState s;
  EXPECT_TRUE(eb::risk_target_due(s, 0U, p.cadence));
  auto r = eb::risk_target_update(p, base, 3U, m.view(), flat, 0.0, s);
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_FALSE(*r);
  EXPECT_FALSE(s.estimated);
  EXPECT_EQ(eb::risk_target_in_force(s, base), base);
  r = eb::risk_target_update(p, base, 4U, m.view(), book, 1.0, s);
  ASSERT_TRUE(r);
  EXPECT_TRUE(*r);
  EXPECT_EQ(s.last, 4U);
  const f64 first = s.at.leverage;
  EXPECT_EQ(eb::risk_target_in_force(s, base), first);
  EXPECT_EQ(s.sigma_hat, *eb::gross_one_vol(m.view(), book, 1.0));
  for (usize t = 5U; t < 25U; ++t) { // the tilted book would give another estimate
    EXPECT_FALSE(eb::risk_target_due(s, t, p.cadence)) << t;
    r = eb::risk_target_update(p, base, t, m.view(), tilted, 0.8, s);
    ASSERT_TRUE(r);
    EXPECT_FALSE(*r) << t;
    EXPECT_EQ(s.last, 4U);
    EXPECT_EQ(eb::risk_target_in_force(s, base), first) << t;
  }
  r = eb::risk_target_update(p, base, 25U, m.view(), tilted, 0.8, s);
  ASSERT_TRUE(r);
  EXPECT_TRUE(*r);
  EXPECT_EQ(s.last, 25U);
  EXPECT_EQ(s.sigma_hat, *eb::gross_one_vol(m.view(), tilted, 0.8));
  EXPECT_NE(eb::risk_target_in_force(s, base), first);
  // Due at 46 but flat: no estimate, the clock stays at 25; 47 estimates.
  r = eb::risk_target_update(p, base, 46U, m.view(), flat, 0.0, s);
  ASSERT_TRUE(r);
  EXPECT_FALSE(*r);
  EXPECT_EQ(s.last, 25U);
  EXPECT_TRUE(eb::risk_target_due(s, 47U, p.cadence));
  r = eb::risk_target_update(p, base, 47U, m.view(), book, 1.0, s);
  ASSERT_TRUE(r);
  EXPECT_TRUE(*r);
  EXPECT_EQ(s.last, 47U);
  EXPECT_EQ(eb::risk_target_in_force(s, base), first);
}

TEST(BookRiskTarget, RefusesParametersOutsideTheirRanges) {
  EXPECT_TRUE(eb::validate_risk_target(eb::RiskTargetParams{0.05, 1.15, 21U}));
  EXPECT_TRUE(eb::validate_risk_target(eb::RiskTargetParams{1.0, 10.0, 1U}));
  for (const eb::RiskTargetParams bad :
       {eb::RiskTargetParams{0.0, 1.15, 21U}, eb::RiskTargetParams{-0.05, 1.15, 21U},
        eb::RiskTargetParams{kNaN, 1.15, 21U}, eb::RiskTargetParams{1.5, 1.15, 21U},
        eb::RiskTargetParams{0.05, 0.0, 21U}, eb::RiskTargetParams{0.05, kNaN, 21U},
        eb::RiskTargetParams{0.05, 11.0, 21U}, eb::RiskTargetParams{0.05, 1.15, 0U},
        eb::RiskTargetParams{0.05, 1.15, 10001U}}) {
    const auto v = eb::validate_risk_target(bad);
    ASSERT_FALSE(v);
    EXPECT_EQ(v.error().code(), atx::core::ErrorCode::InvalidArgument);
  }
}

TEST(BookRiskTarget, RefusesMalformedModelsAndGrossBeforeAnyEstimate) {
  const Model good = market_model();
  const std::vector<f64> w{0.5, -0.5};
  const auto refused = [](const atx::core::Result<f64> &r) {
    return !r && r.error().code() == atx::core::ErrorCode::InvalidArgument;
  };
  Model sizes = good;
  sizes.specific.push_back(1e-5);
  EXPECT_TRUE(refused(eb::factor_variance(sizes.view(), w)));
  Model group = dense_model();
  group.group[1] = 3U; // only 2 groups
  EXPECT_TRUE(refused(eb::factor_variance(group.view(), std::vector<f64>{0.1, 0.1, 0.1, 0.1})));
  Model negative = good;
  negative.specific[0] = -1e-5;
  EXPECT_TRUE(refused(eb::factor_variance(negative.view(), w)));
  Model nan_cov = good;
  nan_cov.covariance[0] = kNaN;
  EXPECT_TRUE(refused(eb::factor_variance(nan_cov.view(), w)));
  EXPECT_TRUE(refused(eb::factor_variance(good.view(), std::vector<f64>{kNaN, 0.5})));
  EXPECT_TRUE(refused(eb::gross_one_vol(good.view(), w, 0.5))); // below sum |w| = 1
  EXPECT_TRUE(refused(eb::gross_one_vol(good.view(), w, kNaN)));
  EXPECT_TRUE(refused(eb::gross_one_vol(good.view(), w, 1.0, 0.0)));
  const auto flat = eb::gross_one_vol(good.view(), std::vector<f64>{0.0, 0.0}, 0.0);
  ASSERT_FALSE(flat);
  EXPECT_EQ(flat.error().code(), atx::core::ErrorCode::Unavailable);
  // The update refuses a bad base or bad parameters and leaves the state alone.
  eb::RiskTargetState s;
  const eb::RiskTargetParams p{0.05, 1.15, 21U};
  const auto bad_base = eb::risk_target_update(p, 0.0, 0U, good.view(), w, 1.0, s);
  ASSERT_FALSE(bad_base);
  EXPECT_EQ(bad_base.error().code(), atx::core::ErrorCode::InvalidArgument);
  const auto bad_p =
      eb::risk_target_update(eb::RiskTargetParams{0.0, 1.15, 21U}, 1.2, 0U, good.view(), w, 1.0, s);
  ASSERT_FALSE(bad_p);
  const auto bad_model = eb::risk_target_update(p, 1.2, 0U, sizes.view(), w, 1.0, s);
  ASSERT_FALSE(bad_model);
  EXPECT_FALSE(s.estimated);
  EXPECT_TRUE(std::isnan(s.sigma_hat));
}

} // namespace atx_test_v8_book_risk_target
