#include "atx/core/io/parquet_writer.hpp"

#include <arrow/builder.h>
#include <arrow/io/file.h>
#include <arrow/memory_pool.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <filesystem>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace atx::core::io {

using atx::core::Error;
using atx::core::ErrorCode;

namespace {

[[nodiscard]] Error from_arrow(const arrow::Status &s, std::string_view ctx) {
  std::string msg{ctx};
  msg += ": ";
  msg += s.ToString();
  if (s.IsIOError()) {
    return Error{ErrorCode::IoError, std::move(msg)};
  }
  if (s.IsInvalid()) {
    return Error{ErrorCode::ParseError, std::move(msg)};
  }
  return Error{ErrorCode::Internal, std::move(msg)};
}

[[nodiscard]] usize column_rows(const WriteColumn &c) noexcept {
  return std::visit([](auto &&s) { return s.size(); }, c.data);
}

[[nodiscard]] std::shared_ptr<arrow::DataType> arrow_type(const WriteColumn &c) {
  return std::visit(
      [](auto &&s) -> std::shared_ptr<arrow::DataType> {
        using T = std::remove_const_t<typename std::decay_t<decltype(s)>::value_type>;
        if constexpr (std::is_same_v<T, i64>) {
          return arrow::int64();
        } else if constexpr (std::is_same_v<T, f64>) {
          return arrow::float64();
        } else if constexpr (std::is_same_v<T, std::string>) {
          return arrow::utf8();
        } else {
          return arrow::timestamp(arrow::TimeUnit::NANO);
        }
      },
      c.data);
}

// Build one column's array from the rows at the given indices.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Array>>
build_array(const WriteColumn &c, std::span<const usize> rows) {
  std::shared_ptr<arrow::Array> out;
  auto append_all = [&](auto &builder, auto fn) -> arrow::Status {
    ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<i64>(rows.size())));
    for (usize idx : rows) {
      ARROW_RETURN_NOT_OK(fn(builder, idx));
    }
    return builder.Finish(&out);
  };
  arrow::Status st;
  std::visit(
      [&](auto &&s) {
        using T = std::remove_const_t<typename std::decay_t<decltype(s)>::value_type>;
        if constexpr (std::is_same_v<T, i64>) {
          arrow::Int64Builder b;
          st = append_all(b, [&](auto &bb, usize i) { return bb.Append(s[i]); });
        } else if constexpr (std::is_same_v<T, f64>) {
          arrow::DoubleBuilder b;
          st = append_all(b, [&](auto &bb, usize i) { return bb.Append(s[i]); });
        } else if constexpr (std::is_same_v<T, std::string>) {
          arrow::StringBuilder b;
          st = append_all(b, [&](auto &bb, usize i) { return bb.Append(s[i]); });
        } else { // time::Timestamp
          arrow::TimestampBuilder b{arrow::timestamp(arrow::TimeUnit::NANO),
                                    arrow::default_memory_pool()};
          st = append_all(b, [&](auto &bb, usize i) { return bb.Append(s[i].unix_nanos()); });
        }
      },
      c.data);
  if (!st.ok()) {
    return st;
  }
  return out;
}

// Build a Table from the columns whose name != skip, taking the given rows.
[[nodiscard]] arrow::Result<std::shared_ptr<arrow::Table>>
build_table(std::span<const WriteColumn> cols, std::span<const usize> rows, std::string_view skip) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  std::vector<std::shared_ptr<arrow::Array>> arrays;
  for (const auto &c : cols) {
    if (c.name == skip) {
      continue;
    }
    fields.push_back(arrow::field(c.name, arrow_type(c), /*nullable=*/false));
    ARROW_ASSIGN_OR_RAISE(auto arr, build_array(c, rows));
    arrays.push_back(std::move(arr));
  }
  return arrow::Table::Make(arrow::schema(fields), arrays, static_cast<i64>(rows.size()));
}

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

[[nodiscard]] std::vector<usize> all_rows(usize n) {
  std::vector<usize> v(n);
  for (usize i = 0; i < n; ++i) {
    v[i] = i;
  }
  return v;
}

[[nodiscard]] Status validate_lengths(std::span<const WriteColumn> cols) {
  if (cols.empty()) {
    return atx::core::Err(ErrorCode::InvalidArgument, "no columns");
  }
  const usize n = column_rows(cols.front());
  for (const auto &c : cols) {
    if (column_rows(c) != n) {
      return atx::core::Err(ErrorCode::InvalidArgument, "column length mismatch");
    }
  }
  return atx::core::Ok();
}

} // namespace

Status write_parquet(std::span<const WriteColumn> cols, std::string_view path, WriteOptions opts) {
  ATX_TRY_VOID(validate_lengths(cols));
  const auto rows = all_rows(column_rows(cols.front()));
  auto table = build_table(cols, rows, /*skip=*/"");
  if (!table.ok()) {
    return atx::core::Err(from_arrow(table.status(), "build table"));
  }
  return write_table(*table, std::string{path}, opts);
}

Result<i64> write_hive_parquet(std::span<const WriteColumn> cols, std::string_view root,
                               std::string_view partition_col, WriteOptions opts) {
  ATX_TRY_VOID(validate_lengths(cols));
  const usize n = column_rows(cols.front());

  // The partition column must be present and a std::string column.
  const WriteColumn *pcol = nullptr;
  for (const auto &c : cols) {
    if (c.name == partition_col) {
      pcol = &c;
      break;
    }
  }
  if (pcol == nullptr) {
    return atx::core::Err(ErrorCode::InvalidArgument, "partition column not found");
  }
  const auto *pvals = std::get_if<std::span<const std::string>>(&pcol->data);
  if (pvals == nullptr) {
    return atx::core::Err(ErrorCode::InvalidArgument, "partition column must be string");
  }

  // Bucket row indices by partition value, preserving first-seen order.
  std::unordered_map<std::string_view, usize> index;
  std::vector<std::string> values;
  std::vector<std::vector<usize>> buckets;
  for (usize i = 0; i < n; ++i) {
    const std::string &v = (*pvals)[i];
    auto it = index.find(v);
    if (it == index.end()) {
      index.emplace(v, buckets.size());
      values.push_back(v);
      buckets.emplace_back(std::vector<usize>{i});
    } else {
      buckets[it->second].push_back(i);
    }
  }

  for (usize b = 0; b < buckets.size(); ++b) {
    auto table = build_table(cols, buckets[b], /*skip=*/partition_col);
    if (!table.ok()) {
      return atx::core::Err(from_arrow(table.status(), "build table"));
    }
    const std::string path =
        std::string{root} + "/" + std::string{partition_col} + "=" + values[b] + "/data.parquet";
    ATX_TRY_VOID(write_table(*table, path, opts));
  }
  return atx::core::Ok(static_cast<i64>(buckets.size()));
}

} // namespace atx::core::io
