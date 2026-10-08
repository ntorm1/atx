#include <atx/server/core.hpp>
#include "workspace_validation.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace atx::server {
namespace {
std::string job_payload(const Job& job) {
    auto status = job.status;
    for (auto& ch : status) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
    }
    return nlohmann::json{{"id", job.id}, {"status", status}, {"progress", job.progress},
        {"label", job.label}, {"alpha_id", job.alpha_id},
        {"alpha_revision", std::to_string(job.alpha_revision)},
        {"experiment_id", job.experiment_id}, {"workflow_id", job.workflow_id},
        {"workflow_slot_ms", std::to_string(job.workflow_slot_ms)}}.dump();
}
void log_notification_failure() noexcept {
    static std::atomic<std::int64_t> previous_ms{std::numeric_limits<std::int64_t>::min()};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    auto previous = previous_ms.load();
    if (previous > now - 1000 || !previous_ms.compare_exchange_strong(previous, now)) return;
    static_cast<void>(std::fprintf(stderr, "atx-server: event notification unavailable\n"));
}
std::string validated_request(std::string_view request) {
    const auto json = workspace_detail::parse_json(request, 65536);
    if (!json.is_object()) throw Error(ErrorCode::invalid, "request JSON must be an object");
    auto canonical = json.dump();
    if (canonical.size() > 65536) throw Error(ErrorCode::invalid, "canonical recipe exceeds limit");
    return canonical;
}
void validate_schedule(std::string_view key, std::int64_t scheduled) {
    if (key.size() > 128 || scheduled < 0 ||
        scheduled > unix_ms() + 365LL * 24 * 60 * 60 * 1000) {
        throw Error(ErrorCode::invalid, "invalid idempotency key or scheduling horizon");
    }
}
} // namespace

struct Scheduler::Impl {
    Store& store;
    EventBus& events;
    Execute execute;
    const WorkflowAuthorize workflow_authorize;
    const std::uint32_t count;
    const std::size_t capacity;
    // Lock order: scheduler then store. Executor/authority/stop callbacks run outside both.
    std::mutex mutex;
    std::condition_variable_any changed;
    std::unordered_map<std::string, std::stop_source> running;
    std::uint64_t generation{};
    bool started{};
    bool stopping{};
    bool shutdown_called{};
    std::atomic<bool> failed{};
    std::vector<std::jthread> workers;
    std::jthread recurring;

    Impl(Store& jobs, EventBus& channels, Execute executor,
         std::uint32_t worker_count, std::size_t queue_capacity, bool autostart,
         WorkflowAuthorize authorize)
        : store(jobs), events(channels), execute(std::move(executor)),
          workflow_authorize(authorize ? std::move(authorize) :
              WorkflowAuthorize{[](std::string_view actor, std::int64_t) {
                  return actor == "local";
              }}),
          count(worker_count), capacity(queue_capacity) {
        if (!execute || count == 0 || count > 64 || capacity == 0 || capacity > 100000) {
            throw Error(ErrorCode::invalid, "invalid scheduler worker or capacity configuration");
        }
        store.recover();
        workers.reserve(count);
        if (autostart) start();
    }
    void start() {
        std::lock_guard lock(mutex);
        if (stopping) throw Error(ErrorCode::unavailable, "scheduler is stopped");
        if (started) return;
        try {
            for (std::uint32_t i = 0; i < count; ++i) {
                workers.emplace_back([this](std::stop_token stop) { worker(stop); });
            }
            recurring = std::jthread([this](std::stop_token stop) { recurrence_loop(stop); });
        } catch (...) {
            stopping = true;
            failed.store(true);
            changed.notify_all();
            throw;
        }
        started = true;
    }
    template<class Function> void notify(Function body) noexcept {
        try { body(); }
        catch (...) {
            // Durable mutations already committed; telemetry failure cannot change their outcome.
            log_notification_failure();
        }
    }
    void notify_job(std::string_view type, const Job& job) noexcept {
        notify([&] { events.publish("jobs", std::string(type), job.id, job_payload(job)); });
    }
    void notify_experiment(std::string_view type, const Experiment& experiment) noexcept {
        notify([&] {
            auto status = experiment.status;
            for (auto& ch : status) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch + ('a' - 'A'));
            }
            events.publish("system", std::string(type), "",
                nlohmann::json{{"id", experiment.id}, {"status", status},
                    {"total", experiment.total}, {"progress", experiment.progress}}.dump());
        });
    }
    void notify_workflow(std::string_view type, const Workflow& workflow) noexcept {
        notify([&] {
            events.publish("system", std::string(type), "", nlohmann::json{
                {"id", workflow.id}, {"state", workflow.state},
                {"revision", std::to_string(workflow.revision)},
                {"admitted_runs", std::to_string(workflow.admitted_runs)},
                {"pause_reason", workflow.pause_reason},
                {"last_dispatch_error", workflow.last_dispatch_error}}.dump());
        });
    }
    void recurring_tick(std::stop_token stop) {
        const auto due = store.due_workflows(store.current_time_ms());
        // At most 256 immutable definitions. Each gets at most one admission per scan.
        for (const auto& workflow : due) {
            if (stop.stop_requested()) return;
            const auto now = store.current_time_ms();
            bool authorized{};
            try { authorized = workflow_authorize(workflow.actor_id, now); }
            catch (...) {
                // Operator authority lookup failed: fail closed by durably pausing the creator.
                log_notification_failure();
            }
            WorkflowDispatch dispatched;
            {
                std::lock_guard lock(mutex);
                if (stopping || failed.load()) return;
                // A cancelled job can still be executing until its stop token is observed.
                // Keep its next slot pending until the executor has actually left run().
                if (authorized && workflow.active_job_id.empty() &&
                    !workflow.last_job_id.empty() && running.contains(workflow.last_job_id)) {
                    continue;
                }
                dispatched = store.dispatch_workflow(workflow.id, workflow.revision,
                    authorized, now, capacity);
                if (dispatched.changed) ++generation;
            }
            if (dispatched.changed) {
                changed.notify_all();
                notify([&] {
                    notify_workflow("workflow.changed", store.get_workflow(workflow.id));
                });
            }
            if (dispatched.job) notify_job("job.submitted", *dispatched.job);
        }
    }
    void recurrence_loop(std::stop_token stop) noexcept {
        try {
            // Stop-aware joined timer; immutable-policy callbacks run outside every lock.
            for (; !stop.stop_requested();) {
                {
                    std::lock_guard lock(mutex);
                    if (stopping || !started || failed.load()) return;
                }
                recurring_tick(stop);
                std::unique_lock lock(mutex);
                const auto observed = generation;
                changed.wait_for(lock, stop, std::chrono::milliseconds(500),
                    [&] { return stopping || generation != observed; });
            }
        } catch (const std::exception& exception) {
            failed.store(true);
            static_cast<void>(std::fprintf(stderr,
                "atx-server recurrence infrastructure: %.2048s\n", exception.what()));
        } catch (...) {
            failed.store(true);
            static_cast<void>(std::fprintf(stderr,
                "atx-server recurrence infrastructure: unknown exception\n"));
        }
    }
    void run(const Job& job, std::stop_token stop) {
        notify_job("job.started", job);
        std::string result;
        std::string error;
        auto last_progress = std::chrono::steady_clock::time_point{};
        try {
            result = execute(job.request_json, stop, [&](double value, std::string_view message) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last_progress < std::chrono::milliseconds(100) && value < 1) return;
                last_progress = now;
                store.progress(job.id, value);
                notify([&] { events.publish("jobs", "job.progress", job.id,
                    nlohmann::json{{"id", job.id}, {"progress", value},
                                   {"message", message.substr(0, 512)}}.dump()); });
            });
            if (result.empty()) error = "Executor returned an empty result";
            else if (result.size() > 16 * 1024 * 1024) error = "Executor result exceeds 16 MiB";
        } catch (const std::exception& exception) {
            error = std::string(exception.what()).substr(0, 2048);
        } catch (...) {
            error = "Executor failed with an unknown exception";
        }
        {
            std::lock_guard lock(mutex);
            if (stop.stop_requested() && error.empty()) error = "Execution interrupted";
            store.finish(job.id, error.empty() ? "SUCCEEDED" : "FAILED",
                         error.empty() ? result : "", error);
            running.erase(job.id);
            ++generation;
        }
        changed.notify_all();
        const auto completed = store.get(job.id);
        notify_job("job.finished", completed);
    }
    void worker(std::stop_token stop) noexcept {
        try {
            // Lifecycle-bounded loop: jthread stop + predicate wake guarantees exit.
            for (; !stop.stop_requested();) {
                std::optional<Job> job;
                std::stop_source execution_source;
                {
                    std::unique_lock lock(mutex);
                    if (stopping || !started) return;
                    job = store.claim_due();
                    if (!job) {
                        const auto observed = generation;
                        changed.wait_for(lock, stop, std::chrono::milliseconds(100),
                                         [&] { return stopping || generation != observed; });
                        continue;
                    }
                    execution_source = running.emplace(job->id, std::stop_source{}).first->second;
                }
                // Also links jthread destruction during partial construction to live execution.
                std::stop_callback forward_stop(stop, [execution_source]() mutable {
                    execution_source.request_stop();
                });
                run(*job, execution_source.get_token());
            }
        } catch (const std::exception& exception) {
            failed.store(true);
            // Best-effort fixed-size logging; worker exits after one infrastructure failure.
            static_cast<void>(std::fprintf(stderr,
                "atx-server worker infrastructure: %.2048s\n", exception.what()));
            try {
                events.publish("system", "scheduler.error", "",
                    nlohmann::json{
                        {"error", std::string(exception.what()).substr(0, 1024)}}.dump());
            } catch (...) {
                // Fatal telemetry allocation/database errors cannot escape a worker boundary.
                failed.store(true);
            }
        } catch (...) {
            failed.store(true);
            static_cast<void>(std::fprintf(stderr,
                "atx-server worker infrastructure: unknown exception\n"));
        }
    }
};

Scheduler::Scheduler(Store& store, EventBus& events, Execute execute,
                     std::uint32_t workers, std::size_t capacity, bool autostart,
                     WorkflowAuthorize workflow_authorize)
    : impl_(std::make_unique<Impl>(store, events, std::move(execute), workers, capacity,
                                  autostart, std::move(workflow_authorize))) {}
Scheduler::~Scheduler() { shutdown(); }
Job Scheduler::submit(std::string_view request, std::string_view key,
    std::int64_t scheduled_at_ms) {
    return submit(request, key, scheduled_at_ms, {});
}
Job Scheduler::submit(std::string_view request, std::string_view key,
    std::int64_t scheduled_at_ms, const JobContext& context) {
    const auto canonical = request.empty() && !context.alpha_id.empty() ? std::string{} :
        validated_request(request);
    validate_schedule(key, scheduled_at_ms);
    Job job;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->started || impl_->stopping || impl_->failed.load()) {
            throw Error(ErrorCode::unavailable, "scheduler is unavailable");
        }
        job = impl_->store.create(canonical, key, scheduled_at_ms, impl_->capacity, context);
        ++impl_->generation;
    }
    impl_->changed.notify_all();
    impl_->notify_job("job.submitted", job);
    return job;
}
Experiment Scheduler::submit_experiment(const ExperimentInput& input) {
    validate_schedule(input.idempotency_key, input.scheduled_at_ms);
    Experiment experiment;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->started || impl_->stopping || impl_->failed.load()) {
            throw Error(ErrorCode::unavailable, "scheduler is unavailable");
        }
        experiment = impl_->store.create_experiment(input, impl_->capacity);
        ++impl_->generation;
    }
    impl_->changed.notify_all();
    impl_->notify_experiment("experiment.created", experiment);
    for (const auto& job : experiment.jobs) impl_->notify_job("job.submitted", job);
    return experiment;
}
Experiment Scheduler::cancel_experiment(std::string_view id) {
    return cancel_experiment(id, "local");
}
Workflow Scheduler::create_workflow(const WorkflowInput& input) {
    Workflow workflow;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->started || impl_->stopping || impl_->failed.load()) {
            throw Error(ErrorCode::unavailable, "scheduler is unavailable");
        }
        workflow = impl_->store.create_workflow(input);
        ++impl_->generation;
    }
    impl_->changed.notify_all();
    impl_->notify_workflow("workflow.created", workflow);
    return workflow;
}
Workflow Scheduler::set_workflow_paused(std::string_view id, std::uint64_t expected,
    bool paused, std::string_view actor) {
    Workflow workflow;
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->started || impl_->stopping || impl_->failed.load()) {
            throw Error(ErrorCode::unavailable, "scheduler is unavailable");
        }
        workflow = impl_->store.set_workflow_paused(id, expected, paused, actor);
        ++impl_->generation;
    }
    impl_->changed.notify_all();
    impl_->notify_workflow(paused ? "workflow.paused" : "workflow.resumed", workflow);
    return workflow;
}
Experiment Scheduler::cancel_experiment(std::string_view id, std::string_view actor_id) {
    Experiment experiment;
    std::array<std::optional<std::stop_source>, 32> sources;
    std::size_t count{};
    {
        std::lock_guard lock(impl_->mutex);
        experiment = impl_->store.cancel_experiment(id, actor_id);
        ++impl_->generation;
        for (const auto& job : experiment.jobs) {
            const auto running = impl_->running.find(job.id);
            if (running != impl_->running.end()) sources.at(count++) = running->second;
        }
    }
    for (std::size_t i = 0; i < count; ++i) sources[i]->request_stop();
    impl_->changed.notify_all();
    impl_->notify_experiment(experiment.cancelled != 0 ? "experiment.cancelled" :
                             "experiment.unchanged", experiment);
    for (const auto& job : experiment.jobs) {
        impl_->notify_job(job.status == "CANCELLED" ? "job.cancelled" : "job.unchanged", job);
    }
    return experiment;
}
Job Scheduler::cancel(std::string_view id) { return cancel(id, "local"); }
Job Scheduler::cancel(std::string_view id, std::string_view actor_id) {
    Job job;
    const std::string lookup(id);
    std::optional<std::stop_source> source;
    {
        std::lock_guard lock(impl_->mutex);
        job = impl_->store.cancel(id, actor_id);
        ++impl_->generation;
        const auto found = impl_->running.find(lookup);
        if (found != impl_->running.end()) source = found->second;
    }
    // stop callbacks are user code; invoke outside the scheduler lock.
    if (source) source->request_stop();
    impl_->notify_job(job.status == "CANCELLED" ? "job.cancelled" : "job.unchanged", job);
    impl_->changed.notify_all();
    return job;
}
void Scheduler::start() { impl_->start(); }
void Scheduler::shutdown() {
    // Copying a stop_source shares existing state and allocates nothing.
    std::array<std::optional<std::stop_source>, 64> sources;
    std::size_t source_count{};
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->shutdown_called) return;
        impl_->shutdown_called = true;
        impl_->stopping = true;
        for (const auto& [id, source] : impl_->running) {
            static_cast<void>(id);
            sources.at(source_count++) = source;
        }
    }
    for (std::size_t i = 0; i < source_count; ++i) sources[i]->request_stop();
    for (auto& worker : impl_->workers) worker.request_stop();
    impl_->recurring.request_stop();
    impl_->changed.notify_all();
    if (impl_->recurring.joinable()) impl_->recurring.join();
    for (auto& worker : impl_->workers) if (worker.joinable()) worker.join();
}
std::uint32_t Scheduler::worker_count() const noexcept { return impl_->count; }
bool Scheduler::healthy() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->started && !impl_->stopping && !impl_->failed.load();
}
} // namespace atx::server
