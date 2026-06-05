# Phase 2 — Implementation Progress

**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `357b7c9`
**Started:** `2026-06-03`
**Source plan:** [`phase-2-backtest-loop-implementation-plan.md`](phase-2-backtest-loop-implementation-plan.md)
**Prior progress:** Phase 1 (event spine) — see [`phase-1-progress.md`](phase-1-progress.md). **Phase 2
requires the Phase-1 spine.**

> **Ledger state:** marker-stage skeleton. Rows are `⏳ pending` until each unit lands. Update each row in
> the same commit as its implementation (or follow up with `docs(p2-N): record SHA`). Never fudge test
> counts. Scope changes go in *Plan adjustments* below, **not** in the frozen plan file.

---

## Plan adjustments vs. the source plan

Recorded at P2-0 kickoff (2026-06-03). Three as-built deltas from the frozen plan:

1. **`InstrumentId` → `atx::core::domain::Symbol`.**  The plan referred to a hypothetical `InstrumentId`
   type; no such type was added to atx-core. The as-built decision is a single alias in
   `loop/types.hpp`: `using InstrumentId = atx::core::domain::Symbol;`. All Phase-2 units use
   `InstrumentId` via this alias — no engine-level id type is introduced.

2. **`Side` reuse from atx-core.**  `atx::core::domain::Side {Buy,Sell}` already exists in
   `atx/core/domain/domain.hpp`. Phase-2 does NOT redefine it; P2-1..P2-6 include
   `atx/core/domain/domain.hpp` directly where `Side` is needed.

3. **atx-core layers all built green — P2-1..P2-6 are NOT blocked.**  The plan's `atx_engine_pending`
   label was reserved for units waiting on L3/L6/L8/L9 atx-core layers. As of this sprint open,
   all required layers (decimal, random, hash, ring_buffer, hash_map, fixed_vector, cross_section,
   domain, series/column+frame, math) are merged and passing. Consequently **only `VmSignalSource`
   (P2-3 VM adapter) is genuinely blocked — on Phase-3 engine completion**. P2-1..P2-6 and P2-7/P2-8
   are unblocked. No `atx_engine_pending` CMake staging targets are needed at this time; the posture
   is noted here rather than encoded in CMake.

4. **P2-4 ↔ P2-5 reorder + new shared `Market` type (recorded 2026-06-04).**  P2-5 (`Portfolio`) was
   implemented BEFORE P2-4 (`WeightPolicy`): P2-5's atx-core deps (L8 `domain`, L1 `decimal`, L3) are all
   green, whereas the reconcile that P2-4 shares with P2-5 reads more naturally once the position book
   exists. The two are independent units, so the swap is order-only (no contract change). Separately, the
   frozen plan references a `Market` type in `Portfolio::mark_to_market` that did not exist; P2-5 introduces
   it as `atx/engine/loop/market.hpp` (`Market` + `InstrumentStats`) — the loop's current price/stats book.
   It is the SHARED downstream contract: P2-4 reconcile and P2-6 exec read `mark`/`bar_volume`/`stats` and
   P2-6 calls `shift_mark` for permanent impact, so those members are defined now to avoid a later reshape.
   (NOTE: a pre-existing `atx/engine/data/market.hpp` holds the unrelated Phase-1 `data::MarketPayload`
   event type; the P2-5 test file is named `market_book_test.cpp` to avoid colliding with `market_test.cpp`.)

Realistic scope for this sprint:

1. **P2-0** — Module scaffold + CMake + ledger (marker). Open ledger, freeze scope.
2. **P2-1** — Signal/Order/Fill payloads (complete the Phase-1 `Event` taxonomy) + trade domain types. *blocked-on Phase-1 `Event`, L8 `domain`.*
3. **P2-2** — `RollingPanel` (bar→panel bridge; PIT, bounded `max_lookback`, column-major). *blocked-on L9 `frame`, L3 `ring_buffer`.*
4. **P2-3** — `ISignalSource` seam: `ScriptedSignalSource` (not blocked) + `VmSignalSource` (*blocked-on Phase 3*).
5. **P2-4** — `WeightPolicy`: rank → dollar-neutral (Σw=0) → gross-norm (Σ|w|=1) + reconcile. *blocked-on L6 `cross_section`.*
6. **P2-5** — `Portfolio`: avg-cost basis, realized/unrealized, cash ledger, exposure/leverage. *blocked-on L8, L1 `decimal`, L3.*
7. **P2-6** — `ExecutionSimulator`: fill→slippage→√-impact→commission→latency→partial; look-ahead firewall. *blocked-on L8, `math`, L1 `random`.*
8. **P2-7** — `BacktestLoop` driver (settle→mark→eval→weights→queue→sample). *green on `ScriptedSignalSource`.*
9. **P2-8** — Integration: determinism + cost-honesty + no-look-ahead + survivorship; bench; sprint close.

Defer (out of Phase 2 scope — see ROADMAP):

- Alpha **store** + correlation/turnover **gates** + mega-alpha **combiner** + **Barra risk model** → **Phase 4**.
- Cost **calibration** (fit `Y/δ/γ/η`), capacity, walk-forward + deflated-Sharpe → **Phase 5**.
- Short borrow-cost accrual, limit-order-book realism, intraday fills → future-work backlog (hooks left).
- Live broker / market connectivity → later module (the loop is built sim==live to avoid skew).

---

## Per-unit ledger

| Unit | Status | Commit | Notes |
|------|--------|--------|-------|
| P2-0 | ✅ done | `5de987d` | `loop/types.hpp` (InstrumentId alias); Phase-2 fwds in `fwd.hpp`; scaffold_test Phase2TypesAliasResolves passes. No blocked CMake targets — only VmSignalSource (P2-3) blocked-on Phase-3. |
| P2-1 | ✅ done | `dca2f45` | `exec/payloads.hpp`: `SignalPayload`/`OrderPayload`/`FillPayload`; `OrderType`; `make_signal/order/fill_event`; signed-qty canonical direction; 31 new tests (payload_test.cpp). Side reused from atx-core. |
| P2-2 | ✅ done | `8879bec` | `loop/panel_types.hpp` (`SliceRow`/`MarketSlice`/`PanelView`/`PanelField`) + `loop/rolling_panel.hpp` (`RollingPanel<Cap>`): PIT append-after-close (structural temporal gate — no API to write an unsealed bar), bounded `max_lookback` ring (pow2 `Cap`, single L2-aligned allocation, zero alloc on append/view), column-major-per-field f64 + per-row membership bitmask, NaN for absent cells, sorted id→col index. 17 new tests (rolling_panel_test.cpp). |
| P2-3 | ✅ done | `db0198d` | `loop/signal_source.hpp`: `ISignalSource` seam (abstract base — virtual dtor, copy/move deleted) + `SignalView` (non-owning `std::span<const f64>`, one score per universe instrument, NaN = no opinion, borrow-valid until next `evaluate()`); `ScriptedSignalSource` GREEN (owned flat schedule buffer, deterministic cursor replay, panel-independent by design, zero per-call alloc, `Err(OutOfRange)` on exhaustion). `evaluate(PanelView)→Result<SignalView>` pure contract; `max_lookback()` returns configured N. `VmSignalSource` adapter is a compile-guarded skeleton behind `ATX_ENGINE_HAS_ALPHA_VM` (defined nowhere → never compiles → green build intact) recording the Phase-3 contract. 11 new tests (signal_source_test.cpp). *VmSignalSource green-gate blocked-on Phase 3 — see Deferred residuals.* |
| P2-4 | ⏳ pending | `—` | `WeightPolicy`: rank→Σw=0→Σ\|w\|=1 + `order_target_percent` reconcile. *blocked-on L6.* |
| P2-5 | ✅ done | `017ddea` | `portfolio/portfolio.hpp` (`Portfolio`/`Holding`): avg-cost open/increase/reduce/close/flip state machine; realized booked `closed·(p−avg)·sign(pos)`; full-close zeroes avg; cash `-= qty·p + fee` and realized/fees in **exact `Decimal`**, f64 sums (equity/gross/net) in fixed universe-index order; `unrealized` zero when flat. Plus a NEW shared `loop/market.hpp` (`Market`/`InstrumentStats`): dense fixed-universe price/stats book — `update_prices` (last-value table), `mark`/`bar_volume`/`stats`/`shift_mark` (P2-6 perm-impact + cost hooks defined now). Dense sorted id→index storage (no `std::hash<Symbol>`). 29 new tests (12 `market_book_test.cpp`, 17 `portfolio_test.cpp`, incl. 5 death). |
| P2-6 | ⏳ pending | `—` | `ExecutionSimulator`: firewall + volume cap + slippage + temp/perm √-impact + commission + latency; partial fills. *blocked-on L8, math.* |
| P2-7 | ⏳ pending | `—` | `BacktestLoop`: settle-prior→mark→panel→eval→weights→queue→sample; decide-`t`/fill-`t+1`. *green on Scripted.* |
| P2-8 | ⏳ pending | `—` | Determinism (P&L hash) + cost-honesty (monotone in size) + no-look-ahead (truncation-invariant) + survivorship; bench. Close. |

### P2-6/P2-8 measured throughput

_(Fill when benches run.)_ Record with host/build/feed-shape context. **Measured** numbers only — do **not**
present default-cost results as calibrated (the cost *fit* is Phase 5).

| Config | orders/s or slices/s | ns/order | Host / build / feed |
|--------|----------------------|----------|---------------------|
| exec-sim settle | — | — | — |
| end-to-end loop | — | — | — |

### Cost-model defaults in force

_(Record the config used; all are knobs — Appendix A of the plan.)_ √-impact `δ`, `Y`; Almgren `γ`,`η`;
slippage `k`/`bps`; `volume_limit`; commission `per_share`/`min`/`max_pct`. Fit deferred to Phase 5.

| Knob | Default | Fitted? |
|------|---------|---------|
| `δ` (sqrt-impact exponent) | 0.5 | ⏳ Phase 5 |
| `γ` / `η` (Almgren) | 0.314 / 0.142 | ⏳ Phase 5 |
| slippage | VolumeShare `k=0.1` / FixedBps `5` | ⏳ Phase 5 |
| commission | `$0.005`/sh, `$1` min, `0.5%` max | ⏳ Phase 5 |

### Deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_

- **`VmSignalSource` green-gate (P2-3) — blocked-on Phase 3.** The production `ISignalSource` adapter over
  the Phase-3 `alpha::Program`/`alpha::Engine` is staged red: its declaration + intended implementation live
  behind `#if defined(ATX_ENGINE_HAS_ALPHA_VM)` in `loop/signal_source.hpp` (the macro is defined nowhere, so
  the block never compiles and the green build is unaffected). When Phase 3 lands, define the macro, add the
  `#include` for the alpha headers, and write the red→green test (wrap a trivial 1-op program; match the
  Phase-3 `Engine` output on a fixture; boundaries: empty signal all-NaN, single instrument). The seam
  (`ISignalSource` + `SignalView`) is already the frozen downstream contract P2-4/P2-7 build against.

Other expected residuals: short borrow-cost accrual; limit-order-book realism; cost calibration (Phase 5);
intraday fills; same-bar-close "cheat" flag (off by default).

---

## Phase 2 sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| `5de987d` | marker (P2-0) | 2/2/0/0 (EngineScaffold.TestTargetLinksAndRuns, Scaffold.Phase2TypesAliasResolves) |
| `dca2f45` | P2-1 | 31/31/0/0 (PayloadTypes×6, SignalPayloadRoundTrip×2, OrderPayloadRoundTrip×2, FillPayloadRoundTrip×1, SignedQtyConvention×3, MakeSignalEvent×4, MakeOrderEvent×6, MakeFillEvent×3, DecimalPriceExactness×2, OrderTypeToString×2) |
| `8879bec` | P2-2 | 17/17/0/0 (RollingPanel append/view/PIT/eviction/wrap/cross-section/NaN/membership/boundaries×N + RollingPanelDeathTest×2 out-of-bounds) |
| `db0198d` | P2-3 | 11/11/0/0 (ScriptedSignalSource: ReplaySameSchedule deterministic, EachResult length==universe, NaN passthrough, MaxLookback==N, PastEndOfSchedule→Err, ConsecutiveCalls cursor-advance, AllNaN, EmptyUniverse, SingleInstrument, EmptySchedule→Err, ThroughBaseInterface). VmSignalSource compile-guarded (no test today — Deferred). |
| `—`    | P2-4 | — |
| `017ddea` | P2-5 | 29/29/0/0 (Market×10 + MarketDeathTest×2; Portfolio×14 + PortfolioDeathTest×3 — open/increase-weighted-avg/partial-reduce/full-close/reduce-short/flip-long↔short/reduce-to-zero/fee/mark/unrealized/equity/gross-net-leverage/dollar-neutral/leverage-guard/cash-sequence; death: zero-price, neg-price, out-of-universe, market out-of-universe×2) |
| `—`    | P2-6 | — |
| `—`    | P2-7 | — |
| `—`    | P2-8 + close | — |

**Phase 2 adds `<N>` new tests (total engine footprint: `<K>`/0/0 across `<J>` binaries).** _(Fill at close.)_

---

## What Phase 2 proves / Next sprint priorities

_(Written at close — the baton handoff.)_ Expected statement: *Phase 2 proves the engine runs a complete,
cost-honest, deterministic backtest of one alpha — bars accrete into a point-in-time rolling panel, a
strategy (a compiled alpha program reached through the `ISignalSource` seam) produces a dollar-neutral target
portfolio, an execution simulator charges spread + slippage + size-dependent √-impact + commission + latency,
and the portfolio books exact-money P&L. Identical feed replays byte-identically, a decision on bar `t` never
fills before `t+1`, and a larger order provably pays more. The loop is `ScriptedSignalSource`-green today;
when Phase 3 merges, `VmSignalSource` drops in as the strategy core. Phase 4 plugs the mega-alpha combiner in
as just another `ISignalSource` and adds correlation/turnover gates + a Barra risk model.*
