#include "atx/core/io/parquet.hpp"

#include <arrow/io/file.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/reader.h>
#include <parquet/metadata.h>

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

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

// Map an arrow::DataType to an atx DType. Unsupported/unknown types collapse to
// DType::Unsupported so the surface stays total. DICTIONARY recurses into its
// value type (dictionary-encoded columns decode to their logical element type).
[[nodiscard]] static DType to_dtype(const arrow::DataType& type) noexcept {
  switch (type.id()) {
  case arrow::Type::BOOL:
    return DType::Bool;
  case arrow::Type::INT8:
    return DType::Int8;
  case arrow::Type::INT16:
    return DType::Int16;
  case arrow::Type::INT32:
    return DType::Int32;
  case arrow::Type::INT64:
    return DType::Int64;
  case arrow::Type::UINT8:
    return DType::UInt8;
  case arrow::Type::UINT16:
    return DType::UInt16;
  case arrow::Type::UINT32:
    return DType::UInt32;
  case arrow::Type::UINT64:
    return DType::UInt64;
  case arrow::Type::FLOAT:
    return DType::Float32;
  case arrow::Type::DOUBLE:
    return DType::Float64;
  case arrow::Type::STRING:
  case arrow::Type::LARGE_STRING:
    return DType::String;
  case arrow::Type::BINARY:
  case arrow::Type::LARGE_BINARY:
    return DType::Binary;
  case arrow::Type::DATE32:
    return DType::Date32;
  case arrow::Type::TIMESTAMP:
    return DType::Timestamp;
  case arrow::Type::DECIMAL128:
    return DType::Decimal128;
  case arrow::Type::DICTIONARY: {
    // SAFETY: id()==DICTIONARY guarantees the dynamic type is DictionaryType, so
    // this static_cast is well-defined; value_type() returns a non-null shared_ptr.
    const auto& dict = static_cast<const arrow::DictionaryType&>(type);
    return to_dtype(*dict.value_type());
  }
  default:
    return DType::Unsupported;
  }
}

// Build an atx Schema from an arrow Schema, preserving column order. Each field
// contributes its name, mapped dtype, and nullability.
[[nodiscard]] static Schema schema_from_arrow(const arrow::Schema& aschema) {
  Schema schema;
  schema.columns.reserve(static_cast<usize>(aschema.num_fields()));
  for (const auto& field : aschema.fields()) {
    schema.columns.push_back(
        ColumnInfo{field->name(), to_dtype(*field->type()), field->nullable()});
  }
  return schema;
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
    // Populate the cached atx Schema from the materialized table so
    // ParquetTable::schema() is non-empty.
    impl->schema = schema_from_arrow(*impl->table->schema());
    return ParquetTable{std::move(impl)};
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "read_parquet: unknown exception"); }
}

// LazyParquet Impl holds the open file + reader + cached metadata/schema, plus
// the deferred-scan plan state consumed by collect()/stream() in later tasks.
struct LazyParquet::Impl {
  std::shared_ptr<arrow::io::ReadableFile> file;
  std::unique_ptr<parquet::arrow::FileReader> reader;
  std::shared_ptr<parquet::FileMetaData> meta;
  Schema schema;
  std::vector<std::string> projection;
  std::vector<Predicate> predicates;
  i64 limit{-1};
  i64 offset{0};
  ScanStats stats{};
};

// Out-of-line special members: defined here where Impl is a complete type so the
// unique_ptr<Impl> in the header (incomplete there) can be destroyed/moved.
LazyParquet::LazyParquet(std::unique_ptr<Impl> impl) noexcept : impl_{std::move(impl)} {}
LazyParquet::LazyParquet(LazyParquet&&) noexcept = default;
LazyParquet& LazyParquet::operator=(LazyParquet&&) noexcept = default;
LazyParquet::~LazyParquet() = default;

Result<LazyParquet> LazyParquet::scan(std::string_view path) {
  try {
    // Friendly NotFound for the common missing-file case (see read_parquet for
    // the benign exists->Open TOCTOU note; Open() below is authoritative).
    if (!std::filesystem::exists(path)) {
      return Err(ErrorCode::NotFound,
                 std::string{"LazyParquet::scan: no such file: "} + std::string{path});
    }

    auto impl = std::make_unique<Impl>();

    // Open the file (Arrow 24: ReadableFile::Open returns arrow::Result).
    auto in = arrow::io::ReadableFile::Open(std::string{path});
    if (!in.ok()) {
      return Err(from_arrow(in.status(), "LazyParquet::scan open"));
    }
    impl->file = *std::move(in);

    // Build a Parquet FileReader (Arrow 24: OpenFile returns
    // arrow::Result<std::unique_ptr<FileReader>>).
    auto reader_res = parquet::arrow::OpenFile(impl->file, arrow::default_memory_pool());
    if (!reader_res.ok()) {
      return Err(from_arrow(reader_res.status(), "LazyParquet::scan open parquet"));
    }
    impl->reader = *std::move(reader_res);

    // Capture Parquet file metadata for num_rows()/num_row_groups().
    impl->meta = impl->reader->parquet_reader()->metadata();

    // Derive the arrow schema. GetSchema(out) is NOT deprecated in Arrow 24
    // (the plain virtual ::arrow::Status form), so it is /WX-safe here.
    std::shared_ptr<arrow::Schema> aschema;
    auto st = impl->reader->GetSchema(&aschema);
    if (!st.ok()) {
      return Err(from_arrow(st, "LazyParquet::scan get schema"));
    }
    impl->schema = schema_from_arrow(*aschema);

    impl->stats.row_groups_total = impl->meta->num_row_groups();
    return LazyParquet{std::move(impl)};
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "LazyParquet::scan: unknown exception"); }
}

const Schema& LazyParquet::schema() const noexcept { return impl_->schema; }
i64 LazyParquet::num_rows() const noexcept { return impl_->meta->num_rows(); }
i64 LazyParquet::num_row_groups() const noexcept { return impl_->meta->num_row_groups(); }
ScanStats LazyParquet::stats() const noexcept { return impl_->stats; }

LazyParquet& LazyParquet::select(std::vector<std::string> columns) {
  impl_->projection = std::move(columns);
  return *this;
}

LazyParquet& LazyParquet::filter(Predicate p) {
  impl_->predicates.push_back(std::move(p));
  return *this;
}

LazyParquet& LazyParquet::limit(i64 n) {
  impl_->limit = n;
  return *this;
}

LazyParquet& LazyParquet::offset(i64 n) {
  impl_->offset = n;
  return *this;
}

} // namespace atx::core::io
