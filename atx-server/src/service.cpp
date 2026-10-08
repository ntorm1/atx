#include <atx/server/service.hpp>
#include <atx/server/analysis.hpp>
#include <atx/server/research.hpp>

#include <openssl/crypto.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace atx::server {
namespace {
class CounterLease final {
public:
    explicit CounterLease(std::atomic<std::uint32_t>& count)
        : count_(count), previous_(count.fetch_add(1)) {}
    ~CounterLease() { count_.fetch_sub(1); }
    CounterLease(const CounterLease&) = delete;
    CounterLease& operator=(const CounterLease&) = delete;
    CounterLease(CounterLease&&) = delete;
    CounterLease& operator=(CounterLease&&) = delete;
    [[nodiscard]] std::uint32_t previous() const noexcept { return previous_; }
private:
    std::atomic<std::uint32_t>& count_;
    const std::uint32_t previous_;
};
void copy_job(const Job& job, v1::Job& response) {
    response.set_id(job.id);
    auto status = job.status;
    for (auto& ch : status) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    response.set_status(status);
    response.set_request_json(job.request_json);
    response.set_result_json(job.result_json);
    response.set_error(job.error);
    response.set_progress(job.progress);
    response.set_created_at_ms(job.created_at_ms);
    response.set_scheduled_at_ms(job.scheduled_at_ms);
    response.set_started_at_ms(job.started_at_ms);
    response.set_finished_at_ms(job.finished_at_ms);
    response.set_label(job.label);
    response.set_alpha_id(job.alpha_id);
    response.set_alpha_revision(job.alpha_revision);
    response.set_experiment_id(job.experiment_id);
    response.set_actor_id(job.actor_id);
    response.set_workflow_id(job.workflow_id);
    response.set_workflow_slot_ms(job.workflow_slot_ms);
}
void copy_workflow(const Workflow& source, v1::Workflow& response) {
    response.Clear();
    response.set_id(source.id);
    response.set_name(source.name);
    response.set_description(source.description);
    response.set_request_json(source.request_json);
    response.set_request_sha256(source.request_sha256);
    response.set_alpha_id(source.alpha_id);
    response.set_alpha_revision(source.alpha_revision);
    response.set_actor_id(source.actor_id);
    response.set_interval_ms(source.interval_ms);
    response.set_max_runs(source.max_runs);
    response.set_first_run_at_ms(source.first_run_at_ms);
    response.set_next_run_at_ms(source.next_run_at_ms);
    response.set_revision(source.revision);
    response.set_enabled(source.enabled);
    response.set_state(source.state);
    response.set_pause_reason(source.pause_reason);
    response.set_last_dispatch_error(source.last_dispatch_error);
    response.set_created_at_ms(source.created_at_ms);
    response.set_updated_at_ms(source.updated_at_ms);
    response.set_admitted_runs(source.admitted_runs);
    response.set_queued(source.queued);
    response.set_running(source.running);
    response.set_succeeded(source.succeeded);
    response.set_failed(source.failed);
    response.set_cancelled(source.cancelled);
    response.set_missed_slots(source.missed_slots);
    response.set_skipped_slots(source.skipped_slots);
    response.set_active_job_id(source.active_job_id);
    response.set_last_job_id(source.last_job_id);
}
void copy_experiment(const Experiment& source, v1::Experiment& response) {
    response.Clear();
    response.set_id(source.id);
    response.set_name(source.name);
    response.set_description(source.description);
    response.set_created_at_ms(source.created_at_ms);
    response.set_scheduled_at_ms(source.scheduled_at_ms);
    auto status = source.status;
    for (auto& ch : status)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    response.set_status(status);
    response.set_progress(source.progress);
    response.set_total(source.total);
    response.set_queued(source.queued);
    response.set_running(source.running);
    response.set_succeeded(source.succeeded);
    response.set_failed(source.failed);
    response.set_cancelled(source.cancelled);
    response.set_actor_id(source.actor_id);
    for (const auto& job : source.jobs) copy_job(job, *response.add_jobs());
}
void copy_workspace(const WorkspaceDocument& source, v1::WorkspaceDocument& response) {
    response.set_id(source.id);
    response.set_name(source.name);
    response.set_description(source.description);
    response.set_document_json(source.document_json);
    response.set_revision(source.revision);
    response.set_created_at_ms(source.created_at_ms);
    response.set_updated_at_ms(source.updated_at_ms);
    response.set_actor_id(source.actor_id);
}
void copy_alpha(const AlphaSnapshot& source, v1::AlphaSnapshot& response) {
    response.Clear();
    response.set_id(source.id);
    response.set_project_id(source.project_id);
    response.set_name(source.name);
    response.set_description(source.description);
    for (const auto& tag : nlohmann::json::parse(source.tags_json)) {
        response.add_tags(tag.get<std::string>());
    }
    response.set_stage(source.stage);
    response.set_revision(source.revision);
    response.set_hypothesis(source.hypothesis);
    response.set_request_json(source.request_json);
    response.set_request_sha256(source.request_sha256);
    response.set_source_job_id(source.source_job_id);
    response.set_created_at_ms(source.created_at_ms);
    response.set_updated_at_ms(source.updated_at_ms);
    response.set_actor_id(source.actor_id);
}
void copy_principal(const Principal& source, v1::Principal& response) {
    response.set_id(source.id);
    response.set_name(source.name);
    response.set_role(std::string(role_name(source.role)));
    response.set_expires_at_ms(source.expires_at_ms);
}
void copy_review(const ReviewRecord& source, v1::ReviewRecord& response) {
    response.set_id(source.id);
    response.set_alpha_id(source.alpha_id);
    response.set_alpha_revision(source.alpha_revision);
    response.set_request_sha256(source.request_sha256);
    response.set_requester_actor_id(source.requester_actor_id);
    response.set_author_actor_id(source.author_actor_id);
    response.set_state(source.state);
    response.set_revision(source.revision);
    response.set_note(source.note);
    response.set_evidence_json(source.evidence_json);
    response.set_created_at_ms(source.created_at_ms);
    response.set_updated_at_ms(source.updated_at_ms);
    response.set_current(source.current);
}
template<class Request> AlphaInput alpha_input(const Request& request, std::string_view actor) {
    if (request.tags_size() > 16 || request.name().size() > 320 ||
        request.description().size() > 512 || request.hypothesis().size() > 4096 ||
        request.request_json().size() > 65536 || request.source_job_id().size() > 32) {
        throw Error(ErrorCode::invalid, "alpha input exceeds byte budgets");
    }
    AlphaInput input{request.name(), request.description(), {}, request.hypothesis(),
        request.request_json(), request.source_job_id(), "local", std::string(actor)};
    input.tags.reserve(static_cast<std::size_t>(request.tags_size()));
    for (const auto& tag : request.tags()) {
        if (tag.size() > 64) throw Error(ErrorCode::invalid, "alpha tag exceeds 64 bytes");
        input.tags.push_back(tag);
    }
    return input;
}
void validate_id(std::string_view id) {
    if (id.size() != 32 || !std::all_of(id.begin(), id.end(), [](char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    })) throw Error(ErrorCode::invalid, "job id must contain 32 lowercase hexadecimal characters");
}
grpc::Status translate_error(const Error& error) {
    switch (error.code()) {
    case ErrorCode::invalid: return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
    case ErrorCode::missing: return {grpc::StatusCode::NOT_FOUND, error.what()};
    case ErrorCode::conflict: return {grpc::StatusCode::ALREADY_EXISTS, error.what()};
    case ErrorCode::capacity: return {grpc::StatusCode::RESOURCE_EXHAUSTED, error.what()};
    case ErrorCode::unavailable:
        return {grpc::StatusCode::UNAVAILABLE, "persistent service is unavailable"};
    }
    return {grpc::StatusCode::INTERNAL, "unknown service error"};
}
grpc::Status translate_data_error(const DataError& error) {
    switch (error.code()) {
    case DataErrorCode::invalid: return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
    case DataErrorCode::missing: return {grpc::StatusCode::NOT_FOUND, error.what()};
    case DataErrorCode::unavailable: return {grpc::StatusCode::UNAVAILABLE, error.what()};
    case DataErrorCode::capacity: return {grpc::StatusCode::RESOURCE_EXHAUSTED, error.what()};
    case DataErrorCode::deadline: return {grpc::StatusCode::DEADLINE_EXCEEDED, error.what()};
    case DataErrorCode::cancelled: return {grpc::StatusCode::CANCELLED, error.what()};
    }
    return {grpc::StatusCode::INTERNAL, "unknown database failure"};
}
DataContext data_context(grpc::ServerContext& context) {
    DataContext operation;
    const auto now = std::chrono::system_clock::now();
    const auto remaining = context.deadline() <= now ? std::chrono::system_clock::duration::zero()
        : std::min(context.deadline() - now,
                   std::chrono::duration_cast<std::chrono::system_clock::duration>(
                       std::chrono::seconds(5)));
    operation.deadline = std::chrono::steady_clock::now() + remaining;
    operation.cancelled = [&context] { return context.IsCancelled(); };
    return operation;
}
void copy_result(const DataResult& source, v1::DataResult& response) {
    response.set_has_more(source.has_more);
    response.set_truncated(source.truncated);
    response.set_elapsed_ms(source.elapsed_ms);
    for (const auto& column : source.columns) {
        auto* output = response.add_columns();
        output->set_name(column.name);
        output->set_type(column.type);
    }
    for (const auto& row : source.rows) {
        auto* output = response.add_rows();
        for (const auto& cell : row.cells) {
            auto* field = output->add_cells();
            field->set_text(cell.text);
            field->set_is_null(cell.is_null);
            field->set_truncated(cell.truncated);
        }
    }
    for (const auto& warning : source.warnings) response.add_warnings(warning);
}
void log_unexpected(std::string_view scope, const char* message) noexcept {
    // Bound output to one message/second globally, including sustained database failure.
    static std::atomic<std::int64_t> last_log_ms{std::numeric_limits<std::int64_t>::min()};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto previous = last_log_ms.load();
    if (previous > now - 1000 || !last_log_ms.compare_exchange_strong(previous, now)) return;
    // Best-effort operational logging never changes the RPC result; credentials are excluded.
    static_cast<void>(std::fprintf(stderr, "atx-server %.*s: %.2048s\n",
        static_cast<int>(scope.size()), scope.data(), message));
}
} // namespace

struct WorkspaceService::Impl {
    Store& store;
    Scheduler& scheduler;
    EventBus& events;
    const DatabaseRegistry* databases;
    const ResearchBackend* research;
    std::function<std::string()> strategies;
    ServiceOptions options;
    const std::string expected_authorization;
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    std::atomic<std::uint32_t> subscribers{};
    std::atomic<std::uint32_t> comparisons{};
    Impl(Store& jobs, Scheduler& jobs_scheduler, EventBus& channels,
         std::function<std::string()> catalog, ServiceOptions settings,
         const DatabaseRegistry* data_registry, const ResearchBackend* research_backend)
        : store(jobs), scheduler(jobs_scheduler), events(channels), databases(data_registry),
          research(research_backend), strategies(std::move(catalog)),
          options(std::move(settings)), expected_authorization("Bearer " + options.bearer_token) {
        if (!strategies || options.max_subscribers == 0 || options.max_subscribers > 256) {
            throw Error(ErrorCode::invalid, "invalid service subscription configuration");
        }
        if (options.access_policy && (options.bearer_token.size() < 32 ||
            options.access_policy->contains_credential(expected_authorization))) {
            throw Error(ErrorCode::invalid, "team mode requires a separate gateway credential");
        }
    }
    const DatabaseRegistry& database_registry() const {
        static const DatabaseRegistry empty;
        return databases == nullptr ? empty : *databases;
    }
    void durable_notification(std::string_view type, std::string_view id,
        std::uint64_t revision) noexcept {
        try {
            // Metadata only: layouts, draft SQL and any credentials stay out of event payloads.
            events.publish("system", std::string(type), "", "{\"id\":\"" + std::string(id) +
                "\",\"revision\":\"" + std::to_string(revision) + "\"}");
        } catch (const std::exception& error) {
            // The transaction already committed. Notification failure must not imply save failure.
            log_unexpected("durable notification", error.what());
        } catch (...) {
            log_unexpected("durable notification", "unknown failure after commit");
        }
    }
    grpc::Status authorize(const grpc::ServerContext& context, Principal& principal,
        Permission permission = Permission::read, bool allow_service = false) const {
        if (context.IsCancelled()) return {grpc::StatusCode::CANCELLED, "RPC cancelled"};
        principal = {"local", "Local operator", TeamRole::admin, 0};
        if (options.bearer_token.empty() && !options.access_policy) return grpc::Status::OK;
        const auto range = context.client_metadata().equal_range("authorization");
        if (range.first == range.second || std::next(range.first) != range.second) {
            return {grpc::StatusCode::UNAUTHENTICATED, "bearer authorization required"};
        }
        const auto& actual = range.first->second;
        const auto service_credential = actual.size() == expected_authorization.size() &&
            CRYPTO_memcmp(actual.data(), expected_authorization.data(),
                          expected_authorization.size()) == 0;
        if (options.access_policy) {
            if (service_credential && allow_service) {
                principal = {"gateway", "Gateway event delivery", TeamRole::viewer, 0};
            } else {
                const auto resolved = options.access_policy->authenticate(
                    std::string_view(actual.data(), actual.size()), unix_ms());
                if (!resolved)
                    return {grpc::StatusCode::UNAUTHENTICATED,
                            "invalid or expired team credential"};
                principal = *resolved;
            }
        } else if (!service_credential) {
            return {grpc::StatusCode::UNAUTHENTICATED, "invalid bearer authorization"};
        }
        if (!permits(principal.role, permission))
            return {grpc::StatusCode::PERMISSION_DENIED, "team role does not permit this action"};
        return grpc::Status::OK;
    }
    template<class Function> grpc::Status boundary(grpc::ServerContext& context, Function body,
        Permission permission = Permission::read, bool allow_service = false) {
        try {
            Principal principal;
            const auto authorized = authorize(context, principal, permission, allow_service);
            if (!authorized.ok()) return authorized;
            if constexpr (std::is_invocable_v<Function, const Principal&>) body(principal);
            else body();
            return grpc::Status::OK;
        } catch (const DataError& error) {
            return translate_data_error(error);
        } catch (const Error& error) {
            if (error.code() == ErrorCode::unavailable) log_unexpected("RPC storage", error.what());
            return translate_error(error);
        } catch (const std::invalid_argument& error) {
            return {grpc::StatusCode::INVALID_ARGUMENT, error.what()};
        } catch (const std::exception& error) {
            if (context.IsCancelled()) return {grpc::StatusCode::CANCELLED, "RPC cancelled"};
            log_unexpected("RPC internal", error.what());
            return {grpc::StatusCode::INTERNAL, "internal service error"};
        } catch (...) {
            log_unexpected("RPC internal", "unknown exception");
            return {grpc::StatusCode::INTERNAL, "unknown service failure"};
        }
    }
};
WorkspaceService::WorkspaceService(Store& store, Scheduler& scheduler, EventBus& events,
    std::function<std::string()> strategies, ServiceOptions options,
    const DatabaseRegistry* databases, const ResearchBackend* research)
    : impl_(std::make_unique<Impl>(store, scheduler, events, std::move(strategies),
                                  std::move(options), databases, research)) {}
WorkspaceService::~WorkspaceService() = default;
std::uint32_t WorkspaceService::subscriber_count() const noexcept {
    return impl_->subscribers.load();
}
grpc::Status WorkspaceService::GetHealth(grpc::ServerContext* context,
    const v1::HealthRequest*, v1::HealthResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto stats = impl_->store.stats();
        response->set_status(impl_->scheduler.healthy() ? "SERVING" : "DEGRADED");
        response->set_version("1.0.0");
        response->set_uptime_seconds(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - impl_->started).count());
        response->set_queued_jobs(stats.queued);
        response->set_running_jobs(stats.running);
        response->set_cache_hits(stats.cache_hits);
        response->set_cache_misses(stats.cache_misses);
        response->set_cache_entries(stats.cache_entries);
        response->set_cache_bytes(stats.cache_bytes);
        response->set_event_sequence(impl_->events.sequence());
        response->set_worker_count(impl_->scheduler.worker_count());
    }, Permission::read, true);
}
grpc::Status WorkspaceService::ListStrategies(grpc::ServerContext* context,
    const v1::ListStrategiesRequest*, v1::ListStrategiesResponse* response) {
    return impl_->boundary(*context, [&] { response->set_strategies_json(impl_->strategies()); });
}
grpc::Status WorkspaceService::SubmitJob(grpc::ServerContext* context,
    const v1::SubmitJobRequest* request, v1::Job* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        if (request->alpha_id().size() > 32 || request->label().size() > 320)
            throw Error(ErrorCode::invalid, "job metadata exceeds byte limits");
        copy_job(impl_->scheduler.submit(request->request_json(), request->idempotency_key(),
            request->scheduled_at_ms(), {request->alpha_id(), request->alpha_revision(),
                                        request->label(), actor.id}), *response);
    }, Permission::research);
}
grpc::Status WorkspaceService::GetJob(grpc::ServerContext* context,
    const v1::GetJobRequest* request, v1::Job* response) {
    return impl_->boundary(*context, [&] {
        validate_id(request->id());
        copy_job(impl_->store.get(request->id()), *response);
    });
}
grpc::Status WorkspaceService::ListJobs(grpc::ServerContext* context,
    const v1::ListJobsRequest* request, v1::ListJobsResponse* response) {
    return impl_->boundary(*context, [&] {
        auto status = request->status();
        for (auto& ch : status) {
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - ('a' - 'A'));
        }
        const auto page = impl_->store.list(request->limit() == 0 ? 50 : request->limit(),
                                            request->offset(), status);
        response->set_total(page.total);
        for (const auto& job : page.jobs) copy_job(job, *response->add_jobs());
    });
}
grpc::Status WorkspaceService::CancelJob(grpc::ServerContext* context,
    const v1::CancelJobRequest* request, v1::Job* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        validate_id(request->id());
        copy_job(impl_->scheduler.cancel(request->id(), actor.id), *response);
    }, Permission::research);
}

grpc::Status WorkspaceService::ListDataSources(grpc::ServerContext* context,
    const v1::ListDataSourcesRequest*, v1::ListDataSourcesResponse* response) {
    return impl_->boundary(*context, [&] {
        for (const auto& source : impl_->database_registry().sources()) {
            auto* output = response->add_sources();
            output->set_id(source.id);
            output->set_name(source.name);
            output->set_provider(std::string(source.provider->name()));
            output->set_read_only(true);
            const auto capabilities = source.provider->capabilities();
            auto* limits = output->mutable_capabilities();
            limits->set_browse(capabilities.browse);
            limits->set_query(capabilities.query);
            limits->set_max_rows(capabilities.max_rows);
            limits->set_max_offset(capabilities.max_offset);
            limits->set_max_query_bytes(capabilities.max_query_bytes);
            limits->set_timeout_ms(capabilities.timeout_ms);
        }
    });
}
grpc::Status WorkspaceService::GetDataCatalog(grpc::ServerContext* context,
    const v1::GetDataCatalogRequest* request, v1::DataCatalog* response) {
    return impl_->boundary(*context, [&] {
        const auto catalog = impl_->database_registry().catalog(request->source_id(),
                                                               data_context(*context));
        response->set_source_id(request->source_id());
        response->set_truncated(catalog.truncated);
        for (const auto& table : catalog.tables) {
            auto* output = response->add_tables();
            output->set_catalog(table.catalog);
            output->set_schema(table.schema);
            output->set_name(table.name);
            output->set_kind(table.kind);
        }
    });
}
grpc::Status WorkspaceService::PreviewData(grpc::ServerContext* context,
    const v1::PreviewDataRequest* request, v1::DataResult* response) {
    return impl_->boundary(*context, [&] {
        const DataTable table{request->catalog(), request->schema(), request->table(), ""};
        copy_result(impl_->database_registry().preview(request->source_id(), table,
            request->limit() == 0 ? 200 : request->limit(), request->offset(),
            data_context(*context)), *response);
        response->set_source_id(request->source_id());
    });
}
grpc::Status WorkspaceService::QueryData(grpc::ServerContext* context,
    const v1::QueryDataRequest* request, v1::DataResult* response) {
    return impl_->boundary(*context, [&] {
        copy_result(impl_->database_registry().query(request->source_id(), request->sql(),
            request->max_rows() == 0 ? 200 : request->max_rows(), data_context(*context)),
            *response);
        response->set_source_id(request->source_id());
    });
}

grpc::Status WorkspaceService::ListWorkspaces(grpc::ServerContext* context,
    const v1::ListWorkspacesRequest* request, v1::ListWorkspacesResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_workspaces(
            request->limit() == 0 ? 50 : request->limit(), request->offset());
        response->set_total(page.total);
        for (const auto& workspace : page.workspaces) {
            copy_workspace(workspace, *response->add_workspaces());
        }
    });
}
grpc::Status WorkspaceService::GetWorkspace(grpc::ServerContext* context,
    const v1::GetWorkspaceRequest* request, v1::WorkspaceDocument* response) {
    return impl_->boundary(*context, [&] {
        copy_workspace(impl_->store.get_workspace(request->id()), *response);
    });
}
grpc::Status WorkspaceService::CreateWorkspace(grpc::ServerContext* context,
    const v1::CreateWorkspaceRequest* request, v1::WorkspaceDocument* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        const auto workspace = impl_->store.create_workspace(
            request->name(), request->description(), request->document_json(), actor.id);
        impl_->durable_notification("workspace.created", workspace.id, workspace.revision);
        copy_workspace(workspace, *response);
    }, Permission::research);
}
grpc::Status WorkspaceService::UpdateWorkspace(grpc::ServerContext* context,
    const v1::UpdateWorkspaceRequest* request, v1::WorkspaceDocument* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        const auto workspace = impl_->store.update_workspace(
            request->id(), request->expected_revision(),
            request->name(), request->description(), request->document_json(), actor.id);
        impl_->durable_notification("workspace.updated", workspace.id, workspace.revision);
        copy_workspace(workspace, *response);
    }, Permission::research);
}
grpc::Status WorkspaceService::DeleteWorkspace(grpc::ServerContext* context,
    const v1::DeleteWorkspaceRequest* request, v1::DeleteWorkspaceResponse*) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        impl_->store.delete_workspace(request->id(), request->expected_revision(), actor.id);
        impl_->durable_notification("workspace.deleted", request->id(),
                                       request->expected_revision());
    }, Permission::research);
}
grpc::Status WorkspaceService::ListAlphas(grpc::ServerContext* context,
    const v1::ListAlphasRequest* request, v1::ListAlphasResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_alphas(request->limit() == 0 ? 50 : request->limit(),
            request->offset(), request->search(), request->tag(), request->project_id(),
            request->include_archived());
        response->set_total(page.total);
        for (const auto& alpha : page.alphas) copy_alpha(alpha, *response->add_alphas());
    });
}
grpc::Status WorkspaceService::GetAlpha(grpc::ServerContext* context,
    const v1::GetAlphaRequest* request, v1::AlphaSnapshot* response) {
    return impl_->boundary(*context, [&] {
        copy_alpha(impl_->store.get_alpha(request->id(), request->revision()), *response);
    });
}
grpc::Status WorkspaceService::CreateAlpha(grpc::ServerContext* context,
    const v1::CreateAlphaRequest* request, v1::AlphaSnapshot* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        if (!request->project_id().empty() && request->project_id() != "local") {
            throw Error(ErrorCode::invalid, "only the shared local project is available");
        }
        const auto alpha = impl_->store.create_alpha(alpha_input(*request, actor.id));
        impl_->durable_notification("alpha.created", alpha.id, alpha.revision);
        copy_alpha(alpha, *response);
    }, Permission::research);
}
grpc::Status WorkspaceService::UpdateAlpha(grpc::ServerContext* context,
    const v1::UpdateAlphaRequest* request, v1::AlphaSnapshot* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        const auto alpha = impl_->store.update_alpha(request->id(), request->expected_revision(),
                                                     alpha_input(*request, actor.id));
        impl_->durable_notification("alpha.updated", alpha.id, alpha.revision);
        copy_alpha(alpha, *response);
    }, Permission::research);
}
grpc::Status WorkspaceService::ListAlphaRevisions(grpc::ServerContext* context,
    const v1::ListAlphaRevisionsRequest* request, v1::ListAlphaRevisionsResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_alpha_revisions(request->id(),
            request->limit() == 0 ? 50 : request->limit(), request->offset());
        response->set_total(page.total);
        for (const auto& alpha : page.alphas) copy_alpha(alpha, *response->add_revisions());
    });
}
grpc::Status WorkspaceService::SetAlphaArchived(grpc::ServerContext* context,
    const v1::SetAlphaArchivedRequest* request, v1::AlphaSnapshot* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        const auto alpha = impl_->store.set_alpha_archived(request->id(),
            request->expected_revision(), request->archived(), actor.id);
        impl_->durable_notification(request->archived() ? "alpha.archived" : "alpha.restored",
                                    alpha.id, alpha.revision);
        copy_alpha(alpha, *response);
    }, Permission::research);
}
grpc::Status WorkspaceService::CreateExperiment(grpc::ServerContext* context,
    const v1::CreateExperimentRequest* request, v1::Experiment* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        if (request->simulations_size() < 1 || request->simulations_size() > 32)
            throw Error(ErrorCode::invalid, "experiment requires 1..32 simulations");
        if (request->name().size() > 320 || request->description().size() > 512 ||
            request->idempotency_key().size() > 128)
            throw Error(ErrorCode::invalid, "experiment metadata exceeds byte limits");
        ExperimentInput input;
        input.name = request->name();
        input.description = request->description();
        input.idempotency_key = request->idempotency_key();
        input.scheduled_at_ms = request->scheduled_at_ms();
        input.actor_id = actor.id;
        input.simulations.reserve(static_cast<std::size_t>(request->simulations_size()));
        for (const auto& simulation : request->simulations()) {
            if (simulation.label().size() > 320 || simulation.request_json().size() > 65536 ||
                simulation.alpha_id().size() > 32)
                throw Error(ErrorCode::invalid, "simulation exceeds byte limits");
            input.simulations.push_back({simulation.label(), simulation.request_json(),
                                         simulation.alpha_id(), simulation.alpha_revision()});
        }
        copy_experiment(impl_->scheduler.submit_experiment(input), *response);
    }, Permission::research);
}

grpc::Status WorkspaceService::GetExperiment(grpc::ServerContext* context,
    const v1::GetExperimentRequest* request, v1::Experiment* response) {
    return impl_->boundary(*context, [&] {
        copy_experiment(impl_->store.get_experiment(request->id()), *response);
    });
}

grpc::Status WorkspaceService::ListExperiments(grpc::ServerContext* context,
    const v1::ListExperimentsRequest* request, v1::ListExperimentsResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_experiments(
            request->limit() == 0 ? 50 : request->limit(), request->offset());
        response->set_total(page.total);
        for (const auto& experiment : page.experiments)
            copy_experiment(experiment, *response->add_experiments());
    });
}

grpc::Status WorkspaceService::CancelExperiment(grpc::ServerContext* context,
    const v1::CancelExperimentRequest* request, v1::Experiment* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        copy_experiment(impl_->scheduler.cancel_experiment(request->id(), actor.id), *response);
    }, Permission::research);
}

grpc::Status WorkspaceService::CreateWorkflow(grpc::ServerContext* context,
    const v1::CreateWorkflowRequest* request, v1::Workflow* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        if (request->name().size() > 320 || request->description().size() > 512 ||
            request->request_json().size() > 65536 || request->alpha_id().size() > 32 ||
            request->idempotency_key().size() > 128)
            throw Error(ErrorCode::invalid, "workflow input exceeds byte limits");
        WorkflowInput input;
        input.name = request->name();
        input.description = request->description();
        input.request_json = request->request_json();
        input.alpha_id = request->alpha_id();
        input.alpha_revision = request->alpha_revision();
        input.interval_ms = request->interval_ms();
        input.max_runs = request->max_runs();
        input.first_run_at_ms = request->first_run_at_ms();
        input.idempotency_key = request->idempotency_key();
        input.actor_id = actor.id;
        copy_workflow(impl_->scheduler.create_workflow(input), *response);
    }, Permission::research);
}

grpc::Status WorkspaceService::GetWorkflow(grpc::ServerContext* context,
    const v1::GetWorkflowRequest* request, v1::Workflow* response) {
    return impl_->boundary(*context, [&] {
        copy_workflow(impl_->store.get_workflow(request->id()), *response);
    });
}

grpc::Status WorkspaceService::ListWorkflows(grpc::ServerContext* context,
    const v1::ListWorkflowsRequest* request, v1::ListWorkflowsResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_workflows(
            request->limit() == 0 ? 50 : request->limit(), request->offset());
        response->set_total(page.total);
        for (const auto& workflow : page.workflows)
            copy_workflow(workflow, *response->add_workflows());
    });
}

grpc::Status WorkspaceService::SetWorkflowPaused(grpc::ServerContext* context,
    const v1::SetWorkflowPausedRequest* request, v1::Workflow* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        copy_workflow(impl_->scheduler.set_workflow_paused(request->id(),
            request->expected_revision(), request->paused(), actor.id), *response);
    }, Permission::research);
}

grpc::Status WorkspaceService::ListWorkflowRuns(grpc::ServerContext* context,
    const v1::ListWorkflowRunsRequest* request, v1::ListWorkflowRunsResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_workflow_runs(request->id(),
            request->limit() == 0 ? 50 : request->limit(), request->offset());
        response->set_total(page.total);
        for (const auto& job : page.jobs) copy_job(job, *response->add_jobs());
    });
}

grpc::Status WorkspaceService::GetSession(grpc::ServerContext* context,
    const v1::SessionRequest*, v1::SessionResponse* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        copy_principal(actor, *response->mutable_principal());
        const auto& policy = impl_->options.access_policy;
        response->set_team_mode(static_cast<bool>(policy));
        response->set_team_name(policy ? std::string(policy->team_name()) : "Local workspace");
        response->set_project_id("local");
    });
}

grpc::Status WorkspaceService::ListTeam(grpc::ServerContext* context,
    const v1::ListTeamRequest*, v1::ListTeamResponse* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        const auto& policy = impl_->options.access_policy;
        response->set_team_mode(static_cast<bool>(policy));
        response->set_team_name(policy ? std::string(policy->team_name()) : "Local workspace");
        response->set_project_id("local");
        if (policy) {
            for (const auto& principal : policy->principals())
                copy_principal(principal, *response->add_principals());
        } else {
            copy_principal(actor, *response->add_principals());
        }
    });
}

grpc::Status WorkspaceService::ListAudit(grpc::ServerContext* context,
    const v1::ListAuditRequest* request, v1::ListAuditResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_audit(request->limit() == 0 ? 50 : request->limit(),
            request->offset(), request->entity_type(), request->entity_id(), request->actor_id());
        response->set_total(page.total);
        for (const auto& record : page.records) {
            auto* output = response->add_records();
            output->set_sequence(record.sequence);
            output->set_entity_type(record.entity_type);
            output->set_entity_id(record.entity_id);
            output->set_action(record.action);
            output->set_actor_id(record.actor_id);
            output->set_revision(record.revision);
            output->set_metadata_json(record.metadata_json);
            output->set_created_at_ms(record.created_at_ms);
        }
    });
}

grpc::Status WorkspaceService::SubmitReview(grpc::ServerContext* context,
    const v1::SubmitReviewRequest* request, v1::ReviewRecord* response) {
    return impl_->boundary(*context, [&](const Principal& actor) {
        if (request->job_ids_size() < 1 || request->job_ids_size() > 12 ||
            request->note().size() > 4096 || request->idempotency_key().size() > 128)
            throw Error(ErrorCode::invalid, "review input exceeds evidence or byte limits");
        validate_id(request->alpha_id());
        ReviewInput input;
        input.alpha_id = request->alpha_id();
        input.alpha_revision = request->alpha_revision();
        input.note = request->note();
        input.actor_id = actor.id;
        input.idempotency_key = request->idempotency_key();
        input.job_ids.reserve(static_cast<std::size_t>(request->job_ids_size()));
        for (const auto& id : request->job_ids()) {
            validate_id(id);
            input.job_ids.push_back(id);
        }
        const auto review = impl_->store.submit_review(input);
        impl_->durable_notification("review.submitted", review.id, review.revision);
        copy_review(review, *response);
    }, Permission::research);
}

grpc::Status WorkspaceService::GetReview(grpc::ServerContext* context,
    const v1::GetReviewRequest* request, v1::ReviewRecord* response) {
    return impl_->boundary(*context, [&] {
        copy_review(impl_->store.get_review(request->id()), *response);
    });
}

grpc::Status WorkspaceService::ListReviews(grpc::ServerContext* context,
    const v1::ListReviewsRequest* request, v1::ListReviewsResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_reviews(request->limit() == 0 ? 50 : request->limit(),
            request->offset(), request->alpha_id(), request->state());
        response->set_total(page.total);
        for (const auto& review : page.reviews) copy_review(review, *response->add_reviews());
    });
}

grpc::Status WorkspaceService::DecideReview(grpc::ServerContext* context,
    const v1::DecideReviewRequest* request, v1::ReviewRecord* response) {
    const auto permission = request->state() == "withdrawn" ? Permission::research
                                                           : Permission::review;
    return impl_->boundary(*context, [&](const Principal& actor) {
        validate_id(request->id());
        if (request->rationale().size() > 4096 || request->state().size() > 16)
            throw Error(ErrorCode::invalid, "review decision exceeds byte limits");
        const auto review = impl_->store.decide_review(request->id(), request->expected_revision(),
            request->state(), request->rationale(), actor.id,
            static_cast<bool>(impl_->options.access_policy), actor.role == TeamRole::admin);
        impl_->durable_notification("review.decided", review.id, review.revision);
        copy_review(review, *response);
    }, permission);
}

grpc::Status WorkspaceService::ListReviewDecisions(grpc::ServerContext* context,
    const v1::ListReviewDecisionsRequest* request, v1::ListReviewDecisionsResponse* response) {
    return impl_->boundary(*context, [&] {
        const auto page = impl_->store.list_review_decisions(request->id(),
            request->limit() == 0 ? 50 : request->limit(), request->offset());
        response->set_total(page.total);
        for (const auto& decision : page.decisions) {
            auto* output = response->add_decisions();
            output->set_id(decision.id);
            output->set_review_id(decision.review_id);
            output->set_revision(decision.revision);
            output->set_state(decision.state);
            output->set_actor_id(decision.actor_id);
            output->set_rationale(decision.rationale);
            output->set_created_at_ms(decision.created_at_ms);
        }
    });
}

grpc::Status WorkspaceService::CompareJobs(grpc::ServerContext* context,
    const v1::CompareJobsRequest* request, v1::CompareJobsResponse* response) {
    auto outcome = grpc::Status::OK;
    const auto status = impl_->boundary(*context, [&] {
        if (request->job_ids_size() < 2 || request->job_ids_size() > 12)
            throw Error(ErrorCode::invalid, "comparison requires 2..12 distinct completed runs");
        std::set<std::string> ids;
        for (const auto& id : request->job_ids()) {
            validate_id(id);
            if (!ids.insert(id).second) throw Error(ErrorCode::invalid, "duplicate comparison run");
        }
        const CounterLease admission(impl_->comparisons);
        if (admission.previous() >= 2)
            throw Error(ErrorCode::capacity, "comparison capacity is in use");
        std::vector<ComparisonInput> inputs;
        std::size_t bytes = 0;
        for (const auto& id : request->job_ids()) {
            auto job = impl_->store.get(id);
            if (job.status != "SUCCEEDED") {
                outcome = {grpc::StatusCode::FAILED_PRECONDITION,
                           "comparison requires completed successful runs"};
                return;
            }
            if (job.result_json.size() > 16U * 1024U * 1024U - bytes)
                throw Error(ErrorCode::invalid, "comparison input budget exceeds 16 MiB");
            bytes += job.result_json.size();
            inputs.push_back({job.id, job.label, std::move(job.result_json)});
        }
        ComparisonOptions options;
        options.start_date = request->start_date();
        options.end_date = request->end_date();
        options.max_chart_points = request->max_chart_points() == 0
            ? 1000 : request->max_chart_points();
        options.allow_mixed_evidence = request->allow_mixed_evidence();
        options.is_cancelled = [context] { return context->IsCancelled(); };
        response->set_comparison_json(compare_research_results(inputs, options));
    });
    return status.ok() ? outcome : status;
}

grpc::Status WorkspaceService::ValidateExpression(grpc::ServerContext* context,
    const v1::ValidateExpressionRequest* request, v1::ValidateExpressionResponse* response) {
    auto outcome = grpc::Status::OK;
    const auto status = impl_->boundary(*context, [&] {
        if (impl_->research == nullptr) {
            outcome = {grpc::StatusCode::FAILED_PRECONDITION,
                       "research expression validation is not configured"};
            return;
        }
        response->set_analysis_json(impl_->research->validate_expression_json(
            request->strategy_dsl(), request->dataset_id(), request->dataset_sha256()));
    });
    return status.ok() ? outcome : status;
}

grpc::Status WorkspaceService::WatchEvents(grpc::ServerContext* context,
    const v1::WatchEventsRequest* request, grpc::ServerWriter<v1::Event>* writer) {
    try {
        Principal actor;
        const auto authorized = impl_->authorize(*context, actor, Permission::read, true);
        if (!authorized.ok()) return authorized;
        const std::string_view channel = request->channel().empty()
            ? std::string_view{"all"} : std::string_view{request->channel()};
        if (channel != "jobs" && channel != "system" && channel != "all") {
            return {grpc::StatusCode::INVALID_ARGUMENT, "invalid event channel"};
        }
        if (!request->job_id().empty()) validate_id(request->job_id());
        const CounterLease subscription(impl_->subscribers);
        if (subscription.previous() >= impl_->options.max_subscribers) {
            return {grpc::StatusCode::RESOURCE_EXHAUSTED, "too many event subscribers"};
        }
        auto cursor = request->after_sequence();
        for (; !context->IsCancelled() && !impl_->events.closed();) {
            if (actor.expires_at_ms > 0 && unix_ms() >= actor.expires_at_ms)
                return {grpc::StatusCode::UNAUTHENTICATED, "team credential expired"};
            const auto observed = impl_->events.sequence();
            const auto batch = impl_->events.read(cursor, channel, request->job_id());
            for (const auto& event : batch) {
                v1::Event response;
                response.set_sequence(event.sequence);
                response.set_channel(event.channel);
                response.set_type(event.type);
                response.set_job_id(event.job_id);
                response.set_payload_json(event.payload_json);
                response.set_created_at_ms(event.created_at_ms);
                response.set_gap(event.gap);
                if (!writer->Write(response)) return {grpc::StatusCode::CANCELLED, "stream closed"};
                cursor = event.sequence;
            }
            // Advance over filtered events too, avoiding busy waits on another channel.
            cursor = std::max(cursor, observed);
            impl_->events.wait(cursor, std::chrono::milliseconds(100));
        }
        return context->IsCancelled()
            ? grpc::Status(grpc::StatusCode::CANCELLED, "stream cancelled") : grpc::Status::OK;
    } catch (const Error& error) {
        return translate_error(error);
    } catch (const std::exception& error) {
        log_unexpected("event stream", error.what());
        return {grpc::StatusCode::INTERNAL, "event stream failed"};
    } catch (...) {
        log_unexpected("event stream", "unknown exception");
        return {grpc::StatusCode::INTERNAL, "event stream failed"};
    }
}
} // namespace atx::server
