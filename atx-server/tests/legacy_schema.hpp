#pragma once

namespace atx::server::test_detail {
inline constexpr auto remove_workflows =
    "DROP TABLE workflow_occurrences; DROP TABLE workflow_heads; DROP TABLE workflow_definitions;"
    "DROP TRIGGER job_workflow_immutable; DROP INDEX jobs_workflow;"
    "ALTER TABLE jobs DROP COLUMN workflow_id;"
    "ALTER TABLE jobs DROP COLUMN workflow_slot_ms;";
// Tests remove only post-v5 additions to model an actual legacy database honestly.
inline constexpr auto remove_governance =
    "DROP TABLE workflow_occurrences; DROP TABLE workflow_heads; DROP TABLE workflow_definitions;"
    "DROP TRIGGER job_workflow_immutable; DROP INDEX jobs_workflow;"
    "ALTER TABLE jobs DROP COLUMN workflow_id;"
    "ALTER TABLE jobs DROP COLUMN workflow_slot_ms;"
    "DROP TABLE review_decisions; DROP TABLE review_heads; DROP TABLE review_submissions;"
    "DROP TABLE audit_records; DROP TRIGGER job_actor_immutable;"
    "ALTER TABLE jobs DROP COLUMN actor_id; ALTER TABLE workspaces DROP COLUMN actor_id;"
    "ALTER TABLE alpha_revisions DROP COLUMN actor_id;"
    "ALTER TABLE experiments DROP COLUMN actor_id;";
} // namespace atx::server::test_detail
