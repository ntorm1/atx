#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "stages.hpp"

namespace {

atx::core::Result<atx::impl::RunConfig> parse_replay(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) {
        argv.push_back(arg.data());
    }
    return atx::impl::parse_args(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST(ConfigReplay, AnnualAndTradeUnitsAreDistinctFromLegacyCharges) {
    auto parsed = parse_replay({"atx-impl", "report", "--report-aum", "1000000",
        "--replay-execution-delay", "2", "--replay-trade-bps", "10",
        "--replay-annual-borrow-bps", "365", "--replay-day-basis", "360"});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_DOUBLE_EQ(parsed->report_aum, 1000000.0);
    EXPECT_EQ(parsed->replay_execution_delay, 2U);
    EXPECT_DOUBLE_EQ(parsed->replay_trade_bps, 10.0);
    EXPECT_DOUBLE_EQ(parsed->replay_annual_borrow_bps, 365.0);
    EXPECT_EQ(parsed->replay_day_basis, 360);
    EXPECT_DOUBLE_EQ(parsed->cost_bps, 0.0);
    EXPECT_DOUBLE_EQ(parsed->borrow_bps, 0.0);
    EXPECT_TRUE(parsed->set_flags.contains("replay-trade-bps"));
    EXPECT_TRUE(parsed->set_flags.contains("replay-execution-delay"));
}

TEST(ConfigReplay, InvalidRatesTimingAndCalendarBasisReject) {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"replay-trade-bps", "nan"}, {"replay-trade-bps", "inf"},
        {"replay-trade-bps", "-1"}, {"replay-annual-borrow-bps", "-inf"},
        {"replay-annual-borrow-bps", "nan"}, {"replay-annual-borrow-bps", "-0.1"},
        {"replay-execution-delay", "-1"}, {"replay-execution-delay", "1.5"},
        {"replay-execution-delay", "18446744073709551616"},
        {"replay-day-basis", "252"}, {"replay-day-basis", "365.0"},
        {"report-aum", "inf"}, {"report-aum", "nan"}, {"report-aum", "0"}};
    for (const auto& [flag, value] : cases) {
        SCOPED_TRACE(flag + "=" + value);
        EXPECT_FALSE(parse_replay({"atx-impl", "report", "--" + flag, value}).has_value());
    }
}

TEST(ConfigReplay, ExplicitZeroRecordsHypotheticalSameCloseAndFrictionlessChoice) {
    const auto defaults = parse_replay({"atx-impl", "report"});
    ASSERT_TRUE(defaults.has_value());
    EXPECT_EQ(defaults->replay_execution_delay, 1U);
    EXPECT_EQ(defaults->replay_day_basis, 365);
    const auto explicit_zero = parse_replay({"atx-impl", "report",
        "--replay-execution-delay", "0", "--replay-trade-bps", "0",
        "--replay-annual-borrow-bps", "0", "--allow-same-close"}); // W0-I0b / B-02 opt-in
    ASSERT_TRUE(explicit_zero.has_value());
    EXPECT_EQ(explicit_zero->replay_execution_delay, 0U);
    EXPECT_TRUE(explicit_zero->set_flags.contains("replay-trade-bps"));
    EXPECT_TRUE(explicit_zero->set_flags.contains("replay-annual-borrow-bps"));
}

TEST(ConfigReplay, RunRejectsAmbiguousPlanningFeesBeforeAnyInputOrOutput) {
    for (const bool meta : {false, true}) {
        atx::impl::RunConfig cfg;
        cfg.zip = "input-must-not-be-opened.zip";
        cfg.out = "output-must-not-be-created";
        cfg.cost_bps = 5.0;
        cfg.metabook = meta;
        const auto result = atx::impl::run_all(cfg);
        ASSERT_FALSE(result.has_value());
        EXPECT_NE(result.error().message().find("explicit --replay-trade-bps"), std::string::npos);
    }
}
