#include "event_owner.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/black76.hpp"

namespace {
using namespace atx::vol;
namespace cg = cash_gaussian;

double intrinsic(double x, double k, Side side) {
  return std::max(side == Side::Call ? x-k : k-x, 0.);
}

TEST(CashGaussianEventOwner, OwnsNormalizesEventsAndPreservesNestedClock) {
  cg::Model model;
  std::vector<cg::Event> raw{{.5, 2.}, {-.1, 3.}, {.5, 1.}, {0., 5.},
      {1.+.5e-12, 4.}, {1.+2e-12, 8.}, {.25, 0.}};
  cg::Options options;
  options.positive_intervals = 16;
  options.time_base = 3;
  const auto base = cg::EventOwner::prepare(model, raw, options);
  ASSERT_TRUE(base);
  ASSERT_EQ(base->events().size(), 2u);
  EXPECT_DOUBLE_EQ(base->events()[0].time, .5);
  EXPECT_DOUBLE_EQ(base->events()[0].cash, 3.);
  EXPECT_DOUBLE_EQ(base->events()[1].time, 1.);
  EXPECT_DOUBLE_EQ(base->events()[1].cash, 4.);
  ASSERT_EQ(base->steps().size(), 4u);
  raw[0].cash = 999.;
  EXPECT_DOUBLE_EQ(base->raw_events()[0].cash, 2.);
  options.time_level = 2;
  const auto refined = cg::EventOwner::prepare(model, base->raw_events(), options);
  ASSERT_TRUE(refined);
  ASSERT_EQ(refined->steps().size(), 16u);
  for (std::size_t i = 0; i < base->steps().size(); ++i) {
    EXPECT_DOUBLE_EQ(refined->steps()[4*i].to, base->steps()[i].to);
    EXPECT_DOUBLE_EQ(refined->steps()[4*i+3].from, base->steps()[i].from);
  }
}

TEST(CashGaussianEventOwner, ExerciseMaximumKeepsAllCrossingsAndInfiniteTail) {
  cg::Work work;
  const std::array<double, 5> x{0., 20., 40., 60., 80.};
  const std::array<double, 5> y{110., 60., 80., 20., 40.};
  const auto f = cg::StockPwl::create(x, y, -.25, work.integral);
  ASSERT_TRUE(f);
  const auto put = cg::exercise_max(*f, 100., Side::Put, work);
  ASSERT_TRUE(put);
  EXPECT_GE(work.intersections, 4u);
  for (int j = 0; j <= 400; ++j) {
    const auto a = put->value(static_cast<double>(j));
    const auto b = f->value(static_cast<double>(j));
    ASSERT_TRUE(a && b);
    EXPECT_NEAR(*a, std::max(*b, intrinsic(j, 100., Side::Put)), 5e-13);
  }
  EXPECT_DOUBLE_EQ(put->right_slope(), 0.);
  const auto call = cg::exercise_max(*f, 100., Side::Call, work);
  ASSERT_TRUE(call);
  for (double s : {0., 15., 50., 100., 120., 200., 1e5}) {
    EXPECT_NEAR(*call->value(s), std::max(*f->value(s), intrinsic(s, 100., Side::Call)),
                5e-12);
  }
  EXPECT_DOUBLE_EQ(call->right_slope(), 1.);
}

TEST(CashGaussianEventOwner, DeterministicAffineRollbackAndZeroState) {
  cg::Work work;
  const std::array<double, 3> x{0., 50., 100.}, y{100., 40., 2.};
  const auto f = cg::StockPwl::create(x, y, .2, work.integral);
  ASSERT_TRUE(f);
  const auto rolled = cg::deterministic_rollback(*f, .7, -.03, .02, work);
  ASSERT_TRUE(rolled);
  for (double s : {0., 20., 50., 80., 100., 200.})
    EXPECT_NEAR(*rolled->value(s), std::exp(.03*.7)* *f->value(s*std::exp(-.05*.7)),
                5e-13);
  EXPECT_EQ(work.deterministic_rollbacks, 1u);
  for (Side side : {Side::Call, Side::Put}) for (double r : {-.04, .0, .04}) {
    cg::Model m;
    m.spot = 0.; m.rate = r; m.side = side;
    if (side == Side::Call && r < 0.) m.exercise = ExerciseStyle::European;
    const std::array<cg::Event, 2> events{{{.2, 500.}, {1., 100.}}};
    const auto owner = cg::EventOwner::prepare(m, events);
    ASSERT_TRUE(owner);
    const auto price = owner->price(work);
    ASSERT_TRUE(price);
    EXPECT_NEAR(*price, side == Side::Call ? 0. : 100.*std::max(1., std::exp(-r)), 2e-11);
    EXPECT_EQ(work.integral.interval_evaluations, 0u);
  }
}

TEST(CashGaussianEventOwner, EuropeanNoCashAndEventOnlyCallUseIndependentBlack76) {
  cg::Work work;
  for (Side side : {Side::Call, Side::Put}) for (double r : {-.03, .0, .07}) {
    cg::Model m;
    m.spot = 105.; m.side = side; m.rate = r; m.yield = .012;
    m.exercise = ExerciseStyle::European;
    const auto owner = cg::EventOwner::prepare(m, {});
    ASSERT_TRUE(owner);
    const auto price = owner->price(work);
    ASSERT_TRUE(price);
    const double expected = black76_price(m.spot*std::exp(r-m.yield), m.strike, 1.,
                                          m.sigma, std::exp(-r), side);
    EXPECT_NEAR(*price, expected, 4e-12);
    EXPECT_EQ(work.completed_steps, 1u);
  }
  cg::Model call;
  call.side = Side::Call;
  const auto owner = cg::EventOwner::prepare(call, {});
  ASSERT_TRUE(owner);
  const auto price = owner->price(work);
  ASSERT_TRUE(price);
  EXPECT_NEAR(*price, black76_price(100.*std::exp(.04), 100., 1., .2,
                                  std::exp(-.04), Side::Call), 4e-12);
}

TEST(CashGaussianEventOwner, TerminalCashAndPrePostExerciseAreDistinct) {
  cg::Work work;
  cg::Model m;
  m.spot = 100.; m.strike = 70.; m.sigma = 0.; m.rate = 0.; m.side = Side::Call;
  const std::array<cg::Event, 1> cash{{{.5, 50.}}};
  const auto american = cg::EventOwner::prepare(m, cash);
  ASSERT_TRUE(american);
  EXPECT_DOUBLE_EQ(*american->price(work), 30.);
  m.exercise = ExerciseStyle::European;
  const auto european = cg::EventOwner::prepare(m, cash);
  ASSERT_TRUE(european);
  EXPECT_DOUBLE_EQ(*european->price(work), 0.);
  m.side = Side::Put; m.strike = 100.; m.sigma = .25; m.rate = .03;
  const std::array<cg::Event, 1> terminal{{{1., 25.}}};
  const auto put = cg::EventOwner::prepare(m, terminal);
  ASSERT_TRUE(put);
  const double F = m.spot*std::exp(m.rate), df = std::exp(-m.rate);
  const double expected = m.strike*df-black76_price(F, 25., 1., .25, df, Side::Call)
      +black76_price(F, 125., 1., .25, df, Side::Call);
  EXPECT_NEAR(*put->price(work), expected, 5e-12);
}

TEST(CashGaussianEventOwner, DeterministicAmericanMatchesExplicitExerciseClock) {
  for (Side side : {Side::Call, Side::Put}) {
    cg::Model m;
    m.sigma = 0.; m.rate = side == Side::Call ? .03 : -.03; m.side = side;
    const std::array<cg::Event, 2> events{{{.25, 5.}, {.75, 7.}}};
    cg::Options options;
    options.time_base = 16;
    const auto owner = cg::EventOwner::prepare(m, events, options);
    ASSERT_TRUE(owner);
    double best = intrinsic(m.spot, m.strike, side), stock = m.spot, prior = 0.;
    for (auto it = owner->steps().rbegin(); it != owner->steps().rend(); ++it) {
      const double t = it->to;
      stock *= std::exp((m.rate-m.yield)*(t-prior));
      best = std::max(best, std::exp(-m.rate*t)*intrinsic(stock, m.strike, side));
      for (const auto &event : owner->events()) if (event.time == t) {
        stock = std::max(stock-event.cash, 0.);
        best = std::max(best, std::exp(-m.rate*t)*intrinsic(stock, m.strike, side));
      }
      prior = t;
    }
    cg::Work work;
    const auto result = owner->price(work);
    ASSERT_TRUE(result);
    EXPECT_NEAR(*result, best, 3e-12);
  }
}

TEST(CashGaussianEventOwner, TypedInputDomainAndResourceRefusalsRetainWork) {
  cg::Model m;
  cg::Options o;
  o.max_events = 1;
  const std::array<cg::Event, 2> events{{{.25, 2.}, {.75, 3.}}};
  EXPECT_FALSE(cg::EventOwner::prepare(m, events, o));
  o = {}; o.max_steps = 1;
  EXPECT_FALSE(cg::EventOwner::prepare(m, {}, o));
  m.side = Side::Call; m.rate = -.01;
  EXPECT_FALSE(cg::EventOwner::prepare(m, {}));
  m.rate = .04; m.yield = .01;
  EXPECT_FALSE(cg::EventOwner::prepare(m, {}));
  m.yield = 0.; m.cash_model = cg::CashModel::Escrowed;
  EXPECT_FALSE(cg::EventOwner::prepare(m, {}));
  m.cash_model = cg::CashModel::Liquidator;
  const std::array<cg::Event, 1> invalid{{{.5, -1.}}};
  EXPECT_FALSE(cg::EventOwner::prepare(m, invalid));
  m.spot = std::numeric_limits<double>::quiet_NaN();
  EXPECT_FALSE(cg::EventOwner::prepare(m, {}));
  m.spot = 100.; o = {}; o.limits.max_expectations = 2;
  const auto owner = cg::EventOwner::prepare(m, {}, o);
  ASSERT_TRUE(owner);
  cg::Work work;
  const auto refused = owner->price(work);
  EXPECT_FALSE(refused);
  EXPECT_EQ(work.integral.expectations, 2u);
  EXPECT_EQ(work.completed_steps, 0u);
  EXPECT_GT(work.integral.interval_evaluations, 0u);
  work.completed_steps = 999;
  EXPECT_FALSE(owner->price(work));
  EXPECT_EQ(work.completed_steps, 0u);
  cg::EventOwner empty;
  EXPECT_FALSE(empty.price(work));
}
TEST(CashGaussianEventOwner, RoundedCrossingsAndOverflowAreExplicitRefusals) {
  cg::Work work;
  const std::array<double, 4> x{0., 1., std::nextafter(1., 2.), 2.};
  const std::array<double, 4> y{2., std::nextafter(1., 2.), .5, 0.};
  const auto close = cg::StockPwl::create(x, y, 0., work.integral);
  ASSERT_TRUE(close);
  EXPECT_FALSE(cg::exercise_max(*close, 2., Side::Put, work));
  EXPECT_EQ(work.rounded_intersections, 1u);
  const std::array<double, 2> tx{0., 1e308}, ty{1e-300, 1e-300};
  const auto tail = cg::StockPwl::create(tx, ty, 0., work.integral);
  ASSERT_TRUE(tail);
  EXPECT_FALSE(cg::exercise_max(*tail, 1e308, Side::Call, work));
  EXPECT_EQ(work.rounded_intersections, 2u);
  EXPECT_FALSE(cg::deterministic_rollback(*tail, 1., -1000., 0., work));
  const auto unchanged = cg::deterministic_rollback(*tail, 0., 1e308, -1e308, work);
  ASSERT_TRUE(unchanged);
  EXPECT_DOUBLE_EQ(*unchanged->value(1.), 1e-300);
  cg::Model model;
  model.sigma = 1000.;
  EXPECT_FALSE(cg::EventOwner::prepare(model, {}));
  model.sigma = .2;
  const std::array<cg::Event, 2> overflow{{{.5, 1e308}, {.5, 1e308}}};
  EXPECT_FALSE(cg::EventOwner::prepare(model, overflow));
  model.maturity = 0.; model.sigma = 0.;
  const auto expiry = cg::EventOwner::prepare(model, {});
  ASSERT_TRUE(expiry);
  EXPECT_DOUBLE_EQ(*expiry->price(work), 0.);
}

TEST(CashGaussianEventOwner, InteriorCashOwnerConvergesToIndependentQuadratureControl) {
  // Independent split-normal Black76 quadrature frozen by the primitive control.
  // Different logarithmic representation here also tests the complete owner order.
  for (Side side : {Side::Call, Side::Put}) {
    cg::Model m;
    m.sigma = .25; m.rate = .04; m.yield = .01; m.side = side;
    m.exercise = ExerciseStyle::European;
    const std::array<cg::Event, 1> events{{{.4, 15.}}};
    const double reference = side == Side::Call ? 4.8427416508554177 : 16.590305716939;
    double prior = std::numeric_limits<double>::infinity();
    for (std::size_t n : {128u, 256u, 512u}) {
      cg::Options o;
      o.positive_intervals = n;
      const auto owner = cg::EventOwner::prepare(m, events, o);
      ASSERT_TRUE(owner);
      cg::Work work;
      const auto result = owner->price(work);
      ASSERT_TRUE(result);
      const double error = std::abs(*result-reference);
      EXPECT_LT(error, prior*.7);
      prior = error;
      EXPECT_EQ(work.completed_steps, 2u);
      EXPECT_EQ(work.jumps, 1u);
      EXPECT_EQ(work.exercise_projections, 0u);
    }
    EXPECT_LT(prior, .002);
  }
}

TEST(CashGaussianEventOwner, MovedOwnerAndConstructionBudgetNeverReturnPartialPrice) {
  cg::Model model;
  model.side = Side::Call;
  cg::Options options;
  options.limits.max_created_knots = 2;
  auto prepared = cg::EventOwner::prepare(model, {}, options);
  ASSERT_TRUE(prepared);
  cg::EventOwner moved = std::move(*prepared);
  cg::Work work;
  EXPECT_FALSE(prepared->price(work));
  EXPECT_EQ(work.integral.expectations, 0u);
  EXPECT_FALSE(moved.price(work));
  EXPECT_EQ(work.integral.created_knots, 2u);
  EXPECT_GT(work.integral.expectations, 0u);
  EXPECT_EQ(work.completed_steps, 0u);
  options.limits.max_knots = 2;
  EXPECT_FALSE(cg::EventOwner::prepare(model, {}, options));
}
} // namespace
