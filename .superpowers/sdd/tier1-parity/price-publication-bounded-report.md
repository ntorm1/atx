# Bounded price publication investigation

## Measured constraint

The native publication attempted a source delete, a 31,959,271-row insert, and
creation of the two `equity_daily_bars` ART indexes in one transaction. The
transaction failed at commit with both 1 GB and 2 GB DuckDB query limits; the
2 GB native process reached 2.708 GiB under the 3 GiB process cap. The previous
31,178,192 live rows remain intact. The retained `equity_daily_bars_bulk_next`
staging table is valid and must remain available for inspection until an
explicit successful publication removes it.

The production defaults remain 1 GB and one thread. The approved 2 GB query
limit and 3 GiB process cap are ceilings, not new defaults.

## Documentary catalog probes

On 2026-09-20, temporary isolated files were probed using the project virtual
environment's DuckDB 1.5.5 with `memory_limit=1GB`, `threads=1`, and
`preserve_insertion_order=false`. These probes were run without the required
process-tree guard and their scratch scripts were removed before a receipt was
persisted. They are design observations only, not verification evidence; root
will run the focused guarded tests and retain receipts.

Observed behavior:

* Renaming a table with a non-unique ART index failed with DuckDB's dependency
  error.
* A transaction that renamed a primary-key-only live table to a previous name,
  renamed a primary-key-only shadow to the live name, then dropped the previous
  table committed successfully.
* A reader view over the live table read the new row after that commit and the
  old row after an otherwise identical transaction was rolled back.

DuckDB documents the capacity limit: ART indexes are not evicted by the buffer
manager and index creation must fit in memory. See
https://duckdb.org/docs/lts/guides/performance/indexing and
https://duckdb.org/docs/current/sql/indexes.

## Narrow physical index policy

The bars table has no primary key or unique index. Its two ART indexes are
non-unique secondary performance indexes, so they are not part of the logical
data contract. Migration 0314 drops those two indexes plus the three non-unique
`equity_price_metrics` secondary indexes. The base schema no longer recreates
the two bar indexes. It does not alter table columns, defaults, nullability,
public relation names, or `equity_price_metrics.metric_id` primary key.

The publisher builds and validates a disk-backed physical shadow with retained
non-source rows and the replacement rows. It verifies the live/shadow column
order, types, nullability, defaults, primary keys, and unique constraints. A
short transaction performs related side-table writes, renames the live table to
a previous name, renames the shadow to the live name, and drops the previous
table. A failure rolls back the table and side tables together; the shadow stays
available for inspection. A successful swap consumes the staging table name.

The daily metrics publisher uses the same swap while retaining its metric ID
primary key. Its persistent build must use key-range batches and only checkpoint
or reopen between independently committed build batches, never during the swap.

## Production command and limits

The native publication keeps `--memory-limit 1GB --threads 1` defaults and uses
a 3 GiB process-tree guard. A separately approved 2 GB investigation limit may
be used only for a measured run; it does not authorize a higher default.

## Guarded focused verification

Root ran the project virtual environment's guarded focused publication suite:
66 tests passed and one default slow test was skipped, with a 0.937 GiB peak.
The receipt files are `atomic-publication-focused1-memory.json`,
`atomic-publication-focused1.log`, and `atomic-publication-focused1.err` in
this evidence directory. The sole initial failure was the required public API
snapshot entry for the new internal `_bulk_publication` module; that entry is
now added and root is rerunning only the snapshot case.
