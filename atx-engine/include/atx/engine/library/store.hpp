#pragma once

// atx::engine::library — LibraryStore: append-only segmented alpha store (S4-1).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  The disk-backed, append-only, immutable-segment SCALE-UP of
//  combine::AlphaStore. It is an LSM-inspired store:
//    * stage()  appends one evaluated alpha (+ its Provenance + canon hash) into
//               an in-memory combine::AlphaStore "memtable".
//    * flush()  seals the memtable into ONE immutable, mmap'able segment file
//               (record.hpp's one-pass write_segment_bytes), records it in the
//               sqlite SEGMENT CATALOG inside a Transaction, mmap-attaches it
//               (validate-before-expose), and resets the memtable.
//    * reads    dispatch over the UNION of sealed segments + the live memtable,
//               keyed by a GLOBAL AlphaId (segment base_alpha_id + local row).
//
//  Reopening a LibraryStore on an existing `dir` re-attaches every catalog
//  segment in id order, so the full pool survives a process restart (the
//  round-trip invariant).
//
// ===========================================================================
//  Threading / lifetime
// ===========================================================================
//  Thread-COMPATIBLE: the owning thread drives stage()/flush(); the sqlite
//  Database is owned per-LibraryStore and never shared across threads
//  (SQLITE_THREADSAFE=2 one-Database-per-thread rule). Sealed segments are
//  immutable after seal, so other threads may read them concurrently via their
//  own readers.
//
//  SAFETY: pnl()/positions() return spans that ALIAS a segment's Mapping (or the
//  live memtable's vectors). A segment span dangles when this store is destroyed;
//  a memtable span dangles on the next stage()/flush() (the AlphaStore growth/
//  reset rule). Copy out before the store grows or dies.

#include <algorithm>   // std::upper_bound
#include <cstring>     // std::memcpy
#include <fstream>     // std::ofstream (one-shot segment write)
#include <span>
#include <string>
#include <string_view> // std::string_view (sqlite binds)
#include <utility>     // std::move
#include <vector>

#include "atx/core/db/sqlite.hpp" // db::Database, Statement, Transaction
#include "atx/core/error.hpp"     // Result, Status, Ok, Err, ATX_TRY*
#include "atx/core/macro.hpp"     // ATX_ASSERT
#include "atx/core/types.hpp"     // i64, u32, u64, usize, f64

#include "atx/engine/combine/metrics.hpp" // combine::AlphaMetrics
#include "atx/engine/combine/store.hpp"   // combine::AlphaStore, AlphaId
#include "atx/engine/library/record.hpp"  // SegmentReaderLite, Provenance, write_segment_bytes

namespace atx::engine {
class ISignalSource; // non-owning re-eval handle (forward-declared, see combine/store.hpp)
} // namespace atx::engine

namespace atx::engine::library {

// ===========================================================================
//  AlphaRecordView — a read-back view of one alpha's metadata.
//
//  SAFETY: `provenance` is an owning copy (safe to keep); `metrics`/`canon_hash`
//  are by value. Returned by LibraryStore::get for any global AlphaId, whether
//  it lives in a sealed segment or the live memtable.
// ===========================================================================
struct AlphaRecordView {
  combine::AlphaMetrics metrics;
  atx::u64 canon_hash;
  Provenance provenance;
};

class LibraryStore {
public:
  /// Open (or create) the library rooted at `dir`: open the sqlite segment
  /// catalog (`<dir>/catalog.sqlite`), create its schema if absent, and
  /// re-attach every catalogued segment in id order, so the full pool survives a
  /// process restart. A failed open (unwritable dir, corrupt catalog) is an
  /// environment fault — ABORTED via ATX_ASSERT, since a half-open store has no
  /// valid use (S4-1 always opens a writable tmpdir). The catalog Database is
  /// opened in the init list (it has no default ctor); schema + re-attach run in
  /// the body.
  explicit LibraryStore(const std::string &dir)
      : dir_{dir}, catalog_{open_or_abort(catalog_path_for(dir))} {
    const auto st = init_schema_and_attach();
    ATX_ASSERT(st.has_value());
    (void)st;
  }

  /// Stage one evaluated alpha into the memtable. Returns its GLOBAL AlphaId
  /// (next_alpha_id_ + local memtable index). Propagates the AlphaStore::insert
  /// Err on a period/shape mismatch (the store is left unchanged on Err).
  [[nodiscard]] atx::core::Result<combine::AlphaId>
  stage(ISignalSource *source, std::span<const atx::f64> pnl,
        std::span<const atx::f64> positions_flat, combine::AlphaMetrics metrics,
        const Provenance &prov, atx::u64 canon_hash = 0) {
    ATX_TRY(const combine::AlphaId local, memtable_.insert(source, pnl, positions_flat, metrics));
    pending_prov_.push_back(prov);
    pending_canon_.push_back(canon_hash);
    return atx::core::Ok(combine::AlphaId{static_cast<atx::u32>(next_alpha_id_ + local.value)});
  }

  /// Seal the memtable into a new immutable segment + catalog row. COLD path
  /// (allocates, does I/O). A no-op (returns Ok) if nothing is staged.
  [[nodiscard]] atx::core::Status flush() {
    const atx::u32 n = static_cast<atx::u32>(memtable_.n_alphas());
    if (n == 0U) {
      return atx::core::Ok();
    }
    const atx::u64 base = next_alpha_id_;
    const auto seg_id = static_cast<atx::u32>(segments_.size());
    const std::string path = segment_file_path(seg_id);

    ATX_TRY(const auto bytes, build_segment_bytes(base));
    ATX_TRY_VOID(write_file(path, bytes));

    // Record the segment in the catalog (transactional) BEFORE mutating the
    // in-memory state, so a catalog failure leaves the store consistent.
    const atx::u32 crc = footer_crc(bytes);
    ATX_TRY_VOID(catalog_insert_segment(seg_id, path, base, n, crc));

    // mmap-attach the freshly sealed file (validate-before-expose).
    ATX_TRY(auto reader, SegmentReaderLite::attach(path));
    if (segments_.empty()) { // adopt the shape from the first sealed segment
      n_periods_ = memtable_.n_periods();
      n_instruments_ = memtable_.n_instruments();
    }
    seg_paths_.push_back(path);
    seg_bases_.push_back(base);
    segments_.push_back(std::move(reader));

    next_alpha_id_ += n;
    reset_memtable();
    return atx::core::Ok();
  }

  // --- read API over the UNION of sealed segments + live memtable -----------

  /// Total alphas: sealed (next_alpha_id_) + live memtable.
  [[nodiscard]] atx::u64 n_alphas() const noexcept {
    return next_alpha_id_ + memtable_.n_alphas();
  }
  [[nodiscard]] atx::usize n_segments() const noexcept { return segments_.size(); }
  /// Shared period count. Set by the first sealed segment; falls back to the live
  /// memtable when nothing is sealed yet (0 for a fully-empty store).
  [[nodiscard]] atx::usize n_periods() const noexcept {
    return n_periods_ != 0 ? n_periods_ : memtable_.n_periods();
  }
  [[nodiscard]] atx::usize n_instruments() const noexcept {
    return n_instruments_ != 0 ? n_instruments_ : memtable_.n_instruments();
  }

  /// Path of sealed segment `i` (i < n_segments()).
  [[nodiscard]] const std::string &segment_path(atx::usize i) const noexcept {
    ATX_ASSERT(i < seg_paths_.size());
    return seg_paths_[i];
  }

  /// Alpha `g`'s PnL stream (length n_periods()).
  /// SAFETY: aliases a segment Mapping (dangles when the store dies) or the live
  /// memtable (dangles on the next stage()/flush()). Copy out before growth.
  [[nodiscard]] std::span<const atx::f64> pnl(combine::AlphaId g) const noexcept {
    if (g.value >= next_alpha_id_) {
      return memtable_.pnl(combine::AlphaId{static_cast<atx::u32>(g.value - next_alpha_id_)});
    }
    const auto [seg, local] = locate(g);
    return segments_[seg].pnl_row(local);
  }

  /// Alpha `g`'s target-weight cross-section at `period` (length n_instruments()).
  /// SAFETY: same aliasing contract as pnl().
  [[nodiscard]] std::span<const atx::f64> positions(combine::AlphaId g,
                                                    atx::usize period) const noexcept {
    if (g.value >= next_alpha_id_) {
      return memtable_.positions(combine::AlphaId{static_cast<atx::u32>(g.value - next_alpha_id_)},
                                 period);
    }
    const auto [seg, local] = locate(g);
    return segments_[seg].pos_row(local, period);
  }

  /// Alpha `g`'s metadata (metrics + canon hash + provenance). For a memtable
  /// alpha the metrics come from the AlphaStore record and the provenance/canon
  /// from the pending buffers; for a sealed alpha they come from the segment's
  /// AlphaDirEntry + provenance blob.
  [[nodiscard]] AlphaRecordView get(combine::AlphaId g) const {
    if (g.value >= next_alpha_id_) {
      const atx::u32 local = static_cast<atx::u32>(g.value - next_alpha_id_);
      return AlphaRecordView{memtable_.get(combine::AlphaId{local}).metrics, pending_canon_[local],
                             pending_prov_[local]};
    }
    const auto [seg, local] = locate(g);
    const AlphaDirEntry &e = segments_[seg].dir_entry(local);
    return AlphaRecordView{e.metrics, e.canon_hash, segments_[seg].provenance(local)};
  }

private:
  // --- global-id dispatch ---------------------------------------------------

  /// Resolve a global AlphaId (< next_alpha_id_) to (segment index, local row)
  /// by binary-searching the ascending segment base ids.
  [[nodiscard]] std::pair<atx::usize, atx::u32> locate(combine::AlphaId g) const noexcept {
    ATX_ASSERT(g.value < next_alpha_id_);
    // Largest seg whose base <= g.value. upper_bound gives the first base > g,
    // so the target is the element before it.
    const auto it = std::upper_bound(seg_bases_.begin(), seg_bases_.end(),
                                     static_cast<atx::u64>(g.value));
    const atx::usize seg = static_cast<atx::usize>(it - seg_bases_.begin()) - 1U;
    const atx::u32 local = static_cast<atx::u32>(g.value - seg_bases_[seg]);
    return {seg, local};
  }

  // --- segment assembly -----------------------------------------------------

  /// Build the sealed-segment bytes for the current memtable at global base
  /// `base`. Gathers the memtable's flat pnl/pos + per-alpha metrics + the
  /// pending canon hashes + provenance into the record.hpp one-pass writer.
  [[nodiscard]] atx::core::Result<std::vector<std::byte>>
  build_segment_bytes(atx::u64 base) const {
    const atx::u32 n = static_cast<atx::u32>(memtable_.n_alphas());
    const atx::u64 t = memtable_.n_periods();
    const atx::u32 ni = static_cast<atx::u32>(memtable_.n_instruments());

    // Gather contiguous pnl/pos (the AlphaStore exposes per-row spans; flatten
    // them into one alpha-major / alpha->period->instrument buffer each).
    std::vector<atx::f64> pnl;
    std::vector<atx::f64> pos;
    pnl.reserve(static_cast<atx::usize>(n) * static_cast<atx::usize>(t));
    pos.reserve(static_cast<atx::usize>(n) * static_cast<atx::usize>(t) * ni);
    std::vector<combine::AlphaMetrics> metrics;
    metrics.reserve(n);
    for (atx::u32 a = 0; a < n; ++a) {
      const std::span<const atx::f64> row = memtable_.pnl(combine::AlphaId{a});
      pnl.insert(pnl.end(), row.begin(), row.end());
      for (atx::usize p = 0; p < t; ++p) {
        const std::span<const atx::f64> cs = memtable_.positions(combine::AlphaId{a}, p);
        pos.insert(pos.end(), cs.begin(), cs.end());
      }
      metrics.push_back(memtable_.get(combine::AlphaId{a}).metrics);
    }
    return atx::core::Ok(write_segment_bytes(n, ni, t, base, pnl, pos, metrics, pending_canon_,
                                             pending_prov_));
  }

  /// Extract the integrity crc from already-assembled segment bytes (the footer
  /// is the last sizeof(SegmentFooter) bytes; crc is its first u32 field).
  [[nodiscard]] static atx::u32 footer_crc(std::span<const std::byte> bytes) noexcept {
    SegmentFooter f{};
    std::memcpy(&f, bytes.data() + (bytes.size() - sizeof(SegmentFooter)), sizeof(f));
    return f.integrity_crc;
  }

  // --- filesystem -----------------------------------------------------------

  [[nodiscard]] std::string segment_file_path(atx::u32 seg_id) const {
    return dir_ + "/seg_" + std::to_string(seg_id) + ".alib";
  }

  /// Write `bytes` to `path` in one shot (truncate-create). Err(IoError) on fail.
  [[nodiscard]] static atx::core::Status write_file(const std::string &path,
                                                    std::span<const std::byte> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
      return atx::core::Err(atx::core::ErrorCode::IoError,
                            "LibraryStore: cannot open segment file for write");
    }
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      return atx::core::Err(atx::core::ErrorCode::IoError, "LibraryStore: segment file write failed");
    }
    return atx::core::Ok();
  }

  // --- sqlite catalog -------------------------------------------------------

  [[nodiscard]] static std::string catalog_path_for(const std::string &dir) {
    return dir + "/catalog.sqlite";
  }

  /// Open the catalog Database or ABORT (environment fault — see the ctor doc).
  /// Used in the member init list, where a Result cannot be propagated.
  [[nodiscard]] static atx::core::db::Database open_or_abort(const std::string &path) {
    auto db = atx::core::db::Database::open(path, atx::core::db::OpenMode::ReadWriteCreate);
    ATX_ASSERT(db.has_value());
    return std::move(*db);
  }

  /// Ensure the segment-catalog schema and re-attach existing segments in id
  /// order. Run from the ctor body after catalog_ is opened.
  [[nodiscard]] atx::core::Status init_schema_and_attach() {
    ATX_TRY_VOID(catalog_.exec("CREATE TABLE IF NOT EXISTS segments ("
                               " segment_id INTEGER PRIMARY KEY,"
                               " path TEXT NOT NULL,"
                               " base_alpha_id INTEGER NOT NULL,"
                               " n_alphas INTEGER NOT NULL,"
                               " crc INTEGER NOT NULL)"));
    ATX_TRY(auto stmt,
            catalog_.prepare("SELECT path, base_alpha_id, n_alphas FROM segments "
                             "ORDER BY segment_id ASC"));
    for (;;) {
      ATX_TRY(const auto step, stmt.step());
      if (step == atx::core::db::Statement::Step::Done) {
        break;
      }
      const std::string path{stmt.column_text(0)};
      const auto base = static_cast<atx::u64>(stmt.column_int(1));
      const auto n = static_cast<atx::u32>(stmt.column_int(2));
      ATX_TRY(auto reader, SegmentReaderLite::attach(path));
      if (segments_.empty()) { // adopt the shape from the first attached segment
        n_periods_ = static_cast<atx::usize>(reader.n_periods());
        n_instruments_ = static_cast<atx::usize>(reader.n_instruments());
      }
      seg_paths_.push_back(path);
      seg_bases_.push_back(base);
      segments_.push_back(std::move(reader));
      next_alpha_id_ = base + n;
    }
    return atx::core::Ok();
  }

  /// Insert one segment catalog row in a Transaction (RAII rollback on failure).
  [[nodiscard]] atx::core::Status catalog_insert_segment(atx::u32 seg_id, const std::string &path,
                                                         atx::u64 base, atx::u32 n, atx::u32 crc) {
    ATX_TRY(auto txn, atx::core::db::Transaction::begin(catalog_));
    ATX_TRY(auto *stmt, catalog_.prepare_cached(
                            "INSERT INTO segments (segment_id, path, base_alpha_id, n_alphas, crc) "
                            "VALUES (?1, ?2, ?3, ?4, ?5)"));
    ATX_TRY_VOID(stmt->bind(1, static_cast<atx::i64>(seg_id)));
    ATX_TRY_VOID(stmt->bind(2, std::string_view{path}));
    ATX_TRY_VOID(stmt->bind(3, static_cast<atx::i64>(base)));
    ATX_TRY_VOID(stmt->bind(4, static_cast<atx::i64>(n)));
    ATX_TRY_VOID(stmt->bind(5, static_cast<atx::i64>(crc)));
    ATX_TRY(const auto step, stmt->step());
    if (step != atx::core::db::Statement::Step::Done) {
      return atx::core::Err(atx::core::ErrorCode::Internal,
                            "LibraryStore: catalog insert did not complete");
    }
    return txn.commit();
  }

  /// Reset the memtable to empty. combine::AlphaStore has no clear(), so a fresh
  /// move-assigned instance is the reset primitive; the pending buffers clear.
  void reset_memtable() {
    memtable_ = combine::AlphaStore{};
    pending_prov_.clear();
    pending_canon_.clear();
  }

  std::string dir_;
  combine::AlphaStore memtable_;                 // staging buffer (the "memtable")
  std::vector<Provenance> pending_prov_;         // provenance per staged memtable row
  std::vector<atx::u64> pending_canon_;          // canon hash per staged memtable row
  std::vector<SegmentReaderLite> segments_;      // sealed segments (mmap-ro), id order
  std::vector<atx::u64> seg_bases_;              // segments_[i].base_alpha_id (ascending)
  std::vector<std::string> seg_paths_;           // segments_[i] file path
  atx::core::db::Database catalog_;              // sqlite segment catalog (one per thread)
  atx::u64 next_alpha_id_{0};                    // global id of the first memtable row
  atx::usize n_periods_{0};                      // shape (fixed once any data exists)
  atx::usize n_instruments_{0};
};

} // namespace atx::engine::library
