#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/engine/alpha/bytecode.hpp"
#include "atx/engine/alpha/panel.hpp"
#include "atx/server/research.hpp"
#include "../src/research_dsl.hpp"

namespace atx::server::research_detail {
namespace {
const std::vector<std::string> fields{"close", "volume", "returns"};

TEST(ResearchDsl, ExpandedOperatorsCompileWithExactCumulativeLookback) {
    const auto program = compile_research_dsl(
        "-rank(ts_corr(returns,ts_mean(volume,10),20))", fields);
    EXPECT_EQ(program.required_lookback, 28);
    EXPECT_EQ(program.fields.size(), 2U);
    EXPECT_EQ(program.roots.size(), 1U);
}

TEST(ResearchDsl, CatalogExamplesAreExecutablePolicyAndHaveDescriptions) {
    const auto catalog = research_operator_catalog();
    EXPECT_GE(catalog.at("operators").size(), 30U);
    for (const auto& item : catalog.at("operators")) {
        SCOPED_TRACE(item.at("name").get<std::string>());
        EXPECT_FALSE(item.at("description").get<std::string>().empty());
        EXPECT_NO_THROW((void)compile_research_dsl(item.at("example").get<std::string>(), fields));
    }
    EXPECT_EQ(catalog.at("limits").at("max_window"), 252);
}

TEST(ResearchDsl, RejectsUnsafeWindowsFieldsScalarsAndUnsupportedState) {
    for (const auto* source : {"close > 1", "future_close", "ts_corr(close,volume,253)",
            "ts_mean(close,0)", "ts_mean(close,2.5)", "ts_mean(close,volume)",
            "ts_corr(close,volume,-1)", "scale(close,volume)", "winsorize(close,-1)",
            "hump(close)", "trade_when(close>1,close,close<1)",
            "delay(delay(delay(delay(close,252),252),252),252)", "1e300", "close("}) {
        SCOPED_TRACE(source);
        EXPECT_THROW((void)compile_research_dsl(source, fields), std::invalid_argument);
    }
}

TEST(ResearchDsl, EnforcesParserAndWorkingMemoryBoundsBeforeEvaluation) {
    EXPECT_THROW((void)compile_research_dsl(std::string(1025, 'x'), fields),
                 std::invalid_argument);
    EXPECT_THROW((void)compile_research_dsl(std::string(17, '(') + "close" +
                                          std::string(17, ')'), fields), std::invalid_argument);
    const auto panel = atx::engine::alpha::Panel::create(3, 2, {"close"},
                                                       {{1, 2, 2, 3, 4, 5}}, {});
    ASSERT_TRUE(panel);
    EXPECT_THROW((void)evaluate_research_signal("rank(close)", *panel, 1),
                 std::invalid_argument);
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_THROW((void)evaluate_research_signal("rank(close)", *panel, 1024 * 1024,
                                               cancel.get_token()), std::runtime_error);
}

TEST(ResearchDsl, TrailingEvaluationIsInvariantToFuturePriceChanges) {
    auto first = atx::engine::alpha::Panel::create(5, 2, {"close"},
        {{10, 20, 11, 19, 12, 18, 13, 17, 14, 16}}, {});
    auto second = atx::engine::alpha::Panel::create(5, 2, {"close"},
        {{10, 20, 11, 19, 12, 18, 1300, 1700, 1400, 1600}}, {});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto a = evaluate_research_signal("rank(ts_mean(close,2))", *first);
    const auto b = evaluate_research_signal("rank(ts_mean(close,2))", *second);
    for (std::size_t day = 1; day <= 2; ++day) {
        const auto left = a.alpha_cross_section(0, day);
        const auto right = b.alpha_cross_section(0, day);
        EXPECT_EQ(left[0], right[0]);
        EXPECT_EQ(left[1], right[1]);
    }
}

TEST(ResearchDsl, InteractiveValidationUsesExactCompilerAndPinsDatasetIdentity) {
    const ResearchBackend backend(ResearchOptions{});
    const auto result = nlohmann::json::parse(backend.validate_expression_json(
        "rank(ts_corr(returns,volume,20))"));
    EXPECT_TRUE(result.at("valid"));
    EXPECT_TRUE(result.at("synthetic"));
    EXPECT_EQ(result.at("required_lookback"), 19);
    EXPECT_EQ(result.at("fields").size(), 2U);
    EXPECT_EQ(result.at("expression_sha256").get<std::string>().size(), 64U);
    EXPECT_THROW((void)backend.validate_expression_json("rank(close)", "missing", "abc"),
                 std::invalid_argument);
    EXPECT_THROW((void)backend.validate_expression_json("rank(close)", "", "abc"),
                 std::invalid_argument);
    EXPECT_THROW((void)backend.validate_expression_json("rank(undeclared_field)"),
                 std::invalid_argument);
}
} // namespace
} // namespace atx::server::research_detail
