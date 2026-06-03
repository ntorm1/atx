# atx-core `io::parquet` — Arrow-backed lazy Parquet loader

**Date:** 2026-06-02
**Status:** Approved (design)
**Module:** `atx::core::io` (new `io` layer)
**Files:** `include/atx/core/io/parquet.hpp`, `src/io/parquet.cpp`, `tests/parquet_test.cpp`

## 1. Goal

A complete, correct, fast Parquet reader for atx-core, with **lazy loading inspired
by Polars `LazyFrame`**: a scan plan that defers I/O until `collect()`, pushing
projection and predicates down to the row-group level, streaming row groups for
bounded memory, and supporting all common Parquet dtypes and compression codecs.

Apache Arrow + Parquet provide the decode substrate ("use arrow lib"). We do **not**
reinvent Parquet decoding; we build the *lazy plan, pushdown, pruning, streaming,
materialization, and atx-native bridges* on top of Arrow.

## 2. Scope

### In scope
- **Lazy scan plan** (`LazyParquet`): `scan` (metadata only) → `select` / `filter` /
  `limit` / `offset` → `collect` / `stream`.
- **Projection pushdown** — only selected (∪ predicate) column chunks are decoded.
- **Predicate pushdown + row-group pruning** — skip whole row groups whose
  column-chunk statistics (min/max/null_count) prove the conjunction unsatisfiable;
  exact row-level filtering on survivors for correctness.
- **Row-group streaming** — iterate per-row-group batches with bounded memory.
- **Slice / limit** — `head`/`offset`; read only the row groups needed.
- **All common dtypes** — bool, int8–64, uint8–64, f32/f64, string/binary, date32,
  timestamp (any unit), decimal128; dictionary-encoded decoded transparently.
- **All common compression** — uncompressed, snappy, gzip, zstd, lz4 (transparent via
  Arrow); brotli when enabled in the linked Arrow build. Tests probe the codecs the
  linked Arrow actually supports rather than hard-requiring brotli.
- **Materialization** — opaque wrapped Arrow `Table` (`ParquetTable`) plus zero-copy
  and owned-copy bridges to atx `series::Column<T>` / `Frame` for the numeric subset,
  and a `strings()` accessor for UTF-8 columns.
- **Schema introspection** — `Schema` / `ColumnInfo` / `DType` from footer metadata.

### Out of scope (YAGNI / other layers)
- Parquet **writing** (tests use Arrow's writer directly; no `write_parquet` in the module).
- Query engine: groupby / join / sort / expression VM → belongs to the atx-engine
  alpha-DSL layer, not core `io`.
- Nested-type (list/struct) atx bridges — schema reports `Unsupported`, bridge errors
  `NotImplemented`.
- Remote filesystems (S3/HDFS), encryption, multi-file datasets.

## 3. Approach

### 3.1 Execution substrate — direct `parquet::arrow::FileReader` (chosen)
Build the lazy plan over `parquet::arrow::FileReader`, **not** `arrow::dataset`/Acero.

- **Why:** dep surface stays `libarrow` + `libparquet` only; full control over
  row-group pruning and streaming (the core "operations" to build); the pruning over
  Parquet `Statistics` is the educational substrate. Acero would be less code but a
  larger dependency and an opaque executor.
- **Alternative considered:** `arrow::dataset` (native pushdown via
  `compute::Expression`) — rejected for the extra `libarrow_dataset`/Acero dependency
  and loss of control.

### 3.2 Arrow isolation — PIMPL
`parquet.hpp` includes **zero Arrow headers** (only `<atx/...>` + std). All Arrow
types (`std::shared_ptr<arrow::Table>`, `parquet::arrow::FileReader`,
`arrow::compute` calls) live behind `struct Impl` defined only in `parquet.cpp`.

- Keeps the umbrella header and clangd free of Arrow's heavy, exception-throwing headers.
- Contains Arrow's exception model in one TU (see §6 firewall).
- Honors the repo rule: prefer specific headers, keep compile times down.
- Cost: `ParquetTable` / `LazyParquet` are move-only `unique_ptr<Impl>` handles;
  accessors marshal across the boundary.

### 3.3 Build integration — vcpkg manifest, required
- Root `vcpkg.json` manifest: `arrow` with the `parquet` feature (pulls thrift +
  the snappy/zstd/lz4 codecs the port enables by default).
- `CMakePresets.json`: the `ninja` preset sets
  `CMAKE_TOOLCHAIN_FILE = $env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake`.
- `atx-core/CMakeLists.txt`: `find_package(Arrow CONFIG REQUIRED)` +
  `find_package(Parquet CONFIG REQUIRED)`; compile `src/io/parquet.cpp`; link
  `Arrow::arrow_shared` + `Parquet::parquet_shared`. Parquet is a **required** core
  dependency (per decision); `VCPKG_ROOT` must be set and Arrow installed to build.
- Triplet `x64-windows` (dynamic CRT) matches `gtest_force_shared_crt ON`. Runtime
  DLLs are copied next to test/bench executables (vcpkg applocal / `add_custom_command`).
- Arrow is included as a **SYSTEM** dependency so its headers never trip
  `/W4 /permissive- /WX` on atx targets.

## 4. Public API (`include/atx/core/io/parquet.hpp`, Arrow-free)

```cpp
namespace atx::core::io {

enum class DType {
  Bool, Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64,
  Float32, Float64, String, Binary, Date32, Timestamp, Decimal128, Unsupported
};

struct ColumnInfo { std::string name; DType dtype; bool nullable; };

struct Schema {
  std::vector<ColumnInfo> columns;
  [[nodiscard]] const ColumnInfo* find(std::string_view name) const noexcept;
  [[nodiscard]] usize size() const noexcept { return columns.size(); }
};

// Arrow-free predicate literal. Closed variant over the supported scalar kinds.
class Scalar {
  // variant<std::monostate, bool, i64, f64, std::string, time::Timestamp>
  // ctors: Scalar(bool|i64|f64|std::string|Timestamp); kind()/get<T>()
};

enum class Compare { Eq, Ne, Lt, Le, Gt, Ge, IsNull, IsNotNull };
struct Predicate { std::string column; Compare op; Scalar value; };

struct ScanStats { i64 row_groups_total; i64 row_groups_pruned; i64 rows_scanned; };

// Materialized result — opaque Arrow Table. Move-only.
class ParquetTable {
public:
  ParquetTable(ParquetTable&&) noexcept;
  ParquetTable& operator=(ParquetTable&&) noexcept;
  ~ParquetTable();

  [[nodiscard]] i64 num_rows() const noexcept;
  [[nodiscard]] i64 num_columns() const noexcept;
  [[nodiscard]] const Schema& schema() const noexcept;

  // Zero-copy view aliasing the Arrow buffer; valid while *this is alive.
  // Numeric/POD columns only; errors InvalidArgument on type/name mismatch,
  // NotImplemented if the column is chunked non-contiguously after combine.
  template <class T>
  [[nodiscard]] Result<std::span<const T>> column_view(std::string_view name) const;

  // Owned copy into atx storage (one bulk memcpy via Column::append_bulk).
  template <class T>
  [[nodiscard]] Result<series::Column<T>> to_column(std::string_view name) const;

  // Numeric + timestamp columns → Frame (strings/nested skipped; reported).
  [[nodiscard]] Result<series::Frame> to_frame() const;

  // UTF-8 string column → per-row views (valid while *this is alive).
  [[nodiscard]] Result<std::vector<std::string_view>> strings(std::string_view name) const;

  struct Impl;
private:
  explicit ParquetTable(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class LazyParquet;
};

// Lazy plan — nothing is read until collect()/stream(). scan() reads footer only.
class LazyParquet {
public:
  [[nodiscard]] static Result<LazyParquet> scan(std::string_view path);

  LazyParquet(LazyParquet&&) noexcept;
  LazyParquet& operator=(LazyParquet&&) noexcept;
  ~LazyParquet();

  [[nodiscard]] const Schema& schema() const noexcept;
  [[nodiscard]] i64 num_rows() const noexcept;       // from metadata
  [[nodiscard]] i64 num_row_groups() const noexcept; // from metadata

  LazyParquet& select(std::vector<std::string> columns); // projection pushdown
  LazyParquet& filter(Predicate p);                      // ANDed conjunction
  LazyParquet& limit(i64 n);
  LazyParquet& offset(i64 n);

  [[nodiscard]] Result<ParquetTable> collect();   // prune → read → filter → slice
  [[nodiscard]] Result<class RowGroupStream> stream();
  [[nodiscard]] ScanStats stats() const noexcept; // populated by the last collect/stream

  struct Impl;
private:
  explicit LazyParquet(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
};

// Streaming iterator over surviving row groups (already projected/filtered/sliced).
class RowGroupStream {
public:
  RowGroupStream(RowGroupStream&&) noexcept;
  RowGroupStream& operator=(RowGroupStream&&) noexcept;
  ~RowGroupStream();
  [[nodiscard]] Result<std::optional<ParquetTable>> next(); // nullopt at end
  struct Impl;
private:
  explicit RowGroupStream(std::unique_ptr<Impl>) noexcept;
  std::unique_ptr<Impl> impl_;
  friend class LazyParquet;
};

// Eager convenience: scan(path).collect().
[[nodiscard]] Result<ParquetTable> read_parquet(std::string_view path);

} // namespace atx::core::io
```

## 5. Data flow — `collect()`

1. **Prune.** For each row group, evaluate the predicate conjunction against each
   referenced column's chunk `Statistics` (min/max/null_count via Parquet metadata).
   A group is skipped iff some conjunct is provably false for the whole group
   (e.g. `col > v` with `max <= v`; `col == v` with `v < min || v > max`; `IsNotNull`
   with all-null). Missing statistics → keep the group (conservative). Update `ScanStats`.
2. **Read.** Decode only the `(selected ∪ predicate)` column chunks of surviving
   groups (projection pushdown via `FileReader` column indices).
3. **Filter.** Build an exact boolean mask with `arrow::compute` (`equal`, `less`,
   `and_`, `is_null`, …) and `Filter` the batch — exact Polars-style row semantics,
   not just group pruning. Predicate-only helper columns are dropped from the output.
4. **Slice.** Apply `offset` then `limit`. Streaming carries the remaining row budget
   across groups and stops once satisfied.

`stream()` performs the same per-group, yielding one `ParquetTable` per surviving
group (post projection/filter/slice), `nullopt` when exhausted — bounded memory.

## 6. Error handling — exception firewall

`parquet.cpp` wraps every Arrow call. `arrow::Status` → atx `Err`:

| Condition | ErrorCode |
|-----------|-----------|
| File does not exist | `NotFound` |
| I/O failure opening/reading | `IoError` |
| Corrupt file / Thrift metadata parse failure | `ParseError` |
| Unsupported dtype/feature (nested bridge, decimal scale > 9) | `NotImplemented` |
| Unknown column / predicate type mismatch (validated pre-Arrow) | `InvalidArgument` |
| Any other Arrow failure | `Internal` |

Implementation bodies are wrapped in `try { … } catch (const std::bad_alloc&) { throw; }
catch (const std::exception& e) { return Err(Internal, e.what()); } catch (...) {
return Err(Internal, "unknown"); }`. The public surface never throws; only
`std::bad_alloc` propagates (profile contract). Column/predicate names and types are
validated against the schema **before** issuing Arrow calls so user errors surface as
`InvalidArgument`, not `Internal`.

## 7. Dtype mapping

| Arrow / Parquet type | `DType` | atx bridge |
|----------------------|---------|------------|
| bool | `Bool` | `Column<bool>` |
| int8/16/32/64 | `Int8..Int64` | `Column<i8..i64>` |
| uint8/16/32/64 | `UInt8..UInt64` | `Column<u8..u64>` |
| float / double | `Float32` / `Float64` | `Column<float>` / `Column<double>` |
| utf8 / large_utf8 | `String` | `strings()` only (no POD bridge) |
| binary / large_binary | `Binary` | `strings()`-style bytes only |
| date32 | `Date32` | `Column<i32>` (days since epoch) |
| timestamp(unit, tz) | `Timestamp` | `Column<time::Timestamp>` (unit → ns) |
| decimal128(p,s) | `Decimal128` | atx `Decimal` when s ≤ 9, else `NotImplemented` |
| dictionary<...> | underlying | decoded transparently |
| list / struct / map | `Unsupported` | bridge errors `NotImplemented` |

`to_frame()` includes the numeric + timestamp columns; string/binary/nested columns
are skipped (their names returned/logged), since `Frame`/`Column<T>` are POD-only.

## 8. Testing (TDD, `tests/parquet_test.cpp`)

Fixtures generated in-test via `parquet::arrow::WriteTable` to a temp directory — no
committed binaries, true round-trip. Each behavior Red → Green:

1. `scan()` reports schema/`num_rows`/`num_row_groups` from metadata without materializing.
2. `read_parquet` round-trips a numeric table with nulls (values + validity).
3. All dtypes round-trip (bool/int/uint/f32/f64/string/date32/timestamp/decimal128).
4. Every supported codec (uncompressed/snappy/gzip/zstd/lz4; brotli if enabled) reads
   back identical — the test enumerates the codecs the linked Arrow advertises.
5. `select(subset)` → only those columns present; non-selected absent.
6. `filter` exactness: single comparison and multi-predicate conjunction return
   exactly the matching rows.
7. **Row-group pruning observable** via `ScanStats`: a multi-row-group file with a
   predicate excluding some groups yields `row_groups_pruned > 0` and correct rows.
8. `limit`/`offset` return the right slice and touch only the needed row groups.
9. `stream()` batches concatenate to the full filtered/sliced result; bounded.
10. Bridges: `to_column<T>` / `to_frame` copy correct values; `column_view<T>` aliases;
    `strings()` returns correct UTF-8.
11. Error paths: missing file → `NotFound`; unknown column in `select`/`filter` →
    `InvalidArgument`; predicate type mismatch → `InvalidArgument`; corrupt file →
    `ParseError`; nested-type bridge → `NotImplemented`.

`tests/CMakeLists.txt` adds `parquet_test.cpp` and links Arrow + Parquet.

## 9. Integration

- `core.hpp` umbrella: add an **L10 — io** group including `io/parquet.hpp`.
- `README.md`: add an `io` layer row documenting `io/parquet.hpp`, plus a note on the
  vcpkg/Arrow build requirement and the `VCPKG_ROOT` toolchain wiring.
- New `vcpkg.json` manifest at repo root.

## 10. File-by-file summary

| File | Change |
|------|--------|
| `vcpkg.json` | New manifest: `arrow[parquet]`, builtin baseline. |
| `CMakePresets.json` | `ninja` preset sets vcpkg `CMAKE_TOOLCHAIN_FILE`. |
| `atx-core/CMakeLists.txt` | `find_package(Arrow/Parquet)`, compile `src/io/parquet.cpp`, link, DLL copy. |
| `atx-core/include/atx/core/io/parquet.hpp` | New Arrow-free public header (PIMPL). |
| `atx-core/src/io/parquet.cpp` | New Arrow-dependent impl + exception firewall. |
| `atx-core/tests/parquet_test.cpp` | New TDD suite (Arrow writer fixtures). |
| `atx-core/tests/CMakeLists.txt` | Add `parquet_test.cpp`; link Arrow/Parquet. |
| `atx-core/include/atx/core/core.hpp` | Add L10 io group. |
| `atx-core/README.md` | Document io layer + vcpkg/Arrow build requirement. |
