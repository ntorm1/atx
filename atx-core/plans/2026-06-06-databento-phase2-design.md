# Databento Daily-OHLCV — Phase 2 (Engine Integration + Scale) Design Spec

> Status: approved (design). Next: implementation plan (writing-plans skill).
> Branch: `feat/databento-loader` (worktree). Builds on Phase 1 (the DBN→Parquet loader).

## Why

Daily OHLCV is foundational for atx-engine (backtest loop, RollingPanel, alpha VM).
Phase 1 produces a faithful, real-data-validated cold store (date-partitioned
Parquet, prices as int64 1e-9). Phase 2 closes the gaps that block engine-wide
reuse: the i64↔f64 impedance, single-file-only ingest, uncompressed output,
unbounded memory, and the absence of a corporate-action story.

Grounding (verified): the engine is **f64 end-to-end** — `atx::tsdb::load_parquet`
projects fields to `std::vector<f64>`; the sealed TSDB segment, `SegmentReader`,
`ShmBarFeed`, `RollingPanel`, Oracle and VM are all f64. Instruments are keyed by
**interned symbol string** (`SymbolTable::intern(name) -> Symbol{u32}`), so the
`symbol` column is the join key; `instrument_id` is not required by the engine.
There is **no corporate-action / adjustment handling anywhere** in the engine
(only `delisted_final` survivorship).

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Price representation | Keep Parquet as **int64 1e-9** (faithful archive); convert to f64 once in the TSDB bridge | Cold store stays bit-exact to source; the engine caps precision at f64 anyway, so a single conversion site (the bridge) avoids per-consumer friction. |
| Adjustment | **Defer**: store raw faithfully; spec an adjustment-factor table as a separate workstream | EQUS.SUMMARY is raw/unadjusted and we have no corporate-actions source yet. Raw-price backtests have split/div discontinuities — documented, not silently shipped. |
| Engine consumption | **Per-date segments + multi-segment feed** | 1:1 parquet-partition ↔ segment; lazy day-by-day load (bounded memory); append-friendly (new day = new segment); aligns with the date-driven backtest loop. |

## Units

Three sequential, independently-testable units, plus a deferred design appendix.
Dependencies: U1 → U2 → U3.

### Unit 1 — Loader scale (atx-core)

**Files:** `atx-core/src/external/databento.cpp`, `atx-core/include/atx/core/io/parquet_writer.hpp`, `atx-core/src/io/parquet_writer.cpp`.

- **Per-date streaming write.** Each `.dbn.zst` entry in a Databento batch zip is
  exactly one trading day. Decode it, derive its date once, and write that day's
  rows straight to `<dest>/date=YYYY-MM-DD/data.parquet` via `write_parquet` — no
  global row accumulation, no global hive bucketing. Peak memory is one day
  (~11k rows) instead of the whole archive (~5.6M rows).
- **Date computed once per file** (from the first decoded record's `ts_event`),
  not per row. Today `date_string()` runs per row (~11k identical strings/file).
- **Parquet compression + encoding.** Extend the writer to set Arrow
  `WriterProperties`: `compression = ZSTD`, `dictionary` enabled (high value on the
  low-cardinality `symbol` column). Add a `WriteOptions{compression, dictionary}`
  parameter to `write_parquet`/`write_hive_parquet` with a sensible default
  (ZSTD + dictionary on); columns stay int64.
- `write_hive_parquet` is retained for the generic multi-date case; the loader path
  switches to per-file `write_parquet`. `LoadStats` semantics unchanged
  (files_processed, records_decoded, partitions_written = files written, records_skipped).
- **Contract unchanged on disk:** same partition layout and column schema
  (`ts:timestamp[ns]`, `symbol:string`, `open/high/low/close:i64` 1e-9, `volume:i64`),
  now ZSTD-compressed.

### Unit 2 — TSDB per-date bridge (atx-tsdb)

**Files:** `atx-tsdb/include/atx/tsdb/load_parquet.hpp`, its `.cpp`, tests.

- **The single i64→f64 conversion site.** Read OHLCV columns via
  `ParquetTable::column_view<i64>` and scale to f64 by ÷ 1e9; `volume` i64→f64;
  `ts` i64 (ns) as-is; `symbol` via `ParquetTable::strings`. Today `load_parquet`
  builds `LongColumns.values` as f64 by reading f64 columns — this adds the i64
  field path (a `field_scale` of 1e-9 for price columns, 1.0 for volume).
- **Per-date segment build.** Add `build_dated_segments(hive_root, seg_dir, created_at)`:
  for each `<hive_root>/date=YYYY-MM-DD/data.parquet`, build one sealed segment at
  `<seg_dir>/YYYY-MM-DD.seg`. Reuses the existing `build_from_long` pivot.
  Idempotent: skip/overwrite an existing dated segment deterministically.
- The single-file `load_parquet` keeps working (now also able to read i64 fields).

### Unit 3 — Multi-segment feed (atx-engine)

**Files:** new `atx-engine/include/atx/engine/data/multi_segment_bar_feed.hpp` (+ test); reuses `IDataHandler` / `SegmentReader` / `ShmBarFeed` patterns.

- A feed that takes an ordered list of per-date segment files (or a `seg_dir` it
  sorts by date) and presents a **continuous** bar stream to the `BacktestLoop`:
  drive the current segment to exhaustion, then lazily open the next. Bounded
  memory (one segment resident at a time).
- Each segment carries its own per-day symbol table; the feed publishes `Market`
  events **by symbol name**, the loop interns → the global universe handles
  cross-day alignment (symbols entering/leaving days). No global symbol table needed
  at build time.
- `step()` semantics match `IDataHandler` (advance to next frontier, publish, return
  false at the end of the last segment).

### Deferred (design appendix, NOT implemented in this plan)

**Adjustment-factor table.** Store raw OHLCV (Phase 1/Unit 1). Separately, design a
table `(entity_key, date, cum_split_factor, cum_div_factor)` sourced from a
corporate-actions feed (Databento corporate actions or another vendor), applied as
`adj_close = close × factor` at or after the bridge. Until it exists, **raw-price
backtests spanning a split or dividend are wrong** — this must be stated wherever
the dataset is documented. Tracking item, not part of the Phase-2 implementation.

## Data flow

```
EQUS.SUMMARY .zip
  └─[U1] per-day decode → <dest>/date=YYYY-MM-DD/data.parquet  (i64 1e-9, ZSTD)
        └─[U2] read i64 ÷1e9 → f64 → <seg_dir>/YYYY-MM-DD.seg  (sealed, f64)
              └─[U3] MultiSegmentBarFeed (date order, lazy)
                    └─ bus → BacktestLoop → RollingPanel → Oracle/VM   (f64)
   [future] adjustment factor applied between U2 and the panel
```

## Error handling

- All expected failures travel in `Result`/`Status` (no throw across surfaces).
- U1: a malformed `.dbn.zst` aborts the load with `Err` (same policy as Phase 1);
  partitions written before the failure remain on disk. Per-file writing does not
  change this contract.
- U2: per-date build failure isolates to that date (batch reports which dates failed);
  i64-with-no-scale or unexpected column type → `InvalidArgument`.
- U3: missing or out-of-order segment file → `Err`; empty seg list → `Err`.

## Testing (TDD)

- **U1:** build a multi-day in-process zip (existing `dbn_fixture`), load, assert one
  partition file per day, memory path writes per-file (assert partition count ==
  file count), and round-trip values still match; assert files are ZSTD-compressed
  (read back via `read_parquet`).
- **U2:** write a known i64 parquet partition → `build_dated_segments` → open with
  `SegmentReader`, assert values equal i64 ÷ 1e9 (scale applied) and symbol/time axes
  correct. Cover the i64 field path directly in a `build_from_long`-adjacent test.
- **U3:** two per-date segments (day1, day2, overlapping + disjoint symbols) →
  `MultiSegmentBarFeed` → assert day1 cross-section then day2, symbols interned
  consistently across the boundary, EOF after the last.

## Out of scope (this plan)

- Corporate-action / adjustment data ingestion (deferred appendix above).
- Non-OHLCV DBN schemas; per-symbol re-partitioning; f64-dollar storage variant.
- Changing the Phase-1 decoder (validated; untouched).
