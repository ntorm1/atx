#include "event_owner.hpp"
#include "semigroup.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <tuple>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "atx/vol/api/pricing/black76.hpp"

namespace {
using namespace atx::vol;
namespace cg = cash_gaussian;

auto work_tuple(const cg::Work &w) {
  return std::make_tuple(w.integral.created_knots, w.integral.interval_evaluations,
      w.integral.expectations, w.integral.deterministic_evaluations,
      w.integral.zero_state_evaluations, w.planned_steps, w.completed_steps,
      w.grid_nodes, w.jumps, w.exercise_projections, w.intersections,
      w.rounded_intersections, w.deterministic_rollbacks);
}

struct Capture {
  std::vector<double> stock, value;
  cg::RoundedCrossing fields;
  std::size_t count = 0;
  bool failed = false;
  static void observe(const cg::RoundedCrossing &view, void *context) noexcept {
    auto &self = *static_cast<Capture *>(context);
    try {
      ++self.count;
      self.stock.assign(view.stock.begin(), view.stock.end());
      self.value.assign(view.value.begin(), view.value.end());
      self.fields = view;
      self.fields.stock = self.stock; self.fields.value = self.value;
    } catch (...) { self.failed = true; }
  }
};

TEST(CashGaussianFollowup, ObserverPreservesFiniteAndTailRefusalAndEveryWorkCount) {
  for (bool tail : {false, true}) {
    cg::Work baseline;
    const std::vector<double> x = tail ? std::vector<double>{0., 1e308} :
        std::vector<double>{0., 1., std::nextafter(1., 2.), 2.};
    const std::vector<double> y = tail ? std::vector<double>{1e-300, 1e-300} :
        std::vector<double>{2., std::nextafter(1., 2.), .5, 0.};
    const auto f = cg::StockPwl::create(x, y, 0., baseline.integral);
    ASSERT_TRUE(f);
    cg::Work observed = baseline;
    Capture capture;
    const cg::CrossingObserver observer{Capture::observe, &capture};
    const double strike = tail ? 1e308 : 2.;
    const Side side = tail ? Side::Call : Side::Put;
    const auto a = cg::exercise_max(*f, strike, side, baseline);
    const auto b = cg::exercise_max(*f, strike, side, observed, &observer);
    ASSERT_FALSE(a); ASSERT_FALSE(b);
    EXPECT_EQ(a.error().to_string(), b.error().to_string());
    EXPECT_EQ(work_tuple(baseline), work_tuple(observed));
    ASSERT_EQ(capture.count, 1u);
    EXPECT_FALSE(capture.failed);
    EXPECT_EQ(capture.stock, x); EXPECT_EQ(capture.value, y);
    EXPECT_EQ(capture.fields.tail, tail);
    EXPECT_DOUBLE_EQ(capture.fields.computed_stock, tail ? 1e308 : 1.);
    EXPECT_DOUBLE_EQ(capture.fields.strike, strike);
    if (tail) EXPECT_TRUE(std::isinf(capture.fields.stock_hi));
    else EXPECT_DOUBLE_EQ(capture.fields.stock_hi, std::nextafter(1., 2.));
  }
}

TEST(CashGaussianFollowup, OwnerForwardsObserverWithoutChangingItsFailure) {
  cg::Model m;
  m.spot = 6.375; m.strike = 5.; m.maturity = .34715297818183899;
  m.sigma = .2; m.rate = .046780478209257126; m.side = Side::Call;
  const std::array<cg::Event, 1> events{{{.016272795852273703, 4.0999999046325684}}};
  const auto owner = cg::EventOwner::prepare(m, events);
  ASSERT_TRUE(owner);
  cg::Work baseline, observed;
  Capture capture;
  const cg::CrossingObserver observer{Capture::observe, &capture};
  const auto a = owner->price(baseline), b = owner->price(observed, &observer);
  ASSERT_FALSE(a); ASSERT_FALSE(b);
  EXPECT_EQ(a.error().to_string(), b.error().to_string());
  EXPECT_EQ(work_tuple(baseline), work_tuple(observed));
  EXPECT_EQ(capture.count, 1u);
  EXPECT_FALSE(capture.failed);
  EXPECT_EQ(observed.jumps, 1u);
}

TEST(CashGaussianFollowup, RepeatedConstantAndLinearMomentsAndExactWork) {
  cg::Model model;
  model.spot = 772.98; model.strike = 700.; model.maturity = 2.182;
  model.sigma = .35; model.rate = .047; model.exercise = ExerciseStyle::European;
  cg::Options options;
  options.positive_intervals = 64;
  const auto owner = cg::EventOwner::prepare(model, {}, options);
  ASSERT_TRUE(owner);
  const auto grid = owner->stock_grid();
  for (bool linear : {false, true}) for (std::size_t steps : {1u, 32u, 128u}) {
    cg::Budget budget;
    const std::array<double, 2> x{0., 100.};
    const std::array<double, 2> y = linear ? x : std::array<double, 2>{1., 1.};
    auto initial = cg::StockPwl::create(x, y, linear ? 1. : 0., budget);
    ASSERT_TRUE(initial);
    cg::SemigroupWork work;
    const auto price = cg::repeat_expectation(std::move(*initial), grid, model.spot,
        model.maturity/static_cast<double>(steps), steps, model.sigma,
        model.rate, model.yield, budget, work);
    ASSERT_TRUE(price);
    const double expected = linear ? model.spot*std::exp(-model.yield*model.maturity) :
        std::exp(-model.rate*model.maturity);
    const double tolerance = 128.*std::numeric_limits<double>::epsilon()*
        static_cast<double>(steps)*std::max(1., expected);
    EXPECT_NEAR(*price, expected, tolerance);
    EXPECT_EQ(work.completed_steps, steps);
    EXPECT_EQ(budget.expectations, steps*grid.size());
    EXPECT_EQ(budget.zero_state_evaluations, steps);
    EXPECT_EQ(budget.interval_evaluations, (grid.size()-1)*(2+(steps-1)*grid.size()));
    EXPECT_EQ(budget.created_knots, 2+steps*grid.size());
  }
}

TEST(CashGaussianFollowup, OneStepEuropeanIdentityAndPartialBudgetFailure) {
  cg::Model model;
  model.exercise = ExerciseStyle::European;
  const auto owner = cg::EventOwner::prepare(model, {});
  ASSERT_TRUE(owner);
  for (Side side : {Side::Call, Side::Put}) {
    cg::Budget budget;
    const std::array<double, 2> x{0., 100.}, y{side == Side::Call ? 0. : 100., 0.};
    auto initial = cg::StockPwl::create(x, y, side == Side::Call ? 1. : 0., budget);
    ASSERT_TRUE(initial);
    cg::SemigroupWork work;
    const auto result = cg::repeat_expectation(std::move(*initial), owner->stock_grid(),
        100., 1., 1, .2, .04, 0., budget, work);
    ASSERT_TRUE(result);
    EXPECT_NEAR(*result, black76_price(100.*std::exp(.04), 100., 1., .2,
                                       std::exp(-.04), side), 4e-12);
  }
  cg::Budget budget;
  budget.limits.max_expectations = 2;
  const std::array<double, 2> x{0., 100.}, y{100., 0.};
  auto initial = cg::StockPwl::create(x, y, 0., budget);
  ASSERT_TRUE(initial);
  cg::SemigroupWork work;
  work.completed_steps = 999;
  EXPECT_FALSE(cg::repeat_expectation(std::move(*initial), owner->stock_grid(),
      100., 1., 1, .2, .04, 0., budget, work));
  EXPECT_EQ(budget.expectations, 2u);
  EXPECT_EQ(work.completed_steps, 0u);
}
TEST(CashGaussianFollowup, MalformedSemigroupAndFiniteOverflowNeverPublishPartialPrice) {
  const std::array<double, 3> grid{0., 50., 100.};
  const std::array<double, 2> x{0., 100.}, y{100., 0.};
  for (int field = 0; field < 5; ++field) {
    cg::Budget budget;
    auto initial = cg::StockPwl::create(x, y, 0., budget);
    ASSERT_TRUE(initial);
    auto test_grid = grid;
    double stock = 100., sigma = .2;
    std::size_t steps = 1;
    switch (field) {
      case 0: test_grid[1] = 0.; break;
      case 1: stock = 99.; break;
      case 2: sigma = std::numeric_limits<double>::quiet_NaN(); break;
      case 3: steps = 0; break;
      default: steps = 4097; break;
    }
    cg::SemigroupWork work;
    EXPECT_FALSE(cg::repeat_expectation(std::move(*initial), test_grid, stock,
        1., steps, sigma, .04, 0., budget, work));
    EXPECT_EQ(budget.expectations, 0u);
    EXPECT_EQ(work.completed_steps, 0u);
  }
  for (bool overflow : {false, true}) {
    cg::Budget budget;
    if (!overflow) budget.limits.max_interval_evaluations = 1;
    auto initial = cg::StockPwl::create(x, y, 0., budget);
    ASSERT_TRUE(initial);
    cg::SemigroupWork work;
    const auto result = cg::repeat_expectation(std::move(*initial), grid, 100.,
        1., 1, .2, overflow ? -1000. : .04, 0., budget, work);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code(), overflow ? ErrorCode::OutOfRange : ErrorCode::Unavailable);
    EXPECT_EQ(budget.expectations, 1u);
    EXPECT_EQ(budget.interval_evaluations, 0u);
    EXPECT_EQ(work.completed_steps, 0u);
  }
}
} // namespace
