# atx-engine Phase 2 — Backtest Loop + Portfolio + Execution Sim (User Reference)

**Status:** ✅ shipped 2026-06-05 · `feat/atx-core-stdlib` · 224 engine tests, 0 fail (green on `ScriptedSignalSource`).
**Plan:** [`phase-2-backtest-loop-implementation-plan.md`](phase-2-backtest-loop-implementation-plan.md) ·
**Ledger:** [`phase-2-progress.md`](phase-2-progress.md).

Phase 2 closes the event-driven loop into the **first runnable, cost-honest, deterministic backtest of one
alpha**. Bars off the Phase-1 spine accrete into a point-in-time `RollingPanel`; on the rebalance cadence a
strategy — reached through the **`ISignalSource` seam** — produces a cross-sectional signal; a **rank →
dollar-neutral → gross-normalized** policy turns it into a target portfolio; an **`ExecutionSimulator`**
charges spread + slippage + √-impact + commission + latency; and a **`Portfolio`** books exact-money P&L.
The strategy is **not** arbitrary C++ — it is a signal source (a `ScriptedSignalSource` test double today;
the Phase-3 `VmSignalSource`, a compiled alpha program, drops into the same seam with zero loop changes).

---

## The four guarantees (each is a test, not a hope)

1. **Determinism.** Identical feed → byte-identical fills + equity curve. A wyhash folded over the ordered
   `(fill, equity-bits)` stream is equal across two runs; a perturbed price and an added late bar each flip
   it (non-vacuous). Enforced by the Phase-1 stable event order, fixed-order `Decimal` cash/P&L sums, FIFO
   open-order processing, and **no RNG**.
2. **No look-ahead.** A decision on bar `t` fills **no earlier than `t+1`**. The loop settles prior-slice
   orders *before* running the signal source and queues new orders *after*; the exec sim additionally
   refuses any fill whose bar `end_time ≤ order.queued_at`. Truncating the feed after `t` leaves every `≤t`
   fill/mark/equity byte-identical.
3. **Honest cost.** A buy fills **above** / a sell **below** the frictionless mark by the modeled cost; a
   larger book pays more (monotone in participation); turning costs off recovers the frictionless equity
   exactly. Every coefficient is a configurable knob (the cost *fit* is Phase 5).
4. **No survivorship.** A delisted symbol trades to its final bar and is excluded thereafter — its earlier
   position is never retroactively removed and its mark freezes at the delisting close.

---

## The canonical backtest

```cpp
namespace eng = atx::engine;
std::vector<eng::InstrumentId> universe = /* sorted instrument set */;

auto bus = std::make_unique<eng::EventBus<>>();          // ~8 MB ring — heap, not stack
eng::SimClock clock;
eng::data::InMemoryBarFeed feed{sources, clock, *bus};   // Phase-1 spine (k-way merge)

eng::RollingPanel<8> panel{universe, /*max_lookback=*/1};         // PIT trailing frame (P2-2)
eng::ScriptedSignalSource src{schedule, universe.size(), 1};      // strategy seam (P2-3; VmSignalSource = prod)
eng::WeightPolicy policy{};                                       // rank → Σw=0 → Σ|w|=1 (P2-4)
eng::exec::ExecutionSimulator sim{};                             // default cost stack (P2-6)
eng::Portfolio pf{atx::core::Decimal::from_int(1'000'000), universe};   // exact-money ledger (P2-5)
eng::Market market{universe, stats};                            // price/stats book (P2-5)

eng::BacktestLoop<8> loop{feed, clock, *bus, panel, src, policy, sim, pf, market,
                          eng::Universe{universe}, eng::Schedule{/*every=*/1}};
const eng::BacktestResult r = loop.run();    // equity curve, fills, final P&L, turnover
```

The loop registers consumer 0 on the bus (the bus must have no prior consumer). Every collaborator is
**non-owning** — the caller owns them and must outlive the loop.

---

## Public API (namespace `atx::engine`)

### `BacktestLoop<Cap>` — the per-slice crank  (`loop/backtest_loop.hpp`)
```cpp
template <usize Cap> class BacktestLoop {          // Cap = the RollingPanel<Cap> ring capacity
  BacktestLoop(data::IDataHandler& feed, SimClock&, EventBus<>&, RollingPanel<Cap>& panel,
               ISignalSource& signal, const WeightPolicy&, exec::ExecutionSimulator& exec,
               Portfolio&, Market&, Universe universe, Schedule);
  BacktestResult run();                            // drives feed.step()+drain to EOF
};
```
The fixed per-slice order **is** the no-look-ahead firewall:
`update_prices → mark_to_market → settle PRIOR fills → append sealed row → (if schedule fires) eval signal →
weights → reconcile → queue (fills on a LATER slice) → sample`. Orders/fills are handled **in-process**
(`exec.queue`/`settle_pending`); the Phase-2 bus carries Market events only.

### `Schedule` — the rebalance cadence gate
```cpp
struct Schedule { usize every = 1;  bool fires(usize slice_index) const; };   // every=1 = daily close
```
An integer slice-index cadence (calendar-aware scheduling is a tracked residual).

### `BacktestResult` / `EquitySample`
```cpp
struct EquitySample { time::Timestamp t; f64 equity; f64 gross; f64 net; };
struct BacktestResult {
  std::vector<EquitySample>      equity_curve;     // one sample per slice
  std::vector<exec::FillPayload> fills;            // every fill, in settle order
  core::Decimal final_cash;  f64 final_equity;     // exact cash + f64 equity at EOF
  f64 turnover;  usize slices;  usize rebalances;  // cumulative traded notional + counts
};
```

### `ISignalSource` — the strategy seam Phase 3 plugs into  (`loop/signal_source.hpp`)
```cpp
struct SignalView { std::span<const f64> values; };   // values[i] ↔ universe[i]; NaN = "no opinion"
class ISignalSource {
  virtual core::Result<SignalView> evaluate(PanelView panel) = 0;   // PURE: same panel → same signal
  virtual usize max_lookback() const = 0;                           // sizes the RollingPanel
};
class ScriptedSignalSource : public ISignalSource;   // test double: replays a baked schedule
// VmSignalSource : public ISignalSource;            // PRODUCTION (Phase 3): runs a compiled alpha::Program
```
`evaluate` is **pure** in the panel (the determinism contract). The returned `SignalView` borrows the
source's internal buffer — consume it within the decision step, never store it. When Phase 3 lands, define
`ATX_ENGINE_HAS_ALPHA_VM` and `VmSignalSource` drops in behind this exact interface (zero loop changes).

### `WeightPolicy` — signal → target weights + trades  (`loop/weight_policy.hpp`)
```cpp
enum class Transform : u8 { Rank, ZScore };
struct WeightPolicy {
  Transform transform     = Rank;
  bool      dollar_neutral = true;     // center Σw = 0
  f64       gross_leverage = 1.0;      // Σ|w| = gross_leverage  (Alpha101 `scale`)
  f64       winsorize_limit = 0.025;   // clamp tail outliers BEFORE the transform
  std::vector<f64>           to_target_weights(SignalView, const Universe&) const;
  std::vector<OrderPayload>  reconcile(span<const f64> w, const Universe&, const Portfolio&,
                                       const Market&, Timestamp now) const;   // trade = target − current
};
```
Pipeline: `winsorize → rank|zscore → demean (Σw=0) → gross-normalize (Σ|w|=L)`. NaN scores get zero weight.
`reconcile` truncates `target_shares = w·equity/price` toward zero and skips NaN/non-positive marks.

### `Portfolio` / `Holding` — avg-cost ledger  (`portfolio/portfolio.hpp`)
```cpp
class Portfolio {
  Portfolio(core::Decimal starting_cash, span<const InstrumentId> universe);
  void apply_fill(const exec::FillPayload&);     // open/increase/reduce/close/flip state machine
  void mark_to_market(const Market&);
  core::Decimal cash() const;                    // EXACT money
  f64 equity()/gross()/net()/leverage() const;   // f64 mark-to-market aggregates (fixed-order sums)
  const Holding& holding(InstrumentId) const;
};
```
Exact `Decimal` for cash / realized / fees / avg-price; f64 only for mark-to-market valuation. Realized for
a closed lot = `closed·(price − avg)·sign(position)`, summed in fixed universe-index order (determinism).

### `Market` / `InstrumentStats` — price + cost-stats book  (`loop/market.hpp`)
```cpp
struct InstrumentStats { f64 adv = 0; f64 sigma = 0; f64 spread = 0; };   // √-impact / cap / spread inputs
class Market {
  Market(span<const InstrumentId> universe, span<const InstrumentStats> stats);
  void update_prices(const MarketSlice&);        // last-value table; absent ⇒ keeps prior (frozen) mark
  f64  mark(InstrumentId) const;                 // NaN until first priced
  f64  bar_volume(InstrumentId) const;
  const InstrumentStats& stats(InstrumentId) const;
  void shift_mark(InstrumentId, f64 delta);      // permanent-impact hook (exec sim writes here)
};
```

### `RollingPanel<Cap>` — the PIT bar→panel bridge  (`loop/rolling_panel.hpp`)
```cpp
template <usize Cap> class RollingPanel {        // Cap = pow2 ≥ max_lookback
  RollingPanel(span<const InstrumentId> universe, usize max_lookback);
  void      append_sealed_row(const MarketSlice&);   // PIT: only after a bar closes; O(n_inst), zero-alloc
  PanelView view() const;                            // last max_lookback rows, newest-first, zero-copy
};
```

---

## Cost model — every coefficient is a knob  (`exec/execution_sim.hpp`)

Applied in order per settle: **eligibility firewall → volume cap (+partial spillover) → slippage →
temporary √-impact (→ fill price) → permanent impact (→ mark) → commission**. All defaults are the
Appendix-A starting points — **documented configurable defaults, not a calibrated fit** (the fit is Phase 5).

| Knob | Cfg field | Default | Effect |
|---|---|---|---|
| same-bar fill cheat | `FillCfg.allow_same_bar_fill` | **off** | look-ahead; opt-in only |
| latency | `LatencyCfg.latency_nanos` | `0` | order eligible at `queued_at + latency` |
| participation cap | `VolumeCapCfg.volume_limit` | `0.025` | `fillable ≤ vlim·bar_vol` per bar; remainder spills |
| slippage (VolumeShare) | `SlippageCfg.{k,cap_volshare}` | `k=0.1`, cap `2.5%` | `±k·share²` |
| slippage (FixedBps) | `SlippageCfg.{bps,cap_bps}` | `5 bps`, cap `10%` | `±bps/1e4` |
| spread floor | `InstrumentStats.spread` | `0` | always cross `±spread/2` |
| temp impact | `ImpactCfg.{Y,delta}` | `Y=1.0`, `δ=0.5` | `Y·σ·part^δ` → fill price (does NOT persist) |
| perm impact | `ImpactCfg.gamma` | `γ=0.314` | `½·γ·σ·part` → shifts the mark (persists) |
| commission (PerShare) | `CommissionCfg.{per_share,min_fee,max_pct}` | `$0.005 / $1 / 0.5%` | `clamp(max(\|q\|·ps, min), 0, max_pct·notional)` |
| commission (PerDollar) | `CommissionCfg.per_dollar_bps` | `15 bps` | `\|notional\|·bps/1e4` |

**Temporary impact** is paid into the fill price and vanishes; **permanent impact** shifts the instrument
mark so later P&L reflects the moved reference (Almgren-Chriss). Note: permanent impact moves the mark
*toward* the just-opened position, so it partially offsets the fill-price/commission drain in
mark-to-market equity — set `γ=0` to isolate the pure cost drain.

---

## Known limits / residuals (carried to the [backlog](ROADMAP.md))

- **`VmSignalSource` is blocked-on Phase 3** — staged behind `ATX_ENGINE_HAS_ALPHA_VM` (defined nowhere).
  The seam (`ISignalSource` + `SignalView`) is the frozen contract; the loop is `ScriptedSignalSource`-green.
- **`Schedule` is integer-cadence only** — no calendar-aware (month-end/session) gate yet.
- **`WeightPolicy.reconcile`/`to_target_weights` and `BacktestResult` accumulators allocate** at rebalance
  cadence / over the run — caller-provided scratch + a streaming sink are tracked optimizations.
- **`OrderType::Limit` is a minimal penetration gate** (no book depth/queue); the loop emits Market orders.
- **No probabilistic-fill RNG** — the sim is deterministic by construction (a seeded model is future work).
- **Cost defaults are NOT calibrated** — they are 2001–2003-era starting points; re-fit per universe/era
  in Phase 5. Never present a default-cost backtest as calibrated truth.
- **Benchmarks are Debug** upper bounds, not release figures.
