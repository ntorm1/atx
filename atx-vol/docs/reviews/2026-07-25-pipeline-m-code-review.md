# `feat/pipeline-m` production implementation code review

Date: 2026-07-25
Branch: `feat/pipeline-m`
Reviewed HEAD: `8a231205039728193ce0edeb1c3c47e9965678b1`
Implementation tip: `b05653819333d5c8fdc2909927a56a5ccd91b06f`
Merge base: `2858cab14489c64312d126c1c45637a5e5258b25`
Sprint plan: `atx-vol/sprints/2026-07-21-atx-vol-pipeline-sota-sprint.md`
Sprint closeout: `atx-vol/sprints/2026-07-25-pipeline-sota-sprint-final.md`

## Decision

**Request changes before treating this branch as production-ready.**

The branch contains substantial, valuable work: strict typed run configuration, deterministic
portfolio and archive paths, stronger corruption checks, durable archive-file publication,
improved fitting concurrency, live Python gates, and unusually candid sprint documentation.
The current `atx_vol` gate is green.

That evidence does not close the production review. The review found multiple high-severity
silent-wrong-number, data-race, stale-artifact, availability, and scale defects. The most
important are:

1. projected historical VaR builds the first historical date's book but calls the last
   revaluation the current reference;
2. configurable multipliers break the gross-vega risk-limit units except at the historical
   multiplier of 100;
3. tearsheet "gross vega" statistics consume signed net vega;
4. enabling market impact silently discards a configured vol-tick spread;
5. RunArchive section carry-forward does not include corpus or schedule identity and can
   preserve stale economic results;
6. benchmark-relative statistics discard benchmark dates and truncate unequal series;
7. Python quote mutation can race GIL-released fitting and valuation;
8. risk buckets accept a mismatched portfolio and can silently assign false expiries;
9. a fit-scheduler launch failure can deadlock `populate_surface_db`;
10. incremental SurfaceDb rewrite can delete a previously valid cell when its refit fails;
11. corpus checkpoint/index pairs are not a recoverable transaction;
12. the documented production report entry point cannot consume the production CLI's
    RunArchive-only output; and
13. loose-TSV reporting turns absent economic columns into authoritative zeroes.

There are also material performance risks: projected VaR retains copied full archives for every
date; the production corpus route retains the whole OPRA range before date batching begins; the
A7 scenario optimization allocates `cells * unique_contracts` doubles; and bucket reduction is
quadratic in rows times distinct buckets.

No implementation code was edited for this review.

## Review scope and method

The branch delta is large: **216 files, +33,005 / -1,740** against the merge base. The reviewed
HEAD changes only the sprint plan and final status after `b056538`; therefore source at HEAD is
the same implementation that was gated at `b056538`.

Three independent review lanes were used:

- correctness and financial semantics;
- performance, concurrency, and storage;
- sprint-plan, product-surface, Python, and feature coverage.

Each lane read the plan and closeout, traced the relevant production paths, inspected their
tests, and returned exact source references, impact, and remediation. Findings below were then
cross-checked against the current worktree and focused tests. The repository's requested
codebase-memory graph tools were not available in this environment, so discovery fell back to
Git delta inspection and targeted source searches.

Severity means:

- **Critical:** immediate corruption, broad exploitability, or unavoidable catastrophic
  failure in a normal production path.
- **High:** plausible silent financial error, stale data, data loss, undefined behavior,
  permanent hang, or inability to use a headline production workflow.
- **Medium:** bounded correctness, important performance, incomplete contract, or operational
  gap that should be scheduled explicitly.
- **Low:** clarity, maintainability, or narrow reporting problem.

No issue was classified Critical. That is not approval: the number and breadth of High findings
are sufficient to block a production sign-off.

## Correctness, integrity, and availability findings

### C-1 — High — projected VaR prices a stale historical book against a current-date reference

`dispersion_run_projected_var` loads manifest dates in ascending order, resolves membership on
`snapshots.front()`, and builds/sizes the book from that first snapshot:

- `atx-vol/src/dispersion_run.cpp:2498-2541`
- specifically `snapshots.front()->ts_ns()`, `snapshots.front()->uid_of(...)`, and
  `build_dispersion_book(..., snapshots.front()->set(), ...)` at lines 2527-2541

`dispersion_book_var` then defines the reference value as the last frame:

- `atx-vol/src/listed_dispersion_pipeline.cpp:508-516`
- losses are `reference_value - frame.value` in
  `atx-vol/src/historical_projection.cpp:118-147`

The source acknowledges the first/last mismatch at `dispersion_run.cpp:2523-2525`. It is not a
mere naming problem. Universe reconstitutions, removals, and changes in spot/vol that affect
vega sizing mean the quantities and membership are stale. The output remains finite and
plausible, but it is not VaR/ES for today's book.

The route should resolve and size the book at `snapshots.back()` or at an explicit as-of
snapshot, then project that immutable current book over prior surfaces. The artifact should
record the as-of timestamp and a book fingerprint. Add a route-level economics test with at
least two PIT universe blocks and materially different first/last sizing.

### C-2 — High — the gross-vega limit has a multiplier-dependent unit error

`measure_book` sums:

```text
abs(straddle_vega * straddle_qty)
```

at `atx-vol/src/dispersion_strategy.cpp:93-105`. The function accepts `multiplier` but discards
it, and also omits the per-unit-vol to per-vol-point scale of `0.01`.

The public contract describes these limits as dollars per vol point:

- `atx-vol/include/atx/vol/strategy.hpp:466-478`
- `atx-vol/include/atx/vol/strategy.hpp:495-498`

The multiplier is now a real typed configuration field and reaches production construction:

- binding: `atx-vol/src/dispersion_run.cpp:1378`
- routing: `atx-vol/src/dispersion_run.cpp:1563-1564`
- surface configuration: `atx-vol/src/dispersion_backtest.cpp:23-29`

The correct unit is:

```text
raw vega * quantity * multiplier * 0.01
```

The current result divided by the correct result is `100 / multiplier`, so the bug is hidden
only when `multiplier == 100`. At 1,000 the cap under-reports exposure by 10x; at 10 it
over-clamps by 10x.

Centralize the conversion used by sizing, schedule validation, and limits. Add an independent
non-100-multiplier test with hand-computed dollar/vol-point exposure. The current X3 helper
`natural_gross_vega` in `atx-vol/tests/dispersion_workflow_test.cpp:584-614` repeats the
production expression and is therefore not an independent unit oracle.

### C-3 — High — headline "gross vega" ratios use signed net vega

The engine stores signed aggregate `PriceTotals::vega` in
`BacktestResult::gross_vega`:

- `atx-vol/src/backtest.cpp:1835-1863`
- declared as gross at `atx-vol/include/atx/vol/backtest.hpp:613-614`

Tests correctly describe the value as net:

- `atx-vol/tests/dispersion_workflow_test.cpp:584-595`
- `atx-vol/tests/multiname_pipeline_test.cpp:918-928`

The tearsheet nevertheless derives `avg_gross_vega`, `return_on_gross_vega`, and
`vega_adj_sharpe` from this column:

- `atx-vol/src/tearsheet.cpp:239-263`
- public contract: `atx-vol/include/atx/vol/tearsheet.hpp:111-116`

A vega-neutral dispersion book intentionally makes signed vega close to zero. Dividing by that
cancellation residual makes these risk-normalized statistics unstable or meaningless while
actual gross leg exposure is unchanged.

Publish separate, explicitly unit-labelled `gross_vega` and `net_vega` columns. Gross should be
the sum of absolute position-scaled leg vegas in dollars per vol point. Add a live
vega-neutral-book test; current tearsheet tests hand-fill positive "gross" vectors and do not
exercise engine semantics.

### C-4 — High — vol-tick spread disappears when impact is active

`dispersion_effective_frictions` switches any active-impact configuration to
`PriceBps`:

- `atx-vol/src/dispersion_backtest.cpp:62-75`

It preserves base spread only when the original kind is already `PriceBps`. For a
`VolTicks` base, `base.vol_tick` remains in the object, but the execution engine dispatches on
the now-changed kind and ignores it:

- `atx-vol/src/backtest.cpp:1822-1831`

This contradicts the documented regime "the above plus impact" in
`atx-vol/include/atx/vol/dispersion_run.hpp:250-253` and the additive-spread description in
`atx-vol/include/atx/vol/dispersion_backtest.hpp:49-56`.

A valid spec combining vol-tick spread and impact therefore charges impact only and silently
overstates NAV. Represent base spread and impact as separate additive components, or reject
`VolTicks + impact` until composition is supported. Existing tests cover None and PriceBps
bases (`atx-vol/tests/dispersion_run_config_test.cpp:274-356`); add the missing combination.

### C-5 — High — RunArchive carry-forward can retain stale economic sections

`RunDir::run_identity_hash` hashes only `run_spec.tsv` and, optionally,
`universe_schedule.tsv`:

- `atx-vol/src/run_archive.cpp:1496-1513`

`write_run_archive` carries all non-colliding old sections when that identity matches:

- `atx-vol/src/run_archive.cpp:1516-1564`

The identity omits `surface_manifest.tsv`, archive content identities, the trade/projected
schedule, fitter/binary version, and other dependencies of economic sections. Rebuilding a
corpus or schedule under the same spec/universe can therefore retain an old backtest,
projection, reconciliation, or diagnostic section. New-wins collision behavior is sound, but
unrelated old section names remain eligible for carry-forward.

Give each result section an explicit dependency fingerprint, including the exact manifest,
archive content identities, and schedule. Carry only sections whose complete dependency set
matches. Add tests that mutate the manifest, an archive, and the schedule independently while
holding spec/universe fixed.

### C-6 — High — benchmark-relative statistics ignore dates and truncate mismatches

`read_dispersion_benchmark_series` parses `date<TAB>pnl` but discards the date:

- `atx-vol/src/dispersion_run.cpp:1239-1278`

`benchmark_stats` then aligns by vector position and uses:

```text
m = min(strategy.size(), benchmark.size())
```

at `atx-vol/src/tearsheet.cpp:84-138`.

A shifted, reversed, duplicated, missing-date, or short benchmark produces plausible
alpha/beta/information-ratio/tracking-error values for the wrong observations. The short case
silently drops the strategy tail. Non-finite values are also accepted into the report.

Return dated benchmark rows, validate finite P&L and unique ordered dates, and require an exact
join to the strategy dates unless a named inner-join policy is explicitly selected. Tests
should cover shifted equal-length dates, missing dates, duplicates, reverse order, non-finite
values, and length mismatch.

### C-7 — High — Python quote mutation races GIL-released readers

The C++ `OptionChain` contract is "many readers OR one writer", and `update_quotes` requires
exclusive access:

- `atx-vol/include/atx/vol/chain.hpp:19-25`

Python `PyPricerFitter::fit` and both `value_chain` overloads release the GIL while reading the
chain and lock only the fitter mutex:

- `atx-vol/python/src/bindings/fit.cpp:241-248`
- `atx-vol/python/src/bindings/fit.cpp:300-320`

`OptionChain.update_quotes` mutates the same chain without a shared chain lock:

- `atx-vol/python/src/bindings/fit.cpp:470-476`

Another Python thread can acquire the GIL while fit/value remains in C++ and mutate vectors,
quotes, or revision state concurrently. This is undefined behavior and can produce a torn fit,
wrong valuation, memory corruption, or a process crash.

Wrap the bound chain in shared/exclusive synchronization, acquire shared access around every
GIL-released reader, and exclusive access around mutations. Document lock ordering relative to
the fitter mutex. The current concurrency test covers immutable-chain `fit` versus
`value_chain`, not mutation (`atx-vol/python/tests/test_fit.py:342-390`).

### C-8 — High — risk buckets silently accept a mismatched portfolio

The API says the portfolio must be the same book as the frame:

- `atx-vol/include/atx/vol/portfolio_pricer.hpp:520-531`

The implementation checks only whether `positions.size() == frame.size()`:

- `atx-vol/src/portfolio_pricer.cpp:2098-2104`

For a size mismatch, `ByExpiry` substitutes `T = 0.0` for every successful row and collapses
the book into a fabricated zero-expiry bucket:

- `atx-vol/src/portfolio_pricer.cpp:2143-2150`

An equal-size unrelated or reordered portfolio is accepted and assigns the wrong expiries.
Totals can still reconcile, hiding the attribution error.

Return `Result<vector<RiskBucket>>` and reject size mismatch. To protect equal-size misuse,
carry the expiry/identity in `PriceFrame` or validate a portfolio generation/fingerprint.
Add smaller, larger, reordered, and unrelated-book negative tests.

### C-9 — High — SurfaceDb population can deadlock after scheduler launch/setup failure

`populate_surface_db` initializes per-date `remaining` counters from enabled task counts:

- `atx-vol/src/surface_db_populate.cpp:174-200`

Only a task that actually enters `fit_task` constructs `MarkDone`, whose destructor decrements
and notifies:

- `atx-vol/src/surface_db_populate.cpp:351-383`

The fit helper records `run_bounded_fit_tasks`' status, while the drain thread waits only on
the counters and reads `fit_status` after all waits:

- launch: `atx-vol/src/surface_db_populate.cpp:480-499`
- status check: `atx-vol/src/surface_db_populate.cpp:577-579`

The scheduler can return before executing any task after allocation/setup failure or
transactional partial thread-launch failure:

- `atx-vol/src/fit_scheduler.cpp:257-263`
- `atx-vol/src/fit_scheduler.cpp:330-382`
- its direct failure test proves `visits == 0` at
  `atx-vol/tests/corpus_test.cpp:196-216`

Those counters never reach zero, so an intended `Internal` result becomes a permanent hang.
Add a shared scheduler-complete/error condition that wakes every waiter, or retire every
unstarted task through the same completion callback. A populate-level failure-injection test
must assert bounded completion.

### C-10 — High — incremental SurfaceDb rewrite can delete a valid existing cell

The cell-aware universe path decides that a whole-date rewrite is safe when every symbol in
the existing partition is represented in the incoming board set:

- `atx-vol/src/surface_db_populate.cpp:665-705`

It then refits every incoming board. The partition writer includes only successful fits:

- `atx-vol/src/surface_db_populate.cpp:524-551`

Consider an existing partition `{A, B}`, an incoming set `{A, B, C}`, and a transient refit
failure for `B`. The guard allows the rewrite because both existing symbols were supplied.
The new archive contains `{A, C}` and silently deletes the previously valid `B`. The coverage
object reports a failure, but the old usable cell is gone.

Merge successful replacements into the existing partition, retaining old cells for failed
refits unless the operator explicitly requests destructive replacement. Add an incremental
resume test where one previously present cell fails during a rewrite and must remain
byte-identical.

### C-11 — High — corpus checkpoint and final-index pairs are not a recoverable transaction

Per-date checkpoint publication writes separate pending manifest and quality files, then
renames them one at a time:

- `atx-vol/src/corpus.cpp:1214-1240`

A crash after the first rename leaves a one-file checkpoint. Restart treats that state as
`AlreadyExists`, not as an abandoned generation it can discard or complete:

- `atx-vol/src/corpus.cpp:1072-1090`

Final `manifest.tsv` and `quality.tsv` use the same two-file commit window:

- `atx-vol/src/corpus.cpp:1595-1644`

The text writers use their own temp/rename implementation with no durable-file helper:

- `atx-vol/src/corpus.cpp:2336-2428`

An otherwise resumable production build can therefore require manual repair after a process
or power loss. Use one authoritative generation/transaction marker, or a generation-stamped
directory that becomes visible atomically. Reuse durable publication and teach restart to
recover abandoned pending/partial generations. Add kill-point tests after each publish step.

### C-12 — High — the documented report path cannot read production CLI output

The shipped CLI now publishes `run.atxrun` and intentionally does not write loose
`backtest.tsv`, `reconciliation.tsv`, or `contract_marks.tsv`:

- `atx-vol/include/atx/vol/dispersion_run.hpp:617-624`
- `atx-vol/python/tests/test_dispersion_runarchive_e2e.py:16-25`
- `atx-vol/python/tests/test_dispersion_runarchive_e2e.py:368-372`

The public Python README still directs users to:

```python
build_report_from_run("runs/golden-run", "pnl_track.html")
```

at `atx-vol/python/README.md:298-307`.

`build_report_from_run` searches only four loose TSV names and never probes
`run.atxrun`:

- `atx-vol/python/src/atxvol/report/dispersion.py:69-79`
- `atx-vol/python/src/atxvol/report/dispersion.py:117-151`

An archive reader already exists, but the end-to-end test exercises archive IO and the
separate parity report rather than this primary report entry point:

- `atx-vol/python/tests/test_dispersion_runarchive_e2e.py:375-387`

Make the entry point prefer `run.atxrun`, or provide and document a replacement archive-native
entry point. Add a shipped-CLI-to-HTML end-to-end gate.

### C-13 — High — omitted TSV economics become real zeroes

`read_backtest_tsv` calls `BacktestResult.resize`, zero-initializing all standard columns, and
then overwrites only columns present in the file:

- `atx-vol/python/src/atxvol/report/io.py:68-96`

The comment at lines 84-93 explicitly records the problem: an omitted column is
indistinguishable from a genuine zero. The renderer then computes attribution, risk panels,
and invariants from those arrays:

- `atx-vol/python/src/atxvol/report/dispersion.py:343-358`
- `atx-vol/python/src/atxvol/report/dispersion.py:412-457`

Partial, older, or malformed TSV output can therefore render absent risk/attribution as flat,
present zeroes. Preserve a `columns_present` set, require critical columns by report type, and
render optional omissions as unavailable. Add a partial-TSV test that must refuse or disclose
the missing economics.

### C-14 — Medium — projected-VaR verification accepts stale or garbage companion files

Projection is computed before output files are opened/truncated:

- `atx-vol/src/dispersion_run.cpp:2570-2585`

A failed rerun can leave a previous successful output triple untouched. The verifier checks
the companion files only for regular/non-empty status and parses just a narrow portion of the
summary:

- `atx-vol/src/dispersion_run.cpp:1794-1873`

It does not validate companion schemas, row counts, dates, book identity, full numeric
consumption, finite risk values, or consistency among the three files. A test uses `"x\n"` as
both companion files and expects success:

- `atx-vol/tests/dispersion_run_config_test.cpp:416-422`

Stage and publish a complete triple atomically. Validate schemas, counts, dates, confidences,
position count, as-of/book fingerprint, and trailing input.

### C-15 — Medium — projected VaR ignores typed construction knobs

The route reads loose `RunSpec`, then hardcodes:

- `side = ShortIndexLongNames`
- `multiplier = 100.0`

at `atx-vol/src/dispersion_run.cpp:2498-2539`.

The strict typed configuration exposes side, weighting, strike policy, and multiplier:

- `atx-vol/include/atx/vol/dispersion_run.hpp:264-320`

The source documents this as an existing limitation in
`atx-vol/examples/spy_dispersion_backtest.cpp:794-805`. A non-default production spec can
therefore run one construction in the surface/listed backtest and a different one in projected
VaR.

Route all construction through the strict typed config and a single shared
`DispersionConfig` builder. Add non-default parity tests for side, multiplier, weighting, and
strike policy.

### C-16 — Low — report prose hardcodes a zero delta band

`atx-vol/python/src/atxvol/report/dispersion.py:443-446` states that a zero-band hedge explains
the observed net delta, although the report accepts and displays a configurable
`delta_band`. Reports for non-zero bands therefore contain a false narrative. Render the
actual configured band and test a non-zero-band report.

## Performance and scalability findings

### P-1 — High — projected VaR copies and retains every full-date archive

`dispersion_run_projected_var` loads every manifest entry into retained
`MarketSnapshot` objects and stores every `SurfaceSet` pointer before resolving the book or
evaluating a scenario:

- `atx-vol/src/dispersion_run.cpp:2506-2514`
- evaluation begins at `atx-vol/src/dispersion_run.cpp:2574-2575`

The default load is LegacyCompatible, empty UID subset, Mutable backing:

- `atx-vol/include/atx/vol/backtest.hpp:132-147`

That route borrows views but Mutable uses `SurfaceArchiveV2::open_copied`, copying the complete
archive, and an empty subset maps every record:

- `atx-vol/src/backtest.cpp:1227-1234`
- `atx-vol/src/backtest.cpp:1265-1272`
- `atx-vol/src/backtest.cpp:1349-1371`

Peak memory is `O(sum of every archive in the history)`, including unused symbols. Load the
current anchor first, resolve required UIDs, then subset subsequent snapshots. Use sealed
read-only backing where lifecycle permits and evaluate scenarios in bounded batches. Add a
load-count/peak-memory route test.

### P-2 — High — corpus date batching begins after the entire OPRA range is resident

`dispersion_build_corpus` first loads the full `date_lo..date_hi` range:

- `atx-vol/src/dispersion_run.cpp:1930-1954`

Only afterward does it group the already materialized entries into date windows:

- `atx-vol/src/dispersion_run.cpp:1974-2047`

`load_opra_daterange` builds all symbol/date cells and retains each loaded `OpraPanel`:

- `atx-vol/src/opra_batch.cpp:335-350`
- `atx-vol/src/opra_batch.cpp:360-443`

`date_batch_size=8` therefore bounds fitted-surface lifetime, not OPRA input-panel lifetime.
The route remains `O(dates * symbols * quotes)` before the first fit/checkpoint and can OOM
before the batching mechanism helps.

Move the date-window loop outside `load_opra_daterange`: load, fit, checkpoint, and release one
bounded date window at a time. Add a production-ingest working-set test, not only a test over
already-supplied cells.

### P-3 — High — A7 allocates dense `cells * unique contracts` exact-price scratch

`scenario_grid.cpp` calculates `n_cells = n_spot * n_vol` without checked multiplication:

- `atx-vol/src/scenario_grid.cpp:243-248`

If any cell is exact, it allocates:

```text
pprime_all.assign(n_cells * n_unique, kNaN)
```

at `atx-vol/src/scenario_grid.cpp:288-304`. Comments acknowledge that Taylor-only rows are
allocated and never read.

A 100 x 100 grid over 10,000 unique contracts consumes about **800 MB** for this vector alone;
one exact cell triggers the allocation for all cells. Compact exact columns/cells behind a
deterministic index and add overflow guards before allocation. Add large-shape peak-memory
coverage; current tests use small grids.

### P-4 — High — risk-bucket reduction is quadratic in rows times distinct buckets

For every successful row, `reduce_risk_buckets` linearly scans the growing bucket vector with
`find_if`, then sorts:

- `atx-vol/src/portfolio_pricer.cpp:2138-2164`

Complexity is `O(N * B + B log B)`. High-cardinality, million-row risk books can perform
billions of key comparisons in a production aggregation feature.

Maintain a key-to-slot index while preserving serial input-order accumulation, then sort only
the final bucket vector. Benchmark 100,000-1,000,000 rows at low and high cardinality and keep
the bit-stability checks.

### P-5 — Medium — "framing-only resume" still reads and allocates the whole archive

Resume opens checkpoints with `SurfaceArchiveV2::open_file`:

- `atx-vol/src/corpus.cpp:1138-1161`

`open_file` sizes, allocates, and reads the complete file before the cheap directory accessors
are used:

- `atx-vol/src/surface_archive.cpp:879-899`

The existing test proves record bodies are not parsed/materialized when scrubbing is disabled;
it does not prove the bytes were not read. Use mapped or metadata-only opening for the
scrub-disabled path. Split the gate into "no materialization" and measurable bytes-read/peak
allocation assertions.

### P-6 — Medium — fitting benchmark scope and phase counters are misleading

The canonical `BM_Facade` timed loop includes fitter construction, `fit`, admission/report
work, and bundle access:

- `atx-vol/bench/fitting_throughput_bench.cpp:360-405`

The reported phase counters come from the last iteration rather than an aggregate. Benchmark
deltas can therefore reflect setup/post-fit work, and phase attribution may not describe the
reported timing. Construct invariants outside timing or pause timing around non-fit work, and
aggregate phase statistics across iterations.

### P-7 — Medium — occupancy probe charges ingestion CPU to fitting

The probe estimates fitting occupancy from total process CPU less a serial-wall estimate:

- `atx-vol/bench/corpus_occupancy_probe.ps1:216-218`

OPRA ingestion is itself parallel:

- `atx-vol/src/opra_batch.cpp:427-443`

CPU consumed outside fitting is therefore attributed to fit fan-out. The final status correctly
does not claim citable throughput, but the 13.30-core value is not a pure fit measurement.
Capture process CPU at phase boundaries or add phase-local CPU counters.

### P-8 — Medium — generic American batch pricing vectorizes puts but not calls

Eligible puts enter the boundary batch:

- `atx-vol/src/american_batch.cpp:340-359`

Calls remain on scalar `price_lane`:

- `atx-vol/src/american_batch.cpp:391-417`

Call-heavy and mixed-side books do not receive the expected generic batch speedup despite a
call kernel existing elsewhere. Integrate it and add call-heavy throughput/parity rows.

### P-9 — Medium — a missed subset match falls back to loading the full board

The `MarketSnapshot` subset path falls back to mapping all archive records when no requested
UID matches:

- `atx-vol/src/backtest.cpp:1299-1349`

This preserves timestamp/unpriced behavior, but turns a missing-name date into full-board I/O.
Return a metadata-bearing empty subset or read timestamp metadata without materializing every
surface. Add a missing-subset load-count test.

### P-10 — Medium, documented debt — publication does not fully close POSIX/concurrent-writer durability

`flush_and_publish_file` syncs the temporary file before rename but does not sync the parent
directory:

- `atx-vol/src/detail/archive_util.cpp:178-212`
- acknowledged at `atx-vol/docs/atxvsa2-format.md:268-272`

Writers also use fixed `destination + ".tmp"` names. A successful POSIX return is not a full
power-loss guarantee, and same-destination concurrent writers can race the temp path. Use
unique same-directory temporary files, serialize same-destination publication, and sync the
directory. This is known debt, not represented here as a newly introduced silent-number bug.

## Feature-gap review

### F-1 — High — discrete-dividend hedge accounting is not reachable from production orchestration

The engine implements `FinancingConfig::share_dividends` and books the cash flows:

- `atx-vol/include/atx/vol/backtest.hpp:404-441`
- `atx-vol/src/backtest.cpp:2293-2325`

The sprint explicitly required reuse of corpus `cash_divs`. Production spec parsing binds only
scalar financing fields and never supplies a dividend schedule:

- `atx-vol/src/dispersion_run.cpp:1406-1410`

Python `FinancingConfig` also omits the field:

- `atx-vol/python/src/bindings/backtest.cpp:186-191`

The native test proves direct engine injection, not corpus-to-orchestrator wiring. Consequently
the shipped CLI/Python routes cannot meet F3(b) and retain the fixed `q_eff_at(0.25)` proxy for
share carry. Persist/load the exact schedule and provenance, expose it in Python, and add a
corpus-to-ledger end-to-end test.

### F-2 — Medium — G2 is only partially implemented

Plan G2 promised:

- per-underlier and per-expiry aggregation for both `PriceTotals` and `PnlTotals`;
- `dP_dq`; and
- per-discrete-dividend `dP_dDiv`.

The shipped public bucket API covers only `PriceFrame`/`PriceTotals`:

- plan: `atx-vol/sprints/2026-07-21-atx-vol-pipeline-sota-sprint.md:242`
- implementation: `atx-vol/include/atx/vol/portfolio_pricer.hpp:500-531`

`PnlTotals` has no bucket reducer. `dP_dDiv` remains a lower-level American helper rather than
a portfolio column:

- `atx-vol/include/atx/vol/american.hpp:576-588`

The narrowed exit criterion ("bucketed vega + dP/dq") is met, but the workstream deliverable is
not. Implement the rest or explicitly de-scope it in the authoritative plan/status.

### F-3 — Medium — Python cannot reproduce term-rate or fully configured C++ fits

C++ `MarketEnv` supports a term `YieldCurve`:

- `atx-vol/include/atx/vol/market_env.hpp:40-55`

Python exposes only spot, timestamp, flat rate, cash dividends, and `rate_at`:

- `atx-vol/python/src/bindings/fit.cpp:354-363`

`PricerConfig` exposes a curve-family pin but explicitly leaves deeper `CurveConfig` knobs
unbound:

- `atx-vol/python/src/bindings/fit.cpp:520-547`

Bind term-curve construction/validation and supported curve configuration, then add C++/Python
parity tests with a non-flat rate curve and non-default curve settings.

### F-4 — Medium — broader Python/C++ configuration and result parity remains incomplete

Examples:

- `DispersionBacktestConfig` omits side, multiplier, limits, weighting, strike, and full hedge
  configuration (`atx-vol/python/src/bindings/dispersion.cpp:137-159`);
- Python exposes only the frozen-universe backtest overload, not PIT schedule resolution
  (`dispersion.cpp:163-181`);
- `ListedDispersionStrategy.create` omits quote mark/fill policies (`dispersion.cpp:260-270`);
- `RunConfig` omits reconciliation controls and entry-fill slippage
  (`atx-vol/python/src/bindings/backtest.cpp:211-223`);
- Python result conversion omits `nav_liquidation` (`backtest.cpp:225-350`).

These are material production controls and audit fields. Bind them or explicitly publish a
smaller supported Python contract. Reflection-style tests should compare bound config/result
members with the intended public C++ surface.

### F-5 — Medium — Y3's NaN+status convention is structurally lossy

`PricedSurface.grid` has one row status for independently failing output columns; IV/total
variance do not participate, so NaN IV can accompany `STATUS_OK`:

- `atx-vol/python/src/bindings/surface_db.cpp:137-168`
- documentation: `surface_db.cpp:271-287`

Default American batch paths collapse their two-state lane status into
`NotImplemented`, indistinguishable from a genuine NotImplemented result:

- `atx-vol/python/src/bindings/pricing.cpp:148-158`
- candidly documented at `atx-vol/python/README.md:383-401`

Use per-column validity/status or a primary row error plus masks, and preserve a distinct batch
regime code.

### F-6 — Medium — production quote-rejection accounting is absent

The quote-quality policy now reaches shipped selection, but only the library-only
`dispersion_build_schedule` writes `quote_rejects.tsv`:

- `atx-vol/include/atx/vol/dispersion_run.hpp:633-662`

The shipped `build-schedule` does not publish the per-date rejection tally. Operators can
confirm effective knobs but cannot audit what those gates rejected. Publish the same accounting
as an archive section or another production artifact.

### F-7 — Medium — the committed build helper cannot reproduce the requested configuration/gate

The committed `HEAD:scripts/atx-build.ps1` computes `$rest` from arguments but the
`configure` arm constructs only `cmake --preset dev` plus the dedicated Groups/Bench switches:

- `scripts/atx-build.ps1:97-116` at reviewed HEAD

Extra `-D...` settings supplied after `configure` are silently dropped. This directly impedes
the sprint's counters/profile configurations. The CTest arm also hardcodes `-j 16` at lines
100-107, while the sprint's trustworthy attribution required a serial run because concurrent
execution had already invalidated evidence.

Pass configure arguments as an argument array, not a shell string, and make CTest parallelism
an explicit option with a serial evidence default. Add a script test/dry-run assertion that
pins the generated argv. This helper is inherited rather than introduced by the sprint, but
the plan relies on it and the final status records its consequences.

### F-8 — sprint deliverables and evidence still open

The final status is candid about these; they remain gaps rather than review discoveries:

- **A5:** the planned sigma-node/cache sampling optimization did not land.
- **A6:** implementation landed with deterministic identity, but the required citable 8%
  timing evidence is absent.
- **B4/B6:** default-policy/selector follow-ups remain deferred.
- **B7:** the required platform baseline JSON is absent.
- **T1:** measured utilization is 13.30/16, below 14/16; the written-explanation clause was
  used, and there is no reserved-host throughput result.
- **BT-P2-8:** staleness is not evaluable on the current production snapshot feed.
- **Counters ON:** no CI lane; the final report records 12 known failures.
- **RunArchive e2e fixture:** the approximately 19 MB populated fixture is absent from the
  repository, causing five production e2e tests to skip.
- **Whole repository:** current-tip evidence covers the `atx_vol` label, not the roughly 5,700
  test full repository inventory.

These are not all merge blockers individually. They prevent the branch from being described as
having completed every sprint deliverable or as carrying citable performance closure.

## Sprint-plan assessment

| Workstream | Assessment | Review conclusion |
|---|---|---|
| M — merge/goldens/gates/docs | Largely met | Golden/reconciliation work is strong; the final status is unusually transparent. Current-tip validation remains label-scoped. |
| A — pricing/performance | Partial | Most correctness and A6/A7 work landed; A5 is absent, A6 lacks its required timing evidence, and A7 adds the dense-memory regression in P-3. |
| B — fitting | Partial | Canonical facade and allocation work landed. B4/B6 follow-ups and B7 baseline remain open; production ingestion still retains the whole range. |
| C — storage | Mostly met, with production gaps | Archive validation and format round-trip are strong. Corpus checkpoint pairing, resume I/O claims, and same-destination publication still need work. |
| G — greeks/risk | Partial | Ok-stamp and legacy fixes are valuable. G2 is incomplete and the shipped reducer has correctness and complexity defects. |
| E — analytics | Largely met/adjudicated | Core analytics fixes and umbrella wiring landed. Gross/net vega reporting semantics remain a material cross-workstream error. |
| F — backtest | Partial | Typed frictions and accounting fixes landed. VolTicks+impact, benchmark alignment, discrete-dividend wiring, staleness evidence, and rejection reporting remain open. |
| T — corpus/utilization | Partial | Deterministic batching/reclaim and byte gates are strong. Numeric utilization missed, ingestion memory is unbounded by date batching, and the framing-only claim is too broad. |
| Y — Python | Partial | Fit/batch/report foundations and CTest registration are real. Chain synchronization, archive-to-report integration, production config parity, and status fidelity are incomplete. |

## Exit-criterion audit

| # | Criterion | Assessment |
|---|---|---|
| 1 | Reconcile hotpath, pin goldens, serial gate within known failures | **Historically met; current-tip scope qualified.** Golden evidence is strong. This review ran the full current `atx_vol` label with no failures, not the full repository. |
| 2 | Zero known silent-wrong-number paths, each with a named test | **Not met.** C-1 through C-8 and C-13 include silent wrong-number paths without named regression tests. |
| 3 | Archive round-trip contract true | **Met for the stated archive fields.** The `n_quad_price` and adversarial-format work is strong. Cross-section dependency freshness is not covered by this criterion and fails C-5. |
| 4 | Mandatory friction regime end to end | **Partially met.** Regime refusal is enforced, but VolTicks+impact drops a cost and the primary production report path cannot consume production output. |
| 5 | Canonical risk frame exposes bucketed vega + dP/dq | **Narrow wording met; implementation not production-safe.** Bucket identity validation and complexity need repair; wider G2 scope is missing. |
| 6 | Python fits and batch-prices with NaN+status | **Narrow wording met.** Source-pinned tests pass. Mutation synchronization, term/config parity, and lossless status fidelity remain incomplete. |
| 7 | Facade bench; utilization >=14/16 or written explanation | **Met only through the explanation clause.** Numeric target missed; B7 baseline and citable throughput are absent; probe attribution is imprecise. |
| 8 | No stale contract docs | **Not met literally.** The README's primary report example is incompatible with RunArchive-only CLI output, and report prose hardcodes a zero hedge band. |

## Validation performed

### Current-tip build and broad gate

```text
cmake --build build -j 6
```

Result: success.

```text
ctest --test-dir build -L atx_vol -j 1 --output-on-failure
```

Current HEAD result:

| Quantity | Result |
|---|---:|
| Enumerated | 2,279 |
| Counted by CTest | 2,272 |
| Passed | 2,225 |
| Failed | 0 |
| Skipped | 47 |
| Disabled | 7 |
| Real time | 894.82 s |

The passing `atx-vol-python` CTest wraps a pytest run with five additional skipped RunArchive
production e2e cases because their populated paired fixture is not present. Those inner pytest
skips are separate from CTest's 47 skipped tests.

### Independent focused gates

- correctness lane: **103/103 passed** across dispersion workflow/config/cost, listed
  projection, historical projection, benchmark/tearsheet, RunArchive, and accounting suites;
- performance/storage lane: **146/146 passed** across scheduler, parallel-for, corpus,
  SurfaceArchiveV2, scenario-grid, batch-boundary, and ISA suites;
- feature/API lane: **6/6 focused C++ tests passed** for G2/lifecycle/umbrella;
- checkout-pinned Python driver: **168 passed, 5 skipped, 0 failed, 1 warning**;
- `git diff --check 2858cab..HEAD`: passed.

Green tests do not contradict the findings. Several tests explicitly pin the incomplete
behavior, repeat the production formula instead of using an independent oracle, or exercise
only direct library injection rather than the shipped route. The missing populated e2e fixture
also removes coverage from exactly the archive/report and route-level economic seams at issue.

### Validation limitations

- No full 5,700-test repository gate was run at current HEAD.
- No counters-ON gate was run; the sprint final records 12 known failures in that mode.
- No reserved, uncontended host was available for citable performance measurements.
- No power-loss fault injection, sanitizer, or ThreadSanitizer run was available on this
  Windows worktree.

## Positive observations

The requested changes should preserve these strengths:

- strict typed run configuration consumes/rejects keys by name and validates ranges;
- archive V2 validation has strong framing, CRC, bounds, alignment, and lookup/directory
  adversarial coverage;
- `FitScheduler`'s launch gate is a sound transactional worker-start pattern—the deadlock is
  in the caller's separate wait protocol;
- deterministic scheduling, solve ledgers, and thread-count identity gates are extensive;
- Python `AtxError` preserves structured `ErrorCode`;
- `PyPricerFitter` correctly synchronizes the fitter itself and Python surfaces co-own their
  fitted generation; the remaining race is specifically the separately owned chain;
- default backtest behavior fails closed on unpriced held lots/shares;
- archive publication syncs the temporary file and retries Windows reader-held rename failures;
- the final sprint report clearly discloses A5, B7, counters-ON failures, missing fixture,
  label-scoped validation, quote-rejection output, and non-citable throughput.

## Recommended remediation order

### P0 — financial correctness and stale data

1. Fix C-1, C-2, C-3, C-4, C-5, and C-6 with independent route-level economic tests.
2. Fix C-7 and C-8 so Python mutation and risk attribution fail safely.
3. Fix C-12 and C-13 so the production output/report contract is real and missing economics
   cannot become zero.

### P1 — availability, storage integrity, and scale

1. Fix C-9 and add scheduler-failure injection at the populate layer.
2. Fix C-10 and prove failed incremental refits preserve previously published cells.
3. Make checkpoint/final-index publication recoverable (C-11).
4. Bound projected-VaR and corpus-ingest memory (P-1/P-2).
5. Replace dense A7 scratch and quadratic bucket lookup (P-3/P-4).

### P2 — product completion and evidence

1. Wire discrete dividends through corpus, CLI, artifacts, and Python.
2. Finish or explicitly de-scope the wider G2 and Python parity commitments.
3. Make projected-VaR verification atomic and complete.
4. Close A5/B7/T1/counters/e2e evidence gaps or relabel them as deferred in the authoritative
   sprint acceptance.
5. Run counters-ON and the complete repository suite after the fixes.

## Final conclusion

`feat/pipeline-m` is a serious integration effort with many high-quality components, but it is
not yet a safe "surface database production implementation" or fully closed pipeline sprint.
The green gate establishes that the tested contracts are stable; it does not establish that the
contracts are economically correct, concurrency-safe, dependency-fresh, or bounded at
production scale.

The branch should remain out of production until the P0 financial/reporting/concurrency issues
and P1 availability/data-preservation issues are fixed and guarded. Performance and feature
deferrals should then be made explicit rather than carried under a blanket completed status.
