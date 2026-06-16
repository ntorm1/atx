#pragma once

// atx::core::io — Arrow-backed, Polars-style lazy Parquet loader.
// This header includes NO Arrow headers; all Arrow types live behind PIMPL in
// parquet.cpp. Expected failures travel in Result<T>; the surface never throws
// (only std::bad_alloc may propagate).

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
  // Materializes the POD columns (numeric + bool + timestamp) into a Frame.
  // String/binary/date32/decimal/nested columns are skipped (no contiguous
  // typed Column<T> bridge).
  [[nodiscard]] Result<series::Frame> to_frame() const;
  [[nodiscard]] Result<std::vector<std::string_view>> strings(std::string_view name) const;

  // DATE32 accessor: returns one epoch-day (days since 1970-01-01) per row.
  // Arrow date32 is physically int32 but carries a distinct type id, so the
  // numeric column_view<int32_t> path rejects it; this bridge reads it explicitly.
  // Null slots read as the sentinel std::numeric_limits<i32>::min() (callers that
  // must distinguish null from a real date use null_mask()). Err(InvalidArgument)
  // if the column is absent or not DATE32.
  [[nodiscard]] Result<std::vector<i32>> date32_days(std::string_view name) const;

  // Per-row null indicator for ANY column type: out[i]==1 iff row i is null in
  // `name`, else 0. The numeric/string/date bridges do NOT propagate validity
  // (a null reads as a stored/sentinel value), so callers needing PIT-correct
  // "missing vs zero" semantics consult this mask. Err(InvalidArgument) if the
  // column is absent. A column with no validity buffer yields an all-zero mask.
  [[nodiscard]] Result<std::vector<u8>> null_mask(std::string_view name) const;

  struct Impl;
  explicit ParquetTable(std::unique_ptr<Impl> impl) noexcept;
private:
  std::unique_ptr<Impl> impl_;
};

// Non-numeric column bridges are provided as explicit specializations in
// parquet.cpp (bool is bit-packed; Timestamp needs unit conversion). Declared
// here so every TU sees them before first use (avoids ODR/IFNDR).
template <>
[[nodiscard]] Result<std::span<const bool>>
ParquetTable::column_view<bool>(std::string_view name) const;
template <>
[[nodiscard]] Result<series::Column<bool>>
ParquetTable::to_column<bool>(std::string_view name) const;
template <>
[[nodiscard]] Result<std::span<const time::Timestamp>>
ParquetTable::column_view<time::Timestamp>(std::string_view name) const;
template <>
[[nodiscard]] Result<series::Column<time::Timestamp>>
ParquetTable::to_column<time::Timestamp>(std::string_view name) const;

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

  // The plan is REUSABLE: collect()/stream() may be called more than once on the
  // same LazyParquet, and builder setters (select/filter/limit/offset) accumulate
  // across calls.
  [[nodiscard]] Result<ParquetTable> collect();
  [[nodiscard]] Result<RowGroupStream> stream();
  // Reflects the MOST RECENT collect()/stream(). rows_scanned is populated by
  // collect() only (left as-is during streaming -- use each batch's num_rows()
  // when streaming).
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
