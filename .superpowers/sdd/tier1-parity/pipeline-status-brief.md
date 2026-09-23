# OPS1: bounded production pipeline status

User steering on2026-09-23: continue platform/infrastructure development when
memory blocks backfill. Production operators currently need bespoke SQL to
distinguish a current schema from completed activation and locate actual source
dataset UUIDs. Existing `atx-db status` says `ready` based on schema only and
scans fact/price coverage; preserve its compatibility and add a distinct command.

Implement `atx-db pipeline-status --db-path PATH --as-of-date YYYY-MM-DD`
with JSON output, plus a thin standalone script if useful. Own only new
`src/atx_db/pipeline_status.py`, small cli.py wiring, new focused tests and
`docs/PIPELINE_STATUS.md`. No activation/jobs/registry/migration edits.

- Open an existing database strictly read-only with256MB/one thread configured
  at connect time; UTC. Never initialize/migrate/checkpoint/create a missing DB,
  scan raw fact/price tables, infer durable row counts, or perform recovery.
- Inspect schema_migrations, activation_stage_runs, dataset_runs only. Missing
  file/table and locked/unavailable DB produce a clear bounded result/nonzero
  command exit, with no raw exception/parameter/error payload exposure.
- Report applied versus registered migration versions, detecting lower-version
  gaps. Report latest attempt per STAGE_ORDER stage for the exact snapshot.
  Rank deterministically; include recorded status,run_id,start/end,ledger rows,
  has_error and whitelist scope controls sufficient to expose partial source
  attempts. Missing or invalid snapshot metadata is unknown, never borrowed
  from another date. Record missing stage rows explicitly.
- Distinguish ledger completion from production readiness and process liveness.
  A recorded `running` row is not proof of a live process; emit liveness unknown
  and operator-check-needed. No automatic stale-run closure or restart. Even all
  stages completed is not a quality/SLO certification. Keep readiness unassessed.
- For source stages companyfacts_load/submissions_load/earnings_release_facts,
  correlate actual dataset_runs UUID using exact safe run-label metadata and
  the activation attempt time interval. Do not guess UUID from label, select an
  ambiguous match, or expose params_json. Derive labels/dataset IDs from actual
  stage code. Expose actual source attempt status and validated unique failed
  UUID as a resume candidate only; normal source-resume proof remains mandatory.
- Never select or print whole params_json or raw errors. Select only explicit
  safe scalar fields; older historical parameter values can include user email.
  Prefer a stable machine-readable summary and stage rows in ladder order.
- Focused tiny fixtures: schema-current but incomplete; failed source and exact
  actual UUID; stale running is liveness-unknown; different snapshot exclusion;
  malformed metadata; lower-version gap; absent DB/table; secret params not
  exposed; ambiguous source attempt not recommended; read-only/low-memory
  opening. Add CLI argument/exit check. Do not initialize the full warehouse.

No runtime/DB/test command by implementer until root grants the single slot.
One fresh Codex review, focused tests only, then pathspec-only task commit with
the requested literal trailer. No registry entries needed. Write result report
and exact focused command; do not commit before root testing/review.
