#pragma once

// track_compact's crash-recovery reconcile pass (Task D5 fix-round, review
// finding 2, backtest-production-lakehouse sprint).
//
// ## The gap this closes
//
// `track_compact.cpp`'s normal path is `compact()` (folds `staging/` into a
// hive-partitioned Parquet batch, deleting each staged input as soon as its
// batch file lands durably) followed by one `Catalog::mark_compacted` call
// per track `compact()` reports (`CompactStats::placements`,
// track_store.hpp). If the PROCESS DIES after `compact()` has already
// deleted a track's staged input but before that track's own
// `mark_compacted` call lands, the track's catalog row is stuck at
// `'staging'` FOREVER: a plain re-run's `compact()` has nothing left to
// report for it (it only ever scans `staging/`, and that file is gone), so
// nothing in the normal path can ever revisit it again. `atxpy.tracks.
// catalog()` (D4) then misreports it, and a future retention/eviction task
// (D6, which needs to know what is safely compactable) would misjudge it.
//
// ## What this does
//
// `reconcile_stuck_compactions` finds and fixes exactly that stuck state.
// For every catalog row still `Staging`:
//   * If `<lake_root>/staging/<track_key>.feather` is PRESENT, the row is
//     genuinely, normally still staging (not yet folded into any batch) --
//     left alone; a future `compact()` call handles it in the ordinary way.
//   * If that file is ABSENT, the row is a candidate: its data was already
//     durably folded into a batch (compact()'s own atomic-publish discipline
//     guarantees the batch landed before the staged input was deleted), just
//     never `mark_compacted`. This function locates it directly in the lake
//     by scanning every batch file under the row's own `underlier`/`family`
//     hive partition and reading each one's `track_key` column for an exact
//     match, then calls `Catalog::mark_compacted` with the file it found
//     (relative to `lake_root`, hive-path style) and `row_group = 0` (schema
//     v1 packs exactly one row group per batch file -- track_store.hpp).
//
// A full per-file column read (rather than pruning candidate files first via
// each row group's `track_key` min/max statistics, which `write_parquet_
// batch`'s `SortingColumn` would make effective, since rows within a batch
// are sorted by `track_key`) is deliberately the simpler, unconditionally
// CORRECT choice here: this is a crash-recovery path expected to run rarely
// and over partitions at this sprint's scale, where the read cost is
// negligible -- not a hot path worth taking on unexercised Parquet
// `Statistics` API risk for.
//
// Idempotent and safe to interrupt at any point ITSELF: a row this function
// already fixed (this call, or a prior one) is no longer `Staging`, so it is
// never revisited -- a crash partway through its own loop still converges on
// the very next call.
//
// Err(NotFound) if a stuck row (staging file absent) cannot be located
// anywhere in its own hive partition -- that is real data loss/corruption,
// never silently tolerated. Err (fail-closed) on any `Catalog`/Arrow/
// filesystem failure, matching this sprint's "a cache/catalog that might
// silently serve a wrong or partial answer is worse than one that refuses"
// posture.

#include <cstdint>
#include <string_view>

#include "atx/vol/research/catalog.hpp" // Catalog, TrackStatus
#include "atx/vol/types.hpp"            // Result

namespace atx::vol {

struct ReconcileStats {
  // `Staging` rows examined whose staging file was ABSENT -- candidates for
  // relocation. A genuinely-still-staging row (file present) is not counted
  // here at all.
  std::uint64_t stuck_rows_found{0};
  // Of those, how many were located and `mark_compacted()`'d by THIS call.
  std::uint64_t rows_reconciled{0};
};

[[nodiscard]] Result<ReconcileStats> reconcile_stuck_compactions(Catalog &catalog,
                                                                 std::string_view lake_root);

} // namespace atx::vol
