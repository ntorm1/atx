# Sprint 3 — Formulaic Alpha Factory — Implementation Progress

**Status:** ⏳ IN PROGRESS
**Worktree:** in-place (p0 precedent; shared branch — explicit pathspecs only, no push)
**Branch:** `feat/atx-core-stdlib`
**Base:** `feat/atx-core-stdlib` @ `8e126ee` (S2 CLOSED at `d7a1b75` + close docs `8e126ee`; S1 CLOSED `2158a17`; P4 CLOSED `f2d22f4`)
**Started:** 2026-06-07
**Source plan:** [`sprint-3-formulaic-alpha-factory-implementation-plan.md`](sprint-3-formulaic-alpha-factory-implementation-plan.md)
**Spec:** [`sprint-3-formulaic-alpha-factory.md`](sprint-3-formulaic-alpha-factory.md)

---

## Plan adjustments vs. the source plan (the §0 as-built amendment + this-run deltas)

The implementation plan's §0 reconciles the spec against reconnaissance of the merged engine. The eight load-bearing corrections, plus the one delta this run inherits:

1. **The AST has no in-place mutator — the genome is a rebuilt arena (§0.1).** `alpha::Ast` is a flat, index-addressed (`ExprId=u32`), relocatable arena (`nodes_`/`strings_`/`roots_`) with a public append-only builder (`add`/`intern`/`add_root`) and **no `Expr&` accessor**. Every mutation/crossover constructs a NEW `Ast` by replaying nodes through the builder; a subtree splice is an offset-remap deep copy. **`Expr::op` is a borrowed `const OpSig*` — the one `Library` owns the whole run and outlives every genome.**
2. **`analyze()` is the validity oracle (§0.2).** A candidate is valid iff `alpha::analyze(ast)` returns `Ok`; re-run it on every produced genome; an un-analyzable genome is a bug, never scored.
3. **Window/scale constants are `Literal` child operands, not operator metadata (§0.3).** The jitter classifier walks the tree: a `Literal` is a *Window* iff it is the trailing integer operand of a `shape_panel` Ts\* op, an *Hparam* iff peeled into `Expr::hparams[]` (`n_hparams>0`), else a *Scale*.
4. **No `Library` iterator, no commutative flag → S3 precomputes an `OpCatalog` (§0.4).** Group `OpCode` set + the 65 named `Library` rows by `(Shape,DType,arity)`; **declare** `{Add,Mul,MinP,MaxP,And,Or,CmpEq,CmpNe}` commutative (the registry won't tell you).
5. **No canonical/commutative normalization exists → S3 builds the dedup hash from scratch (§0.5).** p0 has only parse-time const-fold + one `pow(x,2)→mul(x,x)` rule; `dag.hpp` hashes child order sensitively and `NodeKeyHash` (wyhash, compile-time seeds) is **not** stable across runs. S3 writes its own commutative-sort canonicalization + a **stable** fixed-byte-layout hash keyed by field **name**.
6. **No corr-to-pool helper exists; the gate uses MAX not mean (§0.6).** `combine/correlation.hpp` exposes only `pairwise_complete_corr`; `AlphaGate::max_abs_corr_to_pool` is a private static MAX screen. S3-4 **writes** the public `corr_to_pool(cand, pool, Reduce)` (Max for the gate-consistent screen, Mean for the diversification discount). **Dangling-span hazard:** `AlphaStore::pnl()` spans dangle after the next `insert()` — compute corr-to-pool **before** inserting.
7. **The S1 eval type is `eval::ReturnMetrics` (not `PerfMetrics`); deflation takes loose moment scalars (§0.7).** WQ fitness is `combine::compute_metrics(...).fitness = sqrt(abs(returns)/max(turnover,0.125))*sharpe` (reuse verbatim). `eval::deflated_sharpe(sr,T,skew,exkurt,N,opt)` where **N is the trial count**; moments via `eval::skewness`/`eval::excess_kurtosis`; PBO via `eval::pbo_cscv`.
8. **Sub-universe robustness has no scoring-layer support → re-eval an alternate `Panel` (§0.8).** Universe masking exists only at Panel ingest (`UniversePolicy`); WQ sub-universe robustness re-runs `extract_streams` against an alternate `Panel`.

**This-run delta (supersedes §0.8's "S2 in progress"): S2 is now CLOSED (`d7a1b75`).** `parallel::{Pool, parallel_evaluate, signal_set_digest}` are available verbatim — the factory's generation eval runs the **parallel path** (digest byte-identical to single-thread, invariant across worker counts {1,2,4,8}), with `alpha::Engine::evaluate` as the documented single-thread fallback. The S2 throughput dependency is satisfied, not deferred.

---

## Kickoff risks

1. **sep-CMA-ES → full rotation-invariant CMA-ES is a Pattern-B atx-core L7 lift.** Full CMA-ES needs an eigendecomposition (`linalg`/`eigh`). S3 ships a **separable (diagonal-covariance) CMA-ES** engine-local in `factory/param_search.hpp` — no matrix decomposition, deterministic under a seeded `Xoshiro256pp`, well-suited to the low-dimensional fractional-constant search. Full CMA-ES recorded as the close residual (the S1 `stats_ext` / S2 `DetPool` precedent).
2. **Dominant correctness risk: data snooping.** The factory mass-produces candidates; without deflation it mass-produces overfit garbage. The non-vacuous gate is **noise → zero-admits AND planted-edge → admitted** (F4), fitness OOS-only on CPCV test folds (F3).
3. **Determinism under RNG + parallelism.** Every `Xoshiro256pp` seeded by `(master_seed, gen, cand_idx)`, never worker/thread/time; selection/reproduction in canonical-id order; parallel digest == single-thread (F1/F2). Two-runs-equal is mandatory per RNG-bearing unit.
4. **Shared-branch discipline.** Explicit-pathspec commits only (`git add -- <paths>`; `git commit -- <paths>`); NEVER `git add -A`; `git show HEAD --stat` after each commit (only factory files + this ledger); never touch `atx-core/*`; do not push. The working tree carries FOREIGN uncommitted/untracked files from a concurrent databento/Phase-3d effort (e.g. `alpha/parser.hpp`, `alpha/registry.hpp`, `CMakeLists.txt`, `data/shm_bar_feed.hpp`) — build against them, never commit them.

---

## Per-unit status

| Unit  | Title                                                        | Status | Commit SHA(s) | Tests | Notes |
|-------|-------------------------------------------------------------|--------|---------------|-------|-------|
| S3-0  | Marker + ledger + factory scaffold                          | ✅ done | `b0c07ac` | —    | `factory/fwd.hpp` (Genome/OpCatalog/ParamSpace/FitnessReport/SearchDriver/Factory fwd + F1/F4/F6 contract doc + eval switch point) + this ledger. |
| S3-1  | Genome substrate + OpCatalog + mutation operators           | ✅ done | `ee6ec50` + `c0f09d8` + `5da7cdf` | 8 | `Genome{ast,analysis,canon_hash}` value-owned; `clone`/`clone_subtree` (post-order DFS + memo + field re-intern, `Expr::op` carried verbatim under the shared-`Library` `// SAFETY:` note)/`rebuild_with(target, (Expr&,Ast&)edit)`/`analyze_into`. `OpCatalog` buckets BOTH the 65 named `builtin_ops()` rows AND the bare infix/prefix `OpCode` set (arith `{Add,Sub,Mul,Div,Pow}`, cmp `{CmpLt..CmpNe}`, logical `{And,Or}`, unary `{Neg,Not}`) by `(Shape,DType,arity)` + declared-commutative `{Add,Mul,MinP,MaxP,And,Or,CmpEq,CmpNe}`. `op_swap` (Call→named path, Unary/Binary→opcode path), `field_swap`, `jitter_const` + `classify_literals` (Window/Scale; Hparam best-effort). All analyze-validated (F5); same-seed-identical (F1). *(Spec review caught a MUST-FIX: first cut dropped the infix/OpCode op-swap half (§0.4+§4.2) → fixed in `c0f09d8` (op_swap now reaches Add→{Sub,Mul,Div,Pow}, 300/300 Ok, distinct=4, non-vacuous). Quality review: 2 bounds-check findings declined as false-positive-by-Genome-invariant (children always in-bounds in a valid analyzed Ast); 1 unused-include hygiene fix `5da7cdf`.)* |
| S3-2  | Subtree crossover + canonical-hash dedup                    | ✅ done | `cf30730` + `6d6a5a3` | 11 | `canonical_hash(const Ast&, ExprId) noexcept` — recursive, per-call memo, STABLE fixed-byte FNV-1a fold (NOT wyhash/NodeKeyHash), Field keyed by NAME bytes, Literal by `bit_cast<u64>`, commutative-child sort. `CanonSet` dedup. `subtree_crossover` at `compatible()` cuts (shape-lattice broadcast + EXACT DType; `clone_subtree` splice; analyze backstop; `cfg.max_lookback` donor cap). Crossover non-vacuous (146/200 & 102/200 Ok). **F6 soundness:** the load-bearing `HashEqualImpliesBitIdenticalEval` evaluates BOTH exprs via the real VM and bit-compares. *(Spec review caught a real F6 gap the NaN test missed: signed-zero asymmetry — `min(-0.0,+0.0)=+0.0 ≠ min(+0.0,-0.0)=-0.0` (VM `a<b?a:b`) makes MinP/MaxP operand-swap NOT bit-identical → unsound to hash commutative. Fixed in `6d6a5a3`: narrowed `is_hash_commutative` to the 6 truly-symmetric ops `{Add,Mul,And,Or,CmpEq,CmpNe}`; added `MinMaxSwapIsNotBitSafe` (real-VM bit-divergence + hash-now-differs, guard-verified: re-adding the ops makes it fail with a hash collision) + `RetainedCommutativeStaysSoundOnSignedZero`. Soundness > dedup rate.)* Quality review: no blocking/important findings. |
| S3-3  | Parameter optimizer (grid / random / sep-CMA-ES)           | ✅ done | `50be881` + `37af0b3` | 6 | `ParamSpace`/`dims()`/`box`/`extract_free_constants` (reuses `classify_literals`; Window dims `[1,250]`, Scale dims `[0,2v]`-ish span). `optimize_params_raw` + genome `optimize_params` (→`instantiate` via `rebuild_with`→`analyze_into`, F5; un-analyzable scores `-inf`). Methods `Grid`/`Random`/`SepCmaEs`. **sep-CMA-ES** (separable, diagonal C, NO eigendecomp — full CMA-ES = L7 residual): converges to interior optimum **(3.000002, 7.000077)** within 0.05; seed-robust across 4 seeds (spec review: all within 7e-4 of a shifted optimum; gens=1 fails → non-vacuous). F1 same-seed byte-identical (`std::stable_sort` offspring). §4.5 deviation: path/step updates from the realized POST-clamp normalized step (canonical `C^{-1/2}(m-m_old)/σ` form, clamp-safe) — spec-reviewer-approved sound variant. *(Quality review: 4 fixes in `37af0b3` — removed a null-reference UB in the verbatim Grid test (real stack rng + dropped pragma), guarded K==0/empty-space (zero-free-constants boundary → graceful no-op, `ZeroFreeConstantsIsNoOp` test), floored `cdiag[k]` before division (degenerate-coord 1e20 guard), added `<limits>`.)* |
| S3-4  | Pool-aware fitness (WQ × marginal-corr × robustness, deflated) | ✅ done | `7df5b10` + `fe607a3` | 4 | `corr_to_pool(span, AlphaStore, Reduce{Max,Mean})` (the missing §0.6 helper over `pairwise_complete_corr`; computed BEFORE any insert — dangling-span `// SAFETY:`). `pool_aware_fitness` → `FitnessReport{wq,redundancy,diversify,robust,raw,dsr,haircut_sharpe}`: eval-once → `extract_streams` → per-CPCV-**test**-fold `compute_metrics().fitness` mean (OOS, F3) × `diversify=1-mean\|corr\|` (F7) × sub-universe `robust` (alternate-Panel re-eval; `1.0` degenerate when no weak universe, §0.8), **deflated** by `eval::deflated_sharpe(sr/√252, T, skew, exkurt, N=trial_count, …)` (F4). **De-annualization** (compute_metrics.sharpe is ×√252; deflated_sharpe wants per-period) — spec-reviewer-confirmed correct + self-consistent. **WQ thesis proven non-vacuously** (A=`rank(close)` redundant-strong wq=106 raw=0 vs B=`rank(rev)` diversifying-weak wq=4.4 raw=3.77 — B wins on `raw` only via diversification; fails if `diversify` dropped). F4 lever bites (haircut 1.131→0.662). **NOTE — required eval-layer ODR fix bundled in `7df5b10`:** `eval/cpcv.hpp` + `eval/pbo.hpp` each defined byte-identical `detail::binomial`/`next_combination` ("assumed no single TU includes both"); S3-4 (and S3-6) is the first to include BOTH (fitness→cpcv, bias_audit→pbo) → redefinition. Fix: `cpcv.hpp` now `#include eval/pbo.hpp` + reuses its helpers (−55/+13, behavior-identical, clean DAG — pbo doesn't include cpcv). Controller force-recompiled all 6 at-risk TUs (eval_cpcv/eval_pbo/parallel_run/parallel_determinism/bias_audit/factory_fitness) → 28/28 green; full suite 1760/1760. Quality review: no blocking/important findings (1 unused-include hygiene fix `fe607a3`). |
| S3-5  | Evolutionary search driver                                  | ⏳     |               |       | |
| S3-6  | Factory integration + anti-snooping proof + bench + close   | ⏳     |               |       | |

---

## Sprint commits

| SHA | Unit | Subject |
|-----|------|---------|
| `b0c07ac` | S3-0 | docs(s3-0): open sprint-3 formulaic-alpha-factory ledger + scaffold |
| `ee6ec50` | S3-1 | feat(s3-1): genome rebuild substrate + OpCatalog + mutation operators |
| `c0f09d8` | S3-1 | fix(s3-1): infix/OpCode op-swap (Unary/Binary candidates + opcode buckets) |
| `5da7cdf` | S3-1 | fix(s3-1): drop unused typecheck.hpp include from op_catalog |
| `cf30730` | S3-2 | feat(s3-2): subtree crossover + sound canonical-hash dedup |
| `6d6a5a3` | S3-2 | fix(s3-2): narrow hash-commutative set — drop MinP/MaxP (signed-zero asymmetry breaks F6 soundness) |
| `50be881` | S3-3 | feat(s3-3): parameter optimizer (grid/random/sep-CMA-ES) |
| `37af0b3` | S3-3 | fix(s3-3): guard K==0/degenerate sep-CMA, remove null-rng UB, +<limits>, zero-free-const test |
| `7df5b10` | S3-4 | feat(s3-4): pool-aware fitness (WQ × marginal-corr × robustness, OOS + deflated) — incl. eval/cpcv.hpp ODR dedup |
| `fe607a3` | S3-4 | fix(s3-4): drop unused <cmath> from factory_fitness_test |

---

## Close residuals → p1 ROADMAP future-work backlog

_(filled at S3-6 close. Expected: full rotation-invariant CMA-ES → atx-core L7 `eigh`/`cma`; first-class scoring-layer universe mask vs the S3-4 alternate-Panel re-eval; persisted canonical-hash dedup → S4 library-wide index.)_

## Baton → next

_(filled at close.)_
