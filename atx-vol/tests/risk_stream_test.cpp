// RiskStream (api/risk/pnl_explain.hpp): one frame per state, each step equal to a
// standalone explain over the same frames, and the rate-move -> rho coupling.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "atx/vol/api/risk/pnl_explain.hpp"
#include "support/risk_synthetic_market.hpp"

using namespace atx::vol;
namespace rt = atx::vol::risk_test;

namespace {

// State k of a deterministic intraday path: half a day per step, spot / vol / skew
// drifting on every underlier.
[[nodiscard]] rt::StateMove path_move(int k) {
  rt::StateMove m;
  m.time_units = static_cast<std::int64_t>(k) * (rt::kDayUnits / 2);
  const double x = static_cast<double>(k);
  m.spot_ret = {0.003 * std::sin(x), -0.002 * std::sin(0.7 * x), 0.004 * std::cos(x) - 0.004};
  m.vol_level = 0.002 * std::sin(0.5 * x);
  m.vol_skew = 0.005 * std::sin(0.3 * x);
  return m;
}

template <class T>
[[nodiscard]] bool bytes_equal(const std::vector<T> &a, const std::vector<T> &b) {
  return a.size() == b.size() &&
         (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
}

} // namespace

TEST(RiskStream, FramesComputedOncePerState) {
  std::vector<std::unique_ptr<rt::OwnedState>> states;
  for (int k = 0; k < 11; ++k) {
    states.push_back(rt::make_state(path_move(k)));
  }
  const RiskBook book = RiskBook::create(rt::make_option_legs(), rt::make_share_legs()).value();
  RiskStream stream(book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  ASSERT_TRUE(stream.start(states[0]->state).has_value());
  EXPECT_EQ(stream.n_evaluations(), 1u);
  EXPECT_EQ(stream.n_explains(), 0u);
  for (std::size_t k = 1; k < states.size(); ++k) {
    const Result<const ExplainResult *> r = stream.step(states[k]->state);
    ASSERT_TRUE(r.has_value()) << k;
    ASSERT_NE(*r, nullptr);
    EXPECT_EQ((*r)->size(), book.n_option_legs() + book.n_share_legs());
    EXPECT_EQ((*r)->total.n_ok, (*r)->size()) << k;
    EXPECT_EQ(stream.current().ts_ns, states[k]->state.ts_ns());
    EXPECT_EQ(stream.previous().ts_ns, states[k - 1]->state.ts_ns());
  }
  EXPECT_EQ(stream.n_evaluations(), 11u);
  EXPECT_EQ(stream.n_explains(), 10u);
}

TEST(RiskStream, StepEqualsStandaloneExplain) {
  std::vector<std::unique_ptr<rt::OwnedState>> states;
  for (int k = 0; k < 3; ++k) {
    states.push_back(rt::make_state(path_move(k)));
  }
  const RiskBook book = RiskBook::create(rt::make_option_legs(), rt::make_share_legs()).value();
  ExplainOptions opts;
  opts.trapezoid_second_order = true;
  RiskStream stream(book, RiskKernel::family(), RiskNeeds{}, opts);
  ASSERT_TRUE(stream.start(states[0]->state).has_value());
  ASSERT_TRUE(stream.step(states[1]->state).has_value());
  const ExplainResult *r = stream.step(states[2]->state).value();
  const ExplainResult alone = explain(stream.prepared(), states[1]->state, stream.previous(),
                                      states[2]->state, stream.current(), opts)
                                  .value();
  for (const auto col :
       {&ExplainResult::pv_base, &ExplainResult::pv_target, &ExplainResult::pnl_total,
        &ExplainResult::pnl_delta, &ExplainResult::pnl_gamma, &ExplainResult::pnl_vega,
        &ExplainResult::pnl_volga, &ExplainResult::pnl_vanna, &ExplainResult::pnl_theta,
        &ExplainResult::pnl_rho, &ExplainResult::pnl_charm, &ExplainResult::pnl_unexplained,
        &ExplainResult::vol_level, &ExplainResult::vol_skew, &ExplainResult::vol_curv,
        &ExplainResult::vol_resid, &ExplainResult::d_spot, &ExplainResult::d_vol,
        &ExplainResult::d_time, &ExplainResult::d_rate}) {
    EXPECT_TRUE(bytes_equal(r->*col, alone.*col));
  }
  EXPECT_TRUE(bytes_equal(r->status, alone.status));
  EXPECT_TRUE(bytes_equal(r->flags, alone.flags));
  EXPECT_EQ(std::memcmp(&r->total, &alone.total, sizeof(ExplainTotals)), 0);
  EXPECT_EQ(r->n_pivot_groups, alone.n_pivot_groups);
}

TEST(RiskStream, RateMoveTurnsRhoOn) {
  std::vector<std::unique_ptr<rt::OwnedState>> states;
  for (int k = 0; k < 4; ++k) {
    rt::StateMove m = path_move(k);
    m.rate = 0.001 * static_cast<double>(k); // the rate moves on every step
    states.push_back(rt::make_state(m));
  }
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  RiskNeeds needs;
  needs.rho = false;
  RiskStream stream(book, RiskKernel::family(), needs, ExplainOptions{});
  ASSERT_TRUE(stream.start(states[0]->state).has_value());
  EXPECT_FALSE(stream.current().needs.rho);

  // Step 1: frame 0 had no rho, so the rate move is unexplained and flagged.
  const ExplainResult *r1 = stream.step(states[1]->state).value();
  EXPECT_TRUE(r1->rate_move_without_rho);
  EXPECT_FALSE(stream.current().needs.rho);
  EXPECT_EQ(r1->total.pnl_rho, 0.0);

  // Step 2: the move was seen, so frame 2 is evaluated WITH rho; its base (frame 1)
  // was evaluated before the move was known, so this step still reports it.
  const ExplainResult *r2 = stream.step(states[2]->state).value();
  EXPECT_TRUE(stream.current().needs.rho);
  EXPECT_GT(stream.current().counters.carry_sets_built, 0u);
  EXPECT_TRUE(r2->rate_move_without_rho);

  // Step 3: the base frame carries rho, the rate move lands in pnl_rho.
  const ExplainResult *r3 = stream.step(states[3]->state).value();
  EXPECT_FALSE(r3->rate_move_without_rho);
  EXPECT_NE(r3->total.pnl_rho, 0.0);
  EXPECT_TRUE(std::isfinite(r3->total.pnl_rho));

  // A path with a constant rate never turns rho on.
  std::vector<std::unique_ptr<rt::OwnedState>> flat;
  for (int k = 0; k < 3; ++k) {
    flat.push_back(rt::make_state(path_move(k)));
  }
  RiskStream quiet(book, RiskKernel::family(), needs, ExplainOptions{});
  ASSERT_TRUE(quiet.start(flat[0]->state).has_value());
  for (std::size_t k = 1; k < flat.size(); ++k) {
    const ExplainResult *r = quiet.step(flat[k]->state).value();
    EXPECT_FALSE(r->rate_move_without_rho);
    EXPECT_FALSE(quiet.current().needs.rho);
  }
}
