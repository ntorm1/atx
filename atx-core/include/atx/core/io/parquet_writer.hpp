#pragma once

// atx::core::io — Parquet WRITE surface (companion to the read-side parquet.hpp).
// Arrow types stay in parquet_writer.cpp; this header includes no Arrow. Columns
// are borrowed spans (must outlive the call) and must all share the same length.

#include <memory>
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

// Compression codec applied to the whole file (Arrow mapping lives in the .cpp).
enum class Compression { None, Snappy, Zstd };

// Writer tuning. Defaults to ZSTD + dictionary on (callers that omit this pick
// up compressed output automatically).
struct WriteOptions {
  Compression compression{Compression::Zstd};
  bool dictionary{true};
};

// Write all columns to one Parquet file at `path` (parent dirs created).
[[nodiscard]] Status write_parquet(std::span<const WriteColumn> cols, std::string_view path,
                                   WriteOptions opts = {});

// Hive-partition by string column `partition_col`: bucket rows by its distinct
// values, DROP that column from each file (path-encoded), and write one file per
// bucket at <root>/<partition_col>=<value>/data.parquet. Returns partitions
// written. `partition_col` must name a std::string column in `cols`.
[[nodiscard]] Result<i64> write_hive_parquet(std::span<const WriteColumn> cols,
                                             std::string_view root, std::string_view partition_col,
                                             WriteOptions opts = {});

// Incremental Parquet writer: ONE row group per `write_row_group` call.
//
// ADDITIVE companion to `write_parquet`, whose behaviour is unchanged. That form
// materialises the entire table before it emits a byte, so its peak memory is
// the DATASET; this one's is the CHUNK the caller hands over, which is what lets
// a producer that cannot hold its own output still write it. Chunking on a
// meaningful key (one row group per symbol) is the second reason to reach for
// it: the group's own min/max statistics then let a downstream reader prune to
// the symbols it wants without decoding the rest.
//
// Contract:
//   * `open` fixes the file schema from `schema_cols` — names, order and
//     physical types. The spans' LENGTHS are ignored, so a set of empty columns
//     is the natural way to declare a schema. Opening twice is an error; the
//     parent directories of `path` are created.
//   * `write_row_group` requires exactly that schema and one common row count
//     across the chunk's columns. A ZERO-ROW chunk is accepted and writes
//     nothing: an empty row group carries no rows and no usable statistics, so
//     emitting one would only make the file's group count lie about its content.
//     The chunk is BORROWED for the call and is not referenced afterwards.
//   * `close` writes the footer and closes the file. Until it returns the file
//     on disk is INCOMPLETE (no footer), so a reader must not be pointed at it.
//     Calling `close` on a writer that is not open succeeds and does nothing.
//   * the destructor (and move-assignment onto an open writer) closes it and
//     DISCARDS the status — a caller that needs to know the file landed must
//     call `close()` itself. Neither ever abandons an open writer: a file with
//     no footer is one no reader can open.
//   * one writer at a time; NOT thread-safe. A caller with concurrent producers
//     must funnel them through a single writer, which is also what makes the
//     row-group order (and so the bytes) independent of how many producers ran.
//
// Rule of Five: move-only — an open file has exactly one owner.
class ParquetRowGroupWriter {
public:
  ParquetRowGroupWriter() noexcept;
  ~ParquetRowGroupWriter();
  ParquetRowGroupWriter(ParquetRowGroupWriter &&) noexcept;
  ParquetRowGroupWriter &operator=(ParquetRowGroupWriter &&) noexcept;
  ParquetRowGroupWriter(const ParquetRowGroupWriter &) = delete;
  ParquetRowGroupWriter &operator=(const ParquetRowGroupWriter &) = delete;

  [[nodiscard]] Status open(std::span<const WriteColumn> schema_cols, std::string_view path,
                            WriteOptions opts = {});
  [[nodiscard]] Status write_row_group(std::span<const WriteColumn> cols);
  [[nodiscard]] Status close();

  [[nodiscard]] bool is_open() const noexcept;
  // Row groups actually emitted (zero-row chunks are not counted).
  [[nodiscard]] i64 row_groups_written() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::core::io
