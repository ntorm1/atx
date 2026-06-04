#pragma once

// atx::engine — engine-wide forward declarations (Phase-1 + Phase-2).
//
// A lightweight header other engine headers include to NAME the core spine
// types without pulling in their full definitions (and the atx-core machinery
// behind them). Keeping the forward set here means a header that only passes a
// `event::Event&` or an `IDataHandler*` around does not transitively include
// the Disruptor, the datetime library, or the domain types.
//
// Full definitions live in (added per phase unit):
//   event/event.hpp        — event::EventType, event::Event        (P1-1)
//   data/market.hpp        — data::MarketPayload, make_market_bar  (P1-2)
//   bus/event_bus.hpp      — EventBus<…>, Consumer                 (P1-3)
//   clock/sim_clock.hpp    — SimClock                              (P1-4)
//   data/data_handler.hpp  — data::IDataHandler, InMemoryBarFeed   (P1-5)
//   loop/types.hpp         — InstrumentId alias                    (P2-0)
//   exec/exec.hpp          — SignalPayload/OrderPayload/FillPayload (P2-1)
//   loop/rolling_panel.hpp — RollingPanel<Cap>                     (P2-2)
//   loop/signal_source.hpp — ISignalSource, ScriptedSignalSource   (P2-3)
//   loop/weight_policy.hpp — WeightPolicy                          (P2-4)
//   portfolio/portfolio.hpp — Portfolio, Holding                   (P2-5)
//   exec/execution_sim.hpp  — ExecutionSimulator                   (P2-6)
//   loop/backtest_loop.hpp  — BacktestLoop, BacktestResult         (P2-7)
//
// NOTE: EventBus is a class template with compile-time capacity/consumer/
// producer parameters; templates with default arguments cannot be forward
// declared without re-stating the defaults, so it is intentionally NOT
// forward-declared here — include bus/event_bus.hpp directly where needed.
//
// NOTE: RollingPanel is a class template (Cap is a compile-time capacity);
// its template declaration IS forward-declared below (no defaults to repeat),
// but instantiations must include loop/rolling_panel.hpp for the definition.

#include "atx/core/types.hpp" // atx::usize (needed for RollingPanel<Cap>)

namespace atx::engine {

// =====================================================================
//  Phase-1 — event spine
// =====================================================================

namespace event {
struct Event;
} // namespace event

namespace data {
struct MarketPayload;
class IDataHandler;
} // namespace data

class SimClock;

// =====================================================================
//  Phase-2 — backtest loop subsystem
//
//  Forward declarations only. No definitions. Concrete headers are added
//  per unit (P2-1..P2-7) in exec/, portfolio/, and loop/ subdirectories.
//
//  InstrumentId is an alias for atx::core::domain::Symbol (a u32 opaque
//  id). It is not forward-declared here because aliases cannot be forward-
//  declared in C++; include loop/types.hpp to get InstrumentId.
//
//  Side is atx::core::domain::Side {Buy,Sell}; it lives in atx-core and is
//  NOT redefined here. Include atx/core/domain/domain.hpp where needed.
// =====================================================================

// Event payload PODs that complete the Phase-1 EventType taxonomy.
// Full definitions in exec/exec.hpp (P2-1).
struct SignalPayload;
struct OrderPayload;
struct FillPayload;

// Signal source seam: the strategy contract the loop calls each bar.
// ScriptedSignalSource (test double) and VmSignalSource (Phase-3 adapter)
// both implement ISignalSource. Full definition in loop/signal_source.hpp (P2-3).
class ISignalSource;

// Rolling point-in-time panel: bar data accretes here after close, never
// before, enforcing the no-look-ahead invariant. Cap is the ring capacity
// (max_lookback bars). Full definition in loop/rolling_panel.hpp (P2-2).
template <atx::usize Cap> class RollingPanel;

// Position and cash bookkeeping. Full definitions in portfolio/portfolio.hpp (P2-5).
class Portfolio;
struct Holding;

// Execution model: fills, slippage, sqrt-impact, commission, latency.
// Full definition in exec/execution_sim.hpp (P2-6).
class ExecutionSimulator;

// Weight allocation policy applied to raw signal scores.
// Full definition in loop/weight_policy.hpp (P2-4).
struct WeightPolicy;

// The backtest driver and its result aggregate.
// Full definitions in loop/backtest_loop.hpp (P2-7).
class BacktestLoop;
struct BacktestResult;

} // namespace atx::engine
