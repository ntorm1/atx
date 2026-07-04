#pragma once

// atx::engine::data::DiskStore — date-partitioned (hive) Parquet stores on disk.
// Layout: <root>/<STORE>/date=YYYY-MM-DD/data.parquet. The `date` value is encoded
// in the path and is NOT a column inside the file.

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/core/error.hpp"             // Result
#include "atx/core/io/parquet.hpp"        // LazyParquet
#include "atx/core/io/parquet_writer.hpp" // WriteColumn

namespace atx::engine::data {

using atx::core::Result;

enum class Store { Ohlc1D, Ohlc1M, OpraBbo, OChain };

// Subdir name for a store (e.g. Store::Ohlc1D -> "OHLC1D").
[[nodiscard]] std::string_view store_name(Store s) noexcept;

class DiskStore {
public:
  // Create <root> and all four store subdirs if absent.
  [[nodiscard]] static Result<DiskStore> open(std::filesystem::path root);

  [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }
  [[nodiscard]] std::filesystem::path store_dir(Store s) const;
  // <root>/<STORE>/date=<date>/data.parquet
  [[nodiscard]] std::filesystem::path partition_path(Store s, std::string_view date) const;

  // Write one date partition. `cols` must NOT contain a `date` column.
  [[nodiscard]] Result<void> write_partition(Store s, std::string_view date,
                                             std::span<const core::io::WriteColumn> cols) const;

  // Ascending list of partition dates ("YYYY-MM-DD") present in a store.
  [[nodiscard]] Result<std::vector<std::string>> list_dates(Store s) const;

  // Lazy scan of a single partition file (Err if absent).
  [[nodiscard]] Result<core::io::LazyParquet> scan_partition(Store s, std::string_view date) const;

private:
  explicit DiskStore(std::filesystem::path root) : root_{std::move(root)} {}
  std::filesystem::path root_;
};

} // namespace atx::engine::data
