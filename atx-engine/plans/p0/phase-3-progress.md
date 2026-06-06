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

**(3) Assignment names are NOT let-bindings (decided at P3-4).** The grammar `program := { assignment }`
(`IDENT '=' expr`) names **independent** alphas; a later assignment **cannot** reference an earlier one by
name. A bare identifier always resolves to a panel field — so `x = ts_mean(close,5)` `alpha = rank(x)` treats
the `x` in `rank(x)` as a *field* named `x`, not a back-reference to the binding (it will fail field lookup
at P3-5, not silently mis-evaluate, since the panel has no `x` column). This is deliberate: the CSE throughput
lever (§3.2) comes from **subexpression** overlap across the mined alpha population (`rank(close)` repeated in
thousands of trees), which the hash-cons captures regardless — named-intermediate reuse is not required for
it. The plan's P3-4 diamond illustration (`m=…; a=m+1; b=m*2`) assumed let-binding; the equivalent test uses a
genuine shared **expression** instead, which exercises the same diamond/refcount machinery. **Deferred:**
let-binding (a parser pre-pass substituting prior roots, or a scope table) → future-work if a real alpha needs
named intermediates.

Realistic scope for this sprint:

1. **P3-0** — Module scaffold + CMake + ledger (marker). Open ledger, freeze scope.
2. **P3-1** — Lexer (tokens + spans). *not blocked.*
3. **P3-2** — Pratt parser → AST + operator registry (`Library`); const-fold + desugar. *not blocked.*
4. **P3-3** — Shape/dtype check + lookback analysis (causality rail). *not blocked.*
5. **P3-4** — Hash-consed expression DAG (free CSE) + bytecode linearization + slot/refcount. *not blocked.*
6. **P3-5** — Columnar eval context (Panel/SlotPool) + tree-walking reference oracle. *upstream landed — targets green.*
7. **P3-6** — Vectorized VM core + element-wise/logical/select opcodes. *upstream landed — targets green.*
8. **P3-7** — Cross-sectional opcodes (`rank`/`zscore`/`scale`/(group_)neutralize). *upstream landed — targets green.*
9. **P3-8** — Time-series opcodes (`delay`/`delta`/`ts_*`/`correlation`/`decay_linear`/…). *upstream landed — targets green.*
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
| P3-2 | ✅ done | `f556e23` | Pratt parser → arena `Expr`/`Ast` (precedence ternary<\|\|<&&<eq<cmp<+-<*/<unary<^ right-assoc); `Library` registry (`OpCode` ISA, `Shape`, `DType`, `OpSig`, table-driven `shape_of`) with all Appendix A built-ins; const-fold (pure numeric subtrees + foldable unary fns) + ternary→`Select` desugar; arity checked at parse. Header-only (`inline`). 59 tests. *not blocked.* |
| P3-3 | ✅ done | `96045e9` | `analyze(Ast)→Result<Analysis>`: per-node `TypeInfo{shape,dtype,lookback}`. Table-driven shapes (broadcast-max / P→V / P→P; `Select` widens over cond too); dtype rails (cmp/logic→Mask, group-ops need `Group` arg2, `Select` needs Mask cond, arithmetic F64, `IndClass.*`→Group); lookback = shift(`delay`/`delta`)+d vs rolling+`(d-1)`, max over children (`ts_mean(delta(close,5),10)`⇒14); window must be folded positive-int literal (non-const/≤0/non-int rejected — no-look-ahead rail); scalar-primary into Cs*/Ts*→error. Single forward pass (topo arena), no recursion. Header-only. 36 tests. *not blocked.* |
| P3-4 | ✅ done | `b2fe473` | `build_dag(Ast,Analysis)→Dag`: hash-cons all roots into one DAG (free CSE via `NodeKey{op,param,children}` cons-table, `hash_combine`); `pow(x,2)→Mul(x,x)` strength reduction; refcount counted **once per unique DAG edge** (CSE-miss only — else a duplicate AST occurrence leaks the shared leaf's slot). `linearize(Dag)→Program`: topo emit, `SlotPool` recycle, refcount-driven `Free` after last consumer, one `StoreAlpha` **per root** (two identical alphas → one node, two stores). `compile=build_dag∘linearize`. Header-only. 36 tests (18 dag + 18 bytecode). *not blocked.* |
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
| `a=ts_mean(close,5)+1` / `b=ts_mean(close,5)*2` (shared subtree) | 10 | 7 | 0.70 | 3 |
| `ts_mean(ts_mean(ts_mean(close,3),3),3)` (deep chain) | 7 | 5 | 0.71 | 3 |
| `a=rank(close)` / `b=rank(close)` (identical alphas) | 4 | 2 | 0.50 | 2 |

_(Unit-test fixtures, not a mined battery — these pin the CSE+liveness machinery; the §3.6 mass-scale numbers come once P3-9 runs a real alpha set.)_

### P3-6/P3-9 measured throughput

_(Fill when benches run.)_ Record alphas/s and ns/cell with host/build context (CPU, compiler, build type,
panel shape = dates × instruments). Report **measured** numbers only — no invented or third-party figures.

| Config | alphas/s | ns/cell | cache-hit % | Host / build / panel |
|--------|----------|---------|-------------|----------------------|
| element-wise (add/mul) | — | — | — | — |
| mined set (high overlap) | — | — | — | — |

### Deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_ Expected: Linux/clang TSan pass for the
parallel evaluator; computed-goto dispatch (if profiling warrants); JIT exploration; `indneutralize`
demean-vs-regression edge-case audit vs the actual Alpha101 PDF; `signedpower` vs `x^a`; BRAIN-superset ops.

- **(P3-2) Optional/default args not modeled.** Appendix A lists `scale(x,a=1)` (default 2nd arg) and
  `group_neutralize(x,g[,cap])` (optional 3rd). The registry uses **fixed** arity (`scale`=2,
  `group_neutralize`=2), so `scale(x)` and the 3-arg `group_neutralize` form do not parse. Variadic / default-arg
  support is deferred — revisit when a mined alpha actually needs the short form (one `OpSig` knob or a min/max
  arity pair).

---

## Phase 3 sprint commits

| Commit | Unit | Test counts (suite/total/fail/skip) |
|--------|------|-------------------------------------|
| `ac81776` | marker (P3-0) | 2/2 EngineScaffold / 244/246 total (2 pre-existing failures: atx-core-tests_NOT_BUILT, ShmBarFeed scratch) |
| `94fee4c` | P3-1 | 55/55 AlphaLexer / engine green |
| `f556e23` | P3-2 | 59 new (registry + parser) / engine 114/114 green |
| `96045e9` | P3-3 | 36 new (AlphaTypecheck) / engine 393/395 (2 pre-existing baseline fails) |
| `b2fe473` | P3-4 | 36 new (18 AlphaDag + 18 AlphaBytecode) / engine 431/432 (only `atx-core-tests_NOT_BUILT`) |
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
