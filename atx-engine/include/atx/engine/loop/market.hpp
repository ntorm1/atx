#pragma once

// atx::engine::loop — Market: the shared price / stats book (P2-5).
//
// ===========================================================================
//  Why this type exists
// ===========================================================================
//  Market is the loop's CURRENT market-state book — the single place every
//  downstream Phase-2 unit reads "what is this instrument worth right now" and
//  "what are its execution-cost parameters". It is deliberately a SHARED
//  contract, not a speculative one: Portfolio (P2-5) marks positions against
//  `mark()`; the ExecutionSimulator (P2-6) reads `bar_volume()` for its volume
//  cap and `stats()` for spread/σ/ADV cost terms and calls `shift_mark()` to
//  apply permanent price impact; the BacktestLoop (P2-7) drives `update_prices`
//  once per frontier. Defining `bar_volume`/`stats`/`shift_mark` now means P2-6
//  drops in without reshaping this header.
//
// ===========================================================================
//  State model — a persistent last-value table, not a per-slice snapshot
// ===========================================================================
//  Each instrument carries its LAST known mark + the volume of its last sealed
//  bar. `update_prices` refreshes only the instruments present in the slice; an
//  instrument absent from a frontier (it did not trade) keeps its prior values.
//  A mark is a quiet NaN until the first slice sets it (no synthetic price is
//  invented). bar_volume starts at 0.
//
// ===========================================================================
//  The money boundary — marks are f64 by design
// ===========================================================================
//  Marks, bar volumes and cost stats are f64, NOT exact Decimal. This is
//  intentional and is the counterpart to Portfolio's Decimal money ledger:
//  reference prices and statistics are inherently approximate market DATA, not
//  ledger quantities, and downstream P&L mark-to-market is an f64 estimate of
//  open position value (the realized ledger stays exact in Decimal). Keeping the
//  book in f64 avoids a Decimal round-trip on every mark read in the hot loop.
//
// ===========================================================================
//  Universe & storage
// ===========================================================================
//  DENSE storage indexed by a FIXED universe order (the same id→index sorted-
//  array idiom RollingPanel uses). InstrumentId == atx::core::domain::Symbol has
//  no std::hash and a hash map's iteration order is non-deterministic, so a
//  sorted (id, index) array is the house pattern: deterministic, allocation-
//  light, cache-friendly, O(log n) lookup. A slice row whose id is outside the
//  universe is IGNORED (the feed may legitimately carry non-universe names). The
//  accessors (`mark`/`bar_volume`/`stats`/`shift_mark`) take only universe ids —
//  an out-of-universe id is a programmer error (ATX_ASSERT).
//
// ===========================================================================
//  Ownership / threading
// ===========================================================================
//  The universe span is non-owning — the caller's InstrumentId storage must
//  outlive the Market. Per-instrument state lives in owned std::vectors sized
//  once at construction. Single-threaded backtest use; no synchronisation.

#include <algorithm> // std::sort, std::lower_bound
#include <limits>    // std::numeric_limits (quiet NaN sentinel)
#include <span>      // std::span (universe + stats + slice rows)
#include <vector>    // std::vector (dense per-instrument state + sorted index)

#include "atx/core/macro.hpp" // ATX_ASSERT
#include "atx/core/types.hpp" // usize, f64

#include "atx/engine/loop/panel_types.hpp" // MarketSlice, SliceRow
#include "atx/engine/loop/types.hpp"       // InstrumentId

namespace atx::engine {

// ===========================================================================
//  InstrumentStats — per-instrument execution-cost parameters.
//
//  adv    : average daily volume (shares) — the denominator for the √-impact
//           and volume-cap models (P2-6).
//  sigma  : per-bar return volatility — scales temporary/permanent impact.
//  spread : half-spread (price units) — the baseline slippage floor.
//
//  All zero by default: an unconfigured book charges no statistics-driven cost.
//  Phase-5 calibrates these from data; for P2-5/P2-6 they are caller-supplied
//  knobs. f64 because they are estimated statistics, not ledger money.
// ===========================================================================
struct InstrumentStats {
  atx::f64 adv = 0.0;
  atx::f64 sigma = 0.0;
  atx::f64 spread = 0.0;
};

// ===========================================================================
//  Market — the current price / stats book over a fixed universe.
// ===========================================================================
class Market {
public:
  /// Build a book over `universe` (the fixed instrument set). `stats` is parallel
  /// to `universe` (stats[i] describes universe[i]); an EMPTY stats span ⇒ all-
  /// zero stats for every instrument. NON-OWNING universe: the caller's
  /// InstrumentId storage must outlive the Market.
  ///
  /// PRECONDITION: stats.empty() || stats.size() == universe.size() (ABORTS in
  /// debug) — a partial stats vector is a programmer error.
  Market(std::span<const InstrumentId> universe, std::span<const InstrumentStats> stats) noexcept
      : universe_{universe} {
    ATX_ASSERT(stats.empty() || stats.size() == universe.size());
    build_index();
    marks_.assign(universe.size(), kAbsentMark); // NaN until first slice sets it
    volumes_.assign(universe.size(), 0.0);
    init_stats(stats);
  }

  /// Refresh mark + last bar volume from a sealed cross-section. Only the
  /// instruments present in the slice are touched; absent universe members keep
  /// their prior values. A row whose id is outside the universe is ignored.
  /// O(rows · log n_inst); ZERO allocation.
  void update_prices(const MarketSlice &s) noexcept {
    for (const SliceRow &r : s.rows) {
      const atx::usize idx = index_of(r.id);
      if (idx == kNoIndex) {
        continue; // id outside the universe — ignored.
      }
      // Reference price is the bar's close; volume is the sealed bar's volume.
      marks_[idx] = r.bar.close.to_decimal().to_double();
      volumes_[idx] = r.bar.volume.to_decimal().to_double();
    }
  }

  /// Current reference price for `id` (NaN if never set by a slice).
  /// PRECONDITION: id is in the universe (ABORTS in debug).
  [[nodiscard]] atx::f64 mark(InstrumentId id) const noexcept { return marks_[require_index(id)]; }

  /// Volume of `id`'s last sealed bar (0 if never set).
  /// PRECONDITION: id is in the universe (ABORTS in debug).
  [[nodiscard]] atx::f64 bar_volume(InstrumentId id) const noexcept {
    return volumes_[require_index(id)];
  }

  /// Execution-cost parameters for `id`.
  /// PRECONDITION: id is in the universe (ABORTS in debug).
  [[nodiscard]] const InstrumentStats &stats(InstrumentId id) const noexcept {
    return stats_[require_index(id)];
  }

  /// Permanent-impact hook: shift `id`'s mark by `delta` (price units). The
  /// ExecutionSimulator (P2-6) calls this after a fill to move the reference
  /// price by the modelled permanent impact. PRECONDITION: id is in the universe
  /// AND its mark is already set (shifting an unset NaN mark is a programmer
  /// error — there is no price to move). ABORTS in debug.
  void shift_mark(InstrumentId id, atx::f64 delta) noexcept {
    const atx::usize idx = require_index(id);
    // SAFETY: shifting NaN silently yields NaN and would mask a sequencing bug
    // (impact applied before any price was seen); assert the mark exists first.
    ATX_ASSERT(marks_[idx] == marks_[idx]); // NaN-check: x != x iff x is NaN
    marks_[idx] += delta;
  }

private:
  // Quiet NaN sentinel for an unset mark (no synthetic price invented). constexpr
  // quiet NaN (not std::nan(""), which is non-constexpr / declared throwing).
  static constexpr atx::f64 kAbsentMark = std::numeric_limits<atx::f64>::quiet_NaN();

  /// Sentinel "id not in the universe" (index_of miss).
  static constexpr atx::usize kNoIndex = static_cast<atx::usize>(-1);

  /// One sorted (id → dense index) entry; the index is sorted ascending by id
  /// once at construction so index_of is an O(log n) binary search.
  struct IdIndex {
    InstrumentId id{};
    atx::usize idx{};
  };

  void build_index() {
    index_.reserve(universe_.size());
    for (atx::usize i = 0; i < universe_.size(); ++i) {
      index_.push_back(IdIndex{universe_[i], i});
    }
    std::sort(index_.begin(), index_.end(),
              [](const IdIndex &a, const IdIndex &b) noexcept { return a.id < b.id; });
  }

  void init_stats(std::span<const InstrumentStats> stats) {
    if (stats.empty()) {
      stats_.assign(universe_.size(), InstrumentStats{}); // all-zero defaults
      return;
    }
    stats_.assign(stats.begin(), stats.end()); // parallel to the universe order
  }

  /// Binary-search the sorted index for `id`'s dense index, or kNoIndex if absent.
  [[nodiscard]] atx::usize index_of(InstrumentId id) const noexcept {
    const auto it =
        std::lower_bound(index_.begin(), index_.end(), id,
                         [](const IdIndex &e, InstrumentId key) noexcept { return e.id < key; });
    if (it != index_.end() && it->id == id) {
      return it->idx;
    }
    return kNoIndex;
  }

  /// index_of with the universe-membership precondition (accessor contract).
  [[nodiscard]] atx::usize require_index(InstrumentId id) const noexcept {
    const atx::usize idx = index_of(id);
    ATX_ASSERT(idx != kNoIndex); // out-of-universe accessor id is a programmer error
    return idx;
  }

  std::span<const InstrumentId> universe_; // non-owning, fixed order
  std::vector<IdIndex> index_{};           // sorted id → dense index (built once)
  std::vector<atx::f64> marks_{};          // last reference price (NaN = unset)
  std::vector<atx::f64> volumes_{};        // last sealed bar volume
  std::vector<InstrumentStats> stats_{};   // per-instrument cost params
};

} // namespace atx::engine
