# Phase 3c — Implementation Progress

**Worktree:** none (direct on `feat/atx-core-stdlib`, per `.agents/atx-engine/agent.md`; plan §9 permits in-place).
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `2aeec2a` (Phase-3b close — `docs(p3b-close)`; the prerequisite this sprint extends).
**Started:** `2026-06-06`
**Source plan:** [`phase-3b-vm-completion-implementation-plan.md`](phase-3b-vm-completion-implementation-plan.md) (SPRINT 3c section).
**Prior progress:** Phase 3 ([`phase-3-progress.md`](phase-3-progress.md), closed `cfaf2d2`) → Phase 3b
([`phase-3b-progress.md`](phase-3b-progress.md), closed `2aeec2a`).

> **Ledger state:** 🚧 **OPEN** — sprint 3c (mass evaluation + VM→loop bridge). Closes the Phase-3 extension;
> baton → Phase 4 (combiner/risk). Scope changes go in *Plan adjustments*, **not** the frozen plan.

---

## Plan adjustments vs. the source plan (fossil reconciliation)

The plan is a fossil frozen against assumed Phase-3 API names. Phase 3/3b shipped different surfaces; 3c builds
on the **as-built** API. Deltas (carried from 3b's reconciliation, plus 3c-specific):

**(A) "Batch evaluation" already exists structurally (affects P3c-1).** The plan posits a NEW
`Engine::evaluate_batch(N strings)`. As-built, `parse_program(src, lib)` already parses **multiple
assignments** (`program := { assignment }`, one named alpha per line) into one `Ast`; `analyze`+`compile` fold
them into **one hash-consed DAG** with one `StoreAlpha` per root; and `Engine::evaluate(const Program&)`
already returns a `SignalSet` holding **N alphas × dates × instruments**. So cross-alpha CSE + batch evaluation
are *already* the default path — there is no per-alpha re-evaluation to replace. **P3c-1's real value-add** is
therefore: (a) expose the **measured cross-alpha CSE metrics** (unique vs total AST nodes, the lever Phase-3's
P3-4 table left empty) off the `Dag`/`Program`, (b) a thin batch convenience entry (compile-N-strings → one
Program) if it clarifies the API, and (c) the mined-style-battery bench. To confirm/finalize at P3c-1.

**(B) The `VmSignalSource` bridge needs the as-built alpha API, not the frozen one (affects P3c-3).** The
Phase-2 `loop/signal_source.hpp` froze a guarded `VmSignalSource` against an *assumed* alpha API:
`alpha::Engine::run(program, panel)`, `alpha::Program::max_lookback()`, a movable `alpha::Engine`. The
**as-built** alpha API is:

| Frozen Phase-2 assumption | As-built (Phase 3) |
|---|---|
| `Engine::run(program, panel) → Result<span<f64>>` (per-call panel, single column) | `Engine(const Panel&)` binds the panel at construction; `Engine::evaluate(const Program&) → Result<SignalSet>` returns the WHOLE date×instrument matrix for ALL alphas |
| `Program::max_lookback()` | `Program::required_lookback` (field) |
| loop passes `loop::PanelView` | alpha VM consumes `alpha::Panel` (`panel.hpp`) — the adapter must build/refresh an `alpha::Panel` from the loop's trailing `PanelView` |

So P3c-3 is **real adapter work**, not a macro flip: on each `evaluate(PanelView)` the adapter builds an
`alpha::Panel` from the rolling trailing window, runs the VM, and extracts the **current-date cross-section row**
(the last date) of the program's (single) alpha as the `SignalView`. The Phase-2 §10 risk anticipated exactly
this ("update the adapter to the as-built API and note the delta"). The adapter body in `signal_source.hpp`
will be rewritten to the as-built API and un-guarded behind `ATX_ENGINE_HAS_ALPHA_VM`. Zero-alloc on the hot
path is the target; if building the `alpha::Panel`/`Engine` per call forces allocation, that is recorded as a
measured residual (cold-ish research cadence acceptable per plan §3.5) and the zero-alloc claim is scoped to
what is achievable.

**(C) `AlphaStreams`/`extract_streams` reuse the Phase-2 portfolio glue (affects P3c-2).** New header
`streams.hpp`. Reuses `loop/weight_policy.hpp` (`WeightPolicy`) + `exec/execution_sim.hpp`
(`ExecutionSimulator` / cost model) — NO new portfolio logic (anti-roadmap; plan §10 watch-item). `PanelView`
in the plan's signature = the as-built panel type the WeightPolicy/loop consume (reconcile at the unit).

**(D) No worktree; no `--no-ff` merge; clang-tidy disabled** — same as 3b (adjustments B/C there).
**(E) Shared-tree discipline.** Multiple efforts commit to `feat/atx-core-stdlib` with a SHARED git index;
a concurrent tsdb-v2/SQLite effort holds uncommitted edits (`ROADMAP.md`, `.agents/cpp/agent.md`, `.clang-tidy`,
`.clangd`, `.vscode/`, `atx-core/*`, `panel.hpp`). Commit **path-limited** (`git commit -- <paths>`, never
`git add -A`); never touch those files. The ROADMAP Phase-3b/3c status flip is left for that owner.

Realistic scope (P3c-0…P3c-4):

1. **P3c-0** — Open this ledger; record base `2aeec2a`. Marker commit.
2. **P3c-1** — Cross-alpha CSE metrics off the existing batch path + bench (per adjustment A).
3. **P3c-2** — `extract_streams` → `AlphaStreams` (per-alpha PnL/position; Phase-4 feed; reuse WeightPolicy+ExecSim).
4. **P3c-3** — `VmSignalSource` green-gate (as-built adapter, per adjustment B) + delay-0/delay-1 knob.
5. **P3c-4** — Integration · batch determinism · CSE evidence · bench · `phase-3.md` extension · sprint-3c close.

Defer to Phase 4 (or future-work): the combiner/gates/risk/optimizer; position-based combiner; parallel batch
+ Linux TSan; computed-goto/JIT.

---

## Per-unit ledger

| Unit | Status | Commit | Notes |
|------|--------|--------|-------|
| P3c-0 | ✅ done | _(this)_ | Open ledger; record base `2aeec2a`; fossil reconciliation (A–E) — esp. (A) batch already exists, (B) bridge needs the as-built adapter. Marker. |
| P3c-1 | ✅ done | `89f309f` (+ fix `6c2f157`) | **Batch already exists** (adj. A confirmed): `parse_program` (multi-assignment) → `analyze` → `compile` folds N alphas into ONE hash-consed DAG; `Engine::evaluate` already returns one `SignalSet::Alpha` per root — no `evaluate_batch` needed. Added (a) `compile_batch(span<string_view>, Library&)`, a thin convenience over that pipeline (auto-names entries `aN`, joins one-per-line, propagates Err on a malformed source — never throws); (b) **intern cache-hit telemetry** — `Dag::cache_hits()`/`intern_attempts()` (one `++` per `intern()` call / hit, pure observability) carried onto `Program.cache_hits`/`intern_attempts` + a `cache_hit_pct()` accessor, beside the pre-existing `unique_nodes`/`total_ast_nodes`. Invariant `intern_attempts == cache_hits + unique_nodes`. Proofs: **batch==singly** (each alpha[i] cell-identical, NaN==NaN, to compiling+evaluating it alone), **order-independence** (two submission orders → identical hash after sort-by-name; raw hashes differ so non-vacuous), CSE `unique < total` + `cache_hits>0` on overlap, boundaries (batch-of-1, fully-disjoint `unique==total`/`cache_hits==0`, identical-alphas heavy dedup). 10 AlphaBatch tests; full Alpha suite 348/348 green. Bench `alpha_batch_bench.cpp` (mined 24-alpha battery, 512×256). See measured sub-table below. **Follow-up `fix(p3c-1)`** (review finding): `compile_batch` enforces its `roots[i] <-> alpha_srcs[i]` 1:1 contract — rejects an embedded-newline source up front AND defensively errors when `roots.size() != alpha_srcs.size()` (the lexer treats `\n` as whitespace, so `"close\nfoo = open"` would otherwise inject a silent 2nd root). +2 tests (8→10). |
| P3c-2 | ✅ done | `59a47a3` | **`extract_streams(SignalSet, WeightPolicy, alpha::Panel, ExecutionSimulator) → AlphaStreams`** (new `alpha/streams.hpp`). Per-alpha PnL + position streams, the typed Phase-3→4 handoff (adj. C). **Shape:** `AlphaStreams` owns two flat dense buffers — `pnl_flat` `[n_alphas][n_periods]` and `pos_flat` `[n_alphas][n_periods][n_instruments]`; accessors `pnl(a)` / `positions(a,t)` return correctly-offset `std::span<const f64>` (`[[nodiscard]] const noexcept`). **Reuse (no new portfolio/P&L logic):** positions = `loop::WeightPolicy::to_target_weights(signal_row, universe)` verbatim (winsorize→rank/zscore→dollar-neutral→gross-scale); cost = the SAME `ExecutionSimulator`'s coefficient via a new additive `commission_cfg()` accessor (read-only). **Accounting + alignment:** `pnl[t] = Σ_j w_j[t-1]·ret_j[t] − turnover[t]·cost_rate`, `ret_j[t]=close_j[t]/close_j[t-1]−1` (NaN/≤0 prior close → 0 contribution), `turnover[t]=Σ_j|w_j[t]−w_j[t-1]|`, `cost_rate = PerDollar per_dollar_bps/1e4` (0 for PerShare / frictionless). No look-ahead: prior weights earn this period's return; `pnl[0]=0` (no prior weight/price). **Loop-match anchor:** EXACT (1e-9) for the honest undrifted case — a single price move on the slice where the established integer-share book (Rank weights ±0.5, 100k equity, px 100 → ±500 sh, NO truncation residual) sits against the still-undrifted base equals the analytic Σw·ret. The general multi-period fixed-shares-vs-constant-weight equity-base drift is a DOCUMENTED residual (the loop divides dollar PnL by drifted equity; Phase-4 consumes the constant-weight analytic stream by contract) — not a glue bug. **10 AlphaStreams tests**; full Alpha suite 358/358 green; `/W4 /permissive- /WX` clean; clang-format applied. |
| P3c-3 | ✅ done | `afbb57d` (+ refactor `85fe38d`) | **`VmSignalSource` rewritten to the as-built alpha API + delay knob** (adj. B resolved). **API delta:** the Phase-2 freeze assumed `Engine::run(program, panel)→span` + `Program::max_lookback()`; as-built is `Engine(const Panel&)` (binds at construction) + `evaluate(const Program&)→Result<SignalSet>` (whole date×instrument matrix, all roots) + `Program::required_lookback` (a `u16` field). **Adapter (`evaluate(PanelView)`):** builds a date-major CHRONOLOGICAL `alpha::Panel` from the newest-first `PanelView` — alpha date `d` ← PanelView row `(rows-1)-d` (the load-bearing row reversal), reshaped column-major→date-major, all five OHLCV fields named (`open/high/low/close/volume`; the VM loads only those the program references), PIT universe mask from `present()`. Constructs `Engine{panel}`, `evaluate(program_)`, extracts the CURRENT-date cross-section = root 0's LAST date row (`alpha_cross_section(0, dates-1)`) into a source-owned `signal_` buffer; returns a `SignalView` borrowing THAT buffer (no temporary). `max_lookback()` forwards `required_lookback`. **Green-gate:** `#define ATX_ENGINE_HAS_ALPHA_VM 1` IN-HEADER (alpha VM headers are header-only + always present) — no CMakeLists edit; the frozen `#if` block rewritten + un-guarded. **Delay knob:** `loop::Delay {Same,Next}` (default **Next**) in `types.hpp`; `BacktestLoop` ctor takes a trailing `Delay delay = Delay::Next`. `Delay::Same` (delay-0) flips a new `ExecutionSimulator::set_allow_same_bar_fill(true)` AND adds a dedicated post-queue `settle_at(t)` in `rebalance()` — gated ENTIRELY by `delay_`, so the default (Next) path keeps its single pre-queue settle and the no-look-ahead firewall is STRUCTURALLY intact. **Proofs:** adapter==direct-engine-last-row (rank(close)); chronological-transpose sign (`-delta(close,1)`: rising→neg, falling→pos); pure-in-panel repeatability; `max_lookback` forwards `required_lookback`; single-row panel; **VM drives the REAL BacktestLoop** (runs to EOF, deterministic byte-identical fills/equity, no fill on the decision slice under Next); delay-0 fills one bar earlier than delay-1 (signals identical); default==explicit-Next + firewall intact. **10 VmSignalSource tests; full engine suite 1429/1429 green; `/W4 /permissive- /WX` clean; clang-format applied.** Alloc residual below. **Follow-up `refactor(p3c-3)`** (code-quality nits, no behavior change): (1) `build_alpha_panel` reused `field_data_` in NAME only — the prior `field_data_.assign(kPanelFieldCount, vector<f64>(cells,NaN))` replaced all 5 inner columns with fresh temporaries (5 reallocs/call); now `ensure_field_scratch` resizes the outer vector ONCE then `col.assign(cells,NaN)` per column, reusing capacity in place (no realloc on the steady-state same-shape path) — the documented scratch reuse is now real. (2) `ohlcv_field_names()` hoisted to a function-local `static const std::vector<std::string>` (built once, copied into `Panel::create`'s by-value param) — no per-call `vector<string>` alloc. Class ALLOCATION comment corrected: the only per-call alloc is now the Panel-column COPY + Engine pool. Suites stay 10/10 · 358/358 · 97/97 exec-loop green. |
| P3c-4 | ✅ done | _(SHA pending)_ | **Phase-3c integration suite + bench + `phase-3.md` extension.** NEW `tests/phase3c_integration_test.cpp` (9 tests) orchestrating the landed pieces into the four proof groups: **(1) batch determinism** — a 5-alpha mixed-family batch over a synthetic panel WITH delisted instruments (universe→0 at date `2/3`) + scattered NaN source gaps, evaluated TWICE → identical `signal_hash` (replay-stable); THREE mutations each flip it non-vacuously: reorder-alphas (flips the RAW index-ordered hash but NOT the by-name-sorted hash — result is a function of the alpha SET), perturb-one-input-cell, add-a-late-date-row. **(2) CSE evidence** — a 12-alpha high-overlap battery: `unique_nodes(23) < total_ast_nodes(66)`, `cache_hits(43)>0`, `intern_attempts==cache_hits+unique_nodes`, ratio 0.348 / cache_hit_pct 65.15% (echoed to stdout for this ledger). **(3) bridge E2E** — a compiled `-delta(close,1)` drives `VmSignalSource → BacktestLoop → Portfolio`: costs-off run is byte-identical on replay (fills + per-slice equity curve + final_equity), and BOTH `Delay::Same` and `Delay::Next` run to EOF with identical rebalance counts (same signals) while delay-0 fills strictly one bar earlier; Next keeps the no-fill-on-decision-slice firewall. **(4) Phase-4 readiness** — `extract_streams` output feeds a trivial in-test Sharpe consumer `mean(pnl)/stddev(pnl)` over `pnl(0)` → a finite, non-zero value (-0.1466 on the known stream); a flat stream → defined 0 (no NaN). **Bench** NEW `bench/alpha_streams_bench.cpp`: `BM_ExtractStreams` (alphas/s) + `BM_VmSignalSourceEvaluate` (ns/rebalance) — measured sub-table below. **`phase-3.md`** extended (sections 7–10 appended, existing §1–§6 untouched): the BRAIN-superset operators + variadic/default args, the locked semantics (indneutralize=demean, signedpower=sign·\|x\|^a, floor(d), full-window min_periods), the `compile_batch` API + CSE telemetry, `extract_streams`/`AlphaStreams`, and the `VmSignalSource`+`Delay` bridge (links the ledgers for numbers). **9 Phase3cIntegration tests; full Alpha+VmSignalSource+Backtest run 395/395 green; `/W4 /permissive- /WX` clean; clang-format applied; both bench targets build + run.** |

### P3c-1 measured CSE lever

Mined 24-alpha high-overlap battery (verbatim from the P3-9 proof bench), compiled ONCE to one
cross-alpha-CSE Program; warm `Engine::evaluate` over a fixed 512×256 panel (131 072 cells).
**Debug / clang-cl build — these are UPPER-BOUND figures, not release numbers.** Host: 16× 2496 MHz,
L1d 48 KiB, L2 1280 KiB, L3 18432 KiB. (`build/bin/atx-engine-bench.exe --benchmark_filter=BM_BatchEvaluate`.)

| Metric | Value |
|--------|-------|
| alphas | 24 |
| unique_nodes | 41 |
| total_ast_nodes | 156 |
| unique/total ratio | 0.2628 (~74% of lowered nodes folded by cross-alpha CSE) |
| cache_hits / intern_attempts | 115 / 156 |
| cache_hit_pct | 73.72 % |
| num_slots (peak live) | 12 |
| evaluate wall time | ~282 ms / call (24 alphas × 131 072 cells, Debug) |
| throughput | ~11.50 M alpha-cells/s ⇒ **~86.9 ns/cell**, **~85 alphas/s** |

The cache-hit% (73.7) ≈ 1 − unique/total (0.263); both quantify the same cross-alpha dedup from opposite sides
(`intern_attempts == cache_hits + unique_nodes` holds exactly). The metric is **reportable** off `Program` with
no derived computation needed.

### P3c-4 measured

`build/bin/atx-engine-bench.exe --benchmark_filter="BM_ExtractStreams|BM_VmSignalSourceEvaluate"`.
**Debug / clang-cl build — UPPER-BOUND latencies, NOT release numbers.** Host: 16× 2496 MHz,
L1d 48 KiB, L2 1280 KiB, L3 18432 KiB (`***WARNING*** Library was built as DEBUG`).

| Bench | Panel shape | Time | Throughput |
|-------|-------------|------|------------|
| `BM_ExtractStreams` | 16 alphas × 256 periods × 64 instruments | ~148 623 µs / call | **~108.9 alphas/s** (one alpha-stream = an item) |
| `BM_VmSignalSourceEvaluate` | 16 instruments, lookback 10, `rank(close)-ts_mean(close,5)` | **~76 330 ns / rebalance** | ~13.03 k rebalances/s |

`extract_streams` is the cold-ish research-cadence Phase-3→4 handoff (builds N dense PnL+position streams via
`WeightPolicy` per period). `VmSignalSource::evaluate` is the per-rebalance bridge overhead the `BacktestLoop`
pays (transpose + fresh `alpha::Panel`/`Engine` build + VM run + current-date extract — the per-call
`Panel`/`Engine` allocation is the documented P3c-3 residual below; evaluate runs at the rebalance cadence, not
per bar).

### P3c deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_

- _(filled as units land)_
- **P3c-2 turnover-cost approximation.** `extract_streams` charges only the `ExecutionSimulator`'s linear PerDollar (`per_dollar_bps`) coefficient as `turnover·rate`. PerShare commission, slippage, and √-impact are share-/participation-scaled with no closed per-turnover form at weight granularity, so they are NOT modelled in the per-alpha stream (research-cadence approximation; the FULL FIFO/impact loop remains authoritative for a sized backtest). The frictionless (costs-off) stream is bit-exact to a direct loop run; costs-on is the linear-turnover approximation.
- **P3c-2 constant-weight vs fixed-share accounting.** The analytic stream is a constant-weight (rebased-each-period) return; the `BacktestLoop` holds an integer-share book and divides dollar PnL by drifted equity. They agree exactly only against an undrifted base (proven in the anchor test); multi-period equity-base drift is the documented divergence (by-contract Phase-4 consumes the constant-weight stream).
- **P3c-3 per-call `alpha::Panel`/`Engine` allocation.** The as-built `alpha::Engine` binds its `Panel` at CONSTRUCTION (no rebind API), so `VmSignalSource::evaluate` builds a fresh `alpha::Panel` (owned columns, copied from the source's reused `field_data_`/`universe_` scratch) and a fresh `Engine` (its slot pool) per call. The source REUSES `field_data_`/`universe_`/`signal_` across calls, so the transpose + signal extraction are steady-state allocation-free, but `Panel::create` (which copies the field columns) and the `Engine` allocate per evaluate(). This is acceptable per plan §3.5: `evaluate()` runs at the REBALANCE cadence (per-schedule, not per-bar), so the cold-ish build is a documented residual, NOT a hot-path regression. Zero-alloc is therefore scoped to the transpose/extract, not claimed for the whole call. A future `Engine::rebind(Panel&)` would close it; deferred.
- **P3c-3 delay-0 is a dedicated post-queue settle, not a firewall relaxation of the default path.** Within one slice the loop settles (step 3) BEFORE it queues (step 7) and timestamps strictly increase, so the sim's `allow_same_bar_fill` (`now >= queued_at`) relaxation alone would be a no-op (the next settle is always at a strictly-later `t`). delay-0 is therefore implemented as an EXTRA `settle_at(t)` after queue, gated entirely by `Delay::Same` — the default (Next) never runs it. This keeps the structural no-look-ahead firewall fully intact for the conservative default (proven: `DefaultDelayIsNext_FirewallIntact`, `DrivesRealLoop_NoFillOnDecisionSlice_Next`); delay-0 is genuinely opt-in and fills one bar earlier (`Delay0_FillsOneBarEarlierThanDelay1`). No default-path weakening.

---

## Phase 3c sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| _(this)_ | marker (P3c-0) | — (no logic; build stays green) |
| `89f309f` | P3c-1 | AlphaBatch 8/8; full Alpha suite 346/346/0/0; bench builds + runs |
| `6c2f157` | P3c-1 fix | reject root-desync in compile_batch; AlphaBatch 10/10; full Alpha suite 348/348/0/0 |
| `59a47a3` | P3c-2 | AlphaStreams 10/10; full Alpha suite 358/358/0/0; `/W4 /WX` clean |
| `afbb57d` | P3c-3 | VmSignalSource 10/10; full engine suite 1429/1429/0/0; `/W4 /permissive- /WX` clean; clang-format applied |
| `85fe38d` | P3c-3 refactor | alloc-hygiene (real field-scratch reuse + static field names); VmSignalSource 10/10; Alpha 358/358; exec/loop 97/97; `/W4 /WX` clean |
| _(SHA pending)_ | P3c-4 | Phase3cIntegration 9/9; full Alpha+VmSignalSource+Backtest run 395/395/0/0; `/W4 /permissive- /WX` clean; clang-format applied; bench builds + runs (extract_streams + VmSignalSource) |

---

## What Phase 3c proves / Next sprint priorities

_(Written at sprint close.)_ Baton → Phase 4 (signal combination + Barra risk + optimizer).
