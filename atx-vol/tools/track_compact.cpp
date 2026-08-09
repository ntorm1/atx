// track_compact -- operator CLI over `atx::vol::compact()` (Task D2,
// backtest-production-lakehouse sprint). Folds every staged track under
// <lake_root>/staging/ into hive-partitioned, zstd-compressed Parquet batch
// files under <lake_root>/tracks/underlier=<U>/family=<F>/batch-NNNNNN.parquet.
// See atx/vol/research/track_store.hpp for the schema and the atomic-publish
// discipline this CLI relies on end to end.
//
// Task D5: also syncs the D3 catalog -- for every track `compact()` folded
// in this run, opens the catalog and calls `Catalog::mark_compacted` with
// the placement `compact()` reports (`CompactStats::placements`,
// track_store.hpp). Before this, running `compact()` (via this CLI or
// otherwise) left every compacted track's `tracks.status` permanently stuck
// at `'staging'` -- no code path ever called `mark_compacted` -- which is a
// real production gap: `atxpy.tracks.catalog()` (D4) and a future
// retention/eviction task (D6, which needs to know what is safely
// compactable) both read that column. `compact()` itself stays
// catalog-agnostic (track_store.hpp is deliberately one-directional and does
// not depend on catalog.hpp -- see catalog.hpp's own doc comment on reused
// types); this CLI is where the two halves of the lakehouse meet.
//
// Fails closed: a `mark_compacted` error for one track aborts the whole run
// (matching this sprint's "a cache/catalog that might silently serve a
// wrong or partial answer is worse than one that refuses" posture) -- the
// Parquet data itself is already durably written and safe either way
// (compact()'s own atomic-publish discipline).
//
// SELF-HEALING across a crash at ANY point (Task D5 fix-round, review
// finding 2 -- the PREVIOUS version of this comment claimed "an operator
// can re-run track_compact safely" purely on the strength of
// `mark_compacted`'s own guard, which is FALSE: if this process dies after
// `compact()` has already deleted a track's staged input but before that
// track's `mark_compacted` call lands, the normal loop above has nothing
// left to re-discover it with -- `compact()` only ever scans `staging/`,
// and that file is gone -- so the row would stay `'staging'` forever with
// no staged input to ever re-drive it, and a plain re-run would print
// "0 track(s) compacted" and exit 0 while the stuck row silently rotted).
// `reconcile_stuck_compactions` (research/track_compact_reconcile.hpp) runs
// UNCONDITIONALLY after the normal loop -- independent of whether THIS
// call's `compact()` found anything new, since the crash it recovers from
// may have happened during ANY prior invocation -- and fixes exactly that
// state: it finds every catalog row still `'staging'` whose staging file is
// absent, relocates it directly in the lake, and `mark_compacted()`s it.
// Idempotent and itself safe to interrupt at any point (see that header's
// own doc comment). With this pass, re-running track_compact after a crash
// at ANY point genuinely does converge the catalog -- the claim the old
// comment made prematurely.
//
// Only built when ATX_VOL_LAKEHOUSE is ON (atx-vol/CMakeLists.txt) -- the
// library entry points it wraps do not exist in the OFF build.

#include "atx/vol/research/catalog.hpp"
#include "atx/vol/research/track_compact_reconcile.hpp"
#include "atx/vol/research/track_key.hpp"
#include "atx/vol/research/track_store.hpp"

#include <cstdio>
#include <string>
#include <string_view>

namespace {

void print_usage(const char *argv0) {
  std::fprintf(stderr,
               "usage: %s <lake_root>\n"
               "  Compacts every staged track under <lake_root>/staging/ into\n"
               "  hive-partitioned, zstd-compressed Parquet batch files under\n"
               "  <lake_root>/tracks/underlier=<U>/family=<F>/batch-NNNNNN.parquet,\n"
               "  marks each one 'compacted' in <lake_root>/catalog.sqlite, then\n"
               "  reconciles any track a PRIOR interrupted run left stuck 'staging'.\n",
               argv0);
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2) {
    print_usage(argv[0]);
    return 2;
  }
  const std::string_view arg1{argv[1]};
  if (arg1 == "--help" || arg1 == "-h") {
    print_usage(argv[0]);
    return 0;
  }

  const std::string lake_root{arg1};
  const atx::vol::Result<atx::vol::CompactStats> result = atx::vol::compact(lake_root);
  if (!result.has_value()) {
    std::fprintf(stderr, "track_compact: %s\n", result.error().to_string().c_str());
    return 1;
  }

  const atx::vol::CompactStats &stats = *result;
  std::printf("track_compact: %llu track(s) compacted into %llu batch file(s); "
              "%llu staged input(s) deleted\n",
              static_cast<unsigned long long>(stats.tracks_compacted),
              static_cast<unsigned long long>(stats.batch_files_written),
              static_cast<unsigned long long>(stats.staged_files_deleted));

  // The catalog is opened unconditionally (not gated behind
  // `stats.placements.empty()`) -- the reconcile pass below must run even
  // when THIS call's compact() found nothing new, since the crash it
  // recovers from may have happened during any EARLIER invocation.
  atx::vol::Result<atx::vol::Catalog> catalog = atx::vol::Catalog::open(lake_root);
  if (!catalog.has_value()) {
    std::fprintf(stderr, "track_compact: catalog open failed: %s\n",
                 catalog.error().to_string().c_str());
    return 1;
  }

  std::uint64_t marked = 0;
  for (const atx::vol::CompactedTrackPlacement &placement : stats.placements) {
    const atx::vol::Result<atx::vol::TrackKey> key = atx::vol::track_key_from_hex(placement.track_key_hex);
    if (!key.has_value()) {
      std::fprintf(stderr, "track_compact: bad track_key_hex '%s' from compact(): %s\n",
                   placement.track_key_hex.c_str(), key.error().to_string().c_str());
      return 1;
    }
    const atx::vol::Status marked_status = catalog->mark_compacted(*key, placement.file, placement.row_group);
    if (!marked_status.has_value()) {
      std::fprintf(stderr, "track_compact: mark_compacted(%s) failed: %s\n",
                   placement.track_key_hex.c_str(), marked_status.error().to_string().c_str());
      return 1;
    }
    ++marked;
  }
  if (marked > 0) {
    std::printf("track_compact: %llu catalog row(s) marked compacted\n",
                static_cast<unsigned long long>(marked));
  }

  // Crash-recovery reconcile pass -- see this file's own doc comment and
  // track_compact_reconcile.hpp for what this fixes and why it must run
  // every time, not just when `stats.placements` was non-empty.
  const atx::vol::Result<atx::vol::ReconcileStats> reconciled =
      atx::vol::reconcile_stuck_compactions(*catalog, lake_root);
  if (!reconciled.has_value()) {
    std::fprintf(stderr, "track_compact: crash-recovery reconcile failed: %s\n",
                 reconciled.error().to_string().c_str());
    return 1;
  }
  if (reconciled->stuck_rows_found > 0) {
    std::printf(
        "track_compact: crash-recovery reconcile: %llu stuck row(s) found, %llu reconciled\n",
        static_cast<unsigned long long>(reconciled->stuck_rows_found),
        static_cast<unsigned long long>(reconciled->rows_reconciled));
  }
  return 0;
}
