# Databento Daily-OHLCV Phase 2 — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the date-partitioned daily-OHLCV Parquet store consumable engine-wide at scale: bound loader memory + compress output (U1), bridge the int64→f64 gap into per-date TSDB segments (U2), and feed those segments to the backtest loop in date order (U3).

**Architecture:** Three sequential units. U1 changes the existing atx-core loader/writer (per-file streaming write, date memoization, ZSTD+dictionary). U2 adds an int64-scaling per-date segment builder to atx-tsdb (the single i64→f64 ÷1e9 conversion site). U3 adds a `MultiSegmentBarFeed` to atx-engine that composes the existing `ShmBarFeed` over an ordered list of per-date segments. Adjustment is out of scope (spec appendix).

**Tech Stack:** C++20, CMake+Ninja+clang-cl, Arrow/Parquet, atx-tsdb sealed segments, GoogleTest. `Result`/`Status`, `/W4 /permissive- /WX`.

**Spec:** `atx-core/plans/2026-06-06-databento-phase2-design.md`.

---

## Build / test conventions (this worktree)

An MSVC env is required to build; use the wrapper (created in Phase 1):
`C:\Users\natha\atx-wt\dbenv.cmd` (calls vcvars64 + sets `VCPKG_ROOT` + cd's into the worktree).

- Reconfigure (only after CMake edits): `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --preset ninja"`
- Build everything (incremental): `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build"`
- Run a subset by test-name regex (works across all test targets):
  `ctest --test-dir build -R "Databento|ParquetWriter|LoadParquet|MultiSegment" --output-on-failure`
- atx-core tests can also run directly: `build\bin\atx-core-tests.exe --gtest_filter='...'`
- Format gate: `clang-format -i <files>` then `git diff --exit-code`.

Test-discovery: **atx-core** tests are an explicit list in `atx-core/tests/CMakeLists.txt`; **atx-tsdb** and **atx-engine** tests are auto-globbed (`*_test.cpp` picked up automatically — verify by configuring; if a new file is not discovered, add it to that subdir's `CMakeLists.txt`).

---

## Task 1: U1 — per-file streaming write + date memoization (atx-core loader)

Today `load_equs_summary_zip` accumulates ALL rows from ALL `.dbn.zst` entries into one `Columns`, then does one global `write_hive_parquet`. Change it to decode+write **one entry at a time** (each entry is one trading day): peak memory becomes one day, and partitions written before a later failure persist. Also memoize the date string (one `snprintf` per distinct day instead of per row).

**Files:**
- Modify: `atx-core/src/external/databento.cpp`
- Test: `atx-core/tests/databento_test.cpp`

- [ ] **Step 1: Write the failing test (partial write persists past a later error)**

Append to `atx-core/tests/databento_test.cpp`:

```cpp
TEST(Databento, PerFilePartialWritePersistsBeforeError) {
  using atx::test::OhlcvRow;
  // Entry 1: valid, date 2024-01-02. Entry 2: a zstd blob whose DBN magic is bad.
  const std::vector<OhlcvRow> day = {{101, kJan02, 10, 11, 9, 10, 100, 0x23}};
  auto good = atx::test::build_dbn({{101, "AAPL", 20240101, 20250101}}, day);
  auto good_zst = atx::test::zstd_compress(good);

  std::vector<std::byte> bad_dbn(64, std::byte(0));
  bad_dbn[0] = std::byte('X'); // not "DBN" -> DbnDecoder::open fails
  auto bad_zst = atx::test::zstd_compress(bad_dbn);

  auto zip = atx::test::build_zip(
      {{"a-20240102.dbn.zst", good_zst}, {"b-bad.dbn.zst", bad_zst}});
  const fs::path zip_path = write_zip("atx_db_partial.zip", zip);
  const fs::path dest = fs::temp_directory_path() / "atx_db_partial_dest";
  fs::remove_all(dest);

  auto stats = db::load_equs_summary_zip(zip_path.string(), dest.string());
  EXPECT_FALSE(stats.has_value()); // the bad second entry aborts the load
  // ...but the first day's partition was already written (per-file streaming).
  EXPECT_TRUE(fs::exists(dest / "date=2024-01-02" / "data.parquet"));
}
```

- [ ] **Step 2: Run it; verify it FAILS**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build --target atx-core-tests"` then
`build\bin\atx-core-tests.exe --gtest_filter='Databento.PerFilePartialWritePersistsBeforeError'`
Expected: FAIL — the partition does NOT exist (current code accumulates globally and writes only at the end, so an error before that writes nothing).

- [ ] **Step 3: Refactor the loader to per-file streaming write + memoized date**

In `atx-core/src/external/databento.cpp`, replace the `date_string` per-row usage and the load loop.

First, change `decode_into` to memoize the date by day index (replace its date push):

```cpp
// inside decode_into(...), replace `c.date.push_back(date_string(m.hd.ts_event));`
// with a per-day memo so identical dates are formatted once.
```

Concretely, restructure `decode_into` to hold a memo across the record loop:

```cpp
[[nodiscard]] Result<void> decode_into(std::span<const std::byte> dbn, Columns &c, i64 &decoded,
                                       i64 &skipped) {
  ATX_TRY(auto dec, dbn::DbnDecoder::open(dbn));
  constexpr i64 kNsPerDay = 86'400'000'000'000LL;
  i64 cached_day = std::numeric_limits<i64>::min();
  std::string cached_date;
  for (;;) {
    ATX_TRY(auto rec, dec.next());
    if (!rec.has_value()) {
      break;
    }
    const auto &m = *rec;
    const i64 ns = static_cast<i64>(m.hd.ts_event);
    const i64 day = ns / kNsPerDay; // floor for ns >= 0 (daily bars are positive)
    if (day != cached_day) {
      cached_day = day;
      cached_date = date_string(m.hd.ts_event);
    }
    c.ts.push_back(time::Timestamp::from_unix_nanos(ns));
    const std::string_view sym = dec.symbol_for(m.hd.instrument_id);
    c.symbol.push_back(sym.empty() ? std::to_string(m.hd.instrument_id) : std::string{sym});
    c.open.push_back(m.open);
    c.high.push_back(m.high);
    c.low.push_back(m.low);
    c.close.push_back(m.close);
    c.volume.push_back(m.volume > static_cast<u64>(std::numeric_limits<i64>::max())
                           ? std::numeric_limits<i64>::max()
                           : static_cast<i64>(m.volume));
    c.date.push_back(cached_date);
    ++decoded;
  }
  skipped += dec.skipped_records();
  return Ok();
}
```

Then replace the body of `load_equs_summary_zip` between opening the archive and the final
return with a per-entry decode+write (a small helper builds the `WriteColumn` vector and writes
that entry's partitions):

```cpp
// helper in the anonymous namespace:
[[nodiscard]] Result<i64> write_columns(const Columns &c, std::string_view dest_dir) {
  const std::vector<atx::core::io::WriteColumn> wcols = {
      {"ts", std::span<const time::Timestamp>(c.ts)},
      {"symbol", std::span<const std::string>(c.symbol)},
      {"open", std::span<const i64>(c.open)},
      {"high", std::span<const i64>(c.high)},
      {"low", std::span<const i64>(c.low)},
      {"close", std::span<const i64>(c.close)},
      {"volume", std::span<const i64>(c.volume)},
      {"date", std::span<const std::string>(c.date)},
  };
  return atx::core::io::write_hive_parquet(wcols, dest_dir, "date");
}
```

```cpp
// load loop body (replaces the global-accumulate version):
  i64 total_partitions = 0;
  for (mz_uint i = 0; i < count; ++i) {
    mz_zip_archive_file_stat st{};
    if (!mz_zip_reader_file_stat(&zip, i, &st)) {
      continue;
    }
    const std::string_view name{static_cast<const char *>(st.m_filename)};
    const bool is_zst = ends_with(name, ".dbn.zst");
    const bool is_raw = ends_with(name, ".dbn");
    if (!is_zst && !is_raw) {
      continue;
    }
    std::size_t raw_size = 0;
    void *raw = mz_zip_reader_extract_to_heap(&zip, i, &raw_size, 0);
    if (raw == nullptr) {
      mz_zip_reader_end(&zip);
      return Err(ErrorCode::ParseError, std::string{"failed to extract "} + std::string{name});
    }
    const std::span<const std::byte> entry{static_cast<const std::byte *>(raw), raw_size};

    Columns cols; // ONE file's rows only -> memory bounded to one day
    Result<void> step = Ok();
    if (is_zst) {
      auto dec = zstd_decompress(entry);
      if (!dec.has_value()) {
        mz_free(raw);
        mz_zip_reader_end(&zip);
        return Err(dec.error());
      }
      step = decode_into(*dec, cols, stats.records_decoded, stats.records_skipped);
    } else {
      step = decode_into(entry, cols, stats.records_decoded, stats.records_skipped);
    }
    mz_free(raw);
    if (!step.has_value()) {
      mz_zip_reader_end(&zip);
      return Err(step.error());
    }
    if (!cols.ts.empty()) {
      auto n = write_columns(cols, dest_dir);
      if (!n.has_value()) {
        mz_zip_reader_end(&zip);
        return Err(n.error());
      }
      total_partitions += *n;
    }
    ++stats.files_processed;
  }
  mz_zip_reader_end(&zip);

  if (stats.records_decoded == 0) {
    return Err(ErrorCode::InvalidArgument, "no OHLCV records found in archive");
  }
  stats.partitions_written = total_partitions;
  return Ok(stats);
```

Remove the now-unused global `Columns cols;` / global `wcols` / trailing `write_hive_parquet`
that the old body had. Keep `<limits>` included (used by the memo + volume clamp).

> Note on `partitions_written`: it now counts partition-files written across entries. For
> EQUS.SUMMARY (one disjoint date per entry) this equals the distinct-date count. If two entries
> ever map to the same date, the later overwrites the earlier and the count double-counts — not a
> case EQUS produces; documented, not handled.

- [ ] **Step 4: Run; verify the new test PASSES and the existing Databento tests still pass**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build --target atx-core-tests"` then
`build\bin\atx-core-tests.exe --gtest_filter='Databento.*'`
Expected: PASS (incl. `PerFilePartialWritePersistsBeforeError`; `LoadsZipIntoDatePartitions`,
`ZeroOhlcvErrors`, `SkipsUnsupportedAndCounts`, `MissingZipErrors` unchanged).

- [ ] **Step 5: Format + commit**

```bash
clang-format -i atx-core/src/external/databento.cpp atx-core/tests/databento_test.cpp
git add atx-core/src/external/databento.cpp atx-core/tests/databento_test.cpp
git commit -m "perf(core): per-file streaming write + date memoization in databento loader"
```

---

## Task 2: U1 — ZSTD + dictionary compression in the Parquet writer (atx-core)

Add an Arrow-free `WriteOptions` to the writer and wire it through to `parquet::WriterProperties`.
Default ZSTD + dictionary on. The loader picks up the default automatically (it calls
`write_hive_parquet` with no options argument).

**Files:**
- Modify: `atx-core/include/atx/core/io/parquet_writer.hpp`, `atx-core/src/io/parquet_writer.cpp`
- Test: `atx-core/tests/parquet_writer_test.cpp`

- [ ] **Step 1: Write the failing test (compression shrinks the file; needs the new param)**

Append to `atx-core/tests/parquet_writer_test.cpp`:

```cpp
TEST(ParquetWriter, ZstdShrinksHighlyCompressibleFile) {
  std::vector<i64> v(2000, 42); // very compressible
  std::vector<std::string> sym(2000, std::string("AAAA"));
  const std::vector<io::WriteColumn> cols = {
      {"v", std::span<const i64>(v)},
      {"sym", std::span<const std::string>(sym)},
  };
  const fs::path none = fs::temp_directory_path() / "atx_pqw_none.parquet";
  const fs::path zstd = fs::temp_directory_path() / "atx_pqw_zstd.parquet";
  fs::remove(none);
  fs::remove(zstd);

  ASSERT_TRUE(io::write_parquet(cols, none.string(),
                                io::WriteOptions{io::Compression::None, false})
                  .has_value());
  ASSERT_TRUE(io::write_parquet(cols, zstd.string(),
                                io::WriteOptions{io::Compression::Zstd, true})
                  .has_value());

  EXPECT_LT(fs::file_size(zstd), fs::file_size(none));

  // Both round-trip identically.
  auto t = io::read_parquet(zstd.string());
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 2000);
}
```

- [ ] **Step 2: Run it; verify it FAILS (does not compile — `WriteOptions` undefined)**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build --target atx-core-tests"`
Expected: compile error — `io::WriteOptions` / `io::Compression` undefined. (Red.)

- [ ] **Step 3: Add `WriteOptions` to the header**

In `atx-core/include/atx/core/io/parquet_writer.hpp`, after the `WriteColumn` struct, add:

```cpp
enum class Compression { None, Snappy, Zstd };

struct WriteOptions {
  Compression compression{Compression::Zstd};
  bool dictionary{true};
};
```

and add a defaulted `opts` parameter to both signatures:

```cpp
[[nodiscard]] Status write_parquet(std::span<const WriteColumn> cols, std::string_view path,
                                   WriteOptions opts = {});

[[nodiscard]] Result<i64> write_hive_parquet(std::span<const WriteColumn> cols,
                                             std::string_view root, std::string_view partition_col,
                                             WriteOptions opts = {});
```

- [ ] **Step 4: Thread `opts` through the implementation**

In `atx-core/src/io/parquet_writer.cpp`, add `#include <parquet/properties.h>` to the includes,
then map options to parquet properties in `write_table` and pass `opts` down.

Replace `write_table` and the two public functions' signatures:

```cpp
[[nodiscard]] parquet::Compression::type to_parquet(Compression c) noexcept {
  switch (c) {
  case Compression::None:
    return parquet::Compression::UNCOMPRESSED;
  case Compression::Snappy:
    return parquet::Compression::SNAPPY;
  case Compression::Zstd:
    return parquet::Compression::ZSTD;
  }
  return parquet::Compression::UNCOMPRESSED;
}

[[nodiscard]] Status write_table(const std::shared_ptr<arrow::Table> &table,
                                 const std::string &path, WriteOptions opts) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path{path}.parent_path(), ec);
  auto sink_r = arrow::io::FileOutputStream::Open(path);
  if (!sink_r.ok()) {
    return atx::core::Err(from_arrow(sink_r.status(), "open output"));
  }
  parquet::WriterProperties::Builder pb;
  pb.compression(to_parquet(opts.compression));
  if (opts.dictionary) {
    pb.enable_dictionary();
  } else {
    pb.disable_dictionary();
  }
  const std::shared_ptr<parquet::WriterProperties> props = pb.build();
  auto st = parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), *sink_r,
                                       /*chunk_size=*/1 << 20, props,
                                       parquet::default_arrow_writer_properties());
  if (!st.ok()) {
    return atx::core::Err(from_arrow(st, "write parquet"));
  }
  auto cs = (*sink_r)->Close();
  if (!cs.ok()) {
    return atx::core::Err(from_arrow(cs, "close output"));
  }
  return atx::core::Ok();
}
```

Update the two callers of `write_table` to pass `opts`:

```cpp
Status write_parquet(std::span<const WriteColumn> cols, std::string_view path, WriteOptions opts) {
  ATX_TRY_VOID(validate_lengths(cols));
  const auto rows = all_rows(column_rows(cols.front()));
  auto table = build_table(cols, rows, /*skip=*/"");
  if (!table.ok()) {
    return atx::core::Err(from_arrow(table.status(), "build table"));
  }
  return write_table(*table, std::string{path}, opts);
}
```

and in `write_hive_parquet`, change the per-bucket write line to:

```cpp
    ATX_TRY_VOID(write_table(*table, path, opts));
```

(`write_hive_parquet` gains the `WriteOptions opts` parameter to match the header; pass it through.)

- [ ] **Step 5: Run; verify PASS**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build --target atx-core-tests"` then
`build\bin\atx-core-tests.exe --gtest_filter='ParquetWriter.*'`
Expected: PASS (incl. `ZstdShrinksHighlyCompressibleFile`).

- [ ] **Step 6: Format + commit**

```bash
clang-format -i atx-core/include/atx/core/io/parquet_writer.hpp atx-core/src/io/parquet_writer.cpp atx-core/tests/parquet_writer_test.cpp
git add atx-core/include/atx/core/io/parquet_writer.hpp atx-core/src/io/parquet_writer.cpp atx-core/tests/parquet_writer_test.cpp
git commit -m "feat(core): ZSTD+dictionary WriteOptions for the parquet writer (default on)"
```

---

## Task 3: U2 — int64-scaled per-date segment bridge (atx-tsdb)

Add the single i64→f64 conversion: read int64 field columns, scale each (×scale) to f64, and build
sealed segments — one per `date=…` partition. `load_parquet`'s existing f64 path is untouched.

**Files:**
- Modify: `atx-tsdb/include/atx/tsdb/load_parquet.hpp`, `atx-tsdb/src/load_parquet.cpp`
- Test: `atx-tsdb/tests/load_parquet_test.cpp`

- [ ] **Step 1: Write the failing test (scaled read + per-date segments)**

Append to `atx-tsdb/tests/load_parquet_test.cpp` (it already links atx-core io and atx-tsdb;
add includes `#include "atx/core/io/parquet_writer.hpp"`, `#include "atx/tsdb/segment_reader.hpp"`,
`#include <filesystem>`, `#include <span>`, `#include <string>`, `#include <vector>` if absent):

```cpp
TEST(BuildDatedSegments, ScalesI64FieldsAndPartitions) {
  namespace io = atx::core::io;
  namespace fs = std::filesystem;
  using atx::core::time::Timestamp;

  const fs::path root = fs::temp_directory_path() / "atx_dated_root";
  const fs::path segs = fs::temp_directory_path() / "atx_dated_segs";
  fs::remove_all(root);
  fs::remove_all(segs);

  // Two date partitions, prices as int64 1e-9 (2.0 and 3.0), volume int64 (1500).
  auto write_day = [&](const std::string &date, std::int64_t close_i64) {
    const std::vector<Timestamp> ts = {Timestamp::from_unix_nanos(100)};
    const std::vector<std::string> sym = {"AAA"};
    const std::vector<atx::i64> close = {close_i64};
    const std::vector<atx::i64> vol = {1500};
    const std::vector<io::WriteColumn> cols = {
        {"ts", std::span<const Timestamp>(ts)},
        {"symbol", std::span<const std::string>(sym)},
        {"close", std::span<const atx::i64>(close)},
        {"volume", std::span<const atx::i64>(vol)},
    };
    const fs::path p = root / ("date=" + date) / "data.parquet";
    ASSERT_TRUE(io::write_parquet(cols, p.string()).has_value());
  };
  write_day("2024-01-02", 2'000'000'000); // -> 2.0
  write_day("2024-01-03", 3'000'000'000); // -> 3.0

  const std::vector<std::pair<std::string, atx::f64>> scales = {{"close", 1e-9}, {"volume", 1.0}};
  ASSERT_TRUE(atx::tsdb::build_dated_segments(root.string(), segs.string(), "ts", "symbol", scales, 0)
                  .has_value());

  auto r2 = atx::tsdb::SegmentReader::attach((segs / "2024-01-02.seg").string());
  ASSERT_TRUE(r2.has_value());
  const auto fc = r2->field_index("close");
  ASSERT_TRUE(fc.has_value());
  EXPECT_DOUBLE_EQ(r2->value(*fc, 0, 0), 2.0); // i64 2e9 * 1e-9
  const auto fv = r2->field_index("volume");
  ASSERT_TRUE(fv.has_value());
  EXPECT_DOUBLE_EQ(r2->value(*fv, 0, 0), 1500.0);

  auto r3 = atx::tsdb::SegmentReader::attach((segs / "2024-01-03.seg").string());
  ASSERT_TRUE(r3.has_value());
  EXPECT_DOUBLE_EQ(r3->value(*r3->field_index("close"), 0, 0), 3.0);
}
```

- [ ] **Step 2: Run it; verify it FAILS (does not compile — `build_dated_segments` undefined)**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --preset ninja"` then
`cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build"`
Expected: compile error — `atx::tsdb::build_dated_segments` undeclared. (Red.)

- [ ] **Step 3: Declare the new functions in the header**

In `atx-tsdb/include/atx/tsdb/load_parquet.hpp`, add `#include <utility>` and after `load_parquet`:

```cpp
/// Read `parquet_path` whose field columns are int64 fixed-point; for each
/// (name, scale) in `field_scales`, project that column as f64 (value * scale) and
/// build a sealed segment at `out_path`. `time_col` is a timestamp column,
/// `symbol_col` a string column.
[[nodiscard]] atx::core::Status
load_parquet_scaled(const std::string &parquet_path, const std::string &out_path,
                    const std::string &time_col, const std::string &symbol_col,
                    const std::vector<std::pair<std::string, atx::f64>> &field_scales,
                    atx::i64 created_at_nanos);

/// For each `<hive_root>/date=YYYY-MM-DD/data.parquet`, build a sealed segment at
/// `<seg_dir>/YYYY-MM-DD.seg` via load_parquet_scaled. Creates `seg_dir`. Dates are
/// processed in ascending order. Err if `hive_root` has no `date=` partitions.
[[nodiscard]] atx::core::Status
build_dated_segments(const std::string &hive_root, const std::string &seg_dir,
                     const std::string &time_col, const std::string &symbol_col,
                     const std::vector<std::pair<std::string, atx::f64>> &field_scales,
                     atx::i64 created_at_nanos);
```

- [ ] **Step 4: Implement both in the .cpp**

In `atx-tsdb/src/load_parquet.cpp`, add `#include <algorithm>` (already present), `#include <filesystem>`,
`#include <utility>`, then append:

```cpp
Status load_parquet_scaled(const std::string &parquet_path, const std::string &out_path,
                           const std::string &time_col, const std::string &symbol_col,
                           const std::vector<std::pair<std::string, atx::f64>> &field_scales,
                           atx::i64 created_at_nanos) {
  ATX_TRY(auto table, atx::core::io::read_parquet(parquet_path));

  LongColumns cols;
  cols.field_names.reserve(field_scales.size());

  ATX_TRY(auto ts_col, table.to_column<atx::core::time::Timestamp>(time_col));
  const auto ts = ts_col.view();
  cols.times.reserve(ts.size());
  for (const auto &t : ts) {
    cols.times.push_back(t.unix_nanos());
  }

  ATX_TRY(auto syms, table.strings(symbol_col));
  cols.symbols.reserve(syms.size());
  for (const std::string_view s : syms) {
    cols.symbols.emplace_back(s);
  }

  cols.values.resize(field_scales.size());
  for (atx::usize f = 0; f < field_scales.size(); ++f) {
    cols.field_names.push_back(field_scales[f].first);
    ATX_TRY(auto col, table.column_view<atx::i64>(field_scales[f].first));
    const atx::f64 scale = field_scales[f].second;
    auto &dst = cols.values[f];
    dst.reserve(col.size());
    for (const atx::i64 v : col) {
      dst.push_back(static_cast<atx::f64>(v) * scale);
    }
  }

  return build_from_long(cols, out_path, created_at_nanos);
}

Status build_dated_segments(const std::string &hive_root, const std::string &seg_dir,
                            const std::string &time_col, const std::string &symbol_col,
                            const std::vector<std::pair<std::string, atx::f64>> &field_scales,
                            atx::i64 created_at_nanos) {
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::exists(fs::path{hive_root}, ec)) {
    return Err(ErrorCode::IoError, "hive_root does not exist");
  }
  // Collect date partitions: subdirs named "date=YYYY-MM-DD".
  std::vector<std::string> dates;
  for (const auto &entry : fs::directory_iterator(hive_root, ec)) {
    if (!entry.is_directory(ec)) {
      continue;
    }
    const std::string dir = entry.path().filename().string();
    const std::string prefix = "date=";
    if (dir.rfind(prefix, 0) == 0) {
      dates.push_back(dir.substr(prefix.size()));
    }
  }
  if (dates.empty()) {
    return Err(ErrorCode::InvalidArgument, "no date= partitions under hive_root");
  }
  std::sort(dates.begin(), dates.end()); // ISO dates sort chronologically

  fs::create_directories(fs::path{seg_dir}, ec);
  for (const std::string &date : dates) {
    const std::string parquet =
        (fs::path{hive_root} / ("date=" + date) / "data.parquet").string();
    const std::string out = (fs::path{seg_dir} / (date + ".seg")).string();
    ATX_TRY_VOID(load_parquet_scaled(parquet, out, time_col, symbol_col, field_scales,
                                     created_at_nanos));
  }
  return Ok();
}
```

(Ensure `#include "atx/core/error.hpp"` provides `ATX_TRY`/`ATX_TRY_VOID`/`Ok` — it is already
included; `Ok`/`Err` are already `using`-aliased at the top of this file.)

- [ ] **Step 5: Run; verify PASS**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build"` then
`ctest --test-dir build -R "BuildDatedSegments|LoadParquet" --output-on-failure`
Expected: PASS (new `BuildDatedSegments.ScalesI64FieldsAndPartitions` + existing load_parquet tests).

- [ ] **Step 6: Format + commit**

```bash
clang-format -i atx-tsdb/include/atx/tsdb/load_parquet.hpp atx-tsdb/src/load_parquet.cpp atx-tsdb/tests/load_parquet_test.cpp
git add atx-tsdb/include/atx/tsdb/load_parquet.hpp atx-tsdb/src/load_parquet.cpp atx-tsdb/tests/load_parquet_test.cpp
git commit -m "feat(tsdb): int64-scaled per-date segment bridge (load_parquet_scaled, build_dated_segments)"
```

---

## Task 4: U3 — MultiSegmentBarFeed (atx-engine)

Compose the existing `ShmBarFeed` over an ordered list of per-date segment files: drive the current
segment to EOF, then lazily attach + feed the next. Symbol columns intern into one shared
`SymbolTable` (consistent ids across days). `SimClock::advance_to` enforces monotonic time, so an
out-of-order path fails closed. `ShmBarFeed` is `final` and non-movable, so we compose (own a
`std::optional<SegmentReader>` + `std::optional<ShmBarFeed>`) rather than subclass.

**Files:**
- Create: `atx-engine/include/atx/engine/data/multi_segment_bar_feed.hpp`
- Test: `atx-engine/tests/multi_segment_bar_feed_test.cpp`

- [ ] **Step 1: Write the failing test**

Create `atx-engine/tests/multi_segment_bar_feed_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <vector>

#include "atx/core/domain/symbol.hpp"
#include "atx/core/types.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/market.hpp"
#include "atx/engine/data/multi_segment_bar_feed.hpp"
#include "atx/engine/event/event.hpp"

#include "atx/tsdb/builder.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

std::string temp_path(const std::string &name) {
#if defined(_WIN32)
  wchar_t tmp_dir[MAX_PATH + 1]{};
  GetTempPathW(MAX_PATH + 1, tmp_dir);
  wchar_t tmp_file[MAX_PATH + 1]{};
  GetTempFileNameW(tmp_dir, L"atx", 0, tmp_file);
  std::wstring wpath(tmp_file);
  const std::wstring wsuffix(name.begin(), name.end());
  wpath += wsuffix + L".atxseg";
  return std::string(wpath.begin(), wpath.end());
#else
  char buf[L_tmpnam]{};
  std::tmpnam(buf);
  return std::string(buf) + name + ".atxseg";
#endif
}

// One-row OHLCV segment: `sym` present at time `t`, close = `close`.
std::string make_seg(const std::string &name, const std::string &sym, atx::i64 t, double close) {
  atx::tsdb::SegmentBuilder b({"open", "high", "low", "close", "volume"}, {sym}, {t});
  b.set(0, 0, 0, close);
  b.set(1, 0, 0, close);
  b.set(2, 0, 0, close);
  b.set(3, 0, 0, close);
  b.set(4, 0, 0, 1000.0);
  const std::string path = temp_path(name);
  EXPECT_TRUE(b.write(path, 0).has_value());
  return path;
}

} // namespace

TEST(MultiSegmentBarFeed, StreamsSegmentsInDateOrder) {
  const std::string s1 = make_seg("ms1", "AAA", 100, 11.0);
  const std::string s2 = make_seg("ms2", "BBB", 200, 22.0);

  atx::core::domain::SymbolTable symbols;
  atx::engine::SimClock clock;
  atx::engine::EventBus<> bus;
  (void)bus.add_consumer(0);

  atx::engine::data::MultiSegmentBarFeed feed({s1, s2}, symbols, clock, bus);

  // Day 1: AAA @ 100.
  ASSERT_TRUE(feed.step());
  EXPECT_EQ(clock.now().unix_nanos(), 100);
  int seen1 = 0;
  double close1 = 0.0;
  bus.drain_in_order([&](atx::usize, const atx::engine::event::Event &e) {
    ++seen1;
    close1 = e.payload_as<atx::engine::data::MarketPayload>().as_bar().close.to_decimal().to_double();
  });
  EXPECT_EQ(seen1, 1);
  EXPECT_DOUBLE_EQ(close1, 11.0);

  // Day 2: BBB @ 200 (next segment opened lazily, clock advanced forward).
  ASSERT_TRUE(feed.step());
  EXPECT_EQ(clock.now().unix_nanos(), 200);
  int seen2 = 0;
  double close2 = 0.0;
  bus.drain_in_order([&](atx::usize, const atx::engine::event::Event &e) {
    ++seen2;
    close2 = e.payload_as<atx::engine::data::MarketPayload>().as_bar().close.to_decimal().to_double();
  });
  EXPECT_EQ(seen2, 1);
  EXPECT_DOUBLE_EQ(close2, 22.0);

  // EOF after the last segment.
  EXPECT_FALSE(feed.step());

  static_cast<void>(std::remove(s1.c_str()));
  static_cast<void>(std::remove(s2.c_str()));
}

TEST(MultiSegmentBarFeed, SameSymbolAcrossDaysInternsOnce) {
  const std::string s1 = make_seg("msa", "AAA", 100, 10.0);
  const std::string s2 = make_seg("msb", "AAA", 200, 20.0);

  atx::core::domain::SymbolTable symbols;
  atx::engine::SimClock clock;
  atx::engine::EventBus<> bus;
  (void)bus.add_consumer(0);

  const atx::core::domain::Symbol pre = symbols.intern("AAA");
  atx::engine::data::MultiSegmentBarFeed feed({s1, s2}, symbols, clock, bus);

  int total = 0;
  while (feed.step()) {
    bus.drain_in_order([&](atx::usize, const atx::engine::event::Event &) { ++total; });
  }
  EXPECT_EQ(total, 2);                      // AAA on both days
  EXPECT_EQ(symbols.intern("AAA").id, pre.id); // interning stayed stable

  static_cast<void>(std::remove(s1.c_str()));
  static_cast<void>(std::remove(s2.c_str()));
}
```

- [ ] **Step 2: Run it; verify it FAILS (header does not exist)**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --preset ninja"` then
`cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build"`
Expected: compile error — `multi_segment_bar_feed.hpp` not found / `MultiSegmentBarFeed` undefined. (Red.)
(If the new test is not auto-discovered, add `multi_segment_bar_feed_test.cpp` to
`atx-engine/tests/CMakeLists.txt` the same way `shm_bar_feed_test.cpp` is listed/globbed.)

- [ ] **Step 3: Implement the header**

Create `atx-engine/include/atx/engine/data/multi_segment_bar_feed.hpp`:

```cpp
#pragma once

// atx::engine::data::MultiSegmentBarFeed — an IDataHandler that streams a sequence
// of per-date sealed segments as one continuous chronological feed. It composes
// ShmBarFeed: the current segment is driven to EOF, then the next is attached and
// fed. Symbols intern into one shared SymbolTable, so instrument identity is stable
// across day boundaries. Segments MUST be supplied in ascending date order; the
// SimClock (advanced by ShmBarFeed) fails closed on a backward step.
//
// PRECONDITION: every path is a valid sealed OHLCV segment (ABORTS otherwise — a
// bad path is a wiring error). NON-OWNING: symbols/clock/bus must outlive the feed.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "atx/core/domain/symbol.hpp"
#include "atx/core/macro.hpp"

#include "atx/engine/bus/event_bus.hpp"
#include "atx/engine/clock/sim_clock.hpp"
#include "atx/engine/data/data_handler.hpp"
#include "atx/engine/data/shm_bar_feed.hpp"

#include "atx/tsdb/segment_reader.hpp"

namespace atx::engine::data {

class MultiSegmentBarFeed final : public IDataHandler {
public:
  MultiSegmentBarFeed(std::vector<std::string> seg_paths,
                      atx::core::domain::SymbolTable &symbols, SimClock &clock, EventBus<> &bus)
      : paths_{std::move(seg_paths)}, symbols_{&symbols}, clock_{&clock}, bus_{&bus} {}

  [[nodiscard]] bool step() override {
    for (;;) {
      if (feed_.has_value()) {
        if (feed_->step()) {
          return true;
        }
        feed_.reset();   // current segment drained; tear down before re-attach
        reader_.reset();
      }
      if (next_ >= paths_.size()) {
        return false; // all segments consumed
      }
      auto r = atx::tsdb::SegmentReader::attach(paths_[next_]);
      ++next_;
      ATX_ASSERT(r.has_value()); // a bad segment path is a wiring error
      reader_.emplace(std::move(r.value()));
      feed_.emplace(*reader_, *symbols_, *clock_, *bus_);
    }
  }

private:
  std::vector<std::string> paths_;
  atx::core::domain::SymbolTable *symbols_; // non-owning
  SimClock *clock_;                         // non-owning
  EventBus<> *bus_;                         // non-owning
  std::optional<atx::tsdb::SegmentReader> reader_; // owns the current segment
  std::optional<ShmBarFeed> feed_;                 // points into reader_
  atx::usize next_{0};                             // next path to attach
};

} // namespace atx::engine::data
```

- [ ] **Step 4: Run; verify PASS**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build"` then
`ctest --test-dir build -R "MultiSegmentBarFeed" --output-on-failure`
Expected: PASS (both `StreamsSegmentsInDateOrder` and `SameSymbolAcrossDaysInternsOnce`).

- [ ] **Step 5: Format + commit**

```bash
clang-format -i atx-engine/include/atx/engine/data/multi_segment_bar_feed.hpp atx-engine/tests/multi_segment_bar_feed_test.cpp
git add atx-engine/include/atx/engine/data/multi_segment_bar_feed.hpp atx-engine/tests/multi_segment_bar_feed_test.cpp
# include atx-engine/tests/CMakeLists.txt if you had to add the test there
git commit -m "feat(engine): MultiSegmentBarFeed — date-ordered per-segment bar feed"
```

---

## Task 5: Full-suite gate

**Files:** none (verification only).

- [ ] **Step 1: Reconfigure + build everything clean under /W4 /WX**

Run: `cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --preset ninja"` then
`cmd /c "C:\Users\natha\atx-wt\dbenv.cmd cmake --build build"`
Expected: zero warnings, zero errors across atx-core, atx-tsdb, atx-engine.

- [ ] **Step 2: Run the full test suites**

Run: `ctest --test-dir build --output-on-failure`
Expected: all pass (Phase-1 + new `Databento.PerFile…`, `ParquetWriter.Zstd…`,
`BuildDatedSegments.*`, `MultiSegmentBarFeed.*`). The env-gated `Databento.RealEqusSummarySmoke`
stays skipped (no `ATX_EQUS_ZIP`).

- [ ] **Step 3 (optional): re-validate the real archive end-to-end through the bridge**

```powershell
$env:ATX_EQUS_ZIP = "C:\Users\natha\Downloads\EQUS-20260606-CEXECEMBY6.zip"
build\bin\atx-core-tests.exe --gtest_filter='Databento.RealEqusSummarySmoke'
```
Then (manually, once) point `build_dated_segments` at the produced hive root and attach a couple of
`.seg` files to confirm scaled prices look right (e.g. AAPL close ≈ a few hundred dollars, not 1e11).

- [ ] **Step 4: Verify formatting is idempotent**

Run `clang-format -i` over all files changed in Tasks 1–4, then `git diff --exit-code`.
Expected: no diff.

---

## Self-review notes (coverage vs spec)

- U1 streaming write + date-once → Task 1. Compression → Task 2. ✓
- U2 single i64→f64 (÷1e9) site + per-date segments → Task 3. ✓
- U3 multi-segment date-ordered feed, shared interning, lazy open → Task 4. ✓
- Adjustment → intentionally NOT a task (deferred appendix in the spec). ✓
- Types consistent across tasks: `WriteOptions`/`Compression` (Task 2) used by Task 1's writer calls
  via defaults; `load_parquet_scaled`/`build_dated_segments` signatures (Task 3) match the Task 3
  test; `MultiSegmentBarFeed(vector<string>, SymbolTable&, SimClock&, EventBus<>&)` (Task 4) matches
  its test. ✓
