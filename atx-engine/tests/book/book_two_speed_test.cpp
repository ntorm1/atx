// book_two_speed_test.cpp — platform v8 Y (lane YCOMB, rule Y-5, Ruling PM8-5): two-speed netted
// sleeves (atx::engine::book, two_speed.hpp).
//
//   The registered constants (theta_f = 1 - 2^(-C/5) at cadence C); one step on two names in
//   closed form; equal rates are one partial adjustment toward the summed aim (the single-sleeve
//   book, within rounding); opposite sleeve trades cancel in the netted trade; the fast sleeve
//   halves an aim gap in five sessions; refusals before any write.
//
// Suite: BookTwoSpeed

#include <bit>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/book/two_speed.hpp"

namespace atx_test_v8_book_two_speed {

using atx::f64;
using atx::usize;
namespace eb = atx::engine::book;

TEST(BookTwoSpeed, RegisteredConstants) {
  EXPECT_EQ(eb::two_speed_slow_theta, 0.05);
  EXPECT_EQ(eb::two_speed_fast_half_life, 5.0);
  EXPECT_EQ(eb::two_speed_fast_bound, 10.0);
  EXPECT_NEAR(eb::two_speed_fast_theta(1.0), 0.12944943670387588, 1e-15);
  EXPECT_NEAR(std::pow(1.0 - eb::two_speed_fast_theta(1.0), 5.0), 0.5, 1e-15);
}

// Ruling PM8-16 #4: theta_f = 1 - 2^(-C/5) per rebalance at cadence C, so the fast half-life is
// five sessions at any cadence (C = 5: one step halves the gap; C = 10: one step leaves a quarter).
TEST(BookTwoSpeed, FastThetaFollowsTheCadence) {
  EXPECT_EQ(eb::two_speed_fast_theta(5.0), 0.5);
  EXPECT_EQ(eb::two_speed_fast_theta(10.0), 0.75);
  for (const f64 cadence : {1.0, 2.0, 3.0, 5.0, 7.0, 21.0}) {
    const f64 theta = eb::two_speed_fast_theta(cadence);
    EXPECT_GT(theta, 0.0) << cadence;
    EXPECT_LE(theta, 1.0) << cadence;
    // 5 / C rebalances (5 sessions) leave half of a constant aim gap.
    EXPECT_NEAR(std::pow(1.0 - theta, eb::two_speed_fast_half_life / cadence), 0.5, 1e-14)
        << cadence;
  }
  EXPECT_EQ(eb::two_speed_fast_theta(4096.0), 1.0); // the replay's largest cadence: one step
}

TEST(BookTwoSpeed, OneStepClosedForm) {
  const std::vector<f64> aim_fast{0.4, -0.2}, aim_slow{-0.1, 0.3};
  std::vector<f64> fast{0.0, 0.1}, slow{0.2, 0.0}, book{0.2, 0.1};
  const auto r = eb::two_speed_step(aim_fast, aim_slow, 0.5, 0.1, fast, slow, book);
  ASSERT_TRUE(r) << r.error().to_string();
  EXPECT_NEAR(fast[0], 0.2, 1e-15);   // 0 + .5 (.4 - 0)
  EXPECT_NEAR(fast[1], -0.05, 1e-15); // .1 + .5 (-.2 - .1)
  EXPECT_NEAR(slow[0], 0.17, 1e-15);  // .2 + .1 (-.1 - .2)
  EXPECT_NEAR(slow[1], 0.03, 1e-15);  // 0 + .1 (.3 - 0)
  EXPECT_NEAR(book[0], 0.37, 1e-15);
  EXPECT_NEAR(book[1], -0.02, 1e-15);
  EXPECT_NEAR(r->sleeve_trade, 0.2 + 0.03 + 0.15 + 0.03, 1e-15);
  EXPECT_NEAR(r->net_trade, 0.17 + 0.12, 1e-15);
}

// Equal rates: the sum of the sleeves is one partial adjustment toward the summed aim, and the
// netted trade is that book's trade; the rule's content is the rate difference.
TEST(BookTwoSpeed, EqualRatesAreOneBookTowardTheSummedAim) {
  const std::vector<f64> aim_fast{0.3, -0.1, 0.05}, aim_slow{-0.2, 0.25, 0.1};
  std::vector<f64> fast{0.01, 0.02, -0.03}, slow{0.1, -0.05, 0.0}, book(3U);
  std::vector<f64> single(3U);
  for (usize i = 0; i < 3U; ++i) { book[i] = fast[i] + slow[i]; single[i] = book[i]; }
  for (int step = 0; step < 40; ++step) {
    const auto r = eb::two_speed_step(aim_fast, aim_slow, 0.05, 0.05, fast, slow, book);
    ASSERT_TRUE(r);
    f64 single_trade = 0.0;
    for (usize i = 0; i < 3U; ++i) {
      const f64 d = 0.05 * (aim_fast[i] + aim_slow[i] - single[i]);
      single[i] += d;
      single_trade += std::abs(d);
      EXPECT_NEAR(book[i], single[i], 1e-14) << step << ' ' << i;
    }
    EXPECT_NEAR(r->net_trade, single_trade, 1e-14) << step;
  }
}

// A fast sleeve leaving a name while the slow sleeve enters it: the trades cancel in the book.
TEST(BookTwoSpeed, OppositeSleeveTradesNet) {
  const f64 theta_f = eb::two_speed_fast_theta(1.0);
  const std::vector<f64> aim_fast{0.0}, aim_slow{0.2};
  std::vector<f64> fast{0.1}, slow{0.0}, book{0.1};
  const auto r =
      eb::two_speed_step(aim_fast, aim_slow, theta_f, eb::two_speed_slow_theta, fast, slow, book);
  ASSERT_TRUE(r);
  const f64 d_fast = theta_f * 0.1, d_slow = 0.05 * 0.2;
  EXPECT_NEAR(r->sleeve_trade, d_fast + d_slow, 1e-15);
  EXPECT_NEAR(r->net_trade, std::abs(d_slow - d_fast), 1e-15);
  EXPECT_LT(r->net_trade, r->sleeve_trade);
}

// Half-life matched: five fast steps close half of a constant aim gap; the slow sleeve keeps about
// 77%.
TEST(BookTwoSpeed, FastSleeveHalvesItsGapInFiveSteps) {
  const std::vector<f64> aim{1.0};
  std::vector<f64> fast{0.0}, slow{0.0}, book{0.0};
  const f64 theta_f = eb::two_speed_fast_theta(1.0);
  for (int step = 0; step < 5; ++step)
    ASSERT_TRUE(
        eb::two_speed_step(aim, aim, theta_f, eb::two_speed_slow_theta, fast, slow, book));
  EXPECT_NEAR(fast[0], 0.5, 1e-14);
  EXPECT_NEAR(slow[0], 1.0 - std::pow(0.95, 5.0), 1e-14);
}

TEST(BookTwoSpeed, RefusalsWriteNothing) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<f64> aim{0.1, 0.2}, bad_aim{0.1, nan}, short_aim{0.1};
  std::vector<f64> fast{0.5, 0.5}, slow{0.5, 0.5}, book{1.0, 1.0};
  const auto untouched = [&] {
    for (usize i = 0; i < 2U; ++i) {
      EXPECT_EQ(fast[i], 0.5);
      EXPECT_EQ(slow[i], 0.5);
      EXPECT_EQ(book[i], 1.0);
    }
  };
  for (const f64 theta : {0.0, -0.1, 1.5, nan}) {
    const auto r = eb::two_speed_step(aim, aim, theta, 0.05, fast, slow, book);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code(), atx::core::ErrorCode::InvalidArgument);
    untouched();
  }
  EXPECT_FALSE(eb::two_speed_step(bad_aim, aim, 0.1, 0.05, fast, slow, book));
  untouched();
  EXPECT_FALSE(eb::two_speed_step(short_aim, aim, 0.1, 0.05, fast, slow, book));
  untouched();
}

// two_speed_aim on three names (the third not a member), m_f .4, L 2, theta_f .5, theta_s .25, in
// closed form: name 0 F .1 -> .25, aim .6 x .2 + (.1 + .15 / .25) / 2 = .47; name 1 F -.2 -> -.3,
// aim .06 + (-.2 - .4) / 2 = -.24; name 2: F and aim 0.
TEST(BookTwoSpeed, NettedAimClosedForm) {
  const std::vector<atx::u8> member{1, 1, 0};
  const std::vector<f64> fast_desired{0.5, -0.5, 0.0};
  std::vector<f64> fast{0.1, -0.2, 0.3}, desired{0.2, 0.1, 0.4};
  ASSERT_TRUE(eb::two_speed_aim(member, fast_desired, 0.4, 2.0, 0.5, 0.25, fast, desired));
  EXPECT_NEAR(fast[0], 0.25, 1e-15);
  EXPECT_NEAR(fast[1], -0.3, 1e-15);
  EXPECT_EQ(fast[2], 0.0);
  EXPECT_NEAR(desired[0], 0.47, 1e-15);
  EXPECT_NEAR(desired[1], -0.24, 1e-15);
  EXPECT_EQ(desired[2], 0.0);
}

// The aim's aim-partial step from any current weight c is the two sleeves' moves netted: F_next - F
// plus the remainder (c - F) moving toward L m_s d_s at theta_s.
TEST(BookTwoSpeed, AimPartialStepOnTheAimIsTheNettedSleeveMove) {
  const std::vector<atx::u8> member{1, 1, 1, 1};
  const std::vector<f64> fast_desired{0.3, -0.1, 0.0, -0.4}, slow_desired{-0.2, 0.25, 0.1, 0.05};
  const f64 share = 0.35, L = 1.4, theta_f = eb::two_speed_fast_theta(1.0);
  const f64 theta_s = eb::two_speed_slow_theta;
  std::vector<f64> fast{0.02, -0.05, 0.0, 0.1};
  const std::vector<f64> before = fast;
  std::vector<f64> desired = slow_desired;
  ASSERT_TRUE(eb::two_speed_aim(member, fast_desired, share, L, theta_f, theta_s, fast, desired));
  for (const f64 c : {-0.3, 0.0, 0.07, 0.5})
    for (usize i = 0; i < member.size(); ++i) {
      const f64 book_step = c + theta_s * (L * desired[i] - c);
      const f64 remainder = c - before[i];
      const f64 sleeves =
          fast[i] + remainder + theta_s * (L * (1.0 - share) * slow_desired[i] - remainder);
      EXPECT_NEAR(book_step, sleeves, 1e-15) << c << ' ' << i;
      EXPECT_NEAR(fast[i], before[i] + theta_f * (L * share * fast_desired[i] - before[i]), 1e-15)
          << i;
    }
}

TEST(BookTwoSpeed, NettedAimRefusalsWriteNothing) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<atx::u8> member{1, 0}, bad_member{2, 0};
  const std::vector<f64> fast_desired{0.1, 0.2};
  std::vector<f64> fast{0.5, 0.5}, desired{0.3, 0.3};
  const auto untouched = [&] {
    for (usize i = 0; i < 2U; ++i) {
      EXPECT_EQ(fast[i], 0.5);
      EXPECT_EQ(desired[i], 0.3);
    }
  };
  // share, leverage, theta
  EXPECT_FALSE(eb::two_speed_aim(member, fast_desired, 1.5, 1.0, 0.1, 0.05, fast, desired));
  EXPECT_FALSE(eb::two_speed_aim(member, fast_desired, 0.5, 0.0, 0.1, 0.05, fast, desired));
  EXPECT_FALSE(eb::two_speed_aim(member, fast_desired, 0.5, 1.0, 0.0, 0.05, fast, desired));
  EXPECT_FALSE(eb::two_speed_aim(bad_member, fast_desired, 0.5, 1.0, 0.1, 0.05, fast, desired));
  EXPECT_FALSE(eb::two_speed_aim(member, std::vector<f64>{nan, 0.2}, 0.5, 1.0, 0.1, 0.05, fast,
                                 desired));
  EXPECT_FALSE(
      eb::two_speed_aim(member, std::vector<f64>{0.1}, 0.5, 1.0, 0.1, 0.05, fast, desired));
  untouched();
  // A nonmember's NaN is no refusal (its F and aim become 0).
  std::vector<f64> loose{nan, 0.2};
  EXPECT_TRUE(
      eb::two_speed_aim(std::vector<atx::u8>{0, 1}, loose, 0.5, 1.0, 0.1, 0.05, fast, desired));
  EXPECT_EQ(fast[0], 0.0);
  EXPECT_EQ(desired[0], 0.0);
}

// Ruling PM8-16 #10: under a scaler the plan at lambda L steps any current weight c to the
// remainder R = c - lambda_prev F moving at theta_s toward lambda L m_s d_s plus the scaled fast
// sleeve lambda F_next; equal scales write nothing (the netted aim bit for bit).
TEST(BookTwoSpeed, CarryKeepsTheFastHoldingAtTheBooksScale) {
  const std::vector<atx::u8> member{1, 1, 1, 0};
  const std::vector<f64> fast_desired{0.3, -0.1, 0.0, 0.2}, slow_desired{-0.2, 0.25, 0.1, 0.0};
  const f64 share = 0.35, L = 1.4, theta_f = eb::two_speed_fast_theta(1.0);
  const f64 theta_s = eb::two_speed_slow_theta;
  const std::vector<f64> before{0.02, -0.05, 0.03, 0.1};
  std::vector<f64> fast = before, desired = slow_desired;
  ASSERT_TRUE(eb::two_speed_aim(member, fast_desired, share, L, theta_f, theta_s, fast, desired));
  const std::vector<f64> netted = desired;
  for (const auto& [lambda_prev, lambda] : {std::pair{1.0, 0.8}, std::pair{0.8, 1.1},
                                            std::pair{1.25, 0.96}}) {
    std::vector<f64> carried = netted;
    ASSERT_TRUE(eb::two_speed_carry(member, before, lambda_prev, lambda, L, theta_s, carried));
    for (const f64 c : {-0.3, 0.0, 0.07, 0.5})
      for (usize i = 0; i < 3U; ++i) { // the members
        const f64 book_step = c + theta_s * (lambda * L * carried[i] - c);
        const f64 remainder = c - lambda_prev * before[i];
        const f64 sleeves = remainder +
            theta_s * (lambda * L * (1.0 - share) * slow_desired[i] - remainder) + lambda * fast[i];
        EXPECT_NEAR(book_step, sleeves, 1e-15) << lambda_prev << ' ' << lambda << ' ' << c << ' '
                                               << i;
      }
    EXPECT_EQ(carried[3], 0.0); // a nonmember is untouched
  }
  std::vector<f64> same = netted;
  ASSERT_TRUE(eb::two_speed_carry(member, before, 0.9, 0.9, L, theta_s, same));
  for (usize i = 0; i < member.size(); ++i)
    EXPECT_EQ(std::bit_cast<atx::u64>(same[i]), std::bit_cast<atx::u64>(netted[i])) << i;
}

TEST(BookTwoSpeed, CarryRefusalsWriteNothing) {
  const f64 nan = std::numeric_limits<f64>::quiet_NaN();
  const std::vector<atx::u8> member{1, 0}, bad_member{2, 0};
  const std::vector<f64> before{0.1, nan}; // a nonmember's NaN is no refusal
  std::vector<f64> desired{0.3, 0.0};
  const auto untouched = [&] {
    EXPECT_EQ(desired[0], 0.3);
    EXPECT_EQ(desired[1], 0.0);
  };
  EXPECT_FALSE(eb::two_speed_carry(member, before, 0.0, 1.0, 1.2, 0.05, desired)); // lambda_prev
  EXPECT_FALSE(eb::two_speed_carry(member, before, 1.0, nan, 1.2, 0.05, desired));  // lambda
  EXPECT_FALSE(eb::two_speed_carry(member, before, 1.0, 0.8, -1.0, 0.05, desired)); // leverage
  EXPECT_FALSE(eb::two_speed_carry(member, before, 1.0, 0.8, 1.2, 1.5, desired));   // theta
  EXPECT_FALSE(eb::two_speed_carry(bad_member, before, 1.0, 0.8, 1.2, 0.05, desired));
  EXPECT_FALSE(eb::two_speed_carry(member, std::vector<f64>{nan, 0.0}, 1.0, 0.8, 1.2, 0.05,
                                   desired));
  EXPECT_FALSE(eb::two_speed_carry(member, std::vector<f64>{0.1}, 1.0, 0.8, 1.2, 0.05, desired));
  untouched();
  EXPECT_TRUE(eb::two_speed_carry(member, before, 1.0, 0.8, 1.2, 0.05, desired));
  EXPECT_NE(desired[0], 0.3);
  EXPECT_EQ(desired[1], 0.0);
}

} // namespace atx_test_v8_book_two_speed
