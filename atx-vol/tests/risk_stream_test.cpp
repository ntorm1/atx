// RiskStream (api/risk/pnl_explain.hpp): one frame per state, each step equal to a
// standalone explain over the same frames, the rate-move -> rho coupling, and the
// failure rule (a failed step invalidates the stream until the next start).

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
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

// A state on the VolTime clock at the fixture's instant (Nov 2023, before the
// convention's covered 2024-2032 window): its TimeSpec refuses every book expiry, so
// evaluating it is a structural failure.
[[nodiscard]] std::unique_ptr<rt::OwnedState> refusing_state(const rt::StateMove &m) {
  std::unique_ptr<rt::OwnedState> s = rt::make_state(m);
  s->storage.time.convention = TimeConvention::VolTime;
  s->state = make_market_state(s->storage).value();
  return s;
}

// Both frames of an invalidated stream are cleared: safe to read, no stale numbers.
void expect_cleared(const RiskFrame &f) {
  ASSERT_GT(f.size(), 0u);
  for (std::size_t r = 0; r < f.size(); ++r) {
    EXPECT_EQ(f.status[r], RiskLaneStatus::Refused) << r;
    EXPECT_EQ(f.route[r], RiskRoute::None) << r;
    EXPECT_TRUE(std::isnan(f.price[r])) << r;
  }
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

TEST(RiskStream, FailedStepInvalidatesStream) {
  std::vector<std::unique_ptr<rt::OwnedState>> states;
  for (int k = 0; k < 4; ++k) {
    states.push_back(rt::make_state(path_move(k)));
  }
  const std::unique_ptr<rt::OwnedState> bad = refusing_state(path_move(2));
  ASSERT_FALSE(bad->state.time_to_expiry(rt::expiry_ns(0)).has_value()) << "the premise";
  const RiskBook book = RiskBook::create(rt::make_option_legs(), rt::make_share_legs()).value();
  RiskStream stream(book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  ASSERT_TRUE(stream.start(states[0]->state).has_value());
  ASSERT_TRUE(stream.step(states[1]->state).has_value());

  // States[1] is the stream's previous state now; the failing step must drop it.
  const Result<const ExplainResult *> failed = stream.step(bad->state);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code(), ErrorCode::InvalidArgument);
  const std::uint64_t evaluations = stream.n_evaluations();

  // LIFETIME lets the caller free states[1] once step returns, failed or not: the
  // next step must be refused, never explained against the freed state.
  states[1].reset();
  const Result<const ExplainResult *> after = stream.step(states[3]->state);
  ASSERT_FALSE(after.has_value());
  EXPECT_EQ(after.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(stream.n_evaluations(), evaluations) << "refused before evaluating";
  EXPECT_EQ(stream.n_explains(), 1u);
  expect_cleared(stream.current());
  expect_cleared(stream.previous());

  // A new start revives it.
  ASSERT_TRUE(stream.start(states[2]->state).has_value());
  const Result<const ExplainResult *> revived = stream.step(states[3]->state);
  ASSERT_TRUE(revived.has_value());
  EXPECT_EQ((*revived)->total.n_ok, (*revived)->size());
  EXPECT_EQ(stream.n_explains(), 2u);
}

TEST(RiskStream, FailedExplainInvalidatesStream) {
  std::vector<std::unique_ptr<rt::OwnedState>> states;
  for (int k = 0; k < 3; ++k) {
    states.push_back(rt::make_state(path_move(k)));
  }
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  ExplainOptions opts;
  opts.z_ref = 0.0; // explain_into refuses every step; evaluate does not
  RiskStream stream(book, RiskKernel::family(), RiskNeeds{}, opts);
  ASSERT_TRUE(stream.start(states[0]->state).has_value());
  const Result<const ExplainResult *> failed = stream.step(states[1]->state);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(stream.n_evaluations(), 2u) << "the failing step evaluated, then explain refused";

  states[0].reset(); // the base of the failed step: free to go once step returned
  const Result<const ExplainResult *> after = stream.step(states[2]->state);
  ASSERT_FALSE(after.has_value());
  EXPECT_EQ(after.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(stream.n_evaluations(), 2u) << "refused before evaluating";
  EXPECT_EQ(stream.n_explains(), 0u);
  expect_cleared(stream.current());
  expect_cleared(stream.previous());
}

TEST(RiskStream, MovedFromStreamIsSafe) {
  const auto s0 = rt::make_state(path_move(0));
  const RiskBook book = RiskBook::create(rt::make_option_legs(), {}).value();
  RiskStream a(book, RiskKernel::family(), RiskNeeds{}, ExplainOptions{});
  RiskStream b = std::move(a);
  // Accessors on the moved-from stream return documented empty values.
  // NOLINTBEGIN(bugprone-use-after-move): the moved-from contract is what is pinned.
  EXPECT_EQ(a.prepared().n_unique(), 0u);
  EXPECT_EQ(a.current().size(), 0u);
  EXPECT_EQ(a.previous().size(), 0u);
  EXPECT_EQ(a.n_evaluations(), 0u);
  EXPECT_FALSE(a.start(s0->state).has_value());
  EXPECT_FALSE(a.step(s0->state).has_value());
  // NOLINTEND(bugprone-use-after-move)
  ASSERT_TRUE(b.start(s0->state).has_value());
  EXPECT_EQ(b.current().size(), b.prepared().n_unique());
}
