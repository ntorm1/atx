// book_vol_target_test.cpp — platform v8 Y (lane YCOMB): volatility-managed leverage
// vol-target-v1 (atx::engine::book, vol_target.hpp).
//
//   L_t = clip(L x sigma_ref / sigma_hat, 1, L) on a one-name model whose forecast is known in
//   closed form (sigma_hat = sqrt(252 c)): the first estimate gives L bit for bit (ratio 1), the
//   running mean includes the current estimate, the cap and the floor bind, the cadence holds L_t
//   for 20 sessions and re-estimates on the 21st, a flat due decision keeps the clock and the
//   mean; the wrong rules (a mean that leaves out the current estimate, a variance ratio, no cap)
//   differ on the fixture; cap and model refusals leave the state untouched.
//
// Suite: BookVolTarget

#include <cmath>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/risk_target.hpp"
#include "atx/engine/book/vol_target.hpp"

namespace atx_test_v8_book_vol_target {

using atx::f64;
using atx::u32;
using atx::usize;
namespace eb = atx::engine::book;

// One name, intercept only (K = 1): factor variance c, no specific variance. With w = {1} and
// gross 1 the gross-1 variance is c, so sigma_hat = sqrt(252 c) exactly as gross_one_vol forms it.
struct OneName {
  std::vector<u32> group{0U};
  std::vector<f64> exposures;
  std::vector<f64> covariance;
  std::vector<f64> specific{0.0};
  explicit OneName(f64 c) : covariance{c} {}
  [[nodiscard]] eb::FactorRiskView view() const {
    return eb::FactorRiskView{0U, 0U, group, exposures, covariance, specific};
  }
};

f64 sigma_of(f64 c) { return std::sqrt(252.0 * c); }
// An update that estimated (ok and true) / that ran and estimated nothing (ok and false).
bool took(const atx::core::Result<bool>& r) { return r.has_value() && *r; }
bool skipped(const atx::core::Result<bool>& r) { return r.has_value() && !*r; }

constexpr f64 kC1 = 1e-6;     // sigma_1 = sqrt(2.52e-4), about 1.6% a year
const std::vector<f64> kBook{1.0};

TEST(BookVolTarget, RegisteredConstants) {
  EXPECT_EQ(eb::vol_target_floor, 1.0);
  EXPECT_EQ(eb::vol_target_cadence, 21U);
}

TEST(BookVolTarget, LeverageIsTheCapTimesTheRatioClipped) {
  const auto inside = eb::vol_target_leverage(0.04, 0.03, 2.0, 1.0);
  EXPECT_EQ(inside.raw, 2.0 * (0.03 / 0.04));
  EXPECT_EQ(inside.leverage, inside.raw);
  EXPECT_EQ(inside.clip, eb::RiskTargetClip::None);
  const auto equal = eb::vol_target_leverage(0.02, 0.02, 1.2, 1.0); // ratio 1: L bit for bit
  EXPECT_EQ(equal.raw, 1.2);
  EXPECT_EQ(equal.leverage, 1.2);
  EXPECT_EQ(equal.clip, eb::RiskTargetClip::None);
  const auto calm = eb::vol_target_leverage(0.01, 0.03, 2.0, 1.0);
  EXPECT_EQ(calm.leverage, 2.0);
  EXPECT_EQ(calm.clip, eb::RiskTargetClip::High);
  const auto storm = eb::vol_target_leverage(0.09, 0.03, 2.0, 1.0);
  EXPECT_EQ(storm.leverage, 1.0);
  EXPECT_EQ(storm.clip, eb::RiskTargetClip::Low);
}

// The registered sequence at cap 2: flat (no estimate, L in force), sigma_1 (ratio 1: 2), held,
// 2 sigma_1 (mean 1.5 sigma_1: 1.5), sigma_1 / 2 (mean 3.5 sigma_1 / 3: raw 4.67, the cap 2),
// 8 sigma_1 (mean 2.875 sigma_1: raw .71875, the floor 1).
TEST(BookVolTarget, RunningMeanIncludesTheCurrentEstimateAndTheClipBinds) {
  eb::VolTargetState s;
  const f64 cap = 2.0;
  // A flat book: due, nothing to estimate, the state untouched, L in force.
  const OneName m1(kC1);
  const std::vector<f64> flat{0.0};
  const auto none = eb::vol_target_update(cap, 0U, m1.view(), flat, 0.0, s);
  ASSERT_TRUE(none) << none.error().to_string();
  EXPECT_FALSE(*none);
  EXPECT_FALSE(s.estimated);
  EXPECT_EQ(eb::vol_target_in_force(s, cap), cap);

  const f64 s1 = sigma_of(kC1);
  auto first = eb::vol_target_update(cap, 1U, m1.view(), kBook, 1.0, s);
  ASSERT_TRUE(took(first));
  EXPECT_EQ(s.sigma_hat, s1);
  EXPECT_EQ(s.sigma_ref, s1);
  EXPECT_EQ(s.at.raw, cap); // the first estimate is the cap bit for bit
  EXPECT_EQ(eb::vol_target_in_force(s, cap), cap);
  EXPECT_EQ(s.estimates, 1U);

  // Not due for 20 sessions: held, whatever the book.
  const OneName m_hot(64.0 * kC1);
  const auto held = eb::vol_target_update(cap, 21U, m_hot.view(), kBook, 1.0, s);
  ASSERT_TRUE(held);
  EXPECT_FALSE(*held);
  EXPECT_EQ(s.last, 1U);

  const OneName m2(4.0 * kC1);
  const auto second = eb::vol_target_update(cap, 22U, m2.view(), kBook, 1.0, s);
  ASSERT_TRUE(took(second));
  const f64 s2 = sigma_of(4.0 * kC1);
  EXPECT_EQ(s2, 2.0 * s1); // a power of four inside the root
  const f64 ref2 = (s1 + s2) / 2.0;
  EXPECT_EQ(s.sigma_ref, ref2);
  EXPECT_EQ(s.at.raw, cap * (ref2 / s2));
  EXPECT_NEAR(s.at.leverage, 1.5, 1e-15);
  EXPECT_EQ(s.at.clip, eb::RiskTargetClip::None);

  const OneName m3(0.25 * kC1);
  ASSERT_TRUE(took(eb::vol_target_update(cap, 43U, m3.view(), kBook, 1.0, s)));
  const f64 s3 = sigma_of(0.25 * kC1);
  const f64 ref3 = (s1 + s2 + s3) / 3.0;
  EXPECT_EQ(s.sigma_ref, ref3);
  EXPECT_NEAR(s.at.raw, 2.0 * 3.5 / 3.0 / 0.5, 1e-14);
  EXPECT_EQ(s.at.leverage, cap);
  EXPECT_EQ(s.at.clip, eb::RiskTargetClip::High);

  ASSERT_TRUE(took(eb::vol_target_update(cap, 64U, m_hot.view(), kBook, 1.0, s)));
  const f64 s4 = sigma_of(64.0 * kC1);
  EXPECT_EQ(s4, 8.0 * s1);
  EXPECT_NEAR(s.at.raw, 2.0 * 2.875 / 8.0, 1e-15);
  EXPECT_EQ(s.at.leverage, eb::vol_target_floor);
  EXPECT_EQ(s.at.clip, eb::RiskTargetClip::Low);
  EXPECT_EQ(s.estimates, 4U);

  // A due decision with a flat book keeps the clock and the mean; the next estimable one counts.
  const f64 sum_before = s.sigma_sum;
  ASSERT_TRUE(skipped(eb::vol_target_update(cap, 85U, m1.view(), flat, 0.0, s)));
  EXPECT_EQ(s.last, 64U);
  EXPECT_EQ(s.sigma_sum, sum_before);
  ASSERT_TRUE(took(eb::vol_target_update(cap, 86U, m1.view(), kBook, 1.0, s)));
  EXPECT_EQ(s.last, 86U);
  EXPECT_EQ(s.estimates, 5U);
}

// Wrong rules told apart on the second estimate of the sequence (sigma_1 then 2 sigma_1, cap 2):
// the registered 1.5; a mean without the current estimate 1 (the floor); a variance ratio
// 2 x (1.5 / 2)^2 = 1.125; an absolute target is not the rule (no constant enters).
TEST(BookVolTarget, FixtureTellsWrongRulesApart) {
  eb::VolTargetState s;
  const OneName m1(kC1), m2(4.0 * kC1);
  ASSERT_TRUE(took(eb::vol_target_update(2.0, 0U, m1.view(), kBook, 1.0, s)));
  ASSERT_TRUE(took(eb::vol_target_update(2.0, 21U, m2.view(), kBook, 1.0, s)));
  const f64 registered = s.at.leverage;
  const f64 s1 = sigma_of(kC1), s2 = sigma_of(4.0 * kC1);
  const f64 past_only = 2.0 * (s1 / s2);                          // 1.0
  const f64 variance = 2.0 * ((s1 + s2) / 2.0 / s2) * ((s1 + s2) / 2.0 / s2); // 1.125
  EXPECT_GT(std::abs(registered - past_only), 0.4);
  EXPECT_GT(std::abs(registered - variance), 0.3);
  // The cap is the run's L: the same forecasts at L 1.2 give 1.2 x .75 = .9 -> the floor 1.
  eb::VolTargetState t;
  ASSERT_TRUE(took(eb::vol_target_update(1.2, 0U, m1.view(), kBook, 1.0, t)));
  ASSERT_TRUE(took(eb::vol_target_update(1.2, 21U, m2.view(), kBook, 1.0, t)));
  EXPECT_EQ(t.at.leverage, 1.0);
  EXPECT_EQ(t.at.clip, eb::RiskTargetClip::Low);
}

TEST(BookVolTarget, RefusalsLeaveTheStateUntouched) {
  const OneName m1(kC1);
  for (const f64 cap : {0.5, 0.999, std::numeric_limits<f64>::quiet_NaN(),
                        std::numeric_limits<f64>::infinity(), 1e4}) {
    eb::VolTargetState s;
    const auto r = eb::vol_target_update(cap, 0U, m1.view(), kBook, 1.0, s);
    ASSERT_FALSE(r) << cap;
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
    EXPECT_FALSE(s.estimated);
    EXPECT_EQ(s.estimates, 0U);
  }
  // A model of the wrong shape (two weights for one name) or a gross below the book's.
  eb::VolTargetState s;
  const std::vector<f64> two{0.5, 0.5};
  EXPECT_FALSE(eb::vol_target_update(2.0, 0U, m1.view(), two, 1.0, s));
  EXPECT_FALSE(eb::vol_target_update(2.0, 0U, m1.view(), kBook, 0.5, s));
  EXPECT_FALSE(s.estimated);
  EXPECT_EQ(s.sigma_sum, 0.0);
}

} // namespace atx_test_v8_book_vol_target
