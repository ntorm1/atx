# Parquet Lazy-Loader Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an Arrow-backed, Polars-style lazy Parquet loader to atx-core as `atx::core::io` — scan plan with projection/predicate/row-group-stat pushdown, row-group streaming, slice/limit, materializing to a wrapped Arrow Table with zero-copy bridges to `series::Column<T>`/`Frame`.

**Architecture:** A PIMPL public header (`parquet.hpp`, zero Arrow includes) over an Arrow-dependent `parquet.cpp`. `LazyParquet` builds a deferred plan executed by `parquet::arrow::FileReader`; row groups are pruned via Parquet column statistics, survivors read (projected), exact-filtered with `arrow::compute`, then sliced. An exception firewall maps `arrow::Status`/exceptions to atx `Result`.

**Tech Stack:** C++20, CMake + Ninja + clang-cl, vcpkg (`arrow[parquet]`), Apache Arrow/Parquet C++, GoogleTest. Profile: `/W4 /permissive- /WX`, no UB, `Result<T>` errors (no exceptions across the boundary).

---

## Prerequisites (one-time, may already be running)

`VCPKG_ROOT` must point at a bootstrapped vcpkg with `arrow[parquet]:x64-windows` installed. The session already started:
```
git clone --depth 1 https://github.com/microsoft/vcpkg C:\Users\natha\vcpkg
C:\Users\natha\vcpkg\bootstrap-vcpkg.bat -disableMetrics
C:\Users\natha\vcpkg\vcpkg.exe install "arrow[parquet]:x64-windows" --clean-after-build
```
Set `VCPKG_ROOT=C:\Users\natha\vcpkg` in the build shell. Configure with the `ninja` preset (now wired to the vcpkg toolchain — Task 1).

**Build/test incantation** (inline VS dev-shell, the `-ExecutionPolicy Bypass` script is BLOCKED):
```powershell
Import-Module "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath "C:\Program Files\Microsoft Visual Studio\2022\Community" -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64"
$env:VCPKG_ROOT = "C:\Users\natha\vcpkg"
cmake --preset ninja
cmake --build --preset ninja
ctest --preset ninja --output-on-failure
```

## File Structure

| File | Responsibility |
|------|----------------|
| `vcpkg.json` (root) | Manifest: `arrow[parquet]` + builtin baseline. |
| `CMakePresets.json` | `ninja` preset gains `CMAKE_TOOLCHAIN_FILE` → vcpkg. |
| `atx-core/CMakeLists.txt` | `find_package(Arrow/Parquet)`, add `src/io/parquet.cpp`, link Arrow targets, copy runtime DLLs. |
| `atx-core/include/atx/core/io/parquet.hpp` | Arrow-free public API: enums, `Schema`, `Scalar`, `Predicate`, `ScanStats`, `ParquetTable`/`LazyParquet`/`RowGroupStream` (PIMPL), `read_parquet`. |
| `atx-core/src/io/parquet.cpp` | Arrow impl: open/metadata, dtype map, prune/read/filter/slice, bridges, exception firewall. |
| `atx-core/tests/parquet_test.cpp` | TDD suite + Arrow-writer fixture helpers. |
| `atx-core/tests/CMakeLists.txt` | Add `parquet_test.cpp`, link Arrow/Parquet. |
| `atx-core/include/atx/core/core.hpp` | Add L10 io group. |
| `atx-core/README.md` | Document io layer + vcpkg/Arrow build requirement. |

Implementation note for every `.cpp` public function: wrap the body in the firewall
```cpp
try { /* … */ }
catch (const std::bad_alloc&) { throw; }
catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
catch (...) { return Err(ErrorCode::Internal, "io::parquet: unknown exception"); }
```
and convert any non-ok `arrow::Status`/`arrow::Result` with the helper `from_arrow` (Task 1).

---

## Task 1: Build integration + walking skeleton

**Files:**
- Create: `vcpkg.json`
- Modify: `CMakePresets.json`
- Modify: `atx-core/CMakeLists.txt`
- Modify: `atx-core/tests/CMakeLists.txt`
- Create: `atx-core/include/atx/core/io/parquet.hpp`
- Create: `atx-core/src/io/parquet.cpp`
- Test: `atx-core/tests/parquet_test.cpp`

- [ ] **Step 1: Create `vcpkg.json`**

```json
{
  "name": "atx",
  "version-string": "0.1.0",
  "dependencies": [
    { "name": "arrow", "features": ["parquet"] }
  ],
  "builtin-baseline": "REPLACE_WITH_VCPKG_HEAD_COMMIT"
}
```
Fill `builtin-baseline` from `git -C %VCPKG_ROOT% rev-parse HEAD`.

- [ ] **Step 2: Wire the vcpkg toolchain into the `ninja` preset**

In `CMakePresets.json`, add to the `ninja` preset's `cacheVariables`:
```json
"CMAKE_TOOLCHAIN_FILE": "$env{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake",
"VCPKG_TARGET_TRIPLET": "x64-windows"
```

- [ ] **Step 3: Wire Arrow into `atx-core/CMakeLists.txt`**

Add the source to the `add_library(atx-core STATIC …)` list:
```cmake
    src/io/parquet.cpp
```
After `target_compile_features(... cxx_std_20)` add:
```cmake
find_package(Arrow CONFIG REQUIRED)
find_package(Parquet CONFIG REQUIRED)
```
Add to `target_link_libraries(atx-core PUBLIC …)`:
```cmake
        Arrow::arrow_shared
        Parquet::parquet_shared
```
(Arrow's imported targets carry SYSTEM include dirs, so `/WX` will not fire inside Arrow headers.)

- [ ] **Step 4: Copy Arrow runtime DLLs next to test/bench exes**

At the end of `atx-core/tests/CMakeLists.txt` (after the target exists):
```cmake
# Arrow/Parquet are shared libs under vcpkg; copy their DLLs beside the test exe.
add_custom_command(TARGET atx-core-tests POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:atx-core-tests> $<TARGET_FILE_DIR:atx-core-tests>
    COMMAND_EXPAND_LISTS)
```
Also add `parquet_test.cpp` to the `add_executable(atx-core-tests …)` source list (linalg/io region) and link Arrow to the test target:
```cmake
target_link_libraries(atx-core-tests PRIVATE Arrow::arrow_shared Parquet::parquet_shared)
```

- [ ] **Step 5: Write the public header `parquet.hpp` (full surface, PIMPL)**

```cpp
#pragma once

// atx::core::io — Arrow-backed, Polars-style lazy Parquet loader.
// This header includes NO Arrow headers; all Arrow types live behind PIMPL in
// parquet.cpp. Expected failures travel in Result<T>; the surface never throws
// (only std::bad_alloc may propagate).

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "atx/core/datetime.hpp"        // time::Timestamp
#include "atx/core/error.hpp"           // Result, Ok, Err, ErrorCode
#include "atx/core/series/column.hpp"   // series::Column<T>
#include "atx/core/series/frame.hpp"    // series::Frame
#include "atx/core/types.hpp"           // i64, usize

namespace atx::core::io {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;
using atx::core::Result;

enum class DType {
  Bool, Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64,
  Float32, Float64, String, Binary, Date32, Timestamp, Decimal128, Unsupported
};

struct ColumnInfo {
  std::string name;
  DType dtype{DType::Unsupported};
  bool nullable{true};
};

struct Schema {
  std::vector<ColumnInfo> columns;
  [[nodiscard]] const ColumnInfo* find(std::string_view name) const noexcept;
  [[nodiscard]] usize size() const noexcept { return columns.size(); }
};

// Arrow-free predicate literal.
class Scalar {
public:
  using Storage = std::variant<std::monostate, bool, i64, f64, std::string,
                               time::Timestamp>;
  Scalar() = default;
  explicit Scalar(bool v) : v_{v} {}
  explicit Scalar(i64 v) : v_{v} {}
  explicit Scalar(f64 v) : v_{v} {}
  explicit Scalar(std::string v) : v_{std::move(v)} {}
  explicit Scalar(time::Timestamp v) : v_{v} {}
  [[nodiscard]] const Storage& value() const noexcept { return v_; }
private:
  Storage v_{};
};

enum class Compare { Eq, Ne, Lt, Le, Gt, Ge, IsNull, IsNotNull };

struct Predicate {
  std::string column;
  Compare op{Compare::Eq};
  Scalar value{};
};

struct ScanStats {
  i64 row_groups_total{0};
  i64 row_groups_pruned{0};
  i64 rows_scanned{0};
};

class ParquetTable {
public:
  ParquetTable(ParquetTable&&) noexcept;
  ParquetTable& operator=(ParquetTable&&) noexcept;
  ParquetTable(const ParquetTable&) = delete;
  ParquetTable& operator=(const ParquetTable&) = delete;
  ~ParquetTable();

  [[nodiscard]] i64 num_rows() const noexcept;
  [[nodiscard]] i64 num_columns() const noexcept;
  [[nodiscard]] const Schema& schema() const noexcept;

  template <class T>
  [[nodiscard]] Result<std::span<const T>> column_view(std::string_view name) const;
  template <class T>
  [[nodiscard]] Result<series::Column<T>> to_column(std::string_view name) const;
  [[nodiscard]] Result<series::Frame> to_frame() const;
  [[nodiscard]] Result<std::vector<std::string_view>> strings(std::string_view name) const;

  struct Impl;
  explicit ParquetTable(std::unique_ptr<Impl> impl) noexcept;
private:
  std::unique_ptr<Impl> impl_;
};

class RowGroupStream;

class LazyParquet {
public:
  [[nodiscard]] static Result<LazyParquet> scan(std::string_view path);

  LazyParquet(LazyParquet&&) noexcept;
  LazyParquet& operator=(LazyParquet&&) noexcept;
  LazyParquet(const LazyParquet&) = delete;
  LazyParquet& operator=(const LazyParquet&) = delete;
  ~LazyParquet();

  [[nodiscard]] const Schema& schema() const noexcept;
  [[nodiscard]] i64 num_rows() const noexcept;
  [[nodiscard]] i64 num_row_groups() const noexcept;

  LazyParquet& select(std::vector<std::string> columns);
  LazyParquet& filter(Predicate p);
  LazyParquet& limit(i64 n);
  LazyParquet& offset(i64 n);

  [[nodiscard]] Result<ParquetTable> collect();
  [[nodiscard]] Result<RowGroupStream> stream();
  [[nodiscard]] ScanStats stats() const noexcept;

  struct Impl;
  explicit LazyParquet(std::unique_ptr<Impl> impl) noexcept;
private:
  std::unique_ptr<Impl> impl_;
};

class RowGroupStream {
public:
  RowGroupStream(RowGroupStream&&) noexcept;
  RowGroupStream& operator=(RowGroupStream&&) noexcept;
  RowGroupStream(const RowGroupStream&) = delete;
  RowGroupStream& operator=(const RowGroupStream&) = delete;
  ~RowGroupStream();
  [[nodiscard]] Result<std::optional<ParquetTable>> next();

  struct Impl;
  explicit RowGroupStream(std::unique_ptr<Impl> impl) noexcept;
private:
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] Result<ParquetTable> read_parquet(std::string_view path);

} // namespace atx::core::io
```

- [ ] **Step 6: Write the failing smoke test**

Create `atx-core/tests/parquet_test.cpp`:
```cpp
#include <atx/core/io/parquet.hpp>

#include <gtest/gtest.h>

#include <atx/core/error.hpp>

using namespace atx::core::io;

TEST(Parquet, MissingFileReturnsNotFound) {
  auto r = read_parquet("this_file_does_not_exist_12345.parquet");
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::NotFound);
}
```

- [ ] **Step 7: Run it — verify it fails to link/compile (skeleton missing)**

Run the build incantation above. Expected: link error — `read_parquet` and the PIMPL methods are undefined (RED).

- [ ] **Step 8: Write the minimal `parquet.cpp` skeleton**

```cpp
#include "atx/core/io/parquet.hpp"

#include <arrow/io/file.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

#include <filesystem>

namespace atx::core::io {

// Map an arrow::Status to an atx Error.
[[nodiscard]] static Error from_arrow(const arrow::Status& s, std::string_view ctx) {
  std::string msg{ctx};
  msg += ": ";
  msg += s.ToString();
  if (s.IsIOError())       return Error{ErrorCode::IoError, std::move(msg)};
  if (s.IsInvalid())       return Error{ErrorCode::ParseError, std::move(msg)};
  if (s.IsNotImplemented())return Error{ErrorCode::NotImplemented, std::move(msg)};
  return Error{ErrorCode::Internal, std::move(msg)};
}

const ColumnInfo* Schema::find(std::string_view name) const noexcept {
  for (const auto& c : columns) {
    if (c.name == name) return &c;
  }
  return nullptr;
}

// ParquetTable Impl holds the materialized Arrow table + cached atx Schema.
struct ParquetTable::Impl {
  std::shared_ptr<arrow::Table> table;
  Schema schema;
};

ParquetTable::ParquetTable(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
ParquetTable::ParquetTable(ParquetTable&&) noexcept = default;
ParquetTable& ParquetTable::operator=(ParquetTable&&) noexcept = default;
ParquetTable::~ParquetTable() = default;

i64 ParquetTable::num_rows() const noexcept { return impl_->table->num_rows(); }
i64 ParquetTable::num_columns() const noexcept { return impl_->table->num_columns(); }
const Schema& ParquetTable::schema() const noexcept { return impl_->schema; }

Result<ParquetTable> read_parquet(std::string_view path) {
  try {
    if (!std::filesystem::exists(path)) {
      return Err(ErrorCode::NotFound, std::string{"read_parquet: no such file: "} + std::string{path});
    }
    auto in = arrow::io::ReadableFile::Open(std::string{path});
    if (!in.ok()) return Err(from_arrow(in.status(), "read_parquet open"));

    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto st = parquet::arrow::OpenFile(*in, arrow::default_memory_pool(), &reader);
    if (!st.ok()) return Err(from_arrow(st, "read_parquet open parquet"));

    std::shared_ptr<arrow::Table> table;
    st = reader->ReadTable(&table);
    if (!st.ok()) return Err(from_arrow(st, "read_parquet read table"));

    auto impl = std::make_unique<ParquetTable::Impl>();
    impl->table = std::move(table);
    // schema filled in Task 2.
    return ParquetTable{std::move(impl)};
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "read_parquet: unknown exception"); }
}

} // namespace atx::core::io
```
(Leave `LazyParquet`/`RowGroupStream`/bridges undefined for now — Tasks 2+ add them. The smoke test only needs `read_parquet`.)

- [ ] **Step 9: Run the smoke test — verify GREEN**

Run: `ctest --preset ninja -R Parquet.MissingFileReturnsNotFound --output-on-failure`
Expected: PASS. Confirms Arrow links, DLLs resolve, firewall works.

- [ ] **Step 10: Commit**

```bash
git add vcpkg.json CMakePresets.json atx-core/CMakeLists.txt atx-core/tests/CMakeLists.txt \
        atx-core/include/atx/core/io/parquet.hpp atx-core/src/io/parquet.cpp atx-core/tests/parquet_test.cpp
git commit -m "feat(io): parquet loader skeleton + vcpkg/Arrow build wiring"
```

---

## Task 2: Schema introspection + lazy scan

**Files:**
- Modify: `atx-core/src/io/parquet.cpp`
- Test: `atx-core/tests/parquet_test.cpp`

- [ ] **Step 1: Add the Arrow-writer test fixture helper**

At the top of `parquet_test.cpp` (after includes), add a helper that writes an Arrow table to a temp `.parquet` and returns the path. Include Arrow headers in the TEST TU (allowed):
```cpp
#include <arrow/api.h>
#include <parquet/arrow/writer.h>
#include <arrow/io/file.h>
#include <filesystem>

namespace {
std::string temp_path(const char* stem) {
  auto dir = std::filesystem::temp_directory_path();
  return (dir / (std::string{"atx_pq_"} + stem + ".parquet")).string();
}

// Writes `table` to `path` with the given compression. Aborts the test on error.
void write_table(const std::shared_ptr<arrow::Table>& table, const std::string& path,
                 arrow::Compression::type codec = arrow::Compression::SNAPPY,
                 int64_t row_group_size = 1024) {
  auto out = arrow::io::FileOutputStream::Open(path).ValueOrDie();
  auto props = parquet::WriterProperties::Builder().compression(codec)->build();
  auto arrow_props = parquet::ArrowWriterProperties::Builder().build();
  ASSERT_TRUE(parquet::arrow::WriteTable(*table, arrow::default_memory_pool(), out,
                                         row_group_size, props, arrow_props).ok());
}

// Builds a 2-column int64/double table with `n` rows: i, i*1.5.
std::shared_ptr<arrow::Table> make_numeric_table(int64_t n) {
  arrow::Int64Builder a; arrow::DoubleBuilder b;
  for (int64_t i = 0; i < n; ++i) { (void)a.Append(i); (void)b.Append(static_cast<double>(i) * 1.5); }
  std::shared_ptr<arrow::Array> aa, ba; (void)a.Finish(&aa); (void)b.Finish(&ba);
  auto schema = arrow::schema({arrow::field("id", arrow::int64()),
                               arrow::field("val", arrow::float64())});
  return arrow::Table::Make(schema, {aa, ba});
}
} // namespace
```

- [ ] **Step 2: Write failing schema test**

```cpp
TEST(Parquet, ScanReportsSchemaAndMetadata) {
  auto path = temp_path("schema");
  write_table(make_numeric_table(2500), path);  // 1024 row-group size → 3 groups

  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->num_rows(), 2500);
  EXPECT_EQ(lz->num_row_groups(), 3);
  const Schema& s = lz->schema();
  ASSERT_EQ(s.size(), 2u);
  EXPECT_EQ(s.columns[0].name, "id");
  EXPECT_EQ(s.columns[0].dtype, DType::Int64);
  EXPECT_EQ(s.columns[1].name, "val");
  EXPECT_EQ(s.columns[1].dtype, DType::Float64);
}
```

- [ ] **Step 3: Run — verify it fails (LazyParquet::scan undefined)** (RED)

- [ ] **Step 4: Implement dtype mapping + LazyParquet scan/metadata**

Add to `parquet.cpp`:
```cpp
#include <arrow/type.h>
#include <parquet/metadata.h>

// arrow::DataType -> atx DType.
[[nodiscard]] static DType to_dtype(const arrow::DataType& t) {
  switch (t.id()) {
    case arrow::Type::BOOL:    return DType::Bool;
    case arrow::Type::INT8:    return DType::Int8;
    case arrow::Type::INT16:   return DType::Int16;
    case arrow::Type::INT32:   return DType::Int32;
    case arrow::Type::INT64:   return DType::Int64;
    case arrow::Type::UINT8:   return DType::UInt8;
    case arrow::Type::UINT16:  return DType::UInt16;
    case arrow::Type::UINT32:  return DType::UInt32;
    case arrow::Type::UINT64:  return DType::UInt64;
    case arrow::Type::FLOAT:   return DType::Float32;
    case arrow::Type::DOUBLE:  return DType::Float64;
    case arrow::Type::STRING:
    case arrow::Type::LARGE_STRING: return DType::String;
    case arrow::Type::BINARY:
    case arrow::Type::LARGE_BINARY: return DType::Binary;
    case arrow::Type::DATE32:  return DType::Date32;
    case arrow::Type::TIMESTAMP: return DType::Timestamp;
    case arrow::Type::DECIMAL128: return DType::Decimal128;
    case arrow::Type::DICTIONARY:
      return to_dtype(*static_cast<const arrow::DictionaryType&>(t).value_type());
    default: return DType::Unsupported;
  }
}

[[nodiscard]] static Schema schema_from_arrow(const arrow::Schema& s) {
  Schema out;
  out.columns.reserve(static_cast<usize>(s.num_fields()));
  for (int i = 0; i < s.num_fields(); ++i) {
    const auto& f = *s.field(i);
    out.columns.push_back(ColumnInfo{f->name(), to_dtype(*f->type()), f->nullable()});
  }
  return out;
}
```
> Note: `s.field(i)` returns `std::shared_ptr<arrow::Field>`; use `f->name()`, `*f->type()`, `f->nullable()` (rename the loop var so it reads cleanly).

Add the `LazyParquet::Impl` holding the open reader + plan, plus `scan`:
```cpp
struct LazyParquet::Impl {
  std::shared_ptr<arrow::io::ReadableFile> file;
  std::unique_ptr<parquet::arrow::FileReader> reader;
  std::shared_ptr<parquet::FileMetaData> meta;
  Schema schema;
  std::vector<std::string> projection;   // empty = all
  std::vector<Predicate> predicates;     // ANDed
  i64 limit{-1};
  i64 offset{0};
  ScanStats stats{};
};

LazyParquet::LazyParquet(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
LazyParquet::LazyParquet(LazyParquet&&) noexcept = default;
LazyParquet& LazyParquet::operator=(LazyParquet&&) noexcept = default;
LazyParquet::~LazyParquet() = default;

Result<LazyParquet> LazyParquet::scan(std::string_view path) {
  try {
    if (!std::filesystem::exists(path))
      return Err(ErrorCode::NotFound, std::string{"scan: no such file: "} + std::string{path});
    auto in = arrow::io::ReadableFile::Open(std::string{path});
    if (!in.ok()) return Err(from_arrow(in.status(), "scan open"));
    auto impl = std::make_unique<LazyParquet::Impl>();
    impl->file = *in;
    auto st = parquet::arrow::OpenFile(impl->file, arrow::default_memory_pool(), &impl->reader);
    if (!st.ok()) return Err(from_arrow(st, "scan open parquet"));
    impl->meta = impl->reader->parquet_reader()->metadata();
    std::shared_ptr<arrow::Schema> aschema;
    st = impl->reader->GetSchema(&aschema);
    if (!st.ok()) return Err(from_arrow(st, "scan schema"));
    impl->schema = schema_from_arrow(*aschema);
    return LazyParquet{std::move(impl)};
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "scan: unknown exception"); }
}

const Schema& LazyParquet::schema() const noexcept { return impl_->schema; }
i64 LazyParquet::num_rows() const noexcept { return impl_->meta->num_rows(); }
i64 LazyParquet::num_row_groups() const noexcept { return impl_->meta->num_row_groups(); }
ScanStats LazyParquet::stats() const noexcept { return impl_->stats; }

LazyParquet& LazyParquet::select(std::vector<std::string> c) { impl_->projection = std::move(c); return *this; }
LazyParquet& LazyParquet::filter(Predicate p) { impl_->predicates.push_back(std::move(p)); return *this; }
LazyParquet& LazyParquet::limit(i64 n) { impl_->limit = n; return *this; }
LazyParquet& LazyParquet::offset(i64 n) { impl_->offset = n; return *this; }
```
Also fill `read_parquet`'s `impl->schema = schema_from_arrow(*table->schema());` so `ParquetTable::schema()` is populated.

- [ ] **Step 5: Run — verify GREEN**

Run: `ctest --preset ninja -R Parquet.ScanReportsSchemaAndMetadata --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add atx-core/src/io/parquet.cpp atx-core/tests/parquet_test.cpp
git commit -m "feat(io): parquet schema introspection + lazy scan metadata"
```

---

## Task 3: Eager collect + numeric bridges (`collect`, `to_column`, `to_frame`, `column_view`)

**Files:**
- Modify: `atx-core/src/io/parquet.cpp`
- Test: `atx-core/tests/parquet_test.cpp`

- [ ] **Step 1: Write failing tests**

```cpp
TEST(Parquet, CollectReadsAllRows) {
  auto path = temp_path("collect");
  write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path);
  ASSERT_TRUE(lz.has_value());
  auto t = lz->collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 100);
  EXPECT_EQ(t->num_columns(), 2);
}

TEST(Parquet, ToColumnCopiesValues) {
  auto path = temp_path("tocol");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  auto col = t->to_column<double>("val");
  ASSERT_TRUE(col.has_value());
  ASSERT_EQ(col->size(), 10u);
  EXPECT_DOUBLE_EQ((*col)[3], 4.5);
}

TEST(Parquet, ColumnViewAliasesBuffer) {
  auto path = temp_path("view");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  auto v = t->column_view<int64_t>("id");
  ASSERT_TRUE(v.has_value());
  ASSERT_EQ(v->size(), 10u);
  EXPECT_EQ((*v)[7], 7);
}

TEST(Parquet, ToColumnWrongTypeIsInvalidArgument) {
  auto path = temp_path("wrongtype");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->to_column<double>("id").error().code(),  // id is int64
            atx::core::ErrorCode::InvalidArgument);
}

TEST(Parquet, ToFrameHasNumericColumns) {
  auto path = temp_path("toframe");
  write_table(make_numeric_table(10), path);
  auto t = read_parquet(path);
  ASSERT_TRUE(t.has_value());
  auto f = t->to_frame();
  ASSERT_TRUE(f.has_value());
  EXPECT_TRUE(f->has_column("id"));
  EXPECT_TRUE(f->has_column("val"));
}
```

- [ ] **Step 2: Run — verify it fails (collect/to_column/etc undefined)** (RED)

- [ ] **Step 3: Implement `collect` + numeric bridges**

`collect` (Task 3 form reads all groups; pruning/filter/slice added in Tasks 7-9):
```cpp
Result<ParquetTable> LazyParquet::collect() {
  try {
    std::shared_ptr<arrow::Table> table;
    auto st = impl_->reader->ReadTable(&table);
    if (!st.ok()) return Err(from_arrow(st, "collect read"));
    impl_->stats.row_groups_total = num_row_groups();
    impl_->stats.rows_scanned = table->num_rows();
    auto out = std::make_unique<ParquetTable::Impl>();
    out->table = std::move(table);
    out->schema = schema_from_arrow(*out->table->schema());
    return ParquetTable{std::move(out)};
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "collect: unknown exception"); }
}
```

Bridges. Add a template-trait mapping `T` → expected `arrow::Type::type`, then a
contiguous-buffer fetch. Combine chunks so the column is single-buffer:
```cpp
#include <arrow/array.h>
#include <arrow/compute/api.h>

template <class T> struct ArrowTypeOf;            // numeric mapping
template <> struct ArrowTypeOf<int64_t> { static constexpr auto id = arrow::Type::INT64; using ArrayT = arrow::Int64Array; };
template <> struct ArrowTypeOf<int32_t> { static constexpr auto id = arrow::Type::INT32; using ArrayT = arrow::Int32Array; };
template <> struct ArrowTypeOf<int16_t> { static constexpr auto id = arrow::Type::INT16; using ArrayT = arrow::Int16Array; };
template <> struct ArrowTypeOf<int8_t>  { static constexpr auto id = arrow::Type::INT8;  using ArrayT = arrow::Int8Array;  };
template <> struct ArrowTypeOf<uint64_t>{ static constexpr auto id = arrow::Type::UINT64;using ArrayT = arrow::UInt64Array;};
template <> struct ArrowTypeOf<uint32_t>{ static constexpr auto id = arrow::Type::UINT32;using ArrayT = arrow::UInt32Array;};
template <> struct ArrowTypeOf<uint16_t>{ static constexpr auto id = arrow::Type::UINT16;using ArrayT = arrow::UInt16Array;};
template <> struct ArrowTypeOf<uint8_t> { static constexpr auto id = arrow::Type::UINT8; using ArrayT = arrow::UInt8Array; };
template <> struct ArrowTypeOf<float>   { static constexpr auto id = arrow::Type::FLOAT; using ArrayT = arrow::FloatArray; };
template <> struct ArrowTypeOf<double>  { static constexpr auto id = arrow::Type::DOUBLE;using ArrayT = arrow::DoubleArray;};

// Returns the single contiguous chunk for `name` as ArrayT, or an error.
template <class T>
static Result<std::shared_ptr<typename ArrowTypeOf<T>::ArrayT>>
contiguous(const arrow::Table& table, std::string_view name) {
  const int idx = table.schema()->GetFieldIndex(std::string{name});
  if (idx < 0) return Err(ErrorCode::InvalidArgument, "column not found");
  auto col = table.column(idx);
  if (col->type()->id() != ArrowTypeOf<T>::id)
    return Err(ErrorCode::InvalidArgument, "column element-type mismatch");
  auto combined = col->chunk(0);
  if (col->num_chunks() != 1) {
    auto carr = arrow::Concatenate(col->chunks());
    if (!carr.ok()) return Err(from_arrow(carr.status(), "combine chunks"));
    combined = *carr;
  }
  return std::static_pointer_cast<typename ArrowTypeOf<T>::ArrayT>(combined);
}

template <class T>
Result<std::span<const T>> ParquetTable::column_view(std::string_view name) const {
  auto arr = contiguous<T>(*impl_->table, name);
  if (!arr.has_value()) return Err(arr.error());
  const T* p = (*arr)->raw_values();
  return Ok(std::span<const T>{p, static_cast<usize>((*arr)->length())});
}

template <class T>
Result<series::Column<T>> ParquetTable::to_column(std::string_view name) const {
  auto v = column_view<T>(name);
  if (!v.has_value()) return Err(v.error());
  series::Column<T> out;
  out.append_bulk(*v);
  return Ok(std::move(out));
}
```
Add explicit template instantiations at the bottom of the file for every supported
`T` (so the out-of-line templates link from the header):
```cpp
#define ATX_PQ_INSTANTIATE(T) \
  template Result<std::span<const T>> ParquetTable::column_view<T>(std::string_view) const; \
  template Result<series::Column<T>> ParquetTable::to_column<T>(std::string_view) const;
ATX_PQ_INSTANTIATE(int8_t)  ATX_PQ_INSTANTIATE(int16_t) ATX_PQ_INSTANTIATE(int32_t)
ATX_PQ_INSTANTIATE(int64_t) ATX_PQ_INSTANTIATE(uint8_t) ATX_PQ_INSTANTIATE(uint16_t)
ATX_PQ_INSTANTIATE(uint32_t)ATX_PQ_INSTANTIATE(uint64_t)ATX_PQ_INSTANTIATE(float)
ATX_PQ_INSTANTIATE(double)
#undef ATX_PQ_INSTANTIATE
```
> The `i64`/`int64_t` aliases must match `atx::core::types`. Confirm `i64` is `int64_t` (it is) so `column_view<int64_t>` matches the test.

`to_frame` (numeric + timestamp columns only):
```cpp
Result<series::Frame> ParquetTable::to_frame() const {
  try {
    series::Frame frame;
    for (const auto& ci : impl_->schema.columns) {
      switch (ci.dtype) {
        case DType::Int64: { auto c = to_column<int64_t>(ci.name);
          if (c) { auto a = frame.add_column<int64_t>(ci.name); if (a) a->get() = std::move(*c); } break; }
        case DType::Float64: { auto c = to_column<double>(ci.name);
          if (c) { auto a = frame.add_column<double>(ci.name); if (a) a->get() = std::move(*c); } break; }
        // … one arm per numeric DType (Int8..UInt64, Float32) following the same shape …
        default: break;  // String/Binary/Timestamp/Unsupported skipped
      }
    }
    return Ok(std::move(frame));
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "to_frame: unknown exception"); }
}
```
> Spell out every numeric arm in the real implementation (no `…`). Timestamp handled in Task 4.

- [ ] **Step 4: Run — verify GREEN**

Run: `ctest --preset ninja -R "Parquet.Collect|Parquet.ToColumn|Parquet.ColumnView|Parquet.ToFrame" --output-on-failure`
Expected: all PASS.

- [ ] **Step 5: Commit**

```bash
git add atx-core/src/io/parquet.cpp atx-core/tests/parquet_test.cpp
git commit -m "feat(io): parquet collect + numeric Column/Frame bridges"
```

---

## Task 4: Full dtype coverage (strings, timestamp, date32, decimal, nulls)

**Files:**
- Modify: `atx-core/src/io/parquet.cpp`
- Test: `atx-core/tests/parquet_test.cpp`

- [ ] **Step 1: Add fixture builders + failing tests**

Add a builder for a mixed table (string, bool, timestamp[ms], with a null) to the test
anonymous namespace:
```cpp
std::shared_ptr<arrow::Table> make_mixed_table() {
  arrow::StringBuilder s; (void)s.Append("a"); (void)s.AppendNull(); (void)s.Append("c");
  arrow::BooleanBuilder bo; (void)bo.Append(true); (void)bo.Append(false); (void)bo.Append(true);
  arrow::TimestampBuilder ts(arrow::timestamp(arrow::TimeUnit::MILLI), arrow::default_memory_pool());
  (void)ts.Append(1000); (void)ts.Append(2000); (void)ts.Append(3000);
  std::shared_ptr<arrow::Array> sa, ba, ta; (void)s.Finish(&sa); (void)bo.Finish(&ba); (void)ts.Finish(&ta);
  auto schema = arrow::schema({arrow::field("name", arrow::utf8()),
                               arrow::field("flag", arrow::boolean()),
                               arrow::field("t", arrow::timestamp(arrow::TimeUnit::MILLI))});
  return arrow::Table::Make(schema, {sa, ba, ta});
}
```
Tests:
```cpp
TEST(Parquet, StringsAccessor) {
  auto path = temp_path("strings"); write_table(make_mixed_table(), path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  auto s = t->strings("name"); ASSERT_TRUE(s.has_value());
  ASSERT_EQ(s->size(), 3u);
  EXPECT_EQ((*s)[0], "a");
  EXPECT_EQ((*s)[1], "");   // null → empty view (validity reported separately)
  EXPECT_EQ((*s)[2], "c");
}

TEST(Parquet, BoolBridge) {
  auto path = temp_path("bool"); write_table(make_mixed_table(), path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  auto c = t->to_column<bool>("flag"); ASSERT_TRUE(c.has_value());
  EXPECT_TRUE((*c)[0]); EXPECT_FALSE((*c)[1]);
}

TEST(Parquet, TimestampBridge) {
  auto path = temp_path("ts"); write_table(make_mixed_table(), path);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  auto c = t->to_column<atx::core::time::Timestamp>("t"); ASSERT_TRUE(c.has_value());
  EXPECT_EQ((*c)[0].unix_nanos(), 1'000'000'000);  // 1000 ms → ns
}

TEST(Parquet, SchemaReportsAllDtypes) {
  auto path = temp_path("dtypes"); write_table(make_mixed_table(), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->schema().find("name")->dtype, DType::String);
  EXPECT_EQ(lz->schema().find("flag")->dtype, DType::Bool);
  EXPECT_EQ(lz->schema().find("t")->dtype, DType::Timestamp);
}
```
> Confirm `time::Timestamp` exposes `unix_nanos()`; if the accessor differs, adjust the assertion to the actual getter.

- [ ] **Step 2: Run — verify it fails (strings/bool/timestamp bridges missing)** (RED)

- [ ] **Step 3: Implement string + bool + timestamp bridges**

```cpp
Result<std::vector<std::string_view>> ParquetTable::strings(std::string_view name) const {
  try {
    const int idx = impl_->table->schema()->GetFieldIndex(std::string{name});
    if (idx < 0) return Err(ErrorCode::InvalidArgument, "strings: column not found");
    auto col = impl_->table->column(idx);
    if (col->type()->id() != arrow::Type::STRING && col->type()->id() != arrow::Type::LARGE_STRING)
      return Err(ErrorCode::InvalidArgument, "strings: column is not UTF-8");
    auto chunk = col->chunk(0);
    if (col->num_chunks() != 1) {
      auto c = arrow::Concatenate(col->chunks());
      if (!c.ok()) return Err(from_arrow(c.status(), "strings combine"));
      chunk = *c;
    }
    auto& arr = static_cast<arrow::StringArray&>(*chunk);
    std::vector<std::string_view> out;
    out.reserve(static_cast<usize>(arr.length()));
    for (int64_t i = 0; i < arr.length(); ++i) {
      const auto v = arr.GetView(i);                // empty for nulls
      out.emplace_back(v.data(), v.size());
    }
    return Ok(std::move(out));
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "strings: unknown exception"); }
}
```
Add `bool` to `ArrowTypeOf` + instantiation list (Arrow `BooleanArray` is bit-packed —
`raw_values()` is unavailable, so handle bool specially in `to_column<bool>` and
`column_view<bool>`): provide a `bool` overload that reads `arr.Value(i)` into a fresh
`Column<bool>` and **rejects** `column_view<bool>` with `NotImplemented` (bit-packed,
not aliasable). Add a `time::Timestamp` specialization of `to_column` that reads the
`TimestampArray`, converts each value by its `TimeUnit` to ns via
`time::Timestamp::from_unix_{nanos,micros,millis,seconds}`, and appends. Spell these
out explicitly; add them to the instantiation macro.

- [ ] **Step 4: Run — verify GREEN** (`-R "Parquet.Strings|Parquet.Bool|Parquet.Timestamp|Parquet.SchemaReportsAllDtypes"`)

- [ ] **Step 5: Commit**

```bash
git commit -am "feat(io): parquet string/bool/timestamp bridges + dtype coverage"
```

---

## Task 5: Compression codec round-trips

**Files:** Modify `atx-core/tests/parquet_test.cpp` (impl already transparent via Arrow).

- [ ] **Step 1: Write failing/parameterized test**

```cpp
TEST(Parquet, AllCodecsRoundTrip) {
  struct C { arrow::Compression::type codec; const char* stem; };
  std::vector<C> codecs = {
    {arrow::Compression::UNCOMPRESSED, "raw"}, {arrow::Compression::SNAPPY, "snappy"},
    {arrow::Compression::GZIP, "gzip"}, {arrow::Compression::ZSTD, "zstd"},
    {arrow::Compression::LZ4, "lz4"},
  };
  if (arrow::util::Codec::IsAvailable(arrow::Compression::BROTLI))
    codecs.push_back({arrow::Compression::BROTLI, "brotli"});
  for (const auto& c : codecs) {
    auto path = temp_path(c.stem);
    write_table(make_numeric_table(500), path, c.codec);
    auto t = read_parquet(path);
    ASSERT_TRUE(t.has_value()) << c.stem;
    EXPECT_EQ(t->num_rows(), 500) << c.stem;
    auto col = t->to_column<double>("val");
    ASSERT_TRUE(col.has_value()) << c.stem;
    EXPECT_DOUBLE_EQ((*col)[499], 499 * 1.5) << c.stem;
  }
}
```
Add `#include <arrow/util/compression.h>` to the test.

- [ ] **Step 2: Run — verify it passes immediately** (Arrow decompresses transparently;
  this test guards against a misconfigured Arrow build missing codecs). If a codec is
  missing, the vcpkg Arrow lacks it — note and skip via `IsAvailable` as shown. Expected: PASS.

- [ ] **Step 3: Commit**

```bash
git commit -am "test(io): parquet round-trips across all compression codecs"
```

---

## Task 6: Projection pushdown (`select`)

**Files:** Modify `atx-core/src/io/parquet.cpp`, `atx-core/tests/parquet_test.cpp`.

- [ ] **Step 1: Failing test**

```cpp
TEST(Parquet, SelectProjectsColumns) {
  auto path = temp_path("select"); write_table(make_numeric_table(50), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->select({"val"}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_columns(), 1);
  ASSERT_EQ(t->schema().size(), 1u);
  EXPECT_EQ(t->schema().columns[0].name, "val");
}

TEST(Parquet, SelectUnknownColumnIsInvalidArgument) {
  auto path = temp_path("selbad"); write_table(make_numeric_table(5), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->select({"nope"}).collect().error().code(),
            atx::core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run — verify it fails (collect ignores projection)** (RED)

- [ ] **Step 3: Implement projection in `collect`**

Resolve projection (∪ predicate columns) to leaf column indices and pass to
`ReadTable`. Validate names against the schema first → `InvalidArgument`:
```cpp
// Helper: map column names to parquet leaf indices; error on unknown name.
static Result<std::vector<int>> resolve_indices(const arrow::Schema& s,
                                                const std::vector<std::string>& names) {
  std::vector<int> idx; idx.reserve(names.size());
  for (const auto& n : names) {
    const int i = s.GetFieldIndex(n);
    if (i < 0) return Err(ErrorCode::InvalidArgument, "select: unknown column '" + n + "'");
    idx.push_back(i);
  }
  return Ok(std::move(idx));
}
```
In `collect`, when `impl_->projection` (∪ predicate columns) is non-empty, build the
union of indices and call `impl_->reader->ReadTable(indices, &table)`. Keep the
predicate-only columns to drop after filtering (Task 7); for now (no predicates) the
projection set equals the user selection.

- [ ] **Step 4: Run — verify GREEN** (`-R Parquet.Select`)

- [ ] **Step 5: Commit** `git commit -am "feat(io): parquet projection pushdown (select)"`

---

## Task 7: Predicate filtering — exact row semantics (`filter`)

**Files:** Modify `atx-core/src/io/parquet.cpp`, `atx-core/tests/parquet_test.cpp`.

- [ ] **Step 1: Failing tests**

```cpp
TEST(Parquet, FilterGreaterThan) {
  auto path = temp_path("filt"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{90}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 10);                 // ids 90..99
  auto v = t->column_view<int64_t>("id"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], 90);
}

TEST(Parquet, FilterConjunction) {
  auto path = temp_path("filt2"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{10}}})
              .filter(Predicate{"id", Compare::Lt, Scalar{int64_t{20}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 10);                 // ids 10..19
}

TEST(Parquet, FilterTypeMismatchIsInvalidArgument) {
  auto path = temp_path("filt3"); write_table(make_numeric_table(10), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  // id is int64; comparing against a string literal is invalid.
  EXPECT_EQ(lz->filter(Predicate{"id", Compare::Eq, Scalar{std::string{"x"}}}).collect().error().code(),
            atx::core::ErrorCode::InvalidArgument);
}
```

- [ ] **Step 2: Run — verify it fails (filter ignored)** (RED)

- [ ] **Step 3: Implement row-level filtering via `arrow::compute`**

After reading the (projected ∪ predicate) table, build a boolean mask by ANDing each
predicate, then `Filter`. Convert each `Scalar` to an `arrow::Datum` matching the
column type; mismatch → `InvalidArgument`:
```cpp
#include <arrow/compute/api.h>
#include <arrow/scalar.h>

// Scalar + column type -> arrow Datum, or InvalidArgument on mismatch.
static Result<arrow::Datum> to_datum(const Scalar& s, const arrow::DataType& t);

static Result<std::shared_ptr<arrow::Table>>
apply_filter(const std::shared_ptr<arrow::Table>& table, const std::vector<Predicate>& preds) {
  if (preds.empty()) return Ok(table);
  arrow::Datum mask;  // start null
  for (const auto& p : preds) {
    const int idx = table->schema()->GetFieldIndex(p.column);
    if (idx < 0) return Err(ErrorCode::InvalidArgument, "filter: unknown column '" + p.column + "'");
    auto col = table->column(idx);
    arrow::Datum cmp;
    if (p.op == Compare::IsNull || p.op == Compare::IsNotNull) {
      auto r = arrow::compute::CallFunction(p.op == Compare::IsNull ? "is_null" : "is_valid", {col});
      if (!r.ok()) return Err(from_arrow(r.status(), "filter is_null"));
      cmp = *r;
    } else {
      auto d = to_datum(p.value, *col->type());
      if (!d.has_value()) return Err(d.error());
      const char* fn = nullptr;
      switch (p.op) { case Compare::Eq: fn="equal"; break; case Compare::Ne: fn="not_equal"; break;
        case Compare::Lt: fn="less"; break; case Compare::Le: fn="less_equal"; break;
        case Compare::Gt: fn="greater"; break; case Compare::Ge: fn="greater_equal"; break;
        default: fn="equal"; }
      auto r = arrow::compute::CallFunction(fn, {col, *d});
      if (!r.ok()) return Err(from_arrow(r.status(), "filter compare"));
      cmp = *r;
    }
    if (mask.is_arraylike() == false && mask.kind() == arrow::Datum::NONE) { mask = cmp; }
    else {
      auto r = arrow::compute::CallFunction("and_kleene", {mask, cmp});
      if (!r.ok()) return Err(from_arrow(r.status(), "filter and"));
      mask = *r;
    }
  }
  auto filtered = arrow::compute::CallFunction("filter", {table, mask});
  if (!filtered.ok()) return Err(from_arrow(filtered.status(), "filter apply"));
  return Ok(filtered->table());
}
```
Implement `to_datum` with a `std::visit` over `Scalar::Storage`, checking the variant
arm against `t.id()` (e.g. `i64` arm valid for INT8..INT64; `f64` for FLOAT/DOUBLE;
`std::string` for STRING; `bool` for BOOL; `time::Timestamp` for TIMESTAMP — build
`arrow::MakeScalar`/`arrow::TimestampScalar`). Any arm-vs-type mismatch →
`Err(InvalidArgument)`.

Wire into `collect`: read (selected ∪ predicate cols) → `apply_filter` → drop
predicate-only columns not in the user `select` → set `stats.rows_scanned`.

- [ ] **Step 4: Run — verify GREEN** (`-R Parquet.Filter`)

- [ ] **Step 5: Commit** `git commit -am "feat(io): parquet exact predicate filtering (arrow::compute)"`

---

## Task 8: Row-group pruning via statistics + `ScanStats`

**Files:** Modify `atx-core/src/io/parquet.cpp`, `atx-core/tests/parquet_test.cpp`.

- [ ] **Step 1: Failing test (observable pruning)**

```cpp
TEST(Parquet, RowGroupPruning) {
  // 3 row groups of 1000 rows: ids 0..2999. Filter id >= 2500 → only group 3 matches.
  auto path = temp_path("prune");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, /*row_group=*/1000);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{2500}}}).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 500);                 // 2500..2999
  auto st = lz->stats();
  EXPECT_EQ(st.row_groups_total, 3);
  EXPECT_EQ(st.row_groups_pruned, 2);            // groups 1 and 2 skipped by stats
}
```

- [ ] **Step 2: Run — verify it fails (`row_groups_pruned == 0`)** (RED)

- [ ] **Step 3: Implement statistics-based pruning**

Before reading, decide the surviving row-group set. For each row group, for each
predicate, fetch the column-chunk statistics and test satisfiability; skip the group if
any predicate is provably false:
```cpp
#include <parquet/statistics.h>

// True if the predicate could match some row in [min,max]/null_count of this chunk.
static bool chunk_may_match(const Predicate& p, const parquet::ColumnChunkMetaData& cc,
                            const arrow::DataType& type);

// Returns the indices of row groups that survive pruning.
static std::vector<int> surviving_row_groups(const parquet::FileMetaData& meta,
                                             const arrow::Schema& schema,
                                             const std::vector<Predicate>& preds) {
  std::vector<int> keep;
  for (int rg = 0; rg < meta.num_row_groups(); ++rg) {
    auto group = meta.RowGroup(rg);
    bool ok = true;
    for (const auto& p : preds) {
      const int col = schema.GetFieldIndex(p.column);
      if (col < 0) { ok = true; break; }                 // unknown handled later
      auto cc = group->ColumnChunk(col);
      if (!chunk_may_match(p, *cc, *schema.field(col)->type())) { ok = false; break; }
    }
    if (ok) keep.push_back(rg);
  }
  return keep;
}
```
`chunk_may_match`: if `!cc.is_stats_set()` return true (conservative). Read
`cc.statistics()`, and for numeric types compare typed min/max against the predicate
literal: e.g. `Ge`/`Gt` with `max < v` → false; `Le`/`Lt` with `min > v` → false; `Eq`
with `v < min || v > max` → false; `IsNotNull` with `num_values == 0` → false; else
true. Cast the `parquet::Statistics` to the typed subclass (`Int64Statistics`,
`DoubleStatistics`, …) by physical type. Spell out the numeric arms; return true for
types without comparable stats.

In `collect`, replace the full `ReadTable` with: compute `keep =
surviving_row_groups(...)`; set `stats.row_groups_total = num_row_groups()` and
`stats.row_groups_pruned = total - keep.size()`; read only `keep` via
`reader->ReadRowGroups(keep, indices, &table)`; then `apply_filter` (exact) → slice.
If `keep` is empty, return an empty table with the projected schema.

- [ ] **Step 4: Run — verify GREEN** (`-R Parquet.RowGroupPruning`)

- [ ] **Step 5: Commit** `git commit -am "feat(io): parquet row-group pruning via column statistics + ScanStats"`

---

## Task 9: Slice / limit / offset

**Files:** Modify `atx-core/src/io/parquet.cpp`, `atx-core/tests/parquet_test.cpp`.

- [ ] **Step 1: Failing tests**

```cpp
TEST(Parquet, LimitOffset) {
  auto path = temp_path("slice"); write_table(make_numeric_table(100), path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto t = lz->offset(10).limit(5).collect();
  ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->num_rows(), 5);
  auto v = t->column_view<int64_t>("id"); ASSERT_TRUE(v.has_value());
  EXPECT_EQ((*v)[0], 10); EXPECT_EQ((*v)[4], 14);
}
```

- [ ] **Step 2: Run — verify it fails (no slicing)** (RED)

- [ ] **Step 3: Implement slice in `collect`**

After filtering, apply offset/limit with `arrow::Table::Slice`:
```cpp
const int64_t n = table->num_rows();
const int64_t off = std::min<int64_t>(impl_->offset < 0 ? 0 : impl_->offset, n);
int64_t len = n - off;
if (impl_->limit >= 0) len = std::min<int64_t>(len, impl_->limit);
table = table->Slice(off, len);
```

- [ ] **Step 4: Run — verify GREEN** (`-R Parquet.LimitOffset`)

- [ ] **Step 5: Commit** `git commit -am "feat(io): parquet slice/limit/offset"`

---

## Task 10: Row-group streaming (`stream` / `RowGroupStream`)

**Files:** Modify `atx-core/src/io/parquet.cpp`, `atx-core/tests/parquet_test.cpp`.

- [ ] **Step 1: Failing test**

```cpp
TEST(Parquet, StreamSumsToCollect) {
  auto path = temp_path("stream");
  write_table(make_numeric_table(3000), path, arrow::Compression::SNAPPY, 1000);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  auto stream = lz->filter(Predicate{"id", Compare::Ge, Scalar{int64_t{500}}}).stream();
  ASSERT_TRUE(stream.has_value());
  int64_t total = 0; int batches = 0;
  for (;;) {
    auto batch = stream->next();
    ASSERT_TRUE(batch.has_value());
    if (!batch->has_value()) break;
    total += (*batch)->num_rows();
    ++batches;
  }
  EXPECT_EQ(total, 2500);          // ids 500..2999
  EXPECT_GE(batches, 1);
}
```

- [ ] **Step 2: Run — verify it fails (stream undefined)** (RED)

- [ ] **Step 3: Implement `stream` + `RowGroupStream::next`**

`RowGroupStream::Impl` holds the reader, the surviving row-group list (post-pruning),
the projection indices, the predicate list, a cursor, and remaining offset/limit
budget. `next()` reads the next surviving group via `ReadRowGroups({rg}, indices,
&table)`, applies `apply_filter`, applies the carried offset/limit budget, and returns
the batch as a `ParquetTable`; returns `nullopt` when groups are exhausted or the limit
budget reaches zero. `stream()` constructs the Impl from the plan (sharing the prune
result) and returns it. Spell out the budget arithmetic mirroring Task 9.

- [ ] **Step 4: Run — verify GREEN** (`-R Parquet.Stream`)

- [ ] **Step 5: Commit** `git commit -am "feat(io): parquet row-group streaming (bounded memory)"`

---

## Task 11: Error-path coverage

**Files:** Modify `atx-core/tests/parquet_test.cpp` (+ any small impl gaps).

- [ ] **Step 1: Failing/guard tests**

```cpp
TEST(Parquet, CorruptFileIsParseError) {
  auto path = temp_path("corrupt");
  { std::ofstream f(path, std::ios::binary); f << "not a parquet file at all"; }
  auto r = read_parquet(path);
  ASSERT_FALSE(r.has_value());
  EXPECT_EQ(r.error().code(), atx::core::ErrorCode::ParseError);
}

TEST(Parquet, NestedColumnBridgeNotImplemented) {
  // list<int64> column → schema Unsupported; to_frame skips it, to_column errors.
  arrow::ListBuilder lb(arrow::default_memory_pool(), std::make_shared<arrow::Int64Builder>());
  auto* vb = static_cast<arrow::Int64Builder*>(lb.value_builder());
  (void)lb.Append(); (void)vb->Append(1); (void)vb->Append(2);
  std::shared_ptr<arrow::Array> la; (void)lb.Finish(&la);
  auto schema = arrow::schema({arrow::field("l", arrow::list(arrow::int64()))});
  auto table = arrow::Table::Make(schema, {la});
  auto path = temp_path("nested"); write_table(table, path);
  auto lz = LazyParquet::scan(path); ASSERT_TRUE(lz.has_value());
  EXPECT_EQ(lz->schema().find("l")->dtype, DType::Unsupported);
  auto t = read_parquet(path); ASSERT_TRUE(t.has_value());
  EXPECT_EQ(t->to_column<int64_t>("l").error().code(), atx::core::ErrorCode::InvalidArgument);
}
```
Add `#include <fstream>` to the test.

- [ ] **Step 2: Run — verify failures, then fix any impl gaps**

`ParseError` mapping should already hold (Arrow returns `Invalid`/`IOError` for a bad
magic number — verify the actual code; if Parquet reports `IOError` for the bad footer,
map the "Parquet magic" message to `ParseError` in `from_arrow` or special-case the
open path). Confirm `to_column<int64_t>` on a list column returns `InvalidArgument`
(type mismatch in `contiguous`). Adjust expectations to the codes actually returned;
the test documents the contract.

- [ ] **Step 3: Run — verify GREEN** (`-R "Parquet.Corrupt|Parquet.Nested"`)

- [ ] **Step 4: Commit** `git commit -am "test(io): parquet error-path coverage"`

---

## Task 12: Umbrella + README + full-suite verification

**Files:** Modify `atx-core/include/atx/core/core.hpp`, `atx-core/README.md`.

- [ ] **Step 1: Add the io group to the umbrella**

In `core.hpp`, after the L9 series block:
```cpp
// ---- L10: io -------------------------------------------------------------
#include "atx/core/io/parquet.hpp"
```
Also add `io — Parquet lazy loader` to the namespace doc comment block.

- [ ] **Step 2: Document in README**

Add an `### L10 — io (atx::core::io)` table row for `io/parquet.hpp`
(`LazyParquet`/`read_parquet`/`ParquetTable`), and a short "Build requirement: Arrow via
vcpkg" note referencing `VCPKG_ROOT` and the `ninja` preset toolchain wiring.

- [ ] **Step 3: Build the umbrella TU + run the FULL suite**

Run the full build + `ctest --preset ninja --output-on-failure`.
Expected: all prior tests (765) + new Parquet tests PASS; `/W4 /permissive- /WX` clean;
`core_test.cpp` (umbrella include) compiles with `parquet.hpp` present.

- [ ] **Step 4: Commit**

```bash
git commit -am "feat(io): wire parquet into umbrella + README; full suite green"
```

---

## Self-Review (completed)

- **Spec coverage:** scan/select/filter/limit/offset/collect/stream (T2,3,6,7,9,10);
  projection pushdown (T6); predicate + row-group pruning (T7,T8); streaming (T10);
  slice/limit (T9); all dtypes (T3,T4); all codecs (T5); Arrow-Table materialization +
  bridges (T3,T4); schema introspection (T2); error mapping (T1,T11); vcpkg-required
  build (T1); umbrella/README (T12). All spec sections map to a task.
- **Out-of-scope honored:** no writer (tests use Arrow writer), no query engine, nested
  → Unsupported/NotImplemented.
- **Type consistency:** `ArrowTypeOf<T>`, `contiguous<T>`, `from_arrow`, `to_datum`,
  `apply_filter`, `surviving_row_groups`, `chunk_may_match`, `resolve_indices` used
  consistently; `ParquetTable::Impl`/`LazyParquet::Impl`/`RowGroupStream::Impl` names
  stable; `column_view`/`to_column`/`to_frame`/`strings` signatures match the header.
- **Open verifications flagged inline:** `time::Timestamp::unix_nanos()` getter name;
  exact `ReadRowGroups` overload signature; corrupt-file Arrow status code → confirm
  `ParseError` mapping. Each is called out at its step.
