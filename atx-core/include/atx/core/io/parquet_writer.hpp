#pragma once

// atx::core::io — Parquet WRITE surface (companion to the read-side parquet.hpp).
// Arrow types stay in parquet_writer.cpp; this header includes no Arrow. Columns
// are borrowed spans (must outlive the call) and must all share the same length.

#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "atx/core/datetime.hpp" // time::Timestamp
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"

namespace atx::core::io {

using atx::core::Result;
using atx::core::Status;

struct WriteColumn {
  std::string name;
  std::variant<std::span<const i64>, std::span<const f64>, std::span<const std::string>,
               std::span<const time::Timestamp>>
      data;
};

// Write all columns to one Parquet file at `path` (parent dirs created).
[[nodiscard]] Status write_parquet(std::span<const WriteColumn> cols, std::string_view path);

// Hive-partition by string column `partition_col`: bucket rows by its distinct
// values, DROP that column from each file (path-encoded), and write one file per
// bucket at <root>/<partition_col>=<value>/data.parquet. Returns partitions
// written. `partition_col` must name a std::string column in `cols`.
[[nodiscard]] Result<i64> write_hive_parquet(std::span<const WriteColumn> cols,
                                             std::string_view root, std::string_view partition_col);

} // namespace atx::core::io
