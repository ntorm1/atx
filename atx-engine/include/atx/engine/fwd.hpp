#pragma once

// atx::engine — Phase-1 event-spine forward declarations.
//
// A lightweight header other engine headers include to NAME the core spine
// types without pulling in their full definitions (and the atx-core machinery
// behind them). Keeping the forward set here means a header that only passes a
// `event::Event&` or an `IDataHandler*` around does not transitively include
// the Disruptor, the datetime library, or the domain types.
//
// Full definitions live in (added per Phase-1 unit):
//   event/event.hpp        — event::EventType, event::Event        (P1-1)
//   data/market.hpp        — data::MarketPayload, make_market_bar  (P1-2)
//   bus/event_bus.hpp      — EventBus<…>, Consumer                 (P1-3)
//   clock/sim_clock.hpp    — SimClock                              (P1-4)
//   data/data_handler.hpp  — data::IDataHandler, InMemoryBarFeed   (P1-5)
//
// NOTE: EventBus is a class template with compile-time capacity/consumer/
// producer parameters; templates with default arguments cannot be forward
// declared without re-stating the defaults, so it is intentionally NOT
// forward-declared here — include bus/event_bus.hpp directly where needed.

namespace atx::engine {

namespace event {
struct Event;
} // namespace event

namespace data {
struct MarketPayload;
class IDataHandler;
} // namespace data

class SimClock;

} // namespace atx::engine
