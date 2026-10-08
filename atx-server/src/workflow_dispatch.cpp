#include "workflow_store.hpp"
#include "experiment_jobs.hpp"
#include "governance_store.hpp"
#include "library_store.hpp"

#include <limits>

namespace atx::server::workflow_detail {
namespace {
using namespace store_detail;
void backpressure(sqlite3* db, const Workflow& workflow, std::int64_t now) {
    auto query = prepare(db, "UPDATE workflow_heads SET last_dispatch_error='backpressure',"
        "updated_at_ms=? WHERE id=?");
    bind_value(query, 1, now); bind_value(query, 2, workflow.id);
    static_cast<void>(step(query));
}
void advance(sqlite3* db, const Workflow& workflow, std::int64_t next,
    std::uint64_t missed, std::uint64_t skipped, std::int64_t now) {
    auto query = prepare(db, "UPDATE workflow_heads SET next_run_at_ms=?,"
        "missed_slots=missed_slots+?,skipped_slots=skipped_slots+?,"
        "last_dispatch_error='',updated_at_ms=? WHERE id=?");
    bind_value(query, 1, next); bind_value(query, 2, static_cast<std::int64_t>(missed));
    bind_value(query, 3, static_cast<std::int64_t>(skipped));
    bind_value(query, 4, now); bind_value(query, 5, workflow.id);
    static_cast<void>(step(query));
}
Job occurrence(sqlite3* db, const Workflow& workflow, std::int64_t slot, std::int64_t now) {
    Job job;
    job.id = identifier(); job.status = "QUEUED"; job.request_json = workflow.request_json;
    job.created_at_ms = now; job.scheduled_at_ms = slot;
    job.alpha_id = workflow.alpha_id; job.alpha_revision = workflow.alpha_revision;
    job.actor_id = workflow.actor_id; job.workflow_id = workflow.id; job.workflow_slot_ms = slot;
    // Name is already bounded to 80 codepoints; avoid suffixes which exceed that label contract.
    job.label = workflow.name;
    experiment_detail::insert_job(db, job);
    auto query = prepare(db, "INSERT INTO workflow_occurrences(workflow_id,scheduled_slot_ms,"
        "job_id,created_at_ms) VALUES(?,?,?,?)");
    bind_value(query, 1, workflow.id); bind_value(query, 2, slot);
    bind_value(query, 3, job.id); bind_value(query, 4, now); static_cast<void>(step(query));
    governance_detail::audit(db, "job", job.id, "job.submitted", job.actor_id, 0,
        {{"workflow_id", workflow.id}, {"workflow_slot_ms", slot},
         {"alpha_id", job.alpha_id}, {"alpha_revision", job.alpha_revision}}, now);
    return job;
}
bool source_available(sqlite3* db, const Workflow& workflow) {
    return workflow.alpha_id.empty() ||
        library_detail::get(db, workflow.alpha_id, 0).stage == "research";
}
} // namespace
WorkflowDispatch dispatch(sqlite3* db, std::string_view id, std::uint64_t observed,
    bool authorized, std::int64_t now, std::size_t capacity) {
    Transaction transaction(db);
    auto workflow = get(db, id);
    if (!workflow.enabled || workflow.revision != observed || workflow.next_run_at_ms > now ||
        workflow.admitted_runs >= workflow.max_runs) {
        transaction.commit(); return {};
    }
    if (!authorized || !source_available(db, workflow)) {
        pause_automatically(db, workflow,
            !authorized ? "creator_permission" : "source_archived", now);
        transaction.commit(); return {{}, true};
    }
    const auto interval = static_cast<std::int64_t>(workflow.interval_ms);
    const auto due_count = static_cast<std::uint64_t>((now - workflow.next_run_at_ms) /
        interval) + 1;
    const auto missed = due_count - 1;
    // Multiplication is <= now-next_run, and the sum <=now, by construction.
    const auto slot = workflow.next_run_at_ms + static_cast<std::int64_t>(missed) * interval;
    const auto skipped = workflow.active_job_id.empty() ? missed : due_count;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (slot > maximum - interval || missed >
        static_cast<std::uint64_t>(maximum) - workflow.missed_slots || skipped >
        static_cast<std::uint64_t>(maximum) - workflow.skipped_slots) {
        pause_automatically(db, workflow, "schedule_overflow", now);
        transaction.commit(); return {{}, true};
    }
    WorkflowDispatch result;
    if (workflow.active_job_id.empty()) {
        try { experiment_detail::require_capacity(db, capacity, 1); }
        catch (const Error& error) {
            if (error.code() != ErrorCode::capacity) throw;
            const auto changed = workflow.last_dispatch_error != "backpressure";
            if (changed) backpressure(db, workflow, now);
            transaction.commit(); return {{}, changed};
        }
        result.job = occurrence(db, workflow, slot, now);
    }
    advance(db, workflow, slot + interval, missed, skipped, now);
    governance_detail::audit(db, "workflow", workflow.id,
        result.job ? "workflow.dispatched" : "workflow.slots_skipped", workflow.actor_id,
        workflow.revision, {{"scheduled_slot_ms", slot}, {"missed_slots", missed},
         {"skipped_slots", skipped}, {"job_id", result.job ? result.job->id : ""}}, now);
    result.changed = true;
    transaction.commit(); return result;
}
} // namespace atx::server::workflow_detail
