# Phase 3b — Implementation Progress

**Worktree:** none (direct on `feat/atx-core-stdlib`, per `.agents/atx-engine/agent.md` — the established
engine workflow; plan §9 permits in-place "per the established engine workflow — record in the ledger").
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `cfaf2d2` (Phase-3 close — `docs(p3-close)`; the prerequisite SHA this sprint extends).
**Started:** `2026-06-06`
**Source plan:** [`phase-3b-vm-completion-implementation-plan.md`](phase-3b-vm-completion-implementation-plan.md)
**Prior progress:** Phase 3 (alpha-expression DSL → VM) — see [`phase-3-progress.md`](phase-3-progress.md), closed at `cfaf2d2`.

> **Ledger state:** 🚧 **OPEN** — sprint 3b (vocabulary + variadic args + conformance). Sprint 3c
> (mass eval + VM→loop bridge) opens its own ledger [`phase-3c-progress.md`](phase-3c-progress.md) at P3c-0.
> Scope changes are recorded in *Plan adjustments* below, **not** in the frozen plan file.

---

## Plan adjustments vs. the source plan (fossil reconciliation)

The plan was frozen against *assumed* Phase-3 header/API names. Phase 3 shipped with different names; this
sprint builds on the **as-built** surface. The plan is a fossil — these deltas are recorded here, not edited
into it.

**(A) As-built header map (plan name → real name).** The plan references `engine.hpp`, `reference.hpp`,
`kernels_*.hpp`, `compile({...strings...})`, `Engine(panel, lib)`, `evaluate_batch`, `extract_streams`,
`PanelView`. As-built (Phase 3 close `cfaf2d2`):

| Plan-fossil name | As-built (Phase 3) |
|---|---|
| `engine.hpp` (Engine) | `vm.hpp` — `class Engine{ explicit Engine(const Panel&); Result<SignalSet> evaluate(const Program&); }` |
| `reference.hpp` (oracle) | `oracle.hpp` — `evaluate_reference(const Program&, const Panel&) → Result<SignalSet>` |
| `kernels_*.hpp` | `cs_ops.hpp` (cross-sectional kernels), `ts_ops.hpp` (time-series kernels) |
| `compile({strings}, lib)` one-shot | as-built pipeline: `parse_program(src, lib)` → `analyze(ast)` → `compile(ast, analysis)` |
| `PanelView` | `Panel` (self-contained, `panel.hpp`); no separate view type |
| `Engine(panel, lib)` | `Engine(const Panel&)` — the Library is consulted at parse/compile, not by the Engine |

New headers this sprint adds: none in 3b (ops live in the existing `registry.hpp`/`vm.hpp`/`oracle.hpp`/
`cs_ops.hpp`/`ts_ops.hpp`). 3c adds `streams.hpp` and a batch entry on `vm.hpp`.

**(B) No worktree; no `--no-ff` merge.** Phase 3's adjustment (2) stands: one working tree, direct on
`feat/atx-core-stdlib`; the branch IS the active line, so there is no worktree to merge. The close ceremony
runs every sprint.md step *except* the merge.

**(C) clang-tidy is disabled repo-wide** (`.agents/cpp/agent.md` §8, `.agents/atx-engine/agent.md`). The
per-unit gate is **`/W4 /permissive- /WX` + clang-format clean + tests green** (the plan §7 "clang-tidy clean"
done-criterion does not apply).

**(D) Shared-branch discipline.** `ROADMAP.md`, `.agents/cpp/agent.md`, `.clang-tidy`, `.clangd`, `.vscode/`
carry concurrent **uncommitted** edits (a parallel effort on this shared branch). Stage **explicit pathspecs
only** (never `git add -A`); do not touch those files. The ROADMAP Phase-3b status flip is left for the
concurrent owner (same posture as Phase 3's close note).

Realistic scope for this sprint (P3b-0…P3b-4):

1. **P3b-0** — Open this ledger; freeze scope; record the Phase-3 base SHA. Marker commit.
2. **P3b-1** — Variadic/default/optional operator arguments (`OpSig` `arity` → `[min_arity,max_arity]` +
   trailing `defaults`; parser default-fill + range check). The foundation the new ops sit on.
3. **P3b-2** — BRAIN-superset ops: element-wise (`sigmoid`/`tanh`/`normalize`/`winsorize`), rolling
   (`ts_zscore`/`ts_backfill`/`ts_av_diff`/`ts_quantile`/`ts_scale`/`ts_count_nans`), group
   (`group_count`/`group_mean`/`group_scale`). Each = 1 OpCode + 1 row + 1 VM kernel + 1 oracle entry + diff test.
4. **P3b-3** — `trade_when` + `hump` stateful causal recurrences (reuse the ema/wma state-slot mechanism).
5. **P3b-4** — Alpha101 conformance battery + pinned semantic resolutions + sprint-3b close.

Defer to Phase 3c (or future-work):

- **Batch / cross-alpha eval API, per-alpha PnL/position streams, `VmSignalSource` bridge, delay knob** →
  Phase 3c (P3c-0…P3c-4).
- **True O(1)/cell incremental rolling** for the new rolling ops — carry Phase-3's deferral (bit-exactness
  vs the oracle dominates; an online accumulator needs a documented ULP tolerance).
- **Full BRAIN op zoo** (100+ ops, region universes, vector/matrix ops) — ship the consultant-used subset;
  the registry makes any later op a one-row add.

---

## Per-unit ledger

| Unit | Status | Commit | Notes |
|------|--------|--------|-------|
| P3b-0 | ✅ done | _(this)_ | Open ledger; freeze scope; fossil reconciliation (A–D) recorded; Base `cfaf2d2`. Marker commit. |
| P3b-1 | ⏳ pending | — | |
| P3b-2 | ⏳ pending | — | |
| P3b-3 | ⏳ pending | — | |
| P3b-4 | ⏳ pending | — | |

### P3b deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_

- _(filled as units land)_

---

## Phase 3b sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| _(this)_ | marker (P3b-0) | — (no logic; build stays green) |

---

## What Phase 3b proves / Next sprint priorities

_(Written at sprint close.)_ Baton → Phase 3c (mass eval + VM→loop bridge), then Phase 4 (combiner/risk).
