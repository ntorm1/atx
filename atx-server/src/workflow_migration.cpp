#include "workflow_store.hpp"

namespace atx::server::workflow_detail {
void migrate(sqlite3* db) {
    store_detail::Transaction transaction(db);
    store_detail::execute(db,
        "CREATE TABLE workflow_definitions(id TEXT PRIMARY KEY,name TEXT NOT NULL "
        "CHECK(length(name) BETWEEN 1 AND 80),description TEXT NOT NULL "
        "CHECK(length(CAST(description AS BLOB))<=512),request_json TEXT NOT NULL "
        "CHECK(length(CAST(request_json AS BLOB))<=65536),request_sha256 TEXT NOT NULL,"
        "alpha_id TEXT NOT NULL,alpha_revision INTEGER NOT NULL CHECK(alpha_revision>=0),"
        "actor_id TEXT NOT NULL,interval_ms INTEGER NOT NULL "
        "CHECK(interval_ms BETWEEN 60000 AND 2592000000),max_runs INTEGER NOT NULL "
        "CHECK(max_runs BETWEEN 1 AND 1000),first_run_at_ms INTEGER NOT NULL "
        "CHECK(first_run_at_ms>0),created_at_ms INTEGER NOT NULL,idempotency_key TEXT UNIQUE,"
        "semantic_json TEXT NOT NULL CHECK(length(CAST(semantic_json AS BLOB))<=98304));"
        "CREATE TABLE workflow_heads(id TEXT PRIMARY KEY REFERENCES workflow_definitions(id),"
        "revision INTEGER NOT NULL CHECK(revision>=1),enabled INTEGER NOT NULL "
        "CHECK(enabled IN (0,1)),next_run_at_ms INTEGER NOT NULL CHECK(next_run_at_ms>0),"
        "pause_reason TEXT NOT NULL DEFAULT '',last_dispatch_error TEXT NOT NULL DEFAULT '',"
        "updated_at_ms INTEGER NOT NULL,missed_slots INTEGER NOT NULL DEFAULT 0 "
        "CHECK(missed_slots>=0),skipped_slots INTEGER NOT NULL DEFAULT 0 "
        "CHECK(skipped_slots>=missed_slots));"
        "CREATE INDEX workflows_due ON workflow_heads(enabled,next_run_at_ms,id);"
        "CREATE INDEX workflows_updated ON workflow_heads(updated_at_ms DESC,id DESC);"
        "ALTER TABLE jobs ADD COLUMN workflow_id TEXT NOT NULL DEFAULT '';"
        "ALTER TABLE jobs ADD COLUMN workflow_slot_ms INTEGER NOT NULL DEFAULT 0;"
        "CREATE INDEX jobs_workflow ON jobs(workflow_id,workflow_slot_ms DESC);"
        "CREATE TRIGGER job_workflow_immutable BEFORE UPDATE OF workflow_id,"
        "workflow_slot_ms ON jobs BEGIN SELECT RAISE(ABORT,'workflow link is immutable'); END;"
        "CREATE TABLE workflow_occurrences(workflow_id TEXT NOT NULL "
        "REFERENCES workflow_definitions(id),scheduled_slot_ms INTEGER NOT NULL "
        "CHECK(scheduled_slot_ms>0),job_id TEXT NOT NULL UNIQUE REFERENCES jobs(id),"
        "created_at_ms INTEGER NOT NULL,PRIMARY KEY(workflow_id,scheduled_slot_ms));"
        "CREATE TRIGGER workflow_definition_no_update BEFORE UPDATE ON workflow_definitions "
        "BEGIN SELECT RAISE(ABORT,'workflow recipe is immutable'); END;"
        "CREATE TRIGGER workflow_definition_no_delete BEFORE DELETE ON workflow_definitions "
        "BEGIN SELECT RAISE(ABORT,'workflow recipe is immutable'); END;"
        "CREATE TRIGGER workflow_occurrence_no_update BEFORE UPDATE ON workflow_occurrences "
        "BEGIN SELECT RAISE(ABORT,'workflow occurrence is immutable'); END;"
        "CREATE TRIGGER workflow_occurrence_no_delete BEFORE DELETE ON workflow_occurrences "
        "BEGIN SELECT RAISE(ABORT,'workflow occurrence is immutable'); END;"
        "PRAGMA user_version=7;");
    transaction.commit();
}
} // namespace atx::server::workflow_detail
