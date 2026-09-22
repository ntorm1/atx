# AR1b: bounded survivorship forward panel

Implemented 2026-09-20 by a Codex subagent. Independent review is still required.
No live warehouse writes, production activation, full suite, or source downloads
were performed by this task.

## Behavior and interfaces

- `refresh_survivorship_safe_forward_returns(store, options)` retains its integer
  row-count return, source identifier, deterministic SHA-256 keys and table schema.
  It now stages normalized prices, observed calendar sessions and known terminals
  in DuckDB and performs one `INSERT SELECT` per configured horizon, inside one
  atomic source-scoped replacement transaction. No all-bar or expanded-panel
  pandas fetch occurs. Empty refreshes remove stale rows for only that source;
  failed publication rolls back the deletion and cleans temporary tables.
- `SurvivorshipSafeForwardReturnOptions.price_basis` defaults to
  `"adjusted_close"`, meaning the corrected same-row source adjustment from AR6.
  `"close"` is an explicit raw compatibility option. Missing adjusted prices never
  fall back to raw close or a NULL split factor. This is a source convention, not
  economic total-return or historical-vintage certification.
- `observation_cutoff: datetime | None` filters input vintages before price and
  terminal revision ranking, and removes calendar dates after the cutoff. Aware
  cutoffs normalize to naive UTC. Historical terminal rows are eligible even when
  their `is_latest_revision` flag is false. This cutoff is the outcome observation
  vintage, not the formation date.
- Numbers and published endpoint use the same h-th **bar-observed** calendar
  session. A survivor with no positive finite endpoint price is omitted rather
  than moved to the next security observation. The source calendar is explicitly
  not evidence of an official exchange holiday calendar.
- For a known terminal within a window, the price leg ends at the last positive
  price strictly before the first known terminal. Event-day/later prices do not
  enter the return; formation on/after that terminal is excluded. The result is
  `(last_preterminal_price / formation_price) * (1 + terminal_return) - 1`.
  Formation-only prices produce a zero partial leg. All emitted rows carry symbol,
  full calendar endpoint, observed/policy source and observation ID when present.
- Availability maximizes formation price, endpoint/preterminal price and terminal
  clocks. Missing bar availability retains the existing modeled date +22h
  convention, without claiming that it proves a source's historical vintage.
- Price duplicates use a deterministic single-row selection: latest eligible
  availability, source, vendor ID, symbol, adjusted/raw prices, load timestamp.
  Validity is tested after selection, so an invalid current price does not silently
  resurrect an older revision. Terminal revisions use eligible availability,
  load timestamp and terminal ID; event order is then deterministic by date/ID.
- The separate in-memory `compute_survivorship_safe_forward_returns` compatibility
  transform remains intact; production no longer calls it. The old integration
  assertion that every delisted return equals bare DLRET was corrected to retain
  its fixture's existing 50-to-40 /45-to-40 partial price changes.

## Input-derived quality and activation handoff

`survivorship_forward_return_check` now derives its expected windows from terminal
inputs, each security's own formation bars, configured horizons and the input
calendar. The output never defines the grid. The default check is scoped to
`atx_forward_returns_survivorship_safe_v1`; another source's rows cannot mask a
missing production panel. The standalone check accepts matching `source`,
`price_basis` and `observation_cutoff` arguments for explicit compatibility/vintage
runs. The warehouse sweep checks current production inputs/default source.

A positive raw formation bar with missing or invalid adjusted price still demands
its otherwise eligible terminal stitches in production mode. The writer cannot
calculate them, so an empty output fails instead of becoming vacuously green.
The check also requires correct endpoint/event metadata, finite results, symbol,
observed/policy lineage and minimum formation/terminal availability. Names without
their own formation bar are not demanded at other securities' formation dates.
The separate uncovered-event and code-reconciliation checks remain registered.

`survivorship_forward_return_diagnostics(store, options)` supplies only scalar
operational counts and explicit basis descriptions:

- `rows`, `stitched_rows`, `uncovered_event_rows`;
- `positive_raw_price_source_rows`, `missing_adjusted_price_source_rows`;
- source, price basis, observation cutoff, bar-observed calendar disclaimer and
  source-adjustment quality limitation.

The two price diagnostics count input **source rows**, not unique securities or
economic coverage. Calendar population must run before the panel and quality
check; its own loader row count must be retained by activation. Root owns the
jobs/activation integration, not this task. Activation should call the diagnostic
helper after refresh and retain the input-derived DQC result.

Metadata follow-up is explicitly pending with root's integration migration0312:
`migrations/bodies_0185_0188.py` currently seeds a stale dataset description claiming
raw close via `compute_forward_returns` and no imputed returns. The current catalog
description must state corrected adjusted-close production legs and observed or
named-policy terminals. This task was instructed not to edit migrations.

## Validation and memory limits

One guarded focused run, serial `-n 0`, completed with exit0: **19 cases passed**
(13 new minimal SQL/DQC cases and6 selected existing cases, including the slow
materialized writer integration and production warehouse quality sweep).

The new tests use a minimal in-memory schema, one DuckDB thread and128MB DuckDB
limit. Coverage includes nonzero preterminal movement; adjusted versus raw basis;
missing security session; formation/preterminal/terminal clock maxima; historical
revision selection; late terminal; event-day/post-terminal bars; terminal-only
metadata; deterministic source duplicates; idempotence/source isolation/empty
refresh; rollback; empty-panel failure; halt formation scoping; missing adjusted
input diagnostics; and the quality horizon contract.

Command from `atx-db`:

```powershell
.\.venv\Scripts\python.exe ..\.superpowers\sdd\tier1-parity\run_memory_guarded.py --job-gb 3 --receipt ..\.superpowers\sdd\tier1-parity\ar1b-focused-memory.json -- .\.venv\Scripts\python.exe -m pytest -n 0 -q --run-slow tests/test_survivorship_forward_sql.py tests/test_delisting_returns.py -k 'forward_return or survivorship or no_surviving_bar or non_delisting_names or stitched_availability'
```

The guard completed without a pressure stop; its native job memory peak field was
0.926GiB. That Windows field is not an RSS measurement. No other heavy workload
ran concurrently. The root-controlled DB test slot was explicitly released.

Ruff found no new diagnostics relative to HEAD on either implementation file or
the modified existing test file; the new test file is clean. Existing findings
remain13 in delisting.py (down from15),4 in checks_survivorship.py and11 in the
older test file. AST parsing and `git diff --check` passed.

Full-universe performance is **not measured** by this test run. SQL operators can
spill under the caller's DuckDB limits, but roughly31M prices times5 horizons is
still a large persisted output, and existing primary-key index growth needs
guarded production observation. Continue the1GB/1thread initial production policy
and process-tree cap; treat memory or disk-stage failures as measured follow-ups.
No claim that the full output already fits1GB is made here.

If the existing ART primary-key index becomes the measured limit, dispatch a
bounded publication follow-up rather than reducing horizons: build a complete
shadow table with the identical schema/constraints and preserved other-source
rows, using deterministic key-prefix batches, checkpoint/reopen between staging
batches to release connection-held index state, then verify counts/keys/input
vintage and test an atomic table exchange including dependent views. The live
table must remain unchanged until that exchange; resumable staging needs pinned
input provenance. This is a design option requiring dedicated failure/recovery
tests, not implemented or claimed safe by the current task.
