# Run4 concept catalog memory correction

Status: isolated implementation and focused tests complete; waiting for explicit
`fundamentals.py` ownership handoff after the AR4 Critical fixes. Independent review
required before acceptance. No live refresh, migration, restart, or write performed.

## Failure and scope

Run4 loaded 31,590,760 companyfacts rows for 7,035 targets, with 996 failed targets
out of 8,031 attempted. Its `statement_points` stage then failed after 88.75 seconds
with `MemoryError: cannot allocate memory for array` inside
`fundamentals.refresh_xbrl_concept_catalog`. The old implementation selects fourteen
columns from every raw fact into one pandas DataFrame before grouping. That Python
allocation is outside DuckDB's query memory limit. The writer and its parent exited;
this task did not terminate either process.

Owned files:

- `atx-db/src/atx_db/xbrl_catalog.py`: new bounded aggregation helper.
- `atx-db/tests/test_xbrl_catalog.py`: minimal two-table tests, no warehouse bootstrap.
- `atx-db/tests/data/public_api_snapshot.json`: only the reserved `xbrl_catalog` entry.
- This report. `fundamentals.py` is still untouched until the controller transfers it.

## Implementation and compatibility

`refresh_concept_catalog` aggregates inside DuckDB by the existing
`(source, taxonomy, concept)` key. Python receives only one row per catalog key,
formats the same JSON/category values, stages those small rows, and replaces matching
catalog keys transactionally. It does not change connection memory limits or threads;
the operator remains responsible for a bounded analytical session. The raw fact scan,
distinct counts, and intermediate grouping stay under DuckDB's memory/spill controls.

Preserved behavior:

- Exclude NULL/empty taxonomy and concept; keep source and taxonomy separate.
- Select the first non-NULL label and description independently, retaining empty
  strings. `arg_min(value, rowid)` makes the original physical insertion-order choice
  explicit even with insertion-order preservation disabled.
- Sorted unique nonempty units/forms/fiscal periods, serialized through the existing
  `json_dumps`, including its whitespace and Unicode escaping; empty lists remain `[]`.
- The existing taxonomy/category heuristic and its precedence, including DEI,
  outstanding shares, EPS, cash flow, balance sheet, income statement, and other.
- Count all facts; distinct security/accession counts exclude NULL but include empty
  strings. SQL min/max ignore NULL and retain native DATE/TIMESTAMP values, including
  microseconds and dates outside pandas' nanosecond range.
- Upsert only present keys; retain absent historical catalog keys, and leave the
  catalog unchanged for an entirely empty input. Destination failure rolls back the
  delete and insert, and the temporary staging table is removed.
- Deterministic grouping and publication order by source/taxonomy/concept.

## Validation

Focused run, authorized minimal DB slot, one thread and 128 MB per in-memory
connection:

```text
.venv/Scripts/python.exe -m pytest tests/test_xbrl_catalog.py -n 0 -q
16 passed
.venv/Scripts/python.exe -m ruff check src/atx_db/xbrl_catalog.py tests/test_xbrl_catalog.py
All checks passed!
.venv/Scripts/python.exe -m mypy --strict src/atx_db/xbrl_catalog.py
Success: no issues found in 1 source file
.venv/Scripts/python.exe -m pytest tests/test_module_boundaries.py::test_public_api_snapshot_matches_pinned_fixture -n 0 -q
1 passed (no DB fixture)
```

Coverage includes metadata/count/date/JSON parity, taxonomy and source isolation,
all category branches, repeated deterministic refresh, absent/empty source handling,
transaction rollback/cleanup, and a 100,000-fact aggregate with both DataFrame fetch
methods explicitly forbidden. No full suite or fixture-backed bootstrap was run.

Controller-authorized live read-only validation executed the exact helper SELECT
against `data/warehouse.duckdb` with `memory_limit=1GB`, `threads=1`, and
`preserve_insertion_order=false`. Only aggregate rows were fetched into Python; the
refresh/publish function was never called. The connection was closed afterward.

```json
{"catalog_rows":242,"elapsed_seconds":26.031,"fact_rows":31590760,"peak_commit_bytes":1128935424,"peak_working_set_bytes":1119350784,"status":"passed","taxonomy_counts":{"dei":2,"us-gaap":240}}
```

The process peak was approximately 1.04 GiB working set / 1.05 GiB commit. DuckDB's
1 GB query limit is not an exact process-RSS cap. This is evidence for the catalog
aggregation only, not proof that all later activation stages fit the same budget.

## Minimal pending facade patch

After AR4 releases `fundamentals.py`, remove its catalog-only `_statement_category`
and `_json_values` helpers and replace `refresh_xbrl_concept_catalog`'s body with:

```python
def refresh_xbrl_concept_catalog(store: DuckDBStore) -> int:
    """Refresh concept-level metadata from loaded SEC companyfacts."""
    from .xbrl_catalog import refresh_concept_catalog

    return refresh_concept_catalog(store)
```

The existing public export and activation imports remain unchanged. No jobs,
activation, registry, migration, or API-export edit is needed.

## Bounded audit of immediately following stages

- `fundamentals.refresh_fundamental_fact_revisions` is already `INSERT ... SELECT`
  with SQL windows and only a scalar result count. Optional pandas input contains
  the selected concept list, not fact rows.
- `fundamental_statements.refresh_fundamental_statement_points` is already SQL for
  fact joins, revision windows, and derived REIT inserts. Industry-template refresh
  also uses SQL; pandas is limited to small committed seeds/concept filters.
- `refresh_fundamental_periods` and `refresh_fundamental_ttm_points` are SQL writers
  with scalar count reads. Their window/sort memory and spill requirements still
  need ordinary bounded-session monitoring; this task did not rewrite them.
- The next `calendarization` stage has the same unbounded Python-materialization
  pattern at `calendarization.refresh_fundamental_calendar_map`: its SELECT reads
  every `fundamental_periods` row (17 selected fields) into `.df()` at line 318,
  then `compute_calendar_map_rows` builds Python records and another DataFrame.
  It is period-sized rather than raw-fact-sized, but it bypasses the DuckDB memory
  limit and is a concrete follow-up before calling activation memory-safe.

Suggested separate calendar-map task: preserve the existing issuer fiscal-year
inference, period-type/date labels, identifiers, JSON lineage, PIT/source clocks,
and source-scoped transactional replacement while moving the full-table operation
to SQL or bounded issuer/period chunks. Compare annual/quarter/stub/missing-date
cases to the existing pure computation. Validate under a declared low memory/thread
budget without rebuilding unrelated calendar-TTM or standardized stages. No such
calendarization edits were made here.
