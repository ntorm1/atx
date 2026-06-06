# Phase 3 — Implementation Progress

**Worktree:** none (direct on feat/atx-core-stdlib)
**Branch:** `feat/atx-core-stdlib`
**Base:** feat/atx-core-stdlib @ `e89048b`
**Started:** `2026-06-05` · **Closed:** `2026-06-06` (10 units P3-0…P3-9, ~264 alpha-DSL tests, all green)
**Source plan:** [`phase-3-alpha-expression-dsl-implementation-plan.md`](phase-3-alpha-expression-dsl-implementation-plan.md)
**Prior progress:** Phase 1 (event spine) — see [`phase-1-progress.md`](phase-1-progress.md). Phase 2
(backtest loop) precedes this in the ROADMAP but Phase 3's front-end (P3-1..P3-4) is independent of it.

> **Ledger state:** ✅ **CLOSED** — all 10 units (P3-0…P3-9) landed on `feat/atx-core-stdlib`; see the per-unit
> ledger and *What Phase 3 proves* below. Scope changes are recorded in *Plan adjustments* below, **not** in the
> frozen plan file.

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

**(5) VM eval model = full-buffer columnar, not the date-loop-outer sketch (decided at P3-6).** The plan §P3-6
sketches `for (date t) for (instr) …` with cross-section-sized slots. The VM (and the P3-5 oracle) instead use
the **batch-per-opcode full-buffer** model: each slot is a whole `dates×instruments` f64 buffer and each
instruction sweeps it once in a contiguous loop. Rationale: it is the canonical vectorized-interpreter shape
(DuckDB/Vectorwise-X100, Appendix B), it shares the oracle's exact layout so the P3-9 differential is robust
(both index identically — any mismatch is a real numeric bug, not a reshape artifact), and the P3-7/P3-8
cross-sectional/time-series kernels plug in with the full panel materialized (no ring-buffered rolling state).
Peak-live-slots is small (3–5), so the `num_slots×dates×instruments` working set stays bounded. **Deferred:**
the date-loop-outer + incremental rolling optimization (lower memory at mass scale) is future-work if a real
panel × alpha-batch exceeds memory.

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
| P3-5 | ✅ done | `1ec22ca` | `Panel` (self-contained date-major f64 + PIT universe mask — **not** a `series::Frame` view, see adjustment 4), `SlotPool` (pre-sized buffers, over-acquire = `ATX_ASSERT`), full `SignalSet`. `evaluate_reference(Program,Panel)`: full-buffer model (each slot a whole date×inst buffer), one exhaustive no-`default` `OpCode` switch → simple per-op kernels. **Pinned differential contract** (VM must match bit-for-bit): NaN-propagate min/max, masks 1/0, full-window min_periods (any-NaN window→NaN; delay/delta shift-only), CsRank ordinal tie-break by instrument id, sample-ddof std/var/zscore, CsNeutG≡demean. Universe folds to NaN at LoadField, propagates everywhere. Header-only. 27 tests. *upstream landed — green.* |
| P3-6 | ✅ done | `1078249` | `Engine::evaluate(Program)→SignalSet`: full-buffer columnar VM (batch-per-opcode, deviation 5). Element-wise (`+−×÷`/pow/spow/min/max), unary (neg/abs/sign/log), cmp→mask, and/or/not, `Select`, LoadField (PIT→NaN), Const — re-stating the oracle's pinned scalar semantics independently (differential proves bit-parity). Cs*/Ts*→`Err(NotImplemented)` (P3-7/8). Exhaustive no-`default` switch. Zero-alloc dispatch loop (pool + remap scratch reused; warm `evaluate` allocs only the output) — CRT-alloc-hook guard. Header-only. 11 tests. *upstream landed — green.* |
| P3-7 | ✅ done | `bf02154` | 7 cross-sectional kernels (`cs_ops.hpp`) wired into the VM (surgical vm.hpp stub→`eval_cross_section`). Per date-row over the valid (non-NaN) set; matches the oracle bit-for-bit: ordinal `CsRank` r/(n-1) tie-break by instrument id (singleton→0.5), sample-ddof `CsZscore`, `CsScale` L1→a, `CsDemeanG`≡`CsNeutG` per-group demean, within-group rank/zscore. **L6 `stats::cross_section` deliberately NOT used** (its average-tie/population semantics break the plan §3.5 ordinal-by-id determinism rail). Header-only. 15 tests (differential + known-value + tie-storm/NaN/single-member boundaries). *upstream landed — green.* |
| P3-8 | ✅ done | `7b63c35` | 24 time-series kernels (`ts_ops.hpp`) wired into the VM (surgical vm.hpp stub→`eval_time_series` + reusable scratch). Causal trailing window `[t−d+1,t]` walked strided down the date-major slot. Matches oracle **bit-for-bit** via identical chronological summation order (NOT incremental — that would reorder FP ops); zero per-cell alloc (scratch grown once for med/rank/corr/cov). Full-window min_periods (any-NaN→NaN; delay/delta shift-only), sample-ddof var/std/skew/kurt/cov, population corr∈[-1,1], first-extreme arg*, oldest-seeded ema, `decay_linear`≡`wma`. **True O(1)/cell rolling DEFERRED** (needs ULP-tolerant differential). Header-only. 13 tests (differential×2 windows + known-value + causality-truncation rail + NaN/boundary). *upstream landed — green.* |
| P3-9 | ✅ done | `e303355` | `alpha_proof_test.cpp`: differential (fast VM == oracle, bit-identical over a 7-alpha Alpha101-style battery on a panel with delisted symbols + NaN gaps); determinism (wyhash over ordered `(alpha,date,inst,bits)` — repeat-run identical, FAST==ORACLE, mutation-sensitive); no-look-ahead (truncation invariance). Bench (informational). **Parallel evaluator + Linux TSan DEFERRED** (residual; determinism proven single-thread). 4 tests. Sprint close = `phase-3.md` + this ledger + git history. |

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
| element-wise add | — | ~32.8 | — | clang-cl **Debug** (`/Od`); 1024×512 panel (524288 cells) |
| element-wise mul | — | ~32.8 | — | clang-cl **Debug** (`/Od`); 1024×512 panel |
| select | — | ~79.8 | — | clang-cl **Debug** (`/Od`); 1024×512 panel |
| mined set (24 alphas, high overlap) | ~81.6 | ~93.5 | DAG dedup 41/156 = **0.26** (≈3.8×) | clang-cl **Debug**; 512×256 panel, num_slots 12 |

> ⚠️ **Debug-build upper bounds**, not the §3.6 ≤1–2 ns/cell budget (that targets a `/O2` release build). Re-measure under Release at P3-9 before drawing throughput conclusions.

### Deferred residuals

_(Lift to ROADMAP future-work backlog at close.)_ Expected: Linux/clang TSan pass for the
parallel evaluator; computed-goto dispatch (if profiling warrants); JIT exploration; `indneutralize`
demean-vs-regression edge-case audit vs the actual Alpha101 PDF; `signedpower` vs `x^a`; BRAIN-superset ops.

- **(P3-8) True O(1)/cell incremental rolling deferred.** The VM's `Ts*` kernels recompute each trailing
  window O(d)/cell in the oracle's exact summation order to stay **bit-for-bit** with the differential oracle
  (zero per-cell alloc, so still far faster than the oracle). An incremental/online accumulator (the §3.6
  O(1)/cell budget) reorders FP ops → would need a documented ULP tolerance in the differential. Promote when
  the §3.6 throughput budget is pursued (pairs with the L6 `rolling`/`online_stats` kernels).
- **(P3-5) Oracle numeric choices to audit vs Alpha101/Qlib at sprint close.** The oracle PINS a self-consistent
  contract (the VM matches the oracle, not vice-versa), but a few formulas are engine choices worth confirming
  against the Alpha101 PDF before Phase 4 trusts the signals: `ts_rank` uses average-rank/(n−1) (vs CsRank's
  ordinal); `ts_ema` is a windowed EMA seeded on the oldest obs (not a running EMA over all history); `wma` is
  aliased to `decay_linear` (linear weights, newest heaviest); `ts_skew`/`ts_kurt` use population moments with
  sample-sd / excess kurtosis; regression ops use a 0..n−1 time axis; `CsNeutG`≡`CsDemeanG` (no extra
  regressors). `min_periods` is uniform full-window (registry has no per-op field yet → **promote into `OpSig`**).
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
| `1ec22ca` | P3-5 | 27 new (AlphaPanel + AlphaOracle) / engine 464/465 (only `atx-core-tests_NOT_BUILT`) |
| `1078249` | P3-6 | 11 new (AlphaVm differential) / engine 475/476 (only `atx-core-tests_NOT_BUILT`) |
| `bf02154` | P3-7 | 15 new (AlphaCs differential) / engine 490/491 (only `atx-core-tests_NOT_BUILT`) |
| `7b63c35` | P3-8 | 13 new (AlphaTs differential) / engine 503/504 (only `atx-core-tests_NOT_BUILT`) |
| `e303355` | P3-9 | 4 new (AlphaProof: differential + determinism + lookahead + bench) / engine 507/508 (only `atx-core-tests_NOT_BUILT`) |
| _(this)_  | P3-9 close | `phase-3.md` user reference + ledger finalization (docs only) |

**Phase 3 adds ~264 alpha-DSL tests** across 10 units (AlphaLexer 55 · AlphaRegistry/Parser/Program 59 ·
AlphaTypecheck 36 · AlphaDag/Bytecode 38 · AlphaPanel/Oracle/SlotPool/SignalSet 32 · AlphaVm 11 · AlphaCs 15 ·
AlphaTs 13 · AlphaProof 4 · + the P3-0 scaffold smoke test). **The `atx-engine-tests` binary now runs 409
registered tests, all green** except the single `atx-core-tests_NOT_BUILT` registration (atx-core's own suite
is not built into this target — a pre-existing, non-Phase-3 baseline). Every Phase-3 unit is `/W4 /permissive-
/WX` clean and clang-format clean.

---

## What Phase 3 proves / Next sprint priorities

**Phase 3 proves the engine can turn a quant idea written as a string into a deterministic,
point-in-time-correct, cross-sectional signal at mass scale.** The fast vectorized VM matches the reference
oracle **bit-for-bit** across an Alpha101-style battery; identical input yields an identical signal-hash
run-to-run (and the hash is mutation-sensitive — not vacuous); future data is provably invisible
(truncation-invariant); out-of-universe/delisted instruments read NaN (no survivorship); and a mined alpha set
collapses to its unique-subexpression DAG (≈3.8× node dedup on the fixture — the weak-signal-breadth lever).
The lexer→parser→typecheck→DAG→bytecode→VM pipeline is complete and `/W4 /WX`-clean; `Engine`/`Program`/
`SignalSet` are documented in [`phase-3.md`](phase-3.md).

**Deferred (lifted to the ROADMAP future-work backlog):** the parallel/multi-thread evaluator + Linux/clang
TSan pass (determinism is proven single-thread); true O(1)/cell incremental rolling (needs a ULP-tolerant
differential); the Alpha101 oracle-formula audit + per-op `min_periods` into `OpSig`; default/optional op args;
let-bindings; BRAIN-superset ops; JIT/computed-goto. None block Phase 4.

**Baton → Phase 4:** Phase 3 stops at *source → signal*. Phase 4 screens the `SignalSet` stream on
correlation/turnover/fitness gates and combines the gated pool into one mega-alpha over a Barra-style risk
model, plugging into the Phase-2 loop as the `VmSignalSource` (the one cross-phase green-gate Phase 2 froze).

> **Close note — ROADMAP not flipped here.** `ROADMAP.md` carries concurrent **uncommitted** Phase-4 scoping
> edits (a parallel effort on this shared branch). To avoid entangling diffs, the ROADMAP Phase-3 status flip
> (`⏳ not built` → `✅ e303355`, Phase-1/2 deps row, `Last reviewed` bump) is left for that commit. Phase 3's
> close is fully captured by this ledger, [`phase-3.md`](phase-3.md), and the 10 `feat(p3-*)` commits.
