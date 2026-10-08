#include "experiment_store.hpp"
#include "experiment_jobs.hpp"
#include "job_row.hpp"
#include "library_store.hpp"
#include "workspace_validation.hpp"
#include "governance_store.hpp"

namespace atx::server::experiment_detail {
using namespace store_detail;
std::string resolve(sqlite3* db, std::string_view request, std::string_view alpha_id,
    std::uint64_t revision) {
    if (alpha_id.empty()) {
        if (revision != 0 || request.empty() || request.size() > 65536) {
            throw Error(ErrorCode::invalid, "unlinked jobs require a bounded nonempty recipe");
        }
        return std::string(request);
    }
    workspace_detail::validate_id(alpha_id);
    workspace_detail::validate_revision(revision);
    const auto alpha = library_detail::get(db, alpha_id, revision);
    if (!request.empty()) {
        const auto json = workspace_detail::parse_json(request, 65536);
        if (!json.is_object() || json.dump() != alpha.request_json) {
            throw Error(ErrorCode::conflict, "job recipe does not match immutable alpha revision");
        }
    }
    return alpha.request_json;
}
void require_active_alpha(sqlite3* db, std::string_view id) {
    if (!id.empty() && library_detail::get(db, id, 0).stage != "research") {
        throw Error(ErrorCode::conflict, "restore archived alpha before submitting new research");
    }
}
void require_capacity(sqlite3* db, std::size_t capacity, std::size_t requested) {
    if (capacity == 0 || capacity > 100000 || requested > capacity) {
        throw Error(requested > capacity ? ErrorCode::capacity : ErrorCode::invalid,
                    "invalid job queue capacity");
    }
    auto count = prepare(db, "SELECT count(*) FROM jobs WHERE status IN ('QUEUED','RUNNING')");
    if (step(count) != SQLITE_ROW) database_error(db);
    const auto active = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    if (active > capacity - requested) throw Error(ErrorCode::capacity, "job queue is full");
}
void insert_job(sqlite3* db, const Job& job, std::string_view key) {
    auto query = prepare(db,
        "INSERT INTO jobs(id,status,request_json,created_at_ms,scheduled_at_ms,"
        "idempotency_key,label,alpha_id,alpha_revision,experiment_id,actor_id,workflow_id,"
        "workflow_slot_ms) VALUES(?,'QUEUED',?,?,?,?,?,?,?,?,?,?,?)");
    bind_value(query, 1, job.id); bind_value(query, 2, job.request_json);
    bind_value(query, 3, job.created_at_ms); bind_value(query, 4, job.scheduled_at_ms);
    if (key.empty()) {
        if (sqlite3_bind_null(query.get(), 5) != SQLITE_OK) database_error(db);
    } else bind_value(query, 5, key);
    bind_value(query, 6, job.label); bind_value(query, 7, job.alpha_id);
    bind_value(query, 8, static_cast<std::int64_t>(job.alpha_revision));
    bind_value(query, 9, job.experiment_id); bind_value(query, 10, job.actor_id);
    bind_value(query, 11, job.workflow_id); bind_value(query, 12, job.workflow_slot_ms);
    static_cast<void>(step(query));
}
Job create_job(sqlite3* db, std::string_view request, std::string_view key,
    std::int64_t scheduled, std::size_t capacity, const JobContext& context, std::int64_t now) {
    if (request.size() > 65536 || key.size() > 128 || scheduled < 0 || capacity == 0 ||
        capacity > 100000) throw Error(ErrorCode::invalid, "invalid job submission limits");
    workspace_detail::validate_text(key, 128);
    governance_detail::validate_actor(context.actor_id);
    const auto label = context.label.empty() ? std::string{} :
        workspace_detail::validate_name(context.label);
    Transaction transaction(db);
    const auto resolved = resolve(db, request, context.alpha_id, context.alpha_revision);
    if (!key.empty()) {
        auto query = prepare(db, (std::string("SELECT ") + job_detail::fields +
            " FROM jobs WHERE idempotency_key=?").c_str());
        bind_value(query, 1, key);
        if (step(query) == SQLITE_ROW) {
            auto existing = job_detail::row(query);
            const auto expected_label = label.empty() ? "Research " + existing.id.substr(0, 8) :
                label;
            if (existing.request_json != resolved || existing.scheduled_at_ms != scheduled ||
                existing.alpha_id != context.alpha_id ||
                existing.alpha_revision != context.alpha_revision ||
                existing.label != expected_label || existing.actor_id != context.actor_id) {
                throw Error(ErrorCode::conflict, "idempotency key belongs to a different job");
            }
            transaction.commit();
            return existing;
        }
    }
    require_active_alpha(db, context.alpha_id);
    require_capacity(db, capacity, 1);
    Job job;
    job.id = identifier(); job.status = "QUEUED"; job.request_json = resolved;
    job.created_at_ms = now; job.scheduled_at_ms = scheduled;
    job.label = label.empty() ? "Research " + job.id.substr(0, 8) : label;
    job.alpha_id = context.alpha_id; job.alpha_revision = context.alpha_revision;
    job.actor_id = context.actor_id;
    insert_job(db, job, key);
    governance_detail::audit(db, "job", job.id, "job.submitted", job.actor_id, 0,
        {{"alpha_id", job.alpha_id}, {"alpha_revision", job.alpha_revision}}, now);
    transaction.commit();
    return job;
}
} // namespace atx::server::experiment_detail
