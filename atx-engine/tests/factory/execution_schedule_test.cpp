#include <array>
#include <bit>
#include <cmath>
#include <span>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/alpha/streams.hpp"
#include "atx/engine/cost/cost_surface.hpp"
#include "atx/engine/factory/execution_objective.hpp"
#include "atx/engine/loop/weight_policy.hpp"

namespace {
using namespace atx;
namespace ex = atx::engine::factory;
namespace cost = atx::engine::cost;
constexpr usize D = 12, N = 2;
constexpr i64 day = 86'400'000'000'000;
struct ScheduleInput {
  ex::ExecutionObjectiveConfig cfg;
  cost::CostSurfaceRecipe recipe;
  std::vector<f64> prices = std::vector<f64>(D * N, 100.0);
  std::vector<f64> signal = std::vector<f64>(D * N);
  std::vector<u8> member = std::vector<u8>(D * N, 1);
  std::vector<u8> present = std::vector<u8>(D * N, 1);
  std::vector<u32> guard = std::vector<u32>(D * N, 0);
  std::vector<i64> marks = std::vector<i64>(D), decisions = std::vector<i64>(D);
  f64 borrow{};
  ScheduleInput() {
    cfg.rule = ex::ExecutionObjectiveRule::DelayedSurfaceV2;
    cfg.initial_nav = 1000; cfg.window_end = D; cfg.maturity_end = D;
    recipe.rule = cost::CostSurfaceRule::ModeledInputsV2;
    recipe.impact_y = 0; recipe.commission_bps = 0;
    for (usize t = 0; t < D; ++t) {
      marks[t] = (100 + static_cast<i64>(t)) * day; decisions[t] = marks[t] + 1;
      signal[t * N] = -1; signal[t * N + 1] = 1;
    }
  }
  core::Result<atx::engine::alpha::Panel> panel() const {
    return atx::engine::alpha::Panel::create(D, N, {"close"}, {prices}, present);
  }
  core::Result<ex::ExecutionObjectiveContext> context(const atx::engine::alpha::Panel& p) const {
    const std::array<u64, N> ids{10, 20};
    std::array<cost::CostSurfaceRow, N> rows{};
    for (usize i = 0; i < N; ++i) {
      rows[i].instrument_id = ids[i]; rows[i].state = cost::CostInputState::Available;
      rows[i].available_at_ns = 1; rows[i].adv_dollars = 1e9;
      rows[i].borrow_state = cost::CostInputState::Available;
      rows[i].borrow_available_at_ns = 1; rows[i].borrow_annual_fraction = borrow;
    }
    std::vector<cost::CostSurface> snapshots;
    for (usize t = cfg.window_begin; t < D - cfg.delay - 1; ++t) {
      ATX_TRY(auto s, cost::CostSurface::create(recipe,
          {decisions[t], std::string(64, 'a'), "synthetic-prior", "explicit-test"}, rows));
      snapshots.push_back(std::move(s));
    }
    atx::engine::WeightPolicy weights; weights.winsorize_limit = 0;
    return ex::prepare_execution_objective(p, weights, cfg, snapshots, marks, decisions, ids,
        {std::string(64, 'b'), "synthetic-schedule", "synthetic-close"}, member, guard);
  }
};

TEST(ExecutionSchedule, DefaultCadencePreservesIdentityAndIndependentInitialLedger) {
  ScheduleInput f; f.recipe.commission_bps = 10;
  auto p = f.panel(); ASSERT_TRUE(p);
  auto implicit = f.context(*p); ASSERT_TRUE(implicit);
  f.cfg.rebalance_sessions = 1; f.cfg.trade_fraction = 1;
  auto explicit_default = f.context(*p); ASSERT_TRUE(explicit_default);
  EXPECT_EQ(implicit->identity_sha256(), explicit_default->identity_sha256());
  const auto out = ex::extract_execution_signal(f.signal, *implicit); ASSERT_TRUE(out);
  EXPECT_DOUBLE_EQ(out->turnover_flat[2], 1.0);
  EXPECT_DOUBLE_EQ(out->end_nav_flat[2], 999.0);
  EXPECT_DOUBLE_EQ(out->positions(0, 2)[0], -.5);
  EXPECT_DOUBLE_EQ(out->positions(0, 2)[1], .5);
  f.cfg.trade_fraction = .25;
  auto partial = f.context(*p); ASSERT_TRUE(partial);
  EXPECT_NE(implicit->identity_sha256(), partial->identity_sha256());
}

TEST(ExecutionSchedule, OffCycleKeepsMarkedReturnsAndCalendarBorrowWithoutTrades) {
  ScheduleInput f; f.cfg.rebalance_sessions = 5; f.borrow = .365;
  for (usize t = 2; t < D; ++t) f.prices[t * N + 1] = t == 2 ? 110.0 : 121.0;
  auto p = f.panel(); ASSERT_TRUE(p);
  auto c = f.context(*p); ASSERT_TRUE(c);
  auto out = ex::extract_execution_signal(f.signal, *c); ASSERT_TRUE(out);
  EXPECT_NEAR(out->end_nav_flat[2], 1049.5, 1e-12);
  EXPECT_NEAR(out->end_nav_flat[3], 1104.0, 1e-12);
  EXPECT_NEAR(out->gross_flat[3], 55.0 / 1049.5, 1e-14);
  EXPECT_NEAR(out->borrow_cost_flat[3], .5 / 1049.5, 1e-14);
  for (usize t = 3; t <= 6; ++t) {
    EXPECT_EQ(out->valid_flat[t], 1);
    EXPECT_DOUBLE_EQ(out->turnover_flat[t], 0);
    EXPECT_DOUBLE_EQ(out->execution_cost_flat[t], 0);
  }
  // A later input mutation cannot change the already realized off-cycle prefix.
  for (usize t = 7; t < D; ++t) { f.prices[t * N] = 90; f.signal[t * N] = 100; }
  auto p2 = f.panel(); ASSERT_TRUE(p2); auto c2 = f.context(*p2); ASSERT_TRUE(c2);
  auto out2 = ex::extract_execution_signal(f.signal, *c2); ASSERT_TRUE(out2);
  for (usize t = 2; t < 7; ++t)
    EXPECT_EQ(std::bit_cast<u64>(out->pnl_flat[t]), std::bit_cast<u64>(out2->pnl_flat[t]));
}

TEST(ExecutionSchedule, LongerDelayFreezesPartialTargetsBeforeOtherQueuedFills) {
  ScheduleInput f; f.cfg.delay = 3; f.cfg.rebalance_sessions = 2; f.cfg.trade_fraction = .5;
  auto p = f.panel(); ASSERT_TRUE(p); auto c = f.context(*p); ASSERT_TRUE(c);
  auto out = ex::extract_execution_signal(f.signal, *c); ASSERT_TRUE(out);
  // d0 and d2 both observe flat holdings and independently queue +/-250.
  // The second order at5 therefore adds NOTHING, rather than another 50%.
  EXPECT_DOUBLE_EQ(out->positions(0, 4)[0], -.25);
  EXPECT_DOUBLE_EQ(out->positions(0, 6)[0], -.25);
  EXPECT_DOUBLE_EQ(out->turnover_flat[6], 0);
  // d4 observes the first held +/-250, queues +/-375, and fills at7.
  EXPECT_DOUBLE_EQ(out->positions(0, 8)[0], -.375);
  EXPECT_DOUBLE_EQ(out->positions(0, 8)[1], .375);
  EXPECT_DOUBLE_EQ(out->turnover_flat[8], .25);
  EXPECT_DOUBLE_EQ(out->turnover_flat[5], 0);
  EXPECT_DOUBLE_EQ(out->turnover_flat[7], 0);
}

TEST(ExecutionSchedule, MembershipExitIsPartialAtScheduledEntry) {
  ScheduleInput f; f.cfg.rebalance_sessions = 2; f.cfg.trade_fraction = .5;
  for (usize t = 2; t < D; ++t) f.member[t * N + 1] = 0;
  auto p = f.panel(); ASSERT_TRUE(p); auto c = f.context(*p); ASSERT_TRUE(c);
  auto out = ex::extract_execution_signal(f.signal, *c); ASSERT_TRUE(out);
  EXPECT_DOUBLE_EQ(out->positions(0, 2)[1], .25);
  EXPECT_DOUBLE_EQ(out->positions(0, 3)[1], .25); // off-cycle does not flatten
  EXPECT_DOUBLE_EQ(out->positions(0, 4)[1], .125);
  EXPECT_DOUBLE_EQ(out->positions(0, 6)[1], .0625);
}

TEST(ExecutionSchedule, StrictRefusalIdentifiesActualClockNameExposureAndGuard) {
  {
    ScheduleInput f; f.present[N + 1] = 0; f.prices[N + 1] = 777;
    auto p = f.panel(); ASSERT_TRUE(p); auto c = f.context(*p); ASSERT_TRUE(c);
    const auto out = ex::extract_execution_signal(f.signal, *c); ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code(), core::ErrorCode::Unavailable);
    const auto& message = out.error().message();
    EXPECT_NE(message.find("unavailable entry price"), std::string::npos);
    EXPECT_NE(message.find("date_index=1 "), std::string::npos);
    EXPECT_NE(message.find("mark_time_ns=" + std::to_string(f.marks[1])), std::string::npos);
    EXPECT_NE(message.find("instrument_id=20 source_present=0"), std::string::npos);
    EXPECT_NE(message.find("decision_index=0 "), std::string::npos);
    EXPECT_NE(message.find("held_dollars=0"), std::string::npos);
    EXPECT_NE(message.find("requested_dollars=500 quoted_fill_dollars=500"), std::string::npos);
  }
  for (const bool guarded : {false, true}) {
    ScheduleInput f;
    if (guarded) for (usize t = 2; t < D; ++t) f.guard[t * N + 1] = 1;
    else { f.present[2 * N + 1] = 0; f.prices[2 * N + 1] = 777; }
    auto p = f.panel(); ASSERT_TRUE(p); auto c = f.context(*p); ASSERT_TRUE(c);
    const auto out = ex::extract_execution_signal(f.signal, *c); ASSERT_FALSE(out);
    EXPECT_EQ(out.error().code(), core::ErrorCode::Unavailable);
    const auto& message = out.error().message();
    EXPECT_NE(message.find("missing/guarded held return"), std::string::npos);
    EXPECT_NE(message.find("date_index=2 "), std::string::npos);
    EXPECT_NE(message.find("instrument_id=20"), std::string::npos);
    EXPECT_NE(message.find("held_dollars=500"), std::string::npos);
    EXPECT_NE(message.find("previous_source_present=1 previous_price=100"), std::string::npos);
    EXPECT_NE(message.find(guarded ? "guard_crossed=1" : "guard_crossed=0"), std::string::npos);
  }
}
} // namespace
