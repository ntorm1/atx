#pragma once

// track_gc -- last_access_ts-driven retention/GC for the track lakehouse
// (Task D6, backtest-production-lakehouse sprint). Sits beside D2's
// TrackStore (track_store.hpp) and D3's Catalog (catalog.hpp): TrackStore
// writes/compacts Parquet, Catalog is the metadata of record, and this file
// is where the two meet to reclaim disk for tracks nobody has touched in a
// while -- the same reason track_compact_reconcile.hpp gets its own TU
// rather than living in track_store.cpp (which is deliberately
// Catalog-free, see catalog.hpp's own doc comment) or catalog.cpp
// (deliberately Arrow-free, see this header's own "Arrow-free header" note
// below).
//
// ## What `gc()` does, in order
//
//   1. `Catalog::retire_stale(older_than_ts_ns)` -- flips every `Compacted`
//      row whose `last_access_ts` is older than the threshold to `Retired`.
//      Pure status-label transition, always safe, always cheap (see
//      catalog.hpp's own doc comment on `TrackStatus::Retired`).
//   2. For every batch FILE any `Retired` row (from this call, or an
//      earlier one -- including D5's economics-rev supersession retirees)
//      still points at: skip it entirely if `Catalog::has_live_reader_mark`
//      reports a live advisory mark -- the brief's Step 1(a) replacement
//      mechanism ("reader takes a shared advisory mark in SQLite, GC skips
//      marked batches") verbatim. Otherwise:
//        * if EVERY track in the file is `Retired`, delete the file outright;
//        * otherwise, rewrite the file WITHOUT the retired rows' data (the
//          surviving `Compacted` rows' rows only), publish it durably at a
//          new filename, then delete the old one.
//
// ## Deletion ordering (why this order, stated once here)
//
// A batch file never disappears while the catalog still points a
// `Compacted` row at it -- readers resolve "where is track X" through the
// catalog (`TrackRow::file`), never by globbing the filesystem and guessing,
// so THIS is the invariant that keeps every registered reader safe. The
// rewrite path enforces it by construction: (1) the new file is written and
// durably published FIRST, touching neither the catalog nor the old file --
// if this step fails or the process dies here, nothing downstream has
// changed, and the run is safely retryable; (2) `Catalog::apply_gc_rewrite`
// commits ONE transaction that repoints every surviving row at the new file
// and clears every reclaimed row's `file`/`row_group` to NULL -- the instant
// this commits, the catalog is self-consistent and BOTH files exist on disk,
// so a reader resolving via the catalog never observes a dangling pointer;
// (3) ONLY THEN is the old file removed, best-effort. A crash between (2)
// and (3) leaves a harmless orphan (nothing in the catalog references the
// old file any more) -- disk bytes not yet reclaimed, never a correctness
// hazard, and `GcStats::old_files_not_removed` counts it rather than hiding
// it. The whole-file-delete path is the same shape with no "new file" step.
//
// ## The residual unregistered-reader window (stated honestly, not glossed)
//
// `has_live_reader_mark` protects ONLY a batch a REGISTERED reader marked
// first. `python/src/atxpy/tracks.py` scans `tracks/**/*.parquet` via DuckDB
// directly, with NO lock and NO advisory mark -- it is not a registered
// reader, and `gc()` has no way to see it. A `gc()` run racing an in-flight
// `atxpy.tracks` scan can therefore delete or rewrite a file that scan is
// mid-read on. This is a real, accepted gap (matching the brief's own scope:
// the advisory-mark mechanism protects REGISTERED readers only), not
// something this task invents machinery to close -- see the same note
// repeated at the point `gc()` actually deletes/rewrites a file, in
// track_gc.cpp.
//
// ## Arrow-free header
//
// Like track_store.hpp and track_compact_reconcile.hpp, this header names
// only already Arrow/SQLite-free types (`Result`, `GcStats`) and compiles
// unconditionally; only track_gc.cpp (Arrow + Catalog) is excluded from the
// ATX_VOL_LAKEHOUSE=OFF build (atx-vol/CMakeLists.txt) -- calling `gc()` from
// an OFF build is a LINK error, not a compile error, exactly like `compact()`
// and `reconcile_stuck_compactions()`.

#include <cstdint>
#include <string_view>

#include "atx/vol/types.hpp" // Result

namespace atx::vol {

// Outcome of one `gc()` call. `tracks_retired` counts THIS call's own
// `retire_stale` transitions (Compacted -> Retired by age) -- it does NOT
// count rows that were already Retired coming in (from a prior gc() run, or
// from D5 economics-rev supersession), even though those may still be
// physically reclaimed by the SAME call if their batch file is affected.
struct GcStats {
  std::uint64_t tracks_retired{0};
  std::uint64_t batches_rewritten{0};          // file rewritten without its retired rows
  std::uint64_t batches_deleted{0};             // every track in the file was retired
  std::uint64_t batches_skipped_live_reader{0}; // left untouched -- see has_live_reader_mark
  // A new/rewritten batch published and the catalog updated, but the OLD
  // file could not be removed (best-effort; see the ordering note above) --
  // never a correctness issue, only reclaimed disk left for a later run.
  std::uint64_t old_files_not_removed{0};
};

// Retires tracks by `last_access_ts` and reclaims disk for batches that are
// now entirely, or partially, retired -- see the file's own doc comment for
// the full algorithm and the deletion-ordering argument. Idempotent: a
// second call with the same `older_than_ts_ns` and no new activity finds
// nothing left to do (every stat 0). Err (fail-closed) on any
// Catalog/Arrow/filesystem failure this call cannot itself recover from;
// `old_files_not_removed` (NOT an Err) is the one class of failure this
// function tolerates, since it costs disk, not correctness.
[[nodiscard]] Result<GcStats> gc(std::string_view lake_root, std::int64_t older_than_ts_ns);

} // namespace atx::vol
