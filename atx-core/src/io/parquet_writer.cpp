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
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>
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

// `max_row_group_rows` caps the rows Parquet will put in one row group. The
// whole-table path leaves it at the library default; the incremental path raises
// it because there the CALLER's chunk is the row group by construction.
[[nodiscard]] std::shared_ptr<parquet::WriterProperties>
writer_properties(WriteOptions opts, i64 max_row_group_rows) {
  parquet::WriterProperties::Builder pb;
  pb.compression(to_parquet(opts.compression));
  if (opts.dictionary) {
    pb.enable_dictionary();
  } else {
    pb.disable_dictionary();
  }
  pb.max_row_group_length(max_row_group_rows);
  return pb.build();
}

[[nodiscard]] Status write_table(const std::shared_ptr<arrow::Table> &table,
                                 const std::string &path, WriteOptions opts) {
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path{path}.parent_path(), ec);
  auto sink_r = arrow::io::FileOutputStream::Open(path);
  if (!sink_r.ok()) {
    return atx::core::Err(from_arrow(sink_r.status(), "open output"));
  }
  const std::shared_ptr<parquet::WriterProperties> props =
      writer_properties(opts, parquet::DEFAULT_MAX_ROW_GROUP_LENGTH);
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

// The Arrow schema `cols` describes: names, order and physical types only. The
// spans' lengths are irrelevant here, which is what lets an EMPTY column set
// declare a schema.
[[nodiscard]] std::shared_ptr<arrow::Schema> schema_of(std::span<const WriteColumn> cols) {
  std::vector<std::shared_ptr<arrow::Field>> fields;
  fields.reserve(cols.size());
  for (const auto &c : cols) {
    fields.push_back(arrow::field(c.name, arrow_type(c), /*nullable=*/false));
  }
  return arrow::schema(fields);
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

// ── ParquetRowGroupWriter ───────────────────────────────────────────────────

struct ParquetRowGroupWriter::Impl {
  std::shared_ptr<arrow::Schema> schema;
  std::shared_ptr<arrow::io::FileOutputStream> sink;
  std::unique_ptr<parquet::arrow::FileWriter> writer;
  i64 row_groups{0};
};

namespace {

// Close for side effects only, swallowing every outcome.
//
// Both callers below are `noexcept` and have nobody to report to; what they DO
// have to avoid is dropping an open writer, because abandoning one leaves a
// file with no footer, which no reader can open. A caller that needs to know
// whether the footer landed calls `close()` itself.
void close_quietly(ParquetRowGroupWriter &w) noexcept {
  try {
    const Status ignored = w.close();
    static_cast<void>(ignored);
  } catch (...) { // NOLINT — a noexcept close path has nowhere to propagate
  }
}

} // namespace

ParquetRowGroupWriter::ParquetRowGroupWriter() noexcept = default;
ParquetRowGroupWriter::ParquetRowGroupWriter(ParquetRowGroupWriter &&) noexcept = default;

ParquetRowGroupWriter &ParquetRowGroupWriter::operator=(ParquetRowGroupWriter &&other) noexcept {
  if (this != &other) {
    close_quietly(*this); // never abandon the file this writer already owns
    impl_ = std::move(other.impl_);
  }
  return *this;
}

ParquetRowGroupWriter::~ParquetRowGroupWriter() { close_quietly(*this); }

Status ParquetRowGroupWriter::open(std::span<const WriteColumn> schema_cols,
                                   std::string_view path, WriteOptions opts) {
  if (impl_ != nullptr && impl_->writer != nullptr) {
    return atx::core::Err(ErrorCode::InvalidArgument, "parquet row-group writer already open");
  }
  if (schema_cols.empty()) {
    return atx::core::Err(ErrorCode::InvalidArgument, "no columns");
  }
  const std::string file{path};
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path{file}.parent_path(), ec);
  auto sink_r = arrow::io::FileOutputStream::Open(file);
  if (!sink_r.ok()) {
    return atx::core::Err(from_arrow(sink_r.status(), "open output"));
  }

  auto impl = std::make_unique<Impl>();
  impl->schema = schema_of(schema_cols);
  impl->sink = *sink_r;

  // `FileWriter::WriteTable` CLAMPS its chunk_size to max_row_group_length, so
  // the library's default 1 Mi-row cap would silently split a larger chunk into
  // several row groups and break this class's one-group-per-call contract.
  auto writer_r = parquet::arrow::FileWriter::Open(
      *impl->schema, arrow::default_memory_pool(), impl->sink,
      writer_properties(opts, std::numeric_limits<i64>::max()),
      parquet::default_arrow_writer_properties());
  if (!writer_r.ok()) {
    return atx::core::Err(from_arrow(writer_r.status(), "open parquet writer"));
  }
  impl->writer = std::move(*writer_r);
  impl_ = std::move(impl);
  return atx::core::Ok();
}

Status ParquetRowGroupWriter::write_row_group(std::span<const WriteColumn> cols) {
  if (impl_ == nullptr || impl_->writer == nullptr) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "parquet row-group writer is not open");
  }
  ATX_TRY_VOID(validate_lengths(cols));
  const usize n = column_rows(cols.front());
  if (n == 0) {
    return atx::core::Ok(); // an empty row group would only misstate the file
  }
  const auto rows = all_rows(n);
  auto table = build_table(cols, rows, /*skip=*/"");
  if (!table.ok()) {
    return atx::core::Err(from_arrow(table.status(), "build row group"));
  }
  if (!(*table)->schema()->Equals(*impl_->schema, /*check_metadata=*/false)) {
    return atx::core::Err(ErrorCode::InvalidArgument,
                          "row group schema does not match the schema this file was opened with");
  }
  // chunk_size == the chunk's own row count: exactly one row group per call.
  auto st = impl_->writer->WriteTable(**table, static_cast<i64>(n));
  if (!st.ok()) {
    return atx::core::Err(from_arrow(st, "write row group"));
  }
  ++impl_->row_groups;
  return atx::core::Ok();
}

Status ParquetRowGroupWriter::close() {
  if (impl_ == nullptr || impl_->writer == nullptr) {
    return atx::core::Ok();
  }
  // Release the writer first either way: a failed Close leaves a file that must
  // not be written to again, and a second close attempt would compound it.
  std::unique_ptr<parquet::arrow::FileWriter> writer = std::move(impl_->writer);
  auto st = writer->Close();
  writer.reset();
  Status out = st.ok() ? atx::core::Ok() : atx::core::Err(from_arrow(st, "close parquet writer"));
  if (impl_->sink != nullptr && !impl_->sink->closed()) {
    auto cs = impl_->sink->Close();
    if (!cs.ok() && out.has_value()) {
      out = atx::core::Err(from_arrow(cs, "close output"));
    }
  }
  impl_->sink.reset();
  return out;
}

bool ParquetRowGroupWriter::is_open() const noexcept {
  return impl_ != nullptr && impl_->writer != nullptr;
}

i64 ParquetRowGroupWriter::row_groups_written() const noexcept {
  return impl_ == nullptr ? 0 : impl_->row_groups;
}

} // namespace atx::core::io
