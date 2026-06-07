# Phase 3 — Alpha-Expression DSL + Vectorized VM (user reference)

**Status:** ✅ closed 2026-06-06 · 10 units (P3-0…P3-9) · in-place on `feat/atx-core-stdlib`
**Extended by Phase 3d** (state-space & mean-reversion: local bindings, records/`.pin`, Kalman/OU operators) — see
§1.1, [`phase-3d-statespace-dsl-design.md`](phase-3d-statespace-dsl-design.md), [`phase-3d-progress.md`](phase-3d-progress.md).
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
- **Assignment names are local bindings (Phase 3d).** Each `IDENT = expr` is still an independent named
  alpha (a root/output), but a bare identifier now resolves **binding-first, field-fallback**: it reuses an
  *earlier* assignment of that name (sharing its DAG node) if one exists, otherwise it loads the panel field.
  A binding may shadow a panel field of the same name — `parse_program`'s optional `warnings` out-param flags
  it. A forward or self reference (a name used at or before its own definition) falls back to the field.
  Cross-alpha reuse therefore comes from *either* named bindings *or* automatic subexpression sharing (§4).
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
| state-space & mean-reversion (P→P, causal) | `kalman_level(x,Q,R)` · `kalman(y,x,δ,R)`→record `{alpha,beta,resid}` · `ou_filter(x,θ,μ)` · `ou_theta ou_halflife ou_mean ou_zscore` (each `(x,d)`) — see §1.1 |

The full table with arities, shapes, and opcodes is Appendix A of the source plan. Adding an operator is one
registry row + one opcode + one kernel.

### 1.1 State-space, mean-reversion, records & `.pin` (Phase 3d)

Phase 3d adds a state-space / mean-reversion family plus the language machinery a multi-output filter needs:
**records** (a node that emits several named columns) and **`.pin` member access** to project one column.

**Recurrence operators** carry true cross-date state from the first date forward (no trailing window); they are
causal by construction (output at `t` reads only state at `t-1` and the date-`t` inputs). Hyperparameters are
**compile-time literals** (folded into the op's CSE identity), not panel operands.

| Operator | Signature | Math (per instrument, forward scan) | NaN policy |
|---|---|---|---|
| `kalman_level` | `kalman_level(x, Q, R)` → P | scalar local-level (random-walk+noise) filter. Seed `x̂=z, P=R` on first finite obs; predict `P+=Q`; update `K=P/(P+R)`, `x̂+=K·(z−x̂)`, `P=(1−K)·P`. `Q≥0` (process var), `R>0` (obs var). | NaN before the first finite obs; a NaN obs is predict-only (carries `x̂`, `P+=Q`). |
| `kalman` | `kalman(y, x, δ, R)` → **record** `{alpha, beta, resid}` | Chan 2-state time-varying regression of `y` on `x`. Process covariance `W=(δ/(1−δ))·I₂`, diffuse prior `(a=b=0, P=I₂)`. `alpha`=intercept state, `beta`=slope/hedge-ratio state, `resid`=standardized innovation `e/√Q` (the spread). `δ∈(0,1)` strict, `R>0`. | A NaN `y` or `x` is predict-only (`P+=W`); that date's three pins are NaN. |
| `ou_filter` | `ou_filter(x, θ, μ)` → P | OU AR(1) pull-to-mean smoother. `φ=exp(−θ)`. Seed `x̂=x` on first finite obs; then `x̂=μ+φ·(x̂−μ)` (pulls toward `μ`). `θ≥0`, `μ` finite. | NaN before the first finite obs. |

**Rolling OU-fit operators** are windowed time-series ops (same `(series, window)` shape as `slope`/`resid`):
each fits an AR(1) `x[s]=a+b·x[s−1]` by OLS over the trailing window `[t−d+1, t]`, then derives a quantity.
Lookback is `(d−1)+child`; the window `d` is a positive-integer literal.

| Operator | Signature | Value | NaN policy |
|---|---|---|---|
| `ou_theta` | `ou_theta(x, d)` → P | `θ = −ln(b)` (mean-reversion speed) | NaN unless `b∈(0,1)` |
| `ou_halflife` | `ou_halflife(x, d)` → P | `ln2/θ` (half-life) | NaN unless `b∈(0,1)` |
| `ou_mean` | `ou_mean(x, d)` → P | `a/(1−b)` (long-run equilibrium) | NaN unless `b<1` (and `b` finite) |
| `ou_zscore` | `ou_zscore(x, d)` → P | `(x[t]−μ)/σ_eq`, `σ_eq=resid_std/√(1−b²)` | NaN unless `b∈(0,1)` and `σ_eq>0` |

All rolling ops yield NaN on a short or any-NaN window (`d` cells required) or a degenerate fit (`<2` lagged
pairs / zero predictor variance).

**Records & `.pin`.** `kalman(...)` returns a record, which **cannot itself be an alpha output** —
`analyze` rejects a bare record root with *"a record value cannot be an alpha output; project a pin with .pin"*.
Project a named column with member access: `kalman(y, x, δ, R).beta`, `.alpha`, or `.resid`. Two pins of the
**same** call hash-cons to ONE filter scan (compute once, project many) — so `beta = kalman(...).beta` and
`spread = kalman(...).resid` run a single Kalman pass.

**Headline example — Kalman pairs + OU mean-reversion signal:**

```text
beta   = kalman(ret, hedge, 0.0001, 0.001).beta     # time-varying hedge ratio
spread = kalman(ret, hedge, 0.0001, 0.001).resid    # standardized spread (one shared scan via CSE)
hl     = ou_halflife(spread, 20)                     # mean-reversion half-life of the spread
sig    = -ou_zscore(spread, 20)                      # fade the spread: short rich / long cheap
```

`beta`, `spread`, `hl`, `sig` are four alpha outputs; the two `kalman(...)` calls share one scan. Because
assignment names are local bindings (above), `spread` is computed once and reused by both `hl` and `sig`.

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

---

# Phase 3b / 3c addendum — BRAIN-superset surface · batch API · streams · loop bridge

Phase 3b widened the operator vocabulary to a WorldQuant-BRAIN superset and added variadic/default args; Phase
3c added the mass-evaluation batch convenience, the per-alpha PnL/position stream extraction, and the
`VmSignalSource` bridge that finally drives the real Phase-2 `BacktestLoop` from a compiled alpha. The §1–§6
contract above is unchanged; this addendum documents only the new surface. Measured CSE / throughput numbers
live in the build ledgers ([`phase-3b-progress.md`](phase-3b-progress.md), [`phase-3c-progress.md`](phase-3c-progress.md)) — not duplicated here.

## 7. BRAIN-superset operators (3b)

All are causal (lookahead-safe), full-window `min_periods` (except `ts_backfill`), and NaN-propagating per the
§3 policy. Each is one registry row + one opcode + one kernel.

| Family | New operators |
|---|---|
| element-wise activations (P→P) | `sigmoid(x)` = 1/(1+e⁻ˣ) · `tanh(x)` |
| cross-sectional (P→V) | `normalize(x)` = cross-sectional demean · `winsorize(x, std=4)` clamp to mean ± std·σ (σ sample, ddof=1) |
| group aggregates (P→V) | `group_count(x, g)` · `group_mean(x, g)` · `group_scale(x, g)` — `g` is a `Group` classifier; each broadcasts a within-group aggregate over its members (NaN-fill excluded) |
| rolling time-series (P→P) | `ts_zscore(x,d)` · `ts_backfill(x,d)` (looks PAST NaNs to the most recent valid value in `[t-d+1,t]`) · `ts_av_diff(x,d)` · `ts_quantile(x,d)` · `ts_scale(x,d)` · `ts_count_nans(x,d)` |
| stateful recurrence (P→P, causal) | `trade_when(trigger, alpha, exit)` — `trigger`/`exit` are masks, `alpha` is F64; a forward scan seeds at the first date and reads only prior state + inputs ≤ t · `hump(x, threshold=0.01)` |

### Variadic / default arguments (3b)
A few built-ins now carry an optional trailing argument with a finite default (filled at parse time as a
`Const`, so `scale(x)` and `scale(x, 1)` compile to the same DAG):

| Call | Default | Meaning |
|---|---|---|
| `scale(x)` | 2nd arg = `1.0` | rescale the valid cross-section to target L1 norm |
| `winsorize(x)` | `std = 4.0` | clamp to mean ± 4σ |
| `hump(x)` | `threshold = 0.01` | dampen position changes below the threshold |

### Locked semantics (pinned; the fast VM matches these exactly)
- `indneutralize(x, g)` ≡ `group_neutralize` = per-group **demean** (NOT a full WLS residual).
- `signedpower(x, a)` = `sign(x)·|x|^a`.
- **Window convention is `floor(d)`** — a window literal is floored to an integer; a value that floors to `< 1`
  (e.g. `0.5`, `-3`) or exceeds `u16::max` is a parse-time error (the causality rail: no forward-looking term).
- **`min_periods` = full window** for every rolling op (an incomplete or NaN-containing trailing window yields
  NaN); `ts_backfill` is the sole exception (it skips PAST NaNs within its window).

## 8. Batch API — `compile_batch` (3c)

Multi-assignment programs already compile to one cross-alpha-CSE DAG (§4); `compile_batch` is the thin
convenience that takes N standalone expression strings (auto-named `a0…aN`, joined one-per-line) and returns
the single `Program`:

```cpp
#include "atx/engine/alpha/bytecode.hpp"   // compile_batch, Program

const Library lib;
std::vector<std::string_view> srcs{"rank(close)", "ts_mean(close, 5) - close", "rank(close) + 1"};
ATX_TRY(Program prog, compile_batch(std::span<const std::string_view>{srcs}, lib));
// prog.roots[i] <-> srcs[i] (1:1, submission order); a malformed entry -> Err (never throws).
```

`Program` carries the cross-alpha **CSE telemetry**: `unique_nodes` / `total_ast_nodes` (the node-fold ratio),
`cache_hits` / `intern_attempts`, and `cache_hit_pct()` (the share of lowered nodes deduplicated). Invariant:
`intern_attempts == cache_hits + unique_nodes`. `Engine::evaluate(prog)` then returns a `SignalSet` with one
`alpha` per root. Result is a function of the alpha **set**, not submission order (a by-name-sorted digest is
replay-stable across orders).

## 9. Per-alpha streams — `extract_streams` / `AlphaStreams` (3c)

The typed Phase-3 → Phase-4 handoff. Phase 4 consumes, per alpha, a realized-PnL stream and the position
(target-weight) stream that produced it — NOT raw signals. `extract_streams` builds those by **reusing** the
Phase-2 `WeightPolicy` (positions) and one `ExecutionSimulator` coefficient (turnover cost) — no new portfolio
logic:

```cpp
#include "atx/engine/alpha/streams.hpp"    // extract_streams, AlphaStreams

ATX_TRY(SignalSet signals, engine.evaluate(prog));
ATX_TRY(AlphaStreams streams,
        extract_streams(signals, weight_policy, panel, exec_sim));  // panel == the eval panel
std::span<const f64> pnl = streams.pnl(0);            // [n_periods]; pnl[0]==0 (no prior weight)
std::span<const f64> w   = streams.positions(0, t);   // [n_instruments] target weights at period t
```

- `positions[t]` = `WeightPolicy::to_target_weights(signal_row(t), universe)` (winsorize → rank/zscore →
  dollar-neutral → gross-scale) — the same construction the loop applies each rebalance.
- `pnl[t]` = `Σⱼ wⱼ[t-1]·retⱼ[t] − turnover[t]·cost_rate` (no look-ahead: prior weights earn this period's
  return; `pnl[0]=0`). `cost_rate` = the sim's `PerDollar per_dollar_bps / 1e4` (0 for a frictionless sim →
  the pure analytic Σw·ret stream). Shape mismatch (SignalSet vs panel) or a missing `close` field → `Err`.

## 10. Loop bridge — `VmSignalSource` + the `Delay` knob (3c)

`VmSignalSource` adapts a compiled `alpha::Program` to the Phase-2 `ISignalSource` seam, so the SAME
`BacktestLoop` runs the alpha VM with no loop change:

```cpp
#include "atx/engine/loop/signal_source.hpp"   // VmSignalSource
#include "atx/engine/loop/types.hpp"           // Delay

VmSignalSource src{std::move(prog)};                 // root 0 == the traded alpha
// ... feed/clock/bus/panel/policy/sim/portfolio/market/universe/schedule as usual ...
BacktestLoop<Cap> loop{feed, clock, bus, panel, src, policy, sim,
                       portfolio, market, Universe{universe}, Schedule{every},
                       Delay::Next};                  // trailing arg; defaults to Next
BacktestResult result = loop.run();
```

Each `evaluate(PanelView)` transposes the loop's newest-first trailing window into a chronological
`alpha::Panel` (the row reversal is load-bearing — every `ts_*` op reads a causal `[t-d+1, t]` window), runs the
VM, and returns root 0's current-date (newest) cross-section. `max_lookback()` forwards
`Program::required_lookback`, so the loop sizes its `RollingPanel` to the program.

**`Delay`** is an execution-timing knob (NOT part of the expression — the same program runs under either value
and reads the same panel; only the fill bar differs):
- `Delay::Next` (delay-1, **default**) — the conservative no-look-ahead firewall: an order decided on bar `t`
  fills no earlier than a strictly-later slice.
- `Delay::Same` (delay-0, opt-in) — lets an order fill on the same bar's close it was decided on (mildly
  optimistic; the signal still reads only sealed ≤ t rows).
