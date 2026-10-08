#include <atx/server/service.hpp>

#include <gtest/gtest.h>

#include <string>

namespace atx::server {
namespace {
struct WorkflowFixture {
    Store store{":memory:"};
    EventBus events;
    Scheduler scheduler{store, events,
        [](std::string_view, std::stop_token, const Progress&) { return std::string("{}"); },
        1, 32};
    WorkspaceService service{store, scheduler, events, [] { return "{}"; }};
};

TEST(ServerWorkflows, EveryRecurringMethodAuthenticatesBeforeLookingUpState) {
    WorkflowFixture fixture;
    WorkspaceService secured(fixture.store, fixture.scheduler, fixture.events,
        [] { return "{}"; }, {std::string(32, 'x'), 2});
    grpc::ServerContext context;
    v1::CreateWorkflowRequest create;
    v1::GetWorkflowRequest get;
    v1::ListWorkflowsRequest list;
    v1::SetWorkflowPausedRequest pause;
    v1::ListWorkflowRunsRequest runs;
    v1::Workflow record;
    v1::ListWorkflowsResponse page;
    v1::ListWorkflowRunsResponse jobs;
    EXPECT_EQ(secured.CreateWorkflow(&context, &create, &record).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(secured.GetWorkflow(&context, &get, &record).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(secured.ListWorkflows(&context, &list, &page).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(secured.SetWorkflowPaused(&context, &pause, &record).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(secured.ListWorkflowRuns(&context, &runs, &jobs).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
}

TEST(ServerWorkflows, FrozenRecipeLifecycleAndPagingPreserveExactNativeState) {
    WorkflowFixture fixture;
    grpc::ServerContext context;
    v1::CreateWorkflowRequest create;
    create.set_name("Recurring research");
    create.set_description("Frozen TRAIN recipe");
    create.set_request_json(R"({"mode":"synthetic_demo","sessions":32})");
    create.set_interval_ms(60000);
    create.set_max_runs(3);
    create.set_first_run_at_ms(unix_ms() + 86400000);
    create.set_idempotency_key("transport-workflow-once");
    v1::Workflow result;
    const auto admitted = fixture.service.CreateWorkflow(&context, &create, &result);
    ASSERT_TRUE(admitted.ok()) << admitted.error_message();
    const auto id = result.id();
    EXPECT_EQ(result.actor_id(), "local");
    EXPECT_EQ(result.interval_ms(), 60000U);
    EXPECT_EQ(result.max_runs(), 3U);
    EXPECT_EQ(result.request_json(), create.request_json());
    EXPECT_EQ(result.request_sha256().size(), 64U);
    EXPECT_EQ(result.revision(), 1U);
    ASSERT_TRUE(result.enabled());
    ASSERT_TRUE(fixture.service.CreateWorkflow(&context, &create, &result).ok());
    EXPECT_EQ(result.id(), id);
    v1::SetWorkflowPausedRequest pause;
    pause.set_id(id); pause.set_expected_revision(1); pause.set_paused(true);
    ASSERT_TRUE(fixture.service.SetWorkflowPaused(&context, &pause, &result).ok());
    EXPECT_FALSE(result.enabled());
    EXPECT_EQ(result.revision(), 2U);
    EXPECT_EQ(fixture.service.SetWorkflowPaused(&context, &pause, &result).error_code(),
              grpc::StatusCode::ALREADY_EXISTS);
    v1::GetWorkflowRequest get;
    get.set_id(id);
    ASSERT_TRUE(fixture.service.GetWorkflow(&context, &get, &result).ok());
    EXPECT_EQ(result.state(), "paused");
    v1::ListWorkflowsRequest list;
    v1::ListWorkflowsResponse page;
    ASSERT_TRUE(fixture.service.ListWorkflows(&context, &list, &page).ok());
    ASSERT_EQ(page.workflows_size(), 1);
    EXPECT_EQ(page.total(), 1U);
    EXPECT_TRUE(page.workflows(0).request_json().empty());
    v1::ListWorkflowRunsRequest runs;
    runs.set_id(id);
    v1::ListWorkflowRunsResponse jobs;
    ASSERT_TRUE(fixture.service.ListWorkflowRuns(&context, &runs, &jobs).ok());
    EXPECT_EQ(jobs.total(), 0U);
}
} // namespace
} // namespace atx::server
