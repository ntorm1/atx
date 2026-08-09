// track_compact -- operator CLI over `atx::vol::compact()` (Task D2,
// backtest-production-lakehouse sprint). Folds every staged track under
// <lake_root>/staging/ into hive-partitioned, zstd-compressed Parquet batch
// files under <lake_root>/tracks/underlier=<U>/family=<F>/batch-NNNNNN.parquet.
// See atx/vol/track_store.hpp for the schema and the atomic-publish
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
//
// Task D6: also dispatches a `gc` subcommand over `atx::vol::gc()`
// (research/track_gc.hpp) -- last_access_ts-driven retention. Kept as an
// explicit, separately-invoked subcommand rather than folded into the
// default compact-and-reconcile flow above: GC is destructive (it retires
// and eventually reclaims tracks nobody has read in a while) and needs an
// operator-supplied age policy (`older_than_ts_ns`), so running it
// automatically on every `track_compact <lake_root>` call would silently
// delete data under a default threshold no caller asked for. The default
// (no subcommand) invocation's behavior is completely unchanged.

#include "atx/vol/catalog.hpp"
#include "atx/vol/research/track_compact_reconcile.hpp"
#include "atx/vol/research/track_gc.hpp"
#include "atx/vol/track_key.hpp"
#include "atx/vol/track_store.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

void print_usage(const char *argv0) {
  std::fprintf(stderr,
               "usage: %s <lake_root>\n"
               "       %s gc <lake_root> <older_than_ts_ns>\n"
               "  <lake_root>: compacts every staged track under <lake_root>/staging/ into\n"
               "  hive-partitioned, zstd-compressed Parquet batch files under\n"
               "  <lake_root>/tracks/underlier=<U>/family=<F>/batch-NNNNNN.parquet,\n"
               "  marks each one 'compacted' in <lake_root>/catalog.sqlite, then\n"
               "  reconciles any track a PRIOR interrupted run left stuck 'staging'.\n"
               "  gc: retires tracks whose last_access_ts precedes <older_than_ts_ns>\n"
               "  (nanoseconds since epoch) and reclaims disk for batches that are now\n"
               "  entirely or partially retired, skipping any batch a live reader has\n"
               "  advisory-marked (Catalog::mark_reader). See track_gc.hpp.\n",
               argv0, argv0);
}

int run_gc(const std::string &lake_root, std::string_view older_than_arg) {
  char *end = nullptr;
  const long long parsed = std::strtoll(std::string(older_than_arg).c_str(), &end, 10);
  if (end == nullptr || *end != '\0') {
    std::fprintf(stderr, "track_compact gc: <older_than_ts_ns> is not a valid integer: %.*s\n",
                 static_cast<int>(older_than_arg.size()), older_than_arg.data());
    return 2;
  }
  const atx::vol::Result<atx::vol::GcStats> result =
      atx::vol::gc(lake_root, static_cast<std::int64_t>(parsed));
  if (!result.has_value()) {
    std::fprintf(stderr, "track_compact gc: %s\n", result.error().to_string().c_str());
    return 1;
  }
  const atx::vol::GcStats &stats = *result;
  std::printf("track_compact gc: %llu track(s) retired; %llu batch(es) rewritten, %llu deleted, "
              "%llu skipped (live reader mark), %llu old file(s) not removed\n",
              static_cast<unsigned long long>(stats.tracks_retired),
              static_cast<unsigned long long>(stats.batches_rewritten),
              static_cast<unsigned long long>(stats.batches_deleted),
              static_cast<unsigned long long>(stats.batches_skipped_live_reader),
              static_cast<unsigned long long>(stats.old_files_not_removed));
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc >= 2 && std::string_view(argv[1]) == "gc") {
    if (argc != 4) {
      print_usage(argv[0]);
      return 2;
    }
    return run_gc(argv[2], argv[3]);
  }

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
