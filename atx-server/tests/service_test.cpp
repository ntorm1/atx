#include <atx/server/service.hpp>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <sqlite3.h>

#include <chrono>
#include <future>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <system_error>

namespace atx::server {
namespace {
// SAFETY: these transport tests intentionally exercise a real ephemeral loopback listener.
class GrpcFixture final {
public:
    Store store{":memory:"};
    EventBus events{16};
    Scheduler scheduler{store, events,
        [](std::string_view, std::stop_token, const Progress&) { return std::string("{}"); }};
    const std::string token = std::string(32, 'a');
    WorkspaceService service{store, scheduler, events, [] { return "[]"; }, {token, 1}};
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<v1::WorkspaceService::Stub> stub;
    GrpcFixture() {
        grpc::ServerBuilder builder;
        int port{};
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        if (!server || port == 0) throw std::runtime_error("cannot start loopback test listener");
        stub = v1::WorkspaceService::NewStub(grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    }
    ~GrpcFixture() {
        events.close();
        if (server) {
            server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(1));
            server->Wait();
        }
    }
    void authorize(grpc::ClientContext& context) const {
        context.AddMetadata("authorization", "Bearer " + token);
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
    }
};
} // namespace

TEST(ServerService, HealthReportsCountersAndWorkerCapacity) {
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    });
    WorkspaceService service(store, scheduler, events, [] { return "[]"; });
    grpc::ServerContext context;
    v1::HealthRequest request;
    v1::HealthResponse response;
    EXPECT_TRUE(service.GetHealth(&context, &request, &response).ok());
    EXPECT_EQ(response.status(), "SERVING");
    EXPECT_EQ(response.worker_count(), 2U);
}

TEST(ServerService, InvalidIdsMapToInvalidArgumentAndMissingMapsToNotFound) {
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    });
    WorkspaceService service(store, scheduler, events, [] { return "[]"; });
    grpc::ServerContext context;
    v1::GetJobRequest request;
    v1::Job response;
    EXPECT_EQ(service.GetJob(&context, &request, &response).error_code(),
              grpc::StatusCode::INVALID_ARGUMENT);
    request.set_id("00000000000000000000000000000000");
    EXPECT_EQ(service.GetJob(&context, &request, &response).error_code(),
              grpc::StatusCode::NOT_FOUND);
}

TEST(ServerService, ConfiguredBearerTokenRejectsMissingMetadata) {
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    });
    ServiceOptions options;
    options.bearer_token = std::string(32, 'a');
    WorkspaceService service(store, scheduler, events, [] { return "[]"; }, options);
    grpc::ServerContext context;
    v1::HealthRequest request;
    v1::HealthResponse response;
    EXPECT_EQ(service.GetHealth(&context, &request, &response).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
}

TEST(ServerService, WireStatusesAreLowercaseAndListAcceptsLowercaseFilters) {
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    });
    WorkspaceService service(store, scheduler, events, [] { return "[]"; });
    const auto job = scheduler.submit("{}", "", unix_ms() + 86400000);
    grpc::ServerContext context;
    v1::GetJobRequest get_request;
    get_request.set_id(job.id);
    v1::Job job_response;
    ASSERT_TRUE(service.GetJob(&context, &get_request, &job_response).ok());
    EXPECT_EQ(job_response.status(), "queued");
    v1::ListJobsRequest list_request;
    list_request.set_status("queued");
    v1::ListJobsResponse response;
    ASSERT_TRUE(service.ListJobs(&context, &list_request, &response).ok());
    ASSERT_EQ(response.jobs_size(), 1);
    EXPECT_EQ(response.jobs(0).status(), "queued");
    EXPECT_TRUE(response.jobs(0).request_json().empty());
    EXPECT_TRUE(response.jobs(0).result_json().empty());
}

TEST(ServerTransport, ValidBearerTokenWorksAndDuplicateMetadataIsRejected) {
    GrpcFixture fixture;
    v1::HealthRequest request;
    v1::HealthResponse response;
    grpc::ClientContext authorized;
    fixture.authorize(authorized);
    ASSERT_TRUE(fixture.stub->GetHealth(&authorized, request, &response).ok());
    EXPECT_EQ(response.status(), "SERVING");
    grpc::ClientContext duplicate;
    fixture.authorize(duplicate);
    duplicate.AddMetadata("authorization", "Bearer " + fixture.token);
    EXPECT_EQ(fixture.stub->GetHealth(&duplicate, request, &response).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
}

TEST(ServerTransport, SubscriptionCapacityIsBoundedAndCancellationCompletesPromptly) {
    GrpcFixture fixture;
    fixture.events.publish("system", "ready", "", "{}");
    v1::WatchEventsRequest request;
    request.set_channel("all");
    grpc::ClientContext first;
    fixture.authorize(first);
    auto stream = fixture.stub->WatchEvents(&first, request);
    v1::Event event;
    ASSERT_TRUE(stream->Read(&event)); // A received event proves the first subscription is active.
    EXPECT_EQ(event.type(), "ready");
    grpc::ClientContext second;
    fixture.authorize(second);
    auto denied = fixture.stub->WatchEvents(&second, request);
    EXPECT_FALSE(denied->Read(&event));
    EXPECT_EQ(denied->Finish().error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
    auto completed = std::async(std::launch::async, [&] {
        v1::Event next;
        for (; stream->Read(&next);) {}
        return stream->Finish();
    });
    first.TryCancel();
    ASSERT_EQ(completed.wait_for(std::chrono::seconds(2)), std::future_status::ready);
    EXPECT_EQ(completed.get().error_code(), grpc::StatusCode::CANCELLED);
    fixture.events.publish("system", "wake", "", "{}");
    for (int i = 0; i < 20 && fixture.service.subscriber_count() != 0; ++i) {
        fixture.events.wait(fixture.events.sequence(), std::chrono::milliseconds(100));
    }
    EXPECT_EQ(fixture.service.subscriber_count(), 0U);
}

TEST(ServerService, DatabaseRpcsRequireConfiguredAuthorization) {
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    });
    WorkspaceService service(store, scheduler, events, [] { return "[]"; },
                             {std::string(32, 'a'), 1});
    grpc::ServerContext context;
    v1::ListDataSourcesRequest sources_request;
    v1::ListDataSourcesResponse sources_response;
    v1::GetDataCatalogRequest catalog_request;
    v1::DataCatalog catalog_response;
    v1::PreviewDataRequest preview_request;
    v1::QueryDataRequest query_request;
    v1::DataResult result;
    EXPECT_EQ(service.ListDataSources(&context, &sources_request, &sources_response).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.GetDataCatalog(&context, &catalog_request, &catalog_response).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.PreviewData(&context, &preview_request, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.QueryData(&context, &query_request, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
}

TEST(ServerService, NoDatabaseProvidersReturnsEmptySourcesAndNotFoundForQueries) {
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    });
    WorkspaceService service(store, scheduler, events, [] { return "[]"; });
    grpc::ServerContext context;
    v1::ListDataSourcesRequest sources_request;
    v1::ListDataSourcesResponse sources_response;
    EXPECT_TRUE(service.ListDataSources(&context, &sources_request, &sources_response).ok());
    EXPECT_EQ(sources_response.sources_size(), 0);
    v1::QueryDataRequest query_request;
    query_request.set_source_id("unknown");
    query_request.set_sql("SELECT 1");
    v1::DataResult result;
    EXPECT_EQ(service.QueryData(&context, &query_request, &result).error_code(),
              grpc::StatusCode::NOT_FOUND);
}
TEST(ServerService, AllWorkspaceRpcsRequireConfiguredAuthorization) {
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    });
    WorkspaceService service(store, scheduler, events, [] { return "[]"; },
                             {std::string(32, 'a'), 1});
    grpc::ServerContext context;
    v1::ListWorkspacesRequest list;
    v1::ListWorkspacesResponse page;
    v1::GetWorkspaceRequest get;
    v1::CreateWorkspaceRequest create;
    v1::UpdateWorkspaceRequest update;
    v1::DeleteWorkspaceRequest remove;
    v1::DeleteWorkspaceResponse removed;
    v1::WorkspaceDocument result;
    EXPECT_EQ(service.ListWorkspaces(&context, &list, &page).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.GetWorkspace(&context, &get, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.CreateWorkspace(&context, &create, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.UpdateWorkspace(&context, &update, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.DeleteWorkspace(&context, &remove, &removed).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(store.list_workspaces(100, 0).total, 0U);
}
TEST(ServerTransport, NamedWorkspaceCrudMapsOptimisticConflictsAndPublishesMetadataOnly) {
    GrpcFixture fixture;
    v1::CreateWorkspaceRequest create;
    create.set_name("Database team desk");
    create.set_document_json(R"({"schema_version":1,"layout":{},)"
                             R"("panel_state":{"sql":"SELECT 1"}})");
    v1::WorkspaceDocument saved;
    grpc::ClientContext first;
    fixture.authorize(first);
    ASSERT_TRUE(fixture.stub->CreateWorkspace(&first, create, &saved).ok());
    EXPECT_EQ(saved.revision(), 1U);
    const auto notifications = fixture.events.read(0, "system", "");
    ASSERT_EQ(notifications.size(), 1U);
    EXPECT_EQ(notifications[0].type, "workspace.created");
    EXPECT_NE(notifications[0].payload_json.find(saved.id()), std::string::npos);
    EXPECT_EQ(notifications[0].payload_json.find("SELECT"), std::string::npos);
    v1::ListWorkspacesRequest list;
    v1::ListWorkspacesResponse page;
    grpc::ClientContext list_context;
    fixture.authorize(list_context);
    ASSERT_TRUE(fixture.stub->ListWorkspaces(&list_context, list, &page).ok());
    ASSERT_EQ(page.workspaces_size(), 1);
    EXPECT_TRUE(page.workspaces(0).document_json().empty());
    v1::UpdateWorkspaceRequest update;
    update.set_id(saved.id());
    update.set_name("Updated desk");
    update.set_document_json(create.document_json());
    update.set_expected_revision(1);
    grpc::ClientContext updated_context;
    fixture.authorize(updated_context);
    ASSERT_TRUE(fixture.stub->UpdateWorkspace(&updated_context, update, &saved).ok());
    EXPECT_EQ(saved.revision(), 2U);
    grpc::ClientContext stale_context;
    fixture.authorize(stale_context);
    EXPECT_EQ(fixture.stub->UpdateWorkspace(&stale_context, update, &saved).error_code(),
              grpc::StatusCode::ALREADY_EXISTS);
    v1::DeleteWorkspaceRequest remove;
    remove.set_id(update.id());
    remove.set_expected_revision(1);
    v1::DeleteWorkspaceResponse removed;
    grpc::ClientContext stale_delete;
    fixture.authorize(stale_delete);
    EXPECT_EQ(fixture.stub->DeleteWorkspace(&stale_delete, remove, &removed).error_code(),
              grpc::StatusCode::ALREADY_EXISTS);
    remove.set_expected_revision(2);
    grpc::ClientContext delete_context;
    fixture.authorize(delete_context);
    ASSERT_TRUE(fixture.stub->DeleteWorkspace(&delete_context, remove, &removed).ok());
    v1::GetWorkspaceRequest get;
    get.set_id(remove.id());
    grpc::ClientContext missing_context;
    fixture.authorize(missing_context);
    EXPECT_EQ(fixture.stub->GetWorkspace(&missing_context, get, &saved).error_code(),
              grpc::StatusCode::NOT_FOUND);
}
TEST(ServerService, WorkspaceCommitSucceedsWhenDurableEventAllocationFails) {
    struct File {
        std::filesystem::path path{std::filesystem::temp_directory_path() /
            ("atx-workspace-event-test-" + std::to_string(unix_ms()) + ".db")};
        ~File() {
            std::error_code ignored;
            for (const auto* suffix : {"", "-wal", "-shm"}) {
                std::filesystem::remove(path.string() + suffix, ignored);
            }
        }
    } file;
    Store store(file.path.string());
    EventBus events(1, &store);
    for (unsigned i = 0; i < 4095; ++i) events.publish("system", "fixture", "", "{}");
    EXPECT_EQ(events.sequence(), 4096U);
    sqlite3* raw{};
    ASSERT_EQ(sqlite3_open(file.path.string().c_str(), &raw), SQLITE_OK);
    struct Close {
        sqlite3*& value;
        ~Close() { if (value != nullptr) sqlite3_close(value); }
    } close{raw};
    ASSERT_EQ(sqlite3_exec(raw, "DROP TABLE metadata", nullptr, nullptr, nullptr), SQLITE_OK);
    ASSERT_EQ(sqlite3_close(raw), SQLITE_OK);
    raw = nullptr;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    }, 1, 1, false);
    WorkspaceService service(store, scheduler, events, [] { return "[]"; });
    grpc::ServerContext context;
    v1::CreateWorkspaceRequest request;
    request.set_name("Committed desk");
    request.set_document_json(R"({"schema_version":1,"layout":{}})");
    v1::WorkspaceDocument response;
    ASSERT_TRUE(service.CreateWorkspace(&context, &request, &response).ok());
    EXPECT_EQ(store.get_workspace(response.id()).revision, 1U);
    EXPECT_EQ(events.sequence(), 4096U);
}
TEST(ServerService, AllResearchLibraryRpcsRequireAuthorization) {
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events, [](std::string_view, std::stop_token, const Progress&) {
        return std::string("{}");
    }, 1, 1, false);
    WorkspaceService service(store, scheduler, events, [] { return "[]"; },
                             {std::string(32, 'a'), 1});
    grpc::ServerContext context;
    v1::ListAlphasRequest list;
    v1::ListAlphasResponse page;
    v1::GetAlphaRequest get;
    v1::CreateAlphaRequest create;
    v1::UpdateAlphaRequest update;
    v1::ListAlphaRevisionsRequest history;
    v1::ListAlphaRevisionsResponse revisions;
    v1::SetAlphaArchivedRequest archive;
    v1::AlphaSnapshot result;
    EXPECT_EQ(service.ListAlphas(&context, &list, &page).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.GetAlpha(&context, &get, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.CreateAlpha(&context, &create, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.UpdateAlpha(&context, &update, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.ListAlphaRevisions(&context, &history, &revisions).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    EXPECT_EQ(service.SetAlphaArchived(&context, &archive, &result).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
}
TEST(ServerTransport, AlphaLibraryCrudRetainsHistoryMapsErrorsAndPublishesMetadataOnly) {
    GrpcFixture fixture;
    v1::CreateAlphaRequest create;
    create.set_name("Momentum");
    create.set_hypothesis("Relative strength persists");
    create.add_tags("momentum");
    create.set_request_json(R"({"mode":"synthetic_demo","sessions":32})");
    v1::AlphaSnapshot alpha;
    grpc::ClientContext initial;
    fixture.authorize(initial);
    ASSERT_TRUE(fixture.stub->CreateAlpha(&initial, create, &alpha).ok());
    EXPECT_EQ(alpha.revision(), 1U);
    EXPECT_EQ(alpha.request_sha256().size(), 64U);
    ASSERT_EQ(alpha.tags_size(), 1);
    EXPECT_EQ(alpha.tags(0), "momentum");
    const auto events = fixture.events.read(0, "system", "");
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].type, "alpha.created");
    EXPECT_EQ(events[0].payload_json.find("sessions"), std::string::npos);
    const auto id = alpha.id();
    v1::UpdateAlphaRequest update;
    update.set_id(id);
    update.set_expected_revision(1);
    update.set_name("Revised");
    update.set_request_json(create.request_json());
    grpc::ClientContext updated;
    fixture.authorize(updated);
    ASSERT_TRUE(fixture.stub->UpdateAlpha(&updated, update, &alpha).ok());
    EXPECT_EQ(alpha.revision(), 2U);
    grpc::ClientContext stale;
    fixture.authorize(stale);
    EXPECT_EQ(fixture.stub->UpdateAlpha(&stale, update, &alpha).error_code(),
              grpc::StatusCode::ALREADY_EXISTS);
    v1::GetAlphaRequest get;
    get.set_id(id);
    get.set_revision(1);
    grpc::ClientContext original;
    fixture.authorize(original);
    ASSERT_TRUE(fixture.stub->GetAlpha(&original, get, &alpha).ok());
    EXPECT_EQ(alpha.name(), "Momentum");
    v1::ListAlphaRevisionsRequest history;
    history.set_id(id);
    v1::ListAlphaRevisionsResponse revisions;
    grpc::ClientContext history_context;
    fixture.authorize(history_context);
    ASSERT_TRUE(fixture.stub->ListAlphaRevisions(&history_context, history, &revisions).ok());
    EXPECT_EQ(revisions.total(), 2U);
    EXPECT_EQ(revisions.revisions_size(), 2);
    v1::SetAlphaArchivedRequest archive;
    archive.set_id(id);
    archive.set_expected_revision(2);
    archive.set_archived(true);
    grpc::ClientContext archive_context;
    fixture.authorize(archive_context);
    ASSERT_TRUE(fixture.stub->SetAlphaArchived(&archive_context, archive, &alpha).ok());
    EXPECT_EQ(alpha.stage(), "archived");
    v1::ListAlphasRequest list;
    v1::ListAlphasResponse page;
    grpc::ClientContext list_context;
    fixture.authorize(list_context);
    ASSERT_TRUE(fixture.stub->ListAlphas(&list_context, list, &page).ok());
    EXPECT_EQ(page.total(), 0U);
    list.set_include_archived(true);
    grpc::ClientContext all_context;
    fixture.authorize(all_context);
    ASSERT_TRUE(fixture.stub->ListAlphas(&all_context, list, &page).ok());
    ASSERT_EQ(page.alphas_size(), 1);
    EXPECT_TRUE(page.alphas(0).request_json().empty());
    EXPECT_TRUE(page.alphas(0).hypothesis().empty());
    get.set_revision(999);
    grpc::ClientContext missing;
    fixture.authorize(missing);
    EXPECT_EQ(fixture.stub->GetAlpha(&missing, get, &alpha).error_code(),
              grpc::StatusCode::NOT_FOUND);
}
} // namespace atx::server
