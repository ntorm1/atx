# FQ1 fundamental signal panel implementation report

Status: implementation and isolated focused checks complete. Migration 0324
follows committed DL1 selected-input lineage migration 0323. No live warehouse
build or production-scale performance claim has been made.

## Public interface

`build_fundamental_signal_panel(store: DuckDBStore,
options: FundamentalSignalResearchOptions) -> FundamentalSignalResearchResult`
requires an already migrated warehouse. Options require `start_date`,
`end_date`, `as_of_date`, UTC-aware `run_at`, unique `run_id`; optional
`signals`, `max_age_days=200`, `memory_limit='256MB'`, `threads=1`.
`validate_fundamental_signal_panel(con, build_run_id)` raises on an incomplete
or changed panel and returns verified digest, canonical specs, signal IDs,
decision/entry session pairs and counts. Publication and validation call the
same versioned digest recipe; FQ2 should call this before any outcome read.
CLI: `scripts/research_fundamental_signals.py build --db-path ...
--start-date ... --end-date ... --as-of-date ... --run-at ... --run-id ...
[--signals-json ...] [--memory-limit 256MB] [--threads 1]`.

## Warehouse interface for the next evaluator

- `fundamental_signal_runs`: `run_id` primary key, `status`, canonical
  `spec_json`/`spec_sha256`, exact resolved `definitions_json`/
  `definitions_sha256`, `query_version`, `source_ids_json`, `calendar_sha256`,
  requested dates, `run_at`, decision policy, diagnostics, blockers,
  `panel_sha256`, `production_eligible=false`. Evaluators require
  `status='complete'` and independently check the pinned digest.
- `fundamental_signal_definitions`: `(run_id, signal_id)` key, ordinal,
  canonical per-signal spec and hash.
- `fundamental_signal_values`: one long row per run, signal, decision session
  and historical cohort security. `score` is nullable; `eligible`, `reason`,
  `decision_at`, `entry_date`, `input_end`, `input_available_at`, `cohort_size`
  are explicit. Evaluators rank these scores before outcome joins.
- `fundamental_signal_inputs`: one row per selected signal term and cohort
  security. Holds selected `derived_value_id`, publisher owner ID, definition,
  input, selected-ref and proof hashes, selected leaf CIK and fiscal span,
  period and fiscal operand dates, clock, origin, history/value status,
  raw value and failure reason.
- `fundamental_signal_proofs`: run-scoped exact selected-input proof cache,
  one row per distinct derived root, with owner/event time, selected CIK,
  leaf IDs/clocks/fiscal ends, status/reason and digest.
- `fundamental_signal_coverage`: every observed requested decision session and
  signal, including empty/blocked sessions. Holds membership/common/overlap/
  CIK/score attrition counts, unmatched owner states, constant-score flag and
  reason histogram.

The default family consists of four directionally pinned standalone signals
and the equal-weight complete-case rank mix. Caller specifications are bounded
to 32 signals, eight terms each, and the four audited dimensionless publisher
metrics. Identity and cross-sectional rank transforms are supported; SQL and
unreviewed metrics cannot enter through the CLI.

## Timing, partition, and memory

The builder selects observed market sessions into a bounded list of dates,
then creates transient SQL cohort and selected-leg tables for one decision
session and one term at a time. It writes run-scoped values, inputs and coverage
for each session in a transaction. No Python name/date/history cross product
or all-universe pandas frame is created. With defaults, the largest transient
state is one session's visible membership/CIK cohort plus one metric's visible
derived event history over publisher owners; the persistent partition is at most
`32 signals * 8 terms * cohort size` input rows per session. The source history
scan can still be material and must be measured on a representative bounded
range before any production-scale run. Candidate owner states are capped at
100,000 per metric/session; when a current owner has no CIK proof, its full
prior visible owner/metric history is staged under a separate 100,000-row
limit. Exceeding either cap fails the run, preserving ambiguous-owner safety.
Proof resolution uses 256-root batches and subdivides aggregate resolver-cap
failures. The CLI enforces 256 MB and one thread.

T's decision cutoff is 22:00 UTC; entry is the next observed session. Visible
membership must be dated US common, overlaps are quarantined, and normalized
CIK links must be unique. Publisher bucket events and fiscal periods are ranked
before rejecting NULL, stale, legacy, future, wrong-definition and other
invalid states. The selection does not use today's `is_latest_revision`.

## Exact lineage gate

DL1 migration 0323 persists canonical selected-input references and their
SHA256 on each derived event. FQ1 calls its bounded resolver with the full
transitive definition-hash closure. Fully verified selected leaf CIK links
issuer-owned states to decision-date security CIK; multiple matching owners
are quarantined. A last previously verified CIK for the same publisher owner
and metric can only suppress an unqualified current state. It cannot qualify
that state or backdate identity. Owners without current or prior CIK evidence
remain explicit unmatched diagnostics.

The actual root fiscal end and newest selected leaf fiscal end must both be
within 200 days and <=T. Older declared YoY comparison leaves remain valid
when their exact offsets, spans and clocks verify. The oldest/newest selected
leaf fiscal ends are retained for audit.

## Validation command and prerequisites

Focused command, run from `C:/atx/atx-db` under the repository memory guard:
`C:/atx/atx-db/.venv/Scripts/python.exe -m pytest tests/test_fundamental_signal_research.py -q -n0`.
Prerequisites: installed atx-db test dependencies, committed DL1 migration
0323 and resolver, and the isolated in-memory DuckDB fixture. The fresh
guarded run `fresh-fq1-tests4-memory.json` passed 12/12 with native peak
0.657 GiB under a 1.5 GiB job cap. Cases cover exact lineage, 17 intervening
unproved owner states, two decisions, YoY comparison age, rank/ties, empty
coverage, failed-partial sealing, manifest tampering, aggregate resolver
splitting, and migration replay. Independent review reported no Critical and
two Important findings; both are fixed. No alpha or production eligibility
is claimed.
