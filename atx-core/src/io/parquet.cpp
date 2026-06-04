#include "atx/core/io/parquet.hpp"

#include <arrow/array.h>                 // Int64Array, DoubleArray, ... raw_values()
#include <arrow/array/concatenate.h>     // arrow::Concatenate (kernel-free row take)
#include <arrow/chunked_array.h>         // arrow::ChunkedArray (Table::column)
#include <arrow/io/file.h>
#include <arrow/result.h>
#include <arrow/status.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <parquet/arrow/reader.h>
#include <parquet/metadata.h>
#include <parquet/statistics.h>
#include <parquet/types.h>

#include <algorithm>
#include <cmath>
#include <type_traits>

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace atx::core::io {

using atx::core::Error;

// Forward declaration: the timestamp unit->ns conversion (defined with the
// Task-4 bridges below) is also used by the Task-7 timestamp predicate evaluator.
[[nodiscard]] static atx::core::time::Timestamp to_timestamp(i64 raw,
                                                             arrow::TimeUnit::type unit);

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
    // (Arrow never nests dictionary value types; recursion depth is 1.)
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
    // Combine chunks so every column is a single contiguous chunk owned by the
    // table. This makes column_view's raw_values() alias buffers whose lifetime
    // is tied to impl_->table (no dangling spans on multi-row-group files).
    auto combined = impl->table->CombineChunks(arrow::default_memory_pool());
    if (!combined.ok()) {
      return Err(from_arrow(combined.status(), "read_parquet combine chunks"));
    }
    impl->table = *combined;
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

// Resolves column names to parquet leaf indices against `schema`. Unknown name
// -> InvalidArgument. (Flat schemas: arrow field index == leaf column index.)
[[nodiscard]] static Result<std::vector<int>>
resolve_indices(const arrow::Schema& schema, const std::vector<std::string>& names) {
  std::vector<int> idx;
  idx.reserve(names.size());
  for (const auto& n : names) {
    const int i = schema.GetFieldIndex(n);
    if (i < 0) {
      return Err(ErrorCode::InvalidArgument, "select: unknown column '" + n + "'");
    }
    idx.push_back(i);
  }
  return Ok(std::move(idx));
}

// ---------------------------------------------------------------------------
// Exact row-level filtering (Task 7): Polars semantics, evaluated row-by-row.
//
// DEVIATION (Arrow-24 build): the installed vcpkg Arrow 24 was compiled with
// ARROW_COMPUTE OFF (util/config.h: `#undef ARROW_COMPUTE`), so the scalar
// comparison / boolean / filter kernels are NOT registered and
// arrow::compute::Initialize() is not even exported by arrow.dll. The planned
// arrow::compute::CallFunction("greater_equal"/"filter"/...) path fails to link
// here. We therefore evaluate predicates directly against the typed Arrow arrays
// (no compute kernels) and rebuild the filtered table from contiguous kept-row
// runs via arrow::Array::Slice + arrow::Concatenate (both core arrow, not
// compute). Behaviour is identical: exact, row-level, ANDed predicates.
//
// Row-group statistics pruning is Task 8; here every selected row is compared.
// ---------------------------------------------------------------------------

// Three-valued comparison outcome for one row: True keeps it (subject to AND),
// False drops it, and Mismatch signals a non-comparable literal/column-type pair
// (e.g. a string literal against an int64 column) -> InvalidArgument upstream.
enum class Cmp3 { False, True, Mismatch };

// Folds a C++ three-way ordering (lhs <=> rhs already computed as -1/0/+1 in
// `sign`) through the predicate op into a keep/drop decision.
[[nodiscard]] static Cmp3 fold_order(Compare op, int sign) noexcept {
  switch (op) {
  case Compare::Eq: return sign == 0 ? Cmp3::True : Cmp3::False;
  case Compare::Ne: return sign != 0 ? Cmp3::True : Cmp3::False;
  case Compare::Lt: return sign < 0 ? Cmp3::True : Cmp3::False;
  case Compare::Le: return sign <= 0 ? Cmp3::True : Cmp3::False;
  case Compare::Gt: return sign > 0 ? Cmp3::True : Cmp3::False;
  case Compare::Ge: return sign >= 0 ? Cmp3::True : Cmp3::False;
  case Compare::IsNull:
  case Compare::IsNotNull:
    break;
  }
  return Cmp3::False; // null-ops handled before fold_order is reached
}

// sign(a <=> b) as -1/0/+1 for any comparable T (integral/floating/string).
template <class T>
[[nodiscard]] static int order_sign(const T& a, const T& b) noexcept {
  if (a < b) { return -1; }
  if (b < a) { return 1; }
  return 0;
}

// Compares one numeric array element (index `i`) of typed array `arr` against the
// literal `lit` under `op`. T is the array's element type; Lit is the literal's
// natural C++ type (int64_t / double). The compare happens in the wider of the
// two via common_type so an i64 literal can match an int32 column, etc.
// NOTE (float precision): for |value| > 2^53 the common_type widening to double
// loses integer precision; equality near that boundary may be approximate. Out
// of scope here (no >2^53 fixtures); documented for completeness.
template <class ArrayT, class Lit>
[[nodiscard]] static Cmp3 compare_numeric(const ArrayT& arr, int64_t i, Compare op,
                                          Lit lit) noexcept {
  using Elem = decltype(arr.Value(i));
  // Guard the signed/unsigned wrap pitfall BEFORE the common_type widening: a
  // negative signed literal is strictly less than every unsigned cell, so the
  // order sign is always +1 (cell > lit). Only UInt64 is actually affected
  // (common_type of uint32/int64 is int64), but the guard is correct for any
  // unsigned Elem.
  if constexpr (std::is_unsigned_v<std::decay_t<Elem>> && std::is_signed_v<Lit>) {
    if (lit < 0) {
      return fold_order(op, 1); // negative literal < every unsigned cell
    }
  }
  using Wide = std::common_type_t<std::decay_t<Elem>, Lit>;
  // NaN is unequal to everything (IEEE / Polars): if either operand is NaN, Ne
  // keeps the row and every other op drops it. Must precede order_sign, which
  // would otherwise report "equal" (sign 0) for a NaN operand.
  if constexpr (std::is_floating_point_v<Wide>) {
    const Wide a = static_cast<Wide>(arr.Value(i));
    const Wide b = static_cast<Wide>(lit);
    if (a != a || b != b) { // NaN: unequal to everything
      return (op == Compare::Ne) ? Cmp3::True : Cmp3::False;
    }
  }
  // NOLINTNEXTLINE(bugprone-signed-char-misuse): widening int8->int64 here
  // sign-extends (no truncation); the cast is to a signed wider type.
  const auto cell = static_cast<Wide>(arr.Value(i));
  const int sign = order_sign<Wide>(cell, static_cast<Wide>(lit));
  return fold_order(op, sign);
}

// Evaluates predicate `p` (a binary numeric comparison) against a numeric column
// `col` whose concrete array type is ArrayT, writing the per-row keep decision
// into `keep` (ANDing with the prior mask). Returns Mismatch iff the scalar
// literal is not a numeric (int/float/bool) value.
template <class ArrayT>
[[nodiscard]] static Cmp3 eval_numeric_column(const arrow::ChunkedArray& col,
                                              const Predicate& p,
                                              std::vector<bool>& keep) {
  const Scalar::Storage& v = p.value.value();
  // SAFETY: col is single-chunk after CombineChunks and id() was matched to
  // ArrayT by the caller, so this cast is well-defined.
  const auto& arr = static_cast<const ArrayT&>(*col.chunk(0));
  const int64_t n = arr.length();
  auto run = [&](auto lit) {
    for (int64_t i = 0; i < n; ++i) {
      if (!keep[static_cast<usize>(i)]) { continue; }
      const bool ok = !arr.IsNull(i) &&
                      compare_numeric<ArrayT>(arr, i, p.op, lit) == Cmp3::True;
      keep[static_cast<usize>(i)] = ok;
    }
  };
  if (std::holds_alternative<i64>(v)) {
    run(static_cast<int64_t>(std::get<i64>(v)));
  } else if (std::holds_alternative<f64>(v)) {
    run(std::get<f64>(v));
  } else if (std::holds_alternative<bool>(v)) {
    run(static_cast<int64_t>(std::get<bool>(v) ? 1 : 0));
  } else {
    return Cmp3::Mismatch; // string/timestamp literal vs numeric column
  }
  return Cmp3::True;
}

// Evaluates `p` against a BOOL column. Only a bool literal is comparable; Eq/Ne
// are the meaningful ops but ordering ops fold through bool->int (false<true).
[[nodiscard]] static Cmp3 eval_bool_column(const arrow::ChunkedArray& col,
                                           const Predicate& p,
                                           std::vector<bool>& keep) {
  const Scalar::Storage& v = p.value.value();
  if (!std::holds_alternative<bool>(v)) {
    return Cmp3::Mismatch;
  }
  // SAFETY: id()==BOOL so chunk(0) is a BooleanArray (single-chunk post-combine).
  const auto& arr = static_cast<const arrow::BooleanArray&>(*col.chunk(0));
  const int lit = std::get<bool>(v) ? 1 : 0;
  for (int64_t i = 0; i < arr.length(); ++i) {
    if (!keep[static_cast<usize>(i)]) { continue; }
    const bool ok = !arr.IsNull(i) &&
                    fold_order(p.op, order_sign<int>(arr.Value(i) ? 1 : 0, lit)) == Cmp3::True;
    keep[static_cast<usize>(i)] = ok;
  }
  return Cmp3::True;
}

// Evaluates `p` against a UTF-8 STRING column. Only a string literal is
// comparable; comparison is lexicographic via std::string_view ordering.
[[nodiscard]] static Cmp3 eval_string_column(const arrow::ChunkedArray& col,
                                             const Predicate& p,
                                             std::vector<bool>& keep) {
  const Scalar::Storage& v = p.value.value();
  if (!std::holds_alternative<std::string>(v)) {
    return Cmp3::Mismatch;
  }
  // SAFETY: id()==STRING so chunk(0) is a StringArray (single-chunk post-combine).
  const auto& arr = static_cast<const arrow::StringArray&>(*col.chunk(0));
  const std::string_view lit = std::get<std::string>(v);
  for (int64_t i = 0; i < arr.length(); ++i) {
    if (!keep[static_cast<usize>(i)]) { continue; }
    if (arr.IsNull(i)) {
      keep[static_cast<usize>(i)] = false;
      continue;
    }
    const std::string_view cell = arr.GetView(i);
    keep[static_cast<usize>(i)] = fold_order(p.op, order_sign(cell, lit)) == Cmp3::True;
  }
  return Cmp3::True;
}

// Evaluates `p` against a TIMESTAMP column. Comparable against a Timestamp
// literal (normalised to ns) or a bare i64 literal (interpreted as raw
// nanoseconds); both compare against the array's value normalised to ns. The
// Scalar(Timestamp) path is the intended one.
[[nodiscard]] static Cmp3 eval_timestamp_column(const arrow::ChunkedArray& col,
                                                const Predicate& p,
                                                std::vector<bool>& keep) {
  const Scalar::Storage& v = p.value.value();
  int64_t lit_ns = 0;
  if (std::holds_alternative<time::Timestamp>(v)) {
    lit_ns = std::get<time::Timestamp>(v).unix_nanos();
  } else if (std::holds_alternative<i64>(v)) {
    lit_ns = static_cast<int64_t>(std::get<i64>(v));
  } else {
    return Cmp3::Mismatch;
  }
  // SAFETY: id()==TIMESTAMP (matched by the caller's dispatch) guarantees the
  // dynamic type of col.type() is arrow::TimestampType, so this downcast is
  // well-defined; unit() is then valid.
  const auto unit =
      std::static_pointer_cast<arrow::TimestampType>(col.type())->unit();
  // SAFETY: id()==TIMESTAMP so chunk(0) is a TimestampArray (single-chunk).
  const auto& arr = static_cast<const arrow::TimestampArray&>(*col.chunk(0));
  for (int64_t i = 0; i < arr.length(); ++i) {
    if (!keep[static_cast<usize>(i)]) { continue; }
    if (arr.IsNull(i)) {
      keep[static_cast<usize>(i)] = false;
      continue;
    }
    const int64_t cell_ns = to_timestamp(arr.Value(i), unit).unix_nanos();
    keep[static_cast<usize>(i)] = fold_order(p.op, order_sign(cell_ns, lit_ns)) == Cmp3::True;
  }
  return Cmp3::True;
}

// Dispatches predicate evaluation across the supported column Arrow types,
// ANDing the per-row result into `keep`. Unsupported column type or a
// non-comparable literal -> InvalidArgument. Null-ops (IsNull/IsNotNull) are
// handled here generically via the array validity bitmap.
[[nodiscard]] static Result<std::monostate>
eval_predicate(const arrow::ChunkedArray& col, const Predicate& p,
               std::vector<bool>& keep) {
  if (p.op == Compare::IsNull || p.op == Compare::IsNotNull) {
    const auto& arr = *col.chunk(0);
    const bool want_null = (p.op == Compare::IsNull);
    for (int64_t i = 0; i < arr.length(); ++i) {
      if (!keep[static_cast<usize>(i)]) { continue; }
      keep[static_cast<usize>(i)] = (arr.IsNull(i) == want_null);
    }
    return Ok(std::monostate{});
  }
  Cmp3 r = Cmp3::Mismatch;
  switch (col.type()->id()) {
  case arrow::Type::INT8:   { r = eval_numeric_column<arrow::Int8Array>(col, p, keep);   break; }
  case arrow::Type::INT16:  { r = eval_numeric_column<arrow::Int16Array>(col, p, keep);  break; }
  case arrow::Type::INT32:  { r = eval_numeric_column<arrow::Int32Array>(col, p, keep);  break; }
  case arrow::Type::INT64:  { r = eval_numeric_column<arrow::Int64Array>(col, p, keep);  break; }
  case arrow::Type::UINT8:  { r = eval_numeric_column<arrow::UInt8Array>(col, p, keep);  break; }
  case arrow::Type::UINT16: { r = eval_numeric_column<arrow::UInt16Array>(col, p, keep); break; }
  case arrow::Type::UINT32: { r = eval_numeric_column<arrow::UInt32Array>(col, p, keep); break; }
  case arrow::Type::UINT64: { r = eval_numeric_column<arrow::UInt64Array>(col, p, keep); break; }
  case arrow::Type::FLOAT:  { r = eval_numeric_column<arrow::FloatArray>(col, p, keep);    break; }
  case arrow::Type::DOUBLE: { r = eval_numeric_column<arrow::DoubleArray>(col, p, keep);   break; }
  case arrow::Type::BOOL:   { r = eval_bool_column(col, p, keep);      break; }
  case arrow::Type::STRING: { r = eval_string_column(col, p, keep);    break; }
  case arrow::Type::TIMESTAMP: { r = eval_timestamp_column(col, p, keep); break; }
  default:
    return Err(ErrorCode::InvalidArgument,
               "filter: unsupported column type for predicate on '" + p.column + "'");
  }
  if (r == Cmp3::Mismatch) {
    return Err(ErrorCode::InvalidArgument,
               "filter: literal not comparable to column type");
  }
  return Ok(std::monostate{});
}

// Slices `arr` to the kept rows described by `runs` (contiguous [offset,len)
// ranges) and concatenates them into one array. Empty runs -> a length-0 slice
// so the column still appears with zero rows.
[[nodiscard]] static Result<std::shared_ptr<arrow::Array>>
take_runs(const std::shared_ptr<arrow::Array>& arr,
          const std::vector<std::pair<int64_t, int64_t>>& runs) {
  if (runs.empty()) {
    return Ok(arr->Slice(0, 0));
  }
  arrow::ArrayVector parts;
  parts.reserve(runs.size());
  for (const auto& [off, len] : runs) {
    parts.push_back(arr->Slice(off, len));
  }
  auto cat = arrow::Concatenate(parts, arrow::default_memory_pool());
  if (!cat.ok()) {
    return Err(from_arrow(cat.status(), "filter concatenate"));
  }
  return Ok(*std::move(cat));
}

// Applies the ANDed `preds` to `table`, returning the row-filtered table (Polars
// semantics: rows where every predicate is true are kept; false/null drop). An
// empty predicate list is a no-op. Implemented kernel-free (see file header).
[[nodiscard]] static Result<std::shared_ptr<arrow::Table>>
apply_filter(const std::shared_ptr<arrow::Table>& table,
             const std::vector<Predicate>& preds) {
  if (preds.empty()) {
    return Ok(table);
  }
  const int64_t n = table->num_rows();
  if (n == 0) {
    // No rows: skip every evaluator + the rebuild. This also guards the
    // unconditional col.chunk(0) accesses in the evaluators, which would deref
    // an empty chunk vector (UB) on a 0-row column.
    return Ok(table);
  }
  std::vector<bool> keep(static_cast<usize>(n), true);
  for (const auto& p : preds) {
    const int idx = table->schema()->GetFieldIndex(p.column);
    if (idx < 0) {
      return Err(ErrorCode::InvalidArgument, "filter: unknown column '" + p.column + "'");
    }
    auto ev = eval_predicate(*table->column(idx), p, keep);
    if (!ev.has_value()) {
      return Err(ev.error());
    }
  }
  // Collapse the row mask into maximal contiguous kept runs.
  std::vector<std::pair<int64_t, int64_t>> runs;
  for (int64_t i = 0; i < n;) {
    if (!keep[static_cast<usize>(i)]) { ++i; continue; }
    const int64_t start = i;
    while (i < n && keep[static_cast<usize>(i)]) { ++i; }
    runs.emplace_back(start, i - start);
  }
  // Rebuild each column by slicing + concatenating its kept runs (single-chunk
  // after CombineChunks, so chunk(0) is the whole column).
  arrow::ChunkedArrayVector out_cols;
  out_cols.reserve(static_cast<usize>(table->num_columns()));
  for (int c = 0; c < table->num_columns(); ++c) {
    auto taken = take_runs(table->column(c)->chunk(0), runs);
    if (!taken.has_value()) {
      return Err(taken.error());
    }
    out_cols.push_back(std::make_shared<arrow::ChunkedArray>(*std::move(taken)));
  }
  return Ok(arrow::Table::Make(table->schema(), out_cols));
}

// ---------------------------------------------------------------------------
// Row-group pruning (Task 8): decide which row groups CAN contain a matching
// row from the Parquet column-chunk statistics (min/max/null_count), and skip
// the rest. This is an OPTIMIZATION ONLY -- the exact apply_filter() still runs
// on the survivors, so correctness never depends on pruning. The cardinal rule
// is CONSERVATIVE: any uncertainty (missing stats, non-numeric type, type
// mismatch, NaN/overflow corner) -> KEEP the group (never skip a possible match).
//
// Pruning happens ONLY in an EXACT comparison domain, never via a lossy promotion:
// signed-int columns compare in int64, float columns in double (and an i64 literal
// only enters the double domain when |v| <= 2^53). Unsigned-int columns are not
// pruned at all (their physical INT64 stats mis-sign values >= 2^63). This avoids
// the two precision/signedness hazards that could otherwise SKIP matching rows --
// silent data loss the exact filter could not recover, since a skipped group is
// never read.
// ---------------------------------------------------------------------------

// Converts a predicate literal to an EXACT int64, for pruning signed-integer
// columns. Only integer-domain literals qualify: i64 verbatim, bool -> 0/1. f64
// and non-numeric literals return nullopt (caller keeps the group) -- promoting a
// double into the int domain (or vice versa) is not exact, so it is never pruned.
[[nodiscard]] static std::optional<i64> int_literal(const Scalar& s) noexcept {
  const Scalar::Storage& v = s.value();
  if (std::holds_alternative<i64>(v)) {
    return std::get<i64>(v);
  }
  if (std::holds_alternative<bool>(v)) {
    return std::get<bool>(v) ? i64{1} : i64{0};
  }
  return std::nullopt; // f64 / string / timestamp / monostate: not int-exact
}

// Converts a predicate literal to an EXACT double, for pruning float/double
// columns. f64 is exact; an i64 is exact only when |v| <= 2^53 (every integer up
// to 2^53 is representable in double). i64 with |v| > 2^53, NaN, and non-numeric
// literals return nullopt (caller keeps the group). bool -> 0.0/1.0.
[[nodiscard]] static std::optional<double> double_literal(const Scalar& s) noexcept {
  const Scalar::Storage& v = s.value();
  if (std::holds_alternative<f64>(v)) {
    const double d = std::get<f64>(v);
    return std::isnan(d) ? std::nullopt : std::optional<double>{d};
  }
  if (std::holds_alternative<bool>(v)) {
    return std::get<bool>(v) ? 1.0 : 0.0;
  }
  if (std::holds_alternative<i64>(v)) {
    constexpr i64 kExact = i64{1} << 53; // 2^53: max exactly-representable integer
    const i64 n = std::get<i64>(v);
    if (n > kExact || n < -kExact) {
      return std::nullopt; // |v| > 2^53: not double-exact -> keep group
    }
    return static_cast<double>(n);
  }
  return std::nullopt; // string / timestamp / monostate: not numerically prunable
}

// Given a [min,max] interval and the predicate's literal `v`, returns true iff the
// interval COULD contain a row satisfying `op` (i.e. keep). Skips only when `op`
// is provably unsatisfiable across the whole interval. Evaluated in the EXACT
// domain T (int64_t for signed-int columns, double for float columns) so no
// precision is lost; the comparisons mirror the exact filter's fold_order.
template <class T>
[[nodiscard]] static bool interval_may_match(Compare op, T mn, T mx, T v) noexcept {
  switch (op) {
  case Compare::Ge: return mx >= v;            // some cell >= v
  case Compare::Gt: return mx > v;             // some cell  > v
  case Compare::Le: return mn <= v;            // some cell <= v
  case Compare::Lt: return mn < v;             // some cell  < v
  case Compare::Eq: return mn <= v && v <= mx; // v lies inside [min,max]
  case Compare::Ne: return !(mn == mx && mn == v); // skip iff whole group == v
  case Compare::IsNull:
  case Compare::IsNotNull:
    break;
  }
  return true; // null-ops handled before interval reasoning; keep defensively
}

// Prunes a signed-integer column-chunk (INT8/16/32/64 -> physical INT32/INT64)
// against `p`, in the exact int64 domain. Returns keep/skip. Non-integer literal
// or an unexpected physical type -> keep (conservative). Requires HasMinMax().
[[nodiscard]] static bool int_group_may_match(const std::shared_ptr<parquet::Statistics>& stats,
                                              const Predicate& p) {
  const std::optional<i64> lit = int_literal(p.value);
  if (!lit.has_value()) {
    return true; // f64 / non-numeric literal vs int column: not int-exact -> keep
  }
  i64 mn = 0;
  i64 mx = 0;
  switch (stats->physical_type()) {
  case parquet::Type::INT32: {
    // SAFETY: physical_type()==INT32 -> dynamic type TypedStatistics<Int32Type>.
    const auto t = std::static_pointer_cast<parquet::Int32Statistics>(stats);
    mn = static_cast<i64>(t->min()); // int32 widens to int64 losslessly
    mx = static_cast<i64>(t->max());
    break;
  }
  case parquet::Type::INT64: {
    // SAFETY: physical_type()==INT64 -> dynamic type TypedStatistics<Int64Type>.
    const auto t = std::static_pointer_cast<parquet::Int64Statistics>(stats);
    mn = t->min();
    mx = t->max();
    break;
  }
  default:
    return true; // unexpected physical type for a signed-int column -> keep
  }
  return interval_may_match<i64>(p.op, mn, mx, *lit);
}

// Prunes a float/double column-chunk (physical FLOAT/DOUBLE) against `p`, in the
// double domain. Returns keep/skip. Non-double-exact literal, NaN bound, or an
// unexpected physical type -> keep (conservative). Requires HasMinMax().
[[nodiscard]] static bool float_group_may_match(const std::shared_ptr<parquet::Statistics>& stats,
                                                const Predicate& p) {
  const std::optional<double> lit = double_literal(p.value);
  if (!lit.has_value()) {
    return true; // |i64|>2^53 / NaN / non-numeric literal -> not double-exact -> keep
  }
  double mn = 0.0;
  double mx = 0.0;
  switch (stats->physical_type()) {
  case parquet::Type::FLOAT: {
    // SAFETY: physical_type()==FLOAT -> dynamic type TypedStatistics<FloatType>.
    const auto t = std::static_pointer_cast<parquet::FloatStatistics>(stats);
    mn = static_cast<double>(t->min()); // float widens to double losslessly
    mx = static_cast<double>(t->max());
    break;
  }
  case parquet::Type::DOUBLE: {
    // SAFETY: physical_type()==DOUBLE -> dynamic type TypedStatistics<DoubleType>.
    const auto t = std::static_pointer_cast<parquet::DoubleStatistics>(stats);
    mn = t->min();
    mx = t->max();
    break;
  }
  default:
    return true; // unexpected physical type for a float column -> keep
  }
  if (std::isnan(mn) || std::isnan(mx)) {
    return true; // a NaN bound makes interval reasoning unsafe -> keep
  }
  return interval_may_match<double>(p.op, mn, mx, *lit);
}

// Returns true if row group `rg` COULD contain a row satisfying `p` (i.e. it
// cannot be safely skipped). CONSERVATIVE: any uncertainty -> true (keep). Pruning
// happens ONLY in an exact comparison domain chosen from the ARROW field type:
// signed-int columns prune in int64, float columns in double. Unsigned-int columns
// (UINT8..UINT64) are NEVER pruned -- they are stored as physical INT64, so values
// >= 2^63 would read back negative and could wrongly skip matches; KEEP avoids that
// hazard entirely. Everything else (bool/string/timestamp, missing/absent stats,
// unknown column) also keeps. The exact filter still runs on survivors.
[[nodiscard]] static bool group_may_match(const parquet::RowGroupMetaData& rg,
                                           const arrow::Schema& schema,
                                           const Predicate& p) {
  const int leaf = schema.GetFieldIndex(p.column);
  if (leaf < 0) {
    return true; // unknown column: apply_filter reports InvalidArgument; never prune
  }
  // SAFETY: leaf in [0, num_columns) by construction (GetFieldIndex succeeded),
  // so ColumnChunk(leaf) is in range for this row group.
  auto cc = rg.ColumnChunk(leaf);
  if (!cc->is_stats_set()) {
    return true; // no stats written: cannot prove absence -> keep
  }
  auto stats = cc->statistics();
  if (p.op == Compare::IsNull) {
    // Matchable iff the chunk holds at least one null. null_count() is only
    // meaningful when min/max stats exist alongside it; when unsure, keep.
    return stats->HasMinMax() ? (stats->null_count() > 0) : true;
  }
  if (p.op == Compare::IsNotNull) {
    // Matchable iff some non-null present. num_values() counts non-null values;
    // when it is meaningful and zero, the whole chunk is null -> skip.
    return stats->HasMinMax() ? (stats->num_values() > 0) : true;
  }
  if (!stats->HasMinMax()) {
    return true; // ordering/equality needs a [min,max]; absent -> keep
  }
  // Choose the prune domain from the ARROW field type at the predicate's leaf.
  switch (schema.field(leaf)->type()->id()) {
  case arrow::Type::INT8:
  case arrow::Type::INT16:
  case arrow::Type::INT32:
  case arrow::Type::INT64:
    return int_group_may_match(stats, p);
  case arrow::Type::FLOAT:
  case arrow::Type::DOUBLE:
    return float_group_may_match(stats, p);
  case arrow::Type::UINT8:
  case arrow::Type::UINT16:
  case arrow::Type::UINT32:
  case arrow::Type::UINT64:
    // Unsigned stored as physical INT64; reading Int64Statistics would mis-sign
    // values >= 2^63 and could skip matches. KEEP (no unsigned pruning for now).
    return true;
  default:
    return true; // bool / string / timestamp / decimal / etc.: not pruned here
  }
}

// Indices of row groups that survive pruning (could contain a match). A group is
// skipped only if SOME predicate proves it cannot match; with no predicates every
// group survives. Order is ascending, matching meta's row-group order.
[[nodiscard]] static std::vector<int>
surviving_row_groups(const parquet::FileMetaData& meta, const arrow::Schema& schema,
                     const std::vector<Predicate>& preds) {
  const int total = meta.num_row_groups();
  std::vector<int> keep;
  keep.reserve(static_cast<usize>(total));
  for (int g = 0; g < total; ++g) {
    auto rg = meta.RowGroup(g);
    bool may = true;
    for (const auto& p : preds) {
      if (!group_may_match(*rg, schema, p)) {
        may = false;
        break;
      }
    }
    if (may) {
      keep.push_back(g);
    }
  }
  return keep;
}

// Reads exactly the surviving row groups `keep` (column-projected by `indices`,
// or all columns when `indices` is empty) and combines chunks so every column is
// a single contiguous chunk. An empty `keep` yields a 0-row table with the
// projected schema (built via Table::MakeEmpty) without calling ReadRowGroups on
// an empty list, which Arrow rejects.
[[nodiscard]] static Result<std::shared_ptr<arrow::Table>>
read_table_pruned(parquet::arrow::FileReader& reader, const std::vector<int>& keep,
                  const std::vector<int>& indices) {
  if (keep.empty()) {
    // No surviving groups: synthesize a 0-row table with the projected schema.
    std::shared_ptr<arrow::Schema> aschema;
    const auto st = reader.GetSchema(&aschema);
    if (!st.ok()) {
      return Err(from_arrow(st, "collect get schema"));
    }
    std::shared_ptr<arrow::Schema> proj = aschema;
    if (!indices.empty()) {
      // Restrict the schema to the projected leaf columns (flat schema: arrow
      // field index == leaf index, the same mapping resolve_indices relies on).
      arrow::FieldVector fields;
      fields.reserve(indices.size());
      for (const int i : indices) {
        fields.push_back(aschema->field(i));
      }
      proj = arrow::schema(std::move(fields));
    }
    auto empty = arrow::Table::MakeEmpty(proj, arrow::default_memory_pool());
    if (!empty.ok()) {
      return Err(from_arrow(empty.status(), "collect make empty"));
    }
    return Ok(*std::move(empty));
  }
  // Arrow 24: arrow::Result-returning ReadRowGroups; the out-param forms are
  // ARROW_DEPRECATED and would trip /WX. Empty `indices` -> all-columns overload.
  std::shared_ptr<arrow::Table> table;
  if (indices.empty()) {
    auto res = reader.ReadRowGroups(keep);
    if (!res.ok()) {
      return Err(from_arrow(res.status(), "collect read row groups"));
    }
    table = *std::move(res);
  } else {
    auto res = reader.ReadRowGroups(keep, indices);
    if (!res.ok()) {
      return Err(from_arrow(res.status(), "collect read row groups projected"));
    }
    table = *std::move(res);
  }
  // Combine chunks so every column is single-chunk and owned by the table (keeps
  // column_view's raw_values() alias alive across multi-row-group reads).
  auto combined = table->CombineChunks(arrow::default_memory_pool());
  if (!combined.ok()) {
    return Err(from_arrow(combined.status(), "collect combine chunks"));
  }
  return Ok(*std::move(combined));
}

// Computes the column set to READ for a collect(): the union of the projection
// and every predicate column, deduplicated with a stable order (projection
// first, then any predicate-only helper columns). An empty projection means
// "read all columns", so the union is empty too (signals read-all downstream).
[[nodiscard]] static std::vector<std::string>
read_columns_for(const std::vector<std::string>& projection,
                 const std::vector<Predicate>& predicates) {
  if (projection.empty()) {
    return {}; // read-all; predicate columns are necessarily present
  }
  std::vector<std::string> cols = projection;
  for (const auto& p : predicates) {
    if (std::find(cols.begin(), cols.end(), p.column) == cols.end()) {
      cols.push_back(p.column);
    }
  }
  return cols;
}

// Projects `table` down to exactly `projection` (in projection order), dropping
// predicate-only helper columns read solely for filtering. Unknown name (cannot
// happen post-read) -> InvalidArgument; Arrow failures map via from_arrow.
[[nodiscard]] static Result<std::shared_ptr<arrow::Table>>
project_down(const std::shared_ptr<arrow::Table>& table,
             const std::vector<std::string>& projection) {
  auto idx = resolve_indices(*table->schema(), projection);
  if (!idx.has_value()) {
    return Err(idx.error());
  }
  auto sel = table->SelectColumns(*idx);
  if (!sel.ok()) {
    return Err(from_arrow(sel.status(), "collect select columns"));
  }
  return Ok(*std::move(sel));
}

// Plans and reads the surviving row groups for `read_cols` (empty = all columns):
// resolves the read columns to leaf indices, prunes row groups via column stats,
// reads only the survivors, and reports how many groups were pruned via `pruned`.
// Correctness is unaffected -- the exact apply_filter still runs on the result.
[[nodiscard]] static Result<std::shared_ptr<arrow::Table>>
read_pruned_plan(parquet::arrow::FileReader& reader, const parquet::FileMetaData& meta,
                 const std::vector<std::string>& read_cols,
                 const std::vector<Predicate>& predicates, i64& pruned) {
  std::shared_ptr<arrow::Schema> aschema;
  const auto st = reader.GetSchema(&aschema);
  if (!st.ok()) {
    return Err(from_arrow(st, "collect get schema"));
  }
  std::vector<int> indices; // empty -> read-all-columns path
  if (!read_cols.empty()) {
    auto idx = resolve_indices(*aschema, read_cols);
    if (!idx.has_value()) {
      return Err(idx.error());
    }
    indices = *std::move(idx);
  }
  const std::vector<int> keep = surviving_row_groups(meta, *aschema, predicates);
  pruned = static_cast<i64>(meta.num_row_groups()) - static_cast<i64>(keep.size());
  return read_table_pruned(reader, keep, indices);
}

// Applies offset then limit to `table`. offset<0 treated as 0; limit<0 means "all".
// Arrow's Table::Slice is zero-copy but clamps out-of-range args; we clamp ourselves
// for clarity and to avoid relying on Arrow's internal clamping contract.
[[nodiscard]] static std::shared_ptr<arrow::Table>
apply_slice(const std::shared_ptr<arrow::Table>& table, i64 offset, i64 limit) noexcept {
  const int64_t n = table->num_rows();
  const int64_t off = std::min<int64_t>(offset < 0 ? 0 : offset, n);
  int64_t len = n - off;
  if (limit >= 0) {
    len = std::min<int64_t>(len, limit);
  }
  return table->Slice(off, len);
}

// Eager read: materialize the SURVIVING row groups into a ParquetTable, applying
// the projection (select) and any filter predicates (exact, row-level). Row
// groups that provably cannot match (column-stats pruning) are skipped before
// reading; the exact filter still runs on survivors so the result is identical to
// reading everything. Offset/limit slicing is applied after filtering/projection.
Result<ParquetTable> LazyParquet::collect() {
  try {
    const std::vector<std::string> read_cols =
        read_columns_for(impl_->projection, impl_->predicates);
    i64 pruned = 0;
    auto table = read_pruned_plan(*impl_->reader, *impl_->meta, read_cols,
                                  impl_->predicates, pruned);
    if (!table.has_value()) {
      return Err(table.error());
    }
    auto filtered = apply_filter(*table, impl_->predicates);
    if (!filtered.has_value()) {
      return Err(filtered.error());
    }
    std::shared_ptr<arrow::Table> result = *std::move(filtered);
    // Drop predicate-only helper columns when a projection was set and the read
    // set was widened to a superset of it. (read_all keeps every column.)
    if (!impl_->projection.empty()) {
      auto down = project_down(result, impl_->projection);
      if (!down.has_value()) {
        return Err(down.error());
      }
      result = *std::move(down);
    }
    // Apply offset + limit (slice) after filtering and projection. rows_scanned
    // records how many rows survived the filter (before slicing); num_rows() on
    // the final table reflects the slice.
    impl_->stats.row_groups_total = num_row_groups();
    impl_->stats.row_groups_pruned = pruned;        // groups skipped by stats
    impl_->stats.rows_scanned = result->num_rows(); // rows AFTER filtering
    result = apply_slice(result, impl_->offset, impl_->limit);
    auto out = std::make_unique<ParquetTable::Impl>();
    out->table = std::move(result);
    // schema_from_arrow reflects the final (projected) columns, keeping
    // ParquetTable::schema() consistent.
    out->schema = schema_from_arrow(*out->table->schema());
    return ParquetTable{std::move(out)};
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "collect: unknown exception"); }
}

// ---------------------------------------------------------------------------
// Numeric column bridges (Arrow contiguous buffer -> span / Column / Frame).
// String/bool/timestamp bridges (Task 4) live further below, after the numeric
// primary templates.
// ---------------------------------------------------------------------------

// Maps an atx numeric element type T to its Arrow Type::id and typed Array.
template <class T> struct ArrowTypeOf;
template <> struct ArrowTypeOf<int64_t>  { static constexpr auto id = arrow::Type::INT64;  using ArrayT = arrow::Int64Array;  };
template <> struct ArrowTypeOf<int32_t>  { static constexpr auto id = arrow::Type::INT32;  using ArrayT = arrow::Int32Array;  };
template <> struct ArrowTypeOf<int16_t>  { static constexpr auto id = arrow::Type::INT16;  using ArrayT = arrow::Int16Array;  };
template <> struct ArrowTypeOf<int8_t>   { static constexpr auto id = arrow::Type::INT8;   using ArrayT = arrow::Int8Array;   };
template <> struct ArrowTypeOf<uint64_t> { static constexpr auto id = arrow::Type::UINT64; using ArrayT = arrow::UInt64Array; };
template <> struct ArrowTypeOf<uint32_t> { static constexpr auto id = arrow::Type::UINT32; using ArrayT = arrow::UInt32Array; };
template <> struct ArrowTypeOf<uint16_t> { static constexpr auto id = arrow::Type::UINT16; using ArrayT = arrow::UInt16Array; };
template <> struct ArrowTypeOf<uint8_t>  { static constexpr auto id = arrow::Type::UINT8;  using ArrayT = arrow::UInt8Array;  };
template <> struct ArrowTypeOf<float>    { static constexpr auto id = arrow::Type::FLOAT;  using ArrayT = arrow::FloatArray;  };
template <> struct ArrowTypeOf<double>   { static constexpr auto id = arrow::Type::DOUBLE; using ArrayT = arrow::DoubleArray; };

template <class T>
Result<std::span<const T>> ParquetTable::column_view(std::string_view name) const {
  const int idx = impl_->table->schema()->GetFieldIndex(std::string{name});
  if (idx < 0) {
    return Err(ErrorCode::InvalidArgument, "column_view: column not found");
  }
  auto col = impl_->table->column(idx);
  if (col->type()->id() != ArrowTypeOf<T>::id) {
    return Err(ErrorCode::InvalidArgument, "column_view: column element-type mismatch");
  }
  if (col->length() == 0 || col->num_chunks() == 0) {
    return Ok(std::span<const T>{}); // empty column: nothing to alias
  }
  if (col->num_chunks() != 1) {
    // Unreachable after CombineChunks; defensive guard against a dangling alias.
    return Err(ErrorCode::Internal, "column_view: column is not single-chunk");
  }
  // SAFETY: chunk(0) is owned by impl_->table (single-chunk after CombineChunks),
  // so raw_values() aliases a contiguous buffer that stays alive as long as *this
  // (and therefore impl_->table) lives. id() was checked, so the static_pointer_cast
  // to ArrayT is valid (no UB).
  auto arr = std::static_pointer_cast<typename ArrowTypeOf<T>::ArrayT>(col->chunk(0));
  const T* p = arr->raw_values();
  return Ok(std::span<const T>{p, static_cast<usize>(arr->length())});
}

template <class T>
Result<series::Column<T>> ParquetTable::to_column(std::string_view name) const {
  auto v = column_view<T>(name);
  if (!v.has_value()) {
    return Err(v.error());
  }
  series::Column<T> out;
  out.append_bulk(*v);
  return Ok(std::move(out));
}

// ---------------------------------------------------------------------------
// String / bool / timestamp bridges (Task 4).
//
// Null handling across these bridges (and the numeric ones above) is a
// deliberate simplification: validity is NOT propagated into Column's bitmap;
// a null string becomes an empty view and a null bool/timestamp becomes the
// underlying stored/default value. (No per-element null mask is exposed yet.)
// ---------------------------------------------------------------------------

// UTF-8 string accessor: returns a vector of views aliasing the array buffer
// owned by impl_->table. Only STRING (utf8) is supported; LARGE_STRING is
// rejected with NotImplemented (the Task-4 fixture uses utf8 -> StringArray,
// and GetView's array cast must match the concrete array type).
Result<std::vector<std::string_view>> ParquetTable::strings(std::string_view name) const {
  try {
    const int idx = impl_->table->schema()->GetFieldIndex(std::string{name});
    if (idx < 0) {
      return Err(ErrorCode::InvalidArgument, "strings: column not found");
    }
    auto col = impl_->table->column(idx);
    const auto id = col->type()->id();
    if (id == arrow::Type::LARGE_STRING) {
      return Err(ErrorCode::NotImplemented, "strings: LARGE_STRING not supported");
    }
    if (id != arrow::Type::STRING) {
      return Err(ErrorCode::InvalidArgument, "strings: column is not UTF-8");
    }
    std::vector<std::string_view> out;
    if (col->length() == 0 || col->num_chunks() == 0) {
      return Ok(std::move(out));
    }
    // SAFETY: single-chunk after CombineChunks; id() checked == STRING, so the
    // chunk's dynamic type is arrow::StringArray. GetView aliases the array
    // buffer owned by impl_->table; each view is valid while *this lives. Null
    // slots -> empty view.
    auto& arr = static_cast<arrow::StringArray&>(*col->chunk(0));
    out.reserve(static_cast<usize>(arr.length()));
    for (int64_t i = 0; i < arr.length(); ++i) {
      const std::string_view v = arr.GetView(i);
      out.emplace_back(v.data(), v.size());
    }
    return Ok(std::move(out));
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "strings: unknown exception"); }
}

// Converts an Arrow timestamp `raw` (in `unit`) to an atx Timestamp (i64 ns).
// NOTE: SECOND/MILLI/MICRO scaling multiplies `raw` into i64 ns and can overflow
// the ~292-year ns domain for extreme values; the from_unix_* factories do not
// guard against this (documented Timestamp precondition).
[[nodiscard]] static atx::core::time::Timestamp to_timestamp(i64 raw,
                                                             arrow::TimeUnit::type unit) {
  using atx::core::time::Timestamp;
  switch (unit) {
  case arrow::TimeUnit::SECOND: return Timestamp::from_unix_seconds(raw);
  case arrow::TimeUnit::MILLI:  return Timestamp::from_unix_millis(raw);
  case arrow::TimeUnit::MICRO:  return Timestamp::from_unix_micros(raw);
  case arrow::TimeUnit::NANO:   return Timestamp::from_unix_nanos(raw);
  }
  return Timestamp::from_unix_nanos(raw); // defensive default
}

// bool is bit-packed in Arrow; it cannot be zero-copy aliased as a contiguous
// bool buffer, so column_view<bool> is unsupported and to_column<bool> copies
// element-by-element via BooleanArray::Value.
template <>
Result<std::span<const bool>> ParquetTable::column_view<bool>(std::string_view name) const {
  // Validate the name first for a clearer error, then refuse (bit-packed).
  const int idx = impl_->table->schema()->GetFieldIndex(std::string{name});
  if (idx < 0) {
    return Err(ErrorCode::InvalidArgument, "column_view<bool>: column not found");
  }
  return Err(ErrorCode::NotImplemented,
             "column_view<bool>: Arrow bool is bit-packed; use to_column<bool>");
}

template <>
Result<series::Column<bool>> ParquetTable::to_column<bool>(std::string_view name) const {
  try {
    const int idx = impl_->table->schema()->GetFieldIndex(std::string{name});
    if (idx < 0) {
      return Err(ErrorCode::InvalidArgument, "to_column<bool>: column not found");
    }
    auto col = impl_->table->column(idx);
    if (col->type()->id() != arrow::Type::BOOL) {
      return Err(ErrorCode::InvalidArgument, "to_column<bool>: column element-type mismatch");
    }
    series::Column<bool> out;
    // CombineChunks (at load) guarantees a single chunk, so this positive guard
    // is sufficient; an empty column has length 0 and yields an empty Column.
    if (col->length() > 0 && col->num_chunks() == 1) {
      // SAFETY: id()==BOOL so chunk(0)'s dynamic type is arrow::BooleanArray.
      auto& arr = static_cast<arrow::BooleanArray&>(*col->chunk(0));
      for (int64_t i = 0; i < arr.length(); ++i) {
        out.append(arr.Value(i)); // null slots read as the stored bit (treated as value)
      }
    }
    return Ok(std::move(out));
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "to_column<bool>: unknown exception"); }
}

// Timestamp requires per-unit conversion (Arrow stores raw int64 in the array's
// unit), so column_view<Timestamp> cannot alias the buffer and is rejected;
// to_column<Timestamp> copies, normalising each value to i64 nanoseconds.
template <>
Result<std::span<const atx::core::time::Timestamp>>
ParquetTable::column_view<atx::core::time::Timestamp>(std::string_view name) const {
  const int idx = impl_->table->schema()->GetFieldIndex(std::string{name});
  if (idx < 0) {
    return Err(ErrorCode::InvalidArgument, "column_view<Timestamp>: column not found");
  }
  return Err(ErrorCode::NotImplemented,
             "column_view<Timestamp>: requires unit conversion; use to_column<Timestamp>");
}

template <>
Result<series::Column<atx::core::time::Timestamp>>
ParquetTable::to_column<atx::core::time::Timestamp>(std::string_view name) const {
  try {
    const int idx = impl_->table->schema()->GetFieldIndex(std::string{name});
    if (idx < 0) {
      return Err(ErrorCode::InvalidArgument, "to_column<Timestamp>: column not found");
    }
    auto col = impl_->table->column(idx);
    if (col->type()->id() != arrow::Type::TIMESTAMP) {
      return Err(ErrorCode::InvalidArgument, "to_column<Timestamp>: column element-type mismatch");
    }
    const auto unit =
        std::static_pointer_cast<arrow::TimestampType>(col->type())->unit();
    series::Column<atx::core::time::Timestamp> out;
    // CombineChunks (at load) guarantees a single chunk, so this positive guard
    // is sufficient; an empty column has length 0 and yields an empty Column.
    if (col->length() > 0 && col->num_chunks() == 1) {
      // SAFETY: id()==TIMESTAMP so chunk(0)'s dynamic type is arrow::TimestampArray.
      auto& arr = static_cast<arrow::TimestampArray&>(*col->chunk(0));
      for (int64_t i = 0; i < arr.length(); ++i) {
        const i64 raw = arr.Value(i);
        out.append(to_timestamp(raw, unit)); // null slots read as the stored value
      }
    }
    return Ok(std::move(out));
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "to_column<Timestamp>: unknown exception"); }
}

// Bridges the numeric column `name` (element type T) into `frame`. On success
// the bridged Column<T> is moved into a freshly added frame column. Returns true
// iff the column landed; a failed bridge (e.g. all-null edge) returns false so
// to_frame can skip it silently.
template <class T>
[[nodiscard]] static bool add_numeric(series::Frame& frame, std::string_view name,
                                      const ParquetTable& table) {
  auto col = table.to_column<T>(name);
  if (!col.has_value()) {
    return false;
  }
  auto ref = frame.add_column<T>(name);
  if (!ref.has_value()) {
    return false;
  }
  ref->get() = *std::move(col);
  return true;
}

Result<series::Frame> ParquetTable::to_frame() const {
  try {
    series::Frame frame;
    for (const auto& ci : impl_->schema.columns) {
      switch (ci.dtype) {
      case DType::Int8:    { (void)add_numeric<int8_t>(frame, ci.name, *this);   break; }
      case DType::Int16:   { (void)add_numeric<int16_t>(frame, ci.name, *this);  break; }
      case DType::Int32:   { (void)add_numeric<int32_t>(frame, ci.name, *this);  break; }
      case DType::Int64:   { (void)add_numeric<int64_t>(frame, ci.name, *this);  break; }
      case DType::UInt8:   { (void)add_numeric<uint8_t>(frame, ci.name, *this);  break; }
      case DType::UInt16:  { (void)add_numeric<uint16_t>(frame, ci.name, *this); break; }
      case DType::UInt32:  { (void)add_numeric<uint32_t>(frame, ci.name, *this); break; }
      case DType::UInt64:  { (void)add_numeric<uint64_t>(frame, ci.name, *this); break; }
      case DType::Float32: { (void)add_numeric<float>(frame, ci.name, *this);    break; }
      case DType::Float64: { (void)add_numeric<double>(frame, ci.name, *this);   break; }
      default:
        // Bool/String/Binary/Date32/Timestamp/Decimal128/Unsupported land in
        // Task 4+; silently skipped here so the numeric arms stay total.
        break;
      }
    }
    return Ok(std::move(frame));
  }
  catch (const std::bad_alloc&) { throw; }
  catch (const std::exception& e) { return Err(ErrorCode::Internal, e.what()); }
  catch (...) { return Err(ErrorCode::Internal, "to_frame: unknown exception"); }
}

// Explicit instantiations so the header's out-of-line template definitions link
// for every supported numeric element type.
// T is a TYPE used in template-arg position; wrapping it in parens (the lint's
// usual fix) would be invalid C++ here, so the check is suppressed.
// NOLINTNEXTLINE(bugprone-macro-parentheses)
#define ATX_PQ_INSTANTIATE(T) \
  template Result<std::span<const T>> ParquetTable::column_view<T>(std::string_view) const; \
  template Result<series::Column<T>> ParquetTable::to_column<T>(std::string_view) const;
ATX_PQ_INSTANTIATE(int8_t)
ATX_PQ_INSTANTIATE(int16_t)
ATX_PQ_INSTANTIATE(int32_t)
ATX_PQ_INSTANTIATE(int64_t)
ATX_PQ_INSTANTIATE(uint8_t)
ATX_PQ_INSTANTIATE(uint16_t)
ATX_PQ_INSTANTIATE(uint32_t)
ATX_PQ_INSTANTIATE(uint64_t)
ATX_PQ_INSTANTIATE(float)
ATX_PQ_INSTANTIATE(double)
#undef ATX_PQ_INSTANTIATE

} // namespace atx::core::io
