# Disk Hive Data Layer + Databento Storage Pipeline — Design

Date: 2026-06-07
Status: Approved (design); pending spec review → implementation plan.
Branch base: `feat/atx-core-stdlib`. Implementation in a fresh git worktree via
subagent-driven development.

## 1. Goal

Build a persistent on-disk market-data storage layer for the engine, backed by
date-partitioned (hive) Parquet, plus the Databento wrapper plumbing to download
live data straight into it. Then exercise it end to end:

1. Ingest the EQUS.SUMMARY batch zip into an `OHLC1D` store.
2. From `OHLC1D`, rank symbols by 20-day rolling **median daily notional**
   (`close × volume`) and take the **top 100** most liquid.
3. For the **last date** in `OHLC1D`, at **15:55 America/New_York** (= 19:55:00Z,
   June ⇒ EDT/UTC−4), pull equity **bbo-1m** L1 quotes for the 100 symbols into
   `OHLC1M`.
4. Pull OPRA **cbbo-1m** for the same 100 underlyings' full option chains into
   `OPRA_BBO`.
5. Every Databento query stays **< $2** (preflight-gated, auto-split).

## 2. Context / existing building blocks

- `atx::external::databento::load_equs_summary_zip(zip_path, dest_dir)` already
  decodes the EQUS zip and writes hive Parquet partitioned by `date` with columns
  `ts, symbol, open, high, low, close, volume` to `<dest_dir>/date=YYYY-MM-DD/data.parquet`.
  Prices are Databento native 1e-9 fixed-point `i64`.
- `atx::core::io` read side: `LazyParquet::scan(path)` → `select/filter/limit/offset`
  → `collect()` → `ParquetTable` (`column_view<T>`, `strings()`, `to_frame()`).
- `atx::core::io` write side: `write_parquet(cols, path, opts)` and
  `write_hive_parquet(cols, root, partition_col)`; `WriteColumn` variant over
  `span<const i64|f64|std::string|Timestamp>`.
- Official Databento C++ client (`databento::Historical`) is vendored under
  `atx-core/third-party/databento-cpp` (v0.59.0) and linked into atx-core.
- Source zip: `C:/Users/natha/Downloads/EQUS-20260606-CEXECEMBY6.zip` (156 MB).
- API key: `DATABENTO_API_KEY` env var (from repo `.env`, gitignored).

## 3. Architecture (Approach A — split by layer)

Dependency arrow stays `engine → core`. The core wrapper never references the
engine `DiskStore`; it writes Parquet to a path the caller supplies.

```
atx-core/external/databento.{hpp,cpp}   pull-and-write-parquet (official client) + cost preflight
atx-engine/data/disk.{hpp,cpp}          DiskStore: hive layout, partition I/O, reads
atx-engine examples/build_universe.cpp  orchestrator wiring the pipeline
```

## 4. Component 1 — `atx::engine::data::DiskStore`

Files: `atx-engine/include/atx/engine/data/disk.hpp` (+ `src/.../disk.cpp`).

```cpp
namespace atx::engine::data {

enum class Store { Ohlc1D, Ohlc1M, OpraBbo, OChain };
// dir names: OHLC1D, OHLC1M, OPRA_BBO, OCHAIN

class DiskStore {
 public:
  // Creates <root> and all four store subdirs if absent.
  static Result<DiskStore> open(std::filesystem::path root);

  std::filesystem::path root() const;
  std::filesystem::path store_dir(Store) const;             // <root>/<STORE>
  std::filesystem::path partition_path(Store, std::string_view date) const;
      // <root>/<STORE>/date=YYYY-MM-DD/data.parquet

  // Write one date partition (parent dirs created; overwrites that partition file).
  Result<void> write_partition(Store, std::string_view date,
                               std::span<const core::io::WriteColumn> cols);

  // Sorted ascending list of partition dates present in a store ("YYYY-MM-DD").
  Result<std::vector<std::string>> list_dates(Store) const;

  // Lazy scan of a single partition file (errors if absent).
  Result<core::io::LazyParquet> scan_partition(Store, std::string_view date) const;
};

}  // namespace atx::engine::data
```

Notes:
- `date` is the partition key string `YYYY-MM-DD`; it is path-encoded and NOT a
  column inside the file (mirrors the existing loader output). Caller-supplied
  `cols` to `write_partition` must therefore NOT contain a `date` column.
- `list_dates` enumerates `date=*` subdirs and parses the value.
- `OChain` is created but unused this round.

## 5. Component 2 — Databento wrapper additions

File: `atx-core/include/atx/external/databento.hpp` (+ `.cpp`). Uses the official
`databento::Historical` client. Each pull writes ONE Parquet file at `out_path`.

**Header cleanliness:** databento-cpp is linked PRIVATE to atx-core (its include
dir is not propagated to consumers), so the public wrapper header must NOT include
any `<databento/...>` header. Therefore `schema`, `dataset`, and `stype` cross the
public surface as **strings** (e.g. `"bbo-1m"`, `"cbbo-1m"`, `"DBEQ.BASIC"`,
`"raw_symbol"`, `"parent"`) and are mapped to databento enums inside `.cpp`.

```cpp
struct PullStats {
  i64 records{0};        // L1 rows written
  i64 symbols{0};        // distinct symbols/contracts seen
  i64 api_calls{0};      // number of TimeseriesGetRange calls (after batch split)
  double cost_usd{0.0};  // summed preflight cost actually pulled
};

// Free MetadataGetCost (no egress, no charge). schema/stype_in are strings.
Result<double> estimate_cost(std::string_view api_key, std::string_view dataset,
                             const std::pair<std::string,std::string>& range_utc,
                             std::span<const std::string> symbols,
                             std::string_view schema, std::string_view stype_in);

// Equity L1 ("bbo-1m" or "cbbo-1m", set by caller) for `symbols` over [start,end),
// stype_in = "raw_symbol". Columns: ts, symbol, bid_px, ask_px, bid_sz, ask_sz.
// Auto-splits `symbols` so every API call's preflight < cap_usd; accumulates all
// batches in memory; writes the file once.
Result<PullStats> pull_equity_l1_1m_to_parquet(
    std::string_view api_key, std::string_view dataset,
    std::span<const std::string> symbols,
    const std::pair<std::string,std::string>& range_utc,
    std::string_view schema, std::string_view out_path, double cap_usd = 2.0);

// OPRA full-chain cbbo-1m for `underlyings` via parent symbology "<SYM>.OPT".
// Columns: ts, underlying, symbol, bid_px, ask_px, bid_sz, ask_sz.
Result<PullStats> pull_opra_cbbo_1m_to_parquet(
    std::string_view api_key, std::span<const std::string> underlyings,
    const std::pair<std::string,std::string>& range_utc,
    std::string_view out_path, double cap_usd = 2.0);
```

- Prices stored as `i64` 1e-9 fixed-point (consistent with OHLC1D). Sizes as `i64`.
  Unset bid/ask (`databento::kUndefPrice`) stored as a sentinel (`INT64_MIN`) so the
  reader can distinguish "no quote" from a real price; the orchestrator may also
  drop unset rows — decided in the plan, defaulting to keep-with-sentinel.
- Symbol mapping: build `PitSymbolMap` from the response metadata to resolve
  `instrument_id → text symbol` (raw ticker for equities, OSI for options).
- `load_equs_summary_zip` is unchanged; the orchestrator passes the `OHLC1D`
  store dir as its `dest_dir`.

### Cost gating (recursive split)
```
pull(symbols, cap):
  c = estimate_cost(symbols)
  if c < cap or symbols.size()==1: stream(symbols); return
  else: split symbols in half; pull(left,cap); pull(right,cap)
```
Guarantees every `TimeseriesGetRange` call has preflight cost < cap and the full
set is covered. All decoded rows accumulate in memory (one minute × ≤100 names is
small) and the partition file is written once.

## 6. Component 3 — orchestrator `build_universe`

File: `atx-engine` example/tool `examples/build_universe.cpp` (opt-in, like the
atx-core databento examples). Reads `DATABENTO_API_KEY` from env.
Args: `build_universe [DATA_ROOT] [EQUS_ZIP]`
(defaults: `DATA_ROOT=data/universe`, `EQUS_ZIP=C:/Users/natha/Downloads/EQUS-20260606-CEXECEMBY6.zip`).

Steps:
1. `DiskStore store = open(DATA_ROOT)`.
2. `load_equs_summary_zip(EQUS_zip, store.store_dir(Ohlc1D))`.
3. `dates = store.list_dates(Ohlc1D)`; `last = dates.back()`; `window = last ≤20 dates`.
4. For each date in `window`: scan partition, read `symbol`, `close`, `volume`;
   accumulate per-symbol `notional = (close * 1e-9) * volume` (f64).
5. Per symbol: **median** of its daily notionals over the window; rank desc; take
   **top 100** → `picks`.
6. `range = {last + "T19:55:00", last + "T19:56:00"}` (UTC; 15:55 EDT).
7. `pull_l1_1m_to_parquet(key, EQUITY_DATASET, picks, range, EQUITY_L1_SCHEMA,
   SType::RawSymbol, store.partition_path(Ohlc1M, last))`.
8. `pull_opra_cbbo_1m_to_parquet(key, picks, range,
   store.partition_path(OpraBbo, last))`.
9. Print summary: #dates, last date, top-100 cutoff notional, rows + cost +
   api_calls per store.

## 7. Schemas (per store)

Prices `i64` 1e-9 fixed-point; sizes `i64`; `ts` Timestamp(ns); `date` path-encoded.

| Store     | Columns                                                        | Source                |
|-----------|---------------------------------------------------------------|-----------------------|
| OHLC1D    | ts, symbol, open, high, low, close, volume                    | EQUS zip (existing)   |
| OHLC1M    | ts, symbol, bid_px, ask_px, bid_sz, ask_sz                    | equity 1-min L1       |
| OPRA_BBO  | ts, underlying, symbol, bid_px, ask_px, bid_sz, ask_sz        | OPRA cbbo-1m          |
| OCHAIN    | (created, not populated this round)                           | future                |

## 8. Open items resolved in plan Step 0 (free `MetadataListSchemas` probe)

1. **EQUITY_DATASET / EQUITY_L1_SCHEMA**: pick the consolidated US-equities
   dataset covering the universe that supports a 1-min L1 schema. Candidates:
   `DBEQ.BASIC` / `EQUS.MINI` with `cbbo-1m`, vs `XNAS.ITCH` `bbo-1m` (Nasdaq-only).
   Probe `MetadataListSchemas` + a small `MetadataGetCost`/record-count on a couple
   picks; pin dataset + schema in the plan before writing the pull.
2. **EQUS zip date span**: confirm ≥ 20 distinct dates in `OHLC1D`; if fewer, the
   median uses all available dates (documented, not an error).

## 9. Testing

- Unit (`atx-engine` gtest): `DiskStore` partition_path/list_dates round-trip on a
  temp dir; write_partition → scan_partition row round-trip.
- Unit: median + top-100 ranking on a synthetic multi-date OHLC1D fixture
  (deterministic, no network).
- Unit: cost-split recursion against a stubbed estimator (assert every leaf < cap,
  union == input).
- Integration: the `build_universe` run itself (real API, gated < $2) — manual,
  not in CI.

## 10. Out of scope

- OCHAIN population (option definitions) — future.
- Backfilling intraday history beyond the single 15:55 minute.
- Append/merge into an existing partition (each partition written once per run).
- True fundamental market cap (replaced by liquidity proxy per decision).

## 11. Execution

Spec review → `writing-plans` → fresh git worktree (`using-git-worktrees`) →
subagent-driven development. Cost-incurring live pulls run only from the
orchestrator, each preflight-gated < $2.
