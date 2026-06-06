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
| P3c-3 | ⏳ pending | — | |
| P3c-4 | ⏳ pending | — | |

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

### P3c deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_

- _(filled as units land)_
- **P3c-2 turnover-cost approximation.** `extract_streams` charges only the `ExecutionSimulator`'s linear PerDollar (`per_dollar_bps`) coefficient as `turnover·rate`. PerShare commission, slippage, and √-impact are share-/participation-scaled with no closed per-turnover form at weight granularity, so they are NOT modelled in the per-alpha stream (research-cadence approximation; the FULL FIFO/impact loop remains authoritative for a sized backtest). The frictionless (costs-off) stream is bit-exact to a direct loop run; costs-on is the linear-turnover approximation.
- **P3c-2 constant-weight vs fixed-share accounting.** The analytic stream is a constant-weight (rebased-each-period) return; the `BacktestLoop` holds an integer-share book and divides dollar PnL by drifted equity. They agree exactly only against an undrifted base (proven in the anchor test); multi-period equity-base drift is the documented divergence (by-contract Phase-4 consumes the constant-weight stream).

---

## Phase 3c sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| _(this)_ | marker (P3c-0) | — (no logic; build stays green) |
| `89f309f` | P3c-1 | AlphaBatch 8/8; full Alpha suite 346/346/0/0; bench builds + runs |
| `6c2f157` | P3c-1 fix | reject root-desync in compile_batch; AlphaBatch 10/10; full Alpha suite 348/348/0/0 |
| `59a47a3` | P3c-2 | AlphaStreams 10/10; full Alpha suite 358/358/0/0; `/W4 /WX` clean |

---

## What Phase 3c proves / Next sprint priorities

_(Written at sprint close.)_ Baton → Phase 4 (signal combination + Barra risk + optimizer).
