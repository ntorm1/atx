# Surface Database Production Implementation — Complete Code Review

- **Review date:** 2026-07-25
- **Branch:** `feat/surface-db-prod`
- **Reviewed tip:** `7a01fd3093046df3814eb24c1145fbf4c5abe5bf`
- **Comparison base:** `2858cab14489c64312d126c1c45637a5e5258b25` (`main` merge base)
- **Branch delta:** 56 files, 17,294 insertions, 197 deletions
- **Disposition:** **Request changes — the implementation is not production-ready at its claimed scale or for unattended operation.**

## Executive summary

The sprint delivers a substantial, coherent vertical slice: a date-partitioned OPRA hive, deterministic C++ loading, automatic per-symbol configuration, cell-aware surface population, resumability, production CLIs, administration/verification commands, Python exposure, migration and pull tools, extensive tests, and a real July 2026 production exercise. The implementation is unusually explicit about several of its limitations, and the focused regression suite is strong.

The current production artifact is also useful: the latest status records 17 partitions, 51 configured symbols, 858 surfaces, and a successful zero-refit second pass. That demonstrates a functioning constrained workflow. It does **not**, however, establish that the implementation meets the design's production or million-surface posture.

There are four release-blocking problems:

1. **The migration tool can delete destination-only paid data.** If an existing date contains some but not all source underlyings, migration rewrites the file from the v1 source alone.
2. **A failed refit deletes the previously stored surface.** This is intentional and tested, but an operational mistake such as a wrong interest rate already destroyed 95 stored surfaces in the production exercise.
3. **The loader is not memory-bounded by date.** Discovery retains every readable date table, the returned batch retains every panel, and the build materializes every board before population starts. The full-month production attempt reached 6.5 GB RSS with zero partitions written and had to be killed.
4. **The per-date “split” repeatedly scans the entire date table once per symbol.** Each symbol load performs multiple full-row scans; distinct-underlying validation also uses a linear vector search. This is incompatible with the stated target of roughly 4,000 symbols × 250 sessions.

Several high-severity issues compound those blockers: corrupt-only input exits successfully with an empty database, carry validity ignores rate/data/build identity, a total new-fit failure beside carried cells exits zero, scheduler setup failure can deadlock the streaming drain, and the Python API cannot set the interest rate required by the production build.

The appropriate merge posture is:

- Do not label or ship this branch as production-complete.
- If the branch must merge for continued development, label the workflow beta/operationally constrained and prevent use of the migration tool against non-empty destination dates until its union semantics are fixed.
- Require fresh database roots, explicit and independently checked `--r`, small date chunks, and before/after verification baselines.

## Scope and method

The review covered:

- the complete committed branch delta from the merge base through `7a01fd3`;
- the frozen [design](2026-07-22-surface-db-production-design.md) and [implementation plan](2026-07-22-surface-db-production.md);
- the C++ hive loader, panel conversion, config generator, build driver, population scheduler, archive/manifest mutation, administration and CLI layers;
- the Python bindings, migration tool, pull tool, tests and operational documentation;
- the [production run log](../../atx-vol/research/2026-07-surface-db-production-run.md) and the latest [status report](2026-07-25-surface-db-prod-status-3.md);
- clean builds, focused native tests, focused Python tests, and two isolated negative-path reproductions.

The repository's requested codebase knowledge-graph tools were unavailable in this environment. Discovery therefore used the branch diff and targeted source inspection. No implementation source was edited.

Pre-existing uncommitted `.superpowers/sdd/task-*` changes and the untracked `X/` directory were left untouched and excluded from the committed implementation baseline.

## Severity definitions

| Severity | Meaning |
|---|---|
| Blocker | Can lose data, invalidate production results, or prevents the stated production-scale use case. |
| High | Can silently produce an incomplete/stale result, hang unattended operation, or makes a promised interface materially unsafe. |
| Medium | Important correctness, observability, durability, packaging, or workflow gap with a practical workaround. |
| Low | Documentation, naming, or future-scale issue that does not presently invalidate results. |

## Findings summary

| ID | Severity | Area | Finding |
|---|---:|---|---|
| C-01 | Blocker | Correctness | Partial-overlap migration deletes destination-only symbols. |
| C-02 | Blocker | Correctness | A failed refit removes an existing stored surface during whole-partition rewrite. |
| C-03 | High | Correctness | Carry validity fingerprints configs only, so rate/data/snapshot/build changes reuse stale surfaces. |
| C-04 | High | Correctness | A wholly corrupt hive window can produce exit code 0 and an empty database. |
| C-05 | High | Correctness | A systematic total failure of all newly scheduled cells exits 0 when any old cell was carried. |
| C-06 | High | Correctness | Scheduler setup or worker-launch failure can leave the date drain waiting forever. |
| C-07 | Medium | Correctness | Invalid civil dates such as `2026-02-31` are accepted and normalized to another date. |
| C-08 | Medium | Correctness/diagnostics | Auto-config failures and deep-selection fallbacks discard their reasons. |
| C-09 | Medium | Durability | Partition and manifest replacement is not one durable transaction; crash windows remain. |
| P-01 | Blocker | Performance | Hive/build orchestration retains the full requested window in memory before population. |
| P-02 | Blocker | Performance | The in-memory symbol split repeatedly scans the full table, with superlinear distinct checks. |
| P-03 | High | Performance/recovery | Expensive loading must finish before the first partition is committed, wasting long failed runs. |
| P-04 | Medium | Performance/feature | Explicit symbol subsets do not use the design-promised row-group pruning. |
| F-01 | High | Feature gap | Python cannot set `r` or reproduce the production CLI's market-input behavior. |
| F-02 | High | Feature gap | There is no force-refit/input-version mechanism to repair stale or mispriced surfaces safely. |
| F-03 | Medium | Feature gap | Verification does not fail on absent requested cells unless an operator supplies a threshold. |
| F-04 | Medium | Feature gap | Failed-cell state is not persisted, so permanent failures are retried forever. |
| F-05 | Medium | Feature gap | Production CLIs are example-gated build-tree executables with no install/package rule. |
| F-06 | Medium | Feature gap | Pull-tool “actual spend” is a sampled estimate, not an actual charge. |
| F-07 | Medium | Feature gap | The intended Python notebook workflow is unusable with PyArrow on the documented Windows setup. |

## Correctness review

### C-01 — Partial-overlap migration deletes destination-only data

**Severity: Blocker**

The design requires an existing date file to be rewritten as the **union** of existing and newly available symbols. The pull tool implements that merge discipline; the migration tool does not.

`migrate()` obtains the source-underlying set and skips only when the destination set is already a superset. Every other existing-destination case falls through to `_merge_date`:

- [`migrate_opra_hive.py:238-253`](../../atx-vol/tools/migrate_opra_hive.py#L238-L253) performs the superset check and then calls `_merge_date`.
- [`migrate_opra_hive.py:183-198`](../../atx-vol/tools/migrate_opra_hive.py#L183-L198) reads only `src_files`, concatenates them, and replaces the destination.
- The function's own commentary at lines 119-132 correctly describes how a source-only replacement discards destination-only underlyings, but the normal partial-overlap path still does exactly that.

Isolated reproduction:

```text
destination before: AAA, ZZZ
v1 source:          AAA, MMM
destination after:  AAA, MMM
destination-only ZZZ lost: true
```

The tests cover an already-complete destination, a destination superset, footer-statistics fallbacks, and a multi-underlying row group. They do not cover the legitimate partial-overlap union `dst={AAA,ZZZ}`, `src={AAA,MMM}`.

**Required change:** read and validate the existing destination when it is not a proven source superset, merge destination plus source by underlying, define conflict precedence explicitly, atomically replace only after validating the union, and add a regression test that proves destination-only rows survive.

Until then, the tool should refuse to rewrite a non-empty destination unless an explicit destructive override is supplied.

### C-02 — A failed refit deletes an existing surface

**Severity: Blocker**

Population rewrites a whole date partition. A carried cell is re-emitted, but an enabled cell selected for refit is omitted when its fit fails:

- [`surface_db_populate.cpp:509-570`](../../atx-vol/src/surface_db_populate.cpp#L509-L570) records the failed cell without appending an item for it.
- The resulting whole-date write replaces the old partition, so the old cell disappears.
- `SurfaceDbPopulate.DegradedCellLosesItsStoredSurfaceAndPresenceIsWhatDrivesTheRetry` explicitly pins this behavior in a test.

The reasoning in the source is understandable: retaining old bytes without recording their staleness would make a failed current fit appear healthy. The selected alternative is nevertheless destructive and unsafe for a production updater. The production exercise demonstrated the impact: one invocation with the wrong `--r` removed 95 previously stored surfaces.

This is not merely a theoretical “bad operator input” issue. Rate is an ordinary build input, not stored in or checked against the database, and the Python binding cannot set it at all.

**Required change:** stage a complete candidate partition and apply an explicit commit policy. At minimum, a regression in previously present coverage should fail closed and leave the old partition untouched unless the caller supplies an `--allow-coverage-regression`/retirement policy. A stronger design would preserve the prior surface with a persisted stale/failed status that prevents it from being presented as current.

### C-03 — Carry validity ignores the inputs that determine a surface

**Severity: High**

The partition carry predicate is only a fingerprint of stored per-symbol fit configs:

- [`surface_db.hpp:288-368`](../../atx-vol/include/atx/vol/surface_db.hpp#L288-L368) explicitly states that the fingerprint is not a build fingerprint and excludes `OpraHiveSpec::r`, snapshot time, hive contents, and writer/fitter identity.
- [`surface_db_populate.cpp:768-801`](../../atx-vol/src/surface_db_populate.cpp#L768-L801) carries a whole partition whenever the nonzero stored config fingerprint matches.
- `ArchiveV2Header::writer_version_hash` exists, but the current build path does not supply a meaningful build identity.

Consequences:

- correcting a wrong rate does not invalidate existing cells;
- replacing or correcting a hive date does not invalidate its surfaces;
- changing snapshot conventions or market inputs does not invalidate them;
- deploying fitter/model logic changes does not invalidate them;
- a date selected for rewrite can carry stale sibling bytes verbatim.

Integrity verification proves that stored bytes and indexes are structurally sound; it cannot prove that the bytes were fitted from the intended input.

**Required change:** add a versioned build-input fingerprint containing at least source-date content identity, rate/yield/dividend inputs, snapshot convention, effective config, model/fitter version, and relevant numerical-policy version. Store it per cell or per partition and make mismatch force refit. Provide a deliberate full-refit override.

### C-04 — Wholly corrupt input exits successfully

**Severity: High**

The batch loader treats per-date read failures as cell results rather than a top-level error. The build report records `n_load_errors`, but the CLI only returns a failure for top-level errors, total config failure with a nonempty configured universe, or total scheduled-fit failure.

[`surface_db_build_main.cpp:475-522`](../../atx-vol/tools/surface_db_build_main.cpp#L475-L522) does not make `n_load_errors > 0` or “no readable requested date” an error.

Isolated CLI reproduction with a date file containing non-Parquet bytes:

```text
exit code:        0
n_dates_loaded:   0
n_dates_missing:  1
n_load_errors:    1
configured:       0
cells_to_fit:     0
cells_ok:         0
stderr:           empty
```

This is dangerous in schedulers because “the entire requested input is corrupt” is indistinguishable by exit code from an intentional empty/no-op window.

**Required change:** distinguish no files requested, no files present, all present files corrupt, and a healthy converged resume. A build with requested present data but zero readable boards and nonzero load errors must return nonzero. Add a CLI integration test, not only a library counter test.

### C-05 — Carried cells mask total failure of all new work

**Severity: High**

`is_total_fit_failure` was widened so a carry-only converged run is not treated as failure. The widening exempts any run with carried cells, including one where every newly scheduled cell fails. The CLI recognizes this ambiguous shape, prints a warning, and deliberately exits 0:

- [`surface_db_build_main.cpp:502-537`](../../atx-vol/tools/surface_db_build_main.cpp#L502-L537) documents the exemption and zero exit.
- Lines 561-573 state that the counters cannot distinguish a healthy permanent-failure retry from a systematic regression.

This is well tested, but the test verifies the chosen behavior rather than making unattended operation safe. A deployment can fail every new symbol/date while old carried data keeps the job green.

**Required change:** add a strict production mode, ideally the default, in which `scheduled > 0 && fitted == 0` is nonzero regardless of carried cells. Persist failed-cell identity/reason so the code can distinguish unchanged known failures from new or changed failures without relying on a human comparing logs.

### C-06 — Scheduler setup failure can deadlock the date drain

**Severity: High**

The streaming population path initializes a positive `remaining[date]` count and starts a helper thread. The main thread waits until every count reaches zero:

- [`surface_db_populate.cpp:318-334`](../../atx-vol/src/surface_db_populate.cpp#L318-L334) decrements counts only after a fit task starts.
- [`surface_db_populate.cpp:377-396`](../../atx-vol/src/surface_db_populate.cpp#L377-L396) launches the scheduler and waits on those counters before it can observe `fit_status`.
- [`fit_scheduler.cpp:223-250`](../../atx-vol/src/fit_scheduler.cpp#L223-L250) can fail while reserving/launching workers before any task enters.
- [`fit_scheduler.cpp:266-269`](../../atx-vol/src/fit_scheduler.cpp#L266-L269) can also return a scheduler-setup error before tasks execute.

In either pre-task failure, no `MarkDone` destructor decrements the positive counts. The helper returns with an error while the drain remains asleep forever; it never reaches the join/status check.

**Required change:** publish scheduler completion/cancellation separately from per-date completion and make each wait predicate include it. On setup failure, mark all unstarted tasks failed or cancel and notify every date. Add deterministic failure-injection tests for allocation and worker-launch failure through the full populate path.

### C-07 — Invalid calendar dates normalize silently

**Severity: Medium**

[`opra_batch_detail.hpp:90-107`](../../atx-vol/src/opra_batch_detail.hpp#L90-L107) validates only month `1..12` and day `1..31`. It accepts impossible dates such as `2026-02-31`; `days_from_civil` then normalizes the value, and directory enumeration proceeds using a different civil date.

**Required change:** validate a parsed date by a calendar-aware month/leap-year check or by round-tripping `civil_from_days(days_from_civil(...))` and requiring exact equality. Add invalid-February, leap-day, and 30-day-month tests.

### C-08 — Auto-config diagnostics lose the cause

**Severity: Medium**

[`surface_db_build.cpp:99-178`](../../atx-vol/src/surface_db_build.cpp#L99-L178) reduces all selection failures, hard errors, and caught exceptions to `false`. [`surface_db_build.cpp:257-267`](../../atx-vol/src/surface_db_build.cpp#L257-L267) then stores a disabled config and records only the symbol. Deep-selection `NotFound`/`Unavailable` fallback is also not reported.

The design requires failed symbols and reasons in the report. Current output tells an operator which symbol is disabled, but not whether the cause was an unusable chain, policy rejection, invalid selector input, exception, or expected deep-selection fallback.

**Required change:** return a structured config outcome with status, reason code/message, and `used_fallback`; persist/report it with bounded terminal rendering and complete CSV output.

### C-09 — Partition plus manifest update is not one durable transaction

**Severity: Medium**

Individual files use temporary-write-plus-rename, which protects against partial file contents. The archive and manifest are still separate replacements, with a crash window between them, and the write path does not establish fsync durability before rename. There is also no generation compare-and-swap for concurrent external writers.

The verifier's partition-index cross-check is a valuable detector, but detection after restart is not atomic commit.

**Required change:** document single-writer enforcement as a hard requirement; use a small journal/commit marker or recoverable generation protocol for archive+manifest replacement; flush file and parent-directory metadata before declaring success; and reject stale external-writer generations.

## Performance review

### P-01 — Full-window memory retention defeats the scale posture

**Severity: Blocker**

The loader/build interface is batch-shaped rather than streaming:

1. In discovery mode, [`opra_hive.cpp:131-174`](../../atx-vol/src/opra_hive.cpp#L131-L174) reads every present date and stores its full `ParquetTable` in `DateInfo`.
2. [`opra_hive.cpp:223-283`](../../atx-vol/src/opra_hive.cpp#L223-L283) moves those tables into a task vector, retaining all dates that have queued loads.
3. The panel pass completes the entire `OpraBatchResult`.
4. [`surface_db_build.cpp:301-352`](../../atx-vol/src/surface_db_build.cpp#L301-L352) converts all successful panels into one `boards` vector before calling the nominally streaming population layer.

Therefore peak live state scales with the whole requested date × symbol window, not one date or a bounded pipeline depth. The later population drain cannot recover memory already committed to the loader/batch boundary.

Measured evidence from the production run:

- full-month attempt: 6.5 GB resident after 16 minutes, zero partitions written, killed on a 15.7 GB machine;
- six-session chunk: 5.7 GB resident;
- low-memory attempt: hard `CHECK failed: raw != nullptr`, process death, zero partitions after 17 minutes.

This fails the stated million-surface posture before the production trial reaches even 51 symbols × one month.

**Required change:** replace the batch return boundary with a bounded per-date producer/consumer pipeline: read one date, discover/index rows, produce boards, fit/write that date, release tables/panels/boards, then advance. If global symbol discovery is required, perform a footer/underlying-only first pass rather than retaining full tables.

### P-02 — Per-symbol splitting repeatedly rescans the date table

**Severity: Blocker**

The implementation calls the table loader once per requested/present symbol:

```cpp
for (const SymbolLoad& sl : task.loads) {
  entry.panel = load_opra_cbbo_from_table(tbl, sl.load);
}
```

See [`opra_hive.cpp:302-313`](../../atx-vol/src/opra_hive.cpp#L302-L313).

Each call to the table seam then:

- scans all rows to collect distinct underlyings and find the filter;
- inserts each encountered underlying into a `vector` using `std::find`;
- scans rows for provenance mappings;
- scans rows again to filter and build quotes.

See [`opra_panel.cpp:498-559`](../../atx-vol/src/opra_panel.cpp#L498-L559) and [`opra_panel.cpp:580-702`](../../atx-vol/src/opra_panel.cpp#L580-L702).

For `S` symbols and `N` rows in a date, this performs at least `O(S × N)` full-row work. Because the distinct-underlying vector lookup is linear in the number of seen symbols and is repeated for every requested symbol, that validation can approach `O(N × S²)` over the date split. It also allocates `rows.reserve(n_rows)` for each symbol even though only a fraction of rows survive.

At the design target of roughly 4,000 symbols per date, this architecture is not a plausible million-surface implementation.

**Required change:** validate/index the table once per date. Since rows are sorted by `(underlying, symbol)`, build contiguous `[begin,end)` spans in one pass and feed slice views to a loader that does not rescan other symbols. A hash/set can validate distinct identities in linear expected time if sorted-span validation is not used.

### P-03 — Long failures commit no useful progress

**Severity: High**

The build waits for the complete hive load and board materialization before `populate_universe_streaming` is called. A loader OOM/check failure therefore loses all work even though dates are the intended resume unit. This is exactly what the production run observed twice: 16–17 minutes of work and zero partition commits.

The population layer itself has useful date-drain behavior; the orchestration boundary prevents it from delivering end-to-end streaming.

**Required change:** fuse load → config resolution → fit → verified partition commit into a bounded date pipeline, with a checkpoint after each date.

### P-04 — Explicit subsets do not use row-group pruning

**Severity: Medium**

The frozen design is internally inconsistent: lines 71-74 call pruning a future optimization, while line 116 promises that per-symbol subset loads use row-group pruning. The implementation always calls `io::read_parquet(task.path)` for an explicit subset and filters in memory.

This means a small symbol repair still reads and materializes the whole 4,000-symbol date file.

**Required change:** resolve the contract in the design and documentation. For production-scale partial repair, use Parquet row-group statistics/filter pushdown or write a row-group-aware reader that reads only overlapping underlying ranges.

## Feature-gap review

### F-01 — Python cannot reproduce the production build

**Severity: High**

[`python/src/bindings/surface_db.cpp:112-131`](../../atx-vol/python/src/bindings/surface_db.cpp#L112-L131) exposes roots, dates, symbols, index, preset, deep selection, and worker count. It does not expose:

- `OpraHiveSpec::r`;
- yield-curve pillars;
- per-date/symbol market inputs and missing-input policy;
- snapshot suffix/time policy;
- `retry_disabled`;
- `pin_curve_family`;
- a force-refit policy.

The binding consequently leaves `r = 0`. Its test fixture explicitly prices at zero because “the build driver has no rate knob.” Production required `--r 0.043`, so Python cannot safely create or update the same database.

The report binding also deliberately omits the complete `failed_cells` list, removing the detailed reasons needed for production diagnosis.

**Required change:** expose a typed build-spec object or parity-complete keyword surface, including `r`, and return all failure information.

### F-02 — No safe stale-data repair or deliberate full refit

**Severity: High**

There is no `--force-refit`, input-fingerprint invalidation, or transactional “rebuild and replace only if coverage is acceptable” mode. Correcting rate, data, snapshot, or fitter logic can leave old cells carried. Deleting partition files or using a fresh root is the documented recovery.

**Required change:** implement input-version invalidation plus explicit date/symbol/full refit controls. The default should preserve the old generation until the replacement passes integrity and coverage gates.

### F-03 — Absent cells are informational by default

**Severity: Medium**

Administration verification distinguishes corrupt cells from never-stored/absent cells, which is good. By default, absence does not fail verification. An operator must provide `--max-absent` and must know the expected baseline. The production incident therefore required before/after surface counts to reveal lost cells.

The core absence classification has tests, but the CLI exit/verdict behavior for `--max-absent` lacks an automated end-to-end test.

**Required change:** let verification consume an expected universe/date coverage manifest and fail on any unexpected absence. Add CLI integration tests for threshold exit codes and verdict text.

### F-04 — Permanent failures have no durable state

**Severity: Medium**

Failed cells are absent, so every unchanged rerun schedules them again and rewrites the date while carrying healthy siblings. This creates continuing compute cost and is the reason a converged run is operationally indistinguishable from a systematic new-work failure.

**Required change:** persist a failed-cell record containing input fingerprint, reason, first/last attempt, and retry policy. Retry automatically when inputs/build identity change; otherwise make retry explicit or backoff-controlled.

### F-05 — Production tools are not packaged as production tools

**Severity: Medium**

Both CLIs are inside the `ATX_BUILD_EXAMPLES` block at [`CMakeLists.txt:213`](../../atx-vol/CMakeLists.txt#L213) and [`CMakeLists.txt:432-443`](../../atx-vol/CMakeLists.txt#L432-L443). No install rules package them.

**Required change:** build them behind a tools/production option enabled in release packaging, add install rules, and publish version/build identity in `--version` and reports.

### F-06 — “Actual spend” is not actual

**Severity: Medium**

[`pull_opra_hive.py:520-523`](../../atx-vol/tools/pull_opra_hive.py#L520-L523) computes `actual_spend` as sampled preflight unit cost × boards written. It is not an invoice or an exact post-call charge, and it excludes requested cells that did not become written boards. The production log correctly adds a caveat, but CLI output still labels it `ACTUAL SPEND`.

**Required change:** label this `realized estimate`; record requested, returned, written, unmapped and failed cell counts separately; reconcile against provider billing/usage data when available.

### F-07 — Python/PyArrow notebook collision remains unresolved

**Severity: Medium**

The design and Python README document that importing `atxvol` and `pyarrow` in the same Windows interpreter causes an Arrow DLL collision. Tests work around it by spawning a separate process. That means the explicitly described notebook workflow—build and query alongside ordinary PyArrow data work—is not actually available in the target environment.

**Required change:** align/link/package Arrow consistently or isolate Arrow-dependent operations behind a process boundary exposed as a supported workflow.

## Sprint implementation coverage

| Sprint task | Status | Review |
|---|---|---|
| 1. In-memory `load_opra_cbbo_from_table` seam | Implemented | Strong parity and validation coverage. It becomes a performance bottleneck when invoked once per symbol because the seam assumes it owns a whole table. |
| 2. Synthetic multi-symbol hive fixture | Implemented | Useful fixture shared across loader/build tests. |
| 3. `load_opra_hive` | Functionally implemented, scale gate failed | Deterministic rectangular results, visible coverage holes, corrupt-file accounting, and thread-count parity are good. Full-window retention and repeated scans block production scale. |
| 4. `generate_symbol_configs` | Implemented with diagnostics gap | Fail-closed behavior, idempotence, retry-disabled, index recipe, pin policy, and deep selection exist. Failure/fallback reasons are lost. |
| 5. `build_surface_db` driver | Implemented with safety gaps | End-to-end orchestration and resume work. Empty/corrupt success, incomplete invalidation, destructive regression, and pre-population batching prevent safe unattended use. |
| 6. CLI and feature documentation | Implemented | Strict numeric parsing and detailed reports are positive. Exit semantics are unsafe for corrupt-only and carry-masked failures; tools are not installed/package-ready. |
| 7. Python bindings and pytest | Partially implemented | Basic build/query path exists, but missing rate/market controls means it cannot reproduce production. Detailed failed cells are omitted; PyArrow collision remains. |
| 8. Migration tool | Not correct for existing partial destinations | Basic migration/idempotence work; union/resume semantics are destructive on partial overlap. |
| 9. Pull tool | Mostly implemented | Merge/resume, cached DBN, cost preflight and cap degradation are useful. “Actual” spend is still an estimate. |
| 10. Production run | Completed as a constrained pilot | Produced and verified a useful 17-date/51-symbol artifact within estimated budget. It also empirically exposed memory failure, hard process abort, destructive wrong-rate behavior, and required manual chunking. |

## Test and validation assessment

### Commands run for this review

```text
git diff --check 2858cab14489c64312d126c1c45637a5e5258b25..HEAD
PASS

powershell -File scripts\atx-build.ps1 build all
PASS — 57 targets linked

atx-vol-tests.exe
  --gtest_filter=OpraHive.*:OpraPanelTable.*:GenerateSymbolConfigs.*:
                  BuildSurfaceDb.*:SurfaceDbTotal*.*:SurfaceDbAdmin.*:
                  SurfaceDbPopulate.*:SurfaceDbConfigFingerprint.*:SurfaceDb.*
PASS — 131 tests from 10 suites, 1 disabled

python -m pytest \
  atx-vol/python/tests/test_migrate_opra_hive.py \
  atx-vol/python/tests/test_pull_opra_hive.py -q
PASS — 25 tests
```

Negative-path reproductions:

- partial-overlap migration proved loss of a destination-only underlying;
- corrupt `date=.../data.parquet` proved exit 0 with one load error and no built cells.

### What the suite does well

- table/path loader parity;
- deterministic results across worker counts;
- missing-date, coverage-hole, corrupt-file and schema-error classification;
- config idempotence, overwrite/retry-disabled behavior and pin-policy constraints;
- end-to-end build, incremental new-date work and zero-refit carry behavior;
- byte-identical carried records;
- fit-failure reasons and stable ordering;
- checksum, payload corruption and manifest/partition index mismatch detection;
- disabled-symbol preservation;
- migration schema checks, footer fallback and destination-superset skip;
- pull merge/resume/cache/cap-degradation behavior.

### Material missing tests

1. Migration with legitimate partial overlap where both source and destination have unique symbols.
2. CLI nonzero behavior for an all-corrupt requested window.
3. End-to-end scheduler worker-launch/setup failure without a hang.
4. Rate/data/snapshot/fitter-version changes invalidating stored surfaces.
5. Coverage-regression commit policy preserving the previous generation.
6. Invalid but syntactically well-formed dates.
7. Million-shape or at least representative 4,000-symbol loader benchmarks with peak-RSS gates.
8. A bounded-memory multi-date integration test proving earlier partitions commit before later dates load.
9. CLI `--max-absent` verdict and exit-code integration tests.
10. Python/CLI parameter-parity tests, especially nonzero `r`.

## Positive implementation observations

These strengths should be retained during remediation:

- Deterministic date-major/symbol-major ordering is designed and tested carefully.
- Coverage holes are structurally distinguished from genuine data defects instead of inferred from shared error codes.
- Strict CLI numeric parsing avoids permissive partial values.
- Config selection fails closed rather than fitting with an accidental default.
- Disabled stored surfaces and healthy carried surfaces are preserved byte-for-byte.
- Carry attestation defaults closed when the config fingerprint is unknown.
- Fit failures retain the fitter's actual reason and are deterministically sorted.
- The verifier checks payload CRC, finite probes, partition availability, and manifest/archive index agreement.
- Individual file writes use temporary files plus replacement.
- The production log records failures and limitations candidly rather than presenting only the successful final artifact.

## Prioritized remediation

### P0 — required before production designation

1. Make migration a true destination/source union and add the partial-overlap regression test.
2. Prevent failed updates from replacing a healthier prior generation without explicit authorization.
3. Add a complete input/build fingerprint and a safe force-refit path.
4. Stream load/build per date with a bounded number of live dates.
5. Index/slice each date once instead of rescanning it per symbol.
6. Make corrupt-only input and all-new-work fit failure return nonzero in production mode.
7. Expose nonzero rate and the full production build spec in Python.

### P1 — required for reliable unattended operation

1. Make scheduler failure cancel and notify the streaming drain.
2. Persist failed-cell identity/reason and use it to distinguish stable retry state from regressions.
3. Add expected-coverage verification and default nonzero behavior for unexpected absence.
4. Add a recoverable archive+manifest commit protocol and enforce single-writer/generation rules.
5. Replace hard allocation assertions on input-driven paths with returned errors where feasible.

### P2 — production hardening

1. Record structured auto-config failure and fallback reasons.
2. Enforce real civil-date validation.
3. Package/install/version the two production CLIs.
4. Correct spend terminology and add billing reconciliation.
5. Resolve or formalize the Python/PyArrow process boundary.
6. Add row-group pruning for subset repair after the once-per-date index path is correct.

## Operational guardrails if this branch is used before remediation

These guardrails reduce risk but do not make the implementation production-ready:

1. Do not run `migrate_opra_hive.py` against a destination date that already contains any independently pulled data.
2. Build into a fresh database root whenever rate, hive data, snapshot convention, market inputs, configs, or fitter code may have changed.
3. Require an explicit reviewed `--r`; do not rely on the default.
4. Process small date chunks and retain at least several gigabytes of free memory.
5. Capture `surface-db info`, partition/surface counts, and full verification before every mutation.
6. After every build, compare counts and absent-cell lists with the prior baseline; use `--max-absent` in automation.
7. Treat any nonzero `n_load_errors`, any newly disabled symbol, or “WARNING (exit 0): 0 cells fitted” as a failed job pending review.
8. Keep the previous database generation available for rollback.

## Final assessment

The sprint is a strong prototype and a valuable operational experiment. It proves the core surface archive, deterministic fit path, reports, carry mechanism, and verifier can produce and serve a real small-universe database. It does not yet satisfy the “surface database production” label:

- correctness is undermined by two demonstrated destructive paths and incomplete input invalidation;
- performance is fundamentally batch-shaped and repeatedly rescans data, contrary to the claimed million-surface posture;
- essential production controls are missing from Python, verification, packaging, retry state, and generation management.

**Final recommendation: request changes.** Merge only as an explicitly constrained beta foundation, not as a completed production implementation, and close the P0 items before expanding beyond the current manually supervised July pilot.
