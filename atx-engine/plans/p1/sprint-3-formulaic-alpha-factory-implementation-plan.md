# Sprint S3 — Formulaic Alpha Factory — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan unit-by-unit. Steps use checkbox (`- [ ]`) syntax. This is the FROZEN *how*; the sprint spec [`sprint-3-formulaic-alpha-factory.md`](sprint-3-formulaic-alpha-factory.md) is the *what*. On conflict, **§0 (this plan's as-built amendment) overrides** the spec.

**Goal:** Make the engine **discover** alphas instead of merely evaluating them — a deterministic, seeded **evolutionary search over the Phase-3 DSL** (mutation + crossover + fractional-constant parameter search) whose fitness is a new alpha's **marginal contribution to a diversified pool** (not its standalone Sharpe), with **canonical-hash dedup** so structurally-equivalent expressions are never re-evaluated, and **S1 deflation** so a pure-noise population admits **nothing**.

**Architecture:** A new header-only `factory/` layer (namespace `atx::engine::factory`) on top of the as-built Phase-3 substrate. The genome **is** an `alpha::Ast` (a flat, index-addressed, relocatable arena); mutation/crossover rebuild a fresh `Ast` through the public builder API and re-run `alpha::analyze()` as the validity oracle (a candidate that fails to typecheck/causality-check is rejected, never scored). A generation is scored in one `compile_batch` → `parallel::parallel_evaluate` (S2) → `extract_streams` pass (cross-alpha CSE + canonical dedup are the throughput levers). Fitness is computed **OOS only** on S1's purged+embargoed CPCV folds, discounted by a marginal-correlation-to-pool term, and **deflated** by the running trial count; survivors stream through the P4 `AlphaGate` into the `AlphaStore`. Determinism is by construction: every RNG draw is seeded by `(master_seed, generation, candidate_index)` — never worker/thread/time — and selection/reproduction walk candidates in canonical-id order, so a seeded run replays byte-identically and the parallel digest equals the single-thread digest.

**Tech Stack:** C++20, header-only inline (`#pragma once`), namespace `atx::engine::factory`; reuses `alpha::{Ast,Expr,OpCode,OpSig,Library,analyze,compile,compile_batch,Program,Engine,SignalSet,evaluate_reference,extract_streams,AlphaStreams,Panel,WeightPolicy}`, `combine::{AlphaStore,AlphaId,AlphaMetrics,compute_metrics,AlphaGate,GateConfig,GateVerdict,pairwise_complete_corr}`, `eval::{deflated_sharpe,pbo_cscv,cpcv_folds,compute_return_metrics,skewness,excess_kurtosis}`, `validation::{check_no_lookahead,...}`, `parallel::{Pool,parallel_evaluate,signal_set_digest}` (S2), `atx::core::Xoshiro256pp` (seeded RNG, `jump()`), `atx::core::{Result,Status,hash_combine,hash_bytes}`. GoogleTest (`atx-engine/tests/*_test.cpp`, CONFIGURE_DEPENDS — no per-unit CMake edit). clang-cl `/W4 /permissive- /WX` **+ strict FP** (`/fp:precise`, `-ffp-contract=off`, inherited from S2 — the search RNG and reductions must be bit-stable). Build + ctest are the gates; clang-tidy is disabled (noise).

---

## §0 — As-built reconciliation amendment (the recon fixes)

The spec was drafted from the research north-stars before this sprint's reconnaissance against the merged `feat/atx-core-stdlib` engine. Eight load-bearing corrections; each changes a unit's scope.

### 0.1 The AST has **no in-place mutator** — the genome is a rebuilt arena, not an edited tree
`alpha::Ast` (`alpha/parser.hpp:139–155`) is a flat, **index-addressed** arena: `std::vector<Expr> nodes_` (children referenced by `ExprId = u32` in `Expr::a/b/c`, not pointers), `strings_` (owned field/ident names), `roots_`. `Ast::nodes()` returns `std::span<const Expr>` — **there is no `Expr&` accessor**, so a mutation cannot edit a node through the `Ast` API. What exists is a **public append-only builder**: `ExprId add(const Expr&)`, `u32 intern(std::string_view)`, `void add_root(std::string, ExprId)`. Consequence: **every mutation/crossover constructs a NEW `Ast` by replaying nodes through the builder**, applying the edit during the replay. Because children are indices and the arena is relocatable, a subtree splice is an **offset-remap deep copy**. `Expr` is trivially-copyable POD. **CRITICAL hazard:** `Expr::op` is a non-owning `const OpSig*` borrowed from the `Library` (`parser.hpp:26–28`); the `Library` MUST outlive every genome, and any op swapped into a `Call` node must be a row from **that same `Library`**. The factory therefore owns **one** `Library` for the whole run and hands every genome a borrowed reference. *(Spec S3.1 "type-safe mutations over the Ast" is feasible but is a rebuild, not an in-place edit — S3-1 builds the genome/builder substrate first.)*

### 0.2 `analyze()` is the validity oracle — re-run it on every candidate
`alpha::analyze(const Ast&) -> Result<Analysis>` (`alpha/typecheck.hpp:588`) is a single forward pass that returns shape/dtype/lookback/causality per node (`TypeInfo{shape,dtype,lookback,is_record,pins}`) plus `required_lookback()`. A mutated/crossed Ast is **valid iff `analyze` returns `Ok`**; an `Err(InvalidArgument)` (shape/dtype/causality/record-root violation) rejects the candidate. This is cheaper than compiling and is the search loop's gate. **Caveat:** `analyze` does *not* re-check parse-time invariants (operand arity, hparam peeling) — a hand-built `Ast` must respect them by construction; S3's builders enforce arity from `OpSig::{min,max}_arity` before emitting a `Call`.

### 0.3 Windows/scale constants are **Literal child operands**, not operator metadata
The spec's "fractional-window perturbation" targets the constants the research reveals (`decay_linear(..., 8.22237)`, `delta(..., 2.25164)`). In the as-built tree these are ordinary `Expr{Kind::Literal, value=...}` **children** (e.g. `ts_mean(close, 5)` → child `b` is `Literal(5)`), **not** `OpSig` fields. Filter hyper-parameters (Kalman Q/R, OU θ/μ) *are* peeled into `Expr::hparams[0..1]`+`n_hparams` at parse. So the jitter operator must (a) walk the tree, (b) classify each `Literal` as *window* (the integer operand of a `shape_panel` Ts\* op, floored to `[1, 65535]`) vs *scale/coefficient* (any other numeric literal) vs *hparam*, and (c) jitter accordingly. Temporal ops are identified by `OpSig::shape_of == &shape_panel` plus the typecheck predicates `is_shift_ts`/`is_rolling_ts`. *(S3-3 parameter search reuses this classifier to enumerate a template's free constants.)*

### 0.4 There is **no `Library` iterator** and no commutative flag — S3 precomputes an `OpCatalog`
`alpha::Library` exposes only `find(name) -> const OpSig*` (linear scan, `registry.hpp:496`) and `size()`. There is **no way to enumerate rows**, and `OpSig` has **no commutative flag** and **no per-operand dtype vector** (per-arg constraints are hard-coded in `typecheck.hpp`). The named-function table is `std::array<OpSig, 65>` (`builtin_ops()`); infix ops (`+ - * / ^ < > <= >= == != && ||`, the `OpCode` arithmetic/comparison enumerators) are **not** in the table — the parser maps them directly. Consequence: **op-swap cannot query the Library for compatible replacements.** S3-1 builds an `OpCatalog` once at construction: it walks the known `OpCode` set + the `Library`'s 65 named rows, groups candidates by `(result Shape, out DType, arity-bucket)`, and tags the commutative subset (S3 *declares* `{Add, Mul, MinP, MaxP, And, Or, CmpEq, CmpNe}` commutative — the registry won't tell you). op-swap samples a replacement from the same `(shape,dtype,arity)` bucket.

### 0.5 **No canonical/commutative normalization exists** — S3 builds the dedup hash from scratch
The spec implies "DAG normalization … commutative-operand ordering, constant-fold, strength-reduction already in p0." Reconnaissance: p0 has **only** (a) parse-time constant folding (`parser.hpp`, pure numeric subtrees) and (b) **exactly one** strength-reduction rule `pow(x,2) → mul(x,x)` (`dag.hpp:328`). **Commutative-operand ordering does NOT exist** — `dag.hpp:114` hashes child order sensitively, so `Add(a,b)` and `Add(b,a)` produce **different** `NodeKey`s and **do not dedup**. Moreover the in-process `NodeKeyHash` (wyhash, compile-time seeds, no endian normalization, `hash.hpp:13`) is **not stable across process restarts** — unusable as a persisted/library key. Consequence: S3-2 must **build its own canonicalization pass** (recursive commutative-operand sort by child sub-hash + the existing folds) and a **stable** structural hash (fixed byte layout over `opcode | sorted-child-hashes | param-bits | field-NAME hash`, keyed by field *name* not Ast-local `name_id`). This is the dedup key that lifts to cross-generation and (S4) library-wide dedup. The existing `Program::cache_hits`/`unique_nodes`/`cache_hit_pct()` still measure the *within-batch* CSE win for free.

### 0.6 **No reusable corr-to-pool / marginal-correlation helper exists** — and the gate uses MAX not mean
`combine/correlation.hpp` exposes exactly **one** function: `pairwise_complete_corr(a, b) -> f64` (pairwise-complete Pearson, returns 0 on <2 valid pairs or zero-variance). The corr-to-pool screen `AlphaGate::max_abs_corr_to_pool` (`gate.hpp:130`) is a **private static** computing **MAX |corr|** to any pool member (cutoff `> max_pool_corr = 0.7`), **not** mean-pairwise and **not** a reusable public helper. `AlphaMetrics` caches **no** correlation field. Consequence: S3-4 must **write** the public marginal-corr helper (`corr_to_pool(candidate, pool, Reduce)` over `pairwise_complete_corr`, supporting both `Max` for the gate-consistent screen and `Mean` for the diversification discount). **Dangling-span hazard:** `AlphaStore::pnl()/pnl_matrix()` spans alias the backing vector and **dangle after the next `insert()`** (`store.hpp:223`) — S3 must compute corr-to-pool **before** inserting the candidate.

### 0.7 The S1 eval type is `ReturnMetrics` (not `PerfMetrics`); deflation takes loose moment scalars
The spec/ROADMAP say "reuse S1 `PerfMetrics`." The as-built type is **`eval::ReturnMetrics`** (`eval/perf_metrics.hpp:100`) via `compute_return_metrics(pnl, ReturnMetricsCfg)`. The WQ fitness term is **already implemented** as `combine::compute_metrics(...).fitness` = `sqrt(abs(returns)/max(turnover, 0.125)) * sharpe` (floor `kTurnoverFloor = 0.125` confirmed, `metrics.hpp:83,205`) — S3 reuses it verbatim, no second fitness convention. Deflation: `eval::deflated_sharpe(sr, T, skew, exkurt, N, std::optional<var>) -> DsrResult{psr,sr_star,dsr,haircut_sharpe}` (`deflated_sharpe.hpp:136`) where **`N` is the trial count** — S3 feeds the running search trial count here. Moments come from `eval::skewness`/`eval::excess_kurtosis` (`stats_ext.hpp`). PBO: `eval::pbo_cscv(perf /*candidate-major M[c*T+t]*/, n_candidates, n_splits)`.

### 0.8 Sub-universe robustness has **no scoring-layer support**; S2 parallel is **in progress**
Universe masking exists **only** at Panel ingest (`alpha/panel.hpp` `universe_` bitmap + `in_universe`; `alpha/segment_panel.hpp` `UniversePolicy{PresentBitmap|Field}`). The gate/store/metrics/eval layers take **already-realized** PnL streams with no `universe` parameter. So WQ sub-universe robustness (re-score on a weaker universe, spec S3.4) requires **re-running `extract_streams` against an alternate `Panel`** built with a different `UniversePolicy` — S3-4 builds that re-eval path; nothing downstream supports an in-place masked re-score. Separately, **S2 (parallel batch-eval) is in progress** (`sprint-2-progress.md`: S2-0…S2-4 all ⏳). S3's throughput target consumes `parallel::parallel_evaluate` + `parallel::Pool`; **until S2 lands, S3 runs single-thread `alpha::Engine::evaluate` (correct, slower)**. The factory's eval call site is a one-line switch (`parallel_evaluate(prog, panel, pool)` vs `Engine(panel).evaluate(prog)`); record the S2 dependency as the kickoff risk and gate the parallel path behind S2's digest-equality guarantee.

> **Net scope shift vs spec:** S3-1 grows a *genome/builder/OpCatalog* substrate (0.1, 0.4). S3-2 *builds* canonicalization, not "reuse p0's" (0.5). S3-4 *writes* the marginal-corr helper and the sub-universe re-eval path (0.6, 0.8), and consumes `ReturnMetrics`/`deflated_sharpe` with the as-built signatures (0.7). The validity oracle is `analyze()` (0.2); constants are tree literals (0.3); the parallel path is S2-gated (0.8).

---

## §1 — Research foundation: the factory design rules (with citations)

Derived from the research north-stars (worldquant/renaissance deep-dives, `§7` below) and the carried-forward p1 invariants. **Non-negotiable**; every S3 unit is checked against them.

| # | Rule | Why / source |
|---|------|--------------|
| **F1** | **Seeded-by-id determinism.** Every RNG draw (init, mutation target, crossover cut, param sample) comes from `atx::core::Xoshiro256pp` seeded by `(master_seed, generation, candidate_index)` — **never** worker-id, thread-id, or time. The seed is part of the recorded artifact; a run replays **byte-identically**. | p1 ROADMAP invariant #1; S2 rule R7 (item-keyed streams are worker-count-invariant). Worker-keyed streams change with worker count [R123]. |
| **F2** | **Parallel digest invariance.** A generation is scored via `parallel::parallel_evaluate`; its `signal_set_digest` MUST equal the single-thread `Engine::evaluate` digest and be identical across worker counts {1,2,4,8}. Selection/reproduction iterate candidates in **canonical-id order**, never population/completion order. | S2's load-bearing contract; FP non-associativity is the only thing that breaks bit-reproducibility [G][I1]. |
| **F3** | **OOS-only fitness (fit/apply firewall).** Every candidate's fitness is computed on S1's **purged + embargoed CPCV test folds** (`eval::cpcv_folds`), never in-sample. The search *selects* on OOS fitness; truncation-invariance is asserted (`validation::check_no_lookahead`). | p1 invariant #2; López de Prado AFML Ch. 7 (purged CV) [LdP]; RenTech OOS discipline (§5 below). |
| **F4** | **Deflation kills snooping.** The **running trial count** `N` (every distinct candidate scored) feeds `eval::deflated_sharpe(...,N,...)` and `eval::pbo_cscv`. A **pure-noise population admits NOTHING** after deflation — the non-vacuous anti-snooping proof. | p1 invariant; Deflated-Sharpe / multiple-testing [Bailey-LdP]; the dominant failure mode at 10⁵–10⁹ candidates. |
| **F5** | **In-grammar validity oracle.** Mutation/crossover stay in-grammar by construction (arity/shape/dtype pre-checked from `OpSig` + `TypeInfo`); `analyze()` is the backstop. A candidate that fails `analyze` is **rejected, never scored**. Differential: a mutated AST re-analyzes to a valid causal program or is rejected. | §0.2/§0.4; spec "always produce a valid, causal program"; strongly-typed GP [Montana]. |
| **F6** | **Canonical-dedup soundness.** `canonical_hash` is **sound** (hash-equal ⇒ VM evaluates **bit-identical**) and **discriminating** (commutative reorder ⇒ same hash; genuinely-different expr ⇒ different hash). Dedup never drops a distinct alpha and never re-evaluates an equivalent one. | §0.5; the throughput lever — "evaluate the union DAG of unique sub-expressions," 10–100× [DSL-deep-dive §5.3]. |
| **F7** | **Marginal-contribution fitness.** A candidate's worth = WQ fitness **×** its diversification of the pool (1 − corr-to-pool), **not** standalone Sharpe — optionally × a sub-universe-robustness multiplier. The factory optimizes diversification. | The WQ thesis: 101 alphas, **mean pairwise correlation 15.9%**; "100× models → 10× prediction"; `IR ≈ IC·√breadth` [Kakushadze 1601.00991; Grinold-Kahn]. |
| **F8** | **No hot-path allocation in steady state.** Population buffers, per-worker `Engine`/`Arena`, and scratch are pre-sized; the generation loop allocates only on the cold compile path. | p1 invariant #6; S2 per-worker arena precedent. |

**One-sentence thesis:** *the engine already evaluates thousands of overlapping expressions cheaply (hash-consed DAG + CSE + S2 fan-out); S3 is the seeded, deflated, pool-aware search loop that sits on top — and the only new correctness risks are (a) snooping (answered by F3/F4), (b) determinism under RNG+parallelism (F1/F2), and (c) dedup soundness (F6).*

---

## §2 — File structure

### 2.1 atx-core Pattern-B request (decided at kickoff; engine-local fallback ships S3)

> The engine adds no general-purpose primitive (project rule). Full **CMA-ES** needs an eigendecomposition of the covariance matrix (atx-core **L7** `linalg`/`eigh`). **Decision (recorded):** S3 ships a **separable (diagonal-covariance) CMA-ES** (sep-CMA-ES, Ros & Hansen 2008) engine-local in `factory/param_search.hpp` — it needs **no matrix decomposition** (diagonal `C`, per-coordinate step sizes), is deterministic under a seeded `Xoshiro256pp`, and is well-suited to the low-dimensional fractional-constant search. Full rotation-invariant CMA-ES is recorded as the **Pattern-B atx-core L7 lift** (an `eigh`/`cma` helper), exactly as S1's `stats_ext` and S2's `DetPool` were engine-local-then-lifted.

### 2.2 Engine `factory/` layer (this sprint builds these)

| File | Responsibility | Unit |
|---|---|---|
| `include/atx/engine/factory/fwd.hpp` | forward decls + the doc block (per-unit header map, namespace `atx::engine::factory`, the `using Pool = parallel::Pool` eval switch point) | S3-0 |
| `include/atx/engine/factory/genome.hpp` | `Genome` (owns `alpha::Ast` + cached `alpha::Analysis` + `canon_hash`); `clone()`, `clone_subtree(src, root, dst)` (offset-remap deep copy + field re-intern), `rebuild_with(edit)`, `validate()` (= re-`analyze`). The genome substrate (§0.1). | S3-1 |
| `include/atx/engine/factory/op_catalog.hpp` | `OpCatalog` — precomputed op-candidate lists grouped by `(Shape, DType, arity)` + the declared-commutative set, built once from the `OpCode` set + `Library` (§0.4). | S3-1 |
| `include/atx/engine/factory/mutation.hpp` | `op_swap`, `field_swap`, `jitter_const` (window vs scale vs hparam classifier, §0.3); each `(Genome, …, Xoshiro&) -> Result<Genome>`, validated by `analyze`. | S3-1 |
| `include/atx/engine/factory/crossover.hpp` | `subtree_crossover(parent_a, parent_b, Xoshiro&) -> Result<Genome>` at `TypeInfo`-compatible cut points (§0.1). | S3-2 |
| `include/atx/engine/factory/canonical.hpp` | `canonical_hash(const alpha::Ast&, ExprId) -> u64` (recursive commutative-sort + fold + stable byte layout, field-name keyed, §0.5); `CanonSet` dedup set. | S3-2 |
| `include/atx/engine/factory/param_search.hpp` | `ParamSpace` (free-constant extraction via §0.3 classifier), `optimize_params(template, space, fitness_fn, Xoshiro&, cfg)` — `Grid` / `Random` / `SepCmaEs` (§2.1). | S3-3 |
| `include/atx/engine/factory/fitness.hpp` | `corr_to_pool(candidate_pnl, pool, Reduce)` (the missing helper, §0.6); `pool_aware_fitness(...)` = WQ fitness × diversification × sub-universe robustness, **OOS via CPCV**, **deflated** (§0.6/§0.7/§0.8). | S3-4 |
| `include/atx/engine/factory/search_driver.hpp` | `SearchDriver` — the population loop (init → batch-eval → dedup → select → reproduce → elitism → novelty pressure), seeded/deterministic (§4.7). | S3-5 |
| `include/atx/engine/factory/factory.hpp` | `Factory::mine(...)` — the mine→gate→admit loop over the `AlphaStore` + `AlphaGate`, trial-count accounting, dedup/CSE telemetry (§4.8). | S3-6 |

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`, CONFIGURE_DEPENDS)
`factory_genome_test.cpp` (S3-1), `factory_mutation_test.cpp` (S3-1), `factory_crossover_test.cpp` (S3-2), `factory_canonical_test.cpp` (S3-2), `factory_param_search_test.cpp` (S3-3), `factory_fitness_test.cpp` (S3-4), `factory_search_driver_test.cpp` (S3-5), `factory_integration_test.cpp` (S3-6, the mine→gate→admit + anti-snooping + determinism proof). Bench: `bench/factory_bench.cpp` (S3-6).

### 2.4 Ledger
`sprint-3-progress.md` (S3-0), updated per unit (copy `sprint-2-progress.md` / `sprint-1-progress.md` shape). Likely sub-sprint split **S3-a** (S3-0…S3-3: genome/mutation, crossover/dedup, param search) / **S3-b** (S3-4…S3-6: fitness, driver, integration) per the ROADMAP's ">7 units" rule.

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

- **TDD:** failing GoogleTest first; `Suite_Condition_ExpectedResult`; cover happy path, **boundaries** (single-node genome, leaf-only tree, empty pool, population=1, generations=0, zero free constants, all-mutations-rejected), and the **invariant proofs** (F1 seeded-replay equality, F4 noise→no-admit, F5 invalid→rejected, F6 dedup soundness).
- **Determinism (F1/F2):** all RNG via `Xoshiro256pp` seeded by `(master_seed, gen, cand_idx)`; selection/reproduction in canonical-id order; the parallel eval path's digest equals single-thread. A **two-runs-equal** test (same seed → byte-identical `Factory` output) is mandatory per RNG-bearing unit.
- **No look-ahead (F3):** fitness on CPCV **test** folds only; `validation::check_no_lookahead` truncation-invariance reused; nothing fitted in-sample (the formulaic alpha has no fitted params — the *search* is the only estimation, and F4 deflates it).
- **Validity oracle (F5):** every produced genome passes `analyze()` before it is returned `Ok`; a unit that returns an un-analyzable genome is rejected in review.
- **No hot-path alloc (F8):** the generation loop reuses pre-sized buffers + per-worker `Engine`; cold compile may allocate (documented).
- **Warnings = errors:** `/W4 /permissive- /WX` (clang-cl) **+ strict FP** (`/fp:precise`, `-ffp-contract=off`). clang-tidy disabled — the strict build + ctest are the gate.
- **API discipline:** `const`/`constexpr`/`noexcept`/`[[nodiscard]]`; `Result<T>`/`Status` for expected failures (mutation-can't-find-target, analyze-rejects, empty-space); weakest sufficient types (`std::span`, `const&`); functions ≤ ~60 lines; `// SAFETY:` on every borrow lifetime (the `Library`/`Panel`/`AlphaStore`-span lifetimes — §0.1, §0.6); reuse atx-core/alpha/combine/eval — **no new general-purpose primitive in the engine** (sep-CMA-ES is the one self-contained numeric helper, recorded as a Pattern-B lift).
- **clangd noise:** ignore squiggles; only a real `cmake --build` + ctest are the gates.

### Handoff block (paste into every coding sub-agent brief)
```text
Implementation quality standard (atx): governed by .agents/cpp/agent.md (safety-critical-grade C++20) and
atx-engine/plans/docs/implementation-quality.md. Positive style/API references in-tree: alpha/parser.hpp
(the flat Ast arena + builder API you rebuild genomes through), alpha/typecheck.hpp (analyze = the validity
oracle), alpha/registry.hpp (OpSig/Library — NO iterator, NO commutative flag: build your own OpCatalog),
alpha/dag.hpp (the existing hash-cons + the canonicalization GAP you must fill), combine/gate.hpp +
combine/metrics.hpp + combine/correlation.hpp (the WQ fitness formula + the MAX-corr screen; the marginal
mean-corr-to-pool helper does NOT exist — write it), eval/deflated_sharpe.hpp + eval/cpcv.hpp +
eval/pbo.hpp (deflation + OOS folds), validation/bias_audit.hpp (reuse verbatim), and (S2)
parallel/batch_eval.hpp + parallel/digest.hpp (the parallel eval path + the digest oracle).

THIS SPRINT'S DOMINANT RISK IS DATA SNOOPING that hides until live trading. The factory mass-produces
candidates; without deflation it mass-produces overfit garbage. The gates:
  - OOS-ONLY FITNESS: score every candidate on eval::cpcv_folds TEST folds, never in-sample. (Rule F3.)
  - DEFLATION: feed the running trial count N into eval::deflated_sharpe(...,N,...) and eval::pbo_cscv.
    A PURE-NOISE population must admit NOTHING after deflation. A test that only checks "it admitted
    something" is vacuous; the load-bearing test is noise->zero-admits AND a planted-edge->admitted. (F4.)

THE SECOND RISK IS NON-DETERMINISM under RNG + parallelism:
  - SEED BY ID: every Xoshiro256pp is seeded by (master_seed, generation, candidate_index). NEVER by
    worker-id, thread-id, or time. The seed is in the recorded artifact. (F1.)
  - CANONICAL-ID ORDER: selection/reproduction walk candidates in canonical-id order, never population or
    completion order. The parallel-eval digest must equal the single-thread digest. (F2.)
  - A seeded run replays BYTE-IDENTICALLY. Two-runs-equal is a mandatory test per RNG-bearing unit.

THE THIRD RISK IS AST CORRUPTION: the Ast has no in-place mutator. Rebuild a fresh Ast through add()/intern()/
add_root(); splice subtrees by ExprId offset-remap + field re-intern. Expr::op borrows a const OpSig* from the
Library, which MUST outlive every genome and is shared across the whole run. Every produced genome MUST pass
analyze() before you return Ok — an un-analyzable genome is a bug. (F5.)

No UB, no hidden look-ahead, no second fitness convention (reuse combine::compute_metrics().fitness).
Header-only inline. Functions <= ~60 lines. AlphaStore::pnl() spans DANGLE after the next insert() — compute
corr-to-pool BEFORE inserting. Build gate: cmake --build build --config Debug --target atx-engine-tests
(/W4 /permissive- /WX + /fp:precise) + ctest -R <Suite>.

Shared-branch discipline: stage EXPLICIT pathspecs only (git add -- <paths>; git commit -- <paths>); NEVER
git add -A; after committing run `git show HEAD --stat` (only your files); never touch atx-core/*; do not push.
End commit messages with: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

---

## §4 — Architecture & algorithms (data structures + pseudocode)

### 4.1 The genome and the rebuild substrate (S3-1)

The genome **is** an `alpha::Ast` plus its cached analysis and dedup key:
```
struct Genome {
  alpha::Ast      ast;          // the flat, relocatable arena (value-owned)
  alpha::Analysis analysis;     // cached analyze() result (shape/dtype/lookback per node)
  atx::u64        canon_hash;   // factory/canonical.hpp key (set after canonicalization)
  // INVARIANT: analysis is the result of analyze(ast) and is Ok; canon_hash matches ast.
};
```
The `Library` is **not** in the genome — the `Factory` owns one and lends `const Library&` to every operator (§0.1 lifetime SAFETY).

**`clone_subtree` (the offset-remap deep copy — the core primitive for crossover + rebuild):**
```
clone_subtree(const Ast& src, ExprId root, Ast& dst) -> ExprId:
  // DFS post-order so children are added before parents (dst arena is topologically ordered).
  memo : HashMap<ExprId, ExprId>            // src id -> dst id (handles shared sub-DAG once)
  function visit(s):
    if s in memo: return memo[s]
    e := src.node(s)                         // a value copy of Expr (trivially copyable)
    // remap children that exist
    if e.a != kNoExpr: e.a = visit(src.node(s).a)
    if e.b != kNoExpr: e.b = visit(src.node(s).b)
    if e.c != kNoExpr: e.c = visit(src.node(s).c)
    // re-intern field/member names (string pools differ between src and dst)
    if e.kind in {Field, Member}: e.name_id = dst.intern(src.field_name(s))
    // e.op (const OpSig*) is carried verbatim — valid iff src and dst share the same Library
    d := dst.add(e)
    memo[s] = d ; return d
  return visit(root)
```
**Mutation by rebuild** (op-swap example): clone the whole tree, but when `visit` reaches the chosen target node, emit the edited `Expr` (different `opcode`/`op`, or `value`, or `name_id`) instead of the original. Then `analyze` the new `ast`; on `Ok` return the `Genome`, on `Err` return `Err` (caller resamples or counts a wasted generation).

**Why this is safe:** children are `ExprId` indices (not pointers) and `Expr` is POD, so an offset-remap copy is a valid deep copy; the arena is "relocatable" by construction (`parser.hpp:19–25`). The only non-value member is `Expr::op` (a borrowed `const OpSig*`) — preserved across genomes because the whole run shares one `Library` (§0.1).

### 4.2 Mutation operators (S3-1)

```
op_swap(g, catalog, rng) -> Result<Genome>:
  cands := [id for id in g.ast.nodes() if kind(id) in {Unary,Binary,Call}]   // canonical-id order
  if cands empty: return Err(NotFound)
  target := cands[ rng.uniform_index(cands.size()) ]
  ti := g.analysis.info(target)                       // required (shape,dtype) of the result slot
  arity := materialized_arity(target)
  repl := catalog.sample_compatible(ti.shape, ti.dtype, arity, rng)   // same bucket, != current op
  if none: return Err(NotFound)
  ng := rebuild_with(g, target, set_op(repl))
  return analyze_into(ng)                              // Ok(Genome) | Err(InvalidArgument)

field_swap(g, panel_fields, rng) -> Result<Genome>:
  leaves := [id for id in g.ast.nodes() if kind(id)==Field]
  if leaves empty: return Err(NotFound)
  target := leaves[ rng.uniform_index(leaves.size()) ]
  newf   := panel_fields[ rng.uniform_index(panel_fields.size()) ]   // same DType::F64 price/vol field
  ng := rebuild_with(g, target, set_field_name(newf))
  return analyze_into(ng)

jitter_const(g, rng, cfg) -> Result<Genome>:
  consts := classify_literals(g)        // -> [(id, ConstKind in {Window, Scale, Hparam})], §0.3
  if consts empty: return Err(NotFound)
  (id, kind) := consts[ rng.uniform_index(consts.size()) ]
  old := g.ast.node(id).value
  new := match kind:
    Window: clamp( round_or_step(old * exp(cfg.sigma_w * rng.normal())), 1, cfg.max_lookback )
    Scale : old * exp(cfg.sigma_s * rng.normal())          // the fractional-constant search
    Hparam: project_to_valid_range(old * exp(cfg.sigma_h * rng.normal()))   // e.g. Kalman R>0
  ng := rebuild_with(g, id, set_value(new))
  return analyze_into(ng)
```
`classify_literals` walks the tree: a `Literal` is a **Window** iff it is the trailing integer operand of a `shape_panel` Ts\* op (`is_rolling_ts`/`is_shift_ts`); an **Hparam** iff it landed in a parent's `hparams[]` (`n_hparams>0`); else a **Scale**. Most mutations are in-grammar by construction (arity/shape/dtype pre-checked); `analyze_into` is the F5 backstop for causality (e.g. a jittered window that overflows `required_lookback` past the panel, or a dtype constraint typecheck enforces).

### 4.3 Subtree crossover (S3-2)

```
subtree_crossover(pa, pb, rng) -> Result<Genome>:
  // pick a recipient cut in A (any non-root node) and a donor in B with a compatible result type
  cut_a := pick_non_root(pa, rng)                           // canonical-id order then uniform
  want  := pa.analysis.info(cut_a)                          // (shape, dtype) the slot must yield
  donors := [id in pb.ast.nodes()
             if compatible(pb.analysis.info(id), want)      // shape broadcastable + dtype equal
             and lookback(pb,id) <= cfg.max_lookback]
  if donors empty: return Err(NotFound)
  donor := donors[ rng.uniform_index(donors.size()) ]
  // rebuild A, but when visit() reaches cut_a, splice clone_subtree(pb, donor, dst) instead
  ng := rebuild_splice(pa, cut_a, /*from*/ pb, donor)
  return analyze_into(ng)                                   // typecheck/causality backstop
```
`compatible` uses the shape lattice (`Scalar < CrossSection < Panel`, broadcast on the wider) and exact `DType` equality. Crossover stays in-grammar by the type pre-check; `analyze` catches the residue (per-arg constraints, group-dtype rules typecheck hard-codes).

### 4.4 Canonicalization + dedup (S3-2) — the throughput lever

The recon finding (§0.5): p0 has **no** commutative ordering and an **unstable** in-process hash. S3 builds a sound, discriminating, **stable** structural hash:
```
canonical_hash(const Ast& ast, ExprId root) -> u64:        // memoized over the sub-DAG
  e := ast.node(root)
  switch e.kind:
    Literal: return mix(TAG_LIT, bit_cast<u64>(e.value))
    Field  : return mix(TAG_FLD, e.dollar, fnv_bytes(ast.field_name(root)))  // by NAME, not name_id
    Unary  : return mix(TAG_OP, u8(e.opcode), canonical_hash(ast, e.a))
    Binary : { h0 := canonical_hash(ast,e.a); h1 := canonical_hash(ast,e.b)
               if is_commutative(e.opcode) and h1 < h0: swap(h0,h1)          // <-- the missing pass
               return mix(TAG_OP, u8(e.opcode), h0, h1) }
    Call   : { hs := [canonical_hash(ast,ch) for ch in children(root)]
               if is_commutative_call(e.op): sort(hs)
               return mix(TAG_CALL, fnv_bytes(e.op->name), e.hparams_bits(), hs...) }
    Select/Member: structural mix in fixed slot order (NOT commutative)
  // mix() = a fixed-layout fold (FNV-1a / explicit byte order) — STABLE across runs/platforms,
  // unlike alpha::NodeKeyHash (wyhash, compile-time seeds). This is the persisted dedup key (lifts to S4).
```
Soundness (F6): two genomes with equal `canonical_hash` evaluate **bit-identical** because the only normalization applied (commutative-operand reorder, the parse-time folds) is value-preserving for the declared-commutative ops. Discrimination: a genuinely different sub-expression changes some leaf/opcode/structure → different hash. `CanonSet` is `HashSet<u64>`; before scoring a candidate the driver checks `CanonSet.contains(g.canon_hash)` and **skips re-evaluation** on a hit (dedup), recording the hit-rate. (Within a single `compile_batch`, `Program::cache_hit_pct()` measures the orthogonal *sub-expression* CSE win.)

### 4.5 Parameter optimizer (S3-3)

```
ParamSpace := extract_free_constants(template_genome)      // the Window/Scale literals, §0.3
              -> [(ExprId, ConstKind, lo, hi)]  with per-dim bounds

optimize_params(template, space, fitness_fn, rng, cfg) -> ParamResult:
  switch cfg.method:
    Grid:    enumerate cartesian product of per-dim grids; eval each; argmax
    Random:  draw cfg.budget seeded samples in-bounds; eval each; argmax
    SepCmaEs:  // separable (diagonal-covariance) CMA-ES, Ros & Hansen 2008 — NO eigendecomp
      m := midpoint(space)                  // K-vector mean
      sigma := 0.3 * (hi-lo)                // per-dim step (diagonal C = I scaled by sigma^2)
      paths p_sigma, p_c := 0
      repeat for cfg.generations:
        // sample lambda offspring (deterministic: rng seeded per (gen, k))
        Z := [ rng.normal_vec(K) for i in 1..lambda ]
        X := [ clamp_to_bounds(m + sigma .* z) for z in Z ]
        F := [ fitness_fn( instantiate(template, x) ) for x in X ]   // OOS pool-aware fitness
        sort offspring by F desc; pick top mu
        m_old := m
        m := weighted_mean(top-mu X)        // recombination
        update p_sigma, p_c, sigma (diagonal CSA + rank-mu, per-coordinate) // standard sep-CMA recurrences
      return { best_x, best_fitness, trials = lambda*generations }
  // every fitness call increments the FACTORY trial count (feeds F4 deflation).
```
sep-CMA-ES is chosen because the search is low-dimensional (a handful of constants per template), it needs **no L7 eigendecomposition** (diagonal covariance), and it is deterministic under the seeded `Xoshiro256pp`. Full rotation-invariant CMA-ES is the recorded Pattern-B L7 lift (§2.1).

### 4.6 Pool-aware fitness (S3-4) — the WQ thesis, OOS + deflated

```
corr_to_pool(candidate_pnl, pool, Reduce) -> f64:           // the MISSING helper, §0.6
  acc := Reduce.init()                                       // Max -> 0 ; Mean -> running mean
  for id in 0..pool.n_alphas():
    c := combine::pairwise_complete_corr(candidate_pnl, pool.pnl(AlphaId{id}))  // BEFORE any insert
    acc := Reduce.step(acc, abs(c))
  return Reduce.finish(acc)                                  // empty pool -> 0

pool_aware_fitness(candidate_genome, pool, folds, sim, weight_policy, panel, cfg) -> FitnessReport:
  // 1. Eval ONCE over the full panel (the VM is causal by construction -> no look-ahead),
  //    then PARTITION the realized PnL into CPCV folds. A formulaic alpha has NO fitted
  //    parameters, so the fit/apply firewall reduces to: judge each candidate on its held-out
  //    TEST folds (robustness across periods), never on a single in-sample window. Evaluating
  //    a causal recurrence on NON-CONTIGUOUS test indices would corrupt lookback warm-up, so we
  //    eval contiguously and slice the PnL — equivalent here precisely because nothing is fitted.
  ss   := evaluate_on(candidate_genome, panel)               // S2 parallel path (or Engine::evaluate)
  strm := alpha::extract_streams(ss, weight_policy, panel, sim)   // full causal PnL/position stream
  fold_fitness := []
  for fold in folds:                                         // eval::cpcv_folds(label_spans, cfg)
    test_pnl := slice(strm.pnl(0), fold.test_idx)            // the held-out OOS partition
    test_pos := slice(strm.positions(0,*), fold.test_idx)
    fold_fitness.push( combine::compute_metrics(test_pnl, test_pos, n_inst, book) )  // .fitness = WQ
  agg := mean_over_folds(fold_fitness)                       // OOS WQ fitness + sharpe + turnover
  wq  := agg.fitness                                         // sqrt(abs(ret)/max(turn,0.125))*sharpe
  // (train-fold metrics feed eval::pbo_cscv as the IS-vs-OOS overfit screen, S3-6.)

  // 2. diversification discount (F7): how much it diversifies the live pool
  redundancy   := corr_to_pool(full_oos_pnl(candidate), pool, Reduce::Mean)    // mean |corr|
  diversify    := clamp(1.0 - redundancy, 0.0, 1.0)

  // 3. sub-universe robustness (§0.8): re-score on a weaker universe Panel, ratio to full
  weak_panel   := attach_segment_panel(raw, UniversePolicy{ smaller-universe })
  robust       := clamp( wq_on(weak_panel) / max(wq, eps), 0.0, 1.0 )

  // 4. raw search signal the optimizer/selector maximizes
  raw := wq * diversify * robust

  // 5. DEFLATION for ADMISSION (F4): account for the running trial count N
  N    := factory.trial_count()                              // every distinct candidate scored so far
  skew := eval::skewness(full_oos_pnl); kurt := eval::excess_kurtosis(full_oos_pnl)
  dsr  := eval::deflated_sharpe(agg.sharpe, T, skew, kurt, N, nullopt)
  return { wq, redundancy, diversify, robust, raw, dsr.dsr, dsr.haircut_sharpe }
```
The **search** maximizes `raw` (the diversification-weighted WQ signal); **admission** (S3-6) requires the **deflated** `dsr` to clear S1's bar AND the P4 gates. A pure-noise population drives `dsr → 0` as `N` grows → no admits (the F4 non-vacuous proof). PBO over the generation's candidate×period OOS matrix (`eval::pbo_cscv`) is the second snooping screen.

### 4.7 Evolutionary search driver (S3-5)

```
run_search(seed_exprs, cfg, pool) -> SearchResult:        // cfg: {master_seed, pop, gens, elites, k_tour, novelty_w, budget}
  pop := init_population(seed_exprs, cfg)                  // seeded valid ASTs / random in-grammar trees
  canon := CanonSet{}
  for gen in 0..cfg.gens:
    // --- batch evaluate (the throughput path) ---
    fresh := [g in pop if not canon.contains(g.canon_hash)]      // dedup (F6)
    srcs  := [unparse(g.ast) for g in fresh]                     // expression strings
    prog  := alpha::compile_batch(srcs, library)                 // ONE shared-DAG, cross-alpha CSE
    ss    := parallel::parallel_evaluate(prog, panel, pool)      // S2 (or Engine::evaluate fallback)
    assert signal_set_digest(ss) == single_thread_digest(prog)   // F2 (in tests)
    for g in fresh: g.fitness := pool_aware_fitness(...) ; canon.insert(g.canon_hash)
    // --- selection (canonical-id order, then seeded draws: F1/F2) ---
    sort pop by canonical id                                     // order-fixed BEFORE any RNG
    parents := tournament_select(pop, cfg.k_tour, rng(seed,gen,*))
    // --- reproduction ---
    children := []
    for i in 0..cfg.pop - cfg.elites:
      r := rng(cfg.master_seed, gen, i)                          // seeded by id, NEVER worker/thread
      child := (r.bernoulli(cfg.p_cross) ? subtree_crossover(pick2(parents,r), r)
                                         : mutate_one(pick1(parents,r), r))
      if child.ok(): children.push(child)                        // invalid -> dropped (F5)
    elites := top_k(pop, cfg.elites)                             // elitism
    // --- novelty / diversity pressure (anti-collapse) ---
    pop := elites + novelty_penalize(children, pop, pool, cfg.novelty_w)
  return { admitted_candidates, trial_count = canon.size(), digest, seed = cfg.master_seed }
```
`novelty_penalize` subtracts a behavioral-distance term (distance of a child's OOS PnL / canonical structure from the population **and** the pool) from its selection fitness, so the population does not collapse onto one motif (the QD/novelty-search idea, [Lehman-Stanley]). Determinism: the only entropy is `Xoshiro256pp` seeded by `(master_seed, gen, idx)`; `init`, `tournament`, `pick`, `mutate`, `crossover` all draw from id-seeded streams; selection sorts by canonical id first. **Same seed → byte-identical `SearchResult`.**

### 4.8 Factory integration (S3-6) — mine → gate → admit

```
Factory::mine(cfg, pool, gate) -> FactoryReport:
  res := run_search(cfg.seed_exprs, cfg.search, pool)
  admitted := 0
  for cand in res.ranked_by_deflated_fitness():           // best deflated first
    cand_pnl := full_oos_pnl(cand)                         // computed BEFORE insert (dangling-span, §0.6)
    metrics  := combine::compute_metrics(cand_pnl, cand_pos, n_inst, book)
    verdict  := gate.admit(metrics, cand_pnl, pool)        // P4 fitness/turnover/MAX-corr gate
    if verdict == Accept and cand.dsr >= cfg.min_dsr:      // AND the S1 deflation bar
      pool.insert(cand.source, cand_pnl, cand_pos, metrics)
      admitted += 1
  return { admitted, evaluated = res.trial_count,
           dedup_pct = 1 - res.trial_count/res.candidates_generated,
           cse_pct   = mean Program.cache_hit_pct() over generations,
           trials    = res.trial_count, seed = res.seed }
```
The report's `evaluated/sec`, `admitted/hour`, `dedup_pct`, `cse_pct` are the throughput story (bench §5 S3-6). The trial count is reported to S1 for DSR/PBO. Until S2 lands, the eval path is `Engine(panel).evaluate(prog)` (correct, single-thread); the `parallel_evaluate` swap is one line behind S2's digest-equality contract (§0.8).

---

## §5 — Per-unit plan

> Sequential dispatch (each unit consumes the prior). Fresh implementer → spec-compliance review → code-quality review → fix loop → ledger SHA, per `superpowers:subagent-driven-development`. **Shared branch `feat/atx-core-stdlib` → explicit-pathspec commits** (handoff block). Suggested split: **S3-a** = S3-0…S3-3, **S3-b** = S3-4…S3-6.

### Task S3-0: Marker + ledger + factory scaffold
**Files:** Create `atx-engine/plans/p1/sprint-3-progress.md`, `atx-engine/include/atx/engine/factory/fwd.hpp`.
- [ ] **Step 1:** Write the ledger from the `sprint-2-progress.md` shape: header (`Base: feat/atx-core-stdlib @ <HEAD>`, in-place, shared-branch note), a "Plan adjustments vs source" paragraph quoting §0 (genome-is-rebuilt-Ast, analyze-is-oracle, no-commutative-canon, no-corr-to-pool-helper, ReturnMetrics-not-PerfMetrics, sub-universe-needs-reeval, **S2-in-progress dependency**), empty per-unit table S3-0…S3-6, empty commits table. Record the **atx-core sep-CMA-ES→full-CMA-ES Pattern-B lift** and the **S2 parallel dependency + single-thread fallback** as the kickoff risks.
- [ ] **Step 2:** `factory/fwd.hpp` — forward decls (the `parallel/fwd.hpp` pattern): namespace `atx::engine::factory`; `struct Genome; class OpCatalog; struct ParamSpace; struct FitnessReport; class SearchDriver; class Factory;` + a doc block listing the per-unit headers and the `using Pool = parallel::Pool` eval switch point (and the `Engine::evaluate` fallback note).
- [ ] **Step 3:** Commit (marker): `git add -- <the two files>; git commit -- <them> -m "docs(s3-0): open sprint-3 formulaic-alpha-factory ledger + scaffold" -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"; git show HEAD --stat`.

### Task S3-1: Genome substrate + OpCatalog + mutation operators
**Files:** Create `factory/genome.hpp`, `factory/op_catalog.hpp`, `factory/mutation.hpp`; Test `tests/factory_genome_test.cpp`, `tests/factory_mutation_test.cpp`.
**Scope:** §4.1/§4.2 + §0.1/§0.2/§0.3/§0.4. `Genome` + `clone`/`clone_subtree`/`rebuild_with`/`validate`; `OpCatalog` grouped by `(Shape,DType,arity)` + declared-commutative set; `op_swap`/`field_swap`/`jitter_const`, each validated by `analyze`. **First verify** the exact `Ast` builder API (`add`/`intern`/`add_root`), `Expr` layout, and `analyze`/`Analysis` accessors against `alpha/parser.hpp` + `alpha/typecheck.hpp`.
- [ ] **Step 1 (genome tests):** suite `FactoryGenome` —
```cpp
TEST(FactoryGenome, CloneRoundTripsAst) {
  Library lib; auto g = make_genome("ts_mean(close, 5)", lib);
  Genome c = g.clone();
  EXPECT_EQ(unparse(c.ast), unparse(g.ast));          // structurally identical
  EXPECT_TRUE(analyze(c.ast).has_value());            // still a valid causal program
}
TEST(FactoryGenome, CloneSubtreeRemapsAndReinterns) {
  Library lib; auto src = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  Ast dst; ExprId r = clone_subtree(src.ast, /*the ts_mean subtree*/ find_call(src,"ts_mean"), dst);
  dst.add_root("a", r);
  EXPECT_TRUE(analyze(dst).has_value());
  EXPECT_EQ(field_names_of(dst), std::vector<std::string>{"volume"});  // re-interned, no stale ids
}
TEST(FactoryGenome, ValidateRejectsRecordRoot) {
  Library lib; auto g = parse_program("a = kalman(ret, hedge, 1e-4, 1e-3)\n", lib);  // record root
  EXPECT_FALSE(analyze(g.value()).has_value());       // §0.2: record root -> InvalidArgument
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `genome.hpp` (§4.1): value-owned `Ast`+`Analysis`, `clone_subtree` (post-order DFS, `memo`, field re-intern, `Expr::op` carried verbatim with a `// SAFETY:` note on the shared-`Library` lifetime), `rebuild_with(target, edit)`, `validate()` = `analyze`. **Step 4:** `ctest -R FactoryGenome` → pass.
- [ ] **Step 5 (op_catalog + mutation tests):** suite `FactoryMutation` —
```cpp
TEST(FactoryMutation, OpSwapStaysInGrammar) {
  Library lib; OpCatalog cat(lib); Xoshiro256pp rng(1234);
  auto g = make_genome("ts_mean(close, 10)", lib);
  for (int i=0;i<200;++i){ auto m = op_swap(g, cat, rng);
    if (m) EXPECT_TRUE(analyze(m->ast).has_value()); }   // every accepted mutant is valid (F5)
}
TEST(FactoryMutation, JitterConstClassifiesWindowVsScale) {
  Library lib; Xoshiro256pp rng(7);
  auto g = make_genome("scale(ts_mean(close, 10), 2.0)", lib);
  auto consts = classify_literals(g);
  EXPECT_TRUE(has_kind(consts, ConstKind::Window));     // the 10
  EXPECT_TRUE(has_kind(consts, ConstKind::Scale));      // the 2.0
}
TEST(FactoryMutation, JitterWindowStaysInBounds) {
  Library lib; Xoshiro256pp rng(9);
  auto g = make_genome("ts_mean(close, 10)", lib);
  for (int i=0;i<500;++i){ auto m = jitter_const(g, rng, {/*sigma*/0.5,/*max_lb*/250});
    if (m){ auto w = window_of(*m); EXPECT_GE(w,1); EXPECT_LE(w,250); EXPECT_TRUE(analyze(m->ast).has_value()); } }
}
TEST(FactoryMutation, SameSeedSameMutation) {              // F1
  Library lib; OpCatalog cat(lib);
  auto g = make_genome("add(rank(close), ts_mean(volume,10))", lib);
  Xoshiro256pp r1(42), r2(42);
  auto a = op_swap(g,cat,r1); auto b = op_swap(g,cat,r2);
  ASSERT_EQ(a.has_value(), b.has_value());
  if (a) EXPECT_EQ(unparse(a->ast), unparse(b->ast));     // byte-identical under same seed
}
```
- [ ] **Step 6:** Build → FAIL. **Step 7:** Implement `op_catalog.hpp` (§0.4: enumerate `OpCode` set + 65 `Library` rows, bucket by `(shape,dtype,arity)`, tag commutative `{Add,Mul,MinP,MaxP,And,Or,CmpEq,CmpNe}`) + `mutation.hpp` (§4.2; `classify_literals` per §0.3; each op `(Genome,…,Xoshiro&) -> Result<Genome>`, `analyze`-validated). **Step 8:** `ctest -R "FactoryGenome|FactoryMutation"` → pass; full suite green. **Step 9:** Commit + ledger row (`feat(s3-1): genome rebuild substrate + OpCatalog + mutation operators`).

### Task S3-2: Subtree crossover + canonical-hash dedup
**Files:** Create `factory/crossover.hpp`, `factory/canonical.hpp`; Test `tests/factory_crossover_test.cpp`, `tests/factory_canonical_test.cpp`.
**Scope:** §4.3/§4.4 + §0.5. `subtree_crossover` at `TypeInfo`-compatible cut points; `canonical_hash` (commutative-sort + fold + stable byte layout, field-name keyed) + `CanonSet`.
- [ ] **Step 1 (crossover tests):** suite `FactoryCrossover` —
```cpp
TEST(FactoryCrossover, ProducesValidCausalProgram) {
  Library lib; Xoshiro256pp rng(3);
  auto a = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  auto b = make_genome("sub(ts_std(close, 20), rank(high))", lib);
  for (int i=0;i<200;++i){ auto c = subtree_crossover(a,b,rng);
    if (c) EXPECT_TRUE(analyze(c->ast).has_value()); }       // F5
}
TEST(FactoryCrossover, TypeIncompatibleCutRejected) {
  // a Mask-typed donor cannot splice into an F64 slot -> Err or analyze() rejects
  Library lib; Xoshiro256pp rng(5);
  auto a = make_genome("ts_mean(close, 5)", lib);
  auto b = make_genome("greater(close, open)", lib);          // Mask result
  bool any_invalid_accepted = false;
  for (int i=0;i<200;++i){ auto c = subtree_crossover(a,b,rng);
    if (c && !analyze(c->ast).has_value()) any_invalid_accepted = true; }
  EXPECT_FALSE(any_invalid_accepted);
}
TEST(FactoryCrossover, SameSeedSameChild) {                   // F1
  Library lib; auto a=make_genome("add(rank(close),ts_mean(volume,10))",lib);
  auto b=make_genome("sub(ts_std(close,20),rank(high))",lib);
  Xoshiro256pp r1(11), r2(11);
  auto c1=subtree_crossover(a,b,r1); auto c2=subtree_crossover(a,b,r2);
  ASSERT_EQ(c1.has_value(), c2.has_value());
  if (c1) EXPECT_EQ(unparse(c1->ast), unparse(c2->ast));
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `crossover.hpp` (§4.3; `compatible` over the shape lattice + exact `DType`; `rebuild_splice` reusing `clone_subtree`; `analyze` backstop). **Step 4:** `ctest -R FactoryCrossover` → pass.
- [ ] **Step 5 (canonical tests):** suite `FactoryCanonical` — the **soundness + discrimination** proof (F6):
```cpp
TEST(FactoryCanonical, CommutativeReorderHashesEqual) {
  Library lib;
  auto x = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  auto y = make_genome("add(ts_mean(volume, 10), rank(close))", lib);   // operands swapped
  EXPECT_EQ(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
}
TEST(FactoryCanonical, NonCommutativeOrderHashesDiffer) {
  Library lib;
  auto x = make_genome("sub(rank(close), rank(high))", lib);
  auto y = make_genome("sub(rank(high), rank(close))", lib);            // sub is NOT commutative
  EXPECT_NE(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
}
TEST(FactoryCanonical, DifferentWindowHashesDiffer) {
  Library lib;
  auto x = make_genome("ts_mean(close, 10)", lib);
  auto y = make_genome("ts_mean(close, 11)", lib);
  EXPECT_NE(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
}
TEST(FactoryCanonical, HashEqualImpliesBitIdenticalEval) {             // soundness, the load-bearing one
  Library lib; Panel panel = make_panel(24, 4);
  auto x = make_genome("add(rank(close), ts_mean(volume, 10))", lib);
  auto y = make_genome("add(ts_mean(volume, 10), rank(close))", lib);
  ASSERT_EQ(canonical_hash(x.ast, root(x)), canonical_hash(y.ast, root(y)));
  auto vx = eval_alpha0(x, panel); auto vy = eval_alpha0(y, panel);
  ASSERT_EQ(vx.size(), vy.size());
  for (size_t i=0;i<vx.size();++i)
    EXPECT_TRUE((std::isnan(vx[i])&&std::isnan(vy[i])) || vx[i]==vy[i]);  // bit-for-bit
}
TEST(FactoryCanonical, StableAcrossRebuild) {     // stable key (cross-run intent): clone -> same hash
  Library lib; auto x = make_genome("mul(close, add(open, high))", lib);
  EXPECT_EQ(canonical_hash(x.ast, root(x)), canonical_hash(x.clone().ast, root(x)));
}
```
- [ ] **Step 6:** Build → FAIL. **Step 7:** Implement `canonical.hpp` (§4.4): recursive memoized hash, commutative-child sort (declared set), field hashed **by name** (not `name_id`), fixed-layout `mix` (FNV-1a / explicit bytes — stable, NOT `alpha::NodeKeyHash`), `CanonSet = HashSet<u64>`. **Step 8:** `ctest -R "FactoryCrossover|FactoryCanonical"` → pass; full suite green. **Step 9:** Commit + ledger row (`feat(s3-2): subtree crossover + sound canonical-hash dedup`).

### Task S3-3: Parameter optimizer (grid / random / sep-CMA-ES)
**Files:** Create `factory/param_search.hpp`; Test `tests/factory_param_search_test.cpp`.
**Scope:** §4.5 + §2.1. `ParamSpace` (free-constant extraction via the §0.3 classifier), `optimize_params` with `Grid`/`Random`/`SepCmaEs`. The fitness functor is injected (tested here against a closed-form objective so the optimizer is verified in isolation, before the real pool-aware fitness lands in S3-4).
- [ ] **Step 1:** failing tests, suite `FactoryParamSearch` —
```cpp
TEST(FactoryParamSearch, ExtractsFreeConstants) {
  Library lib; auto g = make_genome("decay_linear(ts_mean(close, 10), 8)", lib);
  ParamSpace sp = extract_free_constants(g);
  EXPECT_EQ(sp.dims(), 2u);                          // the 10 (window) and 8 (window)
}
TEST(FactoryParamSearch, SepCmaEsFindsKnownOptimum) {
  // inject a quadratic objective with a known interior maximum; sep-CMA-ES must converge
  Xoshiro256pp rng(2024);
  ParamSpace sp = box({{0.0,10.0},{0.0,10.0}});
  auto f = [](std::span<const double> x){ return -((x[0]-3.0)*(x[0]-3.0)+(x[1]-7.0)*(x[1]-7.0)); };
  auto r = optimize_params_raw(sp, f, rng, {Method::SepCmaEs, /*lambda*/12, /*gens*/40});
  EXPECT_NEAR(r.best_x[0], 3.0, 0.05);
  EXPECT_NEAR(r.best_x[1], 7.0, 0.05);
}
TEST(FactoryParamSearch, SameSeedSameTrajectory) {  // F1: deterministic search
  Xoshiro256pp r1(99), r2(99); ParamSpace sp = box({{0,10},{0,10}});
  auto f = [](std::span<const double> x){ return -(x[0]*x[0]+x[1]*x[1]); };
  auto a = optimize_params_raw(sp, f, r1, {Method::SepCmaEs, 8, 20});
  auto b = optimize_params_raw(sp, f, r2, {Method::SepCmaEs, 8, 20});
  EXPECT_EQ(a.best_x, b.best_x);                     // byte-identical
  EXPECT_EQ(a.trials, b.trials);
}
TEST(FactoryParamSearch, GridIsExhaustiveAndBounded) {
  ParamSpace sp = box({{1,3}}); int evals=0;
  auto f=[&](std::span<const double>){ ++evals; return 0.0; };
  optimize_params_raw(sp, f, *(Xoshiro256pp*)nullptr, {Method::Grid, /*per-dim*/3, 1});
  EXPECT_EQ(evals, 3);                               // 3 grid points, no RNG used
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `param_search.hpp` (§4.5): `extract_free_constants` (reuse `classify_literals`), `Grid`/`Random`, and **sep-CMA-ES** (diagonal `C`, per-coordinate CSA step-size control, rank-μ recombination; all draws from the seeded `Xoshiro256pp::normal()`; bounds clamping; `// SAFETY:` note that no eigendecomp is performed — the Pattern-B L7 lift covers full CMA-ES). `instantiate(template, x)` rebuilds the genome with the constants set (reuse `rebuild_with`). Trial count returned. **Step 4:** `ctest -R FactoryParamSearch` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s3-3): parameter optimizer (grid/random/sep-CMA-ES)`). Note the full-CMA-ES Pattern-B L7 residual.

### Task S3-4: Pool-aware fitness (WQ × marginal-corr × robustness, OOS + deflated)
**Files:** Create `factory/fitness.hpp`; Test `tests/factory_fitness_test.cpp`.
**Scope:** §4.6 + §0.6/§0.7/§0.8. `corr_to_pool(candidate, pool, Reduce)` (the missing helper over `pairwise_complete_corr`); `pool_aware_fitness` = OOS WQ fitness (`combine::compute_metrics().fitness` on `eval::cpcv_folds` test windows) × diversification × sub-universe robustness, **deflated** by `eval::deflated_sharpe(...,N,...)`. **First verify** the exact `compute_metrics`/`AlphaMetrics`/`deflated_sharpe`/`cpcv_folds`/`extract_streams` signatures.
- [ ] **Step 1:** failing tests, suite `FactoryFitness` — the **WQ thesis** proof + the **deflation** proof:
```cpp
TEST(FactoryFitness, CorrToPoolMeanAndMax) {
  AlphaStore pool = make_pool_from({pnl_a, pnl_b});   // two known streams
  EXPECT_NEAR(corr_to_pool(pnl_a, pool, Reduce::Max),  1.0, 1e-9);   // identical to member a
  EXPECT_GE  (corr_to_pool(pnl_a, pool, Reduce::Max),
              corr_to_pool(pnl_a, pool, Reduce::Mean));               // max >= mean
}
TEST(FactoryFitness, PrefersDiversifyingWeakAlphaOverRedundantStrong) {   // THE WQ thesis (spec exit)
  AlphaStore pool = make_pool_with_strong_member(strong_pnl);
  // candidate A: strong standalone Sharpe but ~identical to the pool member
  // candidate B: weaker standalone Sharpe but uncorrelated with the pool
  auto fa = pool_aware_fitness(redundant_strong, pool, folds, ...);
  auto fb = pool_aware_fitness(diversifying_weak, pool, folds, ...);
  EXPECT_GT(fb.raw, fa.raw);                          // the factory prefers the diversifier
}
TEST(FactoryFitness, FitnessIsOosOnly) {              // F3: no look-ahead
  // perturbing only the LAST date must not change an earlier OOS fold's fitness
  EXPECT_TRUE(validation::check_no_lookahead(full_n, cut,
              [&](size_t n){ return oos_fold_fitness_series(candidate, n); }));
}
TEST(FactoryFitness, DeflationShrinksWithTrialCount) {     // F4
  auto d1  = pool_aware_fitness(cand, pool, folds, cfg_with_trials(1));
  auto d1k = pool_aware_fitness(cand, pool, folds, cfg_with_trials(1000));
  EXPECT_GT(d1.dsr, d1k.dsr);                          // same alpha, more trials -> lower deflated fitness
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `fitness.hpp` (§4.6): `corr_to_pool` (compute **before** any pool insert — `// SAFETY:` dangling-span note, §0.6); OOS fold evaluation via `eval::cpcv_folds` test windows; `combine::compute_metrics(...).fitness` reused verbatim (no second convention); sub-universe robustness via an alternate `attach_segment_panel(UniversePolicy{...})` re-eval (§0.8 — document that this is the only new re-eval path); deflation via `eval::deflated_sharpe(sharpe, T, eval::skewness(pnl), eval::excess_kurtosis(pnl), N, nullopt)`. **Step 4:** `ctest -R FactoryFitness` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s3-4): pool-aware fitness (WQ × marginal-corr × robustness, OOS + deflated)`).

### Task S3-5: Evolutionary search driver
**Files:** Create `factory/search_driver.hpp`; Test `tests/factory_search_driver_test.cpp`.
**Scope:** §4.7. `SearchDriver::run` — population init → batch-eval (compile_batch → eval path → dedup) → tournament select (canonical-id order) → reproduce (mutation+crossover, seeded by id) → elitism → novelty pressure. Deterministic, seeded; budget = generations × population.
- [ ] **Step 1:** failing tests, suite `FactorySearchDriver` —
```cpp
TEST(FactorySearchDriver, SameSeedReplaysByteIdentical) {   // F1 — the load-bearing determinism test
  auto cfg = small_search_cfg(/*seed*/777, /*pop*/16, /*gens*/5);
  auto r1 = SearchDriver(lib, panel).run(cfg, empty_pool());
  auto r2 = SearchDriver(lib, panel).run(cfg, empty_pool());
  EXPECT_EQ(r1.digest, r2.digest);                          // byte-identical search
  EXPECT_EQ(r1.trial_count, r2.trial_count);
}
TEST(FactorySearchDriver, DedupSkipsEquivalentExpressions) {  // F6 throughput
  auto cfg = cfg_seeded_to_produce_duplicates(/*seed*/8);
  auto r = SearchDriver(lib, panel).run(cfg, empty_pool());
  EXPECT_LT(r.trial_count, r.candidates_generated);          // structural duplicates skipped
  EXPECT_GT(r.dedup_pct, 0.0);
}
TEST(FactorySearchDriver, ElitismKeepsBest) {
  auto r = SearchDriver(lib, panel).run(elitism_cfg(/*elites*/2), empty_pool());
  EXPECT_GE(r.best_fitness_per_gen.back(), r.best_fitness_per_gen.front());  // monotone non-decreasing
}
TEST(FactorySearchDriver, AllCandidatesAreValidCausalPrograms) {  // F5
  auto r = SearchDriver(lib, panel).run(small_search_cfg(1,16,5), empty_pool());
  for (auto& g : r.all_scored) EXPECT_TRUE(analyze(g.ast).has_value());
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `search_driver.hpp` (§4.7): id-seeded `Xoshiro256pp` per `(master_seed,gen,idx)`; `compile_batch` per generation; eval via `parallel::parallel_evaluate` **if S2 is available else `Engine::evaluate`** (the §0.8 one-line switch, behind a `using Pool`/`#if`-free runtime flag); `CanonSet` dedup; tournament/truncation selection **after sorting by canonical id**; elitism; `novelty_penalize` (behavioral distance to population + pool). Digest via `parallel::signal_set_digest` folded over generations + selection decisions. **Step 4:** `ctest -R FactorySearchDriver` → pass; full suite green. **Step 5:** Commit + ledger row (`feat(s3-5): deterministic evolutionary search driver`).

### Task S3-6: Factory integration + anti-snooping proof + bench + close
**Files:** Create `factory/factory.hpp`, `tests/factory_integration_test.cpp`, `bench/factory_bench.cpp`; Modify ledger + `ROADMAP.md` (S3 row) + spec (closed) + create `sprint-3.md` user ref.
**Scope:** §4.8 — the mine→gate→admit loop, the **non-vacuous anti-snooping** proof, the throughput bench, the close.
- [ ] **Step 1:** failing tests, suite `FactoryIntegration` — the spec's exit criteria:
```cpp
TEST(FactoryIntegration, MinesAndAdmitsThroughP4Gates) {
  AlphaStore pool; AlphaGate gate{default_gate_cfg()};
  Factory f(lib, panel, sim, weight_policy);
  auto rep = f.mine(real_signal_cfg(/*seed*/1), pool, gate);
  EXPECT_GT(rep.admitted, 0u);                         // admits survivors on a real-signal panel
  EXPECT_EQ(pool.n_alphas(), rep.admitted);            // admitted == inserted
}
TEST(FactoryIntegration, NoiseAdmitsNothingAfterDeflation) {   // F4 — THE anti-snooping proof (non-vacuous)
  AlphaStore pool; AlphaGate gate{default_gate_cfg()};
  Factory f(lib, pure_noise_panel(), sim, weight_policy);      // i.i.d. noise -> no real edge
  auto rep = f.mine(large_budget_cfg(/*seed*/2), pool, gate);
  EXPECT_EQ(rep.admitted, 0u);                         // deflation + gates kill the entire noise population
}
TEST(FactoryIntegration, SeededRunReplaysByteIdentical) {      // F1/F2
  Factory f1(lib, panel, sim, wp), f2(lib, panel, sim, wp);
  auto a = f1.mine(cfg(/*seed*/5), fresh_pool(), gate);
  auto b = f2.mine(cfg(/*seed*/5), fresh_pool(), gate);
  EXPECT_EQ(a.digest, b.digest);
}
TEST(FactoryIntegration, ReuseS1BiasAuditVerbatim) {
  EXPECT_TRUE(validation::catches_overfit_synthetic());        // the shared validity spine still fires
}
```
- [ ] **Step 2:** Build → FAIL. **Step 3:** Implement `factory.hpp` (§4.8): `mine` runs the driver, ranks by deflated fitness, computes `cand_pnl` **before** `pool.insert` (§0.6 dangling-span), screens through `AlphaGate::admit` **AND** the `dsr` bar, accounts the trial count, emits `FactoryReport{admitted, evaluated, dedup_pct, cse_pct, trials, seed}`. **Step 4:** `ctest -R FactoryIntegration` → pass; full suite green.
- [ ] **Step 5:** `bench/factory_bench.cpp` — **alphas evaluated/sec** and **admitted/hour** at a given population size, with **dedup %** (`CanonSet` skips) and **CSE hit %** (`Program::cache_hit_pct()`), vs S2 worker count {1,2,4,8} (or single-thread if S2 not yet merged — record which). Wire into `atx-engine-bench` (CONFIGURE_DEPENDS). Capture the curve into the ledger; do **not** claim ideal speedup (Amdahl + per-worker Engine warm + the compile-batch serial section).
- [ ] **Step 6: Sprint close** (per `../docs/sprint.md`): fill the ledger (per-unit rows, commits table, the throughput numbers, "What S3 proves / baton"); lift residuals to the ROADMAP backlog — **(a) full rotation-invariant CMA-ES → atx-core L7** (`eigh`/`cma`), **(b) sub-universe robustness via a first-class scoring-layer universe mask** (vs the S3-4 alternate-Panel re-eval), **(c) the S2 `parallel_evaluate` swap once S2 closes** (if S3 shipped on the single-thread fallback), **(d) persisted canonical-hash dedup → S4 library-wide index**; flip `p1/ROADMAP.md` S3 row `⏳ → ✅ <sha>` + bump `Last reviewed`; mark `sprint-3-formulaic-alpha-factory.md` `Status: ✅ closed`; create `sprint-3.md` user reference (the `factory::` public API + the determinism/deflation guarantees + the marginal-contribution thesis). **Step 7:** Commit close (explicit pathspecs; `git show HEAD --stat`).

---

## §6 — Exit criteria · invariants · dependencies · NOT-in-scope · baton

**Exit criteria (from the spec, made concrete):**
- The factory mines a population over ≥ N generations and **admits** alphas through the P4 gates, fully deterministic — a seeded run replays **byte-identically** (`FactoryReport.digest` equal across two runs; `FactoryIntegration.SeededRunReplaysByteIdentical`).
- Canonical-hash dedup demonstrably skips re-evaluating structurally-equivalent expressions, with a measured dedup rate in the ledger (`FactorySearchDriver.DedupSkipsEquivalentExpressions`; the bench's `dedup_pct`).
- Mined fitness is **deflated** by S1 — a **pure-noise population admits NOTHING** (`FactoryIntegration.NoiseAdmitsNothingAfterDeflation`, the non-vacuous anti-snooping proof) and a real-signal panel admits survivors.
- Marginal-correlation fitness prefers a **diversifying weak alpha over a strong-but-redundant one** (`FactoryFitness.PrefersDiversifyingWeakAlphaOverRedundantStrong` — the WQ thesis on a fixture).
- Throughput bench recorded (alphas/sec, admitted/hour, dedup %, CSE hit %); scales with S2 worker count (or single-thread baseline noted if S2 not yet merged).
- `/W4 /permissive- /WX` **+ /fp:precise** clean; a `*_test.cpp` per unit; full suite stays green.

**Invariants proven (F1–F8):** seeded-by-id determinism + parallel digest invariance (F1/F2); OOS-only fitness via CPCV + the fit/apply firewall (F3); deflation kills snooping (F4); the `analyze()` validity oracle — invalid mutants rejected, never scored (F5); canonical-dedup soundness, hash-equal ⇒ bit-identical eval (F6); marginal-contribution fitness (F7); no hot-path alloc in steady state (F8). Differential correctness: every search op produces an `analyze`-valid causal program or is rejected; the dedup is proven sound against the VM.

**Dependencies:** Upstream p0 Phase-3 — `alpha::{Ast,Expr,OpCode,OpSig,Library,analyze,compile,compile_batch,Program,Engine,SignalSet,evaluate_reference,extract_streams,AlphaStreams,Panel,WeightPolicy}`; **P4** (closed `f2d22f4`) — `combine::{AlphaStore,AlphaId,AlphaMetrics,compute_metrics,AlphaGate,GateConfig,GateVerdict,pairwise_complete_corr}`; **S1** (closed `2158a17`) — `eval::{deflated_sharpe,pbo_cscv,cpcv_folds,compute_return_metrics,skewness,excess_kurtosis}`, `validation::bias_audit`; **S2** (IN PROGRESS) — `parallel::{Pool,parallel_evaluate,signal_set_digest}` (the throughput path; **single-thread `Engine::evaluate` fallback ships S3 if S2 slips**, §0.8). atx-core — `Xoshiro256pp` (seeded RNG + `jump()`), `hash_combine`/`hash_bytes`, `Result`/`Status`. **Pattern-B request:** full rotation-invariant CMA-ES → atx-core L7 (`eigh`/`cma`); S3 ships sep-CMA-ES engine-local (§2.1).

**Explicitly NOT in this sprint** (spec + ROADMAP anti-roadmap): no **learned** signals (S5 — S3 is purely formulaic search over the DSL); no **persistent library store** (S4 — S3 admits into the in-memory P4 `AlphaStore`; the canonical hash is built persistence-ready but the on-disk index is S4); no **cost calibration** (S6 — S3 prices turnover with the existing `compute_metrics`/`kTurnoverFloor=0.125` model; a cost-aware fitness hook is left for S6); no **new DSL operators** (the vocabulary is Phase-3b's 65-named/81-opcode set; adding an op is a one-row registry task, not a factory concern); no neural/DL search; no distributed/multi-machine search (single-box multicore via S2 is the ceiling).

**Baton → next:** S3 produces a **flood** of admitted alphas — motivating **S4** (a persistent, deduplicated, lifecycle-managed home for 10⁵–10⁹ of them; the S3 canonical hash is the dedup-index seed) and **S5/S7** (combine and operate them). The `factory::` search ops + the canonical hash + the pool-aware-fitness/deflation pattern are reused by S5 (learned-model search admits through the same deflation gate). The seeded-determinism + dedup-soundness tests are the template every later search/discovery feature re-proves against.

---

## §7 — References (open-source web research)

**WorldQuant / formulaic alphas (the factory thesis)**
- Kakushadze, Z. *101 Formulaic Alphas.* Wilmott 84:72–80 (2016); arXiv:1601.00991 — the canon: fractional window constants (`decay_linear(...,8.22237)`, `delta(...,2.25164)`) as the search fingerprint; **mean pairwise correlation 15.9%** across the 101. https://arxiv.org/abs/1601.00991 · https://arxiv.org/pdf/1601.00991
- Kakushadze, Z. & Tulchinsky, I. *Performance v. Turnover: A Story by 4,000 Alphas.* arXiv:1509.08110 — cost ~ 1/turnover; the turnover/return relationship behind the `max(turnover, 0.125)` fitness floor. https://arxiv.org/abs/1509.08110
- Kakushadze, Z. *Combining Alphas via Bounded Regression.* Risks 3(4):474–490 (2015); arXiv:1501.05381. https://arxiv.org/abs/1501.05381
- Kakushadze, Z. & Yu, W. *How to Combine a Billion Alphas.* arXiv:1603.05937 — O(N) mega-alpha combination at scale. https://arxiv.org/abs/1603.05937
- Kakushadze, Z. & Yu, W. *Dead Alphas as Risk Factors.* arXiv:1709.06641 — flatlined alphas → risk factors (the S7 baton). https://arxiv.org/abs/1709.06641
- Tulchinsky, I. (ed.) *Finding Alphas: A Quantitative Approach to Building Trading Strategies.* Wiley (2nd ed. 2019) — the WQ fitness/turnover/correlation gate discipline. (See also Glazar's writeup of the exact fitness formula: https://jglazar.github.io/projects/wq_project/)
- Open-source Alpha101 reference ports (operator semantics): https://github.com/yli188/WorldQuant_alpha101_code · https://github.com/STHSF/alpha101 · DolphinDB: https://docs.dolphindb.com/en/Tutorials/wq101alpha.html

**Genetic programming / symbolic regression over expression trees**
- Koza, J. *Genetic Programming: On the Programming of Computers by Means of Natural Selection.* MIT Press (1992) — tree mutation/crossover canon.
- Montana, D. *Strongly Typed Genetic Programming.* Evolutionary Computation 3(2) (1995) — type-constrained GP (the F5 in-grammar discipline; our shape/dtype lattice is the type system). https://doi.org/10.1162/evco.1995.3.2.199
- gplearn — symbolic regression via GP (arity-constrained function sets, S-expression trees). https://gplearn.readthedocs.io/en/stable/
- DEAP — distributed evolutionary algorithms in Python (GP/ES reference toolkit). https://github.com/DEAP/deap
- Yu, S. et al. *AlphaGen: Generating Synergistic Formulaic Alpha Sets via Reinforcement Learning.* KDD (2023); code: https://github.com/RL-MLDM/alphagen — RL/search emitting alpha expression trees (the learned-search cousin, S5 territory).

**Parameter search (the fractional-constant optimizer)**
- Hansen, N. *The CMA Evolution Strategy: A Tutorial.* arXiv:1604.00772. https://arxiv.org/abs/1604.00772
- Ros, R. & Hansen, N. *A Simple Modification in CMA-ES Achieving Linear Time and Space Complexity* (separable CMA-ES). PPSN (2008) — the diagonal-covariance variant S3 ships engine-local (no eigendecomp). https://hal.inria.fr/inria-00287367
- Bergstra, J. & Bengio, Y. *Random Search for Hyper-Parameter Optimization.* JMLR 13 (2012) — the random-search baseline. https://www.jmlr.org/papers/v13/bergstra12a.html

**Anti-overfitting / deflation (the admission gate — reused from S1)**
- Bailey, D. & López de Prado, M. *The Deflated Sharpe Ratio: Correcting for Selection Bias, Backtest Overfitting, and Non-Normality.* J. Portfolio Management (2014). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2460551
- Bailey, D., Borwein, J., López de Prado, M., Zhu, Q. *The Probability of Backtest Overfitting* (PBO via CSCV). J. Computational Finance (2017). https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2326253
- López de Prado, M. *Advances in Financial Machine Learning.* Wiley (2018) — purged + embargoed CPCV (Ch. 7); the OOS firewall S3 scores through.
- Grinold, R. & Kahn, R. *Active Portfolio Management* (2nd ed., 1999) — `IR ≈ IC·√breadth` (the diversification math behind F7).

**Novelty / quality-diversity (the diversity-pressure term)**
- Lehman, J. & Stanley, K. *Abandoning Objectives: Evolution Through the Search for Novelty Alone.* Evolutionary Computation 19(2) (2011) — novelty search (anti-collapse). https://doi.org/10.1162/EVCO_a_00025
- Mouret, J-B. & Clune, J. *Illuminating Search Spaces by Mapping Elites.* arXiv:1504.04909 — MAP-Elites / QD (a richer diversity scheme, recorded as future work). https://arxiv.org/abs/1504.04909

**Throughput (DAG reuse / CSE — the make-or-break lever)**
- Microsoft Qlib — string DSL + lazy, memoized expression-DAG evaluation. https://github.com/microsoft/qlib (operators: `qlib/data/ops.py`)
- Zipline Pipeline — memoized `Term` DAG nodes, topological execution, window-safe guards. https://github.com/quantopian/zipline (`zipline/pipeline/term.py`)
- Click, C. & Cooper, K. *Combining Analyses, Combining Optimizations* (value numbering / hash-consing) — the CSE foundation the engine's DAG already implements; S3 lifts the key to cross-generation dedup.
- atx-engine internal: `atx-engine/research/alpha-expression-dsl-deep-dive.md` §5.3 (hash-consing → 10–100× evaluation reduction) and `worldquant-systems-deep-dive.md` §9 (the factory throughput story).

---

## §8 — Self-review (against the spec)

- **Spec coverage:** S3.0→S3-0; S3.1 (AST mutation: op-swap/field-swap/window-const perturbation, type-safe, `analyze`-validated)→S3-1; S3.2 (subtree crossover + canonical-hash dedup)→S3-2; S3.3 (grid/random/CMA-ES param optimizer over fractional constants)→S3-3; S3.4 (pool-aware fitness = WQ fitness × marginal corr-to-pool, sub-universe robustness, deflated)→S3-4; S3.5 (evolutionary driver: population/selection/elitism/novelty, seeded)→S3-5; S3.6 (mine→gate→admit + bench + close)→S3-6. Every spec unit + exit criterion maps to a task. ✅
- **User asks satisfied:** data structures (`Genome`, `OpCatalog`, `ParamSpace`, `CanonSet`, `FitnessReport`, `SearchDriver`, `Factory`, `FactoryReport`) ✅; algorithms + pseudocode (§4.1–§4.8: clone-subtree remap, the three mutations, type-compatible crossover, the canonicalization+sound-hash, sep-CMA-ES, OOS+deflated pool-aware fitness, the deterministic population loop, the mine→gate→admit) ✅; public/OSS references (§7: 101-Alphas, GP/gplearn/DEAP/AlphaGen, CMA-ES/sep-CMA-ES, DSR/PBO/CPCV, novelty/MAP-Elites, Qlib/Zipline) ✅; "ties everything together" — explicit consumption of P4 (`AlphaStore`/`AlphaGate`/`compute_metrics`), S1 (`deflated_sharpe`/`cpcv_folds`/`pbo_cscv`/`bias_audit`), S2 (`parallel_evaluate`/`signal_set_digest`) ✅.
- **As-built fixes applied (the recon's value):** genome-is-rebuilt-Ast not in-place-edited (§0.1); `analyze` is the validity oracle (§0.2); windows are tree literals (§0.3); no `Library` iterator → `OpCatalog` (§0.4); **no commutative canonicalization in p0 → S3 builds the sound stable hash** (§0.5); **no corr-to-pool helper → S3 writes it; gate uses MAX not mean** (§0.6); **type is `ReturnMetrics` not `PerfMetrics`; deflation takes loose moments** (§0.7); **sub-universe needs an alternate-Panel re-eval; S2 is in progress → single-thread fallback** (§0.8). ✅
- **Determinism rigor:** the load-bearing acceptance is the seeded byte-identical replay (`SearchDriver`/`Factory` two-runs-equal digests) + the parallel-digest invariance inherited from S2; RNG seeded by `(master_seed, gen, idx)` never worker/thread/time; selection in canonical-id order. ✅
- **Anti-snooping rigor:** the non-vacuous `NoiseAdmitsNothingAfterDeflation` (pure-noise → zero admits) mirrors S1's `catches_overfit_synthetic`; fitness is OOS via CPCV; the trial count feeds `deflated_sharpe`/`pbo_cscv`. ✅
- **Type consistency:** `Genome{ast,analysis,canon_hash}`, `OpCatalog::sample_compatible(shape,dtype,arity)`, `op_swap/field_swap/jitter_const(Genome,…,Xoshiro&)->Result<Genome>`, `subtree_crossover(Genome,Genome,Xoshiro&)`, `canonical_hash(const Ast&,ExprId)->u64`, `corr_to_pool(span,AlphaStore,Reduce)`, `pool_aware_fitness(...)->FitnessReport`, `SearchDriver::run(cfg,pool)->SearchResult`, `Factory::mine(cfg,pool,gate)->FactoryReport` — consistent across §2/§4/§5. Reused symbols match the as-built signatures verified in recon (`compile_batch`, `compute_metrics().fitness`, `deflated_sharpe(sr,T,skew,exkurt,N,opt)`, `cpcv_folds(spans,cfg)`, `pairwise_complete_corr`, `AlphaGate::admit`, `AlphaStore::insert`). ✅
