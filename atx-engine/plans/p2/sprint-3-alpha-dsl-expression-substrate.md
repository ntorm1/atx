# Sprint S3 — Alpha DSL & Expression Substrate (sprint spec)

**Status:** ⏳ proposed (not open). Depends on **p0 Phase-3** (the DSL/VM/oracle) + **p1 S3** (`factory::OpCatalog`,
the genetic miner). Opens first on the `p2` **alpha-depth track** (the spine's first half). Spine partner: **S4**
(genetic search & robust signal pipeline) searches the space S3 widens.
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md)
§1 (WorldQuant Fast Expression / Alpha101), §1.4 (the BRAIN operator superset), §2/§3 (Qlib & Zipline operator
hierarchies — the architectural template), §6.5 (cross-sectional neutralization: demean vs regression-residual), §8
(the WorldQuant ∪ Qlib ∪ Alpha101 operator union table) ·
[`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) §3.3 (operator
catalog).

---

## Why this sprint

`p1` built a *working* alpha factory. `p2` must build a *deep* one — and depth starts with the **language**. The
as-built DSL (`alpha::` — lexer → Pratt parser → typecheck → hash-consed DAG → bytecode → columnar VM, with a
tree-walking differential oracle) is production-grade: 65 operators, causal-by-construction, bit-exact (`oracle == VM`),
allocation-free on the hot path. But it is expressively shallow in three load-bearing ways, and a genetic search can
only ever discover signals its language can *express*:

1. **Neutralization is demean-only.** The DSL has `indneutralize`/`group_neutralize` as *group-demean* (subtract the
   per-group mean). The standard size/sector-neutral factor construction is **regression-residual** — regress the
   signal on group dummies (and a style block: log-cap, beta) and keep the residual (research §6.5). Demean is the
   special case where the only regressors are group indicators. Without the general residualizer, the factory cannot
   express the most common professional neutralization, and every "sector-neutral" alpha it mines is the weaker
   demean variant.

2. **The time-series vocabulary is missing the BRAIN superset.** The as-built `ts_*` family is rich (30+ rolling
   ops) but lacks the operators the live WorldQuant surface and the Alpha101 corpus lean on: `ts_regression`
   (rolling β/α/residual of x on y), `ts_decay_exp` (exponential-window decay, distinct from the linear
   `decay_linear`), `ts_entropy`, `ts_moment`, and a parity audit of `ts_backfill`/`ts_quantile` (research §1.4, §8).
   A richer datafield family — `vwap`, `adv{d}` (average daily dollar volume), explicit `volume`-derived fields —
   is likewise thin; the Alpha101 set uses them constantly (research §1.2).

3. **The mutable operator space is silently narrowed.** The genetic miner runs with **`op_swap` disabled by default**
   (`SearchConfig::enable_op_swap = false`) — a p1 S3-1 residual: an analyze-valid genome that corrupts the VM
   SlotPool at evaluate (an uncatchable abort isolated to `op_swap`; `field_swap`/`jitter_const`/crossover are
   clean). With `op_swap` off, the search cannot mutate one operator into another — it can only swap fields and jitter
   constants — so a huge swath of the expression space is unreachable. Fixing this is not cosmetic; it is **restoring
   half the search operators**.

S3 deepens the substrate so S4's search explores a **richer, correctly-typed, fully-mutable** space. It adds the
general residualizer, the BRAIN-superset operators (each with an oracle reference, lookback-rail causality, and an
explicit NaN policy — the existing discipline), the missing datafield family, **fixes and re-enables `op_swap`**, and
moves candidate generation from *random-then-reject* to **grammar-typed (valid-by-construction)**. It closes on a
differential-conformance suite plus an **Alpha101-subset reproduction** fixture proving the engine can actually express
the canonical public alphas.

The non-negotiable: this is **additive**. Every pre-existing `p1` formula must compile and evaluate **bit-for-bit
identically** after S3 — the `oracle == VM` differential equality is the non-regression anchor on the proven 65-op
core.

---

## Scope — units

### S3.0 — Marker + ledger + as-built recon
Open `sprint-3-progress.md`, freeze scope, base SHA. As-built recon against `alpha::{registry, vm, oracle, ts_ops,
cs_ops, typecheck, dag, bytecode}` and `factory::{op_catalog, mutation}`: enumerate the current 65-op registry, the
oracle/VM kernel pairing, the lookback-rail mechanics in `typecheck`, the `OpCatalog` `(shape, dtype, arity)`
bucketing, and the exact `op_swap` failure path (the SlotPool corruption — reproduce it under a test first). Produce
the operator-gap table (BRAIN superset ∪ Alpha101 ∪ Qlib minus as-built) that S3.1–S3.3 burn down.

### S3.1 — Regression-residual neutralization
A general per-date **cross-sectional residualizer**: at each date, regress the signal cross-section on a design matrix
of **group dummies** (sector/industry/subindustry) plus an optional **style block** (log market-cap, beta), over the
valid universe only, and emit the **residual**. Expose `indneutralize(x, g)` as the demean-only special case (design
matrix = group indicators alone) so existing formulas are unchanged. New op surface: `cs_residualize(x, g[, style…])`
(name TBD at kickoff). **Causality + PIT:** the regression is fit on date-`t`'s cross-section only (no trailing fit —
it is a within-date residual, inherently look-ahead-safe), valid-set-only (NaN out for excluded instruments, never
impute). **Differential:** an obviously-correct oracle (normal-equations / QR over the valid set) and a bit-/ULP-bounded
VM kernel; the demean special case must match the existing `cs_group_demean_row` bit-for-bit. *atx-core Pattern-B edge:
L7 `cs_residualize`; engine-side QR/normal-equations fallback over `core::linalg`.*

### S3.2 — BRAIN-superset time-series operators
Add the missing `ts_*` operators, each as an `(oracle, VM)` kernel pair in the existing `ts_ops.hpp` pattern, with
lookback propagation through `typecheck` and an explicit `min_periods`/NaN policy:
- `ts_regression(y, x, d[, mode])` — rolling-window regression of `y` on `x`; emits slope (default), or intercept /
  residual / R² via a mode selector (reuses the `slope`/`rsquare`/`resid` math already present, generalized to a
  2-input rolling regression).
- `ts_decay_exp(x, d[, f])` — exponential-window weighted MA (weights `f^k` over the trailing window), distinct from
  the linear `decay_linear`.
- `ts_entropy(x, d[, buckets])` — rolling Shannon entropy of the windowed distribution.
- `ts_moment(x, d, k)` — rolling k-th central moment (generalizes the existing `skew`/`kurt`).
- Parity audit + (if needed) fix of `ts_backfill` and `ts_quantile` against the research §1.4 semantics.
Each ships oracle parity + a lookahead-rail test (only `[t−d+1 … t]` read) + a NaN-policy test.

### S3.3 — Cross-sectional/vector gap-fill + datafield family
Two threads:
- **Operators:** `quantile(x[, n])` (cross-sectional quantile bucketing), `reverse(x)` (sign/rank reversal),
  `vec_sum`/`vec_avg` (vector reductions), and winsorize/normalize variants from the research §8 union table not yet
  present. Oracle + VM + valid-set semantics each.
- **Datafields:** a `vwap`, `adv{d}` (average daily dollar volume over `d` days), and explicit `volume`-derived field
  family on the `Panel`, with the same PIT/NaN/universe discipline as the existing OHLC fields. These are the raw
  inputs the Alpha101 corpus (research §1.2) and S3.6's reproduction fixtures need.

### S3.4 — Fix the `op_swap` VM-corruption defect + re-enable
Root-cause and repair the p1 S3-1 defect: an `op_swap`-mutated genome that passes `analyze()` but corrupts the VM
SlotPool at evaluate. The likely seam is a typecheck/bytecode invariant the op-swap rebuild violates (a shape/arity/
slot-liveness mismatch the analyzer doesn't catch). Close the gap at the **root** (tighten `typecheck`/`bytecode` so an
analyze-valid genome is *always* VM-safe — the preferred fork) or, as fallback, constrain `op_swap` to provably-safe
`(shape, dtype, arity)` buckets. Re-enable `op_swap` in `OpCatalog`/`SearchConfig`. **Proof:** a differential VM/oracle
stress harness that op-swaps across **every** `(shape, dtype, arity)` bucket and asserts `oracle == VM` (no abort, no
corruption) on each — the test that would have caught the original defect.

### S3.5 — Grammar-typed expression generation
Move candidate generation from *random-then-reject* (build a random AST, `analyze()`, drop the invalid) to
**valid-by-construction**: a shape/dtype-aware generator that only ever emits well-typed, causal genomes by sampling
from the `OpCatalog` buckets respecting the shape lattice (Scalar / CrossSection / Panel) and dtype (F64 / Mask /
Group) at each node. Extends the existing `OpCatalog` bucketing to a generative grammar. **Payoff:** higher generation
yield (fewer wasted `analyze()` rejections), a tighter search space for S4, and the foundation the multi-objective
search builds on. Deterministic, seeded (the seed axis composes with S4's `(master, run, gen, idx)`).

### S3.6 — Differential-conformance suite + Alpha101 reproduction + bench + close
- **Conformance:** the `oracle == VM` differential equality, run across the **entire widened operator set** (every new
  S3.1–S3.4 op, every bucket), as the non-regression gate.
- **Alpha101 reproduction:** encode a verified **subset** of the canonical 101 alphas (research §1.2, the `yli188`
  reference semantics) as DSL formulas and prove they compile, typecheck, and evaluate on a fixture panel — the proof
  the substrate can actually express professional alphas. (Subset, not all 101 — pick the ones whose operators S3 now
  supports; record which 101-alphas are *not yet* expressible and why, as the residual.)
- **Bench:** DSL eval throughput on the widened op set; confirm the hot-path-allocation-free + CSE properties hold.
- **Close** ceremony.

---

## Exit criteria

- Regression-residual neutralization ships; `indneutralize` reduces to it bit-for-bit in the demean special case;
  the residualizer's oracle and VM agree to ULP bound.
- The BRAIN-superset `ts_*` operators (`ts_regression`, `ts_decay_exp`, `ts_entropy`, `ts_moment`) + the datafield
  family (`vwap`, `adv{d}`, volume-derived) ship, each with `oracle == VM` parity, a lookahead-rail test, and a NaN
  policy.
- **`op_swap` is fixed and re-enabled** (`enable_op_swap = true` by default), proven by the per-bucket differential
  stress harness — no VM corruption on any `(shape, dtype, arity)` bucket.
- Grammar-typed generation emits only analyze-valid genomes (measured rejection rate ≈ 0, vs the random-then-reject
  baseline, reported in the ledger).
- The full-operator differential-conformance suite is green (`oracle == VM` bit-exact across the widened set).
- An Alpha101 **subset** reproduces (compiles + evaluates) on a fixture; the not-yet-expressible remainder is recorded
  with reasons.
- **Non-regression:** every pre-existing `p1` formula compiles and evaluates **bit-for-bit identically** (the proven
  65-op core is untouched in behavior).
- `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit.

## Invariants this sprint must prove

- **No look-ahead.** The residualizer is a within-date cross-sectional fit (no trailing leakage); every new `ts_*` op
  reads only `[t−d+1 … t]` (the lookback rail). Truncation-invariance test per new op.
- **Determinism.** Grammar-typed generation is seeded and order-fixed; the seed is part of the artifact. No new RNG
  site on the eval path.
- **Differential correctness.** Every new kernel (residualizer, each `ts_*`, each cross-sectional op) ships an
  obviously-correct oracle and a bit-/ULP-bounded differential test; `oracle == VM` is the extended gate.
- **No survivorship / PIT.** New datafields read NaN out-of-universe / not-yet-knowable, exactly like OHLC.
- **No hot-path allocation.** New VM kernels allocate zero in steady state (slot-pool discipline preserved).

## Dependencies

- **Upstream:** p0 Phase-3 (`alpha::{lexer, parser, typecheck, dag, bytecode, vm, oracle, registry, panel}`), p1 S3
  (`factory::{OpCatalog, mutation, genome}`).
- **atx-core (Pattern B edges):** **L7 `cs_residualize`** (S3.1 — engine-side QR/normal-equations fallback);
  **L6 `rolling_ext`** for `ts_regression`/`ts_entropy`/`ts_moment` (S3.2 — engine-side strided-window kernels with
  oracle parity, the existing pattern). Both ship engine-side fallbacks; the atx-core kernels are accelerators, not
  blockers.

## Explicitly NOT in this sprint

- **No search changes.** S3 widens the *language*; S4 makes the *search* smarter. Multi-objective fitness, behavioral
  diversity, cost-aware mining, and the robustness battery are all S4.
- **No new signal families.** Deep-learning alphas are S5; learned/ML alphas already exist (p1 S5).
- **No alt-data fields.** Fundamental/analyst/news datafields are S6; S3's datafield family is price-volume-derived
  (`vwap`/`adv`/`volume`), matching the verified Alpha101 inputs.
- **No all-101 reproduction.** S3.6 reproduces a *subset* gated by the operators S3 supports; full coverage (and any
  remaining exotic operators) is a recorded residual, not a S3 commitment.
- **No JIT / dispatch rewrite.** The vectorized bytecode interpreter is retained (research §5.5: defer JIT until
  profiling proves the interpreter is the bottleneck — it won't be at this op granularity).

## Baton → next

S3 hands **S4** a richer, correctly-typed, fully-mutable expression space: regression-residual neutralization, the
BRAIN `ts_*` superset, the `vwap`/`adv`/volume datafields, a repaired `op_swap` (the full mutation set restored), and
grammar-typed generation. S4's multi-objective search and robustness battery operate on exactly this widened substrate
— a deeper language to find deeper, more robust alpha in.
