#pragma once

// Persistent catalog of reusable projection-backed backtests.
//
// On disk a BacktestDb is:
//
//   <root>/manifest.atxbtdb
//   <root>/partitions/t<template-fingerprint>-i<id-hash>-s<symbol-hash>
//                     -g<manifest-generation>.atxrun
//
// Both the manifest and every series partition are ordinary RunArchive files.
// RunArchive therefore owns the binary framing, schema stamp, layered CRC-32C,
// mmap lifetime, and durable temp+rename publication. The custom sections in
// these archives are versioned by kBacktestDbEngineSchemaSalt below.
//
// Thread/process contract: all mutations through one BacktestDb instance are
// serialized. Across processes the database is single-writer/many-reader.
// Readers call refresh() to observe an atomically-published newer manifest.
// Every write uses an immutable generation-versioned partition name and
// publishes that partition before the generation+1 manifest. A crash in between
// leaves only an unindexed orphan; the old manifest still points to its untouched
// partition. Successful replacements also leave the retired version unindexed.
// vacuum_unindexed_partitions() removes either class only during an explicitly
// offline maintenance window, after the writer and any readers holding older
// manifest snapshots exit.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "atx/vol/backtest.hpp"
#include "atx/vol/backtest_template.hpp"
#include "atx/vol/research/run_archive.hpp"
#include "atx/vol/surface_archive.hpp"
#include "atx/vol/types.hpp"

namespace atx::vol {

inline constexpr std::string_view kBacktestDbManifestName = "manifest.atxbtdb";
inline constexpr std::string_view kBacktestDbPartitionDir = "partitions";
inline constexpr std::string_view kBacktestDbPartitionExt = ".atxrun";

// Bump whenever the custom manifest/partition encoding or continuation
// economics change incompatibly. It participates in every series identity.
inline constexpr std::uint64_t kBacktestDbEngineSchemaSalt =
    0x4154584254440001ULL; // "ATXBTD", schema/engine revision 1

struct BacktestSourcePartition {
  std::string date;
  ArchiveContentIdentity identity{};

  [[nodiscard]] bool operator==(const BacktestSourcePartition &) const noexcept = default;
};

struct BacktestSeriesInfo {
  std::string template_id;
  std::uint64_t template_fingerprint{0};
  std::string symbol;
  std::uint32_t uid{0};
  std::uint64_t row_count{0};
  std::int64_t first_ts_ns{0};
  std::int64_t last_ts_ns{0};
  std::uint64_t source_fingerprint{0};
  std::uint64_t run_identity_hash{0};
  std::string partition_filename;
  ArchiveContentIdentity partition_identity{};

  [[nodiscard]] bool operator==(const BacktestSeriesInfo &) const noexcept = default;
};

struct BacktestSeriesData {
  BacktestResult backtest;
  BacktestCheckpoint checkpoint;
  std::uint32_t next_cohort{0};
  std::vector<BacktestSourcePartition> sources;
};

// A mapped backtest section that co-owns the RunArchive whose bytes its
// RaSectionView borrows. The declaration order keeps the archive alive until
// after the view has been destroyed.
struct MappedBacktestView {
  std::shared_ptr<const RunArchive> archive;
  RaSectionView view;

  [[nodiscard]] const RaSectionView *operator->() const noexcept { return &view; }
  [[nodiscard]] const RaSectionView &operator*() const noexcept { return view; }
};

// Stable identities used by the daily extension planner. Inputs must be in
// strict ascending date order; invalid inputs return zero.
[[nodiscard]] std::uint64_t
backtest_source_fingerprint(std::span<const BacktestSourcePartition> sources) noexcept;
[[nodiscard]] std::uint64_t
backtest_series_identity(std::uint64_t template_fingerprint, std::uint32_t uid,
                         std::span<const BacktestSourcePartition> sources) noexcept;

// Append an incremental result (whose resumed engine output omits its anchor)
// to an owned history. Both inputs must be internally row-parallel and have the
// same signal/optional-column shape. The strong guarantee holds on failure.
[[nodiscard]] Status append_backtest_results(BacktestResult &dst, const BacktestResult &src);

// Task D6: a live PID-liveness registration that `BacktestDb::
// vacuum_unindexed_partitions` refuses to run past -- see that method's own
// doc comment and the hazard this closes (immediately above, and originally
// named unenforced at :131-140 pre-D6). Acquire one (`BacktestDb::
// mark_reader`) BEFORE resolving any partition filename you intend to keep
// reading across a window where a concurrent vacuum could otherwise remove
// it (e.g. a batch job that calls `load_series`/`map_backtest` repeatedly
// against series it looked up earlier), and hold it until you no longer
// need the guarantee -- release promptly (explicit `release()` or letting
// the guard go out of scope), since a live mark blocks EVERY vacuum on this
// root, not just a call that would actually collide with it.
//
// MECHANISM: `<root>/readers/reader-<pid>-<nonce>.mark`, body = the owning
// process's PID -- mirrors `detail::WriterLock`'s own PID-liveness
// convention, but UNLIKE WriterLock this is never mutual exclusion: several
// marks (same or different processes, same or different intent) may coexist,
// so `acquire()` never waits or contends, it simply writes its own uniquely-
// named file. A mark whose process has died is stale and is treated as
// absent -- self-healingly removed the next time anything scans the
// directory (a `vacuum_unindexed_partitions()` call, in practice) -- so a
// reader that crashed without releasing does not block vacuum forever.
//
// Independent of any `BacktestDb` handle's lifetime: `release()` is a plain
// file delete, not a call back into the `BacktestDb` that created it (there
// usually isn't one by the time a long-lived reader is done), so a
// `BacktestReaderMark` is safe to outlive, or be destroyed independently of,
// its originating handle.
class BacktestReaderMark {
public:
  [[nodiscard]] static Result<BacktestReaderMark> acquire(std::string_view root);

  ~BacktestReaderMark();
  BacktestReaderMark(BacktestReaderMark &&other) noexcept;
  BacktestReaderMark &operator=(BacktestReaderMark &&other) noexcept;
  BacktestReaderMark(const BacktestReaderMark &) = delete;
  BacktestReaderMark &operator=(const BacktestReaderMark &) = delete;

  // Release early. Idempotent -- a no-op if already released or moved-from.
  // The destructor calls this, so callers normally never need to.
  void release() noexcept;

  [[nodiscard]] bool held() const noexcept { return !mark_path_.empty(); }

private:
  BacktestReaderMark() = default;
  std::string mark_path_;
};

class BacktestDb {
public:
  // Create root + partitions and publish generation 1. AlreadyExists if a
  // manifest is already present.
  [[nodiscard]] static Result<BacktestDb> create(std::string_view root);

  // Open and eagerly validate the manifest, all persisted template
  // fingerprints, sort/uniqueness invariants, and index metadata.
  [[nodiscard]] static Result<BacktestDb> open(std::string_view root);

  BacktestDb(BacktestDb &&) noexcept = default;
  BacktestDb &operator=(BacktestDb &&) noexcept = default;
  BacktestDb(const BacktestDb &) = delete;
  BacktestDb &operator=(const BacktestDb &) = delete;

  [[nodiscard]] const std::string &root() const noexcept { return root_; }
  [[nodiscard]] std::uint64_t generation() const;

  // Re-read the manifest when an external single writer has advanced its
  // generation. A generation rollback is rejected fail-closed.
  [[nodiscard]] Status refresh();

  // Offline maintenance: refresh the manifest, then remove regular non-symlink
  // files directly under partitions/ whose names match the complete
  // generation-versioned BacktestDb filename grammar but are not referenced by
  // the current manifest. Unknown files, writer temporary files, directories,
  // symlinks, and every currently indexed partition are untouched.
  //
  // Serialized with in-process mutations. Task D6: additionally refuses
  // outright -- Err(Unavailable), nothing removed -- while ANY live
  // `BacktestReaderMark` is registered against this root (see that class's
  // own doc comment). This closes the live-reader deletion hazard for a
  // REGISTERED reader specifically; it is not a general cross-process lock
  // on every possible reader. A caller that reads partitions without ever
  // acquiring a mark (a one-shot `load_series`/`map_backtest` call, which
  // resolves its filename and finishes before returning -- the common case)
  // needs no mark and is unaffected either way. Cross-process callers still
  // carry the "ensure no reader still relies on an older manifest snapshot"
  // obligation this comment always named -- what changed is that a reader
  // who WANTS that guarantee enforced now has a mechanism to make vacuum
  // refuse on its behalf, instead of only a documented convention to honor.
  // IoError may report a partial vacuum if a filesystem failure occurs after
  // earlier candidates were removed.
  [[nodiscard]] Result<std::size_t> vacuum_unindexed_partitions();

  // Convenience: `BacktestReaderMark::acquire(root())`.
  [[nodiscard]] Result<BacktestReaderMark> mark_reader() const;

  // Catalog identity is template.id; economic identity is its fingerprint.
  // Re-registering the same id+fingerprint is a no-op. Reusing an id for
  // changed economics is AlreadyExists.
  [[nodiscard]] Status register_template(const BacktestStrategyTemplate &strategy_template);
  [[nodiscard]] std::vector<BacktestStrategyTemplate> templates() const;
  [[nodiscard]] Result<BacktestStrategyTemplate> find_template(std::string_view template_id) const;

  [[nodiscard]] std::vector<BacktestSeriesInfo> series() const;
  [[nodiscard]] Result<BacktestSeriesInfo> find_series(std::string_view template_id,
                                                       std::string_view symbol) const;

  // Derive and validate all index metadata from data, atomically publish the
  // partition first, then publish generation+1 of the manifest.
  [[nodiscard]] Status write_series(std::string_view template_id, std::string_view symbol,
                                    std::uint32_t uid, const BacktestSeriesData &data);

  // Same write with caller-supplied metadata. Every derived field is checked;
  // callers cannot bless inconsistent row/source/template identities. For a
  // new key, partition_filename and partition_identity must both be empty/zero.
  // For an update, both must exactly match the current catalog entry (normally
  // pass the BacktestSeriesInfo returned by find_series after updating its
  // row/source-derived fields). They are optimistic-concurrency tokens: stale
  // metadata is rejected before the new immutable partition is published.
  [[nodiscard]] Status write_series(const BacktestSeriesInfo &info, const BacktestSeriesData &data);

  // Fully validate the partition CRCs, custom-section schemas, checkpoint and
  // source invariants, and manifest/partition identity agreement, then return
  // owned values.
  [[nodiscard]] Result<BacktestSeriesData> load_series(std::string_view template_id,
                                                       std::string_view symbol) const;

  // Validate the same partition/index envelope and return a zero-copy mapped
  // view of its backtest section. The returned handle owns the mapping.
  [[nodiscard]] Result<MappedBacktestView> map_backtest(std::string_view template_id,
                                                        std::string_view symbol) const;

private:
  struct Snapshot;

  BacktestDb() = default;
  [[nodiscard]] static Result<std::shared_ptr<const Snapshot>>
  read_manifest(const std::filesystem::path &path);
  [[nodiscard]] Status persist_locked(std::vector<BacktestStrategyTemplate> templates,
                                      std::vector<BacktestSeriesInfo> series);
  [[nodiscard]] std::filesystem::path manifest_path() const;
  [[nodiscard]] std::filesystem::path partition_path(std::string_view partition_filename) const;

  std::string root_;
  mutable std::unique_ptr<std::mutex> mu_;
  std::shared_ptr<const Snapshot> snapshot_;
};

} // namespace atx::vol
