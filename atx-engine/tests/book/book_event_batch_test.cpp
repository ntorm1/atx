#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/event_batch_builder.hpp"
#include "atx/engine/book/replay.hpp"

namespace atx_test_l8_e2e_event_batch {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;
constexpr atx::f64 kNaN = std::numeric_limits<atx::f64>::quiet_NaN();

inline std::vector<atx::i64> day_axis(atx::usize count) {
  std::vector<atx::i64> times;
  for (atx::usize i = 0; i < count; ++i) times.push_back(static_cast<atx::i64>(i) * kDay);
  return times;
}

inline book::ReplayConfig immediate() {
  book::ReplayConfig cfg;
  cfg.initial_nav = 1000.0;
  cfg.execution_delay_periods = 0;
  return cfg;
}

// Two names, 7 observations. Name 1 prints through period 2 at 50 -> 60 and
// has no close from period 3 on (its formal delisting is recorded at period 5).
// Name 0 is universe-eligible throughout; name 1 only while it prints.
inline std::vector<atx::f64> delisting_close() {
  return {100, 50, 100, 55, 100, 60, 100, kNaN, 100, kNaN, 100, kNaN, 100, kNaN};
}

inline Panel delisting_panel() {
  std::vector<atx::u8> universe{1, 1, 1, 1, 1, 1, 1, 0, 1, 0, 1, 0, 1, 0};
  return Panel::create(7, 2, {"close"}, {delisting_close()}, std::move(universe)).value();
}

inline void expect_identity(const book::ReplayResult &result) {
  for (const auto &row : result.intervals) {
    EXPECT_NEAR(row.cash + row.assets, row.nav, 1.0e-9);
    EXPECT_NEAR(row.pretrade_nav + row.gross_pnl - row.trade_cost - row.borrow_cost, row.nav,
                1.0e-9);
  }
}
} // namespace atx_test_l8_e2e_event_batch

using namespace atx_test_l8_e2e_event_batch;

TEST(BookEventBatch, AbortPolicyStillFailsOnTheMissingHeldClose) {
  const auto panel = delisting_panel();
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.5, 0.2};
  const auto result =
      book::replay_scheduled_targets(panel, day_axis(7), decisions, targets, immediate());
  ASSERT_FALSE(result.has_value());
  EXPECT_NE(result.error().message().find("period=3 instrument=1"), std::string::npos);
}

TEST(BookEventBatch, DelistingAfterMissingHeldCloseDoesNotAbort) {
  const auto close = delisting_close();
  const std::vector<book::DelistingRecord> records{{1, 5, kNaN}};
  const auto events = book::delisting_events_from_records(close, 7, 2, records);
  ASSERT_TRUE(events.has_value()) << events.error().message();
  ASSERT_EQ(events->size(), 1U);
  EXPECT_EQ((*events)[0].last_valid_period, 2U); // Anchored on the last print.

  const auto panel = delisting_panel();
  auto cfg = immediate();
  cfg.delisting_policy = book::DelistingPolicy::LastMarkZeroReturn;
  cfg.delistings = *events;
  const std::vector<atx::usize> decisions{0, 4};
  const std::vector<atx::f64> targets{0.5, 0.2, 0.5, 0.0};
  const auto result = book::replay_scheduled_targets(panel, day_axis(7), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  ASSERT_EQ(result->delistings.size(), 1U);
  const auto &gone = result->delistings[0];
  EXPECT_EQ(gone.period, 3U);
  EXPECT_EQ(gone.instrument, 1U);
  EXPECT_DOUBLE_EQ(gone.tri_units, 4.0);     // 200$ / 50.
  EXPECT_DOUBLE_EQ(gone.last_value, 240.0);  // 4 * 60.
  EXPECT_DOUBLE_EQ(gone.proceeds, 240.0);
  EXPECT_DOUBLE_EQ(result->final_tri_units[1], 0.0);
  EXPECT_DOUBLE_EQ(result->intervals[2].gross_pnl, 0.0); // No move for either name.
  EXPECT_EQ(result->effective_rebalances, 2U);
  expect_identity(*result);
}

TEST(BookEventBatch, CrspDelistReturnIsRealisedAsGrossPnl) {
  const auto close = delisting_close();
  const std::vector<book::DelistingRecord> records{{1, 3, -0.3}};
  const auto events = book::delisting_events_from_records(close, 7, 2, records);
  ASSERT_TRUE(events.has_value());
  auto cfg = immediate();
  cfg.delisting_policy = book::DelistingPolicy::CrspDelistReturn;
  cfg.delistings = *events;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.0, 0.2};
  const auto result =
      book::replay_scheduled_targets(delisting_panel(), day_axis(7), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_NEAR(result->intervals[2].gross_pnl, -72.0, 1.0e-12); // 240 * -30%.
  ASSERT_EQ(result->delistings.size(), 1U);
  EXPECT_NEAR(result->delistings[0].proceeds, 168.0, 1.0e-12);
  EXPECT_NEAR(result->final_cash, 800.0 + 168.0, 1.0e-12);
  EXPECT_NEAR(result->final_nav, 968.0, 1.0e-12);
  expect_identity(*result);
}

TEST(BookEventBatch, CrspPolicyRejectsAnUnfilledReturnButAcceptsAnExplicitFill) {
  const auto close = delisting_close();
  const std::vector<book::DelistingRecord> records{{1, 5, kNaN}};
  const auto unfilled = book::delisting_events_from_records(close, 7, 2, records).value();
  auto cfg = immediate();
  cfg.delisting_policy = book::DelistingPolicy::CrspDelistReturn;
  cfg.delistings = unfilled;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.0, 0.2};
  EXPECT_FALSE(
      book::replay_scheduled_targets(delisting_panel(), day_axis(7), decisions, targets, cfg));
  const auto filled = book::delisting_events_from_records(close, 7, 2, records, -0.3).value();
  cfg.delistings = filled;
  EXPECT_TRUE(
      book::replay_scheduled_targets(delisting_panel(), day_axis(7), decisions, targets, cfg));
}

TEST(BookEventBatch, ShortDelistingBuysBackAtTheDelistingValue) {
  const auto close = delisting_close();
  const auto events = book::detect_terminal_delistings(close, 7, 2, -1.0).value();
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].instrument, 1U);
  EXPECT_EQ(events[0].last_valid_period, 2U);
  auto cfg = immediate();
  cfg.delisting_policy = book::DelistingPolicy::CrspDelistReturn;
  cfg.delistings = events;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> targets{0.0, -0.2};
  const auto result =
      book::replay_scheduled_targets(delisting_panel(), day_axis(7), decisions, targets, cfg);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  // A -100% delisting return wipes a short's liability: gain = 240.
  EXPECT_NEAR(result->intervals[2].gross_pnl, 240.0, 1.0e-12);
  EXPECT_NEAR(result->delistings[0].proceeds, 0.0, 1.0e-12);
  expect_identity(*result);
}

TEST(BookEventBatch, RecordBuilderRejectsInconsistentRecords) {
  const auto close = delisting_close();
  const std::vector<book::DelistingRecord> prints_after{{0, 3, 0.0}};
  EXPECT_FALSE(book::delisting_events_from_records(close, 7, 2, prints_after).has_value());
  const std::vector<book::DelistingRecord> duplicate{{1, 5, 0.0}, {1, 6, 0.0}};
  EXPECT_FALSE(book::delisting_events_from_records(close, 7, 2, duplicate).has_value());
  const std::vector<book::DelistingRecord> out_of_range{{2, 5, 0.0}};
  EXPECT_FALSE(book::delisting_events_from_records(close, 7, 2, out_of_range).has_value());
}

TEST(BookEventBatch, ScheduledPolicyServesBatchesByObservation) {
  book::ScheduledTransition first{};
  first.period = 2;
  first.request.event.event_id = 11;
  book::ScheduledTransition second{};
  second.period = 2;
  second.request.event.event_id = 12;
  book::ScheduledPayment paid{};
  paid.period = 4;
  paid.payment.payment_id = 21;
  const std::vector<book::ScheduledTransition> transitions{first, second};
  const std::vector<book::ScheduledPayment> payments{paid};
  const auto policy = book::make_event_batch_policy(6, transitions, payments);
  ASSERT_TRUE(policy.has_value()) << policy.error().message();
  book::ReplayEventContext context{};
  context.period = 2;
  const auto at_two = (*policy)(context).value();
  ASSERT_EQ(at_two.transition_count, 2U);
  EXPECT_EQ(at_two.transitions[0].event.event_id, 11U);
  EXPECT_EQ(at_two.transitions[1].event.event_id, 12U);
  EXPECT_EQ(at_two.payment_count, 0U);
  context.period = 4;
  const auto at_four = (*policy)(context).value();
  EXPECT_EQ(at_four.payment_count, 1U);
  EXPECT_EQ(at_four.payments[0].payment_id, 21U);
  context.period = 3;
  EXPECT_EQ((*policy)(context).value().transition_count, 0U);
}

TEST(BookEventBatch, ScheduledPolicyRejectsUnqueriedPeriodsAndOverflow) {
  book::ScheduledTransition at_zero{};
  at_zero.period = 0;
  EXPECT_FALSE(book::make_event_batch_policy(6, std::vector{at_zero}, {}).has_value());
  book::ScheduledTransition terminal{};
  terminal.period = 5;
  EXPECT_FALSE(book::make_event_batch_policy(6, std::vector{terminal}, {}).has_value());
  std::vector<book::ScheduledTransition> crowded(book::kMaxEventsPerObservation + 1);
  for (auto &t : crowded) t.period = 2;
  EXPECT_FALSE(book::make_event_batch_policy(6, crowded, {}).has_value());
}

TEST(BookEventBatch, EmptyScheduledPolicyReproducesIntentReplay) {
  const std::vector<atx::f64> close{100, 50, 101, 52, 99, 55, 102, 54, 104, 53};
  const auto panel = Panel::create(5, 2, {"close"}, {close}, {}).value();
  const auto policy = book::make_event_batch_policy(5, {}, {}).value();
  const book::ReplayIntentPolicy intents = [](const book::ReplayAllocationState &state)
      -> atx::core::Result<std::vector<book::ReplayTargetIntent>> {
    std::vector<book::ReplayTargetIntent> out(state.preference_weights.size());
    for (atx::usize i = 0; i < out.size(); ++i) out[i].weight = state.preference_weights[i];
    return atx::core::Ok(std::move(out));
  };
  const std::vector<atx::usize> decisions{0, 2};
  const std::vector<atx::f64> prefs{0.4, -0.2, 0.1, 0.3};
  auto cfg = immediate();
  cfg.trade_bps = 3.0;
  const auto plain =
      book::replay_scheduled_intents(panel, day_axis(5), decisions, prefs, intents, cfg);
  const auto evented = book::replay_scheduled_intents_with_events(
      panel, day_axis(5), decisions, prefs, intents, policy, cfg, book::ReplayClaimsConfig{});
  ASSERT_TRUE(plain.has_value()) << plain.error().message();
  ASSERT_TRUE(evented.has_value()) << evented.error().message();
  EXPECT_EQ(evented->policy.replay.final_nav, plain->replay.final_nav);
  EXPECT_EQ(evented->policy.replay.trades.size(), plain->replay.trades.size());
}

TEST(BookEventBatch, ClaimsReplayRejectsTheOptInReplayExtensions) {
  const auto panel = Panel::create(3, 1, {"close"}, {std::vector<atx::f64>{1, 1, 1}}, {}).value();
  const auto policy = book::make_event_batch_policy(3, {}, {}).value();
  const book::ReplayIntentPolicy intents = [](const book::ReplayAllocationState &)
      -> atx::core::Result<std::vector<book::ReplayTargetIntent>> {
    return atx::core::Ok(std::vector<book::ReplayTargetIntent>(1));
  };
  auto cfg = immediate();
  cfg.delisting_policy = book::DelistingPolicy::LastMarkZeroReturn;
  const std::vector<atx::usize> decisions{0};
  const std::vector<atx::f64> prefs{0.0};
  EXPECT_FALSE(book::replay_scheduled_intents_with_events(
      panel, day_axis(3), decisions, prefs, intents, policy, cfg, book::ReplayClaimsConfig{}));
}
