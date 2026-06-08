#pragma once

// atx::engine::library — DedupIndex: library-wide canonical-hash dedup index (S4-2).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The cross-run, cross-segment dedup gate for the disk-backed library. It is
//  the PERSISTENT superset of factory::CanonSet: where CanonSet is an in-process
//  std::unordered_set<u64> that loses its membership between runs, DedupIndex
//  fronts an in-memory HashMap<canon_hash, AlphaId> with a sqlite table, so the
//  dedup set survives a process restart and spans every sealed segment.
//
//  The key is the S3 factory::canonical_hash — a STABLE (cross-run, cross-
//  platform) and SOUND (hash-equal ⇒ bit-identical VM eval) structural hash. The
//  index is PURE-u64-KEYED: it never calls canonical_hash itself (the admit path
//  computes the hash and hands it in), so this unit has no dependency on the
//  factory/alpha layers — only atx::core (db + container + error + types) and the
//  combine AlphaId handle.
//
//  insert(h, id) returns:
//    * Ok(true)  — h was NEW; the (h, id) row was admitted library-wide.
//    * Ok(false) — h was already present (a structural duplicate); the caller
//                  must SKIP re-evaluation / re-admission. The first id wins.
//    * Err(...)  — a sqlite fault (I/O, corruption). Never an abort.
//
// ===========================================================================
//  Soundness contract (the S4 risk: a dedup that drops a distinct alpha or fails
//  to reject a true dup)
// ===========================================================================
//  Correctness rests entirely on canonical_hash's F6 soundness (computed by the
//  caller). This unit's only job is to not CORRUPT that key on the way to/from
//  disk: the u64 hash is stored in a sqlite INTEGER (a signed i64) column, so it
//  is bit_cast to/from i64 — preserving all 64 bits, including high-bit-set
//  hashes (a naive static_cast<i64> of a hash ≥ 2^63 is implementation-defined
//  in the round trip; std::bit_cast is value-preserving and explicit). The UNIQUE
//  PRIMARY KEY on canon_hash makes dedup library-wide and race-safe: a duplicate
//  INSERT OR IGNORE changes 0 rows, which we report as Ok(false).
//
// ===========================================================================
//  Threading / determinism
// ===========================================================================
//  Thread-COMPATIBLE: owned by the serial admit path. The sqlite Database is
//  one-per-thread (SQLITE_THREADSAFE=2 rule) and never shared. The in-memory
//  cache is rebuilt at open by SELECTing the table ORDER BY alpha_id, so the
//  rebuild order is deterministic (L7) and independent of sqlite's b-tree order.

#include <bit>     // std::bit_cast (u64 <-> i64, all-bits-preserving)
#include <optional>
#include <string>
#include <utility> // std::move

#include "atx/core/container/hash_map.hpp" // HashMap (identity-hashed u64 cache)
#include "atx/core/db/sqlite.hpp"          // db::Database, Statement, Transaction
#include "atx/core/error.hpp"              // Result, Status, Ok, Err, ATX_TRY*
#include "atx/core/macro.hpp"              // ATX_ASSERT
#include "atx/core/types.hpp"              // i64, u32, u64

#include "atx/engine/combine/store.hpp" // combine::AlphaId

namespace atx::engine::library {

// Identity hasher for the cache. The KEY is an already-mixed canonical_hash, so
// re-hashing it (the default ankerl wyhash) would waste cycles and could only
// add collisions — return the key verbatim and let the open-addressing table
// bucket it directly.
struct IdentityHash {
  using is_avalanching = void; // tell ankerl the key is already well-distributed
  [[nodiscard]] atx::u64 operator()(atx::u64 k) const noexcept { return k; }
};

class DedupIndex {
public:
  /// Open (or create) the dedup index rooted at `dir`: open the sqlite db file
  /// (`<dir>/dedup.db`), create the `dedup` table if absent, and rebuild the
  /// in-memory cache from the table in alpha_id order (deterministic). A failed
  /// open (unwritable dir, corrupt db) is an environment fault — ABORTED via
  /// ATX_ASSERT, mirroring LibraryStore: a half-open index has no valid use, and
  /// S4 always opens a writable tmpdir. The Database is opened in the init list
  /// (it has no default ctor); schema + cache rebuild run in the body.
  explicit DedupIndex(const std::string &dir) : db_{open_or_abort(db_path_for(dir))} {
    const auto st = init_schema_and_load();
    ATX_ASSERT(st.has_value());
    (void)st;
  }

  /// True iff `h` is already in the index (in-memory cache lookup; the cache is a
  /// complete mirror of the table after open, so no db round-trip is needed).
  [[nodiscard]] bool contains(atx::u64 h) const { return cache_.contains(h); }

  /// The AlphaId mapped to `h`, or nullopt if `h` is unseen.
  [[nodiscard]] std::optional<combine::AlphaId> find(atx::u64 h) const {
    const auto it = cache_.find(h);
    if (it == cache_.end()) {
      return std::nullopt;
    }
    return it->second;
  }

  /// Admit `(h, id)`. Returns Ok(true) iff `h` was NEW (the row was inserted);
  /// Ok(false) iff `h` was already present (a structural duplicate — the FIRST
  /// id wins and is left untouched). Err on a sqlite fault.
  ///
  /// The cache_ pre-check short-circuits the common dup case without touching
  /// sqlite; the INSERT OR IGNORE + changes()==0 check is the authoritative,
  /// library-wide (and race-safe) dedup via the UNIQUE PRIMARY KEY. The cache is
  /// updated ONLY after the db insert reports a real change, so cache and table
  /// never disagree.
  [[nodiscard]] atx::core::Result<bool> insert(atx::u64 h, combine::AlphaId id) {
    if (cache_.contains(h)) {
      return atx::core::Ok(false); // known duplicate — skip the db entirely
    }
    ATX_TRY(auto *stmt,
            db_.prepare_cached("INSERT OR IGNORE INTO dedup (canon_hash, alpha_id) "
                               "VALUES (?1, ?2)"));
    ATX_TRY_VOID(stmt->bind(1, std::bit_cast<atx::i64>(h)));
    ATX_TRY_VOID(stmt->bind(2, static_cast<atx::i64>(id.value)));
    ATX_TRY(const auto step, stmt->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "DedupIndex: insert did not complete");
    }
    if (db_.changes() == 0) {
      return atx::core::Ok(false); // raced / already present at the db layer
    }
    cache_.insert_or_assign(h, id);
    return atx::core::Ok(true);
  }

  /// Number of distinct canonical hashes in the index.
  [[nodiscard]] atx::u64 size() const noexcept {
    return static_cast<atx::u64>(cache_.size());
  }

private:
  [[nodiscard]] static std::string db_path_for(const std::string &dir) {
    return dir + "/dedup.db";
  }

  /// Open the dedup Database or ABORT (environment fault — see the ctor doc).
  /// Used in the member init list, where a Result cannot be propagated.
  [[nodiscard]] static atx::core::db::Database open_or_abort(const std::string &path) {
    auto db = atx::core::db::Database::open(path, atx::core::db::OpenMode::ReadWriteCreate);
    ATX_ASSERT(db.has_value());
    return std::move(*db);
  }

  /// Ensure the dedup schema and rebuild the in-memory cache from the table in
  /// alpha_id order (deterministic; L7). canon_hash is the UNIQUE PRIMARY KEY so
  /// the dedup is library-wide. Run from the ctor body after db_ is opened.
  [[nodiscard]] atx::core::Status init_schema_and_load() {
    ATX_TRY_VOID(db_.exec("CREATE TABLE IF NOT EXISTS dedup ("
                          " canon_hash INTEGER PRIMARY KEY,"
                          " alpha_id   INTEGER NOT NULL)"));
    ATX_TRY(auto stmt,
            db_.prepare("SELECT canon_hash, alpha_id FROM dedup ORDER BY alpha_id ASC"));
    for (;;) {
      ATX_TRY(const auto step, stmt.step());
      if (step == atx::core::db::Statement::Step::Done) {
        break;
      }
      const atx::u64 h = std::bit_cast<atx::u64>(stmt.column_int(0)); // i64 -> u64, all bits
      const auto id = static_cast<atx::u32>(stmt.column_int(1));
      cache_.insert_or_assign(h, combine::AlphaId{id});
    }
    return atx::core::Ok();
  }

  // The in-mem mirror: u64 canonical hash -> first AlphaId that claimed it. Keyed
  // by the IDENTITY hasher (the key is already a mixed hash).
  atx::core::container::HashMap<atx::u64, combine::AlphaId, IdentityHash> cache_;
  atx::core::db::Database db_; // sqlite dedup table (one per thread)
};

} // namespace atx::engine::library
