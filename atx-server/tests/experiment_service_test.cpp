#include <atx/server/service.hpp>

#include <gtest/gtest.h>

#include <string>

namespace atx::server {
namespace {
struct ExperimentFixture {
    Store store{":memory:"};
    EventBus events;
    Scheduler scheduler{store, events,
        [](std::string_view, std::stop_token, const Progress&) { return std::string("{}"); },
        1, 32};
    WorkspaceService service{store, scheduler, events, [] { return "{}"; }};
};

TEST(ServerExperiments, AtomicBatchPersistsOrderedMembersDeduplicatesAndCancels) {
    ExperimentFixture fixture;
    v1::CreateExperimentRequest request;
    request.set_name("Decay sensitivity");
    request.set_idempotency_key("transport-experiment-once");
    request.set_scheduled_at_ms(unix_ms() + 86400000);
    for (const auto* label : {"Fast", "Slow"}) {
        auto* member = request.add_simulations();
        member->set_label(label);
        member->set_request_json(R"({"mode":"synthetic_demo","sessions":32})");
    }
    grpc::ServerContext context;
    v1::Experiment result;
    ASSERT_TRUE(fixture.service.CreateExperiment(&context, &request, &result).ok());
    const auto id = result.id();
    ASSERT_EQ(result.jobs_size(), 2);
    EXPECT_EQ(result.jobs(0).label(), "Fast");
    EXPECT_EQ(result.jobs(1).label(), "Slow");
    EXPECT_TRUE(result.jobs(0).request_json().empty());
    EXPECT_EQ(result.jobs(0).experiment_id(), id);
    const auto first_job = result.jobs(0).id();
    ASSERT_TRUE(fixture.service.CreateExperiment(&context, &request, &result).ok());
    EXPECT_EQ(result.id(), id);
    EXPECT_EQ(result.jobs(0).id(), first_job);
    request.set_name("Conflicting retry");
    EXPECT_EQ(fixture.service.CreateExperiment(&context, &request, &result).error_code(),
              grpc::StatusCode::ALREADY_EXISTS);
    v1::GetExperimentRequest get;
    get.set_id(id);
    ASSERT_TRUE(fixture.service.GetExperiment(&context, &get, &result).ok());
    EXPECT_EQ(result.queued(), 2U);
    v1::ListExperimentsRequest list;
    v1::ListExperimentsResponse page;
    ASSERT_TRUE(fixture.service.ListExperiments(&context, &list, &page).ok());
    ASSERT_EQ(page.experiments_size(), 1);
    EXPECT_EQ(page.experiments(0).jobs_size(), 0);
    v1::CancelExperimentRequest cancel;
    cancel.set_id(id);
    ASSERT_TRUE(fixture.service.CancelExperiment(&context, &cancel, &result).ok());
    EXPECT_EQ(result.status(), "cancelled");
    EXPECT_EQ(result.cancelled(), 2U);
}

TEST(ServerExperiments, SavedRevisionSubmissionRetainsNativeRecipeAndLineage) {
    ExperimentFixture fixture;
    AlphaInput alpha;
    alpha.name = "Saved reversal";
    alpha.request_json = R"({"mode":"synthetic_demo","sessions":32})";
    const auto saved = fixture.store.create_alpha(alpha);
    v1::SubmitJobRequest request;
    request.set_alpha_id(saved.id);
    request.set_alpha_revision(saved.revision);
    request.set_label("Run saved revision");
    request.set_scheduled_at_ms(unix_ms() + 86400000);
    grpc::ServerContext context;
    v1::Job job;
    const auto status = fixture.service.SubmitJob(&context, &request, &job);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(job.alpha_id(), saved.id);
    EXPECT_EQ(job.alpha_revision(), saved.revision);
    EXPECT_EQ(job.request_json(), saved.request_json);
    EXPECT_EQ(job.label(), "Run saved revision");
}

TEST(ServerExperiments, AllBatchMethodsAuthorizeBeforeLookingUpState) {
    ExperimentFixture fixture;
    WorkspaceService secured(fixture.store, fixture.scheduler, fixture.events,
        [] { return "{}"; }, {std::string(32, 'x'), 2});
    grpc::ServerContext context;
    v1::CreateExperimentRequest create;
    v1::GetExperimentRequest get;
    v1::ListExperimentsRequest list;
    v1::CancelExperimentRequest cancel;
    v1::Experiment experiment;
    v1::ListExperimentsResponse page;
    EXPECT_EQ(secured.CreateExperiment(&context, &create, &experiment).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(secured.GetExperiment(&context, &get, &experiment).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(secured.ListExperiments(&context, &list, &page).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(secured.CancelExperiment(&context, &cancel, &experiment).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
}
} // namespace
} // namespace atx::server
