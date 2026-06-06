#pragma once

// atx::engine::loop — BacktestLoop: the deterministic per-slice crank (P2-7).
//
// ===========================================================================
//  What this unit is
// ===========================================================================
//  BacktestLoop is the integration spine that wires the Phase-1 event spine
//  (InMemoryBarFeed -> SimClock -> EventBus) to the Phase-2 execution/accounting
//  half (RollingPanel -> ISignalSource -> WeightPolicy -> ExecutionSimulator ->
//  Portfolio) and samples the run into a BacktestResult. It owns NO strategy
//  logic — the strategy is reached through the ISignalSource seam (a
//  ScriptedSignalSource test double today, the Phase-3 VmSignalSource in
//  production), so the SAME loop runs both with no change.
//
// ===========================================================================
//  The per-slice order IS the no-look-ahead guarantee (plan §2, §4.1)
// ===========================================================================
//  For each sealed cross-section the feed publishes, on_time_slice runs a FIXED
//  sequence whose ORDER is the look-ahead firewall:
//
//    1. update_prices       refresh the Market book from this slice's closes
//    2. mark_to_market      value the book on the new marks
//    3. settle PRIOR orders  fill orders queued on an EARLIER slice -> Portfolio
//    4. append_sealed_row   write the completed bar into the PIT RollingPanel
//    5. if schedule fires:  evaluate the signal over the panel view,
//    6.                      turn it into target weights, then
//    7.                      reconcile to orders and QUEUE them (fill on a LATER
//                            slice — never this one: settle (3) precedes queue (7))
//    8. sample              record equity / exposure / turnover into the result
//
//  Because settle (3) happens BEFORE queue (7), an order decided on bar `t` can
//  NEVER fill on bar `t` — it is queued only after the prior batch settled, and
//  the ExecutionSimulator's own firewall additionally refuses any fill whose bar
//  end_time <= order.queued_at. The decision-data (the panel, step 4) is sealed
//  before the strategy reads it (step 5), so the in-progress bar is invisible.
//
// ===========================================================================
//  Single logical thread / determinism
// ===========================================================================
//  The loop runs single-threaded backtest mode: it drives feed.step() then
//  drain_in_order() in lockstep, so consumers observe every event in publication
//  order before the clock advances. No RNG, fixed-order Decimal cash/P&L sums
//  (Portfolio), FIFO open-order processing (ExecutionSimulator) — identical feed
//  => byte-identical fills / equity (proven in backtest_integration_test.cpp).
//
// ===========================================================================
//  Orders and fills are IN-PROCESS, not bus events (as-built note)
// ===========================================================================
//  In Phase-2 backtest mode the bus carries ONLY Market events (the feed is the
//  sole producer). The loop processes orders/fills in-process via
//  ExecutionSimulator::queue / settle_pending return values — it does NOT publish
//  Signal/Order/Fill events onto the bus. The P2-1 Signal/Order/Fill payload
//  makers exist to complete the Event taxonomy and for a future live-mode router;
//  the deterministic backtest crank does not need the round-trip. This matches the
//  plan's loop shape (plan §P2-7) where the loop calls exec_ directly.
//
// ===========================================================================
//  Ownership / lifetime / threading
// ===========================================================================
//  NON-OWNING throughout: the loop holds raw NON-OWNING pointers to caller-owned
//  collaborators (feed, clock, bus, panel, signal source, weight policy, exec sim,
//  portfolio, market) — the house pattern for a held-by-reference collaborator
//  (matches InMemoryBarFeed's clock_/bus_; a pointer, not a reference member, so
//  the type stays assignable and avoids the const/ref-data-member guidance). The
//  Universe is a non-owning span. ALL pointees and the span's storage must outlive
//  the loop. The loop registers ONE consumer (index 0) on the bus at construction,
//  so the bus must have no prior consumer registered (EventBus<>'s default
//  ConsumerCount is 1). Single-threaded backtest use; no internal synchronisation.
//
//  Allocation: the per-slice scratch (the drained SliceRow buffer) is reserved
//  once at construction to the universe size, so steady-state slice assembly does
//  not allocate. The result's growable vectors (equity_curve, fills) accrue over
//  the run — they are the OUTPUT, not the hot path; a fixed-length run grows them
//  a bounded number of times. WeightPolicy still allocates once per rebalance
//  (its tracked residual), which is the rebalance cadence, not per bar.

#include <span>    // std::span (drained-slice view, order list)
#include <utility> // std::move (result hand-off)
#include <vector>  // std::vector (per-slice scratch + result accumulators)

#include "atx/core/datetime.hpp" // atx::core::time::Timestamp
#include "atx/core/decimal.hpp"  // atx::core::Decimal (final cash)
#include "atx/core/types.hpp"    // usize, i64, f64

#include "atx/engine/bus/event_bus.hpp"       // EventBus<> (the spine bus)
#include "atx/engine/clock/sim_clock.hpp"     // SimClock (the spine clock)
#include "atx/engine/data/data_handler.hpp"   // data::IDataHandler (the spine feed)
#include "atx/engine/data/market.hpp"         // data::MarketPayload (slice decode)
#include "atx/engine/event/event.hpp"         // event::Event, EventType
#include "atx/engine/exec/execution_sim.hpp"  // exec::ExecutionSimulator, FillPayload
#include "atx/engine/exec/payloads.hpp"       // exec::OrderPayload, FillPayload
#include "atx/engine/loop/market.hpp"         // Market (price/stats book)
#include "atx/engine/loop/panel_types.hpp"    // MarketSlice, SliceRow
#include "atx/engine/loop/rolling_panel.hpp"  // RollingPanel<Cap>
#include "atx/engine/loop/signal_source.hpp"  // ISignalSource, SignalView
#include "atx/engine/loop/types.hpp"          // InstrumentId, Universe
#include "atx/engine/loop/weight_policy.hpp"  // WeightPolicy
#include "atx/engine/portfolio/portfolio.hpp" // Portfolio

namespace atx::engine {

// ===========================================================================
//  Schedule — the rebalance cadence gate.
//
//  Gates step 5 of the loop (Zipline schedule_function / LEAN Schedule.On). The
//  panel updates EVERY slice (cheap, one column write); the strategy is evaluated
//  only when the schedule fires — decoupling eval cost from data rate and
//  controlling turnover (plan §3.3).
//
//  As-built (vs. the plan's literal `fires(Timestamp)`): the gate is a pure
//  integer cadence over the SLICE INDEX, not a calendar-close predicate. A
//  calendar gate needs a trading-calendar dependency that is out of Phase-2 scope;
//  an integer cadence is the honest minimal mechanism and is exactly what the
//  cadence test exercises. `every == 1` (the default) fires on every slice — the
//  daily-close cadence for a daily-bar feed.
// ===========================================================================
struct Schedule {
  atx::usize every = 1; // rebalance every `every` slices (1 = every bar)

  /// True iff a rebalance fires on the slice at 0-based index `slice_index`.
  /// `every == 0` is treated as "never fire" (a degenerate no-rebalance run).
  [[nodiscard]] bool fires(atx::usize slice_index) const noexcept {
    return every != 0U && (slice_index % every) == 0U;
  }
};

// ===========================================================================
//  EquitySample — one row of the equity curve (sampled once per slice).
//
//  f64 throughout: equity / gross / net are mark-to-market quantities derived
//  from f64 market data (matching Portfolio's f64 aggregates), not ledger money.
// ===========================================================================
struct EquitySample {
  atx::core::time::Timestamp t; // slice timestamp
  atx::f64 equity = 0.0;        // cash + Σ market_value
  atx::f64 gross = 0.0;         // Σ |market_value|
  atx::f64 net = 0.0;           // Σ market_value
};

// ===========================================================================
//  BacktestResult — what a run produces (plan §1: equity curve, fills, P&L,
//  turnover). Owned, returned by value from run().
//
//  `turnover` is cumulative traded notional (Σ |fill.qty| · fill_price over every
//  fill) — a frictionless run that opens a $100k gross book reports $100k. It is
//  the simplest honest activity measure; a per-period ratio is a later refinement.
// ===========================================================================
struct BacktestResult {
  std::vector<EquitySample> equity_curve; // one sample per slice
  std::vector<exec::FillPayload> fills;   // every fill, in settle order
  atx::core::Decimal final_cash;          // exact cash at EOF
  atx::f64 final_equity = 0.0;            // cash + Σ market_value at EOF
  atx::f64 turnover = 0.0;                // cumulative traded notional
  atx::usize slices = 0;                  // slices processed
  atx::usize rebalances = 0;              // schedule fires
};

// ===========================================================================
//  BacktestLoop<Cap>
//
//  Templated on the RollingPanel ring capacity `Cap` (a compile-time power of
//  two) because it holds the panel by pointer and the panel's capacity is a
//  template parameter. The alternative — a virtual IPanel — would add a vtable on
//  the hot path for no Phase-2 benefit (there is one panel type); templating keeps
//  the append/view calls direct. Cap is forwarded verbatim from RollingPanel<Cap>.
// ===========================================================================
template <atx::usize Cap> class BacktestLoop {
public:
  /// Wire the loop to its collaborators. NON-OWNING: every referenced collaborator
  /// and the `universe` span must outlive the loop (header lifetime note).
  /// Registers consumer 0 on `bus` — the bus must have no prior consumer
  /// registered. `universe` is the fixed instrument set, in the SAME column order
  /// the panel / market / portfolio / signal source were built over.
  BacktestLoop(data::IDataHandler &feed, SimClock &clock, EventBus<> &bus, RollingPanel<Cap> &panel,
               ISignalSource &signal, const WeightPolicy &policy, exec::ExecutionSimulator &exec,
               Portfolio &portfolio, Market &market, Universe universe, Schedule schedule,
               Delay delay = Delay::Next) noexcept
      : feed_{&feed}, clock_{&clock}, bus_{&bus}, panel_{&panel}, signal_{&signal},
        policy_{&policy}, exec_{&exec}, portfolio_{&portfolio}, market_{&market},
        universe_{universe}, schedule_{schedule}, delay_{delay} {
    // One consumer drives the single-threaded drain. Reserve the slice scratch to
    // the universe size so steady-state slice assembly never allocates.
    (void)bus_->add_consumer(0);
    slice_rows_.reserve(universe_.size());

    // Fill-timing knob (P3c-3). delay-0 (Same) flips the exec sim's same-bar
    // relaxation ON so the dedicated post-queue same-bar settle below CAN fill;
    // delay-1 (Next, the default) leaves the relaxation OFF — the firewall. The
    // knob NEVER touches the signal data: the same program reads the same panel
    // under either value; only the bar an order fills on differs. We flip the sim
    // flag only for Same, so the default (Next) leaves the caller-built posture
    // (relaxation OFF) untouched.
    switch (delay_) {
    case Delay::Same:
      exec_->set_allow_same_bar_fill(true);
      break;
    case Delay::Next:
      // Default firewall: do not relax. The sim was constructed with the
      // relaxation OFF (FillCfg default), so Next is a no-op by construction.
      break;
    }
  }

  // The loop holds non-owning pointers into caller storage and is a stack fixture;
  // it is neither copyable nor movable (a copy would share its registered consumer
  // and result accumulators). Deleted explicitly to make intent loud (agent §1).
  BacktestLoop(const BacktestLoop &) = delete;
  BacktestLoop &operator=(const BacktestLoop &) = delete;
  BacktestLoop(BacktestLoop &&) = delete;
  BacktestLoop &operator=(BacktestLoop &&) = delete;
  ~BacktestLoop() = default;

  /// Run the backtest to EOF and return the result. Drives feed.step() then
  /// drain_in_order() in lockstep; each true step is one sealed cross-section.
  [[nodiscard]] BacktestResult run() {
    while (feed_->step()) {
      // Drain THIS frontier's Market events into the slice scratch. The feed
      // already advanced the clock to the frontier, so clock_->now() == this
      // slice's timestamp and every drained event has knowledge_ts <= now().
      slice_rows_.clear();
      bus_->drain_in_order([this](atx::usize, const event::Event &e) noexcept { collect(e); });

      const atx::core::time::Timestamp t = clock_->now();
      on_time_slice(t, MarketSlice{t, std::span<const SliceRow>{slice_rows_}});
    }

    result_.slices = slice_index_;
    result_.final_cash = portfolio_->cash();
    result_.final_equity = portfolio_->equity();
    return std::move(result_);
  }

private:
  /// Accumulate one drained event into the current slice scratch. Only Market/Bar
  /// events carry a cross-section row; any other event type is ignored (Phase-2
  /// backtest mode only publishes Market bars onto the bus).
  void collect(const event::Event &e) noexcept {
    if (e.kind() != event::EventType::Market) {
      return;
    }
    const auto mp = e.payload_as<data::MarketPayload>();
    if (mp.kind != data::MarketPayload::Kind::Bar) {
      return; // ticks are not a Phase-2 panel input
    }
    slice_rows_.push_back(SliceRow{mp.symbol, mp.as_bar(), mp.delisted_final});
  }

  /// The per-slice crank (plan §2). The fixed step order IS the no-look-ahead
  /// guarantee — see the header. `t` is the sealed slice's timestamp.
  void on_time_slice(atx::core::time::Timestamp t, const MarketSlice &slice) {
    market_->update_prices(slice);        // 1. refresh marks from this slice's closes
    portfolio_->mark_to_market(*market_); // 2. value the book on the new marks

    // 3. Settle orders queued on an EARLIER slice (precedes queue() below — the
    //    firewall). For the default (Next) this is the slice's ONLY settle, so an
    //    order decided here can never fill here. (delay-0 adds a second settle
    //    AFTER queue, in rebalance(), gated entirely by Delay::Same.)
    settle_at(t);

    panel_->append_sealed_row(slice); // 4. seal the completed bar into the panel

    if (schedule_.fires(slice_index_)) { // 5. cadence gate
      ++result_.rebalances;
      rebalance(t);
    }

    sample(t); // 8. record equity / exposure
    ++slice_index_;
  }

  /// Evaluate the strategy over the sealed panel and queue the resulting orders.
  /// Under delay-1 (Next, the default) they fill on a strictly-LATER slice — the
  /// firewall. Under delay-0 (Same) a dedicated post-queue settle at THIS `t`
  /// fills them against this bar's close (opt-in). An expected signal failure
  /// (exhausted scripted schedule) or an all-NaN signal produces no orders — never
  /// an abort.
  void rebalance(atx::core::time::Timestamp t) {
    auto signal = signal_->evaluate(panel_->view()); // 6. strategy = VM (or scripted)
    if (!signal) {
      return; // no opinion this rebalance (e.g. schedule exhausted) -> no orders
    }
    const std::vector<atx::f64> weights = policy_->to_target_weights(*signal, universe_);
    const std::vector<exec::OrderPayload> orders =
        policy_->reconcile(weights, universe_, *portfolio_, *market_, t); // 7. target - current
    exec_->queue(std::span<const exec::OrderPayload>{orders}, t);

    // delay-0 (Same) ONLY: a second settle at THIS same `t` lets the just-queued
    // orders fill against this bar's close. It is gated entirely by `delay_`, so
    // the default (Next) path NEVER runs this — the slice's only settle is step 3
    // (which strictly precedes this queue), keeping the no-look-ahead firewall
    // structurally intact for the conservative default. The sim's same-bar
    // relaxation (now >= queued_at, flipped on in the ctor for Same) is what makes
    // these queued-at-`t` orders eligible at `t`.
    if (delay_ == Delay::Same) {
      settle_at(t); // delay-0: fill on this bar's close
    }
  }

  /// Apply every fill the sim emits at `now` to the portfolio + result. Shared by
  /// the per-slice settle (step 3) and the delay-0 same-bar settle. The returned
  /// span borrows sim-owned scratch valid only until the next sim call, so it is
  /// fully consumed here.
  void settle_at(atx::core::time::Timestamp now) {
    for (const exec::FillPayload &f : exec_->settle_pending(now, *market_)) {
      portfolio_->apply_fill(f);
      record_fill(f);
    }
  }

  /// Append a fill to the result and accrue its traded notional into turnover.
  void record_fill(const exec::FillPayload &f) {
    result_.fills.push_back(f);
    const auto qty = static_cast<atx::f64>((f.qty < 0) ? -f.qty : f.qty);
    result_.turnover += qty * f.price.to_double();
  }

  /// Sample the equity curve at slice `t` (step 8). Reads the Portfolio's f64
  /// aggregates (computed in fixed universe-index order — deterministic).
  void sample(atx::core::time::Timestamp t) {
    result_.equity_curve.push_back(
        EquitySample{t, portfolio_->equity(), portfolio_->gross(), portfolio_->net()});
  }

  // ---- collaborators (non-owning pointers; pointees outlive the loop) --------
  data::IDataHandler *feed_;
  SimClock *clock_;
  EventBus<> *bus_;
  RollingPanel<Cap> *panel_;
  ISignalSource *signal_;
  const WeightPolicy *policy_;
  exec::ExecutionSimulator *exec_;
  Portfolio *portfolio_;
  Market *market_;
  Universe universe_;
  Schedule schedule_;
  Delay delay_; // fill-timing knob (P3c-3): Same = delay-0, Next = delay-1

  // ---- run state ------------------------------------------------------------
  std::vector<SliceRow> slice_rows_; // per-slice scratch (reserved once)
  BacktestResult result_;            // accrues over the run, moved out at EOF
  atx::usize slice_index_ = 0;       // 0-based slice counter (drives the cadence gate)
};

} // namespace atx::engine
