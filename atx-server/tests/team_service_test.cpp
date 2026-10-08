#include <atx/server/service.hpp>

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <array>
#include <stdexcept>
#include <string>

namespace atx::server {
namespace {
std::string team_digest(char value) {
    const auto token = std::string(32, value);
    std::array<unsigned char, 32> hash{};
    unsigned int bytes{};
    if (EVP_Digest(token.data(), token.size(), hash.data(), &bytes, EVP_sha256(), nullptr) != 1 ||
        bytes != hash.size()) throw std::runtime_error("test digest failure");
    std::string result;
    for (const auto byte : hash) {
        result.push_back("0123456789abcdef"[byte >> 4]);
        result.push_back("0123456789abcdef"[byte & 15]);
    }
    return result;
}
std::shared_ptr<const AccessPolicy> team_policy() {
    auto principals = nlohmann::json::array();
    for (const auto* role : {"viewer", "researcher", "reviewer", "admin"}) {
        const auto token = std::string_view(role) == "reviewer" ? 'x' : role[0];
        principals.push_back({{"id", role}, {"name", role}, {"role", role},
                              {"token_sha256", team_digest(token)}});
    }
    return std::make_shared<const AccessPolicy>(nlohmann::json{{"schema_version", 1},
        {"team_name", "Shared research"}, {"principals", principals}}.dump(), unix_ms());
}
struct TeamFixture {
    Store store{":memory:"};
    EventBus events;
    Scheduler scheduler{store, events,
        [](std::string_view, std::stop_token, const Progress&) { return std::string("{}"); }};
    WorkspaceService service{store, scheduler, events, [] { return "{}"; },
                              {std::string(32, 'g'), 32, team_policy()}};
    std::unique_ptr<grpc::Server> server;
    std::unique_ptr<v1::WorkspaceService::Stub> client;
    TeamFixture() {
        grpc::ServerBuilder builder;
        int port{};
        builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
        builder.RegisterService(&service);
        server = builder.BuildAndStart();
        if (!server || port == 0) throw std::runtime_error("test listener unavailable");
        client = v1::WorkspaceService::NewStub(grpc::CreateChannel(
            "127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
    }
    ~TeamFixture() {
        server->Shutdown();
        server->Wait();
    }
    TeamFixture(const TeamFixture&) = delete;
    TeamFixture& operator=(const TeamFixture&) = delete;
    TeamFixture(TeamFixture&&) = delete;
    TeamFixture& operator=(TeamFixture&&) = delete;
};
void credential(grpc::ClientContext& context, char token) {
    context.AddMetadata("authorization", "Bearer " + std::string(32, token));
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
}

TEST(TeamService, ViewerReadsSharedTeamButCannotMutateAnyResearchResource) {
    TeamFixture fixture;
    grpc::ClientContext session_context;
    credential(session_context, 'v');
    v1::SessionRequest session_request;
    v1::SessionResponse session;
    ASSERT_TRUE(fixture.client->GetSession(&session_context, session_request, &session).ok());
    EXPECT_EQ(session.principal().id(), "viewer");
    EXPECT_EQ(session.principal().role(), "viewer");
    EXPECT_TRUE(session.team_mode());
    EXPECT_EQ(session.project_id(), "local");
    grpc::ClientContext write;
    credential(write, 'v');
    v1::CreateAlphaRequest alpha;
    alpha.set_name("Forbidden write");
    alpha.set_request_json(R"({"mode":"synthetic_demo","sessions":32})");
    v1::AlphaSnapshot snapshot;
    EXPECT_EQ(fixture.client->CreateAlpha(&write, alpha, &snapshot).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);
    grpc::ClientContext job_context;
    credential(job_context, 'v');
    v1::SubmitJobRequest submit;
    v1::Job job;
    EXPECT_EQ(fixture.client->SubmitJob(&job_context, submit, &job).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);
    EXPECT_EQ(fixture.store.list_alphas(10, 0).total, 0U);
}

TEST(TeamService, ResearcherMutationsCarryAuthenticatedActorAndReviewRequiresReviewer) {
    TeamFixture fixture;
    grpc::ClientContext create;
    credential(create, 'r');
    v1::CreateAlphaRequest input;
    input.set_name("Attributed research");
    input.set_request_json(R"({"mode":"synthetic_demo","sessions":32})");
    v1::AlphaSnapshot alpha;
    const auto status = fixture.client->CreateAlpha(&create, input, &alpha);
    ASSERT_TRUE(status.ok()) << status.error_message();
    EXPECT_EQ(alpha.actor_id(), "researcher");
    grpc::ClientContext decision_context;
    credential(decision_context, 'r');
    v1::DecideReviewRequest request;
    request.set_state("approved");
    v1::ReviewRecord review;
    EXPECT_EQ(fixture.client->DecideReview(&decision_context, request, &review).error_code(),
              grpc::StatusCode::PERMISSION_DENIED);
}

TEST(TeamService, GatewayCredentialIsRestrictedToHealthAndEventFanoutInTeamMode) {
    TeamFixture fixture;
    grpc::ClientContext health_context;
    credential(health_context, 'g');
    v1::HealthRequest health_request;
    v1::HealthResponse health;
    ASSERT_TRUE(fixture.client->GetHealth(&health_context, health_request, &health).ok());
    grpc::ClientContext session_context;
    credential(session_context, 'g');
    v1::SessionRequest session_request;
    v1::SessionResponse session;
    EXPECT_EQ(fixture.client->GetSession(&session_context, session_request, &session).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
    grpc::ClientContext submit_context;
    credential(submit_context, 'g');
    v1::SubmitJobRequest input;
    v1::Job job;
    EXPECT_EQ(fixture.client->SubmitJob(&submit_context, input, &job).error_code(),
              grpc::StatusCode::UNAUTHENTICATED);
}
TEST(TeamService, ExpiredPrincipalCredentialCannotBecomeGatewayCredentialAtStartup) {
    const auto now = unix_ms();
    auto principals = nlohmann::json::array({
        {{"id", "expired_viewer"}, {"name", "Expired viewer"}, {"role", "viewer"},
         {"token_sha256", team_digest('v')}, {"expires_at_ms", now - 1}},
        {{"id", "admin"}, {"name", "Administrator"}, {"role", "admin"},
         {"token_sha256", team_digest('a')}, {"expires_at_ms", 0}}});
    const auto policy = std::make_shared<const AccessPolicy>(nlohmann::json{
        {"schema_version", 1}, {"team_name", "Research"}, {"principals", principals}}.dump(), now);
    Store store(":memory:");
    EventBus events;
    Scheduler scheduler(store, events,
        [](std::string_view, std::stop_token, const Progress&) { return std::string("{}"); });
    EXPECT_THROW((void)WorkspaceService(store, scheduler, events, [] { return "{}"; },
        {std::string(32, 'v'), 32, policy}), Error);
}
} // namespace
} // namespace atx::server
