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
//  V2 uses a compact f32/sketch memtable and never retains dense positions.
//  positions_checked() requires a context/recipe-bound resolver after reopen.
//  PnL compatibility rows are decoded lazily; this read path is owner-thread only.
//
//  SAFETY: V1 pnl()/positions() spans ALIAS a segment's Mapping (or the
//  live memtable's vectors). A segment span dangles when this store is destroyed;
//  a memtable span dangles on the next stage()/flush() (the AlphaStore growth/
//  reset rule). Copy out before the store grows or dies.

#include <algorithm>   // std::upper_bound
#include <cstring>     // std::memcpy
#include <array>
#include <cmath>
#include <functional>
#include <limits>
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
  AlphaMetadata metadata{};
};

// A resolver must reproduce the position recipe using the exact context hash
// stored at admission. No expression-only/default portfolio inference is made.
struct PositionRequest {
  combine::AlphaId id;
  atx::usize period;
  atx::usize instruments;
  const Provenance& provenance;
  const AlphaMetadata& metadata;
};
using PositionResolver = std::function<atx::core::Result<std::vector<atx::f64>>(
    const PositionRequest&)>;
struct LibraryStorageOptions {
  LibraryStorageRule rule{LibraryStorageRule::ExistingOrLegacyV1};
  atx::usize instruments{0}; // permits staging V2 without a dense positions argument
  PositionResolver position_resolver;
  bool allow_recipe_migration{false}; // explicit existing-artifact migration only
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
  explicit LibraryStore(const std::string &dir, LibraryStorageOptions options = {})
      : dir_{dir}, options_{std::move(options)}, catalog_{open_or_abort(catalog_path_for(dir))} {
    const auto st = init_schema_and_attach();
    ATX_CHECK(st.has_value());
  }

  /// Stage one evaluated alpha into the memtable. Returns its GLOBAL AlphaId
  /// (next_alpha_id_ + local memtable index). Propagates the AlphaStore::insert
  /// Err on a period/shape mismatch (the store is left unchanged on Err).
  [[nodiscard]] atx::core::Result<combine::AlphaId>
  stage(ISignalSource *source, std::span<const atx::f64> pnl,
        std::span<const atx::f64> positions_flat, combine::AlphaMetrics metrics,
        const Provenance &prov, atx::u64 canon_hash = 0,
        const AlphaMetadata& metadata = {}, std::span<const atx::i16> signal_sketch = {}) {
    if (pnl.empty())
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: empty PnL history");
    if (n_alphas() >= std::numeric_limits<atx::u32>::max())
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: alpha id overflow");
    if (n_alphas() != 0U && pnl.size() != n_periods())
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: period mismatch");
    if (options_.rule == LibraryStorageRule::CompressedV2)
      return stage_compressed(pnl, positions_flat, metrics, prov, canon_hash, metadata, signal_sketch);
    if (metadata != AlphaMetadata{} || !signal_sketch.empty())
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: metadata/sketch requires V2");
    if (!extensions_.empty())
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library: period-extended store requires V2 staging");
    if (n_alphas() != 0U && (positions_flat.size() % pnl.size() != 0U ||
                              positions_flat.size() / pnl.size() != n_instruments()))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: instrument mismatch");
    ATX_TRY(const combine::AlphaId local, memtable_.insert(source, pnl, positions_flat, metrics));
    pending_prov_.push_back(prov);
    pending_canon_.push_back(canon_hash);
    return atx::core::Ok(combine::AlphaId{static_cast<atx::u32>(next_alpha_id_ + local.value)});
  }

  /// Seal the memtable into a new immutable segment + catalog row. COLD path
  /// (allocates, does I/O). A no-op (returns Ok) if nothing is staged.
  [[nodiscard]] atx::core::Status flush() {
    const atx::u32 n = static_cast<atx::u32>(pending_count());
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
      n_periods_ = static_cast<atx::usize>(reader.n_periods());
      n_instruments_ = reader.n_instruments();
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
    return next_alpha_id_ + pending_count();
  }
  [[nodiscard]] atx::usize n_segments() const noexcept { return segments_.size(); }
  /// Current period count, extended only by an explicit append_periods call.
  /// Set by the first sealed segment; falls back to the live
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
  [[nodiscard]] std::span<const atx::f64> pnl(combine::AlphaId g) const {
    ATX_CHECK(g.value < n_alphas());
    if (g.value >= next_alpha_id_ && options_.rule == LibraryStorageRule::CompressedV2) {
      const auto local = static_cast<atx::usize>(g.value - next_alpha_id_);
      auto& decoded = pending_decoded_[local];
      if (decoded.empty()) decoded.assign(compact_[local].pnl.begin(), compact_[local].pnl.end());
      return decoded;
    }
    if (g.value < next_alpha_id_ && !extensions_.empty()) return extended_pnl(g);
    if (g.value >= next_alpha_id_) {
      return memtable_.pnl(combine::AlphaId{static_cast<atx::u32>(g.value - next_alpha_id_)});
    }
    const auto [seg, local] = locate(g);
    return segments_[seg].pnl_row(local);
  }

  /// Alpha `g`'s target-weight cross-section at `period` (length n_instruments()).
  /// SAFETY: same aliasing contract as pnl().
  [[nodiscard]] std::span<const atx::f64> positions(combine::AlphaId g,
                                                    atx::usize period) const {
    ATX_CHECK(g.value < n_alphas() && period < n_periods());
    if (requires_positions(g, period)) {
      auto result = positions_checked(g, period);
      ATX_CHECK(result.has_value()); // old span API cannot transport resolver errors
      resolved_positions_ = std::move(*result);
      return resolved_positions_; // valid until next positions() call
    }
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
    ATX_CHECK(g.value < n_alphas());
    if (g.value >= next_alpha_id_ && options_.rule == LibraryStorageRule::CompressedV2) {
      const auto& r = compact_[static_cast<atx::usize>(g.value - next_alpha_id_)];
      return {r.metrics, r.canon_hash, r.provenance, r.metadata};
    }
    if (g.value >= next_alpha_id_) {
      const atx::u32 local = static_cast<atx::u32>(g.value - next_alpha_id_);
      return AlphaRecordView{memtable_.get(combine::AlphaId{local}).metrics, pending_canon_[local],
                             pending_prov_[local]};
    }
    const auto [seg, local] = locate(g);
    const AlphaDirEntry &e = segments_[seg].dir_entry(local);
    return AlphaRecordView{e.metrics, e.canon_hash, segments_[seg].provenance(local),
                           segments_[seg].metadata(local)};
  }

  [[nodiscard]] LibraryStorageRule storage_rule() const noexcept { return options_.rule; }
  [[nodiscard]] std::span<const atx::i16> signal_sketch(combine::AlphaId g) const noexcept {
    ATX_CHECK(g.value < n_alphas());
    if (g.value >= next_alpha_id_) {
      if (options_.rule == LibraryStorageRule::LegacyDenseV1) return {};
      return compact_[static_cast<atx::usize>(g.value - next_alpha_id_)].signal_sketch;
    }
    const auto [seg, local] = locate(g);
    return segments_[seg].signal_sketch(local);
  }
  void set_position_resolver(PositionResolver resolver) {
    options_.position_resolver = std::move(resolver);
  }
  [[nodiscard]] atx::core::Result<std::vector<atx::f64>>
  positions_checked(combine::AlphaId g, atx::usize period) const {
    if (g.value >= n_alphas() || period >= n_periods())
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: position bounds");
    if (n_instruments() == 0U) return atx::core::Ok(std::vector<atx::f64>{});
    if (!requires_positions(g, period)) {
      const auto row = positions(g, period);
      return atx::core::Ok(std::vector<atx::f64>{row.begin(), row.end()});
    }
    const auto record = get(g);
    if (!options_.position_resolver || record.metadata.context_hash == 0U ||
        record.metadata.position_recipe_hash == 0U)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library: positions require a bound context/recipe resolver");
    ATX_TRY(auto weights, options_.position_resolver(
        PositionRequest{g, period, n_instruments(), record.provenance, record.metadata}));
    if (weights.size() != n_instruments() ||
        std::any_of(weights.begin(), weights.end(), [](auto x) { return std::isinf(x); }))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library: resolver returned invalid position geometry/values");
    return atx::core::Ok(std::move(weights));
  }

  // Append an immutable time slab for every CURRENT alpha. New alphas admitted
  // later provide the entire extended history; old sealed bytes never change.
  // The catalog transaction publishes a contiguous [start,start+periods) slab.
  [[nodiscard]] atx::core::Status append_periods(std::span<const atx::f64> pnl_alpha_major,
                                                atx::usize periods) {
    atx::u64 cells = 0, end = 0;
    if (options_.rule != LibraryStorageRule::CompressedV2 || periods == 0U || n_alphas() == 0U ||
        !detail::checked_mul(n_alphas(), periods, cells) || cells != pnl_alpha_major.size() ||
        !detail::checked_add(n_periods(), periods, end) ||
        end > static_cast<atx::u64>(std::numeric_limits<atx::i64>::max()))
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library: invalid period extension geometry/mode");
    std::vector<CompressedAlphaRecord> rows(static_cast<atx::usize>(n_alphas()));
    for (atx::usize a = 0; a < rows.size(); ++a) {
      ATX_TRY(auto values, compress_pnl(pnl_alpha_major.subspan(a * periods, periods)));
      rows[a].pnl = std::move(values);
    }
    ATX_TRY(auto bytes, write_compressed_segment_bytes(
        static_cast<atx::u32>(n_instruments()), periods, 0, rows));
    ATX_TRY_VOID(flush());
    const std::string path = dir_ + "/period_" + std::to_string(extensions_.size()) + ".alib";
    ATX_TRY_VOID(write_file(path, bytes));
    ATX_TRY(auto reader, SegmentReaderLite::attach(path));
    ATX_TRY(auto txn, atx::core::db::Transaction::begin(catalog_));
    ATX_TRY(auto stmt, catalog_.prepare(
        "INSERT INTO period_extensions(start_period,path,n_periods,n_alphas,crc) VALUES(?1,?2,?3,?4,?5)"));
    ATX_TRY_VOID(stmt.bind(1, static_cast<atx::i64>(n_periods())));
    ATX_TRY_VOID(stmt.bind(2, std::string_view{path}));
    ATX_TRY_VOID(stmt.bind(3, static_cast<atx::i64>(periods)));
    ATX_TRY_VOID(stmt.bind(4, static_cast<atx::i64>(n_alphas())));
    ATX_TRY_VOID(stmt.bind(5, static_cast<atx::i64>(reader.integrity_crc())));
    ATX_TRY(const auto step, stmt.step());
    if (step != atx::core::db::Statement::Step::Done)
      return atx::core::Err(atx::core::ErrorCode::Internal, "library: append catalog failed");
    ATX_TRY_VOID(txn.commit());
    extensions_.push_back(Extension{n_periods(), std::move(reader)});
    n_periods_ = static_cast<atx::usize>(end);
    for (const auto& segment : segments_) segment.clear_pnl_cache();
    extended_.clear(); extended_.resize(static_cast<atx::usize>(next_alpha_id_));
    return atx::core::Ok();
  }

  // The recipe is durable even when no new alpha is admitted. requested=0 means
  // use the saved rule, or V1 for metadata-less nonempty libraries, V2 for new.
  // Seed/rule changes require an explicit migration option, never a silent reopen.
  [[nodiscard]] atx::core::Result<atx::u32>
  bind_index_recipe(atx::u32 requested, atx::u64 seed, bool allow_migration = false) {
    ATX_TRY(auto old, metadata_value("corr_recipe"));
    atx::u32 selected = requested;
    if (selected == 0U) {
      if (old.empty()) selected = n_alphas() == 0U ? 2U : 1U;
      else { atx::usize off = 0; if (!detail::get_le(old, off, selected))
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: bad index recipe"); }
    }
    if (selected != 1U && selected != 2U)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: unknown index rule");
    std::vector<std::byte> recipe;
    detail::put_le(recipe, selected); detail::put_le(recipe, selected); // rule + projection recipe
    detail::put_le(recipe, selected == 1U ? atx::u32{64} : atx::u32{256});
    detail::put_le(recipe, seed);
    if ((!old.empty() && old != recipe) ||
        (old.empty() && n_alphas() != 0U && selected != 1U)) {
      if (!allow_migration)
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "library: index recipe changed; explicit migration required");
    }
    if (old != recipe) ATX_TRY_VOID(set_metadata("corr_recipe", recipe));
    recipe_crc_ = atx::tsdb::crc32(recipe.data(), recipe.size());
    bind_recipe_identity_ = selected != 1U;
    return atx::core::Ok(selected);
  }

  [[nodiscard]] atx::u32 record_crc(combine::AlphaId g) const {
    ATX_CHECK(g.value < next_alpha_id_);
    const auto [seg, local] = locate(g); (void)local;
    const auto& reader = segments_[seg];
    if (reader.format_version() == 1U && extensions_.empty() && !bind_recipe_identity_)
      return reader.integrity_crc();
    std::vector<std::byte> identity;
    detail::put_le(identity, reader.integrity_crc());
    detail::put_le(identity, recipe_crc_);
    for (const auto& extension : extensions_)
      if (g.value < extension.reader.n_alphas() && extension.start >= reader.n_periods()) {
        detail::put_le(identity, extension.start);
        detail::put_le(identity, extension.reader.integrity_crc());
      }
    return atx::tsdb::crc32(identity.data(), identity.size());
  }

private:
  [[nodiscard]] atx::usize pending_count() const noexcept {
    return options_.rule == LibraryStorageRule::CompressedV2 ? compact_.size() : memtable_.n_alphas();
  }
  [[nodiscard]] static atx::core::Result<std::vector<atx::f32>>
  compress_pnl(std::span<const atx::f64> pnl) {
    std::vector<atx::f32> out; out.reserve(pnl.size());
    for (const auto x : pnl) {
      if (std::isinf(x) || (std::isfinite(x) && std::abs(x) > std::numeric_limits<atx::f32>::max()))
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: PnL outside f32 range");
      out.push_back(static_cast<atx::f32>(x));
    }
    return atx::core::Ok(std::move(out));
  }
  [[nodiscard]] atx::core::Result<combine::AlphaId>
  stage_compressed(std::span<const atx::f64> pnl, std::span<const atx::f64> positions_flat,
                   combine::AlphaMetrics metrics, const Provenance& prov, atx::u64 canon,
                   const AlphaMetadata& metadata, std::span<const atx::i16> sketch) {
    if (pnl.empty() || sketch.size() > kSignalSketchCapacity ||
        !std::isfinite(metadata.tau) || metadata.tau < 0.0 ||
        !std::isfinite(metadata.sketch_scale) || metadata.sketch_scale <= 0.0)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: invalid compact record");
    atx::usize instruments = n_alphas() == 0U ? options_.instruments : n_instruments();
    if (!positions_flat.empty()) {
      if (positions_flat.size() % pnl.size() != 0U ||
          ((n_alphas() != 0U || instruments != 0U) && positions_flat.size() / pnl.size() != instruments))
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: position geometry mismatch");
      instruments = positions_flat.size() / pnl.size();
    }
    if (instruments > std::numeric_limits<atx::u32>::max())
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: instrument count overflow");
    ATX_TRY(auto values, compress_pnl(pnl));
    const auto id = combine::AlphaId{static_cast<atx::u32>(n_alphas())};
    compact_.push_back(CompressedAlphaRecord{metrics, canon, prov, metadata,
        std::move(values), std::vector<atx::i16>{sketch.begin(), sketch.end()}});
    pending_decoded_.emplace_back();
    n_periods_ = pnl.size(); n_instruments_ = instruments;
    return atx::core::Ok(id);
  }
  [[nodiscard]] bool requires_positions(combine::AlphaId g, atx::usize period) const noexcept {
    if (g.value >= next_alpha_id_) return options_.rule == LibraryStorageRule::CompressedV2;
    const auto [seg, local] = locate(g); (void)local;
    return segments_[seg].format_version() == 2U || period >= segments_[seg].n_periods();
  }
  [[nodiscard]] std::span<const atx::f64> extended_pnl(combine::AlphaId g) const {
    auto& row = extended_[g.value];
    if (row.empty()) {
      const auto [seg, local] = locate(g);
      const auto original_periods = segments_[seg].n_periods();
      if (segments_[seg].format_version() == 2U) {
        const auto original = segments_[seg].pnl_f32_row(local);
        row.assign(original.begin(), original.end());
      } else {
        const auto original = segments_[seg].pnl_row(local);
        row.assign(original.begin(), original.end());
      }
      row.reserve(n_periods());
      for (const auto& extension : extensions_)
        if (g.value < extension.reader.n_alphas() && extension.start >= original_periods) {
          ATX_CHECK(extension.start == row.size());
          const auto tail = extension.reader.pnl_f32_row(g.value);
          row.insert(row.end(), tail.begin(), tail.end());
        }
      ATX_CHECK(row.size() == n_periods());
    }
    return row;
  }
  [[nodiscard]] atx::core::Result<std::vector<std::byte>> metadata_value(std::string_view key) {
    ATX_TRY(auto stmt, catalog_.prepare("SELECT value FROM library_metadata WHERE key=?1"));
    ATX_TRY_VOID(stmt.bind(1, key));
    ATX_TRY(const auto step, stmt.step());
    if (step == atx::core::db::Statement::Step::Done)
      return atx::core::Ok(std::vector<std::byte>{});
    const auto bytes = stmt.column_blob(0);
    return atx::core::Ok(std::vector<std::byte>{bytes.begin(), bytes.end()});
  }
  [[nodiscard]] atx::core::Status set_metadata(std::string_view key,
                                               std::span<const std::byte> value) {
    ATX_TRY(auto stmt, catalog_.prepare(
        "INSERT INTO library_metadata(key,value) VALUES(?1,?2) "
        "ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    ATX_TRY_VOID(stmt.bind(1, key)); ATX_TRY_VOID(stmt.bind(2, value));
    ATX_TRY(const auto step, stmt.step());
    if (step != atx::core::db::Statement::Step::Done)
      return atx::core::Err(atx::core::ErrorCode::Internal, "library: metadata write failed");
    return atx::core::Ok();
  }

  // --- global-id dispatch ---------------------------------------------------

  /// Resolve a global AlphaId (< next_alpha_id_) to (segment index, local row)
  /// by binary-searching the ascending segment base ids.
  [[nodiscard]] std::pair<atx::usize, atx::u32> locate(combine::AlphaId g) const noexcept {
    ATX_ASSERT(g.value < next_alpha_id_);
    // Precondition for the (it - begin) - 1 step below: there is at least one
    // segment and g lands at or after the first segment's base (segment 0's base
    // is always 0, so this holds for any sealed id, but guard it explicitly so a
    // future non-zero base 0 can't underflow `seg` to SIZE_MAX).
    ATX_ASSERT(!seg_bases_.empty() && g.value >= seg_bases_.front());
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
    if (options_.rule == LibraryStorageRule::CompressedV2)
      return write_compressed_segment_bytes(static_cast<atx::u32>(n_instruments()),
                                             n_periods(), base, compact_);
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
      if (base != next_alpha_id_ || reader.base_alpha_id() != base || reader.n_alphas() != n ||
          (!segments_.empty() && reader.n_instruments() != n_instruments_))
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: catalog/segment mismatch");
      if (segments_.empty()) { // adopt the shape from the first attached segment
        n_periods_ = static_cast<atx::usize>(reader.n_periods());
        n_instruments_ = static_cast<atx::usize>(reader.n_instruments());
      }
      seg_paths_.push_back(path);
      seg_bases_.push_back(base);
      segments_.push_back(std::move(reader));
      next_alpha_id_ = base + n;
    }
    ATX_TRY_VOID(init_extensions_and_metadata());
    return atx::core::Ok();
  }

  [[nodiscard]] atx::core::Status init_extensions_and_metadata() {
    ATX_TRY_VOID(catalog_.exec("CREATE TABLE IF NOT EXISTS library_metadata ("
                               "key TEXT PRIMARY KEY,value BLOB NOT NULL)"));
    ATX_TRY_VOID(catalog_.exec("CREATE TABLE IF NOT EXISTS period_extensions ("
                               "start_period INTEGER PRIMARY KEY,path TEXT NOT NULL,"
                               "n_periods INTEGER NOT NULL,n_alphas INTEGER NOT NULL,crc INTEGER NOT NULL)"));
    ATX_TRY(auto saved, metadata_value("storage_rule"));
    if (options_.rule == LibraryStorageRule::ExistingOrLegacyV1) {
      if (saved.empty()) options_.rule = segments_.empty() || segments_.back().format_version() == 1U
          ? LibraryStorageRule::LegacyDenseV1 : LibraryStorageRule::CompressedV2;
      else if (saved.size() == 1U) options_.rule = static_cast<LibraryStorageRule>(saved[0]);
    }
    const auto requested = static_cast<atx::u8>(options_.rule);
    if (requested != 1U && requested != 2U)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: unknown storage rule");
    if (!saved.empty() && (saved.size() != 1U || saved[0] != static_cast<std::byte>(requested)) &&
        !options_.allow_recipe_migration)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library: storage recipe changed; explicit migration required");
    if (saved.empty() && !segments_.empty() && segments_.back().format_version() == 1U &&
        requested == 2U && !options_.allow_recipe_migration)
      return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                            "library: legacy storage migration must be explicit");
    const std::array<std::byte, 1> recipe{static_cast<std::byte>(requested)};
    ATX_TRY(auto stmt, catalog_.prepare(
        "SELECT start_period,path,n_periods,n_alphas,crc FROM period_extensions ORDER BY start_period"));
    for (;;) {
      ATX_TRY(const auto step, stmt.step());
      if (step == atx::core::db::Statement::Step::Done) break;
      const auto start = stmt.column_int(0), periods = stmt.column_int(2), count = stmt.column_int(3);
      ATX_TRY(auto reader, SegmentReaderLite::attach(std::string{stmt.column_text(1)}));
      if (start < 0 || periods <= 0 || count <= 0 || static_cast<atx::u64>(start) != n_periods_ ||
          static_cast<atx::u64>(count) > next_alpha_id_ || reader.format_version() != 2U ||
          reader.base_alpha_id() != 0U || reader.n_periods() != static_cast<atx::u64>(periods) ||
          reader.n_alphas() != static_cast<atx::u64>(count) || reader.n_instruments() != n_instruments_ ||
          reader.integrity_crc() != static_cast<atx::u64>(stmt.column_int(4)) ||
          static_cast<atx::u64>(periods) > std::numeric_limits<atx::usize>::max() - n_periods_)
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: invalid period catalog");
      extensions_.push_back(Extension{n_periods_, std::move(reader)});
      n_periods_ += static_cast<atx::usize>(periods);
    }
    for (const auto& segment : segments_) {
      atx::u64 end = segment.n_periods();
      for (const auto& extension : extensions_) {
        if (extension.start < segment.n_periods()) continue;
        if (extension.start != end ||
            segment.base_alpha_id() + segment.n_alphas() > extension.reader.n_alphas())
          return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                                "library: period slab does not cover existing alpha history");
        end += extension.reader.n_periods();
      }
      if (end != n_periods_)
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument, "library: incompatible segment periods");
    }
    ATX_TRY(auto saved_index, metadata_value("corr_recipe"));
    if (!saved_index.empty()) {
      atx::usize off = 0;
      atx::u32 rule = 0, version = 0, bits = 0; atx::u64 seed = 0;
      if (!detail::get_le(saved_index, off, rule) || !detail::get_le(saved_index, off, version) ||
          !detail::get_le(saved_index, off, bits) || !detail::get_le(saved_index, off, seed) ||
          off != saved_index.size() || (rule != 1U && rule != 2U) || version != rule ||
          bits != (rule == 1U ? 64U : 256U))
        return atx::core::Err(atx::core::ErrorCode::InvalidArgument,
                              "library: malformed saved index recipe");
      recipe_crc_ = atx::tsdb::crc32(saved_index.data(), saved_index.size());
      bind_recipe_identity_ = rule != 1U;
    }
    ATX_TRY_VOID(set_metadata("storage_rule", recipe));
    extended_.resize(static_cast<atx::usize>(next_alpha_id_));
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
    compact_.clear(); pending_decoded_.clear();
    extended_.resize(static_cast<atx::usize>(next_alpha_id_));
  }

  struct Extension { atx::usize start; SegmentReaderLite reader; };
  std::string dir_;
  LibraryStorageOptions options_;
  std::vector<CompressedAlphaRecord> compact_;
  mutable std::vector<std::vector<atx::f64>> pending_decoded_;
  std::vector<Extension> extensions_;
  mutable std::vector<std::vector<atx::f64>> extended_;
  mutable std::vector<atx::f64> resolved_positions_;
  atx::u32 recipe_crc_{0};
  bool bind_recipe_identity_{false};
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
