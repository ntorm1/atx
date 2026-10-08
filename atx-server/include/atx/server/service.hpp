#pragma once

#include <atx/server/core.hpp>
#include <atx/server/access.hpp>
#include <atx/server/database.hpp>
#include <atx/v1/workspace.grpc.pb.h>

#include <chrono>
#include <functional>
#include <memory>
#include <string>

namespace atx::server {
class ResearchBackend;
struct ServiceOptions {
    std::string bearer_token;
    std::uint32_t max_subscribers{32};
    std::shared_ptr<const AccessPolicy> access_policy{};
};
// The store, scheduler, event bus and optional registries must outlive this service
// and its RPC handlers. The registry/backend pointers are optional, non-owning observers.
// All exception translation and authenticated input validation happens at this boundary.
class WorkspaceService final : public v1::WorkspaceService::Service {
public:
    WorkspaceService(Store& store, Scheduler& scheduler, EventBus& events,
                     std::function<std::string()> strategies, ServiceOptions options = {},
                     const DatabaseRegistry* databases = nullptr,
                     const ResearchBackend* research = nullptr);
    ~WorkspaceService() override;
    WorkspaceService(const WorkspaceService&) = delete;
    WorkspaceService& operator=(const WorkspaceService&) = delete;
    WorkspaceService(WorkspaceService&&) = delete;
    WorkspaceService& operator=(WorkspaceService&&) = delete;
    // Thread-safe live stream counter for operational resource monitoring.
    [[nodiscard]] std::uint32_t subscriber_count() const noexcept;
    grpc::Status GetHealth(grpc::ServerContext*, const v1::HealthRequest*,
                           v1::HealthResponse*) override;
    grpc::Status ListStrategies(grpc::ServerContext*, const v1::ListStrategiesRequest*,
                                v1::ListStrategiesResponse*) override;
    grpc::Status SubmitJob(grpc::ServerContext*, const v1::SubmitJobRequest*, v1::Job*) override;
    grpc::Status GetJob(grpc::ServerContext*, const v1::GetJobRequest*, v1::Job*) override;
    grpc::Status ListJobs(grpc::ServerContext*, const v1::ListJobsRequest*,
                          v1::ListJobsResponse*) override;
    grpc::Status CancelJob(grpc::ServerContext*, const v1::CancelJobRequest*, v1::Job*) override;
    grpc::Status WatchEvents(grpc::ServerContext*, const v1::WatchEventsRequest*,
                             grpc::ServerWriter<v1::Event>*) override;
    grpc::Status ListDataSources(grpc::ServerContext*, const v1::ListDataSourcesRequest*,
                                 v1::ListDataSourcesResponse*) override;
    grpc::Status GetDataCatalog(grpc::ServerContext*, const v1::GetDataCatalogRequest*,
                                v1::DataCatalog*) override;
    grpc::Status PreviewData(grpc::ServerContext*, const v1::PreviewDataRequest*,
                             v1::DataResult*) override;
    grpc::Status QueryData(grpc::ServerContext*, const v1::QueryDataRequest*,
                           v1::DataResult*) override;
    grpc::Status ListWorkspaces(grpc::ServerContext*, const v1::ListWorkspacesRequest*,
                                v1::ListWorkspacesResponse*) override;
    grpc::Status GetWorkspace(grpc::ServerContext*, const v1::GetWorkspaceRequest*,
                              v1::WorkspaceDocument*) override;
    grpc::Status CreateWorkspace(grpc::ServerContext*, const v1::CreateWorkspaceRequest*,
                                 v1::WorkspaceDocument*) override;
    grpc::Status UpdateWorkspace(grpc::ServerContext*, const v1::UpdateWorkspaceRequest*,
                                 v1::WorkspaceDocument*) override;
    grpc::Status DeleteWorkspace(grpc::ServerContext*, const v1::DeleteWorkspaceRequest*,
                                 v1::DeleteWorkspaceResponse*) override;
    grpc::Status ListAlphas(grpc::ServerContext*, const v1::ListAlphasRequest*,
                            v1::ListAlphasResponse*) override;
    grpc::Status GetAlpha(grpc::ServerContext*, const v1::GetAlphaRequest*,
                          v1::AlphaSnapshot*) override;
    grpc::Status CreateAlpha(grpc::ServerContext*, const v1::CreateAlphaRequest*,
                             v1::AlphaSnapshot*) override;
    grpc::Status UpdateAlpha(grpc::ServerContext*, const v1::UpdateAlphaRequest*,
                             v1::AlphaSnapshot*) override;
    grpc::Status ListAlphaRevisions(grpc::ServerContext*, const v1::ListAlphaRevisionsRequest*,
                                    v1::ListAlphaRevisionsResponse*) override;
    grpc::Status SetAlphaArchived(grpc::ServerContext*, const v1::SetAlphaArchivedRequest*,
                                  v1::AlphaSnapshot*) override;
    grpc::Status CompareJobs(grpc::ServerContext*, const v1::CompareJobsRequest*,
                             v1::CompareJobsResponse*) override;
    grpc::Status ValidateExpression(grpc::ServerContext*, const v1::ValidateExpressionRequest*,
                                    v1::ValidateExpressionResponse*) override;
    grpc::Status CreateExperiment(grpc::ServerContext*, const v1::CreateExperimentRequest*,
                                  v1::Experiment*) override;
    grpc::Status GetExperiment(grpc::ServerContext*, const v1::GetExperimentRequest*,
                               v1::Experiment*) override;
    grpc::Status ListExperiments(grpc::ServerContext*, const v1::ListExperimentsRequest*,
                                 v1::ListExperimentsResponse*) override;
    grpc::Status CancelExperiment(grpc::ServerContext*, const v1::CancelExperimentRequest*,
                                  v1::Experiment*) override;
    grpc::Status CreateWorkflow(grpc::ServerContext*, const v1::CreateWorkflowRequest*,
                                v1::Workflow*) override;
    grpc::Status GetWorkflow(grpc::ServerContext*, const v1::GetWorkflowRequest*,
                             v1::Workflow*) override;
    grpc::Status ListWorkflows(grpc::ServerContext*, const v1::ListWorkflowsRequest*,
                               v1::ListWorkflowsResponse*) override;
    grpc::Status SetWorkflowPaused(grpc::ServerContext*, const v1::SetWorkflowPausedRequest*,
                                   v1::Workflow*) override;
    grpc::Status ListWorkflowRuns(grpc::ServerContext*, const v1::ListWorkflowRunsRequest*,
                                  v1::ListWorkflowRunsResponse*) override;
    grpc::Status GetSession(grpc::ServerContext*, const v1::SessionRequest*,
                            v1::SessionResponse*) override;
    grpc::Status ListTeam(grpc::ServerContext*, const v1::ListTeamRequest*,
                          v1::ListTeamResponse*) override;
    grpc::Status ListAudit(grpc::ServerContext*, const v1::ListAuditRequest*,
                           v1::ListAuditResponse*) override;
    grpc::Status SubmitReview(grpc::ServerContext*, const v1::SubmitReviewRequest*,
                              v1::ReviewRecord*) override;
    grpc::Status GetReview(grpc::ServerContext*, const v1::GetReviewRequest*,
                           v1::ReviewRecord*) override;
    grpc::Status ListReviews(grpc::ServerContext*, const v1::ListReviewsRequest*,
                             v1::ListReviewsResponse*) override;
    grpc::Status DecideReview(grpc::ServerContext*, const v1::DecideReviewRequest*,
                              v1::ReviewRecord*) override;
    grpc::Status ListReviewDecisions(grpc::ServerContext*, const v1::ListReviewDecisionsRequest*,
                                     v1::ListReviewDecisionsResponse*) override;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace atx::server
