#pragma once

// atx::engine::data::ShmBarFeed — an IDataHandler that streams a sealed atx-tsdb
// segment straight out of shared memory, with no per-process re-parse or data
// copy. It walks the segment's time axis; for each row it advances the clock to
// that row's timestamp (BEFORE publishing, preserving no-look-ahead) and emits a
// Market event for every instrument present at that row.
//
// f64 -> Decimal: the grid is f64 (the alpha/panel path); a Bar's prices are the
// exact Decimal money type. Reconstruction uses Decimal::from_double (rounds
// half-away-from-zero to the nano grid) — the deterministic bridge from spec
// §4.4. A present cell is always finite, so from_double never errs here; an
// unexpected non-finite value falls back to a zero Decimal (defensive).

#include <string_view> // std::string_view (require_field input)
#include <vector>      // std::vector (inst-index -> Symbol map)

#include "atx/core/datetime.hpp"      // time::Timestamp
#include "atx/core/decimal.hpp"       // Decimal::from_double
#include "atx/core/domain/domain.hpp" // domain::Bar, Price, Quantity
#include "atx/core/domain/symbol.hpp" // domain::SymbolTable, Symbol
#include "atx/core/macro.hpp"         // ATX_ASSERT
#include "atx/core/types.hpp"         // u32, u64, f64

#include "atx/engine/bus/event_bus.hpp"     // EventBus
#include "atx/engine/clock/sim_clock.hpp"   // SimClock
#include "atx/engine/data/data_handler.hpp" // IDataHandler
#include "atx/engine/data/market.hpp"       // make_market_bar
#include "atx/engine/event/event.hpp"       // event::Event

#include "atx/tsdb/segment_reader.hpp" // tsdb::SegmentReader

namespace atx::engine::data {

class ShmBarFeed final : public IDataHandler {
public:
  /// Build a feed over `reader` (a sealed OHLCV segment), publishing onto `bus`
  /// and advancing `clock`. Symbol columns are interned into `symbols` once at
  /// construction, building the inst-index -> Symbol map. NON-OWNING: `reader`,
  /// `symbols`, `clock`, and `bus` must outlive this feed.
  ///
  /// PRECONDITION: the segment carries the five OHLCV fields (ABORTS in debug if
  /// any is missing — a non-OHLCV segment is a wiring error, not a runtime case).
  ShmBarFeed(const atx::tsdb::SegmentReader &reader, atx::core::domain::SymbolTable &symbols,
             SimClock &clock, EventBus<> &bus);

  /// Advance one time-axis row: move the clock to that row's timestamp, publish a
  /// Market event for every present instrument, return true. False at EOF.
  [[nodiscard]] bool step() override {
    if (row_ >= reader_->time_count()) {
      return false;
    }
    const atx::i64 ts_nanos = reader_->times()[static_cast<atx::usize>(row_)];
    const auto ts = atx::core::time::Timestamp::from_unix_nanos(ts_nanos);

    // Clock advances BEFORE publishing (no-look-ahead, as in InMemoryBarFeed).
    clock_->advance_to(ts);
    publish_row(row_, ts);
    ++row_;
    return true;
  }

private:
  [[nodiscard]] static atx::u32 require_field(const atx::tsdb::SegmentReader &r,
                                              std::string_view name) {
    const auto idx = r.field_index(name);
    ATX_ASSERT(idx.has_value());
    return idx.value_or(0U);
  }

  [[nodiscard]] static atx::core::domain::Price price_of(atx::f64 v) noexcept {
    const auto d = atx::core::Decimal::from_double(v);
    return atx::core::domain::Price::from_decimal(d.value_or(atx::core::Decimal{}));
  }

  [[nodiscard]] static atx::core::domain::Quantity qty_of(atx::f64 v) noexcept {
    const auto d = atx::core::Decimal::from_double(v);
    return atx::core::domain::Quantity::from_decimal(d.value_or(atx::core::Decimal{}));
  }

  void publish_row(atx::u64 row, atx::core::time::Timestamp ts) {
    for (atx::u32 inst = 0; inst < reader_->instrument_count(); ++inst) {
      if (!reader_->present(row, inst)) {
        continue; // absent cell -> no event (ragged history / survivorship)
      }
      atx::core::domain::Bar bar{};
      bar.ts = ts;
      bar.open = price_of(reader_->value(open_, row, inst));
      bar.high = price_of(reader_->value(high_, row, inst));
      bar.low = price_of(reader_->value(low_, row, inst));
      bar.close = price_of(reader_->value(close_, row, inst));
      bar.volume = qty_of(reader_->value(volume_, row, inst));

      atx::i64 seq = 0;
      event::Event &slot = bus_->claim_slot(seq);
      // knowledge_ts == bar.ts (release-at-close; spec §4.2 v1 assumption).
      slot = make_market_bar(syms_[inst], bar, ts, /*delisted_final=*/false);
      bus_->publish(seq);
    }
  }

  const atx::tsdb::SegmentReader *reader_;      // non-owning; outlives the feed
  SimClock *clock_;                             // non-owning
  EventBus<> *bus_;                             // non-owning
  std::vector<atx::core::domain::Symbol> syms_; // inst index -> interned Symbol
  atx::u32 open_{0};
  atx::u32 high_{0};
  atx::u32 low_{0};
  atx::u32 close_{0};
  atx::u32 volume_{0};
  atx::u64 row_{0}; // next time-axis row to publish
};

} // namespace atx::engine::data
