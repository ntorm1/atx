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

5. **P2-4 reconcile signature + return types (recorded 2026-06-04).**  The frozen plan's
   `reconcile(std::span<const f64> w, const Portfolio&, const Market&, Timestamp)` omitted the
   `Universe`, but reconcile needs it to map `w[i]` → `InstrumentId` (the weights are index-aligned to the
   universe, not self-describing). As-built signature:
   `reconcile(std::span<const f64> w, const Universe&, const Portfolio&, const Market&, Timestamp now)`.
   Also: the plan's aspirational `AlignedVec`/`FixedVector` return types are replaced by `std::vector<f64>`
   (weights) and `std::vector<OrderPayload>` (orders) — `core::container::fixed_vector` has a COMPILE-TIME
   capacity and cannot hold a runtime-sized universe. A new shared `Universe = std::span<const InstrumentId>`
   alias lives in `loop/types.hpp`. `industry_neutral` is kept as a forward-compat field but is inert in
   Phase-2 (no group map until Phase-4) and `ATX_ASSERT`ed false to prevent silent misuse.

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
| P2-4 | ✅ done | `6d040de` (+ `52cb233` winsorize) | `loop/weight_policy.hpp` (`WeightPolicy`/`Transform`) + `Universe` alias in `loop/types.hpp`. Full frozen pipeline `winsorize→transform(Rank\|ZScore)→dollar-neutralize(Σw=0)→gross-normalize(Σ\|w\|=gross_leverage)` = Alpha101 `scale`. **Winsorize is the FIRST step** (configurable `winsorize_limit`, **default 0.025** = standard 95% winsorization / clamp extreme 2.5% per tail; nearest-rank quantile band `[lim, 1−lim]` via `atx::core::stats::winsorize`; near-no-op for Rank, material outlier guard for ZScore; `0.0` disables). NaN/out-of-cross-section scores compacted out before winsorize/`rank`/`zscore` (no NaN handling in the primitive) then scattered back to 0 weight. Tie-break = atx-core `rank` (ties averaged, [0,1], smallest→0/largest→1, stable). `reconcile(w, Universe, Portfolio, Market, now)` = `order_target_percent`: `trade = trunc(w·equity/price) − current` (truncate toward zero), skip NaN/≤0 price, fixed index order, signed-qty `OrderPayload{Market}`. Degenerate Σ\|w\|==0 (all-equal/single/all-NaN) → flat (no div-by-zero). `industry_neutral` INERT (ATX_ASSERT false — no Phase-2 group map). 20 new tests (weight_policy_test.cpp). |
| P2-5 | ✅ done | `017ddea` | `portfolio/portfolio.hpp` (`Portfolio`/`Holding`): avg-cost open/increase/reduce/close/flip state machine; realized booked `closed·(p−avg)·sign(pos)`; full-close zeroes avg; cash `-= qty·p + fee` and realized/fees in **exact `Decimal`**, f64 sums (equity/gross/net) in fixed universe-index order; `unrealized` zero when flat. Plus a NEW shared `loop/market.hpp` (`Market`/`InstrumentStats`): dense fixed-universe price/stats book — `update_prices` (last-value table), `mark`/`bar_volume`/`stats`/`shift_mark` (P2-6 perm-impact + cost hooks defined now). Dense sorted id→index storage (no `std::hash<Symbol>`). `Holding::mark` uses a NaN "unpriced" sentinel (matches `Market::mark`); `market_value`/`unrealized` treat NaN as a 0 contribution so a position opened before the first `mark_to_market` never reports a phantom value. 32 new tests (13 `market_book_test.cpp`, 19 `portfolio_test.cpp`, incl. 7 death: zero/neg-price, zero-qty, out-of-universe fill, Market out-of-universe mark/shift, shift-before-price). Hardened in `fc426b5` — see sprint commits. |
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

- **Sorted id→index helper triplication (P2-2/P2-5) — future cleanup.** The `IdIndex` / `build_index` /
  `index_of` / `require_index` sorted-array universe-index idiom is now duplicated across `RollingPanel`
  (`loop/rolling_panel.hpp`), `Market`, and `Portfolio`. Extract a shared `loop/universe_index.hpp`
  (a small `UniverseIndex` over `std::span<const InstrumentId>` exposing `index_of`/`require_index`) and have
  the three consumers hold one. Deferred now to avoid editing three files concurrently with another agent on
  this branch; it is a pure refactor (no contract change).

- **`WeightPolicy` per-rebalance allocation (P2-4) — future optimization.** `to_target_weights` and
  `reconcile` each allocate once per call (`std::vector<f64>` weights + transform scratch / live-index
  compaction buffers; `std::vector<OrderPayload>` order list). This is acceptable at REBALANCE cadence (not
  per bar), but a caller-provided-scratch overload (pass reusable buffers so the methods are allocation-free)
  is a tracked optimization for when rebalance frequency rises. Pure addition (no contract change).

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
| `6d040de` | P2-4 | 19/19/0/0 (WeightPolicy: RankDollarNeutral-SumIsZero, RankGrossNormalized/DefaultLeverage, RankMonotonic, UnshuffledMatchesShuffled, NaNEntry-zero+excluded, ZScore, AllEqual→zero, SingleInstrument→flat, AllNaN→zero, NotDollarNeutral, OutputLength; Reconcile: FromFlat, AlreadyOnTarget→none, FlipLong→Short full delta, ZeroWeight→close, NaNPrice→skip, ZeroEquity→flat, TruncatesTowardZero) |
| `52cb233` | P2-4 winsorize | 20/20/0/0 (+WeightPolicy.ZScoreWinsorize_ClampsOutlierBeforeStandardizing; NaNEntry strengthened to 2 live names so the NaN is the only exact zero). Adds the frozen-spec `winsorize` first step (`winsorize_limit` default 0.025) — corrects the prior silent omission. |
| `017ddea` | P2-5 | 29/29/0/0 (Market×10 + MarketDeathTest×2; Portfolio×14 + PortfolioDeathTest×3 — open/increase-weighted-avg/partial-reduce/full-close/reduce-short/flip-long↔short/reduce-to-zero/fee/mark/unrealized/equity/gross-net-leverage/dollar-neutral/leverage-guard/cash-sequence; death: zero-price, neg-price, out-of-universe, market out-of-universe×2) |
| `fc426b5` | P2-5 hardening | 32/32/0/0 (+Market shift-before-price death; +Portfolio equity-pre-mark-unpriced, +zero-qty death). `Holding::mark` NaN unpriced sentinel; `fee>=0` precondition; `std::isnan` guard; doc/comment honesty (two Decimal→f64 crossings; std::vector sized-once storage) |
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
