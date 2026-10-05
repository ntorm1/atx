#include "atx/engine/research/fields/sources/parquet_columns.hpp"

#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>

#include "atx/core/datetime.hpp"
#include "atx/engine/research/fields/sources/stage.hpp"

namespace atx::engine::research::fields {
namespace {

using core::io::DType;
using core::io::ParquetTable;

[[nodiscard]] core::Error bad_type(std::string_view name, std::string_view want) {
  return core::Error(core::ErrorCode::InvalidArgument,
                     "stage column " + std::string(name) + " is not " + std::string(want));
}

[[nodiscard]] core::Result<DType> dtype_of(const ParquetTable &table, std::string_view name) {
  const core::io::ColumnInfo *info = table.schema().find(name);
  if (info == nullptr) {
    return core::Err(core::ErrorCode::InvalidArgument,
                     "stage column " + std::string(name) + " is absent");
  }
  return core::Ok(info->dtype);
}

// The column's values as Out (a widening or same-width conversion of every supported type).
template <class T, class Out>
[[nodiscard]] core::Result<std::vector<Out>> converted(const ParquetTable &table,
                                                       std::string_view name) {
  ATX_TRY(const auto view, table.column_view<T>(name));
  std::vector<Out> out;
  out.reserve(view.size());
  for (const T v : view) {
    out.push_back(static_cast<Out>(v));
  }
  return core::Ok(std::move(out));
}

// Every null row of the column set to `fill`.
template <class T>
[[nodiscard]] core::Status apply_nulls(const ParquetTable &table, std::string_view name,
                                       std::vector<T> &values, T fill) {
  ATX_TRY(const auto nulls, table.null_mask(name));
  if (nulls.empty() && values.empty()) {
    return core::Ok();
  }
  if (nulls.size() != values.size()) {
    return core::Err(core::ErrorCode::Internal,
                     "stage column " + std::string(name) + ": null mask length differs");
  }
  for (usize i = 0; i < values.size(); ++i) {
    if (nulls[i] != 0) {
      values[i] = fill;
    }
  }
  return core::Ok();
}

[[nodiscard]] core::Result<std::vector<f64>> real_values(const ParquetTable &table,
                                                         std::string_view name, DType type) {
  switch (type) {
  case DType::Float64:
    return converted<double, f64>(table, name);
  case DType::Float32:
    return converted<float, f64>(table, name);
  case DType::Int64:
    return converted<std::int64_t, f64>(table, name);
  case DType::Int32:
    return converted<std::int32_t, f64>(table, name);
  case DType::Int16:
    return converted<std::int16_t, f64>(table, name);
  case DType::Int8:
    return converted<std::int8_t, f64>(table, name);
  case DType::UInt64:
    return converted<std::uint64_t, f64>(table, name);
  case DType::UInt32:
    return converted<std::uint32_t, f64>(table, name);
  case DType::UInt16:
    return converted<std::uint16_t, f64>(table, name);
  case DType::UInt8:
    return converted<std::uint8_t, f64>(table, name);
  default:
    return core::Err(bad_type(name, "numeric"));
  }
}

// A non-null id text: an optional '-' and decimal digits, the whole text.
[[nodiscard]] core::Result<i64> parse_id(std::string_view text, std::string_view name) {
  i64 value = 0;
  const char *end = text.data() + text.size();
  const auto parsed = std::from_chars(text.data(), end, value);
  if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != end) {
    return core::Err(core::ErrorCode::ParseError, "stage column " + std::string(name) +
                                                      ": an id is not a decimal integer");
  }
  return core::Ok(value);
}

[[nodiscard]] core::Result<std::vector<i64>> text_ids(const ParquetTable &table,
                                                      std::string_view name) {
  ATX_TRY(const auto views, table.strings(name));
  ATX_TRY(const auto nulls, table.null_mask(name));
  std::vector<i64> out(views.size(), -1);
  for (usize i = 0; i < views.size(); ++i) {
    if (i < nulls.size() && nulls[i] != 0) {
      continue;
    }
    ATX_TRY(out[i], parse_id(views[i], name));
  }
  return core::Ok(std::move(out));
}

[[nodiscard]] core::Result<std::vector<i64>> integer_ids(const ParquetTable &table,
                                                         std::string_view name, DType type) {
  switch (type) {
  case DType::Int64:
    return converted<std::int64_t, i64>(table, name);
  case DType::Int32:
    return converted<std::int32_t, i64>(table, name);
  case DType::Int16:
    return converted<std::int16_t, i64>(table, name);
  case DType::Int8:
    return converted<std::int8_t, i64>(table, name);
  case DType::UInt32:
    return converted<std::uint32_t, i64>(table, name);
  case DType::UInt16:
    return converted<std::uint16_t, i64>(table, name);
  case DType::UInt8:
    return converted<std::uint8_t, i64>(table, name);
  case DType::UInt64: {
    ATX_TRY(const auto view, table.column_view<std::uint64_t>(name));
    std::vector<i64> out;
    out.reserve(view.size());
    for (const std::uint64_t v : view) {
      if (v > static_cast<std::uint64_t>(std::numeric_limits<i64>::max())) {
        return core::Err(bad_type(name, "an id below 2^63"));
      }
      out.push_back(static_cast<i64>(v));
    }
    return core::Ok(std::move(out));
  }
  default:
    return core::Err(bad_type(name, "an integer or decimal-text id"));
  }
}

} // namespace

core::Result<std::vector<f64>> real_column(const ParquetTable &table, std::string_view name) {
  ATX_TRY(const DType type, dtype_of(table, name));
  ATX_TRY(auto values, real_values(table, name, type));
  ATX_TRY_VOID(apply_nulls(table, name, values, std::numeric_limits<f64>::quiet_NaN()));
  return core::Ok(std::move(values));
}

core::Result<std::vector<i64>> id_column(const ParquetTable &table, std::string_view name) {
  ATX_TRY(const DType type, dtype_of(table, name));
  if (type == DType::String) {
    return text_ids(table, name);
  }
  ATX_TRY(auto values, integer_ids(table, name, type));
  ATX_TRY_VOID(apply_nulls(table, name, values, i64{-1}));
  return core::Ok(std::move(values));
}

core::Result<std::vector<i64>> day_column(const ParquetTable &table, std::string_view name) {
  ATX_TRY(const DType type, dtype_of(table, name));
  if (type != DType::Date32) {
    return core::Err(bad_type(name, "a DATE32 column"));
  }
  ATX_TRY(const auto days, table.date32_days(name));
  std::vector<i64> out;
  out.reserve(days.size());
  for (const i32 d : days) {
    out.push_back(static_cast<i64>(d));
  }
  ATX_TRY_VOID(apply_nulls(table, name, out, kNullDay));
  return core::Ok(std::move(out));
}

core::Result<std::vector<i64>> instant_column(const ParquetTable &table, std::string_view name) {
  ATX_TRY(const DType type, dtype_of(table, name));
  if (type != DType::Timestamp) {
    return core::Err(bad_type(name, "a TIMESTAMP column"));
  }
  ATX_TRY(const auto column, table.to_column<core::time::Timestamp>(name));
  std::vector<i64> out;
  out.reserve(column.size());
  for (const core::time::Timestamp ts : column.view()) {
    out.push_back(ts.unix_nanos());
  }
  ATX_TRY_VOID(apply_nulls(table, name, out, kNeverNs));
  return core::Ok(std::move(out));
}

core::Result<std::vector<std::string>> text_column(const ParquetTable &table,
                                                   std::string_view name) {
  ATX_TRY(const DType type, dtype_of(table, name));
  if (type != DType::String) {
    return core::Err(bad_type(name, "a UTF-8 column"));
  }
  ATX_TRY(const auto views, table.strings(name));
  std::vector<std::string> out;
  out.reserve(views.size());
  for (const std::string_view v : views) {
    out.emplace_back(v);
  }
  return core::Ok(std::move(out));
}

} // namespace atx::engine::research::fields
