#include "atx/vol/research/track_gc.hpp"

// Third-party Arrow/Parquet headers -- confined to this .cpp, same firewall
// discipline as track_store.cpp/track_compact_reconcile.cpp (see either
// file's own doc comment). This is a SEPARATE translation unit from
// track_store.cpp specifically because it needs BOTH Arrow/Parquet
// reading+writing AND catalog.hpp -- track_store.hpp is deliberately
// one-directional and must not gain a Catalog dependency (catalog.hpp's own
// doc comment), so this coordination logic cannot live there. The Parquet
// WRITER below is a deliberate, independent re-derivation of track_store
// .cpp's own `write_parquet_batch` (NOT a shared export) -- track_store.hpp
// is an Arrow-free header by contract (its own doc comment), so an
// arrow::Table-typed declaration cannot be added to it; this mirrors
// track_compact_reconcile.cpp's own established precedent of re-deriving
// small Arrow-side helpers (list_batch_files/batch_contains_track there)
// rather than exporting across this exact seam. KEEP THIS WRITER'S
// PROPERTIES (compression, one row group, sorting columns) IN SYNC WITH
// track_store.cpp's `write_parquet_batch` -- a real batch file's shape
// contract is defined there.
#include <arrow/array.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>
#include <parquet/properties.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <vector>

#include "atx/vol/detail/archive_util.hpp"  // reserve_unique_publish_temp_file, flush_and_publish_file
#include "atx/vol/catalog.hpp"     // Catalog, TrackRow, TrackStatus

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

// ── Batch naming, deliberately DISTINCT from compact()'s own numbering ────
//
// compact() names batches "batch-NNNNNN.parquet" and picks its next index by
// scanning the destination directory for that exact pattern
// (track_store.cpp's next_batch_index). If gc() reused the SAME scheme and
// computed its next index independently, a compact() and a gc() run against
// the SAME partition around the same time could compute the SAME index and
// race to publish two different files at one destination name (the last
// atomic rename wins, silently discarding the other writer's output) -- a
// real, if narrow, hazard neither this task nor D2's compact() closes today.
// "batch-gc-NNNNNN.parquet" sidesteps it entirely: compact()'s own
// next_batch_index size-checks the "batch-" + 6 digits + ".parquet" shape and
// will never look at (or collide with) a "batch-gc-*" name, so the two
// writers' numbering is completely independent. Any *.parquet reader
// (track_compact_reconcile.cpp's list_batch_files, atxpy.tracks' DuckDB glob)
// picks these up exactly like any other batch file -- nothing filters on the
// filename pattern except this local scan.
[[nodiscard]] std::uint64_t next_gc_batch_index(const fs::path &dir) {
  constexpr std::string_view kPrefix = "batch-gc-";
  constexpr std::string_view kSuffix = ".parquet";
  std::error_code exists_ec;
  if (!fs::exists(dir, exists_ec)) {
    return 0;
  }
  bool any = false;
  std::uint64_t max_seen = 0;
  std::error_code list_ec;
  fs::directory_iterator it(dir, list_ec);
  if (list_ec) {
    return 0;
  }
  const fs::directory_iterator end;
  for (; it != end; it.increment(list_ec)) {
    if (list_ec) {
      break;
    }
    const std::string name = it->path().filename().string();
    if (name.size() != kPrefix.size() + 6 + kSuffix.size() || name.rfind(kPrefix, 0) != 0 ||
        name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) {
      continue;
    }
    const std::string digits = name.substr(kPrefix.size(), 6);
    const bool all_digit =
        std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!all_digit) {
      continue;
    }
    const std::uint64_t idx = std::stoull(digits);
    if (!any || idx > max_seen) {
      max_seen = idx;
      any = true;
    }
  }
  return any ? max_seen + 1 : 0;
}

[[nodiscard]] std::string format_gc_batch_name(std::uint64_t idx) {
  char buf[32];
  const int n =
      std::snprintf(buf, sizeof buf, "batch-gc-%06llu.parquet", static_cast<unsigned long long>(idx));
  return std::string(buf, buf + (n > 0 ? n : 0));
}

[[nodiscard]] Result<std::shared_ptr<arrow::Table>> read_batch_table(const fs::path &path) {
  auto in = arrow::io::ReadableFile::Open(path.string());
  if (!in.ok()) {
    return Err(from_arrow(in.status(), "gc: open batch " + path.string()));
  }
  auto reader_res = parquet::arrow::OpenFile(*in, arrow::default_memory_pool());
  if (!reader_res.ok()) {
    return Err(from_arrow(reader_res.status(), "gc: open reader " + path.string()));
  }
  std::unique_ptr<parquet::arrow::FileReader> reader = *std::move(reader_res);
  auto table_res = reader->ReadTable();
  if (!table_res.ok()) {
    return Err(from_arrow(table_res.status(), "gc: read batch " + path.string()));
  }
  auto combined = (*table_res)->CombineChunks(arrow::default_memory_pool());
  if (!combined.ok()) {
    return Err(from_arrow(combined.status(), "gc: combine chunks " + path.string()));
  }
  return Ok(*combined);
}

// One contiguous run of equal track_key -- guaranteed contiguous because
// every batch file's rows are sorted by (track_key, date)
// (track_store.cpp's write_parquet_batch SortingColumn contract).
struct Run {
  std::int64_t start{0};
  std::int64_t length{0};
  std::string track_key;
};

[[nodiscard]] Result<std::vector<Run>> group_runs_by_track_key(const arrow::Table &table) {
  const int idx = table.schema()->GetFieldIndex("track_key");
  if (idx < 0) {
    return Err(ErrorCode::Internal, "gc: batch is missing its track_key column");
  }
  const auto &arr = static_cast<const arrow::StringArray &>(*table.column(idx)->chunk(0));
  std::vector<Run> runs;
  const std::int64_t rows = arr.length();
  std::int64_t i = 0;
  while (i < rows) {
    const std::string key = arr.GetString(i);
    std::int64_t j = i + 1;
    while (j < rows && arr.GetString(j) == key) {
      ++j;
    }
    runs.push_back(Run{i, j - i, key});
    i = j;
  }
  return Ok(std::move(runs));
}

// Deliberate re-derivation of track_store.cpp's write_parquet_batch -- see
// this file's top comment for why it is not a shared export, and keep the
// WriterProperties below in lockstep with that function if either changes.
[[nodiscard]] Status write_batch_parquet(const arrow::Table &table, const std::string &dst_path) {
  const int track_key_idx = table.schema()->GetFieldIndex("track_key");
  const int date_idx = table.schema()->GetFieldIndex("date");
  if (track_key_idx < 0 || date_idx < 0) {
    return Err(ErrorCode::Internal, "gc: rewritten batch is missing track_key/date");
  }

  const Result<std::string> tmp_path = detail::reserve_unique_publish_temp_file(dst_path);
  if (!tmp_path.has_value()) {
    return Err(tmp_path.error());
  }

  const std::int64_t whole_table_rows = table.num_rows() > 0 ? table.num_rows() : 1;
  parquet::WriterProperties::Builder builder;
  builder.compression(parquet::Compression::ZSTD);
  builder.max_row_group_length(whole_table_rows);
  builder.set_sorting_columns({
      parquet::SortingColumn{track_key_idx, /*descending=*/false, /*nulls_first=*/false},
      parquet::SortingColumn{date_idx, /*descending=*/false, /*nulls_first=*/false},
  });
  const std::shared_ptr<parquet::WriterProperties> props = builder.build();

  auto out_res = arrow::io::FileOutputStream::Open(*tmp_path);
  if (!out_res.ok()) {
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(out_res.status(), "gc: open temp batch file"));
  }
  const std::shared_ptr<arrow::io::FileOutputStream> out = *out_res;

  const arrow::Status write_st =
      parquet::arrow::WriteTable(table, arrow::default_memory_pool(), out, whole_table_rows, props);
  if (!write_st.ok()) {
    [[maybe_unused]] const arrow::Status ignored_close = out->Close();
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(write_st, "gc: parquet write failed"));
  }
  const arrow::Status close_st = out->Close();
  if (!close_st.ok()) {
    std::error_code rm_ec;
    fs::remove(*tmp_path, rm_ec);
    return Err(from_arrow(close_st, "gc: cannot close temp batch file"));
  }

  return detail::flush_and_publish_file(*tmp_path, dst_path);
}

// Best-effort delete with bounded retry -- mirrors detail::WriterLock::
// release()'s own "a concurrent reader can hold a transient open against
// this file" tolerance. Returns true iff the file is confirmed gone.
[[nodiscard]] bool try_remove_best_effort(const fs::path &path) {
  std::error_code exists_ec;
  if (!fs::exists(path, exists_ec)) {
    return true;
  }
  std::error_code rm_ec;
  for (int attempt = 0; attempt < 5; ++attempt) {
    fs::remove(path, rm_ec);
    if (!fs::exists(path, exists_ec)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

} // namespace

Result<GcStats> gc(std::string_view lake_root, std::int64_t older_than_ts_ns) {
  GcStats stats{};
  const fs::path root{std::string(lake_root)};

  ATX_TRY(Catalog catalog, Catalog::open(std::string(lake_root)));

  ATX_TRY(const std::int64_t newly_retired, catalog.retire_stale(older_than_ts_ns));
  stats.tracks_retired = static_cast<std::uint64_t>(newly_retired);

  // Every batch FILE any Retired row still points at -- this call's own
  // fresh retirees above, plus any earlier Retired row (a prior gc() run
  // left untouched because it was live-marked, or a D5 economics-rev
  // supersession) whose data has not been physically reclaimed yet. An
  // ALREADY-reclaimed row has file == NULL (apply_gc_rewrite clears it), so
  // it naturally drops out of this scan -- that is what makes a second gc()
  // call over unchanged state a true no-op (TrackGcTest.
  // IsIdempotentOnRerunWithNothingLeftToDo).
  ATX_TRY(std::vector<TrackRow> retired_rows, catalog.list_by_status(TrackStatus::Retired));
  std::vector<std::string> affected_files;
  affected_files.reserve(retired_rows.size());
  for (const TrackRow &row : retired_rows) {
    if (row.file.has_value() && !row.file->empty()) {
      affected_files.push_back(*row.file);
    }
  }
  std::sort(affected_files.begin(), affected_files.end());
  affected_files.erase(std::unique(affected_files.begin(), affected_files.end()), affected_files.end());

  for (const std::string &file : affected_files) {
    // Brief Step 1(a)'s replacement mechanism, checked BEFORE this file is
    // touched in any way: "reader takes a shared advisory mark in SQLite, GC
    // skips marked batches". This protects a REGISTERED reader only -- see
    // this header's own doc comment, and track_gc.hpp's, for the residual
    // unregistered-reader window (atxpy.tracks' unlocked DuckDB glob) this
    // does NOT close; that is an accepted, documented gap, not an oversight.
    ATX_TRY(const bool marked, catalog.has_live_reader_mark(file));
    if (marked) {
      ++stats.batches_skipped_live_reader;
      continue;
    }

    ATX_TRY(std::vector<TrackRow> members, catalog.rows_by_file(file));
    std::vector<std::string> retired_keys;
    std::vector<std::string> kept_keys;
    for (const TrackRow &member : members) {
      if (member.status == TrackStatus::Retired) {
        retired_keys.push_back(member.track_key);
      } else {
        kept_keys.push_back(member.track_key);
      }
    }
    if (retired_keys.empty()) {
      // Defensive: `file` came from a Retired row's own pointer, so this
      // should be unreachable, but a concurrent writer changing the row
      // between the two queries above is not impossible. Nothing to do.
      continue;
    }

    const fs::path old_path = root / file;

    if (kept_keys.empty()) {
      // Every track in this batch is retired -- the whole file is
      // reclaimable. Catalog-first is not meaningful here (there is no
      // "new file" step): clear the retired rows, THEN best-effort delete
      // the file -- a crash between the two leaves an orphan file with
      // nothing in the catalog pointing at it (harmless, counted via
      // old_files_not_removed's sibling case below, not a correctness
      // hazard) rather than a catalog row pointing at bytes that are gone.
      ATX_TRY_VOID(catalog.apply_gc_rewrite(file, "", retired_keys, {}));
      if (!try_remove_best_effort(old_path)) {
        ++stats.old_files_not_removed;
      }
      ++stats.batches_deleted;
      continue;
    }

    // Rewrite path: read, drop the retired tracks' contiguous runs, publish
    // a NEW file, repoint survivors, THEN remove the old file -- see
    // track_gc.hpp's "Deletion ordering" section for the full argument on
    // why this order (new file durable -> catalog commit -> old file
    // removed) is what keeps a registered reader safe across the whole
    // operation, including a crash at any point in it.
    ATX_TRY(std::shared_ptr<arrow::Table> table, read_batch_table(old_path));
    ATX_TRY(std::vector<Run> runs, group_runs_by_track_key(*table));
    const std::unordered_set<std::string> retired_set(retired_keys.begin(), retired_keys.end());
    std::vector<std::shared_ptr<arrow::Table>> kept_slices;
    kept_slices.reserve(runs.size());
    for (const Run &run : runs) {
      if (!retired_set.contains(run.track_key)) {
        kept_slices.push_back(table->Slice(run.start, run.length));
      }
    }
    if (kept_slices.empty()) {
      return Err(ErrorCode::Internal,
                 "gc: rewrite of " + file + " produced zero surviving rows despite non-empty kept_keys");
    }
    auto combined_res = arrow::ConcatenateTables(kept_slices);
    if (!combined_res.ok()) {
      return Err(from_arrow(combined_res.status(), "gc: concatenate surviving rows for " + file));
    }

    const fs::path dst_dir = old_path.parent_path();
    const std::uint64_t idx = next_gc_batch_index(dst_dir);
    const std::string new_name = format_gc_batch_name(idx);
    const fs::path new_path = dst_dir / new_name;
    // `file` is lake_root-relative, hive-path style (forward slashes) --
    // swap only the filename component, matching how Catalog::mark_compacted
    // /TrackRow::file already store it (track_store.cpp's own relative_file
    // construction, not fs::relative, for the same forward-slash reason).
    const std::size_t last_slash = file.find_last_of('/');
    const std::string new_relative =
        (last_slash == std::string::npos) ? new_name : file.substr(0, last_slash + 1) + new_name;

    ATX_TRY_VOID(write_batch_parquet(**combined_res, new_path.string()));

    // Catalog publish BEFORE the old file is removed -- the load-bearing
    // ordering step. From this point on every surviving row resolves to the
    // NEW file; the old one is now unreferenced disk.
    ATX_TRY_VOID(catalog.apply_gc_rewrite(file, new_relative, retired_keys, kept_keys));

    if (!try_remove_best_effort(old_path)) {
      ++stats.old_files_not_removed;
    }
    ++stats.batches_rewritten;
  }

  return Ok(stats);
}

} // namespace atx::vol
