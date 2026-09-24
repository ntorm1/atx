#include "replay_diagnostics.hpp"

#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace {
namespace book = atx::engine::book;
using atx::engine::alpha::Panel;
using atx::impl::diagnose_replay;
using atx::impl::ReplayLiquidityStatus;
constexpr atx::i64 kDay = 86'400'000'000'000LL;

Panel prices(std::vector<atx::f64> close, std::vector<atx::f64> raw = {},
             std::vector<atx::f64> volume = {}) {
    const auto dates = close.size();
    if (raw.empty()) raw = close;
    if (volume.empty()) volume.assign(dates, 1000.0);
    return Panel::create(dates, 1, {"close", "raw_close", "volume"},
                         {std::move(close), std::move(raw), std::move(volume)}, {}).value();
}

std::vector<atx::i64> days(atx::usize count) {
    std::vector<atx::i64> times;
    for (atx::usize i = 0; i < count; ++i) {
        times.push_back(static_cast<atx::i64>(i) * kDay);
    }
    return times;
}

book::ReplayConfig configuration(atx::f64 initial_nav = 1000.0) {
    book::ReplayConfig config;
    config.initial_nav = initial_nav;
    return config;
}
} // namespace

TEST(ReplayDiagnosticsTest, AllIntervalsDriveNavDrawdownAndSampleSharpe) {
    const auto panel = prices({100, 100, 110, 99, 108.9});
    const std::vector<atx::usize> decisions{0};
    const std::vector<atx::f64> targets{1.0};
    const auto replay = book::replay_scheduled_targets(
        panel, days(5), decisions, targets, configuration());
    ASSERT_TRUE(replay.has_value()) << replay.error().message();
    const auto result = diagnose_replay(*replay, panel);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    const auto &full = result->full;
    EXPECT_EQ(full.observed_intervals, 4U);
    EXPECT_DOUBLE_EQ(full.initial_nav, 1000.0);
    EXPECT_NEAR(full.final_nav, 1089.0, 1e-9);
    EXPECT_NEAR(full.gross_pnl_dollars, 89.0, 1e-9);
    EXPECT_NEAR(full.net_pnl_dollars, 89.0, 1e-9);
    EXPECT_DOUBLE_EQ(full.abs_trade_dollars, 1000.0);
    EXPECT_DOUBLE_EQ(full.trade_cost_dollars, 0.0);
    EXPECT_DOUBLE_EQ(full.borrow_cost_dollars, 0.0);
    EXPECT_NEAR(full.total_return, 0.089, 1e-12);
    EXPECT_NEAR(full.max_drawdown, 0.1, 1e-12);
    // All four interval returns, including the initial cash interval: 0,.1,-.1,.1.
    const auto expected_sharpe = 0.025 / std::sqrt(0.0275 / 3.0) * std::sqrt(252.0);
    ASSERT_TRUE(full.sharpe_252.has_value());
    EXPECT_NEAR(*full.sharpe_252, expected_sharpe, 1e-11);
    EXPECT_FALSE(result->post_fit.has_value());
}

TEST(ReplayDiagnosticsTest, DollarCostsBorrowAndUnexecutedFinalDecision) {
    const auto panel = prices({100, 100, 90, 90});
    const std::vector<atx::usize> decisions{0, 2};
    const std::vector<atx::f64> targets{-0.5, 0.0};
    auto config = configuration();
    config.trade_bps = 10.0;
    config.annual_borrow_bps = 365.0;
    const auto replay = book::replay_scheduled_targets(panel, days(4), decisions, targets, config);
    ASSERT_TRUE(replay.has_value()) << replay.error().message();
    EXPECT_EQ(replay->unexecuted_decisions, 1U);
    const auto result = diagnose_replay(*replay, panel, 2);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->full.observed_intervals, 3U);
    EXPECT_NEAR(result->full.gross_pnl_dollars, 50.0, 1e-10);
    EXPECT_NEAR(result->full.trade_cost_dollars, 0.5, 1e-12);
    // Post-trade short notional is 500 on the first holding interval, then 450.
    EXPECT_NEAR(result->full.borrow_cost_dollars, 0.05 + 0.045, 1e-12);
    EXPECT_NEAR(result->full.abs_trade_dollars, 500.0, 1e-10);
    EXPECT_NEAR(result->full.net_pnl_dollars, 49.405, 1e-9);
    EXPECT_NEAR(result->full.final_nav, 1049.405, 1e-9);
    EXPECT_NEAR(result->full.total_return, 0.049405, 1e-12);
    EXPECT_FALSE(result->post_fit.has_value());
    EXPECT_FALSE(result->first_post_fit_effective_observation.has_value());
}

TEST(ReplayDiagnosticsTest, PostFitStartsAtQualifyingDecisionEvenWithoutNewTrade) {
    const auto panel = prices({100, 100, 110, 121, 133.1, 146.41});
    const std::vector<atx::usize> decisions{0, 3};
    const std::vector<atx::f64> targets{1.0, 1.0};
    const auto replay = book::replay_scheduled_targets(
        panel, days(6), decisions, targets, configuration());
    ASSERT_TRUE(replay.has_value()) << replay.error().message();
    const auto result = diagnose_replay(*replay, panel, 3);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_TRUE(result->post_fit.has_value());
    EXPECT_EQ(result->first_post_fit_effective_observation, 4U);
    EXPECT_EQ(result->post_fit->observed_intervals, 1U);
    // The interval starting at 3 still carries decision 0 and must be excluded.
    EXPECT_NEAR(result->post_fit->initial_nav, 1331.0, 1e-9);
    EXPECT_NEAR(result->post_fit->final_nav, 1464.1, 1e-9);
    EXPECT_NEAR(result->post_fit->gross_pnl_dollars, 133.1, 1e-9);
    EXPECT_NEAR(result->post_fit->abs_trade_dollars, 0.0, 1e-9);
    EXPECT_FALSE(result->post_fit->sharpe_252.has_value());
    const auto beyond = diagnose_replay(*replay, panel, 6);
    ASSERT_TRUE(beyond.has_value());
    EXPECT_FALSE(beyond->post_fit.has_value());
    EXPECT_FALSE(diagnose_replay(*replay, panel, 7).has_value());
}

TEST(ReplayDiagnosticsTest, PriorRawDollarAdvIsCausalAndUncapped) {
    const std::vector<atx::f64> close(24, 5.0);
    std::vector<atx::f64> raw(24, 100.0);
    const std::vector<atx::f64> volume(24, 10.0);
    const auto panel = prices(close, raw, volume);
    const std::vector<atx::usize> decisions{20};
    const std::vector<atx::f64> targets{1.0};
    const auto replay = book::replay_scheduled_targets(
        panel, days(24), decisions, targets, configuration(2000.0));
    ASSERT_TRUE(replay.has_value()) << replay.error().message();
    // Neither execution-day nor future liquidity is a permitted ADV input.
    for (atx::usize i = 21; i < raw.size(); ++i) {
        raw[i] = std::numeric_limits<atx::f64>::infinity();
    }
    const auto result = diagnose_replay(*replay, prices(close, raw, volume));
    ASSERT_TRUE(result.has_value()) << result.error().message();
    ASSERT_EQ(result->trade_participation.size(), 1U);
    const auto &trade = result->trade_participation[0];
    EXPECT_EQ(trade.trade_index, 0U);
    EXPECT_EQ(trade.status, ReplayLiquidityStatus::Known);
    ASSERT_TRUE(trade.prior_dollar_adv.has_value());
    ASSERT_TRUE(trade.participation.has_value());
    EXPECT_DOUBLE_EQ(*trade.prior_dollar_adv, 1000.0);
    EXPECT_DOUBLE_EQ(*trade.participation, 2.0);
    EXPECT_EQ(result->known_participation_count, 1U);
    EXPECT_EQ(result->unknown_participation_count, 0U);
    EXPECT_EQ(result->max_participation, 2.0);
    EXPECT_EQ(result->max_known_participation, 2.0);
    EXPECT_FALSE(result->full.sharpe_252.has_value());
}

TEST(ReplayDiagnosticsTest, MissingLiquidityIsUnknownAndDoesNotUseAdjustedClose) {
    const std::vector<atx::f64> close(24, 5.0);
    std::vector<atx::f64> raw(24, 100.0);
    const std::vector<atx::f64> volume(24, 10.0);
    const auto panel = prices(close, raw, volume);
    const std::vector<atx::usize> decisions{0, 20};
    const std::vector<atx::f64> targets{1.0, 2.0};
    const auto replay = book::replay_scheduled_targets(
        panel, days(24), decisions, targets, configuration(2000.0));
    ASSERT_TRUE(replay.has_value()) << replay.error().message();
    const auto partial = diagnose_replay(*replay, panel);
    ASSERT_TRUE(partial.has_value()) << partial.error().message();
    ASSERT_EQ(partial->trade_participation.size(), 2U);
    EXPECT_EQ(partial->trade_participation[0].status, ReplayLiquidityStatus::InsufficientHistory);
    EXPECT_EQ(partial->known_participation_count, 1U);
    EXPECT_EQ(partial->unknown_participation_count, 1U);
    EXPECT_FALSE(partial->max_participation.has_value());
    EXPECT_EQ(partial->max_known_participation, 2.0);

    raw[5] = std::numeric_limits<atx::f64>::quiet_NaN();
    const auto invalid = diagnose_replay(*replay, prices(close, raw, volume));
    ASSERT_TRUE(invalid.has_value());
    EXPECT_EQ(invalid->trade_participation[1].status, ReplayLiquidityStatus::InvalidWindow);
    EXPECT_EQ(invalid->unknown_participation_count, 2U);
    EXPECT_FALSE(invalid->max_known_participation.has_value());

    const auto no_raw = Panel::create(24, 1, {"close", "volume"}, {close, volume}, {}).value();
    const auto missing = diagnose_replay(*replay, no_raw);
    ASSERT_TRUE(missing.has_value());
    EXPECT_EQ(missing->unknown_participation_count, 2U);
    EXPECT_EQ(missing->trade_participation[1].status, ReplayLiquidityStatus::MissingFields);
    EXPECT_FALSE(missing->max_participation.has_value());
}

TEST(ReplayDiagnosticsTest, SingletonIsUnavailableAndMalformedLedgerFails) {
    const std::vector<atx::usize> decisions{0};
    const std::vector<atx::f64> targets{1.0};
    const auto singleton = prices({100});
    const auto empty = book::replay_scheduled_targets(
        singleton, days(1), decisions, targets, configuration());
    ASSERT_TRUE(empty.has_value()) << empty.error().message();
    const auto result = diagnose_replay(*empty, singleton);
    ASSERT_TRUE(result.has_value()) << result.error().message();
    EXPECT_EQ(result->full.observed_intervals, 0U);
    EXPECT_DOUBLE_EQ(result->full.total_return, 0.0);
    EXPECT_DOUBLE_EQ(result->full.max_drawdown, 0.0);
    EXPECT_FALSE(result->full.sharpe_252.has_value());
    EXPECT_FALSE(result->max_participation.has_value());

    const auto panel = prices({100, 100, 110});
    const auto replay = book::replay_scheduled_targets(
        panel, days(3), decisions, targets, configuration());
    ASSERT_TRUE(replay.has_value()) << replay.error().message();
    auto malformed = *replay;
    malformed.intervals.pop_back();
    EXPECT_FALSE(diagnose_replay(malformed, panel).has_value());
    malformed = *replay;
    malformed.intervals[1].nav = std::numeric_limits<atx::f64>::quiet_NaN();
    EXPECT_FALSE(diagnose_replay(malformed, panel).has_value());
    malformed = *replay;
    malformed.trades[0].instrument = 1;
    EXPECT_FALSE(diagnose_replay(malformed, panel).has_value());
    malformed = *replay;
    malformed.intervals[1].trade_cost = -1.0;
    EXPECT_FALSE(diagnose_replay(malformed, panel).has_value());
}
