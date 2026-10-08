#include "workflow_store.hpp"
#include "job_row.hpp"

namespace atx::server::workflow_detail {
namespace {
using namespace store_detail;
void counts(sqlite3* db, Workflow& workflow) {
    auto query = prepare(db, "SELECT j.status,j.id FROM workflow_occurrences o "
        "JOIN jobs j ON j.id=o.job_id WHERE o.workflow_id=? "
        "ORDER BY o.scheduled_slot_ms DESC LIMIT 1000");
    bind_value(query, 1, workflow.id);
    for (std::uint32_t i = 0; i < 1000 && step(query) == SQLITE_ROW; ++i) {
        const auto status = column(query, 0), id = column(query, 1);
        if (workflow.admitted_runs == 0) workflow.last_job_id = id;
        ++workflow.admitted_runs;
        if (status == "QUEUED") ++workflow.queued;
        else if (status == "RUNNING") ++workflow.running;
        else if (status == "SUCCEEDED") ++workflow.succeeded;
        else if (status == "FAILED") ++workflow.failed;
        else if (status == "CANCELLED") ++workflow.cancelled;
        if (status == "QUEUED" || status == "RUNNING") workflow.active_job_id = id;
    }
    if (workflow.admitted_runs >= workflow.max_runs) {
        workflow.state = workflow.active_job_id.empty() ? "completed" : "draining";
    } else workflow.state = workflow.enabled ? "active" : "paused";
}
} // namespace
Workflow get(sqlite3* db, std::string_view id, bool recipe) {
    const auto sql = std::string("SELECT d.id,d.name,d.description,") +
        (recipe ? "d.request_json" : "''") + ",d.request_sha256,d.alpha_id,d.alpha_revision,"
        "d.actor_id,d.interval_ms,d.max_runs,d.first_run_at_ms,h.next_run_at_ms,h.revision,"
        "h.enabled,h.pause_reason,h.last_dispatch_error,d.created_at_ms,h.updated_at_ms,"
        "h.missed_slots,h.skipped_slots FROM workflow_definitions d JOIN workflow_heads h "
        "ON h.id=d.id WHERE d.id=?";
    auto query = store_detail::prepare(db, sql.c_str());
    store_detail::bind_value(query, 1, id);
    if (store_detail::step(query) != SQLITE_ROW) {
        throw Error(ErrorCode::missing, "workflow not found");
    }
    const auto integer = [&](int index) { return sqlite3_column_int64(query.get(), index); };
    using store_detail::column;
    Workflow workflow;
    workflow.id = column(query, 0); workflow.name = column(query, 1);
    workflow.description = column(query, 2); workflow.request_json = column(query, 3);
    workflow.request_sha256 = column(query, 4); workflow.alpha_id = column(query, 5);
    workflow.alpha_revision = static_cast<std::uint64_t>(integer(6));
    workflow.actor_id = column(query, 7);
    workflow.interval_ms = static_cast<std::uint64_t>(integer(8));
    workflow.max_runs = static_cast<std::uint32_t>(integer(9));
    workflow.first_run_at_ms = integer(10); workflow.next_run_at_ms = integer(11);
    workflow.revision = static_cast<std::uint64_t>(integer(12));
    workflow.enabled = integer(13) != 0; workflow.pause_reason = column(query, 14);
    workflow.last_dispatch_error = column(query, 15); workflow.created_at_ms = integer(16);
    workflow.updated_at_ms = integer(17);
    workflow.missed_slots = static_cast<std::uint64_t>(integer(18));
    workflow.skipped_slots = static_cast<std::uint64_t>(integer(19));
    counts(db, workflow);
    return workflow;
}
WorkflowPage list(sqlite3* db, std::uint32_t limit, std::uint32_t offset) {
    using namespace store_detail;
    auto count = prepare(db, "SELECT count(*) FROM workflow_definitions");
    if (step(count) != SQLITE_ROW) database_error(db);
    WorkflowPage page;
    page.total = static_cast<std::uint64_t>(sqlite3_column_int64(count.get(), 0));
    auto query = prepare(db,
        "SELECT id FROM workflow_heads ORDER BY updated_at_ms DESC,id DESC LIMIT ? OFFSET ?");
    bind_value(query, 1, static_cast<std::int64_t>(limit));
    bind_value(query, 2, static_cast<std::int64_t>(offset));
    page.workflows.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.workflows.push_back(get(db, column(query, 0), false));
    }
    return page;
}
std::vector<Workflow> due(sqlite3* db, std::int64_t now, std::uint32_t limit) {
    using namespace store_detail;
    auto query = prepare(db, "SELECT h.id FROM workflow_heads h "
        "JOIN workflow_definitions d ON d.id=h.id WHERE h.enabled=1 AND h.next_run_at_ms<=? "
        "AND (SELECT count(*) FROM workflow_occurrences o WHERE o.workflow_id=h.id)<d.max_runs "
        "ORDER BY h.next_run_at_ms,h.id LIMIT ?");
    bind_value(query, 1, now); bind_value(query, 2, static_cast<std::int64_t>(limit));
    std::vector<Workflow> workflows;
    workflows.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        workflows.push_back(get(db, column(query, 0), false));
    }
    return workflows;
}
WorkflowRunPage runs(sqlite3* db, std::string_view id, std::uint32_t limit,
    std::uint32_t offset) {
    using namespace store_detail;
    WorkflowRunPage page;
    page.total = get(db, id, false).admitted_runs;
    auto query = prepare(db, (std::string("SELECT ") + job_detail::summaries +
        " FROM jobs WHERE workflow_id=? ORDER BY workflow_slot_ms DESC LIMIT ? OFFSET ?")
        .c_str());
    bind_value(query, 1, id); bind_value(query, 2, static_cast<std::int64_t>(limit));
    bind_value(query, 3, static_cast<std::int64_t>(offset));
    page.jobs.reserve(limit);
    for (std::uint32_t i = 0; i < limit && step(query) == SQLITE_ROW; ++i) {
        page.jobs.push_back(job_detail::row(query));
    }
    return page;
}
} // namespace atx::server::workflow_detail
