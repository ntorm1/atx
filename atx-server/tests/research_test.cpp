#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <stop_token>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "atx/server/research.hpp"

namespace atx::server {
namespace {
using Json = nlohmann::json;

class ResearchTest : public ::testing::Test {
protected:
  void SetUp() override {
    static std::atomic<unsigned> sequence{};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    root_ = std::filesystem::temp_directory_path() /
        ("atx-research-test-" + std::to_string(tick) + "-" +
         std::to_string(sequence.fetch_add(1)));
    std::filesystem::create_directories(root_);
  }
  void TearDown() override { std::filesystem::remove_all(root_); }

  void write(std::string_view name, std::string_view content) const {
    std::ofstream output(root_ / name, std::ios::binary);
    output << content;
    ASSERT_TRUE(output.good());
  }
  ResearchBackend backend(std::size_t points = 2000) const {
    return ResearchBackend({root_, {root_}, 1024U * 1024U, points, 1000});
  }
  Json import_result(const Json& extra = Json::object()) const {
    Json request{{"mode", "import_nav"}, {"artifact_path", "daily.csv"}};
    request.update(extra);
    return Json::parse(backend().execute(request.dump(), {}, {}));
  }
  static std::string daily() {
    return "session_ns,return_observation,pretrade_nav,posttrade_nav,net_return\n"
           "1577836800000000000,0,100,99,0\n"
           "1577923200000000000,1,110,109,0.1\n"
           "1578009600000000000,1,99,98,-0.1\n";
  }
  std::filesystem::path root_;
};

TEST_F(ResearchTest, RuntimeBuildPinIsValidatedAndCarriedIntoEvidence) {
  ResearchOptions options;
  options.repo_root = root_;
  options.artifact_roots = {root_};
  options.runtime_build_sha256 = "not-a-build-pin";
  EXPECT_THROW(static_cast<void>(ResearchBackend(options)), std::invalid_argument);
  options.runtime_build_sha256 = std::string(64, 'a');
  write("daily.csv", daily());
  const auto backend = ResearchBackend(options);
  const auto result = Json::parse(backend.execute(
      R"({"mode":"import_nav","artifact_path":"daily.csv"})", {}, {}));
  ASSERT_TRUE(result.at("provenance").contains("runtime_build_sha256"));
  EXPECT_EQ(result.at("provenance").at("runtime_build_sha256"), std::string(64, 'a'));
}

TEST_F(ResearchTest, ImportedNavMetricsUsePretradeValuationsAndSampleVolatility) {
  write("daily.csv", daily());
  const auto result = import_result();
  const auto& metrics = result.at("metrics");
  EXPECT_NEAR(metrics.at("total_return").get<double>(), -0.01, 1e-12);
  EXPECT_NEAR(metrics.at("annualized_volatility").get<double>(),
              std::sqrt(0.02 * 252.0), 1e-12);
  EXPECT_NEAR(metrics.at("sharpe_ratio").get<double>(), 0.0, 1e-12);
  EXPECT_NEAR(metrics.at("max_drawdown").get<double>(), 0.1, 1e-12);
  EXPECT_EQ(metrics.at("observations"), 2);
  EXPECT_EQ(result.at("equity_curve").back().at("value"), 99.0);
  EXPECT_FALSE(result.at("provenance").at("synthetic").get<bool>());
  EXPECT_EQ(result.at("provenance").at("artifact_sha256").get<std::string>().size(), 64U);
  EXPECT_EQ(result.at("monthly_returns").front().at("month"), "2020-01");
  EXPECT_NEAR(result.at("monthly_returns").front().at("value").get<double>(), -0.01, 1e-12);
}

TEST_F(ResearchTest, DateSelectionExcludesReturnPrecedingWindowBaseline) {
  write("daily.csv", daily());
  const auto result = import_result({{"start_date", "2020-01-02"}, {"end_date", "2020-01-03"}});
  EXPECT_EQ(result.at("metrics").at("observations"), 1);
  EXPECT_NEAR(result.at("metrics").at("total_return").get<double>(), -0.1, 1e-12);
  EXPECT_TRUE(result.at("metrics").at("sharpe_ratio").is_null());
  EXPECT_EQ(result.at("equity_curve").front().at("date"), "2020-01-02");
}

TEST_F(ResearchTest, TraversalAbsoluteAndHoldoutPathsAreRejected) {
  const auto engine = backend();
  for (const auto& path : {"../daily.csv", "validation.csv", "daily_VAL.csv", "daily_2024.csv"}) {
    const auto request = Json{{"mode", "import_nav"}, {"artifact_path", path}}.dump();
    EXPECT_THROW((void)engine.execute(request, {}, {}), std::invalid_argument);
  }
}

TEST_F(ResearchTest, HoldoutDatesCannotBeHiddenBehindInnocentFilename) {
  write("daily.csv", "session_ns,return_observation,posttrade_nav,net_return\n"
                     "1672531200000000000,0,100,0\n");
  EXPECT_THROW((void)import_result(), std::invalid_argument);
}

TEST_F(ResearchTest, MalformedNumericAndNonmonotonicEvidenceRejected) {
  write("daily.csv", "session_ns,return_observation,posttrade_nav,net_return\n"
                     "1577836800000000000,0,100,nan\n");
  EXPECT_THROW((void)import_result(), std::invalid_argument);
  write("daily.csv", daily() + "1577923200000000000,1,110,109,0.1\n");
  EXPECT_THROW((void)import_result(), std::invalid_argument);
}

TEST_F(ResearchTest, ZeroVarianceProducesNullRatios) {
  write("daily.csv", "session_ns,return_observation,posttrade_nav,net_return\n"
                     "1577836800000000000,0,100,0\n"
                     "1577923200000000000,1,100,0\n"
                     "1578009600000000000,1,100,0\n");
  const auto result = import_result();
  EXPECT_TRUE(result.at("metrics").at("sharpe_ratio").is_null());
  EXPECT_TRUE(result.at("metrics").at("sortino_ratio").is_null());
  EXPECT_TRUE(result.at("factor_exposures").empty());
}

TEST_F(ResearchTest, DistinctSubnormalReturnsProduceFiniteHistogramAndExactCounts) {
  write("daily.csv", "session_ns,return_observation,posttrade_nav,net_return\n"
                     "1577836800000000000,0,100,0\n"
                     "1577923200000000000,1,100,1e-323\n"
                     "1578009600000000000,1,100,2e-323\n");
  const auto result = import_result();
  const auto& bins = result.at("returns_histogram");
  ASSERT_FALSE(bins.empty());
  std::size_t count = 0;
  for (const auto& bin : bins) {
    EXPECT_TRUE(std::isfinite(bin.at("lower").get<double>()));
    EXPECT_TRUE(std::isfinite(bin.at("upper").get<double>()));
    count += bin.at("count").get<std::size_t>();
  }
  EXPECT_EQ(count, 2U);
  EXPECT_EQ(bins.front().at("count"), 1);
  EXPECT_EQ(bins.back().at("count"), 1);
}

TEST_F(ResearchTest, InconsistentPretradeNavAndReturnsRejectedAsInvalidEvidence) {
  write("daily.csv", "session_ns,return_observation,pretrade_nav,net_return\n"
                     "1577836800000000000,0,100,0\n"
                     "1577923200000000000,1,110,0.1\n"
                     "1578009600000000000,1,99,0.1\n");
  try {
    (void)import_result();
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string(error.what()).find("NAV returns do not reconcile"), std::string::npos);
    return;
  }
  FAIL() << "Expected rejection of conflicting NAV and return evidence";
}

TEST_F(ResearchTest, StopRequestedBeforeExecutionThrowsWithoutPartialResult) {
  std::stop_source source;
  source.request_stop();
  EXPECT_THROW((void)backend().execute("{\"mode\":\"synthetic_demo\"}",
                                     source.get_token(), {}), std::runtime_error);
}

TEST_F(ResearchTest, DemoUsesRealReplayAndIsDeterministicExplicitSyntheticEvidence) {
  const auto engine = backend(30);
  const std::string request = "{\"mode\":\"synthetic_demo\",\"sessions\":100}";
  const auto first = engine.execute(request, {}, {});
  EXPECT_EQ(first, engine.execute(request, {}, {}));
  const auto result = Json::parse(first);
  EXPECT_TRUE(result.at("provenance").at("synthetic").get<bool>());
  EXPECT_EQ(result.at("provenance").at("engine"), "atx::engine::book::replay_scheduled_targets");
  EXPECT_LE(result.at("equity_curve").size(), 30U);
  EXPECT_EQ(result.at("metrics").at("observations"), 99);
  EXPECT_FALSE(result.at("trades").empty());
}

TEST_F(ResearchTest, InvalidRequestAndImpossibleDatesAreRejected) {
  for (const auto& request : {"{", "[]", "{\"mode\":\"shell\"}",
       "{\"mode\":\"synthetic_demo\",\"sessions\":1000000000}",
       "{\"mode\":\"import_nav\",\"artifact_path\":\"daily.csv\","
       "\"start_date\":\"2020-02-31\"}"}) {
    EXPECT_THROW((void)backend().execute(request, {}, {}), std::invalid_argument);
  }
}

TEST_F(ResearchTest, CustomDslChangesRealEngineAllocationsAndRecordsExactExpression) {
  const auto engine = backend();
  const Json request{{"mode", "synthetic_demo"}, {"sessions", 80},
                     {"strategy_dsl", "rank(delta(close,5))"}};
  const auto positive = Json::parse(engine.execute(request.dump(), {}, {}));
  auto opposite_request = request;
  opposite_request["strategy_dsl"] = "-rank(delta(close,5))";
  const auto negative = Json::parse(engine.execute(opposite_request.dump(), {}, {}));
  EXPECT_EQ(positive.at("provenance").at("strategy_dsl"), "rank(delta(close,5))");
  EXPECT_NE(positive.at("trades"), negative.at("trades"));
  EXPECT_NE(positive.at("metrics").at("final_nav"), negative.at("metrics").at("final_nav"));
}

TEST_F(ResearchTest, UnsafeDynamicOrHugeWindowDslRejected) {
  for (const auto& dsl : {"ts_mean(close,close)", "ts_mean(close,1e30)",
                          "rank(delta(close,-1))", "unknown(close)"}) {
    const Json request{{"mode", "synthetic_demo"}, {"strategy_dsl", dsl}};
    EXPECT_THROW((void)backend().execute(request.dump(), {}, {}), std::invalid_argument);
  }
}

TEST_F(ResearchTest, ArtifactByteAndRowLimitsAreEnforcedBeforeResults) {
  write("daily.csv", daily());
  const auto request = Json{{"mode", "import_nav"}, {"artifact_path", "daily.csv"}}.dump();
  const ResearchBackend bytes({root_, {root_}, 128, 100, 1000});
  EXPECT_THROW((void)bytes.execute(request, {}, {}), std::invalid_argument);
  const ResearchBackend rows({root_, {root_}, 1024U * 1024U, 100, 2});
  EXPECT_THROW((void)rows.execute(request, {}, {}), std::invalid_argument);
}

TEST_F(ResearchTest, CancellationFromProgressStopsExecution) {
  std::stop_source source;
  bool called = false;
  const auto callback = [&](double, std::string_view) {
    called = true;
    source.request_stop();
  };
  EXPECT_THROW((void)backend().execute("{\"mode\":\"synthetic_demo\"}",
                                     source.get_token(), callback), std::runtime_error);
  EXPECT_TRUE(called);
}

TEST_F(ResearchTest, SummaryIsHashedAsMetadataWithoutReplacingComputedMetrics) {
  write("daily.csv", daily());
  write("summary.json", "{\"schema\":\"atx.nav-summary/v1\",\"sharpe\":999}");
  const auto result = import_result({{"summary_path", "summary.json"}});
  EXPECT_EQ(result.at("provenance").at("summary_sha256").get<std::string>().size(), 64U);
  EXPECT_NEAR(result.at("metrics").at("sharpe_ratio").get<double>(), 0.0, 1e-12);
  EXPECT_EQ(result.at("provenance").at("evaluation"), "research_unverified");
}

TEST_F(ResearchTest, UnknownParametersAndDuplicateKeysAreRejected) {
  EXPECT_THROW((void)backend().execute("{\"mode\":\"synthetic_demo\",\"command\":\"echo\"}",
                                     {}, {}), std::invalid_argument);
  EXPECT_THROW((void)backend().execute("{\"mode\":\"synthetic_demo\",\"mode\":\"import_nav\"}",
                                     {}, {}), std::invalid_argument);
}
} // namespace
} // namespace atx::server
