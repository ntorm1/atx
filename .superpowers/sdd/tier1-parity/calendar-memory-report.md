# Calendar-map bounded materialization

Status: implementation and six focused tests complete. Independent review remains
required. No live warehouse
read/write, migration, or activation was performed.

## Scope and behavior

`calendarization.refresh_fundamental_calendar_map` previously fetched every
`fundamental_periods` row into one DataFrame and built a second full result frame.
The writer now stages the complete input in DuckDB and passes at most 2,048 rows
at a time to the unchanged `compute_calendar_map_rows` oracle. Previous batch
frames are explicitly released before the next fetch. This bounds Python row
materialization independently of the number of periods or size of an issuer.

Fiscal-year-end inference still runs once over all annual periods, grouped by
the existing `(source, security_id)` keys, with the exact original `arg_max`
expression, date filter, and equality join. It is attached before batching;
an issuer crossing batch boundaries cannot lose annual evidence. Equal latest
annual dates have equal month values, so the inference tie is output-neutral.
The input is assigned stable ordinals by the unique fundamental-period ID.

The oracle retains IDs, JSON first-value interpretation, reported fiscal labels,
date-midpoint overlap labels, stub/52/53-week behavior, revision flags, input
availability/source-load clocks, and skipped missing-end dates. No wall-clock
read or public API change was added.

Both full-sized temporary tables are managed by DuckDB, so its memory limit and
spill controls apply. Only bounded frames cross into Python. Input staging,
chunk computation, output staging, source-scoped DELETE/INSERT, and temporary
table removal are in one transaction. Failure rolls back all staged work and
preserves the old output. Empty input or all skipped input clears that output
source while retaining other output sources. Destination defaults/constraints
are still applied at final publication.

Owned paths:

- `atx-db/src/atx_db/calendarization.py` (calendar-map writer and private batch size only)
- `atx-db/tests/test_calendarization_bounded.py`
- This report

## Validation

Controller-authorized focused run passed: five new minimal-schema tests and the
existing pure-oracle regression. No full warehouse fixture/bootstrap was needed.
Each minimal connection uses one thread, 128 MB, and insertion-order preservation
disabled. The process ran under the controller's 2 GiB memory guard.

```text
python -m pytest tests/test_calendarization_bounded.py \
  tests/test_calendarization.py::test_compute_calendar_map_rows_fyr_boundaries_and_53_week_flag \
  -n 0 -q
6 passed; exit 0; guarded process elapsed 3.6 seconds
```

Receipt: `.superpowers/sdd/tier1-parity/calendar-memory-tests-1.json`.
The guard reported native peak job memory 0.6205 GiB, with more than 7.6 GiB
physical and 11.4 GiB commit headroom before and after. The native peak field is
not a process-resident-memory measurement. The DB slot was immediately released.

The cases cover original whole-input oracle parity, source isolation, latest
annual ties, malformed/empty/scalar/list JSON, annual/quarter/stub/instant/YTD
labels, missing dates, microsecond clocks, deterministic reruns, a single issuer
with 4,100 rows across three chunks and annual evidence last, bounded DataFrame
fetches, empty source replacement, publication failure rollback, and later-chunk
failure cleanup.

Static checks so far:

- `git diff --check`: passed.
- Ruff on both owned code/test files: one pre-existing `RUF046` at unchanged
  `period_week_count` (`int(round(...))`); no new findings.
- Strict mypy on `calendarization.py`: seven pre-existing findings outside the
  modified writer (`_as_date`, `_first_json_value`, oracle integer conversion,
  and two other writers' optional `fetchone` indexing). No writer findings.
  The pure oracle was deliberately not changed to tidy unrelated typing/lint.

## Remaining operational limits

This change bounds Python period materialization; it does not make DuckDB query
memory a process RSS limit. Keep the reviewed process-tree cap, one thread,
low query memory, adequate spill space, and one heavy workload at a time.
Calendar TTM remains the existing SQL writer; its joins/windows still need
ordinary bounded-session monitoring. No other full-table `.df()`/`fetchall()`
materialization remains in this module's production paths. The public pure
oracle intentionally accepts a caller-provided DataFrame; external direct callers
are responsible for its size.

Unrelated pre-existing determinism detail observed but not changed: the
calendarization coverage writer uses `now() AS source_loaded_at`; this task adds
no clock reads and does not modify the coverage or calendar-TTM writers.
