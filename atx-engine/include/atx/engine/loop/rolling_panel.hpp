#pragma once

// atx::engine::loop — RollingPanel: the point-in-time bar->panel bridge (P2-2).
//
// ===========================================================================
//  Why this type exists
// ===========================================================================
//  RollingPanel is the integration seam between the Phase-1 event spine (a
//  chronological stream of sealed bars) and the Phase-3 alpha VM (which reasons
//  over a `date x instrument` matrix). It is a BOUNDED trailing ring of
//  cross-sections: each appended row is one timestamp's slice across the fixed
//  universe, stored column-major per field. The loop WRITES a row only after its
//  bar closes and READS the trailing window before the next decision.
//
//  This type owns the DATA-side half of the no-look-ahead guarantee. Its mirror
//  is the SimClock, which owns the CLOCK-side half: the clock cannot move
//  backward, and this panel has no API to write a bar that has not yet closed.
//
// ===========================================================================
//  The point-in-time (no-look-ahead) contract
// ===========================================================================
//  The TEMPORAL GATE IS STRUCTURAL. The panel only ever contains rows passed to
//  `append_sealed_row`, and the loop calls that ONLY after a bar's end_time has
//  passed the clock (`end_time <= now()`). There is deliberately NO method to
//  write an in-progress or future bar — the current bar is unrepresentable, so
//  it cannot leak into a strategy's view. `view()` exposes exactly the last
//  `max_lookback` sealed rows, newest-first. A not-yet-appended (t+1) bar is
//  invisible because it does not exist in the ring.
//
// ===========================================================================
//  Memory layout & budget
// ===========================================================================
//  ONE heap allocation at construction, L2-cache-line aligned, holding:
//
//    fields_ : kPanelFieldCount * Cap * n_inst  f64   (column-major per field)
//    mask_   : Cap * ceil(n_inst / 64)          u64   (per-row membership bitmap)
//
//  The field block is laid out [field][row][instrument]: one cross-section row
//  is `n_inst` contiguous f64 within a field block, so a per-field cross-section
//  read is sequential and prefetch-friendly (the plan's "column-major per
//  field"). A cell's value for an absent instrument is a quiet NaN; its mask bit
//  is 0. Total fixed budget:
//
//    Cap * n_inst * kPanelFieldCount * 8 B  +  Cap * ceil(n_inst/64) * 8 B
//
//  ZERO allocation on append_sealed_row and view() — both are O(n_instruments)
//  and O(1) respectively over the pre-sized buffer.
//
// ===========================================================================
//  Universe & membership
// ===========================================================================
//  The universe (column set) is FIXED at construction. An InstrumentId -> column
//  index map is built once as a SORTED (id, column) array; per-row writes are
//  O(log n_inst) binary-search lookups. A sorted array (not a hash map) is used
//  because InstrumentId == atx::core::domain::Symbol has no std::hash and the
//  array is deterministic, allocation-light, and cache-friendly for the modest
//  universes a single alpha trades. A SliceRow whose id is not in the universe
//  is IGNORED (no write) — safer than asserting, because
//  a feed legitimately may carry instruments outside a strategy's chosen
//  universe, and silently dropping them keeps the membership mask point-in-time.
//  A universe member absent from a given slice yields a NaN cell with mask bit 0.
//  Survivorship: a delisted instrument's final bar is written normally (it
//  traded that session); it simply stops appearing in later slices.
//
// ===========================================================================
//  Ownership / threading
// ===========================================================================
//  Owns its ring buffer via a unique_ptr with a custom aligned-free deleter
//  (Rule of Zero at the class level: the smart pointer manages the resource).
//  The universe span is non-owning — the caller's InstrumentId storage must
//  outlive the panel (it is re-exposed verbatim through PanelView::universe()).
//  Single-threaded backtest use; no internal synchronisation.

#include <algorithm> // std::sort, std::lower_bound
#include <cstddef>   // std::byte (aligned storage element type)
#include <limits>    // std::numeric_limits (constexpr quiet NaN sentinel)
#include <memory>    // std::unique_ptr (RAII over the aligned ring allocation)
#include <span>      // std::span (universe + slice rows)
#include <vector>    // std::vector (construction-time sorted index)

#include "atx/core/aligned.hpp"       // aligned_alloc_bytes / aligned_free
#include "atx/core/domain/domain.hpp" // atx::core::domain::Bar (write_cell input)
#include "atx/core/macro.hpp"         // ATX_ASSERT
#include "atx/core/platform.hpp"      // kCacheLineSize
#include "atx/core/types.hpp"         // usize, u32, u64, f64

#include "atx/engine/loop/panel_types.hpp" // SliceRow, MarketSlice, PanelView, PanelField
#include "atx/engine/loop/types.hpp"       // InstrumentId

namespace atx::engine {

// ===========================================================================
//  RollingPanel<Cap>
//
//  Cap is the ring capacity in rows; it MUST be a power of two and >=
//  max_lookback (the static_assert enforces pow2; the ctor ATX_ASSERTs the
//  max_lookback <= Cap relation). A power-of-two Cap turns the ring index wrap
//  into a single mask (head & (Cap-1)) instead of a modulo.
// ===========================================================================
template <atx::usize Cap> class RollingPanel {
  static_assert((Cap & (Cap - 1)) == 0, "Cap must be power of two");
  static_assert(Cap > 0, "Cap must be non-zero");

public:
  /// Build a panel over `universe` (the fixed column set) retaining the last
  /// `max_lookback` sealed rows. ONE aligned allocation here; none afterward.
  /// NON-OWNING universe: the caller's InstrumentId storage must outlive the
  /// panel. PRECONDITION: 0 < max_lookback <= Cap (ABORTS in debug).
  ///
  // bugprone-exception-escape suppressed: the ctor is frozen `noexcept` by the
  // P2-2 plan. Its only throwing operation is build_index's construction-time
  // std::vector growth (bounded by n_inst). A std::bad_alloc here is unrecoverable
  // panel-setup failure, so terminating (the noexcept effect) is the intended
  // fail-closed posture — there is no half-built panel to leak.
  // NOLINTNEXTLINE(bugprone-exception-escape)
  RollingPanel(std::span<const InstrumentId> universe, atx::usize max_lookback) noexcept
      : universe_{universe}, n_inst_{universe.size()}, max_lookback_{max_lookback},
        mask_words_{(universe.size() + 63U) / 64U} {
    ATX_ASSERT(max_lookback > 0U);
    ATX_ASSERT(max_lookback <= Cap);

    allocate_storage();
    build_index();
    fill_all_nan(); // every cell starts absent (NaN, mask 0) before any append.
  }

  // Owns a raw aligned buffer through unique_ptr; copying would alias the buffer
  // and double-free. Move is well-formed (the unique_ptr transfers ownership),
  // but a panel is a fixed fixture in the loop — no need to move it — so the
  // safe default is move-only via the smart-pointer member (Rule of Zero). We
  // delete copy explicitly to make the intent loud (agent profile §1).
  RollingPanel(const RollingPanel &) = delete;
  RollingPanel &operator=(const RollingPanel &) = delete;
  RollingPanel(RollingPanel &&) noexcept = default;
  RollingPanel &operator=(RollingPanel &&) noexcept = default;
  ~RollingPanel() = default;

  /// Append one SEALED cross-section. Advances the ring head, writes every
  /// universe column from the slice (NaN + mask 0 for absent members), and bumps
  /// the live-row count up to max_lookback. O(n_instruments); ZERO allocation.
  ///
  /// PIT: the caller (the loop) invokes this ONLY after the slice's bar has
  /// closed (end_time <= now()); there is no API to write an unsealed bar.
  void append_sealed_row(const MarketSlice &s) noexcept {
    // SAFETY: power-of-two Cap → the wrap is a mask, not a modulo. On the very
    //         first append head_ moves 0 -> 1 (we start at 0 with 0 valid rows),
    //         so head_ always names the newest physical row once valid_rows_ > 0.
    head_ = (head_ + 1U) & (Cap - 1U);

    clear_row(head_); // reset the (possibly evicted) row to all-absent first.

    for (const SliceRow &r : s.rows) {
      const atx::usize col = column_of(r.id);
      if (col == kNoColumn) {
        continue; // id outside the universe — ignored (keeps membership PIT).
      }
      write_cell(head_, col, r.bar);
    }

    if (valid_rows_ < max_lookback_) {
      ++valid_rows_;
    }
  }

  /// A zero-copy, newest-first view of the last `min(valid_rows, max_lookback)`
  /// sealed rows. Valid until the next append_sealed_row or destruction. O(1).
  [[nodiscard]] PanelView view() const noexcept {
    return PanelView{fields_ptr(), mask_ptr(), universe_, Cap, head_, valid_rows_, mask_words_};
  }

  /// Fixed memory footprint in bytes (the single allocation's size). Lets a
  /// caller budget memory at construction; documents the layout formula.
  [[nodiscard]] static constexpr atx::usize bytes_for(atx::usize n_inst) noexcept {
    const atx::usize mask_words = (n_inst + 63U) / 64U;
    return field_count_bytes(n_inst) + Cap * mask_words * sizeof(atx::u64);
  }

private:
  // ---- storage geometry -----------------------------------------------------

  [[nodiscard]] static constexpr atx::usize field_count_bytes(atx::usize n_inst) noexcept {
    return kPanelFieldCount * Cap * n_inst * sizeof(atx::f64);
  }

  [[nodiscard]] atx::usize field_block_floats() const noexcept { return Cap * n_inst_; }

  [[nodiscard]] atx::f64 *fields_ptr() const noexcept {
    // SAFETY: storage_ is the single aligned allocation; the field block occupies
    //         its front (field_count_bytes), the mask block follows. Both blocks
    //         are 8-byte-aligned (f64/u64) within an L2-line-aligned base. The
    //         std::byte -> f64 cast is the documented way to reinterpret a raw
    //         allocation as a typed array (matches atx-core container practice).
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<atx::f64 *>(storage_.get());
  }

  [[nodiscard]] atx::u64 *mask_ptr() const noexcept {
    // SAFETY: the mask block begins immediately after the f64 field block; the
    //         field block is a whole number of 8-byte f64, so the u64 view that
    //         follows is naturally aligned. byte-pointer offset then reinterpret.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast, cppcoreguidelines-pro-bounds-pointer-arithmetic)
    return reinterpret_cast<atx::u64 *>(storage_.get() + field_count_bytes(n_inst_));
  }

  // ---- construction-time setup ----------------------------------------------

  void allocate_storage() noexcept {
    const atx::usize bytes = bytes_for(n_inst_);
    // L2-cache-line aligned so cross-section reads land on aligned boundaries
    // (AVX-friendly) and adjacent panels never false-share.
    void *raw = atx::core::aligned_alloc_bytes(bytes, atx::core::kCacheLineSize);
    ATX_ASSERT(raw != nullptr); // construction-time OOM is unrecoverable here.
    storage_ = StoragePtr{static_cast<std::byte *>(raw)};
  }

  void build_index() {
    // ONE construction-time allocation (the vector). No allocation thereafter.
    index_.reserve(n_inst_);
    for (atx::usize i = 0; i < n_inst_; ++i) {
      index_.push_back(IdCol{universe_[i], i});
    }
    // Sort by id so column_of can binary-search. A stable order on duplicate ids
    // is irrelevant — a well-formed universe has none (the first match wins).
    std::sort(index_.begin(), index_.end(),
              [](const IdCol &a, const IdCol &b) noexcept { return a.id < b.id; });
  }

  /// Binary-search the sorted index for `id`'s column, or kNoColumn if absent.
  [[nodiscard]] atx::usize column_of(InstrumentId id) const noexcept {
    const auto it =
        std::lower_bound(index_.begin(), index_.end(), id,
                         [](const IdCol &e, InstrumentId key) noexcept { return e.id < key; });
    if (it != index_.end() && it->id == id) {
      return it->col;
    }
    return kNoColumn;
  }

  // SAFETY (region below): the buffer helpers index the flat f64/u64 arrays the
  // single construction-time allocation backs. Every offset is bounded by that
  // allocation's size (kPanelFieldCount * Cap * n_inst floats + Cap * mask_words
  // words); phys_row < Cap and col < n_inst at every call site. Linear pointer
  // arithmetic over a contiguous typed array is the intended access pattern for a
  // column-major ring with no per-cell std::span (which the wrap precludes), and
  // mirrors the raw-buffer indexing in atx-core's ring_buffer / fixed_vector.
  // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  void fill_all_nan() noexcept {
    atx::f64 *f = fields_ptr();
    const atx::usize total_floats = kPanelFieldCount * field_block_floats();
    for (atx::usize i = 0; i < total_floats; ++i) {
      f[i] = kAbsent;
    }
    atx::u64 *m = mask_ptr();
    const atx::usize total_words = Cap * mask_words_;
    for (atx::usize i = 0; i < total_words; ++i) {
      m[i] = 0ULL;
    }
  }

  // ---- per-row writes -------------------------------------------------------

  /// Reset one physical ring row to all-absent (NaN cells, cleared mask) so an
  /// evicted row never leaks stale values into a later, sparsely-populated slice.
  void clear_row(atx::usize phys_row) noexcept {
    atx::f64 *f = fields_ptr();
    for (atx::usize field = 0; field < kPanelFieldCount; ++field) {
      atx::f64 *cell = f + field * field_block_floats() + phys_row * n_inst_;
      for (atx::usize i = 0; i < n_inst_; ++i) {
        cell[i] = kAbsent;
      }
    }
    atx::u64 *m = mask_ptr() + phys_row * mask_words_;
    for (atx::usize w = 0; w < mask_words_; ++w) {
      m[w] = 0ULL;
    }
  }

  /// Write one instrument's OHLCV bar into (phys_row, col) and set its mask bit.
  void write_cell(atx::usize phys_row, atx::usize col, const atx::core::domain::Bar &b) noexcept {
    store_field(PanelField::Open, phys_row, col, b.open.to_decimal().to_double());
    store_field(PanelField::High, phys_row, col, b.high.to_decimal().to_double());
    store_field(PanelField::Low, phys_row, col, b.low.to_decimal().to_double());
    store_field(PanelField::Close, phys_row, col, b.close.to_decimal().to_double());
    store_field(PanelField::Volume, phys_row, col, b.volume.to_decimal().to_double());

    atx::u64 *word = mask_ptr() + phys_row * mask_words_ + (col >> 6U);
    *word |= (1ULL << (col & 63U));
  }

  void store_field(PanelField f, atx::usize phys_row, atx::usize col, atx::f64 value) noexcept {
    const atx::usize block = static_cast<atx::usize>(f) * field_block_floats();
    fields_ptr()[block + phys_row * n_inst_ + col] = value;
  }
  // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

  // ---- members --------------------------------------------------------------

  /// Quiet NaN sentinel for an absent (untraded) cell. A constexpr quiet NaN
  /// (not std::nan(""), which is non-constexpr and declared throwing) — so the
  /// sentinel has no dynamic-init or exception surface; std::isnan() detects it.
  static constexpr atx::f64 kAbsent = std::numeric_limits<atx::f64>::quiet_NaN();

  /// Sentinel column for "id not in the universe" (column_of miss).
  static constexpr atx::usize kNoColumn = static_cast<atx::usize>(-1);

  /// One sorted (id -> column) index entry. The index is sorted ascending by id
  /// once at construction so column_of is an O(log n) binary search.
  struct IdCol {
    InstrumentId id{};
    atx::usize col{};
  };

  /// Deleter that routes the unique_ptr through the aligned-free intrinsic the
  /// buffer was allocated with (plain delete/free would be UB on the MSVC path).
  struct AlignedDeleter {
    void operator()(std::byte *p) const noexcept { atx::core::aligned_free(p); }
  };
  using StoragePtr = std::unique_ptr<std::byte, AlignedDeleter>;

  std::span<const InstrumentId> universe_; // non-owning, fixed column order
  atx::usize n_inst_;                      // == universe_.size()
  atx::usize max_lookback_;                // trailing rows exposed by view()
  atx::usize mask_words_;                  // u64 words per row in the bitmap

  StoragePtr storage_{};       // the single aligned ring allocation
  std::vector<IdCol> index_{}; // sorted id -> column (built once at construction)

  atx::usize head_{0};       // physical ring index of the newest sealed row
  atx::usize valid_rows_{0}; // live trailing rows (capped at max_lookback_)
};

} // namespace atx::engine
