# Module p0 — atx-engine: Weak-Signal Backtesting Engine (`p0`)

**Last reviewed:** 2026-06-05
**Started:** Phase 1 (event spine) **CLOSED 2026-06-03** — 7 units, 69 tests green, in-place on `feat/atx-core-stdlib`. Phase 2 (backtest loop) **CLOSED 2026-06-05** — 9 units, 224 engine tests green (`ScriptedSignalSource`-green; `VmSignalSource` blocked-on Phase 3). Phase 3 (alpha-DSL VM) opens next.
**Source:** [`engine-architecture-audit.md`](engine-architecture-audit.md) ← distills [`../../research/renaissance-worldquant-deep-dive.md`](../../research/renaissance-worldquant-deep-dive.md). Governed by [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md).
**Goal:** Build a deterministic, point-in-time, cost-honest backtesting engine for **weak-signal alpha
aggregation** in US equities — many faint, mostly-uncorrelated signals combined into one unified
target portfolio — on top of the `atx-core` stdlib.

---

## Companion docs index

| Doc | Type | Covers |
|---|---|---|
| [`engine-architecture-audit.md`](engine-architecture-audit.md) | source audit | Research → engine positioning, two-layer architecture, invariants, atx-core dependency posture. **Read first.** |
| [`phase-1-event-spine-implementation-plan.md`](phase-1-event-spine-implementation-plan.md) | implementation plan | Per-unit P1-0..P1-6 plan for the first sprint. |
| [`phase-1-progress.md`](phase-1-progress.md) | sprint ledger | Phase 1 marker-stage ledger; fills in as units land. |
| [`phase-2-backtest-loop-implementation-plan.md`](phase-2-backtest-loop-implementation-plan.md) | implementation plan | Per-unit P2-0..P2-8 plan: the backtest loop + portfolio + execution sim, built **around the alpha-DSL VM** as the strategy core. |
| [`phase-2-progress.md`](phase-2-progress.md) | sprint ledger | Phase 2 marker-stage ledger; fills in as units land. |
| [`phase-3-alpha-expression-dsl-implementation-plan.md`](phase-3-alpha-expression-dsl-implementation-plan.md) | implementation plan | Per-unit P3-0..P3-9 plan: the alpha-expression DSL + vectorized VM (the alpha-research layer). |
| [`phase-3-progress.md`](phase-3-progress.md) | sprint ledger | Phase 3 marker-stage ledger; fills in as units land. |
| [`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md) | research | Loop/portfolio/exec-sim grounding: LEAN, Zipline, Nautilus, backtrader; Almgren/√-impact. Cited by the Phase-2 plan. |
| [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md) | research | DSL/VM grounding: Alpha101, Qlib, Zipline, DuckDB, Pratt/CSE. Cited by the Phase-3 plan. |

**Pending (created at later sprint kickoff/close):**
- `phase-1.md` — user reference (written at Phase 1 close).
- `phase-2-backtest-loop-implementation-plan.md` (Phase 2 kickoff) and `phase-3.md` (Phase 3 close), etc.

**Sprint/module discipline:** as defined in [`../docs/sprint.md`](../docs/sprint.md),
[`../docs/module.md`](../docs/module.md), [`../docs/implementation-quality.md`](../docs/implementation-quality.md).

---

## Sibling / upstream modules

- **Upstream:** `atx-core` stdlib — design spec at [`../../../docs/superpowers/specs/2026-06-01-atx-core-quant-stdlib-design.md`](../../../docs/superpowers/specs/2026-06-01-atx-core-quant-stdlib-design.md). The engine is a pure **consumer** of atx-core L0–L9; it adds no general-purpose primitives. Dependency edges live here (in the depending module), per [`../docs/module.md`](../docs/module.md) cross-module rules.

---

## Strategic positioning

`atx-engine` is **not** an alpha library and **not** a trading venue. Its identity is a *trustworthy
simulator*: the layer that lets you believe a backtest. The differentiators are correctness invariants
(determinism, point-in-time, no survivorship, honest cost), not signal cleverness — signal generation
is downstream (Phases 3–4) and deliberately starts simple (formulaic + linear/shrinkage), per the
verified research that **weak-signal breadth beats single-signal strength**.

| Dimension | RenTech (Medallion) | WorldQuant | **atx-engine target** |
|---|---|---|---|
| Core identity | secret unified ML model | alpha-mining sandbox + pool | deterministic bias-free backtester |
| Signal style | kernel/ML ensemble | formulaic price-volume alphas | formulaic + linear/shrinkage combo |
| Combination | one ~500k-LOC system | mega-alpha (10⁵–10⁹) | one unified optimizer over a gated pool |
| Horizon | intraday→multi-day | ~0.6–6.4 days | daily-bar first; intraday later |
| Cost honesty | proprietary | gross-of-cost | √-impact+spread+latency from Phase 2 |
| Infra | petabyte warehouse | WebSim | single-box two-layer spine + research |

When scope creep argues with you, this table and the anti-roadmap (below) are where it goes to die.

---

## Phase 0 — Foundation (pre-roadmap baseline)

**Solid (exists today):**
- `atx-core` **L0** — `types`, `error` (`Result`/`Status`), `macro`/assert, `log` (spdlog), `bit`,
  `util`, `platform`, `math`, `safe_math`. Built and tested.
- `atx-engine` skeleton — `include/atx/engine/engine.hpp`, `src/engine.cpp`, one passing test; CMake
  links `atx-core`.
- Sprint/module/quality style guides under [`../docs/`](../docs/).

**Critical gaps (map explicitly to phases):**
- No event bus, event taxonomy, or data model → **Phase 1**.
- No backtest loop, portfolio, execution/cost simulation → **Phase 2**.
- No alpha-research layer (columnar eval, formulaic vocab) → **Phase 3**.
- No signal combination or risk model → **Phase 4**.
- No cost calibration, capacity, or validation/bias-audit harness → **Phase 5**.
- **Cross-module gap:** atx-core L1–L9 mostly not yet built (L1 `decimal` in progress). **L4
  `concurrent::disruptor` is now built** (SP/MP, 1..N gated consumers, lock-free, cache-padded,
  327/327 suite green), unblocking P1-3's core dependency. Phase 1 remains blocked-on atx-core **L8**
  (time, domain) and **L3** (containers). See cross-module deps below.

---

## Phase 1 — Event spine + data model  ✅ CLOSED (2026-06-03)

**Theme:** A deterministic, point-in-time event spine — Disruptor-backed bus, event taxonomy, market
records, sim clock + look-ahead gate, and a DataHandler that replays bars chronologically with
delisted symbols included. The trustworthy substrate every later phase plugs into.

**Sprint merge:** built **in-place on `feat/atx-core-stdlib`** (no worktree — see ledger *Plan
adjustments*; the human was active in this checkout). No `--no-ff` merge step. **69 engine tests, 0
fail**, all under `/W4 /permissive- /WX`, CLI clang-tidy + clang-format clean. Each unit: spec-compliance
review + code-quality review APPROVED.
**Plan:** [`phase-1-event-spine-implementation-plan.md`](phase-1-event-spine-implementation-plan.md) · **Ledger:** [`phase-1-progress.md`](phase-1-progress.md) · **User ref:** [`phase-1.md`](phase-1.md).

| # | Item | Effort | Status | Ledger unit | Commit |
|---|---|---|---|---|---|
| 1.0 | Module scaffold + CMake test glob | S | ✅ done | P1-0 | `cf6252d` |
| 1.1 | Event taxonomy (`EventType`, `Event` POD) | M | ✅ done | P1-1 | `554cceb`+`14b8c2e` (13 tests) |
| 1.2 | Market records (`MarketPayload` over Bar/Tick + PIT) | S | ✅ done | P1-2 | `1e00593`+`f83c273` (15 tests) |
| 1.3 | Event bus (wrap `concurrent::disruptor`) | M | ✅ done | P1-3 | `8e6602b`+`0c01b8a` (10 tests, zero-alloc, bench) |
| 1.4 | Sim clock + point-in-time visibility gate | M | ✅ done | P1-4 | `f62f465`+`5e3fd71` (13 tests) |
| 1.5 | DataHandler (`IDataHandler` + `InMemoryBarFeed`) | M | ✅ done | P1-5 | `90470da` (11 tests; survivorship + no-look-ahead) |
| 1.6 | Determinism replay harness + sprint close | S | ✅ done | P1-6 | `e4bac79` (6 tests; mutation-detecting hash) |

**Exit criteria — MET.** An in-memory bar feed replays through the bus to a consumer producing a
**byte-identical event-hash across two runs** (determinism proven; three input mutations each change the
hash — non-vacuous), a future-dated record is proven invisible (no look-ahead: clock advances to the
frontier *before* publish), and a delisted symbol is present (no survivorship). Bus publish/drain bench
recorded (Debug). TSan-on-Linux is a tracked residual (clang-cl/Windows has no usable TSan; threaded
test is race-clean by construction).

---

## Phases 2–5 — Phases 2 & 3 scoped (plans linked); 4–5 sketched (detailed at each kickoff)

> Status rows stay `⏳ pending` and are **not** promoted to commitments until their sprint actually opens
> (module.md: don't document hopes as commitments). Phases 2 & 3 now have **frozen implementation plans**
> (the design is scoped); the *sprint* still opens at kickoff. Phases 4–5 remain sketches.

### Phase 2 — Backtest loop + portfolio + execution sim (built around the alpha-DSL VM)  ✅ **CLOSED 2026-06-05** ([plan](phase-2-backtest-loop-implementation-plan.md) · [ledger](phase-2-progress.md) · [reference](phase-2.md))
The deterministic per-slice crank that closes the event-driven loop — **with the Phase-3 alpha-DSL VM as the
strategy core**. Bars off the Phase-1 spine accrete into a point-in-time **`RollingPanel`**; on the rebalance
cadence the **VM evaluates a compiled alpha program** over it (via a thin **`ISignalSource`** seam — the VM
is the production impl; a `ScriptedSignalSource` test-double keeps the loop green before Phase 3 lands); a
**rank → dollar-neutral (Σw=0, Σ|w|=1)** policy yields target weights; an **ExecutionSimulator** (fill +
slippage + **√-impact** [temp→fill, perm→mark] + commission + latency + partial fills) produces fills; a
**`Portfolio`** (avg-cost basis, realized/unrealized, exposure/leverage) books P&L. **First runnable,
cost-honest backtest on one alpha.** Invariants are tests: no-look-ahead (decide-`t`/fill-`t+1`), determinism
(byte-identical replay), honest cost. Consumes Phase-1 spine + atx-core L8 (domain), L9 (frame), L6
(cross_section), L3/L2/L1; cross-phase: only `VmSignalSource` is blocked-on Phase 3.
*Grounded in [`../../research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md).*

> **The strategy engine is the VM.** There is no arbitrary-C++-callback strategy — a strategy *is* a compiled
> alpha program reached through the `ISignalSource` seam, so the Phase-4 mega-alpha combiner later plugs in as
> just another signal source with zero loop changes.

| # | Item | Effort | Status | Ledger unit | Notes |
|---|---|---|---|---|---|
| 2.0 | Module scaffold + CMake + ledger (marker) | S | ✅ done | P2-0 | `5de987d` (2 tests) |
| 2.1 | Signal/Order/Fill payloads + trade domain types | S | ✅ done | P2-1 | `dca2f45` (31 tests) |
| 2.2 | `RollingPanel` (bar→panel bridge; PIT, bounded) | M | ✅ done | P2-2 | `8879bec` (17 tests) |
| 2.3 | `ISignalSource` seam (Scripted + Vm) | S | ✅ done | P2-3 | `db0198d` (11 tests; `VmSignalSource` compile-guarded — blocked-on Phase 3) |
| 2.4 | Signal→weight policy (winsorize → rank → dollar-neutral → reconcile) | M | ✅ done | P2-4 | `6d040de`+`52cb233` (20 tests) |
| 2.5 | `Portfolio` accounting + `Market` price/stats book | M | ✅ done | P2-5 | `017ddea`+`fc426b5` (32 tests) |
| 2.6 | `ExecutionSimulator` (fill+slippage+√-impact+commission+latency+partial) | L | ✅ done | P2-6 | `5ce23c7`+`0c00c1b` (24 tests; sell-remainder fix surfaced by P2-7) |
| 2.7 | `BacktestLoop` driver (green on `ScriptedSignalSource`) | M | ✅ done | P2-7 | `1d30250` (8 tests; hand-computed E2E) |
| 2.8 | Integration: determinism · cost-honesty · no-look-ahead · survivorship + bench + close | M | ✅ done | P2-8 | `0f8fc93` (10 tests) + end-to-end bench |
| — | `VmSignalSource` green-gate | S | ⏳ blocked-on Phase 3 | P2-3 | the one cross-phase edge; seam + test contract already frozen |

**Exit criteria — MET (on `ScriptedSignalSource`).** The first runnable, cost-honest, deterministic backtest
of one alpha. **Determinism**: identical feed → byte-identical fill+equity digest across two runs; a
perturbed price and an added late bar each flip it (non-vacuous). **No look-ahead**: a decision on bar `t`
never fills before `t+1`; truncating the feed after `t` leaves every `≤t` fill/mark/equity byte-identical.
**Honest cost**: a buy fills above / a sell below the frictionless mark by the modeled spread + slippage +
√-impact + commission; a larger book pays more (monotone); costs-off recovers the frictionless equity.
**No survivorship**: a delisted symbol trades to its final bar and is excluded thereafter. Portfolio
accounting is correct across open/increase/reduce/close/flip on an exact-money `Decimal` ledger. End-to-end
+ exec-sim benches recorded (Debug upper bound). The one cross-phase edge — `VmSignalSource` — stays
blocked-on Phase 3; the `ScriptedSignalSource` keeps the loop + integration green without it.

### Phase 3 — Alpha Expression DSL + vectorized evaluation engine  ⬅ **scoped** ([plan](phase-3-alpha-expression-dsl-implementation-plan.md))
A **quant idea as a string** (`rank(ts_corr(close,volume,10))`) → lexer + **Pratt parser** → typed
**hash-consed compute DAG** (free CSE) → **vectorized bytecode** → a **columnar VM** that runs each opcode
over a whole column to emit a **cross-sectional signal vector** per alpha per date. The formulaic-alpha
**operator vocabulary is the VM's instruction set** (`rank`/`zscore`/`scale`/`indneutralize`/`ts_*`/`delta`/
`decay_linear`/`correlation`, …). Built for **mass evaluation** (10⁵–10⁹ alphas sharing subexpressions);
**subexpression-DAG dedup + caching** is the dominant lever (research §II/§VII: orthogonality > count;
DSL deep-dive §3.1/§5.3). Stops at *source → signal vector* (WebSim model); typed handoff to the Phase-2
Strategy. Consumes atx-core L5 (simd), L6 (cross_section/rolling/online_stats), L9 (column/frame), L3/L2/L1.
*Grounded in [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md).*

> **Replaces** the former Phase-3 sketch ("alpha research layer"). The **alpha store** and
> **correlation/turnover orthogonality gates** that lived here move to **Phase 4** (they screen a *pool* of
> evaluated signals — combiner territory). Phase 3 is now the pure expression→signal compute core.

### Phase 4 — Alpha pool gates + signal combination + risk
**Alpha store** + **correlation/turnover orthogonality gates** (WebSim-style fan-out screening; moved from
the old Phase 3) feeding the mega-alpha **combiner** (rank-average → Ledoit-Wolf shrinkage / factor-imposed
covariance → regularized regression), a **Barra-style factor risk model** `r=Xf+u`, `V=XFXᵀ+D` (research §VI),
neutralization, and a turnover-penalized portfolio optimizer. Consumes the Phase-3 `SignalSet` + atx-core L7
(linalg/regression).

### Phase 5 — Cost calibration + capacity + validation
Fit √-impact exponent `δ` (0.45–0.65), split temporary vs permanent (Almgren-Chriss), capacity
analysis, **walk-forward + deflated-Sharpe** harness, and explicit **bias audits** (survivorship,
look-ahead, data-snooping). Resolves research Appendix-C open questions. *Proposed; scope frozen at its
own kickoff.*

---

## Cross-module dependencies (Pattern B — blocked-on upstream)

The engine depends on `atx-core` layers not yet shipped. Per module.md, the edge lives here.

| Engine unit | Blocked-on (atx-core) | Today |
|---|---|---|
| P1-1, P1-2, P1-4, P1-5 | **L8** `time`, `domain` | ⏳ not built |
| P1-3 | **L4** `concurrent::disruptor` | ✅ **built** (327/327 green) |
| P1-5 | **L3** `fixed_vector`, `hash_map` | ⏳ not built |
| Phase 2 (loop/portfolio/exec-sim) | Phase-1 spine; L8 `domain`, L9 `frame`, L6 `cross_section`, L3 `ring_buffer`/`hash_map`, L2 `aligned`, L1 `decimal`/`random`/`hash` | ✅ **built** (224 engine tests green; **green on `ScriptedSignalSource`** without Phase 3) |
| Phase 2 `VmSignalSource` only | **Phase 3** `Engine`/`Program` (cross-phase) | ⏳ blocked-on Phase 3 — the one cross-phase edge (seam-localized) |
| Phase 3 (DSL front-end P3-1..P3-4) | L0, L1 `hash`, L3 `hash_map`/`fixed_vector`, L2 `arena` | ⏳ not built (front-end **not blocked** on L5/L6/L9 — can go green early) |
| Phase 3 (DSL VM P3-5..P3-8) | L9 `column`/`frame`, L6 `cross_section`/`rolling`/`online_stats`, L5 `simd`, L8 `time`, L2 `object_pool` | ⏳ not built |
| Phase 4 | Phase-3 `SignalSet` + L7 `linalg`/`regression` | ⏳ not built |

**Sequencing consequence:** Phase 1 can **open and scaffold immediately** — write headers, interfaces,
and failing TDD tests against the atx-core L4/L8 *contracts* in the stdlib design spec (§6). Per-unit
**green-gate** (tests passing) lands as the depended-on atx-core layer merges. A residual note must be
added to the atx-core build's tracking so the upstream agent sees that engine Phase 1 waits on L4+L8.

---

## Future-work backlog

Deferred units lift here at sprint close with their carrying ledger unit and reason (module.md §8).

**From Phase 2 (carried in [`phase-2-progress.md`](phase-2-progress.md) → Deferred residuals):**
- **`VmSignalSource` green-gate** (P2-3) — blocked-on Phase 3; define `ATX_ENGINE_HAS_ALPHA_VM`, wire the
  `alpha::Program`/`alpha::Engine` adapter, write its red→green test. The seam contract is already frozen.
- **Sorted id→index helper triplication** (P2-2/P2-5) — extract a shared `loop/universe_index.hpp` used by
  `RollingPanel`, `Market`, `Portfolio` (pure refactor, no contract change).
- **`WeightPolicy` per-rebalance allocation** (P2-4) + **`BacktestLoop` result accumulators** (P2-7) —
  caller-provided scratch / streaming sink for allocation-free rebalance and bounded-memory long runs.
- **Exec-sim O(N²) per-bar volume accumulator** (P2-6) — sorted-id accumulator if exec shows on a profile.
- **`Schedule` integer-cadence only** (P2-7) — add a trading-calendar-aware gate (month-end/weekly/session).
- **Probabilistic-fill RNG** (P2-6) — deferred by design (the sim is deterministic by construction).
- **Short borrow-cost accrual, limit-order-book realism, intraday fills** — hooks left; later work.
- **Cost *calibration*** (fit `Y/δ/γ/η`), capacity, walk-forward + deflated-Sharpe — **Phase 5** (Phase 2
  shipped the cost *model* with documented defaults, not the fit).

---

## Anti-roadmap (p0 — explicitly NOT doing)

1. **No live broker / market connectivity.** p0 is a simulator. Live execution is a later module.
2. **No full exchange matching engine.** ExecutionSim models fills/impact, not a limit-order-book
   matching venue.
3. **No alternative data ingestion** (news/satellite/etc.). Price-volume + classifications first
   (matches the verified WorldQuant alpha inputs).
4. **No distributed/cluster compute.** Single-box, cache-efficient first. Scale-out is premature.
5. **No ML autoML alpha mining.** Formulaic alphas + linear/shrinkage combination first; ML ensembles
   only after the honest-backtest substrate is proven (research warns ML overfits at this N).
6. **No UI / dashboard.** Headless engine + reproducible artifacts. Visualization is downstream.
7. **No new general-purpose primitives in the engine.** If it's reusable infrastructure, it belongs in
   `atx-core`, not here.

---

## Strategic decisions — resolved by what shipped

Open forks, resolved at sprint-close cadence as evidence lands.

- **Fork: bar-driven vs tick-driven first?** **RESOLVED at Phase 2 close (2026-06-05): bar-driven.** The
  whole Phase-2 loop (RollingPanel, MarketSlice, daily-close cadence, the cost model's per-bar volume cap)
  is built on sealed OHLCV bars; the `RollingPanel`'s `PanelField` stores OHLCV and the exec sim's
  participation cap reads bar volume. The data model (P1-2) still carries `Tick`, and the loop's `collect`
  ignores tick events — so an intraday/tick path stays open as a later module (listed in Phase-2 residuals:
  intraday fills), but the p0 backtest substrate is daily-bar-driven.
- **Fork: event-driven-only vs hybrid vectorized research layer?** Audit commits to hybrid; the spine
  ships first. *Resolved by architecture audit §2; re-confirm at Phase 3 kickoff.*

---

## Recommended sequencing

If you can only do one slice: **Phase 1** (the event spine) — nothing else is trustworthy without it.
If two: add **Phase 2** (the backtest loop, built around the VM) — earns a runnable, cost-honest backtest on
one alpha, with the `ISignalSource` seam ready for the Phase-3 VM to drop in as the strategy core.
If three: add **Phase 3** (the alpha-expression DSL + vectorized VM) — turns *strings* into cross-sectional
signals at mass scale, the compute core that makes evaluating thousands-to-millions of weak alphas cheap.

Phases 4–5 (pool gates + combination/risk, calibration/validation) follow once there is a stream of evaluated
signals to screen, combine, and validate.

---

*Sprint discipline: per [`../docs/sprint.md`](../docs/sprint.md). Module structure: per
[`../docs/module.md`](../docs/module.md). Code quality gate: per
[`../docs/implementation-quality.md`](../docs/implementation-quality.md) and
[`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md).*
