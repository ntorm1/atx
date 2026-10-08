#include "atx/server/analysis.hpp"

#include <array>
#include <cmath>
#include <stdexcept>
#include <stop_token>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace atx::server {
namespace {
using Json = nlohmann::json;

ComparisonInput evidence(std::string id, const std::array<double, 4>& returns,
                         bool synthetic = false) {
    Json rows = Json::array();
    for (std::size_t i = 0; i < returns.size(); ++i) {
        rows.push_back({{"date", "2020-01-0" + std::to_string(i + 2)},
            {"period_start_date", "2020-01-0" + std::to_string(i + 1)},
            {"value", returns[i]}});
    }
    return {id, id, Json{{"schema_version", "atx.research-result/v1"},
        {"daily_returns", rows}, {"provenance", {{"synthetic", synthetic},
        {"daily_returns_complete", true}, {"evaluation", "TRAIN"}}}}.dump()};
}

TEST(ResearchComparison, ExactAlignedReturnsDetermineCorrelationAndGrowth) {
    const std::array inputs{evidence("a", {0.01, -0.01, 0.02, -0.02}),
                            evidence("b", {-0.01, 0.01, -0.02, 0.02})};
    const auto result = Json::parse(compare_research_results(inputs));
    EXPECT_EQ(result.at("observations"), 4);
    EXPECT_NEAR(result.at("correlation")[0][1].get<double>(), -1, 1e-12);
    EXPECT_NEAR(result.at("correlation")[0][0].get<double>(), 1, 1e-12);
    EXPECT_NEAR(result.at("runs")[0].at("metrics").at("total_return").get<double>(),
                1.01 * 0.99 * 1.02 * 0.98 - 1, 1e-12);
    EXPECT_EQ(result.at("alignment"), "intersection_of_exact_return_periods");
}

TEST(ResearchComparison, PublishedManifestRevisionsArePreservedAndDifferencesReported) {
    auto a = evidence("a", {0.01, -0.01, 0.02, -0.02});
    auto b = evidence("b", {-0.01, 0.01, -0.02, 0.02});
    auto first = Json::parse(a.result_json);
    auto second = Json::parse(b.result_json);
    first["provenance"]["dataset_manifest_sha256"] = std::string(64, 'a');
    second["provenance"]["dataset_manifest_sha256"] = std::string(64, 'b');
    a.result_json = first.dump();
    b.result_json = second.dump();
    const auto result = Json::parse(compare_research_results(std::array{a, b}));
    EXPECT_EQ(result.at("runs")[0].at("dataset_sha256"), std::string(64, 'a'));
    EXPECT_EQ(result.at("runs")[1].at("dataset_sha256"), std::string(64, 'b'));
    const auto warnings = result.at("warnings").dump();
    EXPECT_NE(warnings.find("dataset revisions"), std::string::npos);
    first["provenance"]["dataset_sha256"] = std::string(64, 'b');
    a.result_json = first.dump();
    EXPECT_THROW((void)compare_research_results(std::array{a, b}), std::invalid_argument);
    first["provenance"].erase("dataset_sha256");
    first["provenance"]["dataset_manifest_sha256"] = "not-a-digest";
    a.result_json = first.dump();
    EXPECT_THROW((void)compare_research_results(std::array{a, b}), std::invalid_argument);
}

TEST(ResearchComparison, PeriodStartMustMatchAndNoMissingReturnIsZeroFilled) {
    auto a = evidence("a", {0.01, 0.02, 0.03, 0.04});
    auto b = evidence("b", {0.01, 0.02, 0.03, 0.04});
    auto document = Json::parse(b.result_json);
    document["daily_returns"][0]["period_start_date"] = "2019-12-31";
    b.result_json = document.dump();
    const std::array inputs{a, b};
    const auto result = Json::parse(compare_research_results(inputs));
    EXPECT_EQ(result.at("observations"), 3);
    EXPECT_EQ(result.at("runs")[0].at("excluded_observations"), 1);
    EXPECT_EQ(result.at("runs")[1].at("excluded_observations"), 1);
}

TEST(ResearchComparison, ChartsMayBeSampledWithoutChangingMoments) {
    const std::array inputs{evidence("a", {0.01, -0.01, 0.02, -0.02}),
                            evidence("b", {0.02, 0.03, 0.04, 0.05})};
    ComparisonOptions options;
    options.max_chart_points = 2;
    const auto result = Json::parse(compare_research_results(inputs, options));
    EXPECT_EQ(result.at("observations"), 4);
    EXPECT_EQ(result.at("runs")[0].at("curve").size(), 2U);
    EXPECT_EQ(result.at("runs")[0].at("curve").back().at("date"), "2020-01-05");
    EXPECT_TRUE(result.at("chart_downsampled"));
}

TEST(ResearchComparison, ConstantVarianceProducesNullCorrelationAndSharpe) {
    const std::array inputs{evidence("a", {0, 0, 0, 0}), evidence("b", {0.01, 0, 0.02, 0})};
    const auto result = Json::parse(compare_research_results(inputs));
    EXPECT_TRUE(result.at("correlation")[0][0].is_null());
    EXPECT_TRUE(result.at("correlation")[0][1].is_null());
    EXPECT_TRUE(result.at("runs")[0].at("metrics").at("sharpe_ratio").is_null());
}

TEST(ResearchComparison, MixedEvidenceRequiresExplicitOptInAndRemainsMarked) {
    const std::array inputs{evidence("a", {0.01, -0.01, 0.02, -0.02}),
                            evidence("b", {0.02, 0.03, 0.04, 0.05}, true)};
    EXPECT_THROW((void)compare_research_results(inputs), std::invalid_argument);
    ComparisonOptions options;
    options.allow_mixed_evidence = true;
    const auto result = Json::parse(compare_research_results(inputs, options));
    EXPECT_TRUE(result.at("mixed_evidence"));
    EXPECT_TRUE(result.at("runs")[1].at("synthetic"));
}

TEST(ResearchComparison, RefusesLegacySampledCurvesMalformedReturnsAndDuplicateRuns) {
    auto a = evidence("a", {0.01, -0.01, 0.02, -0.02});
    const std::array duplicates{a, a};
    EXPECT_THROW((void)compare_research_results(duplicates), std::invalid_argument);
    auto b = evidence("b", {0.02, 0.03, 0.04, 0.05});
    auto document = Json::parse(b.result_json);
    document.erase("daily_returns");
    b.result_json = document.dump();
    EXPECT_THROW((void)compare_research_results(std::array{a, b}), std::invalid_argument);
    document["daily_returns"] = Json::parse(a.result_json).at("daily_returns");
    document["daily_returns"][0]["value"] = -1;
    b.result_json = document.dump();
    EXPECT_THROW((void)compare_research_results(std::array{a, b}), std::invalid_argument);
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_THROW((void)compare_research_results(std::array{a, b}, {}, cancel.get_token()),
                 std::runtime_error);
    ComparisonOptions options;
    options.is_cancelled = [] { return true; };
    EXPECT_THROW((void)compare_research_results(std::array{a, b}, options), std::runtime_error);
}
} // namespace
} // namespace atx::server
