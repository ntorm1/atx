# Phase 3 — Alpha Expression DSL + Vectorized Evaluation Engine — Implementation Plan

**Module:** `atx-engine` (`p0`)
**Phase theme:** Turn a **quant idea written as a string** — `rank(ts_corr(close, volume, 10))` — into a
parsed, type-checked, **hash-consed compute DAG**, compiled to **vectorized bytecode**, and executed by a
**columnar virtual machine** over a `date × instrument` panel to emit a **cross-sectional signal vector**.
This is the alpha-research compute core: cheap, point-in-time-correct, deterministic fan-out evaluation of
**thousands-to-millions of weak signals** that share subexpressions — the WorldQuant WebSim role, built for
mass evaluation by default.
**Status:** frozen at kickoff (fossil — scope changes go in the ledger's *Plan adjustments*, not here).
**Authority:** [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md). Quality bar:
[`../docs/implementation-quality.md`](../docs/implementation-quality.md).
**Read first:** [`engine-architecture-audit.md`](engine-architecture-audit.md) §2 (the two-layer split — this
phase **is** the alpha-research layer) and §3 (invariants: determinism, no look-ahead).
**Grounding:** every design choice below is sourced in
[`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md)
(WorldQuant Alpha101, Microsoft Qlib, Zipline Pipeline, DuckDB vectorized execution, *Crafting
Interpreters* Pratt/bytecode, Sethi–Ullman CSE/DAG). Section cites below point at that report; the
operator table is reproduced in Appendix A and the URL index in Appendix B.
**Roadmap position:** this **replaces** the old Phase-3 sketch ("alpha research layer (vectorized)") — the
formulaic-alpha **operator vocabulary is now the VM's instruction set**. The alpha **store** and
**correlation/turnover gates** move to **Phase 4** (combiner); this phase stops at *source → signal vector*.
**Upstream (blocked-on atx-core):** L9 `column`/`frame`, L6 `cross_section`/`rolling`/`online_stats`,
L5 `simd`, L3 `hash_map`/`fixed_vector`, L2 `arena`/`object_pool`/`aligned`, L1 `hash`, L8 `time`. None of
these are built yet — Phase 3 opens against their **contracts** (Pattern-B blocked-on; §6), exactly as
Phase 1 opened against the L4/L8 contracts.

---

## 1. What this sprint delivers

The alpha compute core. After Phase 3, a caller can do:

```cpp
namespace dsl = atx::engine::alpha;
dsl::Library lib;                                            // operator registry (built-ins)
auto program = dsl::compile({"alpha_001 = rank(ts_argmax(close, 5)) - 0.5",
                             "alpha_002 = -correlation(rank(close), rank(volume), 6)"}, lib);
ATX_TRY(program);                                            // Result<Program>: parse/type errors caught here
dsl::Engine eng(panel /* atx-core series::Frame */, lib);
const dsl::SignalSet sig = eng.evaluate(*program);           // one cross-sectional vector per alpha per date
```

Concretely the phase ships:

1. A **lexer + Pratt parser** for the alpha-expression grammar (infix operators, function calls, prefix
   unary, grouping, ternary), emitting an `Expr` AST. New operators are *one registry row* + *one opcode*.
2. An **operator registry** (`Library`): name → arity, **shape signature** (Scalar / CrossSection / Panel),
   dtype (f64 / bool-mask / int-group), and **lookahead flag** — the single source of truth the parser,
   type-checker, and VM all consult.
3. A **semantic pass**: constant folding + desugaring, **shape/dtype type-checking**, and **lookback
   analysis** (each node's required trailing-window depth, propagated up the DAG — Zipline
   `extra_input_rows`).
4. A **hash-consed expression DAG** that collapses structurally identical subexpressions across *all*
   alphas into one node — this **is** the common-subexpression-elimination pass and the dominant throughput
   lever for mass evaluation.
5. A **vectorized bytecode compiler**: topological linearization of the DAG into a flat instruction stream
   with **slot allocation + consumer refcounts** (materialize each intermediate column once, free it when
   its last consumer has run).
6. A **columnar VM** over atx-core `series::Frame`: each opcode processes a **whole column** (a
   cross-section for CS ops, a per-instrument series for TS ops). Element-wise, cross-sectional, and
   time-series opcode families, each a tight, SIMD-friendly, zero-allocation inner loop.
7. A **tree-walking reference interpreter** (slow, obviously-correct) used purely as a **differential
   oracle** to prove the fast VM computes identical results.
8. A **determinism + lookahead + differential** test harness and a micro-benchmark suite (alphas/s,
   ns/cell, cache-hit ratio).

**Not** in this phase (deliberate, per the scope decision): no portfolio, P&L, or backtest wiring (Phase 2
spine consumes the signal); no alpha store, correlation/turnover orthogonality gates, or mega-alpha
combiner (Phase 4); no JIT (deferred until the vectorized interpreter is profiled — research §5.5).

**Maps to research:** [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md)
§1 (Alpha101 vocabulary), §2 (Qlib engine = the architectural template), §3.1 (Zipline DAG/CSE), §4 (Pratt),
§5 (vectorized VM + DAG caching), §6 (lookahead/NaN/determinism), §8 (operator table).

---

## 2. Reference architecture (open-source-grounded)

The pipeline is the **DuckDB vectorized-interpreter** shape fused with the **Zipline Term-DAG** and the
**Qlib string-DSL-over-a-panel** model. Canonical flow:

```
  source strings (N alphas)                ATX ALPHA DSL (Phase 3)
  ┌────────────────────────┐
  │ "rank(corr(close,vol,6))"│  ── lex ──▶ tokens ── Pratt parse ──▶  Expr AST (per alpha)
  └────────────────────────┘                                              │
                                                            const-fold · desugar · TYPE-CHECK
                                                            (shape S/V/P · dtype · arity)         (§4)
                                                                          │
                                                                          ▼
                                          ┌──────────── hash-consed EXPRESSION DAG ───────────┐
   shared subexpressions collapse  ◀──────│  node = (opcode, child-ids, params)  → free CSE   │ (§3.2)
   rank(close), ts_mean(vol,20) once       │  annotate: shape, dtype, required-lookback        │
                                          └───────────────────────┬───────────────────────────┘
                                                  topological linearize + slot alloc + refcount
                                                                  │  (materialize once, free on last use)
                                                                  ▼
                                          ┌──────────────── VECTORIZED BYTECODE ───────────────┐
                                          │ LOAD_FIELD/CONST · ADD/MUL/SELECT · CS_RANK/ZS ·    │
                                          │ TS_DELAY/MEAN/CORR/DECAY · STORE_ALPHA · FREE        │ (§3.1)
                                          └───────────────────────┬───────────────────────────┘
                                                                  ▼
   columnar Frame (atx-core L9)   ───▶   COLUMNAR VM:  each opcode runs over a whole column
   date × instrument, f64 + mask         (CS op = one row/date; TS op = one instrument col)     (§3.1)
                                                                  │
                                                                  ▼
                                          SignalSet:  one cross-sectional signal vector
                                          per alpha per date  ──▶ (Phase 2 Strategy consumes)
```

Each subsystem maps to a proven reference:

| ATX component | Reference (open-source) | What we borrow |
|---|---|---|
| Grammar + Pratt parser | *Crafting Interpreters* "Compiling Expressions"; Pratt (POPL 1973); Crockford TDOP | `ParseRule{prefix,infix,binding-power}` table; left-assoc recurse at `bp+1`; one row per operator |
| String DSL over a panel | **Microsoft Qlib** `qlib/data/ops.py`, `base.py` (`Feature("close")` leaves, `Op(args…)` tree, lazy memoized `load`) | tree shape `Op(args…)`/`Field` leaves; per-(expr) memoization — *we hand-roll the parser instead of Python `eval`* |
| Compute DAG + CSE | **Zipline** `pipeline/term.py` (`Term` memoized by ctor args ⇒ auto-dedup; `extra_input_rows`; `window_safe`) | hash-cons by structure = free CSE; per-input lookback depth; non-causal-into-window guard |
| Vectorized execution | **DuckDB** / Vectorwise-X100 (`STANDARD_VECTOR_SIZE`, interpreted-but-batched); Kersten et al. VLDB'18 | each opcode processes a column batch; dispatch amortized over N; SIMD-friendly contiguous loops |
| CSE / DAG materialization | Sethi–Ullman labeling; CSE liveness | shared interior node = common subexpression; refcount-driven free schedule (bounded peak memory) |
| Operator vocabulary | **Alpha101** (Kakushadze, arXiv:1601.00991) ∪ Qlib `ops.py` ∪ WorldQuant BRAIN | the cross-sectional + time-series + element-wise op set (Appendix A) — becomes the opcode ISA |
| GP/RL mass-mining workload | `gplearn` (arity-typed function set, flat program trees) | why subexpression sharing is the #1 lever: millions of trees overlap on `rank(close)` etc. |
| Columnar storage + NaN | Apache Arrow compute (contiguous arrays + validity bitmap); numpy | column-major f64 panel + validity mask; propagate-don't-impute NaN |

> **Build-order note.** The execution **spine** (Phases 1–2) ships first because it is the harder
> *correctness* surface; this research layer (Phase 3) plugs into a spine already proven deterministic and
> point-in-time. The handoff is one-directional: the VM emits `SignalSet`; the Phase-2 Strategy reads it.

---

## 3. Performance & correctness model (build this in from unit 1)

> This is a **mass-evaluation, performance-by-default** sprint. The target workload is **10⁵–10⁹ alphas**
> over the same panel (GP/RL mining; research §3.3, §5.3). Two levers dominate and are designed in from the
> first unit: **(a) vectorized dispatch** (amortize interpretation over a whole column) and **(b)
> subexpression-DAG dedup + caching** (compute `rank(close)` once, not once per alpha). Correctness rails —
> lookahead, NaN, determinism — are likewise structural, not bolted on.

### 3.1 Execution model — vectorized columnar VM (research §5.1–5.2)

- **Bytecode, not tree-walking.** A flat instruction stream (`(opcode, src-slots, dst-slot, params)`) read by
  a dispatch loop beats recursive AST traversal (no per-node virtual dispatch). The tree-walker survives
  only as the **differential oracle** (P3-5).
- **Each opcode is vectorized.** The "vector" is a **column**: a cross-sectional op processes one date-row
  across all instruments; a time-series op processes one instrument-column down time. Dispatch cost is
  amortized over N values, so the **stack-vs-register and computed-goto debates are largely moot here**
  (they optimize *scalar* dispatch) — vectorization is the lever, not micro-dispatch (research §5.1 reframing).
  Inner kernels are tight loops over contiguous `f64*` + a validity bitmap, alignment-friendly for atx-core
  L5 `simd`.
- **Dispatch.** Start with an exhaustive `switch` over `enum class OpCode`. Upgrade to computed-goto **only**
  if profiling shows dispatch matters (it will not, once vectorized). Instruction pointer kept as a raw
  pointer.
- **Slot pool + materialize-once.** Each DAG node's result column is computed **once** into a pooled slot and
  reused by every consumer; the slot returns to the pool when its consumer **refcount** hits zero. This
  bounds peak memory and is the only place "allocation" happens — from an atx-core L2 arena/pool, never in
  the inner loop (research §5.3 liveness/refcount; agent.md zero-hot-path-alloc).

### 3.2 Subexpression-DAG dedup = CSE = the #1 throughput lever (research §3.1, §5.3)

Evaluating many alphas over one panel means colossal overlap: `rank(close)`, `ts_mean(volume,20)`,
`delta(close,5)` recur in thousands of expressions. Strategy:

1. **Hash-cons** every parsed AST into **one global DAG**: each node keyed by `(opcode, ordered child
   node-ids, params)` via atx-core L1 `hash` into an L3 `hash_map`. Structurally identical nodes — *across
   all alphas* — collapse to a single node. This is exactly Zipline's Term-memoization, generalized; it **is**
   the CSE pass (no separate optimizer).
2. Evaluate the DAG in **topological order**, materializing each unique node's column once.
3. **Refcount** consumers; free an intermediate when its last consumer has run.

This converts *"evaluate N alphas"* into *"evaluate the union DAG of their unique subexpressions"* — commonly
a **10–100× compute reduction** for mined alpha sets (research §5.3). The DAG is built once and reused across
dates; only the leaf field columns change as the panel advances.

### 3.3 Lookahead safety — causal by construction (research §6.1; audit §3 invariant)

The non-negotiable engine invariant (audit §3): a value at `(date t, instrument i)` may depend only on data
at dates `≤ t`. Enforced in three layers:

1. **Operator level.** Every time-series op is *defined* backward: it reads only the trailing window
   `[t−d+1, t]`. `delay(x,d)` / `Ref` shifts past→present; a **negative shift is a compile error** (Qlib's
   `Ref(N<0)=future` is the anti-pattern we forbid).
2. **VM level.** The compiler computes each node's **required lookback** (max trailing depth, summed up the
   DAG — Zipline `extra_input_rows = window_length−1`) and the VM only ever indexes already-computed history.
   A `window_safe` flag forbids feeding a non-causal/level term into a window.
3. **Universe + point-in-time.** Each date carries a **valid-universe mask** (which instruments are listed
   that day); CS ops rank/demean only over the valid set and emit NaN elsewhere — you cannot rank against an
   instrument that did not yet exist (this is *both* a NaN and a lookahead concern; research §6.3). Delisted
   symbols are present (no survivorship) but masked out after their final bar.

### 3.4 NaN / missing-data policy (research §6.2)

- **Time-series ops:** explicit per-op **`min_periods`** — return NaN until the window holds ≥ `min_periods`
  valid observations (e.g. require the full window for `stddev`; allow partial for `ts_mean`). Choose and
  document per op in the registry; never silently default.
- **Cross-sectional ops:** exclude NaN/out-of-universe instruments from the cross-section (rank/zscore only
  the valid set; demean by valid group members) and **propagate NaN** for the excluded — never impute.
- **`correlation`/`covariance`:** one uniform pairwise-valid policy (window with any NaN → NaN, *or*
  complete-pairs-only) applied everywhere.

### 3.5 Determinism (research §6.4; audit §3 invariant)

- **Rank tie-breaking is fixed.** Alpha101 uses `average` (`rank(pct=True)`); for **bit-reproducibility** the
  engine default is **`ordinal` keyed by a stable instrument id**, with `average` available as an explicit
  registry option. NaNs always sort last.
- **Reductions are order-fixed.** Floating-point sums are non-associative, so reductions run in a **canonical
  instrument order** with a fixed scheme (pairwise/Kahan) so that multi-threaded evaluation reproduces the
  single-thread result bit-for-bit.
- **Parallelism preserves order.** Morsel/date- or instrument-partitioned threads share the *immutable* cache
  of already-computed node columns; no thread observes a partial column (research §7). Determinism is proven
  by P3-9's differential + repeat-run hash test.

### 3.6 Performance budgets (record measured; no invented figures)

| Path | Budget (intent) | Notes |
|---|---|---|
| element-wise opcode | ≤ ~1–2 ns / cell amortized | contiguous f64 loop; SIMD via L5 |
| `ts_*` rolling opcode | O(1)/cell incremental where possible | online/rolling accumulators (L6 `rolling`/`online_stats`), not O(d)/cell |
| `cs_rank` per date | ≤ ~O(M·log M), M = universe size | one sort/partition per date-row |
| DAG dedup | report **unique-nodes / total-AST-nodes** + **cache-hit %** | the lever's evidence |
| mass eval | report **alphas/s** and **ns/cell** at the panel size | host/build/panel-shape recorded; measured only |

Benches report measured numbers with host/build context only — never cite unverified third-party figures.

---

## 4. Type / shape system (research §1.3, §9.4)

Three **shapes**, three **dtypes**, checked at compile time (shape mismatch ⇒ compile error):

| Shape | Meaning | Produced by |
|---|---|---|
| **S** scalar | a constant (the `d` in `ts_mean(x,d)`, a literal `0.5`) | literals, folded constants |
| **V** cross-section | one value per instrument at a single date | `rank`, `zscore`, `scale`, `indneutralize` (logically P→V-per-row) |
| **P** panel | `date × instrument` block | raw fields; arithmetic; all `ts_*` |

| dtype | Meaning | Maps to (Zipline) |
|---|---|---|
| `f64` | numeric | `Factor` |
| `mask` | boolean (from comparisons/logical) | `Filter` |
| `group` | integer class label (sector/industry) | `Classifier` |

**Broadcast rules** (enforced in P3-3): element-wise = `P∘P→P` / `P∘S→P`; time-series = `P→P` (each
instrument column rolled independently, trailing window); cross-sectional = `P→P` where each date-row is
ranked/reduced independently (logically `P→V` per row, stored back into a `P`). `SELECT(cond,a,b)` requires
`cond:mask`. Group ops require a `group` second argument. The **registry** carries each operator's shape
signature so the checker is table-driven, not hand-coded per op.

---

## 5. The alpha-expression grammar (research §4)

Concrete syntax (EBNF-ish; precedence low→high), parsed by the Pratt driver in P3-2:

```
program     := { assignment }
assignment  := IDENT '=' expr                      ; "alpha_001 = <expr>"
expr        := ternary
ternary     := logic_or [ '?' expr ':' expr ]
logic_or    := logic_and { '||' logic_and }
logic_and   := equality  { '&&' equality }
equality    := comparison { ('=='|'!=') comparison }
comparison  := additive  { ('<'|'>'|'<='|'>=') additive }
additive    := multiplic { ('+'|'-') multiplic }
multiplic   := unary     { ('*'|'/') unary }
unary       := ('-'|'!') unary | power
power       := primary   [ '^' unary ]             ; right-assoc
primary     := NUMBER | field | call | '(' expr ')'
field       := IDENT | '$' IDENT                   ; close  or  $close (Qlib sigil, optional)
call        := IDENT '(' [ expr { ',' expr } ] ')' ; ts_mean(close, 20)
```

Field identifiers (`open high low close volume vwap returns cap adv20 …` + `IndClass.*` groups) resolve to
`LOAD_FIELD` leaves against the panel's column dictionary; unknown identifiers used as `name(` are operator
calls resolved in the registry (arity + shape checked there); any other bare identifier is a field lookup
(error if absent). Numeric literals are `S`. The grammar is small enough that one Pratt table covers infix,
prefix-unary, grouping, call, and ternary — adding an operator is one registry row + one `ParseRule` entry +
one opcode.

---

## 6. Cross-module dependencies (Pattern B — blocked-on upstream)

The DSL is a **consumer** of atx-core; it adds no general-purpose primitives. Per module.md the edge lives
here. As of 2026-06-02 only atx-core **L0** (+ L1 `decimal`, L3 containers, L4 `disruptor`, L8 `time` partial
landing) exist; the columnar/vectorized layers this phase needs are pending. Phase 3 opens against their
**contracts** in the stdlib design spec.

| Phase-3 unit | Blocked-on (atx-core) | Why |
|---|---|---|
| P3-1, P3-2 lexer/parser | L0 (`types`,`error`,`util`) + L3 `fixed_vector` | tokens/AST nodes in fixed buffers; `Result<T>` errors |
| P3-3 typecheck | L0 | pure logic over the registry |
| P3-4 DAG (hash-cons) | L1 `hash`, L3 `hash_map`, L2 `arena` | structural keys → dedup map; node arena |
| P3-5 eval context | **L9 `column`/`frame`**, L8 `time`, L2 `object_pool`/`aligned` | the panel; pooled column slots; date calendar |
| P3-6 VM core (element-wise) | **L5 `simd`**, L2 pool | SIMD inner loops; slot lifetime |
| P3-7 cross-sectional ops | **L6 `cross_section`** (rank/zscore/scale/neutralize) | the CS kernels |
| P3-8 time-series ops | **L6 `rolling`/`online_stats`** | O(1)/cell rolling accumulators |
| P3-9 harness/bench | L1 `hash` (result hashing), bench harness | determinism hash; alphas/s |

**Sequencing consequence (as Phase 1):** Phase 3 can **open and scaffold immediately** — write headers,
registry, grammar, and **failing TDD tests against the atx-core L5/L6/L9 contracts**. Per-unit green-gate
lands as each atx-core layer merges; blocked units stage red behind the `atx_engine_pending` CMake label. A
residual is filed against the atx-core build so its agent sees engine Phase 3 waits on L5/L6/L9. The ROADMAP
cross-module table is canonical.

---

## 7. Cross-cutting gates (agent.md — every unit)

- **TDD:** failing GoogleTest first; one behavior per `TEST`; name `Subject_Condition_ExpectedResult`; cover
  happy path, **boundaries** (empty program, single alpha, 1-instrument universe, window `d=1` and
  `d>history`, all-NaN column), error paths (parse error, arity/shape mismatch, unknown field, **negative
  lookback**), invariant violations (`EXPECT_DEATH` on documented preconditions).
- **Differential correctness:** every VM opcode is cross-checked against the **tree-walking oracle** (P3-5)
  and, where a faithful formula exists, against the **Alpha101 reference semantics** (Appendix A) on fixed
  fixtures. The fast VM is "correct" only when it matches the oracle bit-for-bit (within a documented ULP
  tolerance for float reductions).
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) / `-Wall -Wextra -Wpedantic -Wconversion -Wshadow
  -Werror` (Clang); clang-tidy (project `.clang-tidy`) + clang-format clean. (`atx_warnings` carries flags.)
- **Sanitizers:** ASan + UBSan on every test run; TSan on the **parallel evaluator** (P3-9) where the
  toolchain supports it (clang-cl/Windows ships no usable TSan — Linux/clang TSan pass is a tracked residual,
  same caveat as the disruptor; parallel tests are written race-clean: join before assert).
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]` where they hold; `Result<T>`/`Status`
  for expected failures (parse/type errors), **never throw** for control flow; weakest sufficient type at
  interfaces (`span`/`string_view`/concepts); no owning raw pointers; Rule of Zero; **exhaustive `enum class`
  switches** over `OpCode`/`Shape`/`TokenKind` (no `default`); functions ≤ ~60 lines; `// SAFETY:` on every
  deviation (slot reinterpret, union access, unchecked index inside a bounds-proven loop).
- **Zero hot-path allocation:** the VM inner loops and per-date evaluation allocate nothing; all column slots
  come from a pre-sized L2 pool. Parsing/compiling (cold path) may allocate.

---

## 8. Unit decomposition (P3-0 … P3-9)

Dispatch **sequential** (each consumes the prior's headers). TDD red→green→refactor within each. P3-1…P3-4
(front-end: lex/parse/typecheck/DAG) are **not blocked** on the heavy atx-core layers and can go fully green
immediately; P3-5…P3-8 stage against L9/L6/L5 contracts.

---

### P3-0 — Module scaffold + CMake + ledger  *(marker commit)*

**Scope.** Create the `atx::engine::alpha` tree and wire the build so new `*_test.cpp`/`*_bench.cpp` are
auto-discovered. Open the Phase 3 ledger and freeze scope. No DSL logic.

**Deliverables.**
- `include/atx/engine/alpha/` with `fwd.hpp` (namespace doc + forward decls: `Token`, `Expr`, `Library`,
  `Node`, `Dag`, `Program`, `Engine`, `SignalSet`, `OpCode`, `Shape`, `DType`).
- `tests/`/`bench/` wiring mirroring atx-core; trivial `scaffold_test.cpp` proves GTest link.
- CMake label `atx_engine_pending` for blocked targets (P3-5…P3-8).

**Acceptance.** `cmake --build` green; `scaffold_test` passes; ledger marker commit
`docs(p3-0): open phase-3 sprint ledger` with `Base: <branch> @ <SHA>`.

---

### P3-1 — Lexer (tokens + spans)

**Scope.** A hand-written tokenizer (research §4.1): scan source → tokens with source spans for diagnostics.
No grammar yet.

**Data structure.**
```cpp
namespace atx::engine::alpha {
enum class TokenKind : u8 {
  Number, Ident, Dollar, Plus, Minus, Star, Slash, Caret,
  Lt, Gt, Le, Ge, EqEq, BangEq, AmpAmp, PipePipe, Bang,
  Question, Colon, LParen, RParen, Comma, Assign, End
};
struct Span { u32 begin; u32 end; };                       // byte offsets into source
struct Token { TokenKind kind; Span span; f64 number{}; }; // number filled for Number
}
```

**Contract.** Total scan; an unrecognized byte yields `Result<…>` `ParseError` with the offending span (no
throw). Whitespace skipped; `==`/`!=`/`<=`/`>=`/`&&`/`||` lexed as single tokens (maximal munch). Numbers via
a bounded parser (no `strtod` locale surprises — document).

**Tests.** each token kind; multi-char operators vs single (`<` vs `<=`); number forms (`5`, `0.5`, `1e3`);
`$close` sigil; span correctness; unknown byte → `ParseError` at the right offset. Boundary: empty source;
trailing whitespace; adjacent operators.

**Acceptance.** `/W4 /WX` clean; green under ASan/UBSan. Commit `feat(p3-1): alpha-expr lexer`.

---

### P3-2 — Pratt parser → AST + operator registry  *(research §4.2–4.3, §5)*

**Scope.** The Pratt/TDOP parser (one `ParseRule` table) producing an `Expr` AST, plus the **operator
registry** (`Library`) that the parser/checker/VM share. Constant folding + desugaring at build time.

**Data structures.**
```cpp
namespace atx::engine::alpha {
enum class Shape : u8 { Scalar, CrossSection, Panel };
enum class DType : u8 { F64, Mask, Group };

struct OpSig {                       // one registry row per operator/function
  std::string_view name;
  u8       arity;                    // fixed arity (variadic handled explicitly)
  OpCode   opcode;
  DType    out_dtype;
  bool     lookahead_safe;           // all built-ins true; kept explicit as a rail
  Shape  (*shape_of)(std::span<const Shape> args);  // shape signature (table-driven §4)
};
class Library {                      // built-ins registered at construction; user ops addable
 public:
  [[nodiscard]] const OpSig* find(std::string_view name) const noexcept;
  Status register_op(OpSig) noexcept;                // Result on duplicate/invalid
};

struct Expr {                        // AST node (owns children via arena indices, not raw ptr)
  enum class Kind : u8 { Literal, Field, Unary, Binary, Call, Ternary, Assign } kind;
  // payload by kind: f64 literal; field-id; op token; child indices; call op-id + arg span
};
}
```

**Why Pratt (over shunting-yard).** A grammar mixing infix operators **and** function calls **and**
prefix-unary **and** ternary is cleanest in one TDOP table (research §4.2): `parsePrecedence(bp)` calls the
prefix parselet, then loops infix while `bp ≤ rule.bp`; left-assoc recurses at `bp+1`, right-assoc (`^`) at
`bp`. New operator = one table row. Shunting-yard emits flat RPN and is awkward for mixed call/prefix/assoc.

**Desugar/fold.** `a-b`→`Binary(SUB)`; `x?a:b`→ a `SELECT` call; `signedpower(x,a)` kept as a primitive opcode
(`SPOW`) — *not* expanded — to preserve NaN/sign semantics; constant subtrees folded (`2*3`→`6`,
`log(1)`→`0`); `pow(x,2)`→`x*x` strength-reduction noted (applied in P3-4 on the DAG).

**Tests.** precedence (`a+b*c`, `-a^b`, `a<b?c:d`); right-assoc `^`; function calls with 1/2/3 args; nested
calls (`rank(ts_corr(close,volume,6))`); unknown operator → `ParseError`; **arity mismatch** caught at parse
(registry); unbalanced parens; trailing comma; constant folding output. Boundary: single literal; deeply
nested; max-arity op.

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p3-2): pratt parser + operator registry`.

---

### P3-3 — Semantic analysis: shape/dtype check + lookback  *(research §6.1, §9.4)*

**Scope.** Walk the AST to assign each node a **shape** (S/V/P) and **dtype** (f64/mask/group), validate
operands against the registry's shape signature, and compute each node's **required lookback** (trailing
window depth, propagated up). Reject non-causal constructs.

**Algorithm (pseudocode).**
```
analyze(node):
  for child in node.children: analyze(child)              // post-order
  sig = registry[node.op]
  check dtypes: SELECT.cond==Mask; group-ops arg2==Group; arithmetic==F64
  node.shape  = sig.shape_of(child.shapes)                // table-driven; mismatch -> TypeError
  node.dtype  = sig.out_dtype
  node.lookback = (sig is ts_*) ? window_arg + max(child.lookback)   // Zipline extra_input_rows
                                 : max(child.lookback over children)
  if node.op is shift/delay and window_arg < 0: error "non-causal (future) access"   // §3.3 rail
  if sig.consumes_window and not child.window_safe: error "non-causal term into window"
```

**Contract.** Pure, allocation-light, `Result<Typed>` (no throw). Window argument must be a **scalar
literal/foldable** (`d` known at compile time) — dynamic windows rejected (keeps lookback static, per
Zipline). A program's total required history = `max` node lookback (the VM pre-rolls that many rows).

**Tests.** shape inference (`rank(close):V-over-P`, `close+open:P`, `ts_mean(close,5):P`); dtype
(`close>open:Mask`, `SELECT(mask,a,b):F64`, group-op needs `Group`); shape mismatch → `TypeError`;
**negative/zero window → error**; non-foldable window → error; `window_safe` violation → error; lookback math
(`ts_mean(delta(close,5),10)` ⇒ 14). Boundary: nested ts (`ts_sum(ts_mean(x,3),4)`); pure-scalar expr.

**Acceptance.** `/W4 /WX` clean; green. Commit `feat(p3-3): shape/dtype/lookback analysis`.

---

### P3-4 — Hash-consed expression DAG + bytecode linearization  *(research §3.1, §5.3 — the throughput core)*

**Scope.** Fold all typed alpha ASTs into **one global hash-consed DAG** (free CSE), then **topologically
linearize** to vectorized bytecode with **slot allocation + consumer refcounts**. This is the unit that makes
mass evaluation cheap.

**Data structures.**
```cpp
namespace atx::engine::alpha {
struct NodeKey { OpCode op; u32 params; std::array<NodeId,3> children; }; // structural key
struct Node    { OpCode op; Shape shape; DType dtype; u16 lookback;
                 std::array<NodeId,3> in; u32 param; u32 refcount; NodeId slot{kNoSlot}; };

class Dag {                                  // built once; reused across dates
 public:
  NodeId intern(const NodeKey&, /*meta*/);   // hash-cons: identical key -> existing id (CSE)
  // ... topo order, refcount accumulation
 private:
  hash_map<NodeKey, NodeId> interned_;       // atx-core L1 hash + L3 hash_map  (SAFETY: key is POD)
  fixed_vector<Node> nodes_;                 // arena-backed; NodeId = index
};

enum class OpCode : u8 { LoadField, Const, Add, Sub, Mul, Div, Neg, Abs, Sign, Log, Pow, Spow,
  MinP, MaxP, CmpLt, CmpGt, CmpLe, CmpGe, CmpEq, CmpNe, And, Or, Not, Select,
  CsRank, CsZscore, CsScale, CsDemeanG, CsNeutG, CsRankG, CsZscoreG,
  TsDelay, TsDelta, TsSum, TsMean, TsStd, TsVar, TsMin, TsMax, TsArgMin, TsArgMax,
  TsRank, TsCorr, TsCov, TsProduct, TsDecayLinear, TsEma, TsWma, TsSkew, TsKurt,
  TsMed, TsMad, TsSlope, TsRsquare, TsResid, StoreAlpha, Free };           // exhaustive switches

struct Instr { OpCode op; SlotId dst; std::array<SlotId,3> src; u32 param; }; // flat program
}
```

**Algorithm.** (1) `intern` each AST node bottom-up — the `interned_` map collapses structurally identical
nodes across *all* alphas (Zipline Term-memoization generalized; research §3.1). (2) Increment a child's
`refcount` per parent edge; each `StoreAlpha` root adds one. (3) Topologically order nodes; **linear-scan
allocate** a slot per node from a free-list, **emitting `Free`** when a node's refcount reaches zero so the
slot returns to the pool (research §5.3 liveness). (4) Strength-reduce on the DAG (`pow(x,2)`→`Mul(x,x)`).

**Tests.** dedup (two alphas sharing `rank(close)` ⇒ one `CsRank` node; assert `nodes_.size()` < total AST
nodes); refcount correctness (shared node freed only after last consumer); topo order respects edges; slot
reuse (peak slots ≤ max live set, not node count); `Free` emitted exactly once per node; strength reduction
applied. Boundary: single alpha (no sharing); fully-shared alphas; diamond DAG.

**Metric.** Emit **unique-nodes / total-AST-nodes** and **peak-live-slots** — the CSE-lever evidence (§3.6).

**Acceptance.** `/W4 /WX` clean; green (front-end, **not** blocked). Commit
`feat(p3-4): hash-consed dag + bytecode linearization`.

---

### P3-5 — Columnar eval context + reference oracle  *(blocked-on atx-core L9 `column`/`frame`, L2 pool, L8 `time`)*

**Scope.** Bind the bytecode to data: a `Frame`-backed **panel** (date × instrument, column-major f64 +
validity mask + per-date universe mask), a **slot pool** of reusable column buffers, and the **tree-walking
reference interpreter** that is the differential oracle for every later opcode.

**Data structures.**
```cpp
namespace atx::engine::alpha {
struct Panel {                                   // thin view over atx-core series::Frame (L9)
  // column-major f64 per field; validity bitmap; per-date universe mask; calendar (L8 time)
  [[nodiscard]] std::span<const f64> field_col(FieldId, DateIdx) const noexcept; // a cross-section
  [[nodiscard]] usize instruments() const noexcept;
  [[nodiscard]] usize dates() const noexcept;
};
class SlotPool {                                 // pre-sized; zero alloc after construction (L2)
  [[nodiscard]] SlotId acquire() noexcept;       // SAFETY: capacity == peak-live-slots from P3-4
  void release(SlotId) noexcept;
  [[nodiscard]] std::span<f64> column(SlotId) noexcept;
};
// The oracle: obvious, slow, allocates freely — exists only to be matched bit-for-bit.
[[nodiscard]] Result<SignalSet> evaluate_reference(const Program&, const Panel&);
}
```

**Contract.** The oracle implements every operator with the simplest correct formula (per Appendix A /
Alpha101 reference semantics), the documented NaN policy, and deterministic tie-breaking — speed irrelevant.
`SlotPool` capacity = P3-4's `peak-live-slots`; acquiring beyond it is a precondition failure (`ATX_ASSERT`).
Universe/NaN rules (§3.3–3.4) implemented **here once** and shared by oracle + fast VM.

**Tests.** panel field access (cross-section at a date; instrument column over time); validity/universe mask
honored; slot acquire/release round-trips; oracle evaluates a known fixture (hand-computed
`close/open - 1`, `rank(close)`) to expected values; NaN-in/NaN-out. Boundary: 1×1 panel; all-NaN date; empty
universe day.

**Acceptance.** `/W4 /WX` clean; green (or `blocked-on L9` staged red). Commit
`feat(p3-5): columnar eval context + reference oracle`.

---

### P3-6 — Vectorized VM core + element-wise opcodes  *(blocked-on atx-core L5 `simd`, L2 pool)*

**Scope.** The dispatch loop and the element-wise/logical/select opcode family — each a vectorized,
zero-allocation column kernel. This is the VM skeleton every later op plugs into.

**Implementation shape.**
```cpp
Result<SignalSet> Engine::evaluate(const Program& p) {
  for (DateIdx t = 0; t < panel_.dates(); ++t) {
    for (const Instr& ins : p.code) {
      switch (ins.op) {                                   // exhaustive; no default
        case OpCode::LoadField: load_field(ins, t); break;
        case OpCode::Add: kernel_add(dst(ins), a(ins), b(ins)); break;   // tight f64 loop, SIMD via L5
        // … element-wise, logical, Select …
        case OpCode::Free: pool_.release(ins.dst); break;
        // CS_* / TS_* dispatched to P3-7 / P3-8 kernels
      }
    }
  }
}
```

**Contract.** Inner kernels: contiguous `f64*` loops, no allocation, no branching on per-cell data beyond
`SELECT`/NaN; alignment for L5 SIMD; `// SAFETY:` on the slot-span reinterpret. Element-wise ops broadcast a
scalar slot over a panel column. NaN propagates per IEEE.

**Tests (differential).** every element-wise/logical/select opcode vs the P3-5 oracle on randomized fixtures
(seeded, deterministic); scalar broadcast; mask logic; `SELECT` picks per-cell; NaN propagation; **zero-alloc
on the eval path** (counting-allocator guard). Boundary: single cell; all-NaN; ±inf; `0/0`.

**Bench.** ns/cell for `add`/`mul`/`select` at panel scale; record host/build. Target ≤ ~1–2 ns/cell
amortized (§3.6); measured only.

**Acceptance.** `/W4 /WX` clean; differential-green (or `blocked-on L5` staged red); bench recorded. Commit
`feat(p3-6): vectorized vm core + element-wise opcodes`.

---

### P3-7 — Cross-sectional opcodes  *(blocked-on atx-core L6 `cross_section`)*

**Scope.** Per-date cross-sectional ops over the valid universe: `rank`, `zscore`, `scale`,
`indneutralize`/`group_neutralize` (demean **and** regression-residual), `group_rank`, `group_zscore`.

**Implementation.** Each consumes one date-row (a cross-section vector), operates over the **valid+universe
mask only**, emits NaN elsewhere. `CsRank` uses the fixed tie-break (`ordinal`+stable instrument id default;
`average` optional) with NaN-last (§3.5). `CsNeutG` implements the **general residualizer** (regress signal on
group dummies [+ log-cap]); `CsDemeanG`/`indneutralize` is the demean special case (research §6.5). Built on
atx-core L6 `cross_section` kernels.

**Tests (differential + known-value).** `rank` percentile on a hand fixture (ties → chosen method);
`zscore` mean≈0/std≈1 over valid set; `scale` Σ|x|=a; demean-by-group sums to 0 per group; residualizer
orthogonal to group dummies; **NaN/out-of-universe excluded then NaN-filled**; determinism: same input →
identical ranks across runs. Boundary: 1-instrument universe (rank degenerate); all-equal values (tie
storm); single non-NaN in a group.

**Acceptance.** `/W4 /WX` clean; differential-green (or `blocked-on L6`); determinism asserted. Commit
`feat(p3-7): cross-sectional opcodes`.

---

### P3-8 — Time-series opcodes  *(blocked-on atx-core L6 `rolling`/`online_stats`)*

**Scope.** Per-instrument trailing-window ops, causal by construction: `delay`, `delta`, `ts_sum`, `ts_mean`,
`ts_std`/`ts_var`, `ts_min`/`ts_max`, `ts_argmin`/`ts_argmax` (1-based), `ts_rank`, `correlation`,
`covariance`, `product`, `decay_linear`, plus Qlib extensions (`ema`, `wma`, `skew`, `kurt`, `med`, `mad`,
`slope`, `rsquare`, `resid`).

**Implementation.** **Incremental rolling accumulators** (atx-core L6 `rolling`/`online_stats`) give
**O(1)/cell** where possible (sum/mean/var via Welford; min/max via monotonic deque) instead of O(d)/cell
re-scan. `decay_linear` precomputes the weight vector `(d,…,1)/Σ` once. Each op reads only `[t−d+1, t]` — the
lookback the VM pre-rolled (P3-3); **a negative `d` cannot reach here** (compile-time rejected). Per-op
`min_periods` policy (§3.4) from the registry.

**Tests (differential + known-value).** `delay`/`delta` shift exactness; `ts_mean`/`ts_std` vs oracle on a
ramp; `ts_argmax` 1-based index; `ts_rank` of today in window; `correlation` on
linearly-related/anti-related series (≈+1/−1); `decay_linear` weights sum to 1; **`min_periods` boundary**
(NaN until window full); **lookahead proof** — output at `t` is byte-identical whether or not rows `>t`
exist (truncation invariance). Boundary: `d=1` (= identity/`delta`=0-diff); `d > history` (all-NaN until
filled); window crossing a NaN gap.

**Acceptance.** `/W4 /WX` clean; differential-green (or `blocked-on L6`); lookahead truncation-invariance
asserted. Commit `feat(p3-8): time-series opcodes`.

---

### P3-9 — Differential + determinism + lookahead harness · bench · sprint close

**Scope.** The proof. An integration suite proving the **fast VM == oracle**, **run-to-run determinism**, and
**no look-ahead** on a realistic multi-alpha program over a synthetic panel (incl. delisted symbols and NaN
gaps); then the parallel evaluator + bench; then the sprint-close ceremony.

**Design.**
- **Differential:** compile a battery of alphas (a subset of Alpha101 expressible from shipped ops) and assert
  `evaluate(fast) == evaluate_reference(oracle)` cell-by-cell within a documented ULP tolerance.
- **Determinism:** evaluate twice (and **once single-thread vs once multi-thread**); fold an atx-core
  `hash`/wyhash over the ordered `(alpha_id, date, instrument, value-bits)` stream; assert identical hashes.
  A **mutated** input (reorder instruments, change one value, add a late row) must yield a **different** hash
  (test not vacuously passing).
- **Lookahead:** evaluate the panel, then **truncate** it after date `t` and re-evaluate; assert outputs for
  dates `≤ t` are byte-identical (future rows provably invisible).
- **Parallelism:** morsel/date-partitioned threads sharing the immutable node-column cache; TSan where
  available (else race-clean + tracked Linux residual).

**Bench.** alphas/s and ns/cell for a mined-style set with high subexpression overlap; report **CSE
reduction** (unique/total nodes) and **cache-hit %** (§3.6). Measured only; host/build recorded.

**Sprint close (sprint.md ceremony).**
1. Lift residuals to ROADMAP future-work backlog (JIT exploration; computed-goto dispatch; Linux TSan pass;
   BRAIN-superset ops `ts_entropy`/`ts_backfill`; `signedpower` vs `x^a` edge-case audit vs the actual paper).
2. Update ROADMAP Phase 3 status table (`⏳ → ✅ <sha>` / `⚠️ partial`).
3. Bump ROADMAP `Last reviewed`.
4. Write "What Phase 3 proves / Next sprint priorities" in the ledger (baton → Phase 4 combiner).
5. Write `phase-3.md` user reference (the DSL grammar + operator table + `compile`/`Engine`/`SignalSet` API +
   the determinism + lookahead + NaN guarantees).
6. Merge worktree → master (`--no-ff`, `merge: phase-3 — alpha expression dsl`). **Do not push unless asked.**

**Acceptance.** Differential/determinism/lookahead suites green; bench recorded; close items 1–6 done; ledger
+ ROADMAP reconciled. Commit `docs(p3-close): close phase-3 — 10 units, <M> tests`.

---

## 9. Sub-agent dispatch checklist (per unit)

Each brief includes (sprint.md): worktree path + branch (`worktree-phase-3-alpha-dsl`); the unit's scope
**quoted** from this plan; acceptance criteria; the verbatim *"Marker-commit pattern is mandatory: commit
before stopping or work is lost."*; commit format `feat(p3-N): <summary>`; predecessor SHAs; the ledger-row
instructions (status, test counts, blocked-on note); and the handoff block below with the atx-core headers
(`error.hpp`, `types.hpp`, `hash.hpp`, and the as-built `series/frame.hpp`, `cross_section.hpp`, `rolling.hpp`,
`simd.hpp` when they land) as the positive style + API reference.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx):
Governed by .agents/cpp/agent.md (safety-critical-grade C++20). Use atx-core public headers
(error.hpp, types.hpp, hash.hpp, container/*.hpp, and the as-built series/cross_section/rolling/simd
headers) as the positive style + API reference: module-level intent comment, grouped types/APIs, explicit
ownership/lifetime/error contracts, concise comments that explain invariants and non-obvious control flow —
never narrate code.

Performance is a first-class requirement. The target workload is 10^5-10^9 alphas over one panel. Two levers
are mandatory by design: (1) VECTORIZED dispatch — every opcode processes a whole column at once (no
per-cell interpretation); (2) SUBEXPRESSION-DAG dedup + caching — hash-cons identical subexpressions across
all alphas, materialize each once, free by refcount. Zero allocation on the eval hot path: column slots come
from a pre-sized pool. Column-major f64 + validity bitmap; cache/alignment-friendly, branch-light SIMD loops.

No UB, no narrowing, no uninitialized vars, no owning raw pointers. const/constexpr/noexcept/[[nodiscard]]
where they hold. Expected failures (parse/type errors) -> Result<T>/Status, never throw for control flow.
Weakest sufficient type at interfaces (span/string_view/concepts). Every enum-class switch exhaustive
(OpCode/Shape/DType/TokenKind, no default); loops bounded; functions <= ~60 lines. // SAFETY: on every
deviation (slot reinterpret, union access, unchecked index in a bounds-proven loop).

Correctness rails are structural, not optional: time-series ops are causal (trailing window only; negative
lookback is a COMPILE error); cross-sectional ops operate over the valid+universe mask and propagate NaN
(never impute); rank tie-breaking and reduction order are FIXED for bit-reproducibility. Every fast-VM opcode
must match the tree-walking reference oracle bit-for-bit (documented ULP tolerance for float reductions).

TDD: failing GoogleTest first, watch it fail for the right reason, then implement. Cover happy path,
boundaries (empty/1-instrument/d=1/d>history/all-NaN), error paths (parse/arity/shape/unknown-field/
negative-lookback), invariant violations (EXPECT_DEATH). Build /W4 /WX clean; clang-tidy/format clean; tests
pass under ASan+UBSan (TSan on the parallel evaluator where supported). Zero allocation on the eval hot path.

A unit is done only when header + impl + tests + ledger row + build/test gate are all present. No TODO stubs,
no fake success paths, no untested skeletons. Prefer a smaller complete slice over a larger partial one.
```

---

## 10. Risks / watch-items

- **atx-core L5/L6/L9 contract drift** — if `series`/`cross_section`/`rolling`/`simd` ship with signatures
  differing from the stdlib spec, update engine headers to the *as-built* API and note the delta in the
  ledger (the plan is a fossil; the ledger records reality). The disruptor already proved the spec maps
  cleanly.
- **`indneutralize` semantics ambiguity** — demean-by-group vs regression-residual is unresolved in the
  paper text (research §6.5, §10). Implement the **general residualizer**; expose demean as the special
  case; record the choice in the ledger. Edge-case audit vs the actual Alpha101 PDF is a tracked residual.
- **`signedpower` vs `x^a`** — keep `SPOW = sign(x)·|x|^a` as the primitive (paper definition); some ports
  simplify to `x^a` (research §1.2 discrepancy). Document; oracle uses the paper form.
- **Float reduction reproducibility** — multi-threaded reductions must match single-thread bit-for-bit; fix
  the order (canonical instrument index + pairwise/Kahan). If a ULP delta is unavoidable for a given op,
  document the tolerance in that op's differential test (don't loosen it silently).
- **NaN policy divergence** — `min_periods`, cross-sectional exclusion, and `ts_corr` pairwise handling must
  be **one** policy implemented in P3-5 and shared by oracle + VM; divergence makes the differential test
  lie. Centralize it.
- **Slot-pool sizing** — capacity must equal P3-4's `peak-live-slots`; an off-by-one starves evaluation.
  Assert it; test the diamond-DAG worst case.
- **DSL scope creep** — the BRAIN superset (`ts_entropy`, `ts_moment`, `ts_backfill`, regions/groups galore)
  is large. Ship the **Alpha101 ∪ core-Qlib** set (Appendix A); everything else is future-work backlog. The
  registry makes adding one later a one-row change — no need to front-load.
- **LSP false positives** without `compile_commands.json` — verify against real `cmake --build`; ignore
  phantom missing-include / out-of-policy clang-tidy noise (the datetime + disruptor work both saw this).
- **JIT temptation** — do **not** add LLVM/asmjit in Phase 3. The vectorized interpreter is the baseline;
  JIT is justified only by profiling (research §5.5). Tracked as future-work, not scope.

## 11. Definition of done (Phase 3)

- P3-0…P3-9 implemented; tests pass under ASan+UBSan (TSan on the parallel evaluator where supported) — or
  blocked-on units (P3-5…P3-8) staged red behind `atx_engine_pending` with the ROADMAP cross-module table
  current.
- `/W4 /permissive- /WX` clean; clang-tidy (project policy) + format clean.
- **Differential** suite green: fast VM == tree-walking oracle bit-for-bit (documented ULP tolerance) across
  the shipped operator set and a battery of multi-op alphas.
- **Determinism** proven: identical input → identical signal-hash; single-thread == multi-thread; mutation
  detected. **Lookahead** proven: truncation-invariance (future rows invisible). NaN/universe policy enforced.
- **CSE lever** evidenced: unique/total-node ratio + cache-hit % recorded; eval bench (alphas/s, ns/cell)
  recorded with host/build context (measured only).
- Zero steady-state allocation on the eval hot path (instrumented).
- Ledger closed; ROADMAP status + `Last reviewed` updated; `phase-3.md` written; worktree merged `--no-ff`
  (not pushed unless asked).

---

## Appendix A — Operator vocabulary (the opcode ISA)

From [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md)
§8 (Alpha101 ∪ Qlib ∪ BRAIN). Shapes: **S** scalar, **V** cross-section, **P** panel. All built-ins are
lookahead-safe (causal). Ship the Alpha101 ∪ core-Qlib rows; BRAIN-superset rows are future-work.

| Operator | Arity | Shape (in→out) | Semantics | OpCode |
|---|---|---|---|---|
| `+ - * /` | 2 | P,P→P / P,S→P | element-wise arithmetic, scalar broadcast | `Add`/`Sub`/`Mul`/`Div` |
| `-x` `!x` | 1 | P→P | unary negate / logical not | `Neg`/`Not` |
| `abs` `sign` `log` | 1 | P→P | abs / −1,0,+1 / natural log | `Abs`/`Sign`/`Log` |
| `power(x,a)` | 2 | P,S→P | `x^a` | `Pow` |
| `signedpower(x,a)` | 2 | P,S→P | `sign(x)·|x|^a` | `Spow` |
| `min(x,y)` `max(x,y)` | 2 | P,P→P | element-wise min/max (Qlib `Less`/`Greater`) | `MinP`/`MaxP` |
| `< > <= >= == !=` | 2 | P,P→mask | comparison → boolean mask | `CmpLt`…`CmpNe` |
| `&& \|\|` | 2 | mask,mask→mask | logical and/or | `And`/`Or` |
| `cond ? a : b` | 3 | mask,P,P→P | ternary select (`np.where`) | `Select` |
| `rank(x)` | 1 | P→V | cross-sectional percentile rank ∈[0,1] | `CsRank` |
| `zscore(x)` | 1 | P→V | cross-sectional standardize per date | `CsZscore` |
| `scale(x,a=1)` | 2 | P,S→V | rescale so Σ|x|=a per date | `CsScale` |
| `indneutralize(x,g)` | 2 | P,group→V | demean within group per date | `CsDemeanG` |
| `group_neutralize(x,g[,cap])` | 2–3 | P,group→V | regression-residual neutralize | `CsNeutG` |
| `group_rank/zscore(x,g)` | 2 | P,group→V | rank/zscore within group | `CsRankG`/`CsZscoreG` |
| `delay(x,d)` | 2 | P,S→P | value d days ago (`Ref`) | `TsDelay` |
| `delta(x,d)` | 2 | P,S→P | `x − delay(x,d)` | `TsDelta` |
| `ts_sum(x,d)` | 2 | P,S→P | trailing-d sum | `TsSum` |
| `ts_mean(x,d)` | 2 | P,S→P | trailing-d mean (`sma`) | `TsMean` |
| `stddev(x,d)` | 2 | P,S→P | trailing-d std / var | `TsStd`/`TsVar` |
| `ts_min/ts_max(x,d)` | 2 | P,S→P | trailing-d min/max | `TsMin`/`TsMax` |
| `ts_argmin/ts_argmax(x,d)` | 2 | P,S→P | 1-based day-of-min/max in window | `TsArgMin`/`TsArgMax` |
| `ts_rank(x,d)` | 2 | P,S→P | rank of today within trailing-d window | `TsRank` |
| `correlation(x,y,d)` | 3 | P,P,S→P | trailing-d Pearson corr | `TsCorr` |
| `covariance(x,y,d)` | 3 | P,P,S→P | trailing-d covariance | `TsCov` |
| `product(x,d)` | 2 | P,S→P | trailing-d product | `TsProduct` |
| `decay_linear(x,d)` | 2 | P,S→P | linear-weighted (d..1) MA, weights Σ=1 (`WMA`) | `TsDecayLinear` |
| `ema(x,d)` | 2 | P,S→P | exponential moving average | `TsEma` |
| `skew/kurt/med/mad(x,d)` | 2 | P,S→P | trailing higher moments / median / MAD | `TsSkew`… |
| `slope/rsquare/resid(x,d)` | 2 | P,S→P | trailing rolling-regression stats | `TsSlope`/`TsRsquare`/`TsResid` |

**Data fields (LOAD_FIELD leaves):** `open high low close volume vwap returns cap adv{d}` +
`IndClass.{sector,industry,subindustry}` (group dtype). ⚠️ `indneutralize` exact variant and per-op NaN
policy are engine choices (research §10) — see §3.4, §10.

---

## Appendix B — Open-source reference index

| System | What we took | Primary source |
|---|---|---|
| Alpha101 (Kakushadze) | the operator vocabulary (cross-sectional + time-series + element-wise) | arxiv.org/abs/1601.00991; ref impl github.com/yli188/WorldQuant_alpha101_code |
| Microsoft Qlib | string-DSL-over-panel; `Op(args…)`/`Feature` tree; rolling causal ops; per-expr memoization | github.com/microsoft/qlib `qlib/data/{ops,base,data}.py`; qlib.readthedocs.io |
| Zipline Pipeline | Term-DAG memoized by structure (free CSE); `extra_input_rows` lookback; `window_safe` guard; Factor/Filter/Classifier shapes | github.com/quantopian/zipline `zipline/pipeline/term.py` |
| gplearn | arity-typed function set; flat program trees; why subexpression sharing is the lever | gplearn.readthedocs.io |
| Crafting Interpreters (Nystrom) | Pratt parser (`ParseRule` table); bytecode stack VM; computed-goto dispatch | craftinginterpreters.com/compiling-expressions.html, /a-virtual-machine.html |
| Pratt / TDOP | top-down operator precedence (= precedence climbing) | Pratt POPL 1973; oilshell.org/blog (Pratt==prec-climbing) |
| DuckDB / Vectorwise-X100 | vectorized interpretation (batch per opcode); morsel-driven parallelism | duckdb.org/why_duckdb; duckdb.org/docs/stable/internals/vector |
| Kersten et al. VLDB'18 | compiled-vs-vectorized: neither dominates; vectorization avoids compile latency → interpreter-first | vldb.org/pvldb/vol11/p2209-kersten.pdf |
| Sethi–Ullman / CSE | shared interior node = common subexpression; refcount-driven free schedule | en.wikipedia.org/wiki/Common_subexpression_elimination |
| Apache Arrow compute | contiguous columnar arrays + validity bitmap; propagate-don't-impute NaN | arrow.apache.org |
| pandas rolling/rank | `min_periods` NaN policy; rank tie-breaking methods | pandas.pydata.org (rolling, rank) |

*Full URL index and confidence tags: [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md) §11.*
