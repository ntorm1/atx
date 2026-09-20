# DP1: bound connection lifetime during the full derived-metric build

Root production-capacity task, 2026-09-20. Use current source plus the focused
fundamentals-activation-memory-audit.md when available; do not reopen P1/AF1
metric semantics. Current derived_metrics.py bounds event frames and each
security's atomic publication, but select_security_batches holds a second result
cursor for the full issuer stream and refresh_derived_metrics never recycles the
writer connection. Index state can accumulate across all issuers. CF5 already
repaired this same connection-lifetime problem after a measured ingestion COMMIT
failure; the derived stage itself has not yet had a production-scale failure.

Prepare draft changes only under derived-connection-draft/, with report
derived-connection-capacity-report.md. Do not edit live source while archive4
runs. Own derived_metrics.py and one new focused test file; no migrations,
registry, jobs, activation, connection.py or _bulk_publication.py edits.

Replace the lifetime-long identifier result cursor with bounded keyset batches
that remain correct across reopen. The input scope must stay identical: all
quarterly/instant/annual standardized IDs plus existing selected-source derived
IDs, including stale-output-only IDs requiring cleanup. Preserve explicit scoped
IDs, ordering, deduplication and dependency closure. Do not omit any issuer or
metric or accumulate full fact/event/output frames in Python.

After a bounded number of successfully committed complete security scopes
(use the established conservative10-operation cadence), clean owned PIT temps,
checkpoint and reopen persistent stores. Flush the final partial group too.
Reapply the store's recorded analytical configuration using the existing reopen
mechanism. Do not keep any result cursor, transaction or owned temp object alive
across reopen. Preserve the existing per-security atomic failure contract,
original/amended/null event states, annual fallback, row limits and return counts.
In-memory callers must retain their database and remain supported. Do not silently
discard caller-owned temporary objects; validate incompatible persistent-session
state before any mutation, following the established CF5 safety precedent.

Focused tests should use a tiny real file-backed build and forced small cadence
to cross multiple keyset/reopen boundaries; include standardized IDs and a later
stale-derived-only ID, scoped IDs, preserved full results/return counts and actual
budget replay. Cover caller-owned temporary-state refusal before mutation and
the unchanged in-memory path only where existing tests do not already do so.
Reuse P1/AF1 arithmetic/history fixtures rather than duplicating a large suite.
Report exact selectors. Small fixtures establish control flow, not production
memory capacity.

Fresh Codex implementer, static reads/edits only. NO Python/imports/tests,
collection, Ruff/mypy, DB/probes, network, installs, live source edits or commits.
No stash/checkout/reset/restore/clean. No subagents. Root integrates and runs all
focused verification after the sole active writer finishes, then one independent
review. Important fixes accepted on report; re-review only Critical. No RAM or
query-budget increase. All output contracts and production scope stay intact.
