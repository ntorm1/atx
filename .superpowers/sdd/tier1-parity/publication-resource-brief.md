# PR1: bounded release CLI resources

Production prerequisite discovered by root static command preparation on
2026-09-20, after CF5/AF1 source commits. This is a narrow addendum to S4 T8,
not a change to release quality gates or schemas.

## Evidence and objective

`cli.py` defines publish-release with only paths, IDs and predecessor arguments.
Its handler opens DuckDBStore and calls publish_release without configuring an
analytical budget. The library exports/sorts full-universe tables. The guarded
operator therefore cannot request the established 1 GB/one-thread query budget
through the scheduled release command. A process guard protects the host but is
not a replacement for that database budget.

Add --memory-limit (default 1GB) and --threads (default 1) to publish-release.
Configure the existing analytical session before calling publish_release, using
the established helper/settings replay mechanism. Preserve pending-migration
refusal before writable opening, deterministic release queries, export/manifest
contracts, clock injection, existing directory refusal and release ledgers.
Do not change the generic store lifecycle, migration code or resource defaults
for unrelated commands. The outer process guard remains mandatory for operators.

## Ownership and process

Fresh Codex implementer only. Own actual `atx-db/src/atx_db/cli.py` and a new
`atx-db/tests/test_publication_resources.py`; write
`publication-resource-report.md`. Root owns runbook updates and all runtime
execution. No registry/jobs/activation edits, data changes, extra dependencies
or commits. No stash, checkout, reset, restore or clean.

The healthy archive4 process is the only runtime workload. Agent must not run
imports, tests, collection, lint/type checks, DB/probes, package installs or
production commands. Static source reads/edits only. Root will execute focused
checks after the writer finishes; no interruption or RAM escalation for this task.

Add meaningful focused checks that exercise the actual CLI and inspect settings
at the publication boundary: conservative defaults and explicit bounded override,
including recorded configuration that survives reopen where applicable. Reuse
existing publication migration-refusal tests rather than duplicating them. Do
not test every argparse permutation or copy implementation assertions. Existing
publication tests retain artifact-level coverage; full suite stays at the gate.

One fresh static review after source is stable. Important fixes accepted on the
implementer's report and root focused execution; rereview only for Critical.
Report exact changed paths, selectors, limitations and pending runtime status.
