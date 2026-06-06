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
| P3c-1 | ⏳ pending | — | |
| P3c-2 | ⏳ pending | — | |
| P3c-3 | ⏳ pending | — | |
| P3c-4 | ⏳ pending | — | |

### P3c deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_

- _(filled as units land)_

---

## Phase 3c sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| _(this)_ | marker (P3c-0) | — (no logic; build stays green) |

---

## What Phase 3c proves / Next sprint priorities

_(Written at sprint close.)_ Baton → Phase 4 (signal combination + Barra risk + optimizer).
