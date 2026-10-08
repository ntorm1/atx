#include <atx/server/service.hpp>
#include <atx/server/research.hpp>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>

namespace atx::server {
namespace {
using Json = nlohmann::json;

struct AnalysisFixture {
    Store store{":memory:"};
    EventBus events;
    Scheduler scheduler{store, events,
        [](std::string_view, std::stop_token, const Progress&) { return std::string("{}"); },
        1, 16, false};
    WorkspaceService service{store, scheduler, events, [] { return "{}"; }};

    std::string completed(double direction) {
        const auto job = store.create("{}", "", 0, 16);
        const auto claimed = store.claim_due();
        if (!claimed || claimed->id != job.id) throw std::runtime_error("fixture claim failed");
        Json rows = Json::array();
        for (int i = 0; i < 4; ++i) {
            rows.push_back({{"date", "2020-01-0" + std::to_string(i + 2)},
                {"period_start_date", "2020-01-0" + std::to_string(i + 1)},
                {"value", direction * static_cast<double>(i + 1) * 0.001}});
        }
        const auto result = Json{{"schema_version", "atx.research-result/v1"},
            {"daily_returns", rows}, {"provenance", {{"synthetic", true},
            {"daily_returns_complete", true}, {"evaluation", "synthetic_test"}}}}.dump();
        store.finish(job.id, "SUCCEEDED", result, "");
        return job.id;
    }
};

TEST(ServerAnalysis, ComparisonFetchesCompletedEvidenceAndMapsInvalidRequests) {
    AnalysisFixture fixture;
    v1::CompareJobsRequest request;
    request.add_job_ids(fixture.completed(1));
    request.add_job_ids(fixture.completed(-1));
    v1::CompareJobsResponse response;
    grpc::ServerContext context;
    const auto status = fixture.service.CompareJobs(&context, &request, &response);
    ASSERT_TRUE(status.ok()) << status.error_message();
    const auto result = Json::parse(response.comparison_json());
    EXPECT_EQ(result.at("observations"), 4);
    EXPECT_NEAR(result.at("correlation")[0][1].get<double>(), -1, 1e-12);
    request.add_job_ids(request.job_ids(0));
    EXPECT_EQ(fixture.service.CompareJobs(&context, &request, &response).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(ServerAnalysis, QueuedOrMissingRunsCannotBeTreatedAsEvidence) {
    AnalysisFixture fixture;
    v1::CompareJobsRequest request;
    request.add_job_ids(fixture.completed(1));
    request.add_job_ids(fixture.store.create("{}", "", 0, 16).id);
    v1::CompareJobsResponse response;
    grpc::ServerContext context;
    EXPECT_EQ(fixture.service.CompareJobs(&context, &request, &response).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
    request.set_job_ids(1, std::string(32, '0'));
    EXPECT_EQ(fixture.service.CompareJobs(&context, &request, &response).error_code(),
              grpc::StatusCode::NOT_FOUND);
}

TEST(ServerAnalysis, AuthenticationPrecedesComparisonAndExpressionValidation) {
    AnalysisFixture fixture;
    WorkspaceService secured(fixture.store, fixture.scheduler, fixture.events,
        [] { return "{}"; }, {std::string(32, 'x'), 2});
    grpc::ServerContext context;
    v1::CompareJobsRequest compare;
    v1::CompareJobsResponse comparison;
    EXPECT_EQ(secured.CompareJobs(&context, &compare, &comparison).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    v1::ValidateExpressionRequest validate;
    v1::ValidateExpressionResponse analysis;
    EXPECT_EQ(secured.ValidateExpression(&context, &validate, &analysis).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
}

TEST(ServerAnalysis, ExpressionValidationUsesBackendAndReturnsActionableCompilerErrors) {
    AnalysisFixture fixture;
    const ResearchBackend backend(ResearchOptions{});
    WorkspaceService service(fixture.store, fixture.scheduler, fixture.events,
        [] { return "{}"; }, {}, nullptr, &backend);
    grpc::ServerContext context;
    v1::ValidateExpressionRequest request;
    request.set_strategy_dsl("rank(delta(close,5))");
    v1::ValidateExpressionResponse response;
    ASSERT_TRUE(service.ValidateExpression(&context, &request, &response).ok());
    EXPECT_EQ(Json::parse(response.analysis_json()).at("required_lookback"), 5);
    request.set_strategy_dsl("ts_mean(close,-1)");
    const auto invalid = service.ValidateExpression(&context, &request, &response);
    EXPECT_EQ(invalid.error_code(), grpc::StatusCode::INVALID_ARGUMENT);
    EXPECT_NE(invalid.error_message().find("literal"), std::string::npos);
    EXPECT_EQ(fixture.service.ValidateExpression(&context, &request, &response).error_code(),
              grpc::StatusCode::FAILED_PRECONDITION);
}
} // namespace
} // namespace atx::server
