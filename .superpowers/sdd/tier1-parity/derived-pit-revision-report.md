# Derived PIT revision repair — implementation handoff

Date: 2026-09-20. Branch: `feat/tier1-parity`. Status: implementation complete for independent static review; **runtime verification and production reconstruction remain unperformed**. No DB connection, tests, probes, package imports, lint process, production process control, network/API work, or commits were run by this implementer. Companyfacts archive3 remains exclusively controlled by root.

Read first: `derived-pit-revision-brief.md`, `derived-pit-revision-audit.md`, then program/continuation rulings. Graph tools were unavailable; discovery used source reads and `rg` fallback. No CSV/core-breadth definition edits, no `derived_registry.py` edits, no changes to historical migrations, source loader, frozen compatibility formulas, or DSL formula lowering. This change works with the original 173 definitions as well as the concurrent 201-definition tree; it imports no core-breadth test/helper.

## Implemented state algorithm

1. For each security, retain admitted quarterly/instant standardized revisions, including explicitly present NULL numeric states, without filtering `is_latest_revision`. Copy only necessary state columns into SQL temporary storage after a row-count guard.
2. Construct whole input states per code/calendar bucket at each availability event. Precedence is newest visible period, then availability, then descending source/rule/basis/stable standardized ID. Revision counters and warehouse load time do not break ties. Running `arg_max(struct, key)` and equal-event `RANGE` windows see all same-time candidates before choosing one. Compress unchanged selections and derive half-open `valid_to` intervals.
3. Construct bucket representative-period transitions from observations visible at each event. A future stub period neither renames a past state nor creates a target before observation. Entirely absent target buckets do not emit rows; missing buckets inside a frame remain real dense NULL rows.
4. In DAG order, map each direct input/dependency event only to offsets `0..local_max_lag`, union target-appearance/representative-date events, and ASOF-join visible target metadata. No security-event × whole-history expansion exists. This conservative offset set supports all registry expressions without metric-specific formulas.
5. Process bounded target/event key chunks. Each key receives exactly `local_max_lag + 1` dense calendar rows. ASOF equality keys include security/code/bucket; complete input interval relations remain available across every chunk, including predecessor states. SQL windows partition by security and target/event key. Only the target row is emitted.
6. Persist NULL invalid results with a nonnull transition `available_at=event_at`. Statuses are `valid`, `zero_denominator`, `nonfinite`, or `missing_input_or_domain`. The explicit zero diagnostic covers a top-level division/`safe_div`; other missing/domain failures share the general status, without changing their formulas. `arithmetic_available_at` remains the DSL's arithmetic/selected-branch clock, which may precede a control-flow transition.
7. Across all chunks, compress only identical selected-frame lineage/value/status/representative-period states. Same-value lineage changes survive. Revision sequence/count/latest/`valid_to` are convenience metadata over the retained group stream.

`revision_group_id` hashes typed source/security/metric/window/calendar-bucket/definition identity. `derived_value_id` adds event time and the event-visible period date. `definition_hash` includes engine contract, formula, inputs, metric/window and definition version. `inputs_hash` hashes the definition and an ordered typed frame containing the actual selected source/dependency states, including IDs, values, clocks and source/definition provenance. There are no delimiter-concatenated canonical state identities or run/load-time identifiers. A same-event source correction deterministically replaces that event on rebuild; this is not a separate warehouse-observation vintage dimension.

The conservative frame lineage includes selected states throughout each expression's bounded frame, even where control flow chooses another branch or an offset is numerically unused. Such lineage changes can retain an additional same-value event; they do not remove or backdate an observable state.

## Scope, publication and resource bounds

The atomic publication scope is **one source/security and the resolved quarterly metric closure**. Its entire DAG is staged before a short explicit DELETE/INSERT/COMMIT transaction. A failed INSERT or COMMIT attempts rollback. Other sources/securities are untouched. Earlier complete securities may remain committed if a later security fails, but the call raises; a partial invocation is never reported as a successful rebuild. Unscoped selection includes existing canonical securities whose raw inputs disappeared, so a successful empty rebuild clears stale rows. Unscoped replacement also removes obsolete metric codes in that source/security.

Requested metrics are closed to a fixed point over prerequisites **and descendants**. Recomputing a prerequisite can change the state of sibling consumers, so a scoped request can rebuild its connected metric family. Daily panels/projections remain separate activation stages and must be rebuilt after quarterly reconstruction.

Defaults in `DerivedMetricsOptions`:

| Limit | Default | Guard/location |
|---|---:|---|
| Input standardized rows per security | 250,000 | Count before `_pit_raw` materialization |
| Candidate event upper bound per metric | 250,000 | Before offset expansion: `(item_events + dependency_events) * width + target_metadata_events` |
| Materialized distinct candidate keys | 250,000 | Count after visible-target selection |
| Keys per evaluation chunk | 1,024 | Further reduced by frame-row budget |
| Dense rows per evaluation chunk | 8,192 | `chunk_size=min(event_chunk_size, max_frame_rows // width)`; oversized width fails |
| Complete staged publication rows | 100,000 | Checked after every metric and before publication |
| Existing rows deleted in publication scope | 100,000 | Counted before opening transaction |
| Python security metadata block | <=500 IDs | Separate SQL result cursor + `fetchmany`; no universe-wide Python fact/value panel |

Let E be local input/dependency events, T target-metadata events, W local frame width, and K distinct visible candidate keys. K <= E×W+T. Evaluation frame work is K×W, with at most 8,192 rows resident in each chunk's explicit frame relation; joins add a finite registry-defined input-column count, not another history axis. Input and stage relations remain in SQL and can spill under the existing analytical session budget. `_pit_metric` can contain up to the candidate limit. A just-completed metric can temporarily bring staging to the previous scope limit plus the candidate limit before the scope guard raises. Publication touches at most the bounded old+new scope, never a full-universe indexed transaction or index build.

These are row/work bounds, **not measured byte-capacity guarantees**. The engine does not raise RAM or thread limits. Root's existing 1GB/1-thread session and 3GiB process-tree guard remain required. Very large issuers fail explicitly before publication; no issuer is silently skipped. No optional ART index was added. Migration0315 removes the existing optional derived lookup ART index while retaining the required physical table and primary key.

Removing that optional index permits the nullable-value schema alteration without retaining its secondary dependency; its performance and resource effects have not been measured and remain for reviewer/root assessment.

Staging is connection-local and is not a durable resumable generation ledger. Completed security scopes are durable/atomic; interrupted scopes are rebuilt from retained inputs on retry. Production capacity of the configured limits, incremental required-PK publication, COMMIT rollback, and the independent identifier result cursor must be validated by root. No unbounded fallback was implemented.

## Canonical schema and consumer closure

Migration0315 evolves `derived_metric_values.value` to nullable and adds status, revision group/sequence/count, `valid_to`, target bucket, definition fingerprint, arithmetic clock and history classification. Old rows are retained as `history_status='legacy_latest_only'` with no invented complete revision group. Rebuilt rows explicitly use `event_reconstructed`. Table/field/dataset catalog wording and schema-contract pin are updated without flipping any measured coverage condition.

- Daily raw standardized and DEI selectors use all historical states, deterministic same-event ties and newest-visible-period precedence. Invalid DEI counts are selected first, then mapped to unavailable so the existing archive fallback is chosen without resurrecting older positive DEI history.
- Daily derived selection ranks whole nullable states. Raw Q4 remains newer than an amended Q2, while Q4 TTM changes when the Q2 amendment affects its window. Daily lineage now includes selected canonical state IDs/input hashes and the selected DEI identity.
- Quarterly exports select derived state by bucket/event before extracting nullable value; they never `arg_max` a nullable value across revisions. Raw export ties match source/rule/basis/stable-ID precedence. Panel export contract is 2.0.0.
- Quarterly factor projection ranks whole states with stable event IDs rather than source-loaded timestamps. The selected state ID/status/input hash is passed into factor lineage. Numerical sign/winsorization/z-score and legacy compatibility formulas are unchanged.
- Public derived schema is 2.0.0 with nullable values, event identity/status/history/revision metadata, and revision-group logical key. API `latest`/`first_reported` ranks visible states before extracting values; legacy keys fall back to their physical ID. Representative-date range filtering occurs after group ranking, preventing an old stub date from being resurrected. Stable state ID replaces load/run time in derived tie ordering. Daily schema is 1.1.0 with honest modeled-clock/history wording.
- Publication continues exporting every canonical row keyed by `derived_value_id`, including invalid and same-value revision states. No valid-only compatibility surface was introduced.
- Provider coverage, derived-family quality and activation's metric count now require `is_latest_revision AND value_status='valid' AND value IS NOT NULL AND isfinite(value) AND history_status='event_reconstructed'`. This measures current finite reconstructed-state breadth over the existing period/metric basis; it does not prove SLO thresholds, source completeness, listing eligibility or vendor vintage quality.

Upstream limits remain explicit: omitted concepts do not constitute withdrawals, and reconstructed modeled filing chronology does not establish historical vendor delivery or local observation-time replay. Legacy scopes must be reconstructed before they can support PIT certification. Daily and projected tables must be rebuilt from the repaired stream; loading migration0315 alone cannot repair their historical values.

## Prepared root verification

No checks have been executed. After the live writer exits, run the following once from `C:\atx\atx-db`, under a 2.5GiB process guard and 1GB/1-thread test/session setup. Both guard and pytest explicitly use the pinned project Python executable, avoiding the previously observed system-DuckDB mismatch. The unique suffix supplies fresh stdout/stderr/receipt paths for each authorized invocation:

```powershell
$pitReviewRun = 'derived-pit-focused-' + [guid]::NewGuid().ToString('N')
& 'C:/atx/atx-db/.venv/Scripts/python.exe' 'C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py' --job-gb 2.5 --receipt "C:/atx/.superpowers/sdd/tier1-parity/$pitReviewRun-memory.json" --stdout "C:/atx/.superpowers/sdd/tier1-parity/$pitReviewRun-stdout.log" --stderr "C:/atx/.superpowers/sdd/tier1-parity/$pitReviewRun-stderr.log" -- 'C:/atx/atx-db/.venv/Scripts/python.exe' -m pytest -n0 -q tests/test_derived_pit_revisions.py tests/test_derived_metrics.py tests/test_market_daily.py tests/test_panel_export.py
```

The new focused file covers the exact 460→510 amendment and PS10 example, gross-margin dependency effects, target+4 growth and sibling invalidation, invalid/recovery/`valid_to` states, coalesce fallback clock, NULL-safe API/export/publication and finite breadth, delayed quarter arrival, same-value lineage, same-time ties, future stub dates, chunk equivalence, unaffected security scope, prepublication limits, required-PK failure rollback, historical direct raw/DEI daily joins, and measured candidate/frame growth for 20/40-quarter synthetic issuers. It reuses `tmp_store` and uses only tiny fixture values.

Existing formula tests now explicitly choose their latest state before presenting a valid numeric result, while the new file verifies stored invalid states directly. The old “zero denominator emits no row” and “all rows are nonnull” assertions became status/value consistency assertions. Schema tests expect revision-group key, nullable value and version2. Security batch helper callers materialize the now-streaming iterator when asserting its contents. Existing batch-equivalence ordering is fully deterministic.

Root should run touched-file Ruff and schema bootstrap/pin checks with the same slot. Fresh migration315 bootstrap and an upgrade containing legacy rows remain essential runtime concerns. Public schema snapshot/data-dictionary regeneration belongs to the existing root integration gate once source ownership is stable. No generated documentation or frozen parity artifact was modified here. Core breadth's separate test can be added to root's joint working-tree verification but is not required to import or commit P1 against the original173 catalog.

Post-review test preparation: `derived-pit-revision-fix1-report.md` records Important I1 and Minor M1. The focused file now includes a real populated0314-to0315 upgrade with legacy/API/coverage/PK/catalog-pin/re-entry assertions, plus direct invalid daily/factor and API first-reported/stub-range assertions. These cases are included in the same root command above; none has been executed by the implementer. The independent review closed the original Critical at source level and found no new Critical; no additional review pass is required for these prepared noncritical verification fixes.

One independent Codex static implementation review should explicitly close the original Critical audit finding. A scoped rereview is needed only for a new Critical finding requiring fixes. Runtime evidence remains root-owned. No commit until root explicitly dispatches it after verification.

## Exact owned file inventory

- `atx-db/src/atx_db/_derived_pit.py` — new private event/frame/state helper.
- `atx-db/src/atx_db/derived_metrics.py` — state writer, bounded options/identifier streaming, closure and scoped transaction.
- `atx-db/src/atx_db/market_daily.py` — historical raw/DEI/derived selection and selected-state lineage.
- `atx-db/src/atx_db/panel_export.py` — nullable state ranking and version.
- `atx-db/src/atx_db/derived_factor_projection.py` — deterministic whole-state selection and state lineage.
- `atx-db/src/atx_db/api/catalog.py` — public state fields/keys/versions/clock description.
- `atx-db/src/atx_db/api/service.py` — derived API group/tie/range semantics.
- `atx-db/src/atx_db/provider_coverage.py` — derived-only finite/current/reconstructed stats predicate.
- `atx-db/src/atx_db/quality/checks_identities.py` — derived-family finite/current/reconstructed predicate.
- `atx-db/src/atx_db/publication.py` — document preservation of all physical state keys.
- `atx-db/src/atx_db/activation.py` — root-authorized derived metric count predicate only.
- `atx-db/src/atx_db/migrations/bodies_0315.py` — forward canonical schema/catalog migration.
- `atx-db/src/atx_db/migrations/registry.py` — 0315 import/list entry only.
- `atx-db/src/atx_db/migrations/__init__.py` — 0315 facade cleanup entry only.
- `atx-db/tests/test_derived_pit_revisions.py` — new focused correctness/resource cases.
- `atx-db/tests/test_derived_metrics.py` — historical state-aware existing assertions.
- `atx-db/tests/test_panel_export.py` — changed public schema contract assertion.
- `.superpowers/sdd/tier1-parity/derived-pit-revision-report.md` — this report.
- `.superpowers/sdd/tier1-parity/derived-pit-revision-fix1-report.md` — issue-by-issue post-review test preparation.
