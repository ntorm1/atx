# S4 Task 11: independent documentation/CI review

Reviewed once by a Codex agent on 2026-09-20: `de32ea6e`, the dictionary/source
documentation follow-up in `dc130c53`, plan lines 6388 onward, the continuation
addendum, implementation reports, current runbook/generator, historical-banner
diffs, CI and focused test source. No test, generator or production command was
rerun. No warehouse was opened.

**Disposition: two Important documentation corrections; no Critical findings.**
The implementer's correction report is sufficient; no re-review is required.

## Important T11-R1: production examples conflict with the current memory envelope

`atx-db/docs/PRODUCTION_RUNBOOK.md:128` advertises an 8 GB DuckDB cap, four
threads and sixteen shards for the full ladder. The AR6 operator handoff also
still specifies 6 GB/eight threads. The controller has now observed the run4
statement stage exhausting memory, including an OS paging-file allocation
failure, on this roughly 16 GiB host. The user explicitly requires avoiding
machine exhaustion. These examples are no longer safe guidance for this rebuild.

Replace rebuild commands with the controller-approved bounded settings and
guarded launch policy: start at 1 GB/one DuckDB thread, one heavy process tree,
and an explicit aggregate job cap of at most 4 GiB plus headroom checks. The
controller's runner audit confirms the sixteen shards execute sequentially,
so retaining sixteen small partitions does not mean sixteen concurrent workers.
Do not imply that a DuckDB setting also caps pandas. Keep this concrete and
operator-facing; do not change unrelated global machine settings. The documented
resource envelope should agree across the runbook and AR6 handoff.

## Important T11-R2: blanket determinism statements overstate the inspected code

At `PRODUCTION_RUNBOOK.md:186`, the runbook says no ingest path reads the wall
clock; at `:196`, it says `source_loaded_at` is never an ordering/dedupe key.
The price publisher's `_create_symbol_map` uses `current_date` and orders identity
candidates by `source_loaded_at` (`ticker_history_bulk.py:100` onward). AR6 also
newly selects a prior vendor link using `arg_max(security_id, source_loaded_at)`
(`ticker_history.py:385`), reported independently as AR6-R2. Thus the universal
claims remain false even if the factor-panel-specific example is correct.

Scope the guarantee to the surfaces actually audited, name the unresolved price
identity/timing behavior, and record the corrective task rather than asserting
whole-ingest determinism. Do not imply that merely labeling source-vintage
uncertainty makes current-symbol identity selection deterministic or historical.
If the code is corrected, state the resulting supported contract accurately.

## Accepted changes

- The four historical documents receive the fixed banner; the reviewed diffs
  leave their prior bodies intact. The links use the actual two documentation
  roots. Their preservation is backed by the implementer's byte/hash record.
- README distinguishes generated contract breadth from pending live annual
  coverage. The dictionary generator reads implemented vocabularies directly
  and does not silently replace import failures with obsolete fallbacks.
- The current stage list and repeated `--only` syntax do not advertise deferred
  LEI/FIGI stages. Default Shumway policy, observed-first precedence, historical
  exchange requirements and incomplete terminal coverage are explained.
- Publication documents the existing standardized schema, six datasets,
  governed migrations, preserved backups, first-release null diffs and export
  success being separate from a quality gate.
- CI runs the unconditional dictionary check immediately before the existing
  full non-slow suite. The report says the three deferred dictionary cases and
  final regeneration passed in AR6; this review did not repeat them. No local
  full suite was run as part of the review.

## Small follow-up edits while reconciling the checkpoint

The runbook's dated status still says run4 is loading companyfacts (`:50`) and
the S3 eighteen-module retirement is in progress (`:296`). These accurately
described an earlier checkpoint but should now cite the completed retirement
commit and actual run4 failure, without implying activation/coverage success.
The controller has already recorded the run4 state in
`atx-db/docs/TIER1_ACTIVATION_STATUS.md`; reference that record rather than
duplicating its inventory or rerunning measurements.

At `:110`, advice that the extracted TSV may be deleted conflicts with the AR6
source-retention handoff. Preserve the actual source/sidecar for this rebuild
and require the controller's explicit retention decision before any cleanup.
No file deletion is requested or authorized by this review.

The machine's local gate should use bounded concurrency regardless of the
existing four-worker CI lane on a separate GitHub runner. Measured item/provider
coverage and schema-condition flips remain later production work.
