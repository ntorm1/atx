#include "atx/vol/research/track_compact_reconcile.hpp"

// Third-party Arrow/Parquet headers -- confined to this .cpp, same firewall
// discipline as track_store.cpp (see that file's own doc comment). This is a
// SEPARATE translation unit from track_store.cpp specifically because it
// needs BOTH Arrow/Parquet reading AND catalog.hpp -- track_store.hpp is
// deliberately one-directional and must not gain a Catalog dependency (see
// catalog.hpp's own doc comment on reused types), so this coordination logic
// cannot live there.
#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "atx/vol/research/track_key.hpp" // track_key_from_hex

namespace atx::vol {

using atx::core::Err;
using atx::core::ErrorCode;
using atx::core::Ok;

namespace {

namespace fs = std::filesystem;

[[nodiscard]] Error from_arrow(const arrow::Status &s, std::string_view ctx) {
  std::string msg{ctx};
  msg += ": ";
  msg += s.ToString();
  return Error{ErrorCode::IoError, std::move(msg)};
}

// Every "*.parquet" file directly under `partition_dir`, sorted by full path
// (== numeric batch order, given format_batch_name's fixed 6-digit
// zero-padded width, track_store.cpp) so a re-run scans in a stable,
// reproducible order. Empty (not an error) if the partition directory does
// not exist -- a stuck row whose OWN partition was never compacted at all is
// simply not found by the caller's scan, which reports that as Err(NotFound)
// itself.
[[nodiscard]] Result<std::vector<fs::path>> list_batch_files(const fs::path &partition_dir) {
  std::vector<fs::path> files;
  std::error_code exists_ec;
  if (!fs::exists(partition_dir, exists_ec)) {
    return Ok(files);
  }
  std::error_code list_ec;
  fs::directory_iterator it(partition_dir, list_ec);
  if (list_ec) {
    return Err(ErrorCode::IoError, "reconcile_stuck_compactions: cannot list " +
                                       partition_dir.string() + ": " + list_ec.message());
  }
  const fs::directory_iterator dir_end;
  for (; it != dir_end; it.increment(list_ec)) {
    if (list_ec) {
      return Err(ErrorCode::IoError,
                 "reconcile_stuck_compactions: directory iteration failed: " + list_ec.message());
    }
    std::error_code type_ec;
    if (it->is_regular_file(type_ec) && it->path().extension() == ".parquet") {
      files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  return Ok(std::move(files));
}

// True iff `path`'s `track_key` column contains a row exactly equal to
// `target_hex`. Reads the WHOLE file (see the header's doc comment on why
// that is the deliberate, unconditionally-correct choice here).
[[nodiscard]] Result<bool> batch_contains_track(const fs::path &path, const std::string &target_hex) {
  auto in = arrow::io::ReadableFile::Open(path.string());
  if (!in.ok()) {
    return Err(from_arrow(in.status(), "reconcile_stuck_compactions: open " + path.string()));
  }
  auto reader_res = parquet::arrow::OpenFile(*in, arrow::default_memory_pool());
  if (!reader_res.ok()) {
    return Err(
        from_arrow(reader_res.status(), "reconcile_stuck_compactions: open reader " + path.string()));
  }
  std::unique_ptr<parquet::arrow::FileReader> reader = *std::move(reader_res);
  auto table_res = reader->ReadTable();
  if (!table_res.ok()) {
    return Err(from_arrow(table_res.status(), "reconcile_stuck_compactions: read " + path.string()));
  }
  auto combined = (*table_res)->CombineChunks(arrow::default_memory_pool());
  if (!combined.ok()) {
    return Err(from_arrow(combined.status(),
                          "reconcile_stuck_compactions: combine chunks " + path.string()));
  }
  const std::shared_ptr<arrow::Table> &table = *combined;
  const int idx = table->schema()->GetFieldIndex("track_key");
  if (idx < 0) {
    return Err(ErrorCode::Internal,
               "reconcile_stuck_compactions: " + path.string() + " is missing its track_key column");
  }
  const auto &arr = static_cast<const arrow::StringArray &>(*table->column(idx)->chunk(0));
  for (std::int64_t i = 0; i < arr.length(); ++i) {
    if (arr.GetString(i) == target_hex) {
      return Ok(true);
    }
  }
  return Ok(false);
}

} // namespace

Result<ReconcileStats> reconcile_stuck_compactions(Catalog &catalog, std::string_view lake_root) {
  ReconcileStats stats;
  const fs::path root{std::string(lake_root)};

  ATX_TRY(std::vector<TrackRow> staging_rows, catalog.list_by_status(TrackStatus::Staging));

  for (const TrackRow &row : staging_rows) {
    const fs::path staging_file = root / "staging" / (row.track_key + ".feather");
    std::error_code exists_ec;
    if (fs::exists(staging_file, exists_ec)) {
      continue; // genuinely still staging -- a normal compact() run handles it
    }
    ++stats.stuck_rows_found;

    const fs::path partition_dir =
        root / "tracks" / ("underlier=" + row.underlier) / ("family=" + row.family);
    ATX_TRY(std::vector<fs::path> batch_files, list_batch_files(partition_dir));

    std::optional<fs::path> located;
    for (const fs::path &batch_path : batch_files) {
      ATX_TRY(const bool contains, batch_contains_track(batch_path, row.track_key));
      if (contains) {
        located = batch_path;
        break;
      }
    }
    if (!located.has_value()) {
      return Err(ErrorCode::NotFound,
                 "reconcile_stuck_compactions: track_key " + row.track_key +
                     " is 'staging' with no staged input, and was not found in any batch file "
                     "under " +
                     partition_dir.string());
    }

    ATX_TRY(TrackKey key, track_key_from_hex(row.track_key));
    const std::string relative_file = "tracks/underlier=" + row.underlier + "/family=" + row.family +
                                      "/" + located->filename().string();
    // row_group = 0: schema v1 packs exactly one row group per batch file
    // (write_parquet_batch, track_store.cpp).
    ATX_TRY_VOID(catalog.mark_compacted(key, relative_file, /*row_group=*/0));
    ++stats.rows_reconciled;
  }

  return Ok(stats);
}

} // namespace atx::vol
