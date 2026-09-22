# MP1: bound daily market publication connection lifetime

Root source finding, 2026-09-21 UTC / Sep20 local. Market_daily already computes
and commits one security batch at a time. Python receives a small identifier
list and scalar INSERT counts, not the full panel. However,
refresh_market_daily_metrics keeps the same connection/index state for all
batches of the31.96M-row price population, as CF5 and DP1 previously did for
their issuer loops. This is a source-level lifetime gap, not a measured market
stage failure. Preserve all existing daily metric and P1/AF1 semantics.

Prepare DRAFT ONLY under market-connection-draft/ and write
market-connection-capacity-report.md. Own market_daily.py and one new focused
test file; no migration, registry, jobs, activation, connection.py or shared
publication-helper changes. Do not edit live atx-db source during archive4.

After each successfully committed existing security batch, checkpoint/reopen the
configured persistent store through its existing resource replay. The current
identifier list is already detached; consume INSERT count results before close.
No cursor, transaction or task-owned temp state may remain across reopen.
Use the CF5/DP1 rule: persistent file plus both recorded analytical fields.
Unconfigured/in-memory callers retain their connection and full session state;
the lifetime guarantee applies to the configured production path only. Refuse
caller temp tables/views before initialization/mutation when recycling applies;
do not silently discard caller state or invent settings. No RAM increase and no
need to change batch size, full scope, dates, annual/PIT selection, source filters,
trailing-window lookback, output schema, atomic batch failure behavior or totals.

Focused coverage: tiny real file build crossing multiple forced-small security
batches, compare all logical output with existing baseline/fixtures, record SQL
budget settings before any first reopen and check each replay, verify scoped
dates/sources/foreign rows and count totals, injected failing batch retains its
old rows while earlier completed batch remains committed, caller temp refusal
before mutation, and preserved in-memory/unconfigured behavior. Reuse existing
market arithmetic/PIT tests rather than duplicating their matrix. Small fixtures
prove lifecycle/output behavior, not actual production memory capacity.

Fresh Codex implementer, STATIC reads/edits only. NO Python/imports/tests,
collection/Ruff/mypy/DB/probes/network/install/runtime commands or live edits.
No commits/stash/checkout/reset/restore/clean or subagents. Root owns integration
and all guarded focused execution after the sole healthy archive writer ends.
One fresh independent static review may occur on the stable draft while waiting;
Important fixes accepted on report, re-review only Critical. Report exact files,
basis hashes, integration steps, selectors and limits.
