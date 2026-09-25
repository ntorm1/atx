#include <bit>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "atx/engine/alpha/panel.hpp"
#include "atx/engine/book/replay.hpp"
#include "atx/engine/book/report.hpp"

namespace atx_test_w0_replay_causal {
namespace book = atx::engine::book;
constexpr auto kNan = std::numeric_limits<double>::quiet_NaN();
constexpr atx::i64 kDay = 86'400'000'000'000LL;

auto replay(const std::vector<double> &close, double weight,
            const std::vector<book::DelistingEvent> &events = {},
            book::ListingExchange venue = book::ListingExchange::Unknown) {
    auto panel = atx::engine::alpha::Panel::create(close.size(), 1, {"close"}, {close}, {}).value();
    std::vector<atx::i64> sessions;
    for (atx::usize i = 0; i < close.size(); ++i) {
        sessions.push_back(static_cast<atx::i64>(i) * kDay);
    }
    book::ReplayConfig config;
    config.initial_nav = 1000;
    config.delistings = events;
    const std::vector<book::ListingExchange> venues{venue};
    config.listing_exchange = venues;
    const std::vector<atx::usize> decisions{0};
    const std::vector<double> targets{weight};
    return book::replay_scheduled_targets(panel, sessions, decisions, targets, config);
}

void identical_prefix(const book::ReplayResult &left, const book::ReplayResult &right) {
    ASSERT_GE(left.intervals.size(), 2U);
    ASSERT_GE(right.intervals.size(), 2U);
    for (atx::usize i = 0; i < 2; ++i) {
        const auto &a = left.intervals[i];
        const auto &b = right.intervals[i];
        const std::vector<double> av{a.pretrade_nav, a.cash, a.assets, a.nav, a.gross_pnl,
            a.trade_cost, a.borrow_cost, a.net_return, a.traded_dollars, a.start_gross, a.end_gross};
        const std::vector<double> bv{b.pretrade_nav, b.cash, b.assets, b.nav, b.gross_pnl,
            b.trade_cost, b.borrow_cost, b.net_return, b.traded_dollars, b.start_gross, b.end_gross};
        for (atx::usize j = 0; j < av.size(); ++j) {
            EXPECT_EQ(std::bit_cast<atx::u64>(av[j]), std::bit_cast<atx::u64>(bv[j]));
        }
    }
    ASSERT_EQ(left.delistings.size(), 1U);
    ASSERT_EQ(right.delistings.size(), 1U);
    EXPECT_EQ(left.delistings[0].period, right.delistings[0].period);
    EXPECT_DOUBLE_EQ(left.delistings[0].proceeds, right.delistings[0].proceeds);
}

TEST(BookReplayCausal, FuturePricesAndFutureEventsCannotAlterMissingClosePrefix) {
    for (const double weight : {0.4, -0.4}) {
        const auto prefix = replay({100, 100, kNan}, weight);
        const auto reappears = replay({100, 100, kNan, 200, 300}, weight);
        const auto vanishes = replay({100, 100, kNan, kNan, kNan}, weight);
        const auto future_event = replay({100, 100, kNan, 200, 300}, weight, {{0, 3, -0.9}});
        const auto mutated_event = replay({100, 100, kNan, kNan, kNan}, weight, {{0, 4, 0.5}});
        const auto unavailable_event = replay({100, 100, kNan, kNan, kNan}, weight,
                                              {{0, 1, -0.9, 3}});
        ASSERT_TRUE(prefix);
        ASSERT_TRUE(reappears);
        ASSERT_TRUE(vanishes);
        ASSERT_TRUE(future_event);
        ASSERT_TRUE(mutated_event);
        ASSERT_TRUE(unavailable_event);
        identical_prefix(*prefix, *reappears);
        identical_prefix(*prefix, *vanishes);
        identical_prefix(*prefix, *future_event);
        identical_prefix(*prefix, *mutated_event);
        identical_prefix(*prefix, *unavailable_event);
        EXPECT_EQ(prefix->flagged_delistings, 1U);
        EXPECT_TRUE(prefix->gap_carries.empty());
        EXPECT_DOUBLE_EQ(prefix->final_nav, weight > 0 ? 780.0 : 880.0);
        EXPECT_EQ(prefix->assumed_liquidations, 1U);
        EXPECT_LT(prefix->assumed_liquidation_pnl, 0.0);
        EXPECT_EQ(prefix->flagged_short_delistings, weight < 0 ? 1U : 0U);
    }
}

TEST(BookReplayCausal, SuppliedReturnOnlyAppliesWhenAvailableAtTheMissingValuation) {
    const auto known = replay({100, 100, kNan, kNan}, 0.4, {{0, 1, -0.9, 2}});
    const auto late = replay({100, 100, kNan, kNan}, 0.4, {{0, 1, -0.9, 3}});
    ASSERT_TRUE(known);
    ASSERT_TRUE(late);
    ASSERT_EQ(known->delistings.size(), 1U);
    ASSERT_EQ(late->delistings.size(), 1U);
    EXPECT_EQ(known->delistings[0].source, book::TerminalReturnSource::Table);
    EXPECT_DOUBLE_EQ(known->delistings[0].delist_return, -0.9);
    EXPECT_FALSE(known->delistings[0].flagged);
    EXPECT_EQ(late->delistings[0].source, book::TerminalReturnSource::AssumedMissingPriceAdverse);
    EXPECT_DOUBLE_EQ(late->delistings[0].delist_return, -0.55);
    EXPECT_TRUE(late->delistings[0].flagged);
    const auto unknown = replay({100, 100, kNan, kNan}, -0.4, {{0, 1, kNan, 2}});
    ASSERT_TRUE(unknown);
    EXPECT_DOUBLE_EQ(unknown->final_nav, 1120.0); // Due evidence permits a Shumway estimate.
    EXPECT_EQ(unknown->assumed_liquidations, 0U);
}

TEST(BookReplayCausal, UnevidencedMissingPriceStressIsAdverseForEveryVenueAndPosition) {
    for (const auto venue : {book::ListingExchange::NyseAmex, book::ListingExchange::Nasdaq,
                            book::ListingExchange::Unknown}) {
        for (const double weight : {0.4, -0.4}) {
            const auto result = replay({100, 100, kNan}, weight, {}, venue);
            ASSERT_TRUE(result);
            ASSERT_EQ(result->delistings.size(), 1U);
            const auto &event = result->delistings[0];
            EXPECT_EQ(event.source, book::TerminalReturnSource::AssumedMissingPriceAdverse);
            EXPECT_TRUE(event.flagged);
            EXPECT_LT(event.proceeds - event.last_value, 0.0);
            EXPECT_LT(result->final_nav, result->initial_nav);
        }
    }
}

TEST(BookReplayCausal, RepeatedMissingPricesAndShortReentryCannotManufactureAlpha) {
    const std::vector<double> close{100, 100, kNan, 100, kNan, 100, kNan};
    const auto panel = atx::engine::alpha::Panel::create(7, 1, {"close"}, {close}, {}).value();
    const std::vector<atx::i64> sessions{0, kDay, 2*kDay, 3*kDay, 4*kDay, 5*kDay, 6*kDay};
    const std::vector<atx::usize> decisions{0, 2, 4};
    const std::vector<double> targets{-0.4, -0.4, -0.4};
    book::ReplayConfig config;
    config.initial_nav = 1000;
    const auto result = book::replay_scheduled_targets(panel, sessions, decisions, targets, config);
    ASSERT_TRUE(result);
    ASSERT_EQ(result->delistings.size(), 3U);
    EXPECT_EQ(result->assumed_liquidations, 3U);
    EXPECT_LT(result->assumed_liquidation_pnl, 0.0);
    EXPECT_NEAR(result->final_nav, 681.472, 1e-10);
    for (const auto &event : result->delistings) {
        EXPECT_LT(event.proceeds - event.last_value, 0.0);
        EXPECT_EQ(event.source, book::TerminalReturnSource::AssumedMissingPriceAdverse);
    }
}

TEST(BookLegacyReportCausal, FuturePrintsCannotReclassifyAHoldingWindow) {
    const std::vector<atx::usize> periods{0, 2};
    const std::vector<std::vector<double>> books{{0.4}, {0.0}};
    const std::vector<double> returns_later{100, 110, kNan, 200, 300};
    const std::vector<double> never_returns{100, 110, kNan, kNan, kNan};
    const auto a = book::holding_interval_returns(returns_later, 5, 1, periods, books,
        book::LegacyReportRule::HoldingIntervalV3);
    const auto b = book::holding_interval_returns(never_returns, 5, 1, periods, books,
        book::LegacyReportRule::HoldingIntervalV3);
    ASSERT_TRUE(a);
    ASSERT_TRUE(b);
    EXPECT_EQ(std::bit_cast<atx::u64>(a->returns[0]), std::bit_cast<atx::u64>(b->returns[0]));
    EXPECT_NEAR(a->returns[0], -0.505, 1e-14);
    ASSERT_EQ(a->terminals.size(), 1U);
    EXPECT_EQ(a->terminals[0].last_valid_date, 1U);
    EXPECT_EQ(a->gap_marks, 0U);
    const auto legacy = book::holding_interval_returns(returns_later, 5, 1, periods, books,
        book::LegacyReportRule::HoldingIntervalV2);
    ASSERT_TRUE(legacy);
    EXPECT_NEAR(legacy->returns[0], 0.1, 1e-14);
    EXPECT_EQ(legacy->gap_marks, 1U);
}

TEST(BookLegacyReportCausal, ReappearanceInsideHoldingWindowDoesNotUndoFirstMissingFallback) {
    const std::vector<atx::usize> periods{0};
    const std::vector<std::vector<double>> books{{-0.4}};
    const std::vector<double> close{100, 110, kNan, 200, 300};
    const auto result = book::holding_interval_returns(close, 5, 1, periods, books,
        book::LegacyReportRule::HoldingIntervalV3);
    ASSERT_TRUE(result);
    EXPECT_NEAR(result->returns[0], 0.43, 1e-14); // Positive mark return debits the short.
    ASSERT_EQ(result->terminals.size(), 1U);
    EXPECT_EQ(result->terminals[0].last_valid_date, 1U);
}
} // namespace atx_test_w0_replay_causal
