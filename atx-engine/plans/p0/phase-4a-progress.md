# Phase 4a — Implementation Progress

**Worktree:** NONE — in-place on the active shared branch (the established engine workflow; Phase 3b/3c worked this way too; `.agents/atx-engine/agent.md` is the authority).
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `bf4cbfb`
**Started:** 2026-06-06
**Source plan:** [`phase-4-signal-combination-risk-implementation-plan.md`](phase-4-signal-combination-risk-implementation-plan.md)
**Prior progress:** Phase 3c ([`phase-3c-progress.md`](phase-3c-progress.md))

---

## Plan adjustments vs. the source plan

Phase 4 is split into two sprints: **4a** (this ledger, P4-0…P4-5 — alpha pool + gates + combiner) and **4b** (P4-6…P4-10 — risk model, optimizer, portfolio integration). The source plan §1–§11 is frozen; this sprint covers only the 4a scope.

The plan carries an **amendment §0 (Phase-3 as-built reconciliation, 2026-06-06)** that overrides the frozen §1–§11 wherever they disagree. The 4a-relevant deltas are: **P4-1** wraps the as-built `alpha::AlphaStreams` + `extract_streams` API rather than rebuilding dense signal matrices from scratch — `AlphaRecord` holds a compiled `Program` and the store's streaming interface is layered on the existing Phase-3 VM output; **P4-2** computes metrics over `AlphaStreams::pnl(a)` and treats `pnl[0]=0` as a structural zero (the first bar is always NaN-equivalent for a causal signal, so any Sharpe/IC formula must skip it); **P4-4** the combiner's input pool is a batch `SignalSet → extract_streams` pass — weights are fit on the constant-weight stream (equal-weight baseline first, IC-weighted second), keeping the hot path allocation-free; **P4-5** the production form is a single `compile_batch` Program adapter (`CombinedSignalSource`) whose `max_lookback()` forwards `Program::required_lookback` (a field, not a virtual call), and a `vector<ISignalSource*>` form is retained for unit tests (avoids pulling the full combiner into test fixtures).

Realistic scope for this sprint:

1. **P4-0** — Module scaffold + CMake + ledger (marker). Create `combine/fwd.hpp`, trivial scaffold test, open ledger. No combination logic.
2. **P4-1** — `AlphaStore`: insertion-ordered registry of live alphas (`AlphaId`, `AlphaRecord`, `AlphaStore`), wrapping the as-built `alpha::AlphaStreams` + `extract_streams`.
3. **P4-2** — `AlphaMetrics`: per-alpha fitness statistics (IC, turnover, Sharpe, max-drawdown, pairwise-correlation summary) computed over `AlphaStreams::pnl(a)`, `pnl[0]=0` structural-zero convention.
4. **P4-3** — `AlphaGate`: stateless fitness screen (`GateConfig` thresholds → `GateVerdict`) filtering the pool on IC floor, max pairwise correlation, max turnover, min Sharpe.
5. **P4-4** — `AlphaCombiner`: weight-fitting over admitted pool (`CombineMethod`, `CombinerConfig`, `Combination`); equal-weight baseline + IC-weighted; batch `SignalSet → extract_streams` hot path.
6. **P4-5** — `CombinedSignalSource`: `compile_batch` Program adapter implementing `ISignalSource`, `max_lookback()` = `Program::required_lookback`. Sprint close.

Defer to Phase 4b (or later):

- **P4-6…P4-10** — Barra-style risk model, portfolio optimizer, full Phase-2 loop integration, live-trading adapter — in the 4b ledger.

---

## Per-unit ledger

| Unit  | Status | Commit  | Notes |
|-------|--------|---------|-------|
| P4-0  | done   | `44d88a8` | scaffold + ledger; combine_scaffold_test green (CombineScaffold 1/1/0/0) |
| P4-1  | done   | `<sha>` | `combine/metrics.hpp` (AlphaMetrics POD only — P4-2 adds compute) + `combine/store.hpp` (AlphaId, AlphaRecord, AlphaStore). Append-only, insertion-ordered pool owning each alpha's PnL row + per-period position stream + a NON-OWNING `atx::engine::ISignalSource*` re-eval handle (forward-declared). **§0-A wrap-not-rebuild:** insert() COPIES pre-computed `extract_streams` rows into the store's own id-ordered dense matrices — it never recomputes PnL; `ingest_streams(AlphaStreams, sources, metrics)` is the "one extract_streams call" batch path (one insert per row in id order). `pnl_matrix()` is **alpha-major row-major `[n_alphas × n_periods]`** (element (a,t) at `a*n_periods+t`; row a == `pnl(id)`); first insert fixes n_periods/n_instruments, later period-count mismatch → `Err(InvalidArgument)` (store unchanged). NaN streams stored verbatim. `ingest_streams` tested against a hand-built `alpha::AlphaStreams` (aggregate-constructible, no extract_streams pipeline needed). AlphaStore 8/0/0 green; /W4 /permissive- /WX clean. |

---

## Phase 4a sprint commits

| Commit  | Unit   | Test counts |
|---------|--------|-------------|
| `44d88a8` | marker (P4-0) | CombineScaffold 1/1/0/0 |
| `<sha>` | P4-1 (alpha store + record) | AlphaStore 8/0/0 |

---

## What Phase 4a proves / Next

_(Fill at sprint close.)_
