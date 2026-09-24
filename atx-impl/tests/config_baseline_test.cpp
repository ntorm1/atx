#include <string>
#include <sstream>
#include <vector>

#include <gtest/gtest.h>

#include "config.hpp"
#include "dispatch.hpp"

namespace {

atx::core::Result<atx::impl::RunConfig> parse_baseline(std::vector<std::string> args) {
    std::vector<char*> argv;
    argv.reserve(args.size());
    for (auto& arg : args) argv.push_back(arg.data());
    return atx::impl::parse_args(static_cast<int>(argv.size()), argv.data());
}

} // namespace

TEST(ConfigBaseline, EvaluationBoundsRemainDistinctFromContextWindow) {
    const auto result = parse_baseline({"atx-impl", "equity-baseline",
        "--panel", "context.bin", "--out", "book", "--evaluation-start", "2013-04-04",
        "--evaluation-end", "2014-01-01", "--max-working-bytes", "3000000000"});
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->subcommand, "equity-baseline");
    EXPECT_EQ(result->equity_evaluation_start, "2013-04-04");
    EXPECT_EQ(result->equity_evaluation_end, "2014-01-01");
    EXPECT_EQ(result->equity_max_working_bytes, 3'000'000'000ULL);
    EXPECT_TRUE(result->start.empty());
    EXPECT_TRUE(result->end.empty());
    EXPECT_TRUE(result->set_flags.contains("evaluation-start"));
    EXPECT_TRUE(result->set_flags.contains("max-working-bytes"));
}

TEST(ConfigBaseline, MalformedOrMissingBudgetNeverCoercesToAnUnboundedRun) {
    for (const std::string value : {"0", "-1", "3e9", "1.5", "+10",
                                    "18446744073709551616", "nan"}) {
        SCOPED_TRACE(value);
        EXPECT_FALSE(parse_baseline({"atx-impl", "equity-baseline",
            "--max-working-bytes", value}).has_value());
    }
    EXPECT_FALSE(parse_baseline({"atx-impl", "equity-baseline",
        "--max-working-bytes"}).has_value());
    EXPECT_FALSE(parse_baseline({"atx-impl", "equity-baseline",
        "--evaluation-start", "--evaluation-end", "2014-01-01"}).has_value());
}

TEST(ConfigBaseline, ExplicitZeroFeeScenarioKeepsItsPresence) {
    const auto result = parse_baseline({"atx-impl", "equity-baseline",
        "--report-aum", "10000000", "--replay-trade-bps", "0",
        "--replay-annual-borrow-bps", "0"});
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->report_aum, 10'000'000.0);
    EXPECT_DOUBLE_EQ(result->replay_trade_bps, 0.0);
    EXPECT_DOUBLE_EQ(result->replay_annual_borrow_bps, 0.0);
    EXPECT_TRUE(result->set_flags.contains("replay-trade-bps"));
    EXPECT_TRUE(result->set_flags.contains("replay-annual-borrow-bps"));
    EXPECT_EQ(result->equity_max_working_bytes, 3'000'000'000ULL);
}

TEST(ConfigBaseline, DispatchAppliesConfigFileBeforeValidatingFixedRecipe) {
    std::vector<std::string> args{"atx-impl", "equity-baseline", "--config",
        std::string(ATX_IMPL_TESTS_DIR) + "/fixtures/equity_baseline_rejected_override.cfg"};
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    std::ostringstream out, error;
    EXPECT_EQ(atx::impl::dispatch(static_cast<int>(argv.size()), argv.data(), out, error), 1);
    EXPECT_NE(error.str().find("unsupported flag --gross"), std::string::npos);
}
