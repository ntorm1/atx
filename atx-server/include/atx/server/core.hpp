#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace atx::server {
enum class ErrorCode { invalid, missing, conflict, capacity, unavailable };
class Error final : public std::runtime_error {
public:
    Error(ErrorCode code, std::string message);
    [[nodiscard]] ErrorCode code() const noexcept;
private:
    ErrorCode code_;
};

struct Job {
    std::string id;
    std::string status;
    std::string request_json;
    std::string result_json;
    std::string error;
    double progress{};
    std::int64_t created_at_ms{};
    std::int64_t scheduled_at_ms{};
    std::int64_t started_at_ms{};
    std::int64_t finished_at_ms{};
    std::string label;
    std::string alpha_id{};
    std::uint64_t alpha_revision{};
    std::string experiment_id{};
    std::string actor_id{"migration_unknown"};
    std::string workflow_id{};
    std::int64_t workflow_slot_ms{};
};
struct JobPage { std::vector<Job> jobs; std::uint64_t total{}; };
struct JobContext {
    std::string alpha_id;
    std::uint64_t alpha_revision{};
    std::string label;
    std::string actor_id{"local"};
};
struct SimulationInput {
    std::string label;
    std::string request_json;
    std::string alpha_id;
    std::uint64_t alpha_revision{};
};
struct ExperimentInput {
    std::string name;
    std::string description;
    std::vector<SimulationInput> simulations;
    std::string idempotency_key;
    std::int64_t scheduled_at_ms{};
    std::string actor_id{"local"};
};
struct Experiment {
    std::string id;
    std::string name;
    std::string description;
    std::int64_t created_at_ms{};
    std::int64_t scheduled_at_ms{};
    std::string status;
    double progress{};
    std::uint32_t total{}, queued{}, running{}, succeeded{}, failed{}, cancelled{};
    // Child summaries preserve member order and omit request/result JSON.
    std::vector<Job> jobs;
    std::string actor_id{"migration_unknown"};
};
struct ExperimentPage { std::vector<Experiment> experiments; std::uint64_t total{}; };
struct WorkspaceDocument {
    std::string id;
    std::string name;
    std::string description;
    std::string document_json;
    std::uint64_t revision{};
    std::int64_t created_at_ms{};
    std::int64_t updated_at_ms{};
    std::string actor_id{"migration_unknown"};
};
struct WorkspacePage { std::vector<WorkspaceDocument> workspaces; std::uint64_t total{}; };
struct AlphaInput {
    std::string name;
    std::string description;
    std::vector<std::string> tags;
    std::string hypothesis;
    std::string request_json;
    std::string source_job_id;
    std::string project_id;
    std::string actor_id{"local"};
};
struct AlphaSnapshot {
    std::string id;
    std::string project_id;
    std::string name;
    std::string description;
    std::string tags_json;
    std::string stage;
    std::uint64_t revision{};
    std::string hypothesis;
    std::string request_json;
    std::string request_sha256;
    std::string source_job_id;
    std::int64_t created_at_ms{};
    std::int64_t updated_at_ms{};
    std::string actor_id{"migration_unknown"};
};
struct AlphaPage { std::vector<AlphaSnapshot> alphas; std::uint64_t total{}; };
struct AuditRecord {
    std::uint64_t sequence{};
    std::string entity_type, entity_id, action, actor_id;
    std::uint64_t revision{};
    std::string metadata_json;
    std::int64_t created_at_ms{};
};
struct AuditPage { std::vector<AuditRecord> records; std::uint64_t total{}; };
struct ReviewInput {
    std::string alpha_id;
    std::uint64_t alpha_revision{};
    std::vector<std::string> job_ids;
    std::string note;
    std::string actor_id{"local"};
    std::string idempotency_key;
};
struct ReviewRecord {
    std::string id, alpha_id;
    std::uint64_t alpha_revision{};
    std::string request_sha256, requester_actor_id, author_actor_id, state;
    std::uint64_t revision{};
    std::string note, evidence_json;
    std::int64_t created_at_ms{}, updated_at_ms{};
    bool current{};
};
struct ReviewPage { std::vector<ReviewRecord> reviews; std::uint64_t total{}; };
struct ReviewDecision {
    std::string id, review_id;
    std::uint64_t revision{};
    std::string state, actor_id, rationale;
    std::int64_t created_at_ms{};
};
struct ReviewDecisionPage { std::vector<ReviewDecision> decisions; std::uint64_t total{}; };
struct WorkflowInput {
    std::string name, description, request_json, alpha_id;
    std::uint64_t alpha_revision{};
    std::uint64_t interval_ms{};
    std::uint32_t max_runs{};
    std::int64_t first_run_at_ms{};
    std::string idempotency_key;
    std::string actor_id{"local"};
};
struct Workflow {
    std::string id, name, description, request_json, request_sha256, alpha_id;
    std::uint64_t alpha_revision{};
    std::string actor_id;
    std::uint64_t interval_ms{};
    std::uint32_t max_runs{};
    std::int64_t first_run_at_ms{}, next_run_at_ms{};
    std::uint64_t revision{};
    bool enabled{};
    std::string state, pause_reason, last_dispatch_error;
    std::int64_t created_at_ms{}, updated_at_ms{};
    std::uint64_t admitted_runs{}, queued{}, running{}, succeeded{}, failed{}, cancelled{};
    std::uint64_t missed_slots{}, skipped_slots{};
    std::string active_job_id, last_job_id;
};
struct WorkflowPage { std::vector<Workflow> workflows; std::uint64_t total{}; };
struct WorkflowRunPage { std::vector<Job> jobs; std::uint64_t total{}; };
struct WorkflowDispatch { std::optional<Job> job; bool changed{}; };
struct StoreStats {
    std::uint64_t queued{};
    std::uint64_t running{};
    std::uint64_t cache_hits{};
    std::uint64_t cache_misses{};
    std::uint64_t cache_entries{};
    std::uint64_t cache_bytes{};
};
struct CacheOptions {
    std::size_t entries{512};
    std::size_t bytes{32 * 1024 * 1024};
    std::chrono::milliseconds ttl{2000};
};

// Thread-safe durable job store. All database and cache operations share one mutex.
// Mutations invalidate cached snapshots before releasing that mutex.
class Store final {
public:
    // Injected clock must be thread-safe; it is invoked outside store locks.
    explicit Store(std::string_view path, CacheOptions cache = {},
                   std::function<std::int64_t()> clock = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;
    Store(Store&&) = delete;
    Store& operator=(Store&&) = delete;
    [[nodiscard]] Job create(std::string_view request, std::string_view key,
                            std::int64_t scheduled_at_ms, std::size_t capacity);
    [[nodiscard]] Job create(std::string_view request, std::string_view key,
        std::int64_t scheduled_at_ms, std::size_t capacity, const JobContext& context);
    [[nodiscard]] Job get(std::string_view id);
    [[nodiscard]] JobPage list(std::uint32_t limit, std::uint32_t offset,
                               std::string_view status);
    [[nodiscard]] std::optional<Job> claim_due();
    [[nodiscard]] Job cancel(std::string_view id);
    [[nodiscard]] Job cancel(std::string_view id, std::string_view actor_id);
    void progress(std::string_view id, double value);
    void finish(std::string_view id, std::string_view status,
                std::string_view result, std::string_view error);
    // Running work cannot be replayed safely after process interruption.
    void recover();
    // Atomically reserves persistent sequence IDs; returns the previous high-water mark.
    [[nodiscard]] std::uint64_t reserve_event_sequences(std::uint64_t count);
    [[nodiscard]] StoreStats stats();
    [[nodiscard]] WorkspaceDocument create_workspace(std::string_view name,
        std::string_view description, std::string_view document_json);
    [[nodiscard]] WorkspaceDocument create_workspace(std::string_view name,
        std::string_view description, std::string_view document_json, std::string_view actor_id);
    [[nodiscard]] WorkspaceDocument get_workspace(std::string_view id);
    // List summaries deliberately omit the heavy document JSON.
    [[nodiscard]] WorkspacePage list_workspaces(std::uint32_t limit, std::uint32_t offset);
    [[nodiscard]] WorkspaceDocument update_workspace(std::string_view id,
        std::uint64_t expected_revision, std::string_view name, std::string_view description,
        std::string_view document_json);
    [[nodiscard]] WorkspaceDocument update_workspace(std::string_view id,
        std::uint64_t expected_revision, std::string_view name, std::string_view description,
        std::string_view document_json, std::string_view actor_id);
    void delete_workspace(std::string_view id, std::uint64_t expected_revision);
    void delete_workspace(std::string_view id, std::uint64_t expected_revision,
                          std::string_view actor_id);
    [[nodiscard]] AlphaSnapshot create_alpha(const AlphaInput& input);
    [[nodiscard]] AlphaSnapshot update_alpha(std::string_view id,
        std::uint64_t expected_revision, const AlphaInput& input);
    [[nodiscard]] AlphaSnapshot get_alpha(std::string_view id, std::uint64_t revision = 0);
    [[nodiscard]] AlphaPage list_alphas(std::uint32_t limit, std::uint32_t offset,
        std::string_view search = {}, std::string_view tag = {}, std::string_view project_id = {},
        bool include_archived = false);
    [[nodiscard]] AlphaPage list_alpha_revisions(std::string_view id,
        std::uint32_t limit, std::uint32_t offset);
    [[nodiscard]] AlphaSnapshot set_alpha_archived(std::string_view id,
        std::uint64_t expected_revision, bool archived);
    [[nodiscard]] AlphaSnapshot set_alpha_archived(std::string_view id,
        std::uint64_t expected_revision, bool archived, std::string_view actor_id);
    [[nodiscard]] Experiment create_experiment(const ExperimentInput& input,
                                               std::size_t capacity);
    [[nodiscard]] Experiment get_experiment(std::string_view id);
    [[nodiscard]] ExperimentPage list_experiments(std::uint32_t limit, std::uint32_t offset);
    [[nodiscard]] Experiment cancel_experiment(std::string_view id);
    [[nodiscard]] Experiment cancel_experiment(std::string_view id, std::string_view actor_id);
    [[nodiscard]] AuditPage list_audit(std::uint32_t limit, std::uint32_t offset,
        std::string_view entity_type = {}, std::string_view entity_id = {},
        std::string_view actor_id = {});
    [[nodiscard]] ReviewRecord submit_review(const ReviewInput& input);
    [[nodiscard]] ReviewRecord get_review(std::string_view id);
    [[nodiscard]] ReviewPage list_reviews(std::uint32_t limit, std::uint32_t offset,
        std::string_view alpha_id = {}, std::string_view state = {});
    [[nodiscard]] ReviewRecord decide_review(std::string_view id, std::uint64_t expected_revision,
        std::string_view state, std::string_view rationale, std::string_view actor_id = "local",
        bool require_independent_reviewer = false, bool allow_withdraw_any = false);
    [[nodiscard]] ReviewDecisionPage list_review_decisions(std::string_view id,
        std::uint32_t limit, std::uint32_t offset);
    [[nodiscard]] Workflow create_workflow(const WorkflowInput& input);
    [[nodiscard]] Workflow get_workflow(std::string_view id);
    [[nodiscard]] WorkflowPage list_workflows(std::uint32_t limit, std::uint32_t offset);
    [[nodiscard]] Workflow set_workflow_paused(std::string_view id,
        std::uint64_t expected_revision, bool paused, std::string_view actor_id = "local");
    [[nodiscard]] WorkflowRunPage list_workflow_runs(std::string_view id,
        std::uint32_t limit, std::uint32_t offset);
    // Internal scheduling interface; callers authorize outside all store/scheduler locks.
    [[nodiscard]] std::vector<Workflow> due_workflows(std::int64_t now_ms,
                                                    std::uint32_t limit = 256);
    [[nodiscard]] WorkflowDispatch dispatch_workflow(std::string_view id,
        std::uint64_t observed_revision, bool authorized, std::int64_t now_ms,
        std::size_t capacity);
    [[nodiscard]] std::int64_t current_time_ms() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct Event {
    std::uint64_t sequence{};
    std::string channel;
    std::string type;
    std::string job_id;
    std::string payload_json;
    std::int64_t created_at_ms{};
    bool gap{};
};
// Bounded replay. Durable sequence allocation prevents cursor overlap after restart.
class EventBus final {
public:
    // A non-null durable store must outlive this bus. Lock order: event bus, then store.
    explicit EventBus(std::size_t capacity = 2048, Store* durable_sequence_store = nullptr);
    ~EventBus();
    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;
    void publish(std::string channel, std::string type, std::string job_id,
                 std::string payload_json);
    [[nodiscard]] std::vector<Event> read(std::uint64_t after, std::string_view channel,
                                        std::string_view job_id) const;
    void wait(std::uint64_t after, std::chrono::milliseconds timeout) const;
    void close();
    [[nodiscard]] bool closed() const;
    [[nodiscard]] std::uint64_t sequence() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

using Progress = std::function<void(double, std::string_view)>;
using Execute = std::function<std::string(std::string_view, std::stop_token, const Progress&)>;
// Immutable operator policy lookup, called outside every store/scheduler lock.
using WorkflowAuthorize = std::function<bool(std::string_view actor_id, std::int64_t now_ms)>;
// Owns joined workers. Executor must cooperate with stop_token for bounded shutdown.
class Scheduler final {
public:
    Scheduler(Store& store, EventBus& events, Execute execute,
              std::uint32_t workers = 2, std::size_t capacity = 1024, bool autostart = true,
              WorkflowAuthorize workflow_authorize = {});
    ~Scheduler();
    Scheduler(const Scheduler&) = delete;
    Scheduler& operator=(const Scheduler&) = delete;
    Scheduler(Scheduler&&) = delete;
    Scheduler& operator=(Scheduler&&) = delete;
    [[nodiscard]] Job submit(std::string_view request, std::string_view key,
                             std::int64_t scheduled_at_ms);
    [[nodiscard]] Job submit(std::string_view request, std::string_view key,
        std::int64_t scheduled_at_ms, const JobContext& context);
    [[nodiscard]] Experiment submit_experiment(const ExperimentInput& input);
    [[nodiscard]] Experiment cancel_experiment(std::string_view id);
    [[nodiscard]] Experiment cancel_experiment(std::string_view id, std::string_view actor_id);
    [[nodiscard]] Workflow create_workflow(const WorkflowInput& input);
    [[nodiscard]] Workflow set_workflow_paused(std::string_view id,
        std::uint64_t expected_revision, bool paused, std::string_view actor_id = "local");
    [[nodiscard]] Job cancel(std::string_view id);
    [[nodiscard]] Job cancel(std::string_view id, std::string_view actor_id);
    // Starts deferred workers. Call from the lifetime owner after listener binding succeeds.
    void start();
    // Joins executors. Call from the lifetime owner, never from an executor callback.
    void shutdown();
    [[nodiscard]] std::uint32_t worker_count() const noexcept;
    [[nodiscard]] bool healthy() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
[[nodiscard]] std::int64_t unix_ms();
} // namespace atx::server
