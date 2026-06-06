# Phase 3 — Implementation Progress

**Worktree:** none (direct on feat/atx-core-stdlib)
**Branch:** `feat/atx-core-stdlib`
**Base:** feat/atx-core-stdlib @ `e89048b`
**Started:** `2026-06-05`
**Source plan:** [`phase-3-alpha-expression-dsl-implementation-plan.md`](phase-3-alpha-expression-dsl-implementation-plan.md)
**Prior progress:** Phase 1 (event spine) — see [`phase-1-progress.md`](phase-1-progress.md). Phase 2
(backtest loop) precedes this in the ROADMAP but Phase 3's front-end (P3-1..P3-4) is independent of it.

> **Ledger state:** marker-stage skeleton. Rows are `⏳ pending` until each unit lands. Update each row in
> the same commit as its implementation (or follow up with `docs(p3-N): record SHA`). Never fudge test
> counts. Scope changes go in *Plan adjustments* below, **not** in the frozen plan file.

---

## Plan adjustments vs. the source plan

**(1) All atx-core upstream layers have landed.** The frozen plan assumed P3-5..P3-8 would be blocked behind
the `atx_engine_pending` staged-red CMake label, waiting on:

- **L5** (`simd`) — `atx/core/simd.hpp` ✅ present
- **L6** (`cross_section`, `rolling`, `online_stats`) — `atx/core/stats/cross_section.hpp`,
  `rolling.hpp`, `online_stats.hpp` ✅ present
- **L9** (`series/column`, `series/frame`) — `atx/core/series/column.hpp`, `frame.hpp` ✅ present
- **L1** (`hash`), **L2** (`arena`, `object_pool`, `aligned`), **L3** (`hash_map`, `fixed_vector`),
  **L8** (`datetime`) ✅ all present

All blocking headers exist under `atx-core/include/atx/core/` as of base SHA `e89048b`. Therefore
**the entire sprint (P3-0..P3-9) targets fully green** — no `atx_engine_pending` staged-red label is
needed or created.

**(2) No worktree.** The established engine workflow (`.agents/atx-engine/agent.md`) is to work directly
on the current branch `feat/atx-core-stdlib`. There is only one working tree. The stale worktree path
`.claude/worktrees/phase-3-alpha-dsl` / branch `worktree-phase-3-alpha-dsl` recorded in the skeleton
header has been corrected above.

Realistic scope for this sprint:

1. **P3-0** — Module scaffold + CMake + ledger (marker). Open ledger, freeze scope.
2. **P3-1** — Lexer (tokens + spans). *not blocked.*
3. **P3-2** — Pratt parser → AST + operator registry (`Library`); const-fold + desugar. *not blocked.*
4. **P3-3** — Shape/dtype check + lookback analysis (causality rail). *not blocked.*
5. **P3-4** — Hash-consed expression DAG (free CSE) + bytecode linearization + slot/refcount. *not blocked.*
6. **P3-5** — Columnar eval context (Panel/SlotPool) + tree-walking reference oracle. *blocked-on L9 + L2 + L8.*
7. **P3-6** — Vectorized VM core + element-wise/logical/select opcodes. *blocked-on L5 + L2.*
8. **P3-7** — Cross-sectional opcodes (`rank`/`zscore`/`scale`/(group_)neutralize). *blocked-on L6 `cross_section`.*
9. **P3-8** — Time-series opcodes (`delay`/`delta`/`ts_*`/`correlation`/`decay_linear`/…). *blocked-on L6 `rolling`/`online_stats`.*
10. **P3-9** — Differential + determinism + lookahead harness + parallel eval + bench + sprint close.

Defer (out of Phase 3 scope — see ROADMAP):

- Alpha **store** + **correlation/turnover orthogonality gates** (mass-alpha screening) → **Phase 4**.
- Mega-alpha **combiner**, **Barra risk model**, portfolio optimizer → **Phase 4**.
- **JIT** (LLVM/asmjit) — future-work backlog; vectorized interpreter is the baseline.
- BRAIN-superset ops (`ts_entropy`, `ts_moment`, `ts_backfill`, region/group zoo) → future-work (registry
  makes each a one-row add).

---

## Per-unit ledger

| Unit | Status | Commit | Notes |
|------|--------|--------|-------|
| P3-0 | ✅ done | `ac81776` | Scaffold `include/atx/engine/alpha/fwd.hpp`; Phase-3 section in scaffold_test.cpp; ledger. No `atx_engine_pending` label (upstream landed). Marker commit. |
| P3-1 | ✅ done | `94fee4c` | `Token`/`TokenKind`/`Span`; hand-written lexer (`lex`), `from_chars` numbers, maximal-munch ops, interior-dot idents (`IndClass.sector`), `Result` ParseError w/ offset on bad byte. Header-only (`inline`), tidy/format clean. 55 tests. *not blocked.* |
| P3-2 | ⏳ pending | `—` | Pratt parser → `Expr` AST; `Library` registry (`OpSig`); const-fold + desugar. *not blocked.* |
| P3-3 | ⏳ pending | `—` | Shape (S/V/P) + dtype (f64/mask/group) check; lookback propagation; negative-lookback = error. *not blocked.* |
| P3-4 | ⏳ pending | `—` | Hash-consed `Dag` (free CSE); topo linearize → `Instr` stream; slot alloc + refcount `Free`. *not blocked.* |
| P3-5 | ⏳ pending | `—` | `Panel` over `series::Frame`; `SlotPool`; universe/NaN policy; tree-walking oracle. *upstream landed — targets green.* |
| P3-6 | ⏳ pending | `—` | VM dispatch loop + element-wise/logical/`Select` opcodes; zero-alloc; bench ns/cell. *upstream landed — targets green.* |
| P3-7 | ⏳ pending | `—` | `CsRank`/`CsZscore`/`CsScale`/`CsDemeanG`/`CsNeutG`/group; fixed tie-break; valid-mask only. *upstream landed — targets green.* |
| P3-8 | ⏳ pending | `—` | `TsDelay`/`Delta`/`Sum`/`Mean`/`Std`/`Min`/`Max`/`Arg*`/`Rank`/`Corr`/`Cov`/`Product`/`DecayLinear`/…; O(1)/cell rolling; lookahead. *upstream landed — targets green.* |
| P3-9 | ⏳ pending | `—` | Differential (VM==oracle) + determinism-hash + lookahead-truncation + parallel/TSan; bench. Sprint close. |

### P3-4 CSE-lever metrics

_(Fill when the DAG builds on a real alpha battery.)_ The throughput evidence (§3.2/§3.6): how much the
hash-consed DAG collapses a mined-style alpha set.

| Alpha set | total AST nodes | unique DAG nodes | unique/total | peak-live-slots |
|-----------|-----------------|------------------|--------------|-----------------|
| —         | —               | —                | —            | —               |

### P3-6/P3-9 measured throughput

_(Fill when benches run.)_ Record alphas/s and ns/cell with host/build context (CPU, compiler, build type,
panel shape = dates × instruments). Report **measured** numbers only — no invented or third-party figures.

| Config | alphas/s | ns/cell | cache-hit % | Host / build / panel |
|--------|----------|---------|-------------|----------------------|
| element-wise (add/mul) | — | — | — | — |
| mined set (high overlap) | — | — | — | — |

### Deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_ None recorded yet. Expected: Linux/clang TSan pass for the
parallel evaluator; computed-goto dispatch (if profiling warrants); JIT exploration; `indneutralize`
demean-vs-regression edge-case audit vs the actual Alpha101 PDF; `signedpower` vs `x^a`; BRAIN-superset ops.

---

## Phase 3 sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| `ac81776` | marker (P3-0) | 2/2 EngineScaffold / 244/246 total (2 pre-existing failures: atx-core-tests_NOT_BUILT, ShmBarFeed scratch) |
| `94fee4c` | P3-1 | 55/55 AlphaLexer / engine green |
| `—`    | P3-1 | — |
| `—`    | P3-2 | — |
| `—`    | P3-3 | — |
| `—`    | P3-4 | — |
| `—`    | P3-5 | — |
| `—`    | P3-6 | — |
| `—`    | P3-7 | — |
| `—`    | P3-8 | — |
| `—`    | P3-9 + close | — |

**Phase 3 adds `<N>` new tests (total engine footprint: `<K>`/0/0 across `<J>` binaries).** _(Fill at close.)_

---

## What Phase 3 proves / Next sprint priorities

_(Written at close — the baton handoff.)_ Expected statement: *Phase 3 proves the engine can turn a quant
idea written as a string into a deterministic, point-in-time-correct, cross-sectional signal at mass scale —
the fast vectorized VM matches the reference oracle bit-for-bit, identical input yields an identical
signal-hash (single-thread == multi-thread), future data is provably invisible (truncation-invariant), and a
mined alpha set collapses to its unique-subexpression DAG (the weak-signal-breadth lever). Phase 4 screens the
resulting signal stream on correlation/turnover and combines the gated pool into one mega-alpha over a
Barra-style risk model.*
