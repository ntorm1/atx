# Sprint S3 (p2) — Alpha DSL & Expression Substrate — Implementation Plan

**Status:** frozen at kickoff 2026-06-13 (`feat/p2-s3-alpha-dsl`, base `main @ 0790157`).
**Spec (the *what*):** [`sprint-3-alpha-dsl-expression-substrate.md`](sprint-3-alpha-dsl-expression-substrate.md) ·
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md) ·
**Quality bar:** [`../docs/implementation-quality.md`](../docs/implementation-quality.md) ·
**C++ authority:** [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md) ·
**Engine deltas:** [`../../../.agents/atx-engine/agent.md`](../../../.agents/atx-engine/agent.md)
**Grounded in:** [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md)
§1/§1.4 (WorldQuant Fast Expression / BRAIN superset), §2/§3 (Qlib & Zipline operator hierarchies), §6.5
(cross-sectional neutralization: demean vs regression-residual), §8 (the union operator table).

This document is the frozen *how*. The spec is the *what* and does not change; this plan records the as-built
reconciliation, the per-unit algorithms, and the gates. Where the spec left a name/fork "TBD at kickoff," this
plan resolves it (§0, §4).

---

## §0 — As-built reconciliation (recon fixes)

Recon was run against `alpha::{registry, typecheck, cs_ops, ts_ops, oracle, vm, dag, bytecode, panel}` and
`factory::{op_catalog, mutation, genome, search_driver}` at base SHA. The findings below correct the spec's
working assumptions and pin the exact seams each unit edits.

### 0.1 The operator add-surface is a fixed 9-site checklist
A new built-in operator touches exactly these sites (verified against `ts_mean` and `indneutralize`):
1. `include/atx/engine/alpha/registry.hpp` — add the `OpCode` enumerator (the exhaustive switches below force every
   consumer to handle it — that is the feature, not a chore).
2. `src/alpha/registry.cpp` — add the `OpSig` row to `builtin_ops()` (`kOps`; bump the `std::array<OpSig, N>` size).
3. `include/atx/engine/alpha/registry.hpp` — reuse an existing `shape_of` rule (`shape_panel` / `shape_cross_section`
   / `shape_elementwise` / `shape_unary`) or add one if the family is new.
4. `src/alpha/typecheck.cpp` — register in `detail::is_rolling_ts` / `is_shift_ts` if windowed; add per-operand dtype
   pinning in `analyze_call` / `validate_stateful_op_dtypes` if it has a role contract; add range checks in
   `validate_hparam_ranges` if it peels hparams.
5. `include/atx/engine/alpha/{ts_ops,cs_ops}.hpp` — the production kernel (`tsv_*` / `cs_*_row`), `inline`,
   valid-set-only, allocation-free in steady state.
6. `include/atx/engine/alpha/vm.hpp` — add the `case OpCode::X` to the `dispatch` switch and wire it to
   `eval_time_series` / `eval_cross_section` (and the inner kernel-family switch).
7. `include/atx/engine/alpha/oracle.hpp` — add the `case OpCode::X` to the oracle dispatch.
8. `src/alpha/oracle.cpp` — the obviously-correct tree-walking reference kernel.
9. `tests/alpha_*_test.cpp` — the per-op `oracle == VM` differential + lookahead-rail + NaN-policy tests.

This checklist is the backbone of S3.1–S3.3. Every exhaustive `switch` over `OpCode` (in `vm.hpp` dispatch,
`oracle.hpp` dispatch, `typecheck.cpp::is_rolling_ts`, the canonical/unparse paths) has **no `default`**, so the
compiler enumerates every site that needs a new case — adding an `OpCode` without handling it fails `/WX`.

### 0.2 Neutralization is genuinely demean-only, and the code says so
`cs_ops.hpp::cs_group_demean_row` (lines 156–176) computes the per-group mean and subtracts it; the header contract
explicitly states *"the WLS residual on a pure group-dummy design IS the demean … the full residualizer is
deferred."* Both `indneutralize` (`CsDemeanG`) and `group_neutralize` (`CsNeutG`) route to it. So S3.1 is a genuine
generalization, not a rewrite: the new residualizer must reduce **bit-for-bit** to `cs_group_demean_row` when the
design matrix is group dummies alone. That equality is S3.1's load-bearing regression test.

### 0.3 The `op_swap` defect is a structural-contract gap, not a bytecode bug — root cause confirmed
The miner ships `SearchConfig::enable_op_swap = false` (`search_driver.hpp:116`). Reproduced root cause:

- `OpCatalog` buckets named ops by **`(shape_cat, out_dtype, min_arity)`** (`op_catalog.hpp:214`), but `op_swap`
  looks up using the **materialized** `call_arity(tnode)` (`mutation.cpp:103`), which counts only present `a/b/c`
  children. A **hparam-peeling op** (`kalman_level`, `ou_filter`, `kalman`) declares `min_arity = 3/4` but its parsed
  node carries materialized arity **1–2** (Q/R/δ peeled into `Expr::hparams`, their literal nodes orphaned). So a
  `kalman_level` node (materialized arity 1) is looked up in the **arity-1** bucket and can be swapped for `hump`
  (min_arity 1) — a different operand/immediate contract entirely.
- `analyze_call` (`typecheck.cpp:277`) validates operand **shape/dtype**, hparam **finiteness**, and hparam
  **ranges**, but **never checks the node's structure matches its op's declared contract**: it does not assert that a
  Call carries exactly the operands its op requires, nor that `e.n_hparams == e.op->n_hparams`, nor that the orphaned
  hparam literals actually exist. `op_swap` overwrites `e.op/e.opcode/e.n_hparams` (`mutation.cpp:109–113`) but
  leaves `e.a/b/c` and `e.hparams[]` stale. The result analyzes clean yet, at VM eval, the kernel reads an operand
  slot or an immediate the node never carried → **SlotPool out-of-range / uninitialized read** (`panel.hpp:206`
  `ATX_ASSERT(live_ < capacity_)` or a garbage immediate).

**Fix (root, the preferred fork).** Two complementary edits, both at the root:
- **(a) Analyzer hardening (load-bearing):** add a `validate_node_contract` step to `analyze_call` that rejects any
  Call whose structure does not match `e.op`'s declared contract — `e.n_hparams == e.op->n_hparams`, the required
  operand slots present (materialized non-hparam arity within `[min_arity − n_hparams, max_arity − n_hparams]`), and
  the group/mask role operands typed. This makes **analyze-valid ⟹ VM-safe for every mutation path**, not just
  `op_swap` — the true root fix, and the test that would have caught the original defect.
- **(b) Catalog refinement (efficiency + defense-in-depth):** refine the `OpCatalog` bucket key to the **full swap
  contract** — `(shape_cat, out_dtype, materialized_arity, n_hparams, operand-dtype-signature)` — and **exclude
  `n_hparams > 0` ops and record ops** from the swappable set (their immediate/pin contract cannot be reconstructed
  by a structure-only swap). With (b), `op_swap` rarely proposes a contract-violating candidate; with (a), one that
  slips through is rejected by analyze rather than aborting the VM.

S3.4 ships both, then re-enables `enable_op_swap = true`, proven by the per-bucket differential stress harness.

### 0.4 The lookback rail is centralized — new windowed ops register in one switch
`analyze_call` computes lookback as `max_child_lookback` plus, for windowed ops, the window: `is_shift_ts ⇒ d + child`
(`delay`/`delta`), `is_rolling_ts ⇒ (d−1) + child` (every trailing-window op). A new rolling `ts_*` op **must** be
added to `detail::is_rolling_ts` (`typecheck.cpp:13`) or its lookback under-counts and the causality rail leaks. The
window literal is validated/floored/clamped to `[1, 65535]` by `window_value` (`typecheck.cpp:104`) — reused as-is.

### 0.5 Datafields are name-recognized panel inputs, not opcodes
`panel.hpp` resolves fields by name (`field_id`); dtype is `Group` iff `is_group_field(name)`, else `F64`
(`typecheck.cpp:354`). There are **no hardcoded OHLC accessors** — `close`/`open`/`high`/`low`/`volume` are just
F64 columns the caller supplies. So S3.3's `vwap`/`adv{d}` are **input datafields**, not VM ops: the unit adds a
panel-side **derived-field helper** (compute `vwap`, `dollar_volume = close·volume`, `adv{d} = ts_mean(dollar_volume,
d)` from OHLCV when not supplied) plus the field-name plumbing and the same NaN/PIT/universe discipline. No new
`OpCode`. (Rationale: Alpha101 treats `adv20` as a datafield, research §1.2; deriving it on the panel keeps the VM
op set price-volume-clean and avoids a stateful "field that is secretly a rolling op" in the ISA.)

### 0.6 Generation is currently parser-only — there is no AST generator yet
The factory mutates/crosses existing genomes (`mutation.cpp`, `crossover.cpp`) and seeds from parsed expressions;
there is **no from-scratch random-AST generator** in `factory/`. So S3.5 is **additive** (a new `generate.hpp`/`.cpp`),
not a replacement of an existing random-then-reject generator. The "random-then-reject baseline" the spec measures
against is the *conceptual* alternative the grammar-typed generator must beat; S3.5 builds the generator and reports
its analyze-rejection rate (target ≈ 0) against a random-AST control built inside the test.

### 0.7 The differential oracle and VM are already the gate — S3 widens it, never weakens it
`oracle.hpp`/`vm.hpp` re-state the SAME numeric policy in two independent code paths; the P3-6/7 differential tests
assert every cell agrees (NaN==NaN). The two paths are deliberately in the same `detail` namespace via distinct
names to avoid ODR (`cs_ops.hpp` header note). Every new S3 kernel ships **both** paths and joins the differential
suite. The non-regression anchor: the existing 65-op core is byte-for-byte untouched (S3.6 reproduces a frozen p1
formula corpus and asserts identical output).

---

## §1 — Design rules (the substrate-widening contract)

1. **Additive, never mutative on the proven core.** No existing `OpCode` changes shape/dtype/kernel/lookback
   semantics. New ops append to the registry; new datafields append to the panel. The p1 formula corpus is a frozen
   regression fixture (S3.6).
2. **Every kernel ships an obviously-correct oracle and a bit-/ULP-bounded VM twin.** `oracle == VM` is the gate.
   For exact-arithmetic kernels (counts, ranks, gather-sum in a fixed order) the bound is **bit-exact**; for kernels
   with a genuinely order-sensitive reduction (regression normal-equations, entropy logs) the bound is a documented
   ULP/relative tolerance, and both paths use the **same** summation order so they still agree bit-for-bit where
   possible (the cs_ops/oracle convention).
3. **Causality is a type, not a convention.** Every windowed op registers in `is_rolling_ts`/`is_shift_ts`; the
   lookback rail propagates and the root `required_lookback` gates the warm-up. A truncation-invariance test per new
   op proves no read past `[t−d+1 … t]`.
4. **Valid-set-only, NaN out, never impute.** New kernels operate on the per-date/per-window valid set (non-NaN =
   universe ∩ knowable), write every output cell (recycled scratch), and emit NaN out-of-set. No survivorship.
5. **Determinism.** Grammar-typed generation is seeded; the seed axis composes with S4's `(master, run, gen, idx)`.
   No new RNG site on the eval path. Reductions are order-fixed.
6. **No hot-path allocation.** New VM kernels reuse the slot-pool scratch; any per-row/per-window scratch is built
   once and reused (the `cs_ops` pattern). The cold paths (generation, catalog build, residualizer design-matrix
   assembly) may allocate.
7. **atx-core is the home of general primitives; the engine ships a self-contained fallback.** The QR/least-squares
   and strided-window kernels are recorded as Pattern-B lifts (§2.1) but **shipped engine-local** so S3 never blocks
   on an atx-core change; the lift note says "promote when atx-core lands it."

---

## §2 — File structure

### 2.1 atx-core / Pattern-B requests (recorded; engine ships the fallback)
- **L7 `cs_residualize`** (S3.1): a per-date cross-sectional least-squares residual (`r = y − X(XᵀX)⁻¹Xᵀy` over the
  valid set, via normal equations or thin QR). Engine ships the fallback in `cs_ops.hpp` over an engine-local small
  dense solver; recorded for promotion to `atx-core` L7.
- **L6 `rolling_ext`** (S3.2): strided trailing-window primitives for `ts_regression` (2-input rolling OLS),
  `ts_entropy` (windowed histogram), `ts_moment` (windowed central moment). Engine ships the fallback in `ts_ops.hpp`
  following the existing `tsv_*` strided-window pattern; recorded for promotion to `atx-core` L6.

Both are accelerators, not blockers — the engine-local kernels are the proof-of-correctness reference regardless.

### 2.2 Engine files (this sprint builds/edits these)
| Unit | New | Edited |
|---|---|---|
| S3.1 | — | `registry.hpp` (`CsResidualize` opcode), `registry.cpp` (row), `cs_ops.hpp` (`cs_residualize_row` + reduce-to-demean), `vm.hpp` (dispatch+wire), `oracle.hpp`/`oracle.cpp` (reference), `typecheck.cpp` (group/style arg roles) |
| S3.2 | — | `registry.hpp`/`registry.cpp` (`TsRegression`, `TsDecayExp`, `TsEntropy`, `TsMoment` + modes), `ts_ops.hpp` (kernels), `vm.hpp`, `oracle.hpp`/`oracle.cpp`, `typecheck.cpp` (`is_rolling_ts` membership, mode/arg validation), parity audit of `TsBackfill`/`TsQuantile` |
| S3.3 | `alpha/datafields.hpp`/`.cpp` (derived-field helper: `vwap`, `dollar_volume`, `adv{d}`) | `panel.hpp`/`panel.cpp` (field plumbing if needed), `registry.hpp`/`registry.cpp` + kernels for the cross-sectional gap-fill ops (`quantile`, `reverse`, `vec_*`, winsorize/normalize variants per research §8 not yet present) |
| S3.4 | `tests/alpha_op_swap_stress_test.cpp` | `typecheck.cpp` (`validate_node_contract` in `analyze_call`), `op_catalog.hpp` (refined bucket key + n_hparams/record exclusion), `mutation.cpp` (materialized-arity lookup aligned to the refined key), `search_driver.hpp` (`enable_op_swap = true`) |
| S3.5 | `factory/generate.hpp`/`.cpp` (grammar-typed generator) | `op_catalog.hpp` (expose buckets for generative sampling if needed) |
| S3.6 | `tests/alpha_conformance_suite_test.cpp`, `tests/alpha101_repro_test.cpp`, `bench/alpha_widened_bench.cpp`, `tests/fixtures/alpha101_subset.txt` | — |

Tests/benches are auto-globbed (`tests/*_test.cpp`, `bench/*_bench.cpp`, CONFIGURE_DEPENDS) — **do not hand-edit
CMakeLists.txt**. New `src/alpha/*.cpp` TUs are picked up by the existing engine glob; confirm at first build.

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`)
`alpha_cs_residualize_test.cpp` (S3.1), `alpha_ts_brain_test.cpp` (S3.2), `alpha_datafields_test.cpp` +
`alpha_cs_gapfill_test.cpp` (S3.3), `alpha_op_swap_stress_test.cpp` (S3.4), `factory_generate_test.cpp` (S3.5),
`alpha_conformance_suite_test.cpp` + `alpha101_repro_test.cpp` (S3.6). Each: happy path, boundaries (empty/singleton
valid set, window = 1, all-NaN row, single group), the load-bearing invariant proof, and `EXPECT_DEATH` for any new
`ATX_ASSERT` precondition.

### 2.4 Ledger
`sprint-3-progress.md` — opened in S3.0 (marker), one row per unit with SHA + test count + the measured numbers
(dedup/rejection rates, bench throughput) on close.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

**Gates (a unit is done only when ALL hold):**
- TDD: failing GoogleTest first, for the right reason, then implement.
- `oracle == VM` differential green for every new/edited kernel (bit-exact, or documented ULP bound with shared
  summation order).
- Lookahead-rail / truncation-invariance test for every windowed op; NaN-policy test for every kernel.
- Non-regression: the p1 formula corpus evaluates byte-identically (full from S3.6; spot-checked each unit).
- Determinism: seeded paths replay byte-identically; no new eval-path RNG.
- `/W4 /permissive- /WX` + `/fp:precise` clean; clang-format clean. **Do not run clang-tidy** (disabled repo-wide).
- Functions ≤ ~60 lines, exhaustive switches (no `default` over `OpCode`), `const`/`constexpr`/`noexcept`/
  `[[nodiscard]]`, `Result<T>` at boundaries, zero hot-path alloc.
- Commit `feat(s3-M): …` with the `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` trailer.
  Do not push. Stage explicit pathspecs (one branch per worktree — no shared-branch race here, but keep the habit).

**Handoff block (paste into every coding sub-agent brief):**
> Worktree `C:\Users\natha\atx-wt\p2-s3` (branch `feat/p2-s3-alpha-dsl`). Build env: `cmd /c "call
> C:\Users\natha\atx-wt\p2-s3-env.cmd && <cmake/ctest>"`. Build: `cmake --build --preset dev --target
> atx-engine-tests`. Test: `ctest --preset dev -R <Suite> --output-on-failure`. C++ authority: `.agents/cpp/agent.md`
> (no UB, TDD, `Result<T>`, exhaustive switches, ≤60-line fns, zero hot-path alloc). Engine deltas:
> `.agents/atx-engine/agent.md` (heap-allocate `EventBus<>`; dangling spans alias backing storage — copy out before
> growth; fixed-iteration determinism). Add an operator via the §0.1 nine-site checklist. Every kernel ships an
> oracle twin; `oracle == VM` is the gate. NaN out-of-set, never impute. No CMakeLists edits (auto-glob).

---

## §4 — Architecture & algorithms (per-unit)

### 4.1 S3.1 — Regression-residual neutralization (`CsResidualize`)
**Op:** `cs_residualize(x, g[, style…])` — arity (2, 2+k). `x` = signal (Panel→V), `g` = Group classifier, optional
style block = F64 panel columns (log market-cap, beta). Output CrossSection, `shape_cross_section`. Per date `t`:
1. Build the valid set (cells where `x`, `g`, and every style column are non-NaN). Out-of-set → NaN.
2. Design matrix `X` over the valid set: one indicator column per distinct group label present + one column per style
   regressor (mean-centered) + (no explicit intercept — group dummies span it; matches the demean special case).
3. Solve least squares `β = argmin ‖y − Xβ‖²` via **normal equations** `XᵀX β = Xᵀy` with a small dense Cholesky/LDLᵀ,
   falling back to a tiny **column-pivoted QR** when `XᵀX` is singular (collinear groups/styles) — the rank-deficient
   columns get zero coefficient, never NaN-poison the residual.
4. Residual `r = y − Xβ` written to the valid cells; NaN elsewhere.

**Demean reduction (the regression test):** with no style block, `X` = group indicators only ⇒ `β_j` = group-`j`
mean ⇒ `r_i = x_i − mean(group(i))` — **identical to `cs_group_demean_row`**. The test asserts bit-for-bit equality
on a fixture with ≥3 groups and ragged sizes (the normal-equations path must reproduce the demean's exact summation;
if a ULP gap appears, the demean special case is **dispatched to the existing `cs_group_demean_row` directly** when
the style block is empty, guaranteeing the boundary pin — preferred).

**Causality/PIT:** the fit uses date-`t`'s cross-section only — inherently look-ahead-safe (no trailing fit). The
oracle is the obvious dense normal-equations solve; the VM kernel shares the summation order. ULP bound documented;
demean special case is bit-exact.

### 4.2 S3.2 — BRAIN-superset time-series ops
Each is a strided trailing-window kernel in the `ts_ops.hpp` `tsv_*` pattern, registered in `is_rolling_ts`, lookback
`(d−1)+child`, full-window `min_periods` (NaN until the window fills) except where noted:
- **`ts_regression(y, x, d[, mode])`** — rolling OLS of `y` on `x` over `[t−d+1, t]`. `mode` (peeled hparam, default
  0=slope): 0 slope `β`, 1 intercept `α`, 2 residual `y_t − (α+β x_t)`, 3 `R²`. Reuses the `slope`/`rsquare`/`resid`
  math (already present as 1-input `TsSlope`/`TsRsquare`/`TsResid` against an implicit time axis), generalized to a
  2-input regression. Degenerate window (zero x-variance) → NaN.
- **`ts_decay_exp(x, d[, f])`** — weights `wₖ = fᵏ` for `k = 0…d−1` (k=0 most recent), normalized `Σwₖ`. Distinct
  from `decay_linear` (linear weights). `f` (peeled hparam, default 0.5) ∈ (0,1]; validated in `validate_hparam_ranges`.
- **`ts_entropy(x, d[, buckets])`** — Shannon entropy of the windowed value distribution: bucket the `d` values into
  `buckets` (default 10) equal-width bins over `[min,max]` of the window, `H = −Σ pᵢ log pᵢ` (natural log; empty bins
  skipped). Documented ULP bound (log sum order fixed).
- **`ts_moment(x, d, k)`** — `k`-th central moment `(1/d)Σ(xⱼ−μ)ᵏ` over the window; generalizes `skew`/`kurt` (which
  stay as their own normalized ops). `k` is a required peeled hparam (integer ≥ 1).

**Parity audit:** `ts_backfill` (looks past NaNs to the most recent valid in window) and `ts_quantile` are checked
against research §1.4 semantics; if the as-built kernel diverges, fix the kernel **and** its oracle together (the
differential test stays green by construction). Record the audit result in the ledger even if no fix is needed.

### 4.3 S3.3 — Cross-sectional gap-fill ops + datafield family
**Cross-sectional ops** (each `oracle == VM`, valid-set-only): `quantile(x[, n])` (bucket into `n` quantiles, default
5, value = bucket index/`(n−1)` like `cs_rank` but discretized), `reverse(x)` (negate; the rank-reversal idiom),
`vec_sum`/`vec_avg` (reductions over the valid set broadcast back), plus any research-§8-union winsorize/normalize
variant not already present (audit the registry first — `normalize`/`winsorize` exist; add only the genuine gaps).
**Datafields** (§0.5 — panel inputs, not opcodes): `alpha/datafields.hpp` derives `vwap` (if not supplied:
`(high+low+close)/3` typical-price proxy, documented as a proxy when true VWAP is absent), `dollar_volume =
close·volume`, and `adv{d} = ts_mean(dollar_volume, d)` columns on the panel, with the same NaN/PIT/universe
discipline as OHLC (NaN out-of-universe, never impute). A field-name registry recognizes `vwap`/`adv20`/… so
formulas reference them like `close`. These feed S3.6's Alpha101 fixtures.

### 4.4 S3.4 — Fix `op_swap` at the root + re-enable
**(a) `validate_node_contract(op, e)` in `analyze_call`** — reject a Call unless: `e.n_hparams == e.op->n_hparams`;
the materialized non-hparam operand count is in `[min_arity − n_hparams, max_arity − n_hparams]`; required operand
slots (`a`, and `b`/`c` per arity) are present (not `kNoExpr`); group-role / mask-role operands carry the right dtype
(extend `validate_stateful_op_dtypes` coverage to **every** role-bearing op, not just `TradeWhen`/`Hump`). This is
the load-bearing fix: it closes the "analyze-valid but VM-unsafe" gap for **all** mutation/crossover, and is exactly
the assertion the original defect needed.
**(b) `OpCatalog` bucket refinement** — key becomes `(shape_cat, out_dtype, materialized_arity, n_hparams,
operand_dtype_sig)`; `add_op` skips `n_hparams > 0` ops and record ops; `op_swap`'s lookup passes the same
materialized arity + the node's operand-dtype signature so it only ever samples a fully-contract-compatible op.
**Re-enable** `enable_op_swap = true` (`search_driver.hpp`).
**Proof harness (`alpha_op_swap_stress_test.cpp`):** for **every** `(shape, dtype, arity)` bucket, build a seed
genome, op-swap across every candidate in the bucket, and assert (i) the result either analyzes-clean **and** runs
`oracle == VM` with no abort/corruption, or is cleanly rejected by analyze — **never** an abort. Include the exact
original repro (kalman_level→hump materialized-arity-1 swap) as a named regression case. Determinism: seeded, replays
byte-identically.

### 4.5 S3.5 — Grammar-typed (valid-by-construction) generation
`factory/generate.hpp` — a recursive shape/dtype-targeted sampler: given a target `(Shape, DType)` and a remaining
depth budget, sample an `OpCatalog` bucket that *produces* that target, then recurse to generate each operand at the
operand's required `(Shape, DType)` (leaves = panel fields of the right dtype, or literals for windows/scales). The
shape lattice (Scalar/CrossSection/Panel) and dtype (F64/Mask/Group) constrain every node, so the emitted genome is
analyze-valid **by construction**. Windows are sampled as integer literals in `[1, max_lookback]`; group args resolve
to a Group field. Deterministic, seeded (composes with S4's seed axis). **Payoff measured:** the test generates N
genomes and asserts the analyze-rejection rate ≈ 0 (vs a random-AST control that ignores types, whose rejection rate
is reported alongside — the yield delta is the unit's headline number). The generator reuses the §0.3-hardened
contract so generated filter/record ops (if included) are well-formed or excluded.

### 4.6 S3.6 — Conformance suite + Alpha101 reproduction + bench + close
- **Conformance:** parametrized `oracle == VM` over the **entire** widened op set (every S3.1–S3.4 op × representative
  fixtures), as the non-regression gate. Plus the **p1 corpus regression**: a frozen list of pre-existing formulas
  evaluated and asserted byte-identical to a stored baseline (the proven-core guarantee).
- **Alpha101 reproduction:** `tests/fixtures/alpha101_subset.txt` — the canonical alphas whose operators S3 now
  supports (the `yli188` reference semantics, research §1.2), encoded as DSL strings. The test parses, typechecks,
  and evaluates each on a fixture panel (with the S3.3 datafields), asserting finite, in-universe output. The
  **not-yet-expressible** remainder is enumerated in the ledger with the blocking operator (the recorded residual).
- **Bench (`alpha_widened_bench.cpp`):** eval throughput on the widened op set; confirm CSE hit-rate and zero
  hot-path allocation hold (Debug upper-bound numbers only — never cited as release figures).
- **Close:** ledger close ceremony; update `p2/ROADMAP.md` status row for S3.

---

## Exit criteria
Mirror the spec's exit criteria (verbatim authority is the spec). Concretely green when: `CsResidualize` ships and
reduces to demean bit-for-bit; the four BRAIN `ts_*` ops + the datafield family ship with `oracle == VM` + rail +
NaN tests; `enable_op_swap = true` proven by the per-bucket stress harness (no corruption on any bucket);
grammar-typed generation rejection-rate ≈ 0 (reported vs control); the full-operator differential-conformance suite
green; an Alpha101 subset reproduces with the residual recorded; the p1 corpus is byte-identical; `/W4 /permissive-
/WX` + clang-format clean.

## Invariants this sprint must prove
No look-ahead (within-date residualizer; every `ts_*` reads only `[t−d+1 … t]`; truncation-invariance per op).
Determinism (seeded generation; order-fixed reductions; no eval-path RNG). Differential correctness (every kernel:
oracle twin, `oracle == VM`). No survivorship / PIT (new datafields NaN out-of-universe). No hot-path allocation
(slot-pool discipline preserved).

## Dependencies
**Upstream:** p0 Phase-3 (`alpha::{lexer, parser, typecheck, dag, bytecode, vm, oracle, registry, panel}`), p1 S3
(`factory::{OpCatalog, mutation, genome, search_driver}`). **atx-core (Pattern-B, §2.1):** L7 `cs_residualize`,
L6 `rolling_ext` — both shipped engine-local; lifts recorded.

## Explicitly NOT in this sprint
No search changes (S4: multi-objective fitness, behavioral diversity, cost-aware mining, robustness battery). No new
signal families (S5 DL; p1 S5 learned already exist). No alt-data fields (S6; S3 datafields are price-volume only).
No all-101 reproduction (subset gated by supported ops; remainder recorded). No JIT/dispatch rewrite (interpreter
retained, research §5.5).

## Baton → next
S3 hands S4 a richer, correctly-typed, **fully-mutable** substrate: regression-residual neutralization, the BRAIN
`ts_*` superset, the `vwap`/`adv`/volume datafields, a repaired `op_swap` (full mutation set restored, contract-safe
by construction), and grammar-typed generation. S4's multi-objective search and robustness battery operate on exactly
this widened space.

## References
- Spec: [`sprint-3-alpha-dsl-expression-substrate.md`](sprint-3-alpha-dsl-expression-substrate.md).
- Research: [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md)
  §1/§1.4/§2/§3/§6.5/§8; [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
  §3.3.
- As-built: `alpha::{registry,typecheck,cs_ops,ts_ops,oracle,vm,dag,bytecode,panel}`,
  `factory::{op_catalog,mutation,genome,search_driver}` at base SHA.

## §8 — Self-review (pre-implementation)
- [ ] No placeholder/TBD left unresolved (the spec's one TBD — the residualizer op name — is resolved to
  `cs_residualize`/`CsResidualize` here).
- [ ] Every unit maps to the §0.1 nine-site checklist or an explicit additive new file.
- [ ] The op_swap fix is rooted (analyzer hardening), not band-aided (catalog refinement is the complement).
- [ ] The demean boundary pin has a concrete guarantee (dispatch to `cs_group_demean_row` when style block empty).
- [ ] Datafields are inputs, not opcodes (§0.5) — the ISA stays price-volume-clean.
- [ ] No CMakeLists edits anywhere (auto-glob).
