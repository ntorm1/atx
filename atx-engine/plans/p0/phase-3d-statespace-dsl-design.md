# Phase 3d — State-Space & Mean-Reversion DSL Nodes (with Named Multi-Output)

**Status:** DESIGN (brainstorming output) — awaiting user review before `writing-plans`.
**Date:** 2026-06-07
**Branch / worktree:** `feat/alpha-statespace-nodes` @ `C:/Users/natha/atx-wt/alpha-statespace-nodes` (off `feat/atx-core-stdlib`).
**Authority:** `.agents/cpp/agent.md` (safety-critical C++20) + `.agents/atx-engine/agent.md` (build/test, landmines).

---

## 1. Summary

Add a family of state-space / mean-reversion operators to the alpha-expression DSL, and the
**language machinery** they need: local `let`-bindings, a record type, member-access (`.pin`)
syntax, and a genuine **multi-output IR** (SSA-style compute node + projection pins).

Headline capability the user asked for:

```text
kf    = kalman(returns, hedge, 1e-4, 1e-3)   # 2-state Chan regression, record-valued
beta  = kf.beta                               # time-varying hedge ratio   (alpha root)
spread= kf.resid                              # standardized innovation     (alpha root)
hl    = ou_halflife(spread, 60)               # rolling OU half-life        (alpha root)
sig   = -ou_zscore(spread, 60)                # mean-reversion signal        (alpha root)
```

`kalman(...)` runs the filter **once** (CSE-shared compute node); `.beta`, `.alpha`, `.resid`
are zero-recompute projections.

### Goals
- Multi-output operators are first-class in the IR (not a recompute hack).
- Named pins via member access on record-valued nodes.
- Local bindings so a node can be named once and referenced/pinned multiple times.
- Kalman (scalar local-level + Chan 2-state regression) and OU (recurrence filter + rolling-fit
  family) operators.
- Every new op: fast VM kernel **and** an independent oracle reference, proven bit-for-bit equal
  by the differential harness (NaN==NaN), plus known-value tests vs a Python/numpy reference.

### Non-goals (YAGNI for this phase)
- N-dimensional (>2 state) Kalman; user-defined record types; pins of differing shapes.
- Adaptive/EM hyperparameter learning — all filter hyperparameters are **compile-time constants**.
- Cross-instrument Kalman (pairs across the universe): every recurrence is **per instrument
  column** (instrument `j` regresses its own `y_j` on `x_j` over time), consistent with the
  existing per-instrument forward-scan model.

---

## 2. Background — current architecture (what we build on)

- **Pipeline:** `lex → parse_program (Pratt) → analyze (typecheck/lookback) → build_dag
  (hash-cons CSE) → linearize (slot alloc + Free) → Engine::evaluate (VM) ‖ evaluate_reference
  (oracle)`.
- **One value = one node = one slot.** `Instr{op, dst, src[3], param:u32, imm:f64}`. `SlotPool`
  hands out uniform-width columns (`cells = dates*instruments`). `StoreAlpha` copies a slot → a
  named alpha in the `SignalSet`.
- **Stateful recurrence** (`trade_when`, `hump`) bypasses the windowed Ts path: a per-instrument
  forward scan holds **one** `f64` of state per instrument in `Engine::state_[j]` (grown once,
  reused). VM (`vm.hpp::eval_recurrence`) and oracle (`oracle.hpp::eval_recurrence`) are
  independent re-statements; the differential test pins them equal.
- **Windowed rolling-OLS already exists:** `slope`, `resid`, `rsquare` regress a series on a time
  index over a trailing window — the exact shape an OU AR(1) fit needs (regress on lagged-self).
- **Bare `IDENT`** in an expression → `Expr::Kind::Field` (panel `LoadField`). Each `IDENT = expr`
  is an **independent alpha root**. There is **no** symbol table / variable reference today.
- **Arity wall:** `Expr.a/b/c`, `Node.in[3]`, `NodeKey.children[3]`, `Instr.src[3]` are all size 3.
  Max existing operand arity is 3 (`correlation`, `trade_when`, `select`).

---

## 3. Language features

### 3.1 Local bindings + references (Phase A)

**Grammar unchanged** (`program := { IDENT '=' expr }`). New *semantics*: each assignment enters a
symbol table in declaration order. Inside an expression, bare `IDENT` resolves:

1. **binding-first** — if `IDENT` matches an earlier binding, the reference reuses that binding's
   `ExprId` (the DAG hash-cons dedups, so a referenced binding is computed once);
2. **field-fallback** — otherwise it is a panel `Field` (`LoadField`), exactly as today.

- **Shadowing policy (decided):** a binding shadows a same-named field, and the parser emits a
  **warning** (collected into a `std::vector<Diagnostic>` carried out of the parse; non-fatal).
  Rationale: maximal ergonomics, visible accidents. (The parser has no panel field list, so the
  warning fires whenever a *binding name is later used as a bare identifier that could also be a
  field*; precise "is this a real field" detection is out of scope — the warning is advisory.)
- **Backward compatible:** existing programs have no binding/field collisions and no `.`; bare
  identifiers still load fields; every assignment still becomes a root (see output rule).

**Output rule (decided, backward compatible):** a binding whose value is a *single signal*
(`Scalar`/`CrossSection`/`Panel`, dtype `F64`/`Mask`) becomes an alpha root in declaration order —
identical to today. A **record-valued** binding (e.g. `kf = kalman(...)`) is an *intermediate
only*: never a root. Pinning it (`kf.beta`) yields a single signal which, if bound, becomes a root.
A record binding never referenced by any `.pin` is dead (CSE drops it); emit an advisory warning.

**Implementation:** `Parser` gains `std::vector<Binding{std::string name; ExprId id; bool record;}>`
plus the `Diagnostic` sink. `parse_prefix`'s `Ident` arm checks the binding table before creating a
`Field`. No new `Expr::Kind` for scalar references (reuse the bound `ExprId`).

### 3.2 Records + member access (Phase B)

- **Lexer:** add `TokenKind::Dot` (`.`). One-char token; trivial lexer addition. `Dot` is not an
  infix arithmetic op — it is a **postfix** member accessor with the highest binding power.
- **AST:** add `Expr::Kind::Member { a = record expr, name_id = pin name }`.
- **Parser:** postfix `.IDENT` parselet in `parse_precedence` (bind power above `^`). Applies to any
  primary; type-checking validates the LHS is record-valued and the pin name exists.
- **Type lattice:** `TypeInfo` gains a record representation. Minimal form: a `bool is_record` plus a
  small `RecordInfo` (a `std::span<const PinSig>` borrowed from the op's registry row — pins are
  statically known per multi-output op, so no per-node ownership needed). Non-record nodes are
  unchanged (`is_record=false`).
- **`analyze_member`:** LHS must be a record; resolve `name_id` against the record's pin list; result
  `TypeInfo` = that pin's `{shape, dtype, lookback}` (lookback inherited from the compute node).
  Unknown pin name → `Err(InvalidArgument, "no pin 'X' on record")`. Applying `.pin` to a
  non-record → error. Using a record value anywhere a single signal is required (arithmetic operand,
  alpha root, Ts/Cs operand) → error ("record value must be projected with .pin").

### 3.3 Multi-output IR (Phase B)

SSA projection model — a multi-result instruction plus `Pin` extracts:

- **`Node.n_out` (`atx::u8`, default 1).** Existing ops untouched. A multi-output compute node sets
  `n_out = K`.
- **Slot blocks.** A `K`-output node reserves **K contiguous slots**; the kernel writes output
  columns `[dst, dst+K)` (each a `cells`-wide region, contiguous in the pool's backing store).
  `SlotPool` gains `acquire_block(k)` / `release_block(first, k)` with a per-size free-list; `peak()`
  still bounds VM pool capacity. `Instr.dst` holds the first slot of the block; `Instr` carries
  `n_out` so `Free`/liveness release the whole block.
- **`OpCode::Pin`.** `Pin(compute, k)`: `param = k`; `src[0] = compute_first_slot`. Kernel copies
  column `src[0] + k` → its own (single) `dst`. Cheap (`cells` copy) vs the filter scan. Downstream
  consumers see a normal single-output value — **no other VM/linearizer code changes** for pins.
- **Liveness.** The compute block's refcount = number of distinct `Pin` consumers (+ any direct
  consumer, though records are never used directly). `linearize` emits the compute, then each `Pin`;
  the block is `Free`d after the last pin retires. Hash-consing makes `kf.beta` and `kf.resid`
  reference the *same* compute node ⇒ one scan.
- **NodeKey & immediates.** Filter hyperparameters are **immediates**, not operands (keeps operand
  arity ≤ 3 even for the 2-panel dyn-Kalman). Concretely:
  - `Instr.imm` becomes `std::array<atx::f64,2> imm` (Const uses `imm[0]`).
  - `Node` gains `std::array<atx::f64,2> hparams`.
  - `NodeKey` gains `std::array<atx::u64,2> imm_bits` (bit_cast of the two hparams), folded into
    `operator==` and the hash so distinct hyperparameters do **not** wrongly CSE. `Pin` nodes key on
    `{op=Pin, param=k, children={compute}}`.

### 3.4 Registry extensions (Phase B)

`OpSig` gains:
- `atx::u8 n_hparams{0}` — count of **trailing** call arguments parsed as constant-literal
  hyperparameters (peeled into `Node.hparams`, validated by `analyze` to be finite folded literals
  in range). The remaining leading args are operand children. (Orthogonal to `defaults`: defaults
  still fill omitted trailing args first; hparam-peeling happens after default-fill.)
- `std::span<const PinSig> pins{}` — empty for single-output ops; for a record op, the ordered pin
  table `PinSig{std::string_view name; DType dtype;}` (all pins share the op's output shape rule).
  `n_out = pins.size()` when non-empty.

`register_op` validates: `n_hparams ≤ arity`; a record op has `pins` non-empty and `n_out ≥ 2`.

---

## 4. Node catalog & math

All hyperparameters (`Q, R, delta, theta, mu`) are **compile-time constant literals**, validated
like Ts windows (finite; op-specific range checks). NaN/seed policy is **pinned** here and restated
independently in VM and oracle; the differential test enforces bit-equality.

### 4.1 `kalman_level(z, Q, R)` — scalar local-level (Phase C, single output, recurrence)

1-D random-walk-plus-noise filter (adaptive EMA). State per instrument: `x̂` (level), `P` (variance).
Hyperparams: `Q ≥ 0` (process noise), `R > 0` (measurement noise). Output: filtered `x̂`. Shape
Panel, dtype F64. Recurrence state stride = 2 (`x̂`, `P`).

```
seed (first finite z[t,j]):  x̂ = z; P = R;        output x̂
step (subsequent t):
    P⁻ = P + Q
    if z finite:  K = P⁻/(P⁻+R);  x̂ = x̂ + K*(z - x̂);  P = (1-K)*P⁻
    if z NaN:     x̂ unchanged;     P = P⁻             # uncertainty grows, no update
    output x̂
before first finite z: output NaN (state unseeded)
```

### 4.2 `kalman(y, x, delta, R)` — Chan 2-state regression (Phase D, **record**, recurrence)

Per instrument, time-varying OLS of `y_j` on `x_j`. State β = `[α, b]` (intercept, slope); covariance
`P` (2×2). Hyperparams: `delta ∈ (0,1)` (state-transition noise, e.g. `1e-4`), `R > 0` (observation
noise). Process covariance `W = (delta/(1-delta)) · I₂` (Chan). Recurrence state stride = **5**:
`α, b, P00, P01, P11` (P is symmetric, so `P10 ≡ P01` is not stored). The innovation variance `Q`
in the step below is the *local* innovation variance, distinct from `kalman_level`'s process-noise
hyperparam `Q`.

```
seed (t=0):  β = [0, 0];  P = I₂           # pinned diffuse prior, reference-tested
step (obs y_t, x_t both finite):
    P⁻ = P + W
    F  = [1, x_t]                          # observation row
    ŷ  = F·β = α + b·x_t
    e  = y_t - ŷ                            # innovation
    Q  = F·P⁻·Fᵀ + R                        # innovation variance (scalar)
    Kg = (P⁻·Fᵀ) / Q                        # 2-vector gain
    β  = β + Kg·e
    P  = P⁻ - Kg·(F·P⁻)                     # = (I - Kg·F)·P⁻
obs incomplete (y_t or x_t NaN): predict-only (P = P + W, β unchanged); pins emit NaN this cell
```

**Pins** (all Panel, F64): `alpha = α`, `beta = b`, `resid = e/√Q` (standardized innovation — the
mean-reversion trading signal). `n_out = 3`.

### 4.3 `ou_filter(x, theta, mu)` — OU AR(1) pull-to-mean smoother (Phase C, single output, recurrence)

Discrete OU as exponential pull toward a constant equilibrium `mu`. `φ = exp(-theta)`,
`theta ≥ 0` (mean-reversion speed/bar), `mu` finite. State stride = 1 (`x̂`).

```
seed (first finite x): x̂ = x;  output x̂
step:                  x̂ = mu + φ*(x̂_prev - mu);  output x̂   # obs-independent pull
```

(The mean-reversion *signal* is `x - ou_filter(x,theta,mu)`, composable in the DSL. A constant-`mu`
filter is the simple primitive; the windowed family below gives the adaptive equilibrium.)

### 4.4 OU rolling-fit family (Phase E, single output, windowed Ts)

Over the trailing window of `d` bars, fit AR(1) `x[s] = a + b·x[s-1]` by OLS on the `d-1` lagged
pairs (the same rolling-OLS machinery as `slope`/`resid`, regressing on lagged-self instead of a
time index). `φ = b`. Rolling family: lookback `(d-1) + max(child)`; shape Panel; window is an
operand literal (read like `ts_mean`'s window). NaN/min-period policy mirrors the existing rolling
ops; results are NaN when `b ∉ (0,1)` (no valid mean reversion).

| Op | Output | Definition | NaN when |
|----|--------|------------|----------|
| `ou_theta(x, d)`    | speed θ | `-ln(b)`             | `b ∉ (0,1)` |
| `ou_halflife(x, d)` | half-life | `ln(2)/θ = -ln(2)/ln(b)` | `b ∉ (0,1)` |
| `ou_mean(x, d)`     | equilibrium μ | `a/(1-b)`        | `b ≥ 1` |
| `ou_zscore(x, d)`   | mean-rev z | `(x[t]-μ)/σ_eq`, `σ_eq = σ_resid/√(1-b²)` | `b ∉ (0,1)` or `σ_eq=0` |

---

## 5. Per-file impact map

| File | Phase | Change |
|------|-------|--------|
| `alpha/lexer.hpp` | B | `TokenKind::Dot`; lex `.`. |
| `alpha/parser.hpp` | A,B | binding table + diagnostics; binding-first `Ident` resolution; postfix `.IDENT` member parselet; `Expr::Kind::Member`; hparam peeling after default-fill. |
| `alpha/registry.hpp` | B,C,D,E | `OpSig.n_hparams`, `OpSig.pins`, `PinSig`; new `OpCode`s (`KalmanLevel`, `KalmanReg`, `OuFilter`, `OuTheta`, `OuHalflife`, `OuMean`, `OuZscore`, `Pin`); built-in rows. |
| `alpha/fwd.hpp` | B,C | forward-declare new `OpCode`s if listed there. |
| `alpha/typecheck.hpp` | A,B,C,D,E | record `TypeInfo`; `analyze_member`; hparam validation; per-op dtype/shape pinning; OU rolling family in `is_rolling_ts`; recurrence family classification. |
| `alpha/dag.hpp` | B,C,D | `Node.n_out`, `Node.hparams`; `NodeKey.imm_bits`; lower `Member`→`Pin`; lower multi-output call→compute node; hparam capture. |
| `alpha/bytecode.hpp` | B | `Instr.imm` → `array<f64,2>`; `Instr.n_out`; `SlotPool::acquire_block`/`release_block`; block-aware Free/liveness; emit `Pin` instrs. |
| `alpha/state_ops.hpp` | C,D | `kalman_level_step`, `kalman_reg_step` (struct-view 2×2), `ou_filter_step`. |
| `alpha/ts_ops.hpp` | E | rolling AR(1) OLS kernels for the OU family. |
| `alpha/vm.hpp` | B,C,D,E | dispatch + kernels: `Pin` copy; strided multi-scalar `state_` (stride per op); `eval_recurrence` branches; OU windowed via `eval_time_series`. |
| `alpha/oracle.hpp` | B,C,D,E | independent reference for every above. |
| `tests/alpha_*_test.cpp` | all | unit + differential + known-value (auto-globbed; do not edit CMakeLists). |

**Recurrence state generalization (Phase C):** `Engine::state_` (and the oracle's per-column scalar)
become a strided buffer sized `instruments * max_stride`; each recurrence op uses its own stride
(`kalman_level`=2, `kalman`=5, `ou_filter`=1, existing `trade_when`/`hump`=1). Grown once, reused.

---

## 6. Phasing (subagent-driven; each phase lands green + tested independently)

- **Phase A — Local bindings + references.** Parser symbol table, binding-first resolution, shadow
  warning, output rule. Tests: reference reuse, shadowing warning, record-binding-not-a-root,
  backward-compat (existing programs unchanged). *No new ops.*
- **Phase B — Multi-output IR + records + member access.** Lexer `Dot`; `Member`; record `TypeInfo`;
  `Node.n_out`/`Pin`; slot blocks; `imm[2]`/`NodeKey.imm_bits`; registry `pins`/`n_hparams`.
  Validate with a **synthetic test-only 2-pin op** (`split2(x) → {hi, lo}`, e.g. hi=x, lo=-x) so the
  IR is proven before any filter math. Differential test on `split2`.
- **Phase C — Strided recurrence state + `kalman_level` + `ou_filter`.** state_ops steps; VM+oracle
  `eval_recurrence` branches; typecheck. Differential + known-value (numpy) tests.
- **Phase D — Chan `kalman` record {alpha,beta,resid}.** Uses B (multi-output) + C (recurrence,
  stride 5). 2×2 covariance math. Differential + reference tests (vs a pinned Python/pykalman run).
- **Phase E — OU rolling-fit family** (`ou_theta`/`ou_halflife`/`ou_mean`/`ou_zscore`). Rolling
  AR(1) OLS Ts kernels; VM+oracle; typecheck. Differential + known-value tests.

Each phase: TDD (failing GoogleTest first), `clang-format` clean, `/W4 /WX` green, full alpha suite
green, commit `feat(p3d-N): …` with the `Co-Authored-By: Claude Opus 4.8 <noreply@…>` trailer.
**Shared-branch race does not apply** (this is an isolated worktree on its own branch) — but still
stage explicit pathspecs, never `git add -A`.

---

## 7. Testing strategy

- **Differential (load-bearing):** every new op runs through both `Engine::evaluate` and
  `evaluate_reference`; a differential test asserts bit-for-bit equality (NaN==NaN) over randomized
  panels — the same harness used for `trade_when`/`hump`/Ts/Cs.
- **Known-value:** Kalman (scalar + Chan) and OU fits checked against a small pinned Python/numpy
  reference (a few hand-computed sequences embedded as expected arrays), as `ShrinkageMv`/Ts ops were
  validated against numpy.
- **Causality:** assert no look-ahead — output at `t` depends only on inputs ≤ `t` (structural in the
  forward scan; add a regression test that perturbing `x[t+1]` never changes `out[t]`).
- **Language:** parser tests for binding resolution, shadow warnings, member access, record-misuse
  errors; `compile_batch`/CSE test proving `kf.beta`+`kf.resid` produce **one** compute instr.
- **Lookback:** each op's `required_lookback` asserted (recurrence = child lookback; OU windowed =
  `(d-1)+child`).

---

## 8. Risks & mitigations

- **Arity/immediate plumbing (`imm[2]`, `NodeKey.imm_bits`, hparam peel)** touches DAG/bytecode/VM.
  *Mitigation:* land it in Phase B behind `split2` before any filter; differential test guards it.
- **Slot-block contiguity** in `SlotPool`. *Mitigation:* per-size free-list; assert block contiguity;
  `peak()` invariant test; Phase B `split2` exercises Free of a 2-slot block.
- **Chan seed/NaN policy** is a pinned *choice*; a different seed changes early outputs. *Mitigation:*
  pin in this spec, restate in VM+oracle, lock with the Python reference; document in the op header.
- **Record-misuse leakage** (a record value used as a scalar). *Mitigation:* typecheck rejects record
  values everywhere except `.pin`; explicit error-path tests.
- **Backward compatibility** of binding-first resolution. *Mitigation:* shadowing is warn-only;
  existing-program regression tests; no grammar change in Phase A.

---

## 9. Decisions resolved during brainstorming

1. Kalman flavors: **both** scalar local-level (`kalman_level`) and Chan 2-state (`kalman`).
2. OU: **family + recurrence** — windowed `ou_theta/halflife/mean/zscore` and recurrence `ou_filter`.
3. Dyn-Kalman: **Chan 2-state**, emits **beta + alpha + resid** (record pins).
4. Multi-output realization: **B — true multi-output named-pin IR** (not recompute).
5. Pin surface: **member-access `.pin`** plus **local let-bindings** (full ergonomic form).
6. Spec structure: **one phased spec** (this doc), phases A–E.
7. Shadowing: **binding shadows field, warn**.

---

## 10. Open items for the implementation plan (writing-plans)

- Exact `RecordInfo` storage (span-from-registry vs owned) and how `analyze` threads pin lookback.
- Whether `state_` stride is per-op-max (single buffer) or per-op typed scratch (struct for the 2×2).
- `Diagnostic` channel plumbing (return type change vs out-param) for parser warnings.
- Precise OU rolling NaN/min-period semantics aligned to existing `slope`/`resid`.
- Python reference fixtures location + generation script for known-value tests.
