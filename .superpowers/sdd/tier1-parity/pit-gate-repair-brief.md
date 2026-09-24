# PG1: repair the two completed PIT gate failures

`quant-platform-head6.log` records two assertion/contract failures from exported
HEAD `09fed6066da85169c5b4796941a55c6108588299`; this receipt is a completed
failed test run, not a capacity stop. The bounded task owns only
`atx-db/tests/test_derived_pit_revisions.py` and this brief plus
`pit-gate-repair-report.md`.

The economic acceptance question is whether filing-event metric states remain
identical across chunk sizes, physical input order and run IDs, and whether an
actual populated 0314 warehouse upgrades to the current schema without losing
legacy observations or inventing historical provenance.

Repair the fixture's temporary staging-table lifetime before the second derived
refresh. The production connection recycler intentionally refuses caller-owned
temporary objects, so no production guard or recorded memory limit changes.
Update the migration test to expect every registered migration after 0314,
including required 0315/0316, while retaining exact legacy values, nullable
invalid states, primary-key enforcement, API vintage behavior, uncertified
coverage, schema-contract and repeat-initialization assertions.

Acceptance is the two failed cases and the complete existing PIT revision test
file. Keep all deterministic state and selected-input-lineage equality checks.
Run no Python, database or test workload until root grants the single guarded
runtime slot after required sustained headroom. Root owns runtime, one
independent Codex review, and final commit coordination with explicit owned
pathspecs. No live warehouse access, migration/registry/job/activation edits,
source promotion or production-readiness claim is in this task.
