#include "atx/core/io/parquet.hpp"

#include <arrow/io/file.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

#include <filesystem>

namespace atx::core::io {

using atx::core::Error;

// Map an arrow::Status to an atx Error.
[[nodiscard]] static Error from_arrow(const arrow::Status& s, std::string_view ctx) {
  std::string msg{ctx};
  msg += ": ";
  msg += s.ToString();
  if (s.IsIOError()) {
    return Error{ErrorCode::IoError, std::move(msg)};
  }
  if (s.IsInvalid()) {
    return Error{ErrorCode::ParseError, std::move(msg)};
  }
  if (s.IsNotImplemented()) {
    return Error{ErrorCode::NotImplemented, std::move(msg)};
  }
  return Error{ErrorCode::Internal, std::move(msg)};
}

const ColumnInfo* Schema::find(std::string_view name) const noexcept {
  for (const auto& c : columns) {
    if (c.name == name) {
      return &c;
    }
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
    // Best-effort pre-check: lets us return a friendly NotFound instead of a
    // raw Arrow IOError for the common missing-file case. The exists->Open
    // window is a benign TOCTOU; Open() below re-checks and is authoritative.
    if (!std::filesystem::exists(path)) {
      return Err(ErrorCode::NotFound,
                 std::string{"read_parquet: no such file: "} + std::string{path});
    }
    // Open the file (Arrow 24: ReadableFile::Open returns arrow::Result).
    auto in = arrow::io::ReadableFile::Open(std::string{path});
    if (!in.ok()) {
      return Err(from_arrow(in.status(), "read_parquet open"));
    }

    // Build a Parquet FileReader (Arrow 24: OpenFile returns
    // arrow::Result<std::unique_ptr<FileReader>>; the Status out-param form
    // was removed/changed in this version).
    auto reader_res = parquet::arrow::OpenFile(*in, arrow::default_memory_pool());
    if (!reader_res.ok()) {
      return Err(from_arrow(reader_res.status(), "read_parquet open parquet"));
    }
    std::unique_ptr<parquet::arrow::FileReader> reader = *std::move(reader_res);

    // Read the whole table (Arrow 24: the out-param ReadTable is deprecated and
    // would trip /WX, so use the arrow::Result-returning overload).
    auto table_res = reader->ReadTable();
    if (!table_res.ok()) {
      return Err(from_arrow(table_res.status(), "read_parquet read table"));
    }

    auto impl = std::make_unique<ParquetTable::Impl>();
    impl->table = *std::move(table_res);
    return ParquetTable{std::move(impl)};
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "read_parquet: unknown exception"); }
}

} // namespace atx::core::io
