// W0-B0 (B-02, engine half): an execution delay of 0 fills at the decision
// close — the close any signal computed at that decision has already seen — so
// the replay rejects it unless ReplayConfig::allow_same_close is set. Every
// entry point shares the check; the default delay 1 is unaffected.

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/event_batch_builder.hpp"
#include "atx/engine/book/replay.hpp"

namespace atx_test_w0_b0_replay_delay {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

Panel panel_up() {
  // One name: 100 -> 110 -> 121 -> 133.1 (10 % a day).
  return Panel::create(4, 1, {"close"}, {std::vector<atx::f64>{100.0, 110.0, 121.0, 133.1}}, {})
      .value();
}

std::vector<atx::i64> days(atx::usize count) {
  std::vector<atx::i64> times;
  for (atx::usize i = 0; i < count; ++i) times.push_back(static_cast<atx::i64>(i) * kDay);
  return times;
}

book::ReplayConfig zero_delay() {
  book::ReplayConfig cfg;
  cfg.initial_nav = 100.0;
  cfg.execution_delay_periods = 0;
  return cfg;
}

bool mentions_opt_in(const atx::core::Error &error) {
  return error.message().find("allow_same_close") != std::string::npos;
}

TEST(BookReplayDelay, ZeroDelayIsRejectedWithoutTheOptIn) {
  const auto panel = panel_up();
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> target{1.0};
  const auto rejected =
      book::replay_scheduled_targets(panel, days(4), schedule, target, zero_delay());
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), atx::core::ErrorCode::InvalidArgument);
  EXPECT_TRUE(mentions_opt_in(rejected.error())) << rejected.error().message();
}

TEST(BookReplayDelay, EveryEntryPointSharesTheRejection) {
  const auto panel = panel_up();
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> preference{1.0};
  const book::ReplayAllocationPolicy weights = [](const book::ReplayAllocationState &state) {
    return atx::core::Result<std::vector<atx::f64>>(
        std::vector<atx::f64>(state.preference_weights.begin(), state.preference_weights.end()));
  };
  const book::ReplayIntentPolicy intents = [](const book::ReplayAllocationState &) {
    return atx::core::Result<std::vector<book::ReplayTargetIntent>>(
        std::vector<book::ReplayTargetIntent>{{book::ReplayTargetAction::TargetWeight, 1.0}});
  };
  const auto events = book::make_event_batch_policy(4, {}, {}).value();
  const auto by_policy =
      book::replay_scheduled_targets(panel, days(4), schedule, preference, weights, zero_delay());
  ASSERT_FALSE(by_policy.has_value());
  EXPECT_TRUE(mentions_opt_in(by_policy.error()));
  const auto by_intent =
      book::replay_scheduled_intents(panel, days(4), schedule, preference, intents, zero_delay());
  ASSERT_FALSE(by_intent.has_value());
  EXPECT_TRUE(mentions_opt_in(by_intent.error()));
  const auto by_claims = book::replay_scheduled_intents_with_events(
      panel, days(4), schedule, preference, intents, events, zero_delay(),
      book::ReplayClaimsConfig{});
  ASSERT_FALSE(by_claims.has_value());
  EXPECT_TRUE(mentions_opt_in(by_claims.error()));
}

TEST(BookReplayDelay, OptInAdmitsSameCloseAndMeasuresItsLookAhead) {
  // The decision at row 0 "sees" close 100. A same-close fill buys at 100 and
  // earns the whole 100 -> 133.1 path; the default delay-1 fill buys at 110.
  const auto panel = panel_up();
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> target{1.0};
  auto same_close = zero_delay();
  same_close.allow_same_close = true;
  const auto opted =
      book::replay_scheduled_targets(panel, days(4), schedule, target, same_close);
  ASSERT_TRUE(opted.has_value()) << opted.error().message();
  book::ReplayConfig next_close;
  next_close.initial_nav = 100.0;
  ASSERT_EQ(next_close.execution_delay_periods, 1U);
  ASSERT_FALSE(next_close.allow_same_close);
  const auto delayed =
      book::replay_scheduled_targets(panel, days(4), schedule, target, next_close);
  ASSERT_TRUE(delayed.has_value()) << delayed.error().message();
  EXPECT_NEAR(opted->final_nav, 133.1, 1.0e-9);   // +33.1 %: fills at the seen close.
  EXPECT_NEAR(delayed->final_nav, 121.0, 1.0e-9); // +21.0 %: fills at the next close.
  EXPECT_EQ(opted->trades.at(0).period, 0U);
  EXPECT_EQ(delayed->trades.at(0).period, 1U);
}

TEST(BookReplayDelay, DefaultConfigIsDelayOneWithoutSameClose) {
  const book::ReplayConfig cfg;
  EXPECT_EQ(cfg.execution_delay_periods, 1U);
  EXPECT_FALSE(cfg.allow_same_close);
  EXPECT_EQ(cfg.delisting_policy, book::DelistingPolicy::TerminalReturn);
  EXPECT_EQ(cfg.locate_breach, book::LocateBreach::ClipV2);
  // Any positive delay is accepted without the opt-in.
  auto two = cfg;
  two.initial_nav = 100.0;
  two.execution_delay_periods = 2;
  const std::vector<atx::usize> schedule{0};
  const std::vector<atx::f64> target{1.0};
  const auto result = book::replay_scheduled_targets(panel_up(), days(4), schedule, target, two);
  ASSERT_TRUE(result.has_value()) << result.error().message();
  EXPECT_EQ(result->trades.at(0).period, 2U);
}

} // namespace atx_test_w0_b0_replay_delay
