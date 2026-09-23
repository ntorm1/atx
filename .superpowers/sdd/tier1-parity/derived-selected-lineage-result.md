# DL1 selected-operand lineage result

Status: implementation staged; isolated focused tests and scoped Ruff passed.
Affected full-schema regression tests remain pending host capacity.

Migration 0323 adds nullable `derived_metric_values.selected_input_refs_json` and
`selected_input_refs_hash`. Legacy rows retain NULL and are unverified. Rebuilt
filing events store deterministic JSON `{"version":1,"refs":[...]}`. A direct
ref has `kind`, `code`, `bucket`, `offset`, `status`, `state_id`, `available_at`,
`cik`, `basis`, `source`, `period_start`, `period_end`, `inputs_hash`, and
`definition_hash`. Item refs use actual standardized state identity, CIK,
source and span. Metric refs pin the dependency row ID, input hash and
definition hash. Inapplicable fields are JSON null. `status=missing` retains
NULL-selected operands; literal-only expressions have no source leaf and cannot
qualify issuer ownership. Exact serialized UTF-8 bytes are SHA256 hashed.

The canonical publisher follows DSL arithmetic, window offsets, coalesce's
winning branch, direct annual fallback and weighted annual share selection.
It sorts and deduplicates direct refs before serialization. It retains the
existing `inputs_hash` candidate-frame meaning, but adding retained CIK to the
candidate state changes its serialized bytes: rebuilt `inputs_hash` values
generally differ from pre-DL1 history. Downstream materializations or pins to
that fingerprint need rebuilding. The deterministic derived ID formula is
unchanged and does not include `inputs_hash`; an unchanged retained event tuple
keeps its ID, but the retained event set can change, so ID-set compatibility is
not promised. Existing rows are not upgraded in place. Same-value selected-ref
changes participate in event compression. A row limit of 128 refs and 32 KiB serialized bytes is checked
for the whole staged security before prior scope deletion; options allow tighter
caps. Candidate, frame and publication row limits remain in place.

`atx_db.derived_lineage.qualify_selected_lineage(con, root_ids, *,
expected_cik=None, decision_cutoff=None, expected_definition_hashes,
max_depth=16, max_nodes=512, max_bytes=1048576, batch_size=256)` returns a
mapping from each root ID to `LineageQualification(status, reason,
selected_cik, leaf_ids, leaf_ciks, input_clocks, fiscal_ends, digest)`.
`expected_definition_hashes` must map `(metric_code, metric_window)` to the
approved exact hash for every reachable node. The reader batches table fetches,
preflights SQL payload byte lengths before fetching each page, and defaults to
4,096 total retained nodes and 8 MiB total retained evidence per batch,
checks root/parent clocks and interval visibility, retained IDs and raw source
fields, all selected CIKs, hash pins, reconstructed history, source, definition,
cycles and limits. `decision_cutoff=None` qualifies each root at its own event
for an immutable proof cache. Failures return empty evidence and an explicit
status: `mismatch`, `missing`, `legacy_unverifiable`, `invalid`,
`limit_exceeded`, or `definition_unverified`.

First guarded focused run was stopped by the host headroom guard before any
pytest output (`fresh-dl1-tests1-memory.json`: physical free 2.33 GiB,
commit free 2.91 GiB at stop). This is a guard stop, not a test failure. The
new tests now use a 256 MiB isolated in-memory minimal schema; no full warehouse
bootstrap is needed for their inner loop. Guarded run `fresh-dl1-tests2` found
DuckDB reserves `offset` in `struct_pack`; quoting that field fixed the SQL.
Guarded `fresh-dl1-tests3` passed six tests, peak 0.6825 GiB. Guarded
`fresh-dl1-tests4` passed eight tests, peak 0.6822 GiB, covering direct and
nested refs, TTM offsets, annual unused candidate, coalesce winner, same-value
source transition, NULL owner evidence, cross-CIK rejection, tampering, missing
and cyclic graph, cutoff, deterministic chunking, oversized pre-delete scope,
and a minimal 0323 migration. Both completed under the 1.5 GiB job limit.
Fresh review found one Important gap: metric-ref source and fiscal span were not
compared to the referenced child row. That reader check and a tampered-hash
fixture were added. `fresh-dl1-tests5` ran 11 tests: 10 passed (including the
new source/span and lag checks); the weighted-share fixture assumed annual
shares win over a valid quarterly denominator, which contradicts the existing
numeric plan. Its quarterly denominator was changed to NULL to exercise the
actual annual branch. `fresh-dl1-tests6` was refused at guard preflight due to
low host headroom; no child process started. `fresh-dl1-tests7` exposed a
fixture-only span mismatch in the synthetic weighted ratio. The revised fixture
uses an instant common-equity numerator and a NULL quarterly weighted-share
state, matching the existing annual-share branch predicate. Guarded
`fresh-dl1-tests8` passed all 11 checks, peak 0.6830 GiB. Guarded scoped Ruff
`fresh-dl1-ruff1` passed on all owned Python paths, peak 0.0657 GiB.

Affected full-schema regression commands remain for the coordinated runtime
slot: `pytest -q -n0 tests/test_derived_metrics.py
tests/test_derived_annual.py tests/test_derived_pit_revisions.py` from
`atx-db/`, under the same host guard. The root's first full-schema bootstrap
was stopped by the host headroom guard before test output; these regression
checks are not passed yet. The proof does not certify historical
delivery vintages beyond the warehouse's reconstructed available-at policy.
