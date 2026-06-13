# Phase 2 — Backtest Loop + Portfolio + Execution Sim (built around the Alpha-DSL VM) — Implementation Plan

**Module:** `atx-engine` (`p0`)
**Phase theme:** Close the event-driven loop into a **runnable, cost-honest, deterministic backtest** whose
**strategy engine is the alpha-expression VM** (Phase 3). Bars flow off the Phase-1 spine into a
point-in-time **rolling panel**; on each rebalance the **VM evaluates a compiled alpha program** over that
panel to a cross-sectional signal; a **signal→weight policy** turns it into a dollar-neutral target
portfolio; an **ExecutionSimulator** (fill + slippage + **√-impact** + latency + partial fills) produces
fills; a **Portfolio** books P&L. The first end-to-end backtest of one alpha — with honest cost.
**Status:** frozen at kickoff (fossil — scope changes go in the ledger's *Plan adjustments*, not here).
**Authority:** [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Quality bar:
[`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Read first:** [`engine-architecture-audit.md`](engine-architecture-audit.md) §3 (invariants: determinism,
no look-ahead, **honest cost**) and §4 (√-impact mandate). This phase is the **execution spine** half of the
two-layer architecture (audit §2); the Phase-3 DSL is the research half — Phase 2 **builds around it**.
**Grounding:** every design choice is sourced in
[`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md)
(LEAN `AlgorithmManager`, Zipline `tradesimulation`/`slippage`/`commission`, NautilusTrader
`SimulatedExchange`/`LatencyModel`, backtrader `BackBroker`; Almgren 2005 + Tóth/Bouchaud √-law) and the
high-level [`../../research/renaissance-worldquant-deep-dive.md`](../../research/renaissance-worldquant-deep-dive.md)
§IV/§V. Section cites point at the loop deep-dive; formulas + defaults are reproduced in Appendix A.
**Strategy-core decision (this phase's defining choice):** the strategy engine is **VM-centric with a thin
signal seam** — `ISignalSource` yields a per-rebalance cross-sectional vector; the Phase-3 VM `Engine` is
**THE production implementation**; a `ScriptedSignalSource` test-double drives the loop so Portfolio /
ExecutionSim / loop mechanics are **green before Phase 3 lands**. There is **no arbitrary-C++-callback
strategy** — a strategy *is* a compiled alpha program. Signal→weight policy = **rank → dollar-neutral
(Σw=0) → gross-normalized (Σ|w|=1)** (Phase-4 combiner replaces it).
**Prerequisite:** Phase 1 (event spine) must be built. **Cross-phase:** only the `VmSignalSource` adapter
(P2-3) is blocked-on Phase 3; the rest of Phase 2 stands up on the spine + atx-core.

---

## 1. What this sprint delivers

The first runnable backtest. After Phase 2, a caller can do:

```cpp
namespace eng = atx::engine;
eng::RollingPanel panel(universe, max_lookback);             // PIT trailing frame (P2-2)
eng::VmSignalSource src(compiled_program, vm_engine);        // strategy = a compiled alpha program (P2-3)
eng::WeightPolicy   policy{.transform=Rank, .dollar_neutral=true, .gross_leverage=1.0}; // (P2-4)
eng::ExecutionSimulator exec(FillCfg{}, SlippageCfg{}, ImpactCfg{/*delta=0.5*/}, CommissionCfg{}); // (P2-6)
eng::Portfolio      pf(starting_cash);                       // (P2-5)

eng::BacktestLoop loop(spine /*Phase-1 bus+clock+feed*/, panel, src, policy, exec, pf, schedule);
const eng::BacktestResult r = loop.run();                    // (P2-7) → equity curve, fills, P&L, turnover
```

Concretely the phase ships:

1. **Signal / Order / Fill event payloads** completing the Phase-1 `Event` taxonomy (Phase 1 reserved the
   tags), plus the strong domain types the loop trades in (target weight, order qty, fill price).
2. A **point-in-time rolling panel** (`RollingPanel`) — the bridge that accretes streaming `MarketEvent`s
   into a bounded, column-major `date × instrument` frame the VM reads. **Append-after-close ⇒ no
   look-ahead; bounded to `max_lookback` ⇒ fixed memory.** This is the Phase-1↔Phase-3 integration.
3. The **`ISignalSource` seam** — `VmSignalSource` (wraps the Phase-3 `Engine`, the production path) and
   `ScriptedSignalSource` (test double that replays canned signal vectors, so the loop is testable now).
4. A **signal→weight policy** — rank → dollar-neutral → gross-normalized target weights, then
   `order_target_percent`-style reconciliation (target − current → trade list).
5. A **Portfolio** with average-cost-basis accounting: open/increase/reduce/close/flip state machine,
   realized/unrealized P&L, cash ledger, gross/net exposure, leverage, time-weighted returns.
6. An **ExecutionSimulator** with pluggable models applied in order **fill → slippage → √-impact →
   commission → latency/partial**: a look-ahead firewall (a decision on bar `t` fills no earlier than
   `t+1`), volume-participation caps with partial-fill spillover, **temporary impact → fill price /
   permanent impact → mark**, all coefficients configurable.
7. The **BacktestLoop driver** — the deterministic per-slice crank wiring the Phase-1 spine to all of the
   above (settle-prior-fills → mark → VM eval → weights → orders → queue → next-slice fill → sample).
8. An **end-to-end integration + determinism + cost-honesty + no-look-ahead** harness and a bench.

**Not** in this phase: no alpha store / correlation-turnover gates / mega-alpha combiner / Barra risk model
(Phase 4); no cost *calibration*, capacity, or walk-forward/deflated-Sharpe validation (Phase 5 — Phase 2
ships the cost *model* with sane defaults, not the fit); no live broker (later module). The VM itself is
Phase 3 — Phase 2 builds the loop **around its contract**.

**Maps to research:** [`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md)
§1 (loop), §2 (portfolio), §3 (fill/slippage/commission), §4 (√-impact/Almgren), §5 (bar→panel bridge),
§6 (signal→weight), §7 (determinism), §9 (the C++ design this plan refines).

---

## 2. Reference architecture (open-source-grounded)

The loop is the **LEAN `AlgorithmManager` / Zipline `every_bar`** per-slice crank, with the user-strategy
slot **replaced by the alpha VM**. Canonical flow (one `SimClock` slice = one sealed date/bar):

```
  Phase-1 SPINE                         ATX BACKTEST LOOP (Phase 2)                    strategy core = VM
  ┌──────────────┐
  │ DataHandler  │  MarketEvent   ┌───────────────────────────────────────────────────────────────┐
  │ (k-way merge)│ ─────────────▶ │ 1. mark prices      2. settle PRIOR-slice fills  → Portfolio    │
  │ SimClock     │   via EventBus │     (Portfolio.mark)    (ExecutionSim.settle_pending)           │
  │ EventBus     │  (Disruptor)   │ 3. append_sealed_row → RollingPanel (PIT, bounded max_lookback) │
  └──────────────┘                │ 4. if schedule.fires(t):                                        │
                                  │      signal  = ISignalSource.evaluate(panel.view())  ◀── VM ────┐│
                                  │                 ( VmSignalSource → Phase-3 Engine )    Program  ││
                                  │      weights = WeightPolicy.to_target(signal)  rank→Σw=0→Σ|w|=1 ││
                                  │      orders  = WeightPolicy.reconcile(weights, portfolio)       ││
                                  │      ExecutionSim.queue(orders, t)   ── fill on NEXT slice ──────┘│
                                  │ 5. metrics.sample(t, portfolio)   equity / returns / turnover    │
                                  └───────────────────────────────────────────────────────────────┘
                                            decide at close t  →  fill at t+1  (no look-ahead)
```

Each subsystem maps to a proven reference (loop deep-dive §8):

| ATX component | Reference (open-source) | What we borrow |
|---|---|---|
| `BacktestLoop` driver | LEAN `AlgorithmManager.Run`; Zipline `AlgorithmSimulator.transform` (`every_bar`) | per-slice order: mark → settle-prior-fills → strategy → queue → next-slice fill → sample |
| decide/fill firewall | LEAN `pricesEndTime <= order.Time → no fill`; Zipline blotter settles at top of next bar; backtrader fills at next open | a decision on bar `t` fills no earlier than `t+1` |
| `ISignalSource` (strategy slot) | Zipline `handle_data` / LEAN `OnData` — **replaced by the alpha VM** | the strategy *is* a compiled alpha program (VM), not arbitrary code |
| `RollingPanel` bridge | LEAN `RollingWindow<T>`+`History`; Zipline `data.history(bar_count)` | bounded trailing window, append-after-close, PIT by construction |
| `ExecutionSimulator` | Zipline `SimulationBlotter`+`slippage.py`; LEAN `EquityFillModel`/`VolumeShareSlippageModel`; Nautilus `OrderMatchingEngine`/`LatencyModel` | open-order queue; slippage+commission on settle; volume cap + partial-fill spillover; latency defer |
| √-impact model | Almgren-Thum-Hauptmann-Li 2005 (`γ=0.314`,`η=0.142`); Tóth/Bouchaud √-law | temporary→fill, permanent→mark; configurable `δ∈[0.45,0.65]` |
| `Portfolio`/`Holding` | LEAN `SecurityHolding`/`SecurityPortfolioManager`; Zipline `Ledger`/`Portfolio` | avg-cost basis, realized/unrealized split, mark-to-market, exposure/leverage |
| `WeightPolicy` | Zipline `order_target_percent`; Alpha101 `scale`/`rank` | rank/z → demean (Σw=0) → gross-norm (Σ|w|=1) → diff to trades |
| rebalance schedule | Zipline `schedule_function`; LEAN `Schedule.On` | per-bar panel update; scheduled eval/rebalance (daily-close default) |

> **Build-order note.** The Phase-1 spine is the harder *correctness* surface and ships first. Phase 2 plugs
> the **execution/accounting** half onto it; the Phase-3 VM plugs into the **`ISignalSource` seam**. Because
> the seam admits a `ScriptedSignalSource`, the entire loop is provable end-to-end **before** the VM exists —
> only the one-line `VmSignalSource` adapter is blocked-on Phase 3.

---

## 3. The VM-centric strategy engine (the defining design)

> The directive: **make the alpha-DSL VM the core of the strategy engine and build directly around it.**
> This phase encodes that as an architecture, not a callback.

### 3.1 A strategy *is* a compiled alpha program

In a classic backtester the strategy is arbitrary user code (`handle_data`/`OnData`) that reads history and
places orders. Here the strategy is **a compiled alpha program evaluated by the VM** — the WorldQuant/Qlib
model where strategies are *expressions*, not imperative code. The loop never calls user C++; it calls the
**`ISignalSource`** seam, whose production implementation is the Phase-3 `Engine`:

```cpp
namespace atx::engine {
// The one seam between the loop and "the strategy". Yields a cross-sectional signal over the live
// universe for the current rebalance date. Pure: same panel view → same signal (determinism).
class ISignalSource {
 public:
  virtual ~ISignalSource() = default;
  // panel: PIT trailing frame (knowledge_ts ≤ now). Returns one f64 per instrument (NaN = no opinion).
  [[nodiscard]] virtual Result<SignalView> evaluate(PanelView panel) = 0;
  [[nodiscard]] virtual usize max_lookback() const noexcept = 0;   // sizes the RollingPanel
};

// PRODUCTION: the strategy core. Wraps a Phase-3 compiled Program + vectorized VM Engine.
class VmSignalSource final : public ISignalSource {            // blocked-on Phase 3
  // holds alpha::Program + alpha::Engine; evaluate() runs the VM over `panel`, returns the alpha column.
  // max_lookback() forwards the program's compile-time lookback (Phase-3 lookback analysis).
};

// BRING-UP / TEST DOUBLE: replays canned signal vectors. Lets P2-2..P2-8 go green without the VM.
class ScriptedSignalSource final : public ISignalSource {      // not blocked
  // holds a pre-baked schedule of (date → signal vector); evaluate() returns the next; max_lookback() = N.
};
}
```

**Why a seam and not a hardwire.** The seam is *still* VM-centric — `VmSignalSource` is the only production
path — but it (a) makes the loop, portfolio, and execution sim **independently testable** before Phase 3,
(b) gives the differential/determinism harness a trivial oracle signal to isolate loop bugs from VM bugs,
and (c) lets Phase 4's combiner later present *itself* as an `ISignalSource` (mega-alpha = one signal source)
with zero loop changes. One narrow virtual call per **rebalance** (not per event) — its cost is negligible
against a VM evaluation, so the abstraction is free on the hot path.

### 3.2 The panel is the contract between spine and VM

The VM wants a `date × instrument` panel; the spine produces a stream of point-in-time `MarketEvent`s. The
**`RollingPanel`** (P2-2) is the bridge and it owns the no-look-ahead guarantee on the *data* side
(the SimClock owns it on the *clock* side): a bar is written **only after it closes** and the VM reads the
panel **before** any order is queued. The panel is **bounded to `max_lookback`** (from
`ISignalSource::max_lookback()`, itself the Phase-3 compile-time deepest `ts_*` window) so memory is fixed
regardless of backtest length (loop deep-dive §5.2).

### 3.3 Rebalance cadence decouples eval cost from data rate

The panel updates **every bar** (cheap: one column write); the **VM evaluates only on the rebalance
cadence** (daily-close default — matches the Alpha101 ~0.6–6.4-day horizon and controls turnover/cost). A
`Schedule` gates step 4 of the loop (Zipline `schedule_function` / LEAN `Schedule.On`).

---

## 4. Loop invariants (build them in from unit 1)

The audit §3 invariants, made concrete for this phase (loop deep-dive §1.5, §7):

1. **No look-ahead — decide/fill separation.** A decision on bar `t` fills **no earlier than `t+1`**. The
   loop *settles prior-slice orders first*, then runs the VM, then *queues* new orders for the next slice;
   the ExecutionSim's fill model additionally refuses any fill whose bar `end_time ≤ order.queued_at`
   (LEAN's stale-data firewall). Same-bar-close fills are available only behind an explicit, named "cheat"
   flag — **never the default** (backtrader convention).
2. **Determinism — byte-identical replay.** Same feed → identical fills, P&L, and equity curve. Enforced by:
   stable event order `(knowledge_ts, source_idx, symbol_id, seq)` (Phase-1 spine); **fixed-order** cash/P&L
   summation; deterministic open-order processing (insertion/order-id order) so the volume cap allocates
   scarce liquidity reproducibly; any probabilistic fill RNG **seeded and logged**. Proven by P2-8's
   run-to-run P&L-hash equality.
3. **Honest cost — modeled before belief.** The ExecutionSim applies **spread + slippage + size-dependent
   √-impact + commission + latency** by default. The audit's hard rule (§4): a backtest that ignores
   size-dependent impact passes strategies that lose at scale. Phase 2 ships the *model* (defaults from
   Almgren/Tóth); Phase 5 *fits* the coefficients.
4. **No survivorship — delisted present.** The universe is point-in-time (Phase-1 DataHandler keeps delisted
   symbols with their final bar); the panel's per-date membership mask excludes them only *after* delisting,
   never retroactively.
5. **No hot-path allocation.** The per-slice crank, panel update, and fill path allocate nothing in steady
   state (atx-core arena/pool/fixed containers); compile/setup may allocate.

---

## 5. Execution-sim cost model (the cost-honesty core; loop deep-dive §3–§4, §9.3)

Models are **pluggable** (strategy pattern, like LEAN/Zipline) and applied in this order per settle:

```
eligible? : bar.end_time > order.queued_at  AND  venue open          ← look-ahead firewall
volume cap: fillable = min(|open_qty|, volume_limit·bar_vol − vol_for_bar)   ← partial; rest rolls forward
slippage  : fill = ref · (1 ± k·share²)        [VolumeShare, k=0.1, share=fillable/bar_vol, cap 2.5%]
            or fill = ref · (1 ± bps/1e4)        [FixedBps, bps=5, cap 10%]   ; always cross ±spread/2
impact    : temporary → fill price :  temp = Y·σ·(part)^δ,  part = fillable/ADV,  δ∈[0.45,0.65] (def 0.5)
            permanent → mark        :  perm = ½·γ·σ·(fillable/ADV)  (linear, γ≈0.314); shift instrument mark
commission: max(|qty|·per_share, min_fee) capped at max_pct·notional   [def $0.005 / $1 / 0.5%]
            or |notional|·per_dollar_bps
latency   : defer order eligibility queued_at → queued_at + latency (Nautilus in-flight queue)
```

**Temporary impact is paid (goes into the fill price); permanent impact persists (shifts the mark)** so
future P&L on the position reflects the moved reference (Almgren-Chriss; loop deep-dive §4.3). **Every
coefficient — `Y, δ, γ, η, volume_limit, bps, per_share, min_fee, max_pct` — is configuration, never
hardcoded** (the OSS defaults drift from reality; §10, §Appendix A). Phase 2 proves the *mechanism* with
documented defaults; Phase 5 calibrates.

---

## 6. Cross-module + cross-phase dependencies (Pattern B — blocked-on)

Phase 2 consumes Phase-1 (intra-engine), Phase-3 (one adapter), and atx-core layers. Per module.md the edge
lives here. The **seam** (§3.1) keeps all but `VmSignalSource` independent of Phase 3.

| Phase-2 unit | Blocked-on | Note |
|---|---|---|
| P2-1 payloads | Phase-1 `Event`; atx-core **L8 `domain`** (Price/Qty/Side/Notional/Symbol) | extends the reserved Signal/Order/Fill tags |
| P2-2 RollingPanel | atx-core **L9 `column`/`frame`**, **L3 `ring_buffer`**, L2 `aligned`; Phase-1 `MarketEvent`/`SimClock` | the bridge |
| P2-3 ISignalSource | `ScriptedSignalSource`: none · **`VmSignalSource`: Phase-3 `Engine`/`Program`** | only the VM adapter is cross-phase blocked |
| P2-4 WeightPolicy | atx-core **L6 `cross_section`** (rank/zscore) or reuse Phase-3 `CsRank`; `math` | signal→weights |
| P2-5 Portfolio | atx-core **L8 `domain`**, **L1 `decimal`** (cash/realized money), **L3 `hash_map`/`fixed_vector`** | exact-money ledger; dense holdings |
| P2-6 ExecutionSim | atx-core **L8 `domain`**, `math` (sqrt/pow impact), **L1 `random`** (seeded, optional prob-fill) | the cost stack |
| P2-7 BacktestLoop | Phase-1 `EventBus`/`SimClock`/`DataHandler`; all of the above | the crank |
| P2-8 harness/bench | atx-core **L1 `hash`** (P&L/run hash); bench harness | determinism evidence |

**Sequencing consequence:** Phase 2 **opens after Phase 1** (it needs the spine). Within Phase 2, P2-1..P2-8
build against atx-core L8/L9/L6/L3/L2/L1 contracts (Pattern-B; blocked units stage red behind
`atx_engine_pending`). The single cross-phase edge — `VmSignalSource` — stays red until Phase 3 merges; the
**`ScriptedSignalSource` keeps P2-7/P2-8 fully green** end-to-end without it. A residual is filed against
both the atx-core build (L6/L8/L9) and the Phase-3 tracking (the `Engine` contract) so those agents see the
dependency. The ROADMAP cross-module table is canonical.

---

## 7. Cross-cutting gates (agent.md — every unit)

- **TDD:** failing GoogleTest first; one behavior per `TEST`; `Subject_Condition_ExpectedResult`; cover
  happy path, **boundaries** (empty universe, single instrument, zero-volume bar, first/last slice, flat→
  long→short flip), error paths (insufficient cash, NaN signal, unfillable order), invariants (`EXPECT_DEATH`
  on monotonic-clock / same-bar-fill / negative-cash-if-disallowed).
- **Determinism is a test, not a hope:** P2-8 asserts identical input → identical equity/P&L hash, and a
  **mutation** (reorder fills, perturb one price) flips the hash (not vacuously passing).
- **Cost-honesty is a test:** a sized order’s fill price is provably worse than mid by the modeled
  slippage+impact; a larger order pays more (monotonic in participation); zero-size → zero cost.
- **No-look-ahead is a test:** an order queued at `t` never fills at `t`; truncating the feed after `t`
  leaves all fills/marks at `≤ t` byte-identical.
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) / `-Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Werror` (Clang); clang-tidy (project `.clang-tidy`) + clang-format clean.
- **Sanitizers:** ASan + UBSan every run; TSan on any threaded path (the loop is single-threaded
  deterministic in backtest mode — keep it that way; live multi-threading is a later module).
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]` where they hold; `Result<T>`/`Status`
  for expected failures, **never throw** for control flow; **exact-money `Decimal`** for cash/realized P&L
  (not raw `double`) — marks/unrealized may be `f64`; strong domain types across boundaries (no naked
  `double` price/qty); exhaustive `enum class` switches (`OrderType`/`EventType`/`Transform`, no `default`);
  no owning raw pointers (models held by `unique_ptr`/value; Rule of Zero); functions ≤ ~60 lines;
  `// SAFETY:` on every deviation.
- **Zero hot-path allocation:** crank/panel/fill path allocate nothing in steady state.

---

## 8. Unit decomposition (P2-0 … P2-8)

Dispatch **sequential** (each consumes the prior's headers). TDD red→green→refactor within each. The loop
(P2-7) and harness (P2-8) go green on the **`ScriptedSignalSource`**; `VmSignalSource` flips green when
Phase 3 merges.

---

### P2-0 — Module scaffold + CMake + ledger  *(marker commit)*

**Scope.** Extend the `atx::engine` tree for the loop subsystem and open the Phase-2 ledger. No logic.

**Deliverables.** `include/atx/engine/exec/`, `portfolio/`, `loop/` (or a flat `engine/` per the Phase-1
layout) with `fwd.hpp` forward-decls (`ISignalSource`, `RollingPanel`, `Portfolio`, `Holding`,
`ExecutionSimulator`, `Order`, `Fill`, `WeightPolicy`, `BacktestLoop`, `BacktestResult`). Tests/bench wiring;
`scaffold_test` proves link; `atx_engine_pending` label for blocked targets.

**Acceptance.** `cmake --build` green; `scaffold_test` passes; marker commit
`docs(p2-0): open phase-2 sprint ledger` with `Base: <branch> @ <SHA>`.

---

### P2-1 — Signal / Order / Fill payloads + trade domain types  *(blocked-on Phase-1 `Event`, atx-core L8 `domain`)*

**Scope.** Complete the Phase-1 `Event` taxonomy: the `Signal`, `Order`, `Fill` payload PODs (Phase 1
reserved the `EventType` tags and sized the ring slot for them). Trivially-copyable, ride the Disruptor.

**Data structures.**
```cpp
namespace atx::engine {
enum class Side : u8 { Buy, Sell };
enum class OrderType : u8 { Market, Limit };                 // exhaustive switches; no default

struct SignalPayload { InstrumentId id; f64 value; };       // VM output per instrument (NaN = no opinion)
struct OrderPayload  { InstrumentId id; i64 qty; OrderType type; atx::core::Decimal limit;
                       Timestamp queued_at; };               // signed qty; limit unused for Market
struct FillPayload   { InstrumentId id; i64 qty; atx::core::Decimal price; atx::core::Decimal fee;
                       f64 impact; Timestamp t; };           // realized fill; impact for attribution
static_assert(std::is_trivially_copyable_v<OrderPayload>);
}
```

**Contract.** `knowledge_ts ≥ event_ts` inherited from Phase-1. Prices/fees are exact-money `Decimal`;
`qty` signed integer shares. Payloads fit the Phase-1 `kPayloadBytes` budget (assert).

**Tests.** payload round-trips through `Event`; trivially-copyable/size asserts; `Side`/`OrderType`
exhaustive-switch compiles; signed-qty sign conventions; `Decimal` price exactness. Boundary: zero-qty order
rejected; epoch-zero `queued_at`.

**Acceptance.** `/W4 /WX` clean; green (or `blocked-on L8`). Commit `feat(p2-1): signal/order/fill payloads`.

---

### P2-2 — RollingPanel (bar→panel bridge, PIT, bounded)  *(blocked-on atx-core L9 `frame`, L3 `ring_buffer`)*

**Scope.** The bridge from the Phase-1 `MarketEvent` stream to the VM's `date × instrument` panel. A bounded
trailing ring of cross-sections, column-major per field, written **after a bar closes**, read **before** the
next decision. Owns the data-side no-look-ahead guarantee + fixed memory.

**Data structure.**
```cpp
namespace atx::engine {
template <usize Cap>                                          // Cap = pow2 ≥ max_lookback
class RollingPanel {
  static_assert((Cap & (Cap - 1)) == 0, "Cap must be power of two");
 public:
  RollingPanel(std::span<const InstrumentId> universe, usize max_lookback) noexcept;
  void append_sealed_row(const MarketSlice& s) noexcept;     // head=(head+1)&(Cap-1); write cols; NaN absent
  [[nodiscard]] PanelView view() const noexcept;             // last max_lookback rows, PIT, newest-first
 private:
  // column-major f64 [Cap rows × n_instruments] per field (close/volume/vwap/returns/…), L2-aligned.
  // per-row universe membership bitmask; row sealed only when the slice's date is complete.
};
}
```

**Contract.** A row is appended only after its bar `end_time ≤ now` (the current in-progress bar is never
visible). `view()` exposes exactly `max_lookback` trailing rows; absent symbols are `NaN`; membership mask is
point-in-time (delisted excluded only after delisting). Memory = `Cap · n_inst · n_fields · 8 B`, fixed.

**Tests.** append + read trailing window; **PIT proof** — the row for an unsealed/future bar is invisible;
eviction past `max_lookback` (oldest dropped); column-major layout (cross-section = one row contiguous);
NaN for missing symbol; membership excludes delisted after its final bar; wrap-around at `Cap`. Boundary:
`max_lookback=1`; single instrument; universe larger than `Cap` cols; all-NaN date.

**Perf.** Append O(n_instruments) writes; no alloc after construction; prefetch-friendly column writes.

**Acceptance.** `/W4 /WX` clean; green (or `blocked-on L9/L3`). Commit `feat(p2-2): point-in-time rolling panel`.

---

### P2-3 — ISignalSource seam (Scripted + Vm)  *(`VmSignalSource` blocked-on Phase 3)*

**Scope.** The one seam between the loop and "the strategy" (§3.1). `ScriptedSignalSource` (test double, not
blocked) and `VmSignalSource` (production, wraps the Phase-3 `Engine`/`Program`).

**Contract.** `evaluate(PanelView)` is **pure** (same panel → same signal — determinism); returns one `f64`
per live-universe instrument (`NaN` = no opinion). `max_lookback()` sizes the `RollingPanel`:
`VmSignalSource` forwards the Phase-3 program's compile-time lookback; `ScriptedSignalSource` returns its
configured `N`. `VmSignalSource::evaluate` runs the VM over the panel and returns the alpha column — **no
allocation per call beyond the VM's own pooled slots**.

**Tests.** `ScriptedSignalSource` replays a baked schedule deterministically; signal length == universe;
`NaN` passthrough; `max_lookback` reported correctly. `VmSignalSource` (staged red until Phase 3): wraps a
trivial 1-op program, matches the Phase-3 `Engine` output on a fixture. Boundary: empty signal (all-NaN);
single instrument.

**Acceptance.** `/W4 /WX` clean; `ScriptedSignalSource` green; `VmSignalSource` `blocked-on Phase-3` staged.
Commit `feat(p2-3): signal-source seam (scripted + vm)`.

---

### P2-4 — Signal→weight policy (rank → dollar-neutral)  *(blocked-on atx-core L6 `cross_section`)*

**Scope.** Turn a cross-sectional signal into a target portfolio and a trade list (§3, loop deep-dive §6).
Default policy: rank → dollar-neutral (Σw=0) → gross-normalized (Σ|w|=1) → `order_target_percent` reconcile.

**Data structure.**
```cpp
namespace atx::engine {
enum class Transform : u8 { Rank, ZScore };                  // exhaustive
struct WeightPolicy {
  Transform transform     = Transform::Rank;
  bool      industry_neutral = false;                        // demean within group (later)
  bool      dollar_neutral   = true;                         // center Σw = 0
  f64       gross_leverage   = 1.0;                          // Σ|w| = gross_leverage

  [[nodiscard]] AlignedVec<f64> to_target_weights(SignalView, const Universe&) const;   // §6 pipeline
  [[nodiscard]] FixedVector<OrderPayload> reconcile(std::span<const f64> w,
                                                    const Portfolio&, const Market&,
                                                    Timestamp now) const;               // target − current
};
}
```

**Contract.** Pipeline = winsorize → `rank`/`zscore` (optional `indneutralize`) → subtract mean (Σw=0) →
divide by Σ|w| × leverage (= Alpha101 `scale`) → `target_shares = w·equity/price` → `trade = target −
current`. NaN/out-of-universe instruments get zero weight. Deterministic rank tie-break (reuse Phase-3
`CsRank` convention). Σw=0 and Σ|w|=gross to documented tolerance.

**Tests.** dollar-neutral (Σw≈0); gross-normalized (Σ|w|≈gross); rank monotonic in signal; NaN→zero weight;
reconcile = target−current (no-op when already on target); flip (long→short) generates the full delta.
Boundary: all-equal signal (zero net weight); single instrument (degenerate; flat or rejected); zero equity.

**Acceptance.** `/W4 /WX` clean; green (or `blocked-on L6`). Commit `feat(p2-4): rank dollar-neutral weight policy`.

---

### P2-5 — Portfolio accounting  *(blocked-on atx-core L8 `domain`, L1 `decimal`, L3)*

**Scope.** Average-cost-basis position keeping + P&L (loop deep-dive §2). The open/increase/reduce/close/flip
state machine, realized/unrealized split, cash ledger, exposure/leverage, returns.

**Data structure.**
```cpp
namespace atx::engine {
struct Holding {
  InstrumentId id;
  i64             qty       = 0;                  // signed shares
  atx::core::Decimal avg_price{};                 // average-cost basis (exact money)
  atx::core::Decimal realized{};                  // booked P&L (ex-fees)
  atx::core::Decimal fees{};
  f64             mark      = 0.0;                 // last trade/close/mid
  [[nodiscard]] f64 market_value() const noexcept { return static_cast<f64>(qty) * mark; }
  [[nodiscard]] f64 unrealized()   const noexcept;            // qty·(mark − avg_price)
};
class Portfolio {
 public:
  void apply_fill(const FillPayload&) noexcept;               // §2.4 open/increase/reduce/close/flip
  void mark_to_market(const Market&) noexcept;
  [[nodiscard]] atx::core::Decimal cash()  const noexcept;
  [[nodiscard]] f64 equity()   const noexcept;                // cash + Σ market_value
  [[nodiscard]] f64 gross()    const noexcept;                // Σ |market_value|
  [[nodiscard]] f64 net()      const noexcept;                // Σ market_value
  [[nodiscard]] f64 leverage() const noexcept;                // gross / equity
 private:
  atx::core::Decimal cash_{};
  hash_map<InstrumentId, Holding> holdings_;                  // or dense fixed_vector over the universe
};
}
```

**Contract.** `apply_fill` implements: **increase** → weighted-avg price; **reduce/close** → book
`closed·(p−avg)·sign(pos)` realized; **flip** → reset avg to fill price for the remainder; cash `-= qty·p +
fee`. Cash + realized in **exact `Decimal`**, summed in fixed order (determinism). Unrealized only nonzero
when `qty≠0`.

**Tests.** open→increase weighted avg; partial reduce books proportional realized; full close zeroes
position + avg; **flip** (long→short) books the long leg's P&L and opens the short at fill price; cash ledger
balances; equity = cash + Σ mv; gross/net/leverage; dollar-neutral book → net≈0. Boundary: reduce-to-zero;
over-reduce (flip); zero-price guard; first fill from flat.

**Acceptance.** `/W4 /WX` clean; green (or `blocked-on L8/L1`). Commit `feat(p2-5): portfolio accounting`.

---

### P2-6 — ExecutionSimulator (fill→slippage→√-impact→commission→latency→partial)  *(blocked-on atx-core L8, `math`, L1 `random`)*

**Scope.** The cost-honesty core (§5, loop deep-dive §3–§4, §9.3). Pluggable models; the look-ahead
firewall; volume cap + partial-fill spillover; temporary→fill / permanent→mark impact.

**Data structures.**
```cpp
namespace atx::engine {
struct Fill;  // = FillPayload
class FillModel       { /* eligibility: bar.end_time > order.queued_at; touch price */ };
class SlippageModel   { /* VolumeShare price·k·share² | FixedBps ±bps/1e4; ±spread/2 floor */ };
class ImpactModel     { /* temp = Y·σ·part^δ → fill; perm = ½·γ·σ·(q/ADV) → mark */ };
class CommissionModel { /* max(|q|·per_share,min) cap max_pct·notional | per_dollar bps */ };
class LatencyModel    { /* defer queued_at → eligible_at */ };

class ExecutionSimulator {
 public:
  void queue(std::span<const OrderPayload>, Timestamp) noexcept;     // new orders → open set
  // settle each open order against THIS slice (it was queued on a prior slice); emit Fills, update mark-shift.
  [[nodiscard]] FixedVector<FillPayload> settle_pending(Timestamp, const Market&) noexcept;
 private:
  FixedVector<OrderPayload> open_;                                   // deterministic iteration order
  // per-bar volume_for_bar accumulator; pluggable models held by value/unique_ptr
};
}
```

**Contract.** Apply order: eligibility firewall (`bar.end_time > queued_at`) → volume cap
(`fillable = min(open_qty, volume_limit·bar_vol − vol_for_bar)`; remainder stays open → next slice) →
slippage → **temporary impact into fill price** → **permanent impact shifts the instrument mark** →
commission → emit `Fill`. Process open orders in fixed order (determinism). Every coefficient configurable
(Appendix A); **no hardcoded magic numbers** beyond documented defaults. Any RNG (optional prob-fill) seeded
+ logged.

**Tests (cost-honesty + determinism).** firewall: order queued at `t` does **not** fill at `t`; fills at
`t+1`. Fill price worse than mid by modeled cost; **monotonic in participation** (bigger order → worse fill);
zero-size → zero cost. Volume cap → partial fill; remainder fills next slice; sum of partials = order qty.
Permanent impact shifts mark (next mark reflects it); temporary does not persist. Commission min/max-cap
applied. Deterministic across runs; seeded RNG reproduces. Boundary: zero bar volume (no fill); limit not
penetrated (no fill); exact-cap fill.

**Bench.** settle throughput (orders/s); fill-path ns/order. Measured only; host/build recorded.

**Acceptance.** `/W4 /WX` clean; green (or `blocked-on L8`); bench recorded. Commit
`feat(p2-6): execution simulator (fill+slippage+impact+commission+latency)`.

---

### P2-7 — BacktestLoop driver  *(green on `ScriptedSignalSource`; uses Phase-1 spine)*

**Scope.** The deterministic per-slice crank (§2, loop deep-dive §9.1) wiring the Phase-1 spine to panel →
signal source → policy → exec → portfolio → metrics. The settle-then-decide-then-queue order **is** the
no-look-ahead guarantee.

**Implementation shape.**
```cpp
void BacktestLoop::on_time_slice(Timestamp t, const MarketSlice& slice) {
  market_.update_prices(slice);                       // 1. mark (prices only)
  portfolio_.mark_to_market(market_);                 // 2. equity on new marks
  for (const FillPayload& f : exec_.settle_pending(t, market_))  // 3. settle PRIOR-slice orders
    portfolio_.apply_fill(f);
  panel_.append_sealed_row(slice);                    // 4. write completed bar (PIT)
  if (schedule_.fires(t)) {                            // 5. cadence gate
    auto sig = signal_.evaluate(panel_.view());       // 6. strategy = VM (or scripted)
    if (sig) {
      auto w  = policy_.to_target_weights(*sig, universe_);
      exec_.queue(policy_.reconcile(w, portfolio_, market_, t), t);   // 7. fill on NEXT slice
    }
  }
  metrics_.sample(t, portfolio_);                     // 8. equity/returns/turnover
}
```

**Contract.** A freshly queued order **never** fills in the same slice (settle precedes queue). Single
logical thread owns ordering (deterministic backtest mode). The loop is agnostic to which `ISignalSource` it
holds — `Scripted` for bring-up, `Vm` for production. `BacktestResult` carries the equity curve, fill log,
final P&L, and turnover.

**Tests (end-to-end on `ScriptedSignalSource`).** a 3-instrument, 10-bar synthetic feed + scripted signal →
expected positions/fills/equity (hand-computed); order queued at `t` fills at `t+1`; cadence gate fires only
on schedule; flat-signal → no trades; equity = cash + MV each slice. Boundary: single bar (no fill possible);
empty universe; signal NaN (no orders).

**Acceptance.** `/W4 /WX` clean; green on `ScriptedSignalSource`. Commit `feat(p2-7): backtest loop driver`.

---

### P2-8 — Integration: determinism · cost-honesty · no-look-ahead · bench · sprint close

**Scope.** The proof. A full single-alpha backtest (`ScriptedSignalSource` now; swap to `VmSignalSource`
once Phase 3 lands) over a synthetic multi-symbol feed incl. a delisted symbol and NaN gaps, asserting the
three invariants, then the bench, then the sprint-close ceremony.

**Design.**
- **Determinism:** run the backtest twice; fold an atx-core `hash`/wyhash over the ordered `(t, fill,
  position, equity-bits)` stream; assert identical hashes. A **mutation** (reorder fills, perturb one price,
  add a late bar) yields a **different** hash (not vacuously passing).
- **Cost-honesty:** a sized rebalance’s realized fills are provably worse than the frictionless marks by the
  modeled slippage+impact; doubling order size increases cost (monotone); turning impact off recovers the
  frictionless P&L (isolates the cost term).
- **No-look-ahead:** truncating the feed after date `t` leaves every fill/mark/equity at `≤ t`
  byte-identical (future invisible); an order decided at `t` never appears as a fill at `t`.
- **Survivorship:** the delisted symbol trades up to its final bar and is excluded thereafter (no
  retroactive removal).

**Bench.** end-to-end backtest throughput (slices/s; bars·symbols/s) on the synthetic feed; record host/
build/feed-shape. Measured only.

**Sprint close (sprint.md ceremony).**
1. Lift residuals to ROADMAP future-work backlog (`VmSignalSource` green-gate when Phase 3 merges; short
   borrow-cost accrual hook; limit-order book realism; cost *calibration* → Phase 5; intraday fills).
2. Update ROADMAP Phase 2 status table (`⏳ → ✅ <sha>` / `⚠️ partial`).
3. Bump ROADMAP `Last reviewed`.
4. Write "What Phase 2 proves / Next sprint priorities" in the ledger (baton → Phase 3 VM integration,
   then Phase 4 combiner).
5. Write `phase-2.md` user reference (the loop API + cost-model knobs + the determinism/cost-honesty/no-look-
   ahead guarantees + the `ISignalSource` contract Phase 3 plugs into).
6. Merge worktree → master (`--no-ff`, `merge: phase-2 — backtest loop`). **Do not push unless asked.**

**Acceptance.** Determinism/cost-honesty/no-look-ahead suites green on `ScriptedSignalSource`; bench
recorded; close items 1–6 done; ledger + ROADMAP reconciled. Commit `docs(p2-close): close phase-2 — 9
units, <M> tests`.

---

## 9. Sub-agent dispatch checklist (per unit)

Each brief includes (sprint.md): worktree path + branch (`worktree-phase-2-backtest-loop`); the unit's scope
**quoted** from this plan; acceptance criteria; the verbatim *"Marker-commit pattern is mandatory: commit
before stopping or work is lost."*; commit format `feat(p2-N): <summary>`; predecessor SHAs; ledger-row
instructions; and the handoff block below with the atx-core headers (`error.hpp`, `types.hpp`, `decimal.hpp`,
`domain/*.hpp`, and the as-built `series/frame.hpp`, `cross_section.hpp`) **and the Phase-1 spine headers**
(`event.hpp`, `event_bus.hpp`, `sim_clock.hpp`, `data_handler.hpp`) as the positive style + API reference.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx):
Governed by .agents/cpp/agent.md (safety-critical-grade C++20). Use atx-core public headers (error.hpp,
types.hpp, decimal.hpp, domain/*, series/*, cross_section.hpp) and the Phase-1 spine headers (event,
event_bus, sim_clock, data_handler) as the positive style + API reference: module-level intent comment,
grouped types/APIs, explicit ownership/lifetime/error contracts, concise comments that explain invariants
and non-obvious control flow — never narrate code.

This phase builds the backtest loop AROUND the alpha-DSL VM. The strategy is NOT arbitrary C++ — it is a
compiled alpha program reached through the ISignalSource seam (VmSignalSource = production; ScriptedSignalSource
= test double so the loop is green before Phase 3). One virtual call per REBALANCE, never per event.

Three invariants are tests, not hopes:
 (1) NO LOOK-AHEAD — a decision on bar t fills no earlier than t+1; settle prior-slice orders BEFORE running
     the signal source, queue new orders AFTER; the fill model refuses any bar with end_time <= order.queued_at.
 (2) DETERMINISM — identical feed => byte-identical fills/P&L/equity; stable event order, fixed-order Decimal
     cash/P&L sums, deterministic open-order processing, seeded+logged RNG for any probabilistic fill.
 (3) HONEST COST — spread + slippage + size-dependent SQRT-impact (temporary->fill price, permanent->mark) +
     commission + latency, ALL coefficients configurable, applied before any strategy is believed.

Money is exact: Decimal for cash and realized P&L summed in fixed order; marks/unrealized may be f64. Strong
domain types across boundaries (no naked double price/qty). No UB, no narrowing, no owning raw pointers.
const/constexpr/noexcept/[[nodiscard]] where they hold. Expected failures -> Result<T>/Status, never throw.
Exhaustive enum-class switches (OrderType/EventType/Transform, no default); loops bounded; functions <= ~60
lines. // SAFETY: on every deviation. Zero allocation on the per-slice crank / panel / fill hot path.

TDD: failing GoogleTest first, watch it fail for the right reason, then implement. Cover happy path,
boundaries (empty/1-instrument/zero-volume/first-last-slice/flip), error paths (insufficient cash/NaN
signal/unfillable), invariant violations (EXPECT_DEATH on same-bar fill / backward clock). Build /W4 /WX
clean; clang-tidy/format clean; tests pass under ASan+UBSan. The loop (P2-7) and harness (P2-8) go green on
ScriptedSignalSource; VmSignalSource is blocked-on Phase 3.

A unit is done only when header + impl + tests + ledger row + build/test gate are all present. No TODO stubs,
no fake success paths, no untested skeletons. Prefer a smaller complete slice over a larger partial one.
```

---

## 10. Risks / watch-items

- **Phase-1 prerequisite** — Phase 2 needs the spine (EventBus/SimClock/DataHandler). It opens *after*
  Phase 1; if Phase-1 signatures differ from its plan, update to the *as-built* API and note in the ledger.
- **Phase-3 `Engine` contract drift** — `VmSignalSource` is written against the Phase-3 `Engine`/`Program`
  contract; if Phase 3 ships a different signature, update the adapter (one unit) and note the delta. The
  seam localizes the blast radius to P2-3.
- **Cost defaults are not truth** — Almgren `γ=0.314`/`η=0.142`, √-law `Y~O(1)`, Zipline `k=0.1`/`bps=5` are
  *starting points* from 2001–2003 fits (loop deep-dive §10). Ship them as **documented configurable
  defaults**; the *fit* is Phase 5. Never present a default-cost backtest as calibrated truth.
- **Zipline VolumeShare is quadratic, not √-law** — do not conflate the two slippage choice and the
  microstructure √-impact term; they are separate, composable models (Appendix A keeps them distinct).
- **Money type** — cash/realized P&L in `Decimal` (exact, deterministic sums); marks/unrealized in `f64`
  (market data is inherently float). Mixing requires care at the boundary — centralize conversions; document.
- **Determinism vs float marks** — equity uses `f64` marks; the determinism hash must fold a **canonical
  bit representation** with a defined NaN policy, and sums must be fixed-order, or the hash flickers.
- **Same-bar-fill "cheat"** — provide it only behind an explicit named flag (backtrader convention); default
  off. A reviewer must never find it on by default.
- **Scope creep into Phase 4/5** — no alpha store, no corr/turnover gates, no combiner, no cost calibration,
  no walk-forward here. The single-alpha loop + the cost *model* is the line.
- **LSP false positives** without `compile_commands.json` — verify against real `cmake --build`; ignore
  phantom noise (datetime + disruptor both saw this).

## 11. Definition of done (Phase 2)

- P2-0…P2-8 implemented; tests pass under ASan+UBSan — or blocked-on units (P2-1/2/4/5/6 on atx-core L6/L8/
  L9, `VmSignalSource` on Phase 3) staged red behind `atx_engine_pending` with the ROADMAP cross-module
  table current. The loop (P2-7) and integration (P2-8) are **green on `ScriptedSignalSource`**.
- `/W4 /permissive- /WX` clean; clang-tidy (project policy) + format clean.
- **Determinism** proven: identical feed → identical equity/P&L hash; mutation detected.
- **No-look-ahead** proven: order decided at `t` fills ≥ `t+1`; feed-truncation-invariance.
- **Honest cost** proven: fills worse than frictionless by the modeled cost; monotone in participation;
  impact-off recovers frictionless P&L. **No-survivorship**: delisted trades to its final bar.
- Portfolio accounting correct across open/increase/reduce/close/flip; exact-money `Decimal` ledger.
- Exec-sim + loop benches recorded (orders/s, slices/s) with host/build context (measured only).
- Zero steady-state allocation on the crank/panel/fill path (instrumented).
- Ledger closed; ROADMAP status + `Last reviewed` updated; `phase-2.md` written; worktree merged `--no-ff`
  (not pushed unless asked).

---

## Appendix A — Execution-sim cost model (formulas + defaults)

From [`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md)
§3–§4, §9.3. **All defaults are configurable knobs — the cost *fit* is Phase 5.** Application order:
**fill → slippage → √-impact → commission → latency/partial.**

| Stage | Model | Formula | Default | Source |
|---|---|---|---|---|
| eligibility | look-ahead firewall | fill iff `bar.end_time > order.queued_at` ∧ venue open | — | LEAN `EquityFillModel` |
| volume cap | participation limit | `fillable = min(\|open_qty\|, vlim·bar_vol − vol_for_bar)`; remainder → next slice | `vlim=0.025` (vol-share) / `0.10` (bps) | Zipline/LEAN |
| slippage | VolumeShare | `fill = ref·(1 ± k·share²)`, `share = fillable/bar_vol` | `k=0.1` | Zipline `VolumeShareSlippage` |
| slippage | FixedBps | `fill = ref·(1 ± bps/1e4)` | `bps=5` | Zipline `FixedBasisPointsSlippage` |
| slippage | spread floor | always cross `±spread/2` on aggressive orders | — | 📘 universal |
| impact (temp) | √-law → **fill price** | `temp = Y·σ·(part)^δ`, `part = fillable/ADV` | `Y~O(1)`, `δ=0.5` (cfg 0.45–0.65) | Tóth/Bouchaud; Almgren temp `η=0.142`, β=0.6 |
| impact (perm) | linear → **mark** | `perm = ½·γ·σ·(fillable/ADV)`; shift instrument mark by `perm·sign` | `γ=0.314` | Almgren 2005 |
| commission | per-share | `max(\|qty\|·per_share, min_fee)` capped `max_pct·notional` | `$0.005 / $1 / 0.5%` | LEAN IB |
| commission | per-dollar | `\|notional\|·per_dollar_bps` | `15 bps` (Zipline) | Zipline `PerDollar` |
| latency | defer | eligible at `queued_at + latency` | `0` | Nautilus `LatencyModel` |

⚠️ Almgren `γ/η` are a 2001–2003 Citigroup US-equity fit; √-law `Y` is only "order unity" in the sources —
**re-fit per universe/era (Phase 5)**. Zipline VolumeShare is *quadratic in participation*, **not** the
√-law — they are distinct composable models. Defaults drift from reality across all engines; every number
here is a knob.

---

## Appendix B — Open-source reference index

| System | What we took | Primary source |
|---|---|---|
| QuantConnect LEAN | per-slice crank order; stale-data fill firewall; avg-cost holdings; IB fees; `RollingWindow`/`History` | github.com/QuantConnect/Lean `Engine/AlgorithmManager.cs`, `Common/Securities/SecurityHolding.cs`, `Common/Orders/{Fills,Slippage,Fees}/*` |
| Zipline | `every_bar` settle-then-decide order; `SimulationBlotter`; `VolumeShareSlippage`/`FixedBasisPointsSlippage`; `PerShare`/`PerDollar` commission; `order_target_percent`; `data.history`; `schedule_function` | github.com/quantopian/zipline `zipline/gens/tradesimulation.py`, `zipline/finance/{slippage,commission,ledger}.py` |
| NautilusTrader | three-phase matching; cascading same-cycle orders; `FillModel` (prob fill/slippage); `LatencyModel` (in-flight queue) | nautilustrader.io/docs; docs.rs/nautilus-backtest |
| backtrader | default fill at next bar's open; explicit opt-in Cheat-on-Close/Open | backtrader.com/docu/broker; github.com/mementum/backtrader `brokers/bbroker.py` |
| Almgren-Thum-Hauptmann-Li 2005 | permanent (linear, γ=0.314) + temporary (β=3/5, η=0.142) impact split; temp→fill, perm→mark | cis.upenn.edu/~mkearns/finread/costestim.pdf |
| Tóth/Bouchaud √-law | `I(Q)=Y·σ·√(Q/V)`; exponent ~½ universal; level `Y` must be calibrated | arxiv.org/abs/1105.1694; bouchaud.substack.com |

*Full URL index + confidence tags: [`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md)
§11. Companion research: [`renaissance-worldquant-deep-dive.md`](../../research/renaissance-worldquant-deep-dive.md)
(§IV/§V), [`alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md) (the VM this loop is built around).*
