#pragma once
// The SQLite index of one research cache directory (P9 lane SQL4; contract K-P9-13; sql-design
// sections 3.3, 3.7 group `cache`, 3.10; rulings SQL-6, SQL-11). It replaces a cache's JSON index
// files -- the IC signal cache's sidecars, the IC-result cache's record files and the marginal
// verb's pair shards -- by rows of DIR/index.sqlite, ONLY when an explicit flag asks for it:
// `--candidate-cache-index sqlite` on the IC exe, `--pair-cache-index sqlite` on the marginal verb.
// The flag absent (or `files`) is today's code path and bytes whatever files exist: nothing opens
// an index, and a present index.sqlite is never read (no selection by file presence). The index is
// created only by `atx-research-store cache init DIR`; this object never creates one. Payload
// files (*.f64) stay where they are; a row names its payload by its '/'-path under DIR.
//
// One CacheIndex per cache directory per process: ONE connection (atx::engine::research::store::
// open_cache) guarded by ONE std::mutex, prepared statements through ops_cache.hpp (the cache
// group's only instantiating TU).
//
// Lock order: the mutex is a LEAF lock. It is taken inside every member below, held only while
// that member's SQLite statements run on the connection (one statement for a read, one
// BEGIN IMMEDIATE transaction for a write, including SQLite's busy wait of up to 30 s per attempt
// and with_immediate's bounded retry), and released before the member returns or writes a progress
// line. No callback runs under it and no other lock is taken while it is held, so it never takes
// part in a lock cycle. Callers hash payloads, run the VM and read or write payload files without
// it: those happen before or after a member call, never inside one. Thread-safety: every member
// may be called from any thread; the connection is only touched under the mutex (SQLite's
// multi-thread mode forbids concurrent use of one connection, not serialized use).
//
// Failure policy (brief task 2): a SQLITE_BUSY after the busy timeout and retries
// (ErrorCode::Unavailable) or any I/O error (IoError: SQLITE_IOERR / FULL / CORRUPT ...) on a READ
// is a miss: the member writes one progress line and returns "no row". The same on a WRITE is a
// skipped store: one progress line and IndexPut::Skipped, exactly where today's file cache would
// have published (the run goes on with the bytes it holds; the entry is simply not committed).
// Every other error (a constraint, a row that does not decode, a conflicting statistic) is
// returned as an Err and refuses as today's file cache refuses.
//
// Memory: PRAGMA cache_size is fixed at kCacheIndexPageCacheBytes per connection, counted inside
// the fixed 32 MiB slack of the IC runner's admission (strategy_ic_admission.cpp
// fixed_slack_bytes) and of the marginal verb's working bytes (its 32 MiB metadata term). Reads
// hold one row (signals, IC results) or one bounded page of pair rows (kPairPageRows) at a time.
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "atx/core/error.hpp"
#include "atx/core/types.hpp"
#include "atx/engine/research/store/rows_cache.hpp"

namespace atx::impl::strategy {

// --candidate-cache-index / --pair-cache-index. Files: today's JSON index files (the default and
// the meaning of an absent flag). Sqlite: rows of DIR/index.sqlite, legacy files read through.
enum class CacheIndexMode : atx::u8 { Files, Sqlite };
// "files" / "sqlite"; nullopt for anything else.
[[nodiscard]] std::optional<CacheIndexMode> parse_cache_index_mode(std::string_view text) noexcept;
// The `index` value the outputs record for an indexed cache.
inline constexpr std::string_view kCacheIndexSqlite = "sqlite";
// Whether DIR/index.sqlite exists as a regular file. Used ONLY to refuse
// `--*-cache-index sqlite` without an index (exit 2); never to select a backend.
[[nodiscard]] bool cache_index_present(const std::filesystem::path& dir);

// PRAGMA cache_size of every index connection (8 MiB).
inline constexpr atx::i64 kCacheIndexPageCacheBytes = 8LL << 20;
// Rows of one pair_stat page (a keyset page; about 1.5 MiB of row text).
inline constexpr atx::usize kPairPageRows = 4096;

// Lookups only (--plan-only: metadata, never a write) or lookups and stores.
enum class IndexAccess : atx::u8 { ReadWrite, ReadOnly };
// What a store did: inserted the row, found a row with its key (returned beside it, for the
// caller to compare), or skipped (a read-only index, or a write failure under the policy above).
enum class IndexPut : atx::u8 { Inserted, Present, Skipped };
template <class Row> struct IndexPutResult {
  IndexPut outcome{IndexPut::Skipped};
  std::optional<Row> existing; // set for Present
};
// put_pairs: the rows inserted (pairs already present with the same bits are not counted).
struct PairsPut {
  IndexPut outcome{IndexPut::Skipped};
  atx::usize inserted{};
};

class CacheIndex {
public:
  // Opens DIR/index.sqlite (never creates it: Err(NotFound) without one; a foreign or drifted
  // file refuses as open_cache refuses) and fixes its page cache. `progress` (nullable) receives
  // the policy's progress lines; the caller serializes its own use of that stream (the IC runner
  // and the marginal verb call from their main thread only). Moved-from: destroy or assign only.
  [[nodiscard]] static atx::core::Result<CacheIndex> open(const std::filesystem::path& dir,
                                                          std::ostream* progress,
                                                          IndexAccess access);
  ~CacheIndex(); // best-effort wal_checkpoint(TRUNCATE) of a read-write index (sql-design 3.3)
  CacheIndex(CacheIndex&& other) noexcept;
  CacheIndex& operator=(CacheIndex&& other) noexcept;
  CacheIndex(const CacheIndex&) = delete;
  CacheIndex& operator=(const CacheIndex&) = delete;

  [[nodiscard]] const std::filesystem::path& directory() const noexcept;
  [[nodiscard]] bool writable() const noexcept;
  // The '/'-path of `path` relative to directory() (the relpath columns: payload, entry_dir).
  // Err(InvalidArgument) for a path outside the directory.
  [[nodiscard]] atx::core::Result<std::string> relative(const std::filesystem::path& path) const;
  // One progress line "cache-index <file>: <table> <key>: <what>" (nothing without a stream):
  // a caller's own policy outcome, e.g. a row that fails its self-hash read as a miss.
  void note(std::string_view table, std::string_view key, std::string_view what) const;

  // ic_signal: the signal cache's commit marker (one row per signal key).
  [[nodiscard]] atx::core::Result<std::optional<engine::research::store::IcSignalRow>>
  find_signal(std::string_view key_sha256);
  // replace false: insert unless the key exists (Present returns that row); true: upsert.
  [[nodiscard]] atx::core::Result<IndexPutResult<engine::research::store::IcSignalRow>>
  put_signal(const engine::research::store::IcSignalRow& row, bool replace);

  // ic_result: one IC-result record per (entry_dir, id).
  [[nodiscard]] atx::core::Result<std::optional<engine::research::store::IcResultRow>>
  find_ic_result(std::string_view entry_dir, std::string_view id);
  [[nodiscard]] atx::core::Result<IndexPutResult<engine::research::store::IcResultRow>>
  put_ic_result(const engine::research::store::IcResultRow& row, bool replace);

  // pair_key / pair_stat: the marginal verb's pair statistics of one key.
  [[nodiscard]] atx::core::Result<std::optional<engine::research::store::PairKeyRow>>
  find_pair_key(std::string_view key_sha256);
  // One keyset page (select_pair_stats_page); nullopt when unreadable (a miss, policy above).
  [[nodiscard]] atx::core::Result<std::optional<std::vector<engine::research::store::PairStatRow>>>
  pair_stats_page(std::string_view key_sha256, std::string_view after_lo,
                  std::string_view after_hi);
  // One BEGIN IMMEDIATE: every pair (all of key `key_sha256`) ON CONFLICT DO NOTHING, then the key
  // row `key` unless null or present. A present pair must carry the same sum bits and dates and a
  // present key row the same version and text, else Err(InvalidArgument) and nothing is written
  // (two shards with different statistics refuse the same way today). The key row marks a key
  // as indexed (its legacy shards imported), so an import in several calls passes it last.
  [[nodiscard]] atx::core::Result<PairsPut> put_pairs(
      std::string_view key_sha256, std::span<const engine::research::store::PairStatRow> rows,
      const engine::research::store::PairKeyRow* key);

private:
  struct Impl;
  explicit CacheIndex(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace atx::impl::strategy
