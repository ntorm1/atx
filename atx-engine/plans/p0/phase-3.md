# Phase 3 — Alpha-Expression DSL + Vectorized VM (user reference)

**Status:** ✅ closed 2026-06-06 · 10 units (P3-0…P3-9) · in-place on `feat/atx-core-stdlib`
**Source plan:** [`phase-3-alpha-expression-dsl-implementation-plan.md`](phase-3-alpha-expression-dsl-implementation-plan.md) ·
**Build log:** [`phase-3-progress.md`](phase-3-progress.md)

Phase 3 turns a quant idea written as a **string** into a deterministic, point-in-time-correct,
cross-sectional **signal** at mass scale. The pipeline is:

```
source string ──lex──► tokens ──parse──► Ast ──analyze──► shape/dtype/lookback
   └─► compile (hash-cons DAG → bytecode Program) ─► evaluate (fast VM | reference oracle) ─► SignalSet
```

All headers are header-only under `atx/engine/alpha/`. The whole layer consumes `atx-core` only.

---

## 1. The DSL

### Grammar (precedence low→high)
```
program     := { assignment }
assignment  := IDENT '=' expr               ; "alpha_001 = <expr>"  (one named alpha per line)
expr        := ternary
ternary     := logic_or [ '?' expr ':' expr ]
logic_or    := logic_and { '||' logic_and }
logic_and   := equality  { '&&' equality }
equality    := comparison { ('=='|'!=') comparison }
comparison  := additive  { ('<'|'>'|'<='|'>=') additive }
additive    := multiplic { ('+'|'-') multiplic }
multiplic   := unary     { ('*'|'/') unary }
unary       := ('-'|'!') unary | power
power       := primary   [ '^' unary ]      ; right-associative
primary     := NUMBER | field | call | '(' expr ')'
field       := IDENT | '$' IDENT            ; close  or  $close  (the '$' Qlib sigil is optional)
call        := IDENT '(' [ expr { ',' expr } ] ')'
```

- **Fields**: `open high low close volume vwap returns cap adv{d}` resolve to panel columns;
  `IndClass.sector` / `.industry` / `.subindustry` are integer **group** classifiers.
- **`$vwap` and `vwap` are the same field** (the `$` sigil is stripped).
- **Assignment names are NOT let-bindings** — each `IDENT = expr` is an independent named alpha; a bare
  identifier always resolves to a panel field, never to an earlier binding. (Cross-alpha reuse comes from
  *subexpression* sharing in the DAG, not named intermediates — see §4.)
- Build-time **constant folding** (`2*3`→`6`, `log(1)`→`0`, `-3`, `2^10`) and **ternary desugaring**
  (`c ? a : b` → a `Select` node) happen in the parser. `pow(x,2)`→`x*x` strength reduction happens in the DAG.

### Operator vocabulary (the opcode ISA)
Shapes: **S** scalar · **V** cross-section (one value/instrument/date) · **P** panel (date×instrument).
All built-ins are lookahead-safe (causal).

| Family | Operators |
|---|---|
| arithmetic | `+ - * /`, unary `-`, `abs sign log`, `power(x,a)`, `signedpower(x,a)`, `min(x,y)` `max(x,y)` |
| logical → mask | `< > <= >= == !=`, `&& ||`, `!x`, `cond ? a : b` |
| cross-sectional (P→V) | `rank zscore scale indneutralize group_neutralize group_rank group_zscore` |
| time-series (P→P, causal) | `delay delta ts_sum ts_mean stddev/ts_std ts_var ts_min ts_max ts_argmin ts_argmax ts_rank correlation covariance product decay_linear ema wma skew kurt med mad slope rsquare resid` |

The full table with arities, shapes, and opcodes is Appendix A of the source plan. Adding an operator is one
registry row + one opcode + one kernel.

---

## 2. API

```cpp
#include "atx/engine/alpha/parser.hpp"     // parse_program / parse_expr, Ast
#include "atx/engine/alpha/registry.hpp"   // Library (the operator catalogue)
#include "atx/engine/alpha/typecheck.hpp"  // analyze
#include "atx/engine/alpha/bytecode.hpp"   // compile, Program
#include "atx/engine/alpha/panel.hpp"      // Panel, SignalSet
#include "atx/engine/alpha/vm.hpp"         // Engine (the fast path)
#include "atx/engine/alpha/oracle.hpp"     // evaluate_reference (the slow reference)

using namespace atx::engine::alpha;

const Library lib;                                            // built-ins registered at construction
ATX_TRY(auto ast,      parse_program(src, lib));              // Result<Ast>
ATX_TRY(auto analysis, analyze(ast));                         // Result<Analysis>  (shape/dtype/lookback; rejects non-causal)
ATX_TRY(auto program,  compile(ast, analysis));               // Result<Program>   (hash-consed DAG → flat bytecode)

Engine engine{panel};                                         // Panel = date×instrument input + universe mask
ATX_TRY(SignalSet signals, engine.evaluate(program));         // the fast vectorized VM
// SignalSet.alphas[i] = { name, values (date-major, NaN where masked/undefined) }
```

- **`Library`** — name→`OpSig` catalogue (arity, opcode, out-dtype, lookahead flag, shape rule). Register
  custom ops via `register_op`.
- **`analyze`** assigns every node a `{shape, dtype, lookback}` and enforces the type lattice + causality rail
  (a window argument must be a compile-time positive-integer literal; negative/zero/non-constant windows are a
  parse-time error — there is no way to express a forward-looking term).
- **`compile`** folds all of a program's alphas into one **hash-consed DAG** (free common-subexpression
  elimination), then linearizes to a flat `Program` (slot-allocated bytecode with refcount-driven `Free`s).
- **`Engine::evaluate`** is the fast, zero-allocation production path. **`evaluate_reference`** is the simple
  tree-walking oracle; the two agree **bit-for-bit** (see §3).
- **`Program::num_slots`** is the working-set size; **`required_lookback`** is the warm-up the panel must
  provide before the first valid signal date.

---

## 3. Guarantees (what Phase 3 proves — see `alpha_proof_test.cpp`)

- **Differential correctness.** `Engine::evaluate` (fast) == `evaluate_reference` (oracle) **cell-by-cell,
  bit-identical**, across an Alpha101-style battery (element-wise + cross-sectional + time-series + nesting).
  The VM is the optimized path; the oracle is the obviously-correct reference.
- **Determinism.** Identical input ⇒ identical signal: a wyhash over the ordered
  `(alpha, date, instrument, value-bits)` stream is stable run-to-run, and *changes* when any input
  (a value, instrument order, or the universe mask) changes (non-vacuous).
- **No look-ahead.** Output at date `t` is byte-identical whether or not dates `> t` exist — future data is
  provably invisible (truncation invariance). Time-series ops read only the trailing window `[t-d+1, t]`.
- **Point-in-time universe.** Out-of-universe / delisted-after instruments read **NaN** (no survivorship,
  no stale values); cross-sectional ops rank/standardize over the valid set only and NaN elsewhere.
- **NaN policy.** Propagate, never impute. Element-wise NaN propagates per IEEE; a trailing window containing
  any NaN (or shorter than its full period) yields NaN.
- **CSE lever.** A mined-style program with heavy subexpression overlap collapses to its unique-node DAG
  (measured ≈3.8× node reduction on a 24-alpha fixture), so shared work is computed once.

### Pinned numeric contract (the fast VM matches these exactly)
`CsRank` = ordinal percentile `r/(n-1)`, ties broken by ascending instrument id, NaNs last, singleton→0.5 ·
`zscore`/`ts_std`/`ts_var`/`skew`/`kurt`/`covariance` use **sample** (ddof=1) · `correlation` is the
population cross-moment ratio (∈[-1,1]) · `scale(x,a)` rescales the valid set to L1 norm `a` ·
`indneutralize` ≡ `group_neutralize` = per-group demean · `ts_ema` α=2/(d+1) seeded on the oldest window
element · `decay_linear` ≡ `wma` (linear weights, newest heaviest) · `ts_argmin/argmax` = 1-based first
extreme · `min/max` yield NaN if either operand is NaN.

---

## 4. How it scales (the mass-alpha lever)

The throughput story is **subexpression sharing**, not faster per-op math. Thousands of mined alpha trees
overlap on terms like `rank(close)` or `ts_mean(close,5)`; `compile` interns structurally-identical
computations into a single DAG node (Zipline-style term memoization, generalized across the whole program),
so each unique subexpression is evaluated once and freed when its last consumer is done. The VM is a
**batch-per-opcode vectorized interpreter** (DuckDB/X100 style): each instruction sweeps a contiguous column
in one zero-allocation loop.

---

## 5. Known residuals (lifted to the ROADMAP backlog)

- **Parallel / multi-thread evaluator** + Linux/clang TSan pass (clang-cl ships no usable TSan; determinism is
  proven single-thread). The VM is the date/instrument-partitionable target.
- **True O(1)/cell incremental rolling** for time-series ops (current kernels recompute each window to stay
  bit-exact with the oracle; an online accumulator would need a documented ULP tolerance).
- **Oracle formula audit vs the Alpha101 PDF** (`ts_rank` average-vs-ordinal, `ema` windowed-vs-running,
  `signedpower` vs `x^a`, `indneutralize` demean-vs-full-WLS-residual), and **per-op `min_periods` promoted
  into `OpSig`** (currently a uniform full-window policy).
- **`scale(x,a=1)` / `group_neutralize(x,g[,cap])` default/optional args** (registry uses fixed arity).
- **Let-bindings** (named-intermediate reuse) and **BRAIN-superset ops** (`ts_entropy`, `ts_backfill`, …),
  each a one-row registry add.
- **JIT / computed-goto dispatch** — the vectorized interpreter is the baseline.

---

## 6. Next sprint (the baton → Phase 4)

Phase 3 stops at *source → signal*. Phase 4 screens the resulting signal stream on
correlation/turnover/fitness gates and combines the gated pool into one mega-alpha over a Barra-style risk
model, plugging into the Phase-2 loop as just another `ISignalSource` (the `VmSignalSource` green-gate). The
`SignalSet` this phase emits is that input.
