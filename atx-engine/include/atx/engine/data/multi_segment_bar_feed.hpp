#pragma once

// atx::engine::data::MultiSegmentBarFeed — an IDataHandler that streams a sequence
// of per-date sealed segments as one continuous chronological feed. It composes
// ShmBarFeed: the current segment is driven to EOF, then the next is attached and
// fed. Symbols intern into one shared SymbolTable, so instrument identity is stable
// across day boundaries. Segments MUST be supplied in ascending date order; the
// SimClock (advanced by ShmBarFeed) fails closed on a backward step.
//
// PRECONDITION: every path is a valid sealed OHLCV segment (ABORTS otherwise — a
// bad path is a wiring error). NON-OWNING: symbols/clock/bus must outlive the feed.

#include <optional> // std::optional (resident reader/feed — one segment at a time)
#include <string>   // std::string (segment paths)
#include <utility>  // std::move
#include <vector>   // std::vector (ordered path list)

#include "atx/core/domain/symbol.hpp" // domain::SymbolTable (shared, stable ids)
#include "atx/core/macro.hpp"         // ATX_CHECK (always-on wiring precondition)
#include "atx/core/types.hpp"         // atx::usize

#include "atx/engine/bus/event_bus.hpp"     // EventBus
#include "atx/engine/clock/sim_clock.hpp"   // SimClock
#include "atx/engine/data/data_handler.hpp" // IDataHandler
#include "atx/engine/data/shm_bar_feed.hpp" // ShmBarFeed (the per-segment feed)

#include "atx/tsdb/segment_reader.hpp" // tsdb::SegmentReader

namespace atx::engine::data {

class MultiSegmentBarFeed final : public IDataHandler {
public:
  /// Build a feed over `seg_paths` (ascending date order), interning symbols into
  /// the shared `symbols`, advancing `clock`, and publishing onto `bus`. The first
  /// segment is attached lazily on the first step(). NON-OWNING: `symbols`,
  /// `clock`, and `bus` must outlive this feed.
  MultiSegmentBarFeed(std::vector<std::string> seg_paths, atx::core::domain::SymbolTable &symbols,
                      SimClock &clock, EventBus<> &bus)
      : paths_{std::move(seg_paths)}, symbols_{&symbols}, clock_{&clock}, bus_{&bus} {}

  /// Advance one row of the resident segment; at its EOF, lazily attach the next
  /// segment and continue. Returns false only once every segment is drained.
  [[nodiscard]] bool step() override {
    for (;;) {
      if (feed_.has_value()) {
        if (feed_->step()) {
          return true;
        }
        feed_.reset();   // current segment drained: tear down the alias...
        reader_.reset(); // ...before the reader it points into (order matters).
      }
      if (next_ >= paths_.size()) {
        return false; // all segments consumed
      }
      auto r = atx::tsdb::SegmentReader::attach(paths_[next_]);
      ++next_;
      ATX_CHECK(r.has_value()); // a bad segment path is a wiring error: fail closed
      reader_.emplace(std::move(r.value()));
      feed_.emplace(*reader_, *symbols_, *clock_, *bus_);
    }
  }

private:
  std::vector<std::string> paths_;
  atx::core::domain::SymbolTable *symbols_;        // non-owning
  SimClock *clock_;                                // non-owning
  EventBus<> *bus_;                                // non-owning
  std::optional<atx::tsdb::SegmentReader> reader_; // owns the current segment
  std::optional<ShmBarFeed> feed_;                 // aliases into reader_ (destroy first)
  atx::usize next_{0};                             // next path to attach
};

} // namespace atx::engine::data
