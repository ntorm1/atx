# Databento EQUS.SUMMARY Loader — Design Spec

> Status: approved (design). Next: implementation plan (writing-plans skill).
> Target branch: `feat/atx-core-stdlib` (implemented in a fresh git worktree).

## Goal

A self-contained C++20 ingest path in atx-core that takes the path to a Databento
EQUS.SUMMARY batch `.zip` (~150 MB, zstd-compressed DBN files) and a destination
data directory, then unzips, zstd-decompresses, decodes the DBN OHLCV records, and
writes a **date-partitioned Parquet hive** dataset on disk.

Public entry (in `atx/external/databento.hpp`):

```cpp
namespace atx::external::databento {

struct LoadStats {
  i64 files_processed{0};    // .dbn.zst entries decoded
  i64 records_decoded{0};    // OHLCV rows written
  i64 partitions_written{0}; // distinct date= partitions emitted
  i64 records_skipped{0};    // records of unsupported rtype
};

// Decode an EQUS.SUMMARY batch zip into <dest_dir>/date=YYYY-MM-DD/data.parquet.
// Self-contained: no network, no Python, no JSON dependency (symbols come from
// the DBN metadata symbol mappings).
[[nodiscard]] Result<LoadStats>
load_equs_summary_zip(std::string_view zip_path, std::string_view dest_dir);

} // namespace atx::external::databento
```

## Ground truth (verified against databento/dbn source + docs)

- **Container**: standard PKZIP `.zip` holding multiple `*.dbn.zst` data files plus
  JSON sidecars (`metadata.json`, `symbology.json`, `manifest.json`, `condition.json`).
  Data files are zstd frames; the zip entries are typically STORE (already compressed).
- **DBN stream** (inside each decompressed `.dbn` file): 3-byte magic `"DBN"`, 1-byte
  version, `u32` little-endian metadata length, then the metadata block, then a packed
  sequence of fixed-layout records.
- **`RecordHeader`** (`#[repr(C)]`, 16 bytes): `length:u8` (record size in 32-bit words,
  i.e. ×4 bytes), `rtype:u8`, `publisher_id:u16`, `instrument_id:u32`, `ts_event:u64`
  (ns since UNIX epoch). All little-endian.
- **`OhlcvMsg`** (56 bytes, `length`==14): `hd:RecordHeader(16)`, `open:i64`, `high:i64`,
  `low:i64`, `close:i64` (prices fixed-point, 1e-9 per unit), `volume:u64`.
- **rtype**: `OHLCV_1S=0x20`, `OHLCV_1M=0x21`, `OHLCV_1H=0x22`, `OHLCV_1D=0x23`,
  `OHLCV_EOD=0x24`. EQUS.SUMMARY delivers `ohlcv-1d` (and/or `ohlcv-eod`); both decode to
  `OhlcvMsg`. We accept `0x23` and `0x24`.
- **Symbol resolution**: the DBN metadata block carries symbol mappings
  (raw_symbol ↔ instrument_id over date intervals). We resolve symbols from metadata —
  no `symbology.json` parse, no JSON library.

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| DBN decode | Hand-rolled parser + vendored zstd/miniz | Matches the sqlite vendoring precedent; zero heavy transitive deps; full control; the struct is a fixed 56 bytes. |
| Partitioning | `date=YYYY-MM-DD/data.parquet` | Daily EOD product; clean time-range pruning; the backtest loop reads cross-sections by date. ~252 files/yr. |
| Schema scope | OHLCV only (`0x23`, `0x24`) | Exactly what EQUS.SUMMARY contains; smallest correct MVP; decoder is extensible. |
| Parquet writer | Reusable `atx::core::io` writer | `atx::core::io` is read-only today; a shared writer keeps future vendor loaders DRY. |
| Price storage | raw `i64` (units of 1e-9) | Lossless, Databento-native; documented scale; no float error. f64 knob deferred. |
| Memory | buffer all rows, then write | Simple/correct MVP; per-date streaming is a follow-up. Memory ceiling documented. |

## Components / files

Three units plus two vendored third-party libraries. Each unit has one responsibility
and a well-defined, Arrow/zip/zstd-free public interface.

### Vendored (third-party, sqlite pattern)

- `atx-core/third-party/miniz/{miniz.h,miniz.c,PROVENANCE.md}` — single-file PKZIP reader.
- `atx-core/third-party/zstd/{zstd.h,zstd.c,PROVENANCE.md}` — single-file **full** zstd
  (compress + decompress). Production uses decompress; compress is needed so tests can
  build fixtures in-process.

CMake: each builds as its own STATIC lib (`atx_miniz`, `atx_zstd`) with warnings disabled
(`/w` MSVC, `-w` else), linked **PRIVATE** into atx-core so symbols propagate to the final
link but the include dirs do **not** — `miniz.h`/`zstd.h` stay confined to the
implementation TUs. Identical to `atx_sqlite3`.

### Unit 1 — DBN parser (`atx/external/dbn.hpp`, `src/external/dbn.cpp`)

Pure DBN decode. No zip, no zstd, no Arrow — consumes an already-decompressed byte span.

- `enum class RType : u8 { ... Ohlcv1S=0x20, Ohlcv1M=0x21, Ohlcv1H=0x22, Ohlcv1D=0x23, OhlcvEod=0x24, ... }`
- `struct RecordHeader` and `struct OhlcvMsg` — exact little-endian layouts; read via
  byte-wise loads (no `reinterpret_cast` of misaligned/aliased pointers; `// SAFETY:` where
  a typed read is justified).
- `struct DbnMetadata { u8 version; std::string dataset; u16 schema; ... ; symbol mappings }`.
- `class DbnDecoder`:
  - `static Result<DbnDecoder> open(std::span<const std::byte> dbn);` — validates magic +
    version, parses the metadata block (incl. symbol mappings).
  - `const DbnMetadata& metadata() const;`
  - `std::string_view symbol_for(u32 instrument_id, /*date*/...) const;` — from mappings;
    empty if unmapped.
  - `Result<std::optional<OhlcvMsg>> next();` — yields the next OHLCV record, advancing past
    (and counting) unsupported rtypes; `nullopt` at end. Validates each record's `length`
    against the buffer bound.
- Unit-testable on **golden, hand-authored byte arrays** whose offsets are verified by hand
  against the spec — independent of our own encoder.

### Unit 2 — Parquet writer (`atx/core/io/parquet_writer.hpp`, `src/io/parquet_writer.cpp`)

Arrow lives behind PIMPL (mirrors the read-side `parquet.cpp`); the header includes no Arrow.

- `struct WriteColumn { std::string name; std::variant<std::span<const i64>,
  std::span<const f64>, std::span<const std::string>, std::span<const time::Timestamp>> data; };`
  (all columns must share the same row count.)
- `[[nodiscard]] Status write_parquet(std::span<const WriteColumn> cols, std::string_view path);`
  — one flat Parquet file.
- `[[nodiscard]] Result<i64> write_hive_parquet(std::span<const WriteColumn> cols,
  std::string_view root, std::string_view partition_col);`
  — buckets rows by the distinct values of `partition_col` (which must be a string column),
  **drops that column from the written file** (true hive convention; readers reconstruct it
  from the path), writes `<root>/<partition_col>=<value>/data.parquet` per bucket, returns
  the number of partitions written. Creates directories as needed.
- Arrow may throw internally; the writer catches and converts to `Status` (no throw across
  the surface; `std::bad_alloc` may still propagate, per existing io convention).

### Unit 3 — Loader orchestration (`atx/external/databento.hpp`, `src/external/databento.cpp`)

The entry point. Glues the other units; the only TU that includes `miniz.h` + `zstd.h`.

Flow:

```
zip_path
  → miniz: open archive, enumerate entries
  → for each "*.dbn.zst" entry:
      → miniz: extract entry bytes (STORE/DEFLATE)
      → zstd: decompress to a byte buffer
      → DbnDecoder::open → metadata (symbol mappings)
      → loop next(): for each OhlcvMsg →
          ts    = Timestamp(ns = hd.ts_event)
          symbol= decoder.symbol_for(hd.instrument_id, date)   // fallback: to_string(id)
          o/h/l/c = open/high/low/close  (i64, 1e-9)
          volume  = (i64) volume          // overflow-guarded
          date    = civil YYYY-MM-DD from ts_event             // partition key
        accumulate into column vectors; count skipped rtypes
  → io::write_hive_parquet(cols, dest_dir, "date")
  → LoadStats
```

Output row schema (per `data.parquet`): `ts:Timestamp`, `symbol:string`,
`open:i64`, `high:i64`, `low:i64`, `close:i64`, `volume:i64`. (`date` is in the path only.)

## Error handling

All expected failures travel in `Result`/`Status`; the surface never throws (except
`std::bad_alloc`).

- zip path missing / unreadable → `IoError`.
- malformed zip → `ParseError`.
- zstd decode failure → `ParseError`.
- bad DBN magic or unsupported version → `ParseError`.
- unsupported rtype → **skipped and counted**, not fatal.
- zero OHLCV records decoded across all files → `Err(InvalidArgument, "no OHLCV records")`
  (guards against pointing the loader at the wrong product).
- Arrow write failure → `Status` (caught in writer).

## Testing (TDD, red → green → refactor)

1. **DBN parser** (`dbn_test.cpp`): hand-authored golden byte arrays (offsets verified vs
   spec) → assert metadata fields, symbol mapping resolution, OHLCV field decode, `length`
   validation, unsupported-rtype skip. No encoder involved — independent verification.
2. **Parquet writer** (`parquet_writer_test.cpp`): build `WriteColumn`s → `write_parquet`
   → read back via existing `LazyParquet`/`read_parquet`, assert values round-trip;
   `write_hive_parquet` → assert hive dirs created, partition col absent from files, per-
   partition row counts correct.
3. **Loader integration** (`databento_test.cpp`): build a DBN byte buffer in-process
   (multi-symbol, multi-day) → zstd-compress (vendored zstd) → assemble an in-memory zip
   (vendored miniz) → write a temp `.zip` → `load_equs_summary_zip` → read back the
   partitions, assert OHLCV/symbol/ts round-trip and `LoadStats`. Edge cases: unsupported
   rtype skipped+counted; corrupt zip → `Err`; bad DBN magic → `Err`; zero-OHLCV → `Err`;
   multi-day → multiple partitions; multi-symbol within a day.
4. **Stretch**: vendor one small real `ohlcv-1d.dbn.zst` from databento/dbn test data under
   `atx-core/tests/data/` as a golden fixture (real-encoder verification; structural
   asserts on record count / monotone ts / positive prices).

## Build / standards

- `/W4 /permissive- /WX` (clang-cl) on atx targets; vendored C compiled with warnings off.
- C language already enabled at the top-level project (sqlite). zstd/miniz are C.
- RAII / Rule of Zero-Five; `Result<T>`/`Status` for expected failures; no hot-path alloc
  in the parser inner loop; exhaustive enum switches; functions ≤60 lines; `// SAFETY:`
  comments on every byte-level typed read.

## Out of scope (follow-ups)

- Non-OHLCV DBN schemas (trades, mbp-1/10, statistics, definition, status).
- Per-date streaming write (bounded memory for very large archives).
- f64 / Decimal price column option.
- Network download from the Databento Historical API (this is local-file ingest only).
