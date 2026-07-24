# `atx-vol-surface-db-build` — the production surface-database build CLI

Point it at an OPRA hive-v2 tree and a `SurfaceDb` root; it create-or-opens the
db, loads the date window, auto-generates the per-symbol fit configs, and
cell-aware-streaming-populates every `(symbol, date)` vol surface. One call over
`build_surface_db` (`atx/vol/surface_db_build.hpp`) wrapped in a hand-rolled arg
loop (`tools/surface_db_build_main.cpp`).

It is **fully resumable at every stage**: re-running over an unchanged hive fits
**zero** surfaces and spends nothing. See "Resume semantics" below.

## OPRA hive v2 — the on-disk layout it reads

```
<hive-root>/date=YYYY-MM-DD/data.parquet     # ALL symbols for that session
```

- True hive partitioning (`date=` key), so DuckDB / `pyarrow.dataset` also read
  the tree natively with partition-column inference.
- **Exactly one parquet file per session**, holding every underlying's rows. The
  date file is written atomically (tmp + rename) and is the **unit of resume**.
- Schema: 8 columns, unchanged from the corpus format —
  `ts, underlying, symbol, instrument_id, bid_px, ask_px, bid_sz, ask_sz`.
  Prices are `int64` 1e-9 fixed-point; an unset side is `INT64_MIN`. The `date`
  partition value lives in the directory name, not as an in-file column.
- Rows are sorted by `underlying`, then `symbol`, so `underlying` column
  statistics support predicate pushdown for future selective readers. The C++
  loader does **one materialized read per date file** and splits by `underlying`
  in memory (one IO pass per date regardless of universe size); per-symbol
  row-group pruning is a documented future optimization, not built now.
- Snapshot minute is the fixed **19:55:00Z** hive convention (uniform with the
  existing corpus; the DST rationale is documented in the pull tool).

This replaces the old per-symbol tree `<root>/<symbol>/<date>.parquet` (one file
per `(symbol, date)`). To convert an existing per-symbol tree, see
[Sibling tools](#sibling-tools).

## Usage

```
atx-vol-surface-db-build --db <root> --hive <root>
    --from YYYY-MM-DD --to YYYY-MM-DD
    [--symbols A,B,C] [--index SPY] [--preset populate]
    [--deep-selection] [--fit-workers N] [--report out.csv]
```

| Flag | Required | Meaning |
| --- | --- | --- |
| `--db <root>` | yes | `SurfaceDb` root. Created if absent, else **opened/resumed**. |
| `--hive <root>` | yes | OPRA hive-v2 root holding `date=<YYYY-MM-DD>/data.parquet`. |
| `--from` / `--to` | yes | Inclusive date window (every calendar date in range is enumerated). |
| `--symbols A,B,C` | no | CSV universe. **Omit (or empty) to discover** every underlying present in the window. Surrounding whitespace per field is trimmed. |
| `--index SPY` | no | Designated index leg — pinned to the dense index recipe (bypasses per-board selection), for both config generation and the populate. |
| `--preset NAME` | no | `fast` \| `accurate` \| `robust` \| `hft` \| `populate`. Default `populate`. Drives both the manifest seeding and the populate fit tier. |
| `--deep-selection` | no | Additionally run the full held-out `select_curve` OOS search per symbol and pin its winner (falls back to the fit-policy decision when the selector has no scorable holdout). |
| `--fit-workers N` | no | Outer fit fan-out. `0` = auto (honors `ATX_VOL_FIT_WORKERS`); `1` = serial. |
| `--report out.csv` | no | Also write the two-section CSV report to this path. |

**Discover-all vs explicit `--symbols`.** With an explicit list, exactly those
underliers are loaded for every date (a date whose file lacks a requested symbol
is a visible coverage hole — a zero-match load error for that cell, never a
silent gap). With no `--symbols`, the effective universe is the sorted distinct
**union** of `underlying` across every readable date in range; every date then
spans the full union, so the ingest grid is rectangular (date × union) with
visible holes where a symbol is absent from a given date.

### Examples

```bash
# Explicit 3-symbol build over a July window, SPY as the index leg, CSV report:
atx-vol-surface-db-build \
  --db   C:/atx-data/surfdb-2026-07 \
  --hive C:/atx-data/opra-hive \
  --from 2026-07-01 --to 2026-07-31 \
  --symbols SPY,AAPL,MSFT --index SPY \
  --report C:/atx-data/surfdb-2026-07/build_report.csv

# Discover the whole universe present in the window (no --symbols):
atx-vol-surface-db-build \
  --db C:/atx-data/surfdb-2026-07 --hive C:/atx-data/opra-hive \
  --from 2026-07-01 --to 2026-07-31

# Deep per-symbol curve selection, serial fit (reproducible), robust tier:
atx-vol-surface-db-build \
  --db /db --hive /hive --from 2026-07-01 --to 2026-07-31 \
  --preset robust --deep-selection --fit-workers 1
```

### Output and exit codes

Every scalar report field prints one-per-line to **stdout** as `key value`
(mirroring the CSV `key,value` section), followed by `config.failed_symbols` and
one `symbol.<S> attempted=.. ok=.. failed=.. disabled=..` line per symbol. With
`--report`, the same data is written as CSV: a `key,value` scalar section then a
`symbol,n_attempted,n_ok,n_failed,n_disabled` row per symbol.

| Exit | When |
| --- | --- |
| `0` | Build succeeded (including a graceful empty-window no-op). |
| `1` | A build error — malformed hive spec, or a db config/write failure. Message on stderr. |
| `2` | A usage error — unknown flag, a missing required flag, or an unknown `--preset`. Usage on stderr. |

Note: a single unloadable or unselectable board never aborts the build — it is
tallied (and, for config, stored **disabled** = fail-closed) and the call still
succeeds.

## Report fields

**Config generation** (`generate_symbol_configs`, stage 1). The three
disposition counters partition the distinct symbols seen:
`n_configured + n_skipped_existing + n_disabled_failed == n_symbols`.

| Field | Meaning |
| --- | --- |
| `config.n_symbols` | Distinct symbols across the loaded boards. |
| `config.n_configured` | Freshly configured (or overwritten), enabled. |
| `config.n_skipped_existing` | Already in the manifest, left untouched (idempotent resume). |
| `config.n_disabled_failed` | Selection failed → stored **disabled** (never silently served). |
| `config.failed_symbols` | The disabled names (sorted). |

**Populate coverage** (`populate_universe_streaming`, stage 2). Cells are
`(symbol, date)` pairs; the counters describe what the cell-aware resume did.

| Field | Meaning |
| --- | --- |
| `coverage.cells_loaded` | Boards handed to the populate (available parquet cells). |
| `coverage.cells_to_fit` | NEW `(symbol, date)` cells scheduled this run. |
| `coverage.cells_refit` | Already-present cells re-fit by a same-date rewrite. |
| `coverage.cells_already_present` | Skipped: symbol already in its date partition. |
| `coverage.cells_ok` / `cells_failed` | Fit outcomes over the (re)written dates. |
| `coverage.dates_total` | Distinct dates among the loaded boards. |
| `coverage.dates_written` | Dates that needed a (re)write this run. |
| `coverage.dates_skipped_complete` | Dates whose loaded cells were all already present. |
| `coverage.dates_skipped_would_drop` | Dates skipped to avoid dropping an existing symbol (safety guard). |
| `symbol.<S> ...` | Per-symbol populate stats over the written dates. |

**Hive ingest** — the three date counters describe distinct **dates**, not cells:

| Field | Meaning |
| --- | --- |
| `n_dates_loaded` | Distinct dates that produced at least one board. |
| `n_dates_missing` | Distinct in-range dates that produced **none** (a fully absent OR fully unreadable date). |
| `n_load_errors` | **Cell** count of present-but-unparseable files (never reach the fit). |

**Double-count, by design.** A date whose file is present but fully corrupt is
counted in **both** `n_dates_missing` (it produced no boards, so it is not a
loaded date) **and** `n_load_errors` (each of its cells is a present-but-
unparseable file). This is deliberate: `n_dates_missing` answers "how many
in-range sessions have no usable surfaces?" and `n_load_errors` answers "how many
present files failed to parse?" — a corrupt date legitimately answers yes to
both. A merely absent date (no file at all) bumps only `n_dates_missing`.

## Resume semantics

The build is idempotent and resumable because each stage independently no-ops on
work already done:

1. **Create-or-open db.** A manifest at `--db` root → open (resume); absent →
   create. A re-run reuses the same root.
2. **Config idempotence.** A symbol already in the manifest is left **untouched**
   (`n_skipped_existing`), so a re-run never clobbers an operator override. (The
   library exposes an `overwrite_existing` escape hatch; the CLI does not — it is
   deliberately non-destructive.)
3. **Cell-aware populate resume.** A partition (= date) is (re)written only when a
   loaded board adds a symbol the partition does not already carry. So as the
   OPRA pull dribbles in new `(symbol, date)` cells, only the new work is fit; a
   re-run over unchanged data fits **zero** (`cells_to_fit == 0`,
   `dates_written == 0`, `dates_skipped_complete == dates_total`). A **grown**
   hive (new dates, or new symbols on existing dates) fits only the delta.
   - Safety guard: a date is **skipped, never rewritten**, if its partition
     already holds a symbol NOT present in this run's loaded set — a
     whole-partition rewrite would drop it (`dates_skipped_would_drop`). This
     cannot happen on the intended grow-only workflow but guards a
     narrower-symbol re-run from data loss.

An **empty window** (un-pulled days) is a graceful success: all-zero coverage,
the db still created. Absent dates in range are non-fatal (`n_dates_missing`).

The pull unit upstream is `(date, symbol-set)`: a present date file is read for
its on-disk symbol set (footer statistics, no data scan) and only the missing
symbols are pulled, then the date file is rewritten as the union — so the hive
never re-bills for data already on disk. See the pull tool below.

## Scale posture

Target shape: thousands of symbols × ~250 sessions/yr → ~1M surfaces
(≈ 4k symbols × 250 dates).

- **Per-date partition file**: 4k surfaces × ~2–6 KB ≈ 10–25 MB — comfortably
  inside `SurfaceArchiveV2` mmap + the LRU partition-view cache (16 resident
  partitions by default; O(1) `map_surface` probe per query).
- **Manifest** (`manifest.atxdb`): 4k × 256 B symbol records + 250 × 128 B
  partition records/yr ≈ ~1 MB, rewritten atomically — fine at this scale.
- **Asserted limits**: partition key ≤ 32 chars, symbol ≤ 32 chars.
- **Out of scope**: multi-year growth (manifest > ~100 MB or partitions
  > ~100k). The scaling seam is **one db root per year** (documented, not built —
  no sharding, YAGNI).

**Build-time peak memory scales with the date-range length in discovery mode.**
When `--symbols` is omitted, the hive loader runs a serial pre-pass that
materializes each date's table to compute the discovered union, and it **holds
each date's table in memory from that pre-pass until the date's panel pass
completes**. So the loader's peak RSS grows with the number of dates in the
requested window, not just with a single date. The downstream **populate** stays
`O(dates in flight)` (per-date fit → serialize → release), but the loader's
discovery retention is the memory ceiling for a wide window. For very long
ranges, build in date chunks (the cell-aware resume makes chunked runs stitch
losslessly) or pass an explicit `--symbols` list (which skips the discovery
pre-pass).

## Sibling tools

The build CLI only **reads** a hive; two Python tools produce and maintain it:

- **`atx-vol/tools/migrate_opra_hive.py`** — converts the old
  `<root>/<symbol>/<date>.parquet` tree into the new `date=*/data.parquet` hive.
  Pure local IO ($0), atomic per date, idempotent (complete date files skipped),
  verifies row counts and schema equality per date, writes a migration manifest
  CSV.
- **`atx-vol/tools/pull_opra_hive.py`** — the v2 Databento pull targeting the new
  layout. Free `get_cost` preflight for missing cells only, hard `--cap` with
  degrade-to-top-N-by-weight, `--dry-run`, DBN cache, atomic writes, spend log.
  One `get_range` per date over the union of missing parents; output is the
  merged date file (per the resume/merge rule above).
