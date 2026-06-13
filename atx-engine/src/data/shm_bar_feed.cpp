#include "atx/engine/data/shm_bar_feed.hpp"

namespace atx::engine::data {

ShmBarFeed::ShmBarFeed(const atx::tsdb::SegmentReader &reader, atx::core::domain::SymbolTable &symbols,
                       SimClock &clock, EventBus<> &bus)
    : reader_{&reader}, clock_{&clock}, bus_{&bus}, open_{require_field(reader, "open")},
      high_{require_field(reader, "high")}, low_{require_field(reader, "low")},
      close_{require_field(reader, "close")}, volume_{require_field(reader, "volume")} {
  syms_.reserve(reader.instrument_count());
  for (atx::u32 i = 0; i < reader.instrument_count(); ++i) {
    syms_.push_back(symbols.intern(reader.symbol_name(i)));
  }
}

} // namespace atx::engine::data
