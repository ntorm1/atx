# Sprint S3 — Formulaic Alpha Factory (user reference)

**Status:** ✅ CLOSED 2026-06-08 (`feat/atx-core-stdlib @ 705cf22`). **Spec:** [`sprint-3-formulaic-alpha-factory.md`](sprint-3-formulaic-alpha-factory.md) · **Plan:** [`sprint-3-formulaic-alpha-factory-implementation-plan.md`](sprint-3-formulaic-alpha-factory-implementation-plan.md) · **Ledger:** [`sprint-3-progress.md`](sprint-3-progress.md)

S3 is the engine's **discovery** layer — the first time the engine *finds* alphas instead of merely *scoring* the ones a human wrote. It is a **seeded, deflated, pool-aware evolutionary search** over the Phase-3 alpha DSL that sits on top of the existing hash-consed-DAG + CSE + columnar VM (which already evaluates thousands of overlapping expressions cheaply) and the S1/S2 substrate (an honest deflated scorer + deterministic multicore). One header-only inline subsystem, no atx-core changes.

```
atx/engine/factory/   — atx::engine::factory   (genome substrate + search operators + pool-aware fitness + search driver + mine→gate→admit)
```

**The one fact that defines this sprint:** *a new alpha's worth is NOT its standalone Sharpe — it is its **marginal contribution to a diversified pool*** (WorldQuant's ~15.9% mean-pairwise-correlation thesis). The factory optimizes **diversification**, and S1 **deflation** guarantees a pure-noise population admits **nothing**.

---

## The genome (the substrate everything else rebuilds)

`alpha::Ast` is a flat, index-addressed (`ExprId=u32`), relocatable arena with a public append-only builder (`add`/`intern`/`add_root`) and **no in-place `Expr&` mutator**. So a genome is a **rebuilt Ast**, never an edited tree:

```cpp
struct Genome { alpha::Ast ast; alpha::Analysis analysis; atx::u64 canon_hash; };
```
Every operator constructs a NEW `Ast` through the builder and re-runs `alpha::analyze()` as the validity oracle (**F5** — an un-analyzable genome is a bug, never scored). `Expr::op` borrows a `const alpha::OpSig*` from the **one run-wide `Library`**, which **MUST outlive** the driver and every genome (the load-bearing SAFETY note). `clone()` / `clone_subtree()` (offset-remap deep copy + field re-intern) / `rebuild_with()` / `analyze_into()` are the rebuild primitives.

---

## Public API

### `factory/op_catalog.hpp` — op candidates by type
```cpp
class OpCatalog {                                    // built once from the OpCode set + the Library's 65 named rows
  explicit OpCatalog(const alpha::Library&);
  [[nodiscard]] static bool is_commutative(alpha::OpCode) noexcept;
  // sample a type-compatible replacement op for an (Shape,DType,arity) bucket
};
```
The `Library` exposes no iterator and no commutative flag, so S3 precomputes the catalog: it buckets BOTH the 65 named `builtin_ops()` rows AND the bare infix/prefix `OpCode` set (arith `{Add,Sub,Mul,Div,Pow}`, cmp `{CmpLt..CmpNe}`, logical `{And,Or}`, unary `{Neg,Not}`) by `(Shape,DType,arity)`, and **declares** the commutative set.

### `factory/mutation.hpp` — type-safe AST mutation
```cpp
Result<Genome> op_swap(const Genome&, const OpCatalog&, Xoshiro256pp&);       // op → type-compatible op
Result<Genome> field_swap(const Genome&, std::span<const std::string_view>, Xoshiro256pp&); // field → field
Result<Genome> jitter_const(const Genome&, Xoshiro256pp&, JitterCfg);          // perturb a Window/Scale literal
std::vector<ClassifiedConst> classify_literals(const Genome&);                 // ConstKind{Window,Scale,Hparam}
```
Each rebuilds + `analyze()`-validates (F5). The window/scale classifier walks the tree (a `Literal` is a *Window* iff it is the trailing integer operand of a `shape_panel` Ts\* op). **`op_swap` is a known residual** — it can build an analyze-valid genome that corrupts the VM SlotPool at evaluate; the driver gates it OFF by default.

### `factory/canonical.hpp` — sound canonical-hash dedup (F6)
```cpp
[[nodiscard]] atx::u64 canonical_hash(const alpha::Ast&, alpha::ExprId) noexcept;
[[nodiscard]] atx::u64 canonical_hash(const Genome&) noexcept;
class CanonSet { bool contains(u64) const; void insert(u64); usize size() const; };
```
A **stable** fixed-byte-layout FNV-1a fold (NOT the run-unstable wyhash `NodeKeyHash`): `Field` keyed by NAME bytes, `Literal` by `bit_cast<u64>`, commutative-children sorted. **Only the 6 truly bit-symmetric ops `{Add,Mul,And,Or,CmpEq,CmpNe}` are sorted** — `MinP`/`MaxP` are excluded because signed-zero asymmetry (`min(-0.0,+0.0)=+0.0 ≠ min(+0.0,-0.0)=-0.0` under the VM's `a<b?a:b`) makes their operand-swap NOT bit-identical. **Soundness contract: hash-equal ⇒ the VM evaluates bit-identical** (proven by a real-VM bit-compare test, fail-on-bad guard-verified).

### `factory/crossover.hpp` — subtree crossover
```cpp
Result<Genome> subtree_crossover(const Genome& a, const Genome& b, Xoshiro256pp&, CrossoverCfg{max_lookback=250});
```
Splices a type-compatible donor subtree from B into a cut in A (shape-lattice broadcast `Scalar<CrossSection<Panel` + EXACT DType), within the lookback cap; `analyze()` backstop.

### `factory/param_search.hpp` — fractional-constant optimizer
```cpp
struct ParamSpace { /* free Window/Scale literals of a template genome + per-dim box */ };
enum class Method { Grid, Random, SepCmaEs };
ParamResult optimize_params(const Genome&, Fitness&&, Xoshiro256pp&, ParamCfg);  // → instantiate via rebuild_with → analyze_into
```
`SepCmaEs` is a **separable (diagonal-covariance) CMA-ES** — no eigendecomposition (the full rotation-invariant optimizer is an atx-core L7 residual). Converges to the interior optimum, seed-robust, F1 byte-identical.

### `factory/fitness.hpp` — pool-aware marginal-contribution fitness (the thesis)
```cpp
enum class Reduce { Max, Mean };
[[nodiscard]] f64 corr_to_pool(std::span<const f64> cand, const combine::AlphaStore& pool, Reduce);  // computed BEFORE insert (§0.6 dangling-span)
struct FitnessReport { f64 wq, redundancy, diversify, robust, raw, dsr, haircut_sharpe; };
Result<FitnessReport> pool_aware_fitness(const Genome&, const combine::AlphaStore& pool,
                                         const alpha::Panel&, const WeightPolicy&,
                                         const exec::ExecutionSimulator&, const FitnessCfg&);
```
`raw` = WQ fitness (`compute_metrics().fitness`, OOS-only over CPCV **test** folds, **F3**) × `diversify` (`1 − mean|corr-to-pool|`, **F7**) × `robust` (sub-universe alternate-`Panel` re-eval) — then **deflated** by `eval::deflated_sharpe(sr/√252, T, skew, exkurt, N=trial_count, …)` (**F4**). The WQ thesis is proven non-vacuously: a diversifying weak alpha beats a strong-but-redundant one on `raw`, and fails if `diversify` is dropped.

### `factory/search_driver.hpp` — the seeded population loop
```cpp
struct SearchConfig { u64 master_seed; usize population, generations, elites, k_tournament;
                      f64 p_cross, novelty_w; u16 max_lookback; usize n_workers;
                      FitnessCfg fitness; bool enable_op_swap{false}; };
struct SearchResult { u64 digest; usize trial_count, candidates_generated; f64 dedup_pct;
                      std::vector<f64> best_fitness_per_gen;
                      std::vector<Genome> all_scored, admitted_candidates; u64 seed; };
class SearchDriver {
  SearchDriver(const alpha::Library&, const alpha::Panel&, const WeightPolicy&,
               const exec::ExecutionSimulator&, std::vector<std::string> seed_exprs,
               std::vector<std::string> panel_fields);
  SearchResult run(const SearchConfig&, const combine::AlphaStore& pool);
};
```
Per generation: `CanonSet` dedup (F6) → eval-digest fold → `pool_aware_fitness` score → **canonical-id-ordered** tournament select → **id-seeded** crossover/mutation reproduce → **raw-fitness** elitism → canonical-structure novelty pressure.

### `factory/factory.hpp` — mine → gate → admit
```cpp
struct FactoryReport { usize admitted, evaluated, trials; f64 dedup_pct, cse_pct; u64 seed, digest; };
class Factory {
  Factory(const alpha::Library&, const alpha::Panel&, const exec::ExecutionSimulator&, const WeightPolicy&);
  FactoryReport mine(const FactoryConfig&, combine::AlphaStore& pool, const combine::AlphaGate& gate);
};
```
`mine` runs the driver, ranks candidates **by deflated fitness** (best first), and admits each that passes `AlphaGate::admit` **AND** `dsr >= cfg.min_dsr` into the pool (PnL/positions/metrics computed BEFORE insert — §0.6).

---

## Guarantees (all proven by non-vacuous tests — fail-on-bad AND pass-on-good)

- **F1 — seed-by-id determinism:** every `Xoshiro256pp` draw across init/mutation/crossover/param-search/selection/reproduction is seeded purely by `(master_seed, gen, candidate_index)` via a SplitMix mix — **never** worker/thread/time. A seeded `Factory::mine` / `SearchDriver::run` **replays byte-identically** (equal `digest`).
- **F2 — worker-invariance:** selection/reproduction iterate the population in a total, value-based **canonical order** (`canon_hash`, ast size, root id) fixed BEFORE any RNG draw; the eval digest equals the single-thread path and (on the seed grammar) `signal_set_digest(parallel_evaluate) == single_thread_digest`, invariant across {1,2,4} workers.
- **F4 — deflation kills snooping (the anti-snooping proof, non-vacuous):** under the SAME gate + `min_dsr` bar, a **pure-noise panel admits 0** while a **real-signal panel admits survivors** — and the **DSR bar is load-bearing** (with deflation removed, a noise candidate clears all four P4 gate floors and gets admitted; deflation is what rejects it). The deflation **N is the search's running distinct-candidate count** (`res.trial_count`), so the bar **auto-scales with search effort** — a larger mine deflates harder.
- **F5 — in-grammar validity:** every scored genome is `analyze()`-valid (every operator rebuilds + re-analyzes; a 0-alpha eval is guarded, not aborted).
- **F6 — canonical-dedup soundness:** hash-equal ⇒ the VM evaluates bit-identical; structurally-equivalent expressions are never re-evaluated (measured `dedup_pct`).
- Builds under clang-cl `/W4 /permissive- /WX` + strict-FP; **38 factory tests** across 8 suites (genome 3 · mutation 5 · crossover 3 · canonical 8 · param-search 6 · fitness 4 · search-driver 5 · integration 4); full engine suite **1769/1769 green**; **atx-core unmodified**.

## Known residuals (p1 backlog)

1. **Full rotation-invariant CMA-ES → atx-core L7** (`eigh`/`cma`) — S3 ships separable diagonal CMA-ES.
2. **First-class scoring-layer universe mask** — vs the S3-4 alternate-`Panel` sub-universe re-eval.
3. **Persisted canonical-hash dedup → S4 library-wide index** — the in-run `CanonSet` is per-run (S4-2 lands the cross-run index).
4. **op_swap VM-corruption fix** — gated OFF (`enable_op_swap{false}`); root-cause the SlotPool over-acquire before re-enabling.
5. **`parallel_evaluate` in the driver** — the driver runs the §4.8-sanctioned single-thread fallback; reinstate the S2 parallel path (one-line eval swap) once the per-worker warm-up+reuse is safe on evolved stateful candidates.

## Baton → next

S3 hands **S4** a stream of admitted, deduplicated, deflation-gated alphas + the structural `canonical_hash` it persists into the library-wide dedup index; the `Factory`/`SearchDriver` determinism digest is the replay contract every later discovery feature re-proves against. The marginal-contribution fitness (`pool_aware_fitness`) is the scoring seam **S5**'s learned alphas and **S7**'s portfolio construction reuse — a candidate's worth is always its diversification of the pool, deflated, never its standalone Sharpe.
