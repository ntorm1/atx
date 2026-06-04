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
| P2-0 | ✅ done | `fill-sha` | `loop/types.hpp` (InstrumentId alias); Phase-2 fwds in `fwd.hpp`; scaffold_test Phase2TypesAliasResolves passes. No blocked CMake targets — only VmSignalSource (P2-3) blocked-on Phase-3. |
| P2-1 | ⏳ pending | `—` | `SignalPayload`/`OrderPayload`/`FillPayload` (complete Phase-1 `Event`); `Side`/`OrderType`; `Decimal` money. *blocked-on Phase-1, L8.* |
| P2-2 | ⏳ pending | `—` | `RollingPanel<Cap>`: PIT append-after-close, bounded `max_lookback`, column-major + membership mask. *blocked-on L9, L3.* |
| P2-3 | ⏳ pending | `—` | `ISignalSource`; `ScriptedSignalSource` (green); `VmSignalSource` over Phase-3 `Engine`. *VM adapter blocked-on Phase 3.* |
| P2-4 | ⏳ pending | `—` | `WeightPolicy`: rank→Σw=0→Σ\|w\|=1 + `order_target_percent` reconcile. *blocked-on L6.* |
| P2-5 | ⏳ pending | `—` | `Portfolio`/`Holding`: open/increase/reduce/close/flip; realized/unrealized; exposure/leverage; `Decimal` cash. *blocked-on L8, L1.* |
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

_(Lift to ROADMAP future-work backlog at close.)_ None recorded yet. Expected: `VmSignalSource` green-gate
when Phase 3 merges; short borrow-cost accrual; limit-order-book realism; cost calibration (Phase 5);
intraday fills; same-bar-close "cheat" flag (off by default).

---

## Phase 2 sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| `fill-sha` | marker (P2-0) | 2/2/0/0 (EngineScaffold.TestTargetLinksAndRuns, Scaffold.Phase2TypesAliasResolves) |
| `—`    | P2-1 | — |
| `—`    | P2-2 | — |
| `—`    | P2-3 | — |
| `—`    | P2-4 | — |
| `—`    | P2-5 | — |
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
