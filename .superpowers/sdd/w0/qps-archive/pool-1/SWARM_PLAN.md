# atx-engine: implementation plan for the zoo-to-portfolio lanes

Frozen base: `C:\atx-wt\pool-1`. Every lane leases its own pool worktree from the same frozen SHA.

## 0. How the build is wired, and what that means for conflicts

| File | How it works | Conflict risk | Mitigation |
|---|---|---|---|
| `atx-engine/tests/CMakeLists.txt` | Each group directory is globbed with `file(GLOB CONFIGURE_DEPENDS "${grp}/*_test.cpp")` and builds `atx-engine-<grp>-tests`. Groups: alpha, risk, data, factory, parallel, learn, eval, library, combine, fund, book, core, regime, store, quant | **None.** A new `tests/<grp>/*_test.cpp` is picked up with no CMake edit | Lanes only drop files into their group directory. |
| `atx-engine/bench/CMakeLists.txt` | Globs `*_bench.cpp` into `atx-engine-bench` (built only when `ATX_BUILD_BENCH=ON`) | **None** | New `bench/<lane>_bench.cpp` files are auto-discovered. |
| `atx-impl/tests/CMakeLists.txt` | Globbed | **None** | — |
| `atx-engine/CMakeLists.txt` | The `add_library(atx-engine …)` source list is **explicit**; each `src/**/*.cpp` is listed by hand | **HIGH.** Every lane that adds a `.cpp` edits the same hunk | Lane 0 scaffold (below). Otherwise new code is **header-only**, the house pattern that `vm.hpp`, `ts_ops.hpp` and `combiner.hpp` already follow. |
| `atx-impl/CMakeLists.txt` | `add_library(atx-impl-core …)` has an explicit source list | HIGH, but only Lane 8 touches it | Lane 8 owns it outright. |
| `CMakePresets.json` | Shared | Medium | Lane 0 adds the `release-bench` preset once. |
| Unity build (batch 16, every group except `parallel`) | File-local helpers in `namespace {}` from different test files merge into one TU | ODR collisions across lanes landing in the same group | **Rule:** each new test file puts its helpers in a **named** namespace, `namespace atx_test_<lane>_<file> {}`. Never reuse the names `make_panel`, `Lcg` or `frictionless_sim` at file scope. |

**Lane 0 (orchestrator, one commit on the frozen base, before fan-out; about 15 minutes):**
1. Append these `.cpp` stubs to `atx-engine/CMakeLists.txt`. Each stub is an empty TU containing only `#include` of its header.
   - `src/alpha/subtree_cache.cpp`
   - `src/factory/rewrite.cpp`
   - `src/factory/fidelity.cpp`
   - `src/eval/trial_registry.cpp`
   - `src/eval/multiple_testing.cpp`
   - `src/combine/signal_combiner.cpp`
   - `src/combine/orthogonalize.cpp`
   - `src/risk/model_validation.cpp`
   - `src/risk/hybrid_factor_model.cpp`
   - `src/risk/cost_terms.cpp`
   - `src/book/replay_cost.cpp`
2. Create empty headers for those stubs, so the tree configures and builds green.
3. Add a `release-bench` preset to `CMakePresets.json`: inherits `dev`, `CMAKE_BUILD_TYPE=Release`, `ATX_BUILD_BENCH=ON`, `ATX_TEST_GROUPS=all`.
4. Freeze that SHA as the lane base.

After Lane 0, **no lane edits `atx-engine/CMakeLists.txt`**. A lane that finds it needs another `.cpp` makes it header-only (`inline`) or requests it at integration time.

**Rule for files that already exist:** each one has exactly one owning lane, listed in the "Modified" line of each lane. Other lanes may `#include` it but must not edit it.

---

## Lane 1: Rolling and cross-sectional kernels, plus the streaming engine

**Goal:** make per-cell cost independent of window length d, and make live evaluation O(1) per day.
- Close these gaps: sliding corr/cov/decay/regression; order-statistic `ts_rank`/`med`/`mad`/`ts_quantile`; radix `rank`; `StreamingEngine`.
- Measured today: correlation(20) is 69 ns/cell, decay_linear 30, ts_rank 39, rank 51.

**New files**
- `atx-engine/include/atx/engine/alpha/ts_sliding.hpp`: stateful push/pop window kernels. The same structs serve the batch sweep and the streaming engine.
- `atx-engine/include/atx/engine/alpha/ts_order_stat.hpp`:
  - a sorted small-array window for d ≤ 64 (branchless SIMD count via xsimd)
  - a Fenwick tree over rank-compressed values for larger d
- `atx-engine/include/atx/engine/alpha/cs_radix.hpp`: LSD radix argsort on sign-flipped f64 bits. It is stable, and NaNs go last, deterministically.
- `atx-engine/include/atx/engine/alpha/streaming_engine.hpp`

**Modified (owned):**
- `alpha/ts_ops.hpp`: extend `ts_is_online_variance_op` / `tsv_welford_dispatch` to route Corr, Cov, DecayLinear/Wma, TsRegression/Slope/Resid under ResearchFast, and route TsRank/Med/Mad/Quantile always.
- `alpha/cs_ops.hpp`: `rank`, `quantile` and `group_rank` use the radix argsort.
- Not modified: `vm.hpp`.

**API sketch**
```cpp
namespace atx::engine::alpha::sliding {
struct CoMoment { // Neumaier-compensated Sx,Sy,Sxx,Syy,Sxy + count; Pebay remove
  void push(f64 x, f64 y) noexcept; void pop(f64 x, f64 y) noexcept;
  f64 corr() const noexcept; f64 cov() const noexcept; f64 slope() const noexcept; f64 resid(f64 x, f64 y) const noexcept; };
struct LinDecay { void push(f64 x) noexcept; void pop(f64 x) noexcept; f64 value(u32 d) const noexcept; }; // W'=W+d*x-S
void sweep_comoment(OpCode, std::span<const f64> x, std::span<const f64> y, u32 d, std::span<f64> out);
}
namespace atx::engine::alpha::ordstat {
class Window { public: explicit Window(u32 d); void push(f64); void pop(f64);
  f64 rank_of_last() const; f64 quantile(f64 q) const; f64 median() const; }; }
void cs_argsort_radix(std::span<const f64> x, std::span<u32> perm, std::span<u32> scratch) noexcept;

class StreamingEngine {           // header-only; owns per-instruction ring/accumulator state
 public:
  static core::Result<StreamingEngine> create(const Program&, u32 n_instruments, EvalMode);
  core::Result<void> warm(const Panel& lookback);                   // prime state from history
  core::Result<std::span<const f64>> step(const CrossSection& today); // one row per call
};
```

**Tests** (`atx-engine-alpha-tests`, dropped into `tests/alpha/`)
- `alpha/ts_sliding_comoment_test.cpp` (suites `TsSlidingCoMoment_*`): matches the oracle at atol=rtol=1e-9 over NaN runs, warm-up and d ∈ {2,5,20,60,250}; includes an adversarial large-offset series to test cancellation.
- `alpha/ts_order_stat_test.cpp` (`TsOrderStat_*`): **bit-exact** against the oracle in both eval modes.
- `alpha/cs_radix_rank_test.cpp` (`CsRadixRank_*`): bit-exact against the current rank, covering ties, ±0, NaN and ±inf.
- `alpha/streaming_engine_test.cpp` (`StreamingEngine_*`): on every one of the 101 WQ alphas that uses only supported ops, the streaming output for the last K days equals the last K rows of batch `evaluate`. Tolerance is bit-exact in AuditExact and 1e-9 in ResearchFast.
- The existing `AlphaVm_Differential`, `AlphaConformance_*`, `TsWelford*` and `TsEvalMode` must stay green.

**Bench:** `bench/alpha_kernels_bench.cpp` covers corr/cov/decay/ts_rank/rank at 512×3000 and d ∈ {10,20,60}, reporting ns/cell. It also has `BM_StreamingStep` (µs per alpha-day).

**Acceptance (Release, `release-bench` preset)**
- ResearchFast: correlation(20) ≤ 15 ns/cell, decay_linear(20) ≤ 10.
- ts_rank(20) ≤ 15; rank at 3000 names ≤ 20.
- Streaming step does not grow with lookback: 250 vs 60 days within 10%.
- AuditExact is byte-identical for every op not explicitly routed.

---

## Lane 2: VM planner, subtree cache, cross-worker CSE, and the throughput gate

**Goal:** stop recomputing shared subtrees across genomes, alphas and workers, and put a regression gate on throughput. Closes: strategy B, `evaluate_root`/`evaluate_nodes`, Cs date-parallelism, element-wise fusion plus zero-copy LoadField, and the release benchmark gate.

**New files**
- `atx-engine/include/atx/engine/alpha/subtree_cache.hpp` + `src/alpha/subtree_cache.cpp` (stub from Lane 0)
- `atx-engine/include/atx/engine/alpha/fusion.hpp`: the bytecode pass that builds `FusedElementwise` micro-programs.
- `atx-engine/include/atx/engine/parallel/global_dag_eval.hpp`: strategy B, a level-scheduled union DAG.
- `atx-engine/bench/baselines/alpha_throughput.json` and `scripts/bench-gate.ps1`. The script parses `--benchmark_format=json` and fails on a regression of more than 20%.

**Modified (owned):**
- `alpha/vm.hpp`: add `evaluate_nodes` and `set_cs_pool`, and dispatch the fused opcode.
- `alpha/bytecode.hpp`, `src/alpha/bytecode.cpp`: fused opcode and liveness-based slot reuse.
- `alpha/dag.hpp`
- `parallel/batch_eval.hpp`, `src/parallel/batch_eval.cpp`

**API sketch**
```cpp
struct SubtreeKey { u64 node_hash; u64 panel_digest; EvalMode mode; };
class SubtreeCache {                         // LRU under byte budget; publish-then-read-only
 public:
  explicit SubtreeCache(std::size_t byte_budget);
  std::shared_ptr<const PanelBuf> find(const SubtreeKey&) const;
  void publish(const SubtreeKey&, PanelBuf&&);   // serial phase only
  CacheStats stats() const;                  // hits, misses, bytes, evictions
};
class Engine { /* existing */ 
  core::Result<void> evaluate_nodes(const Program&, std::span<const NodeId>, SubtreeCache* = nullptr);
  core::Result<std::span<const f64>> evaluate_root(const Program&, NodeId, SubtreeCache* = nullptr);
  void set_cs_pool(parallel::DetPool*) noexcept; };
core::Result<std::vector<SignalSet>> parallel_evaluate_shared(std::span<const Expr>, const Panel&,
                                                             parallel::DetPool&, SubtreeCache&);
```

**Tests**
- `alpha/subtree_cache_test.cpp` (`SubtreeCache_*`): LRU budget, key isolation by panel digest, and cached equals fresh byte-for-byte.
- `alpha/fusion_test.cpp` (`AlphaFusion_*`): the fused program is bit-identical to the unfused one on all 101 WQ alphas.
- `alpha/vm_cs_pool_test.cpp` (`AlphaVmCsPool_*`): pool=1 vs pool=8 gives identical digests.
- `parallel/global_dag_eval_test.cpp` (`ParallelGlobalDag_*`): strategy B equals strategy A byte-for-byte.
  - Location: the `parallel` group. It is outside Unity, but named namespaces are still used.
- The existing `AlphaVm_ZeroAlloc`, `AlphaBatch_*`, `ParallelBatchEval` and `ParallelDeterminism` must stay green.

**Bench:** `bench/alpha_wq101_battery_bench.cpp` runs all 101 alphas at 3000×2520 and reports ns/cell per opcode family, µs per alpha-day, CSE %, cache-hit % and a threads sweep {1,2,4,8,16}.

**Acceptance**
- Baseline JSON is checked in, and `bench-gate.ps1` fails on a regression of more than 20%.
- WQ101 battery at 8 threads is at least 3× faster than today's strategy A.
- A second pass with a warm cache gives ≥ 90% hits.
- Add a line to `LEDGER.md` with the Release numbers.

---

## Lane 3: Search-factory throughput and quality

**Goal:** more distinct, non-redundant genomes per second. Closes: multi-fidelity racing, semantic canonicalization plus output fingerprint dedup, effective-N and cross-trial V[SR] for DSR, a novelty archive with a sketch index, and the alphas/sec benchmark.

**New files**
- `factory/rewrite.hpp` + `src/factory/rewrite.cpp`: rule table covering `neg∘neg`, `rank∘rank`, `rank(c·x), c>0`, `abs∘abs`, `sign∘sign`, and scale/sign invariance flags.
- `factory/fingerprint.hpp`: hash of the rank-quantized signal on a fixed probe slice.
- `factory/fidelity.hpp` + `src/factory/fidelity.cpp`: `FidelityCfg`, rungs, and deterministic eta promotion with ties broken by canon hash.
- `factory/sketch_index.hpp`: z-normalized PnL random projection to 128 dims, a flat SIMD top-k search with an exact recheck, and a farthest-point archive.

**Modified (owned):**
- `factory/search_driver.hpp`, `src/factory/search_driver.cpp`
- `factory/canonical.hpp`, `src/factory/canonical.cpp` (calls `rewrite` before hashing, behind `CanonCfg::semantic`)
- `factory/behavior.hpp`
- `factory/op_catalog.hpp`: invariance flags.

**Dependency:** Lane 3 only **consumes** Lane 4's `TrialRegistry` and Lane 2's `SubtreeCache` through `SearchConfig` pointers that default to `nullptr`. Each side builds against the Lane 0 stub headers, and the wiring is switched on at integration (§9).

**API sketch**
```cpp
struct FidelityCfg { bool enabled=false; std::array<Rung,3> rungs; f64 eta=1.0/3; };
struct Rung { u32 date_stride, inst_stride; u32 n_folds; };
std::vector<GenomeId> race(std::span<const Genome>, const FidelityCfg&, const RungEvaluator&); // all evals counted as trials
Genome canonical_rewrite(const Genome&, const OpCatalog&);  // idempotent, bit-safe rules only
u64 output_fingerprint(std::span<const f64> signal_probe, u32 n_quant = 32);
class SketchIndex { public: void add(AlphaId, std::span<const f64> pnl); std::vector<Neighbor> topk(std::span<const f64>, u32 k) const; };
```

**Tests** (`atx-engine-factory-tests`, `tests/factory/`)
- `factory_rewrite_test.cpp` (`FactoryRewrite_*`): every rewrite is VM bit-identical on a fixture panel and idempotent.
- `factory_fingerprint_test.cpp` (`FactoryFingerprint_*`): `rank(close)` and `rank(close*1.0001)` collide; distinct alphas do not.
- `factory_fidelity_test.cpp` (`FactoryFidelity_*`): with `enabled=false` the SearchResult digest is byte-identical to the current one. It is deterministic across worker counts, and the trial count includes rejected rungs.
- `factory_sketch_index_test.cpp` (`FactorySketchIndex_*`): recall@10 ≥ 0.95 against brute force on 10k series.
- Existing suites that must pass: `FactorySearchDriver`, `NsgaSearch`, `FactoryCanonical`, `DeflateSelection`, `CascadeTrialCount`, `StagnationStop`.

**Bench:** `bench/factory_throughput_bench.cpp` (`BM_SearchThroughput`) on 3000×2500. Counters:
- genomes/sec
- dedup-hit %
- fidelity-rejection %
- time split across compile, eval, fitness, pareto and novelty
- a workers sweep over the thread and process executors

**Acceptance:**
- Distinct genomes/sec at least 4× today at equal final-front quality (hypervolume within 5%).
- With the new flags off, results are byte-identical.
- Baseline recorded in the ledger.

---

## Lane 4: Multiple-testing and overfitting control plane

**Goal:** one audited N and one set of statistics for every admission decision. Closes:
- the global trial registry with effective N
- FDR, Romano-Wolf and SPA/Reality-Check tests
- the lockbox open API
- MinTRL, the Harvey-Liu haircut and combination-level weight stability

**New files**
- `eval/trial_registry.hpp` + `src/eval/trial_registry.cpp`: append-only, content-addressed (hash of the config and kind), with a Welford V[SR] and N_eff. N_eff is estimated by the participation ratio (reusing `breadth.hpp`) or by ONC clustering.
- `eval/multiple_testing.hpp` + `src/eval/multiple_testing.cpp`: BH and BY, Holm, Romano-Wolf stepdown, Hansen SPA and White RC, with a seeded stationary block bootstrap (counter-based RNG so it is deterministic).
- `eval/min_trl.hpp`, `eval/haircut.hpp`, `eval/weight_stability.hpp` (jackknife weight dispersion and refit turnover).

**Modified (owned):** `eval/lockbox.hpp` (adds `open_lockbox`), `eval/deflated_sharpe.hpp` (adds an overload that takes a `TrialSummary`).

**API sketch**
```cpp
enum class TrialKind : u8 { MinerExpr, CombinerHyper, StackHyper, RegimeCount, OptimizerHyper };
class TrialRegistry { public:
  static core::Result<TrialRegistry> open(std::filesystem::path);    // durable, append-only
  TrialId record(TrialKind, u64 config_hash, std::span<const f64> oos_pnl, f64 sharpe);
  TrialSummary summary() const;   // n_raw, n_eff, var_sr, registry_hash
};
f64 deflated_sharpe(f64 sr, const TrialSummary&, i64 T, f64 skew, f64 kurt);
std::vector<bool> benjamini_yekutieli(std::span<const f64> p, f64 q);
RomanoWolfResult romano_wolf(const PnlMatrix&, const BootstrapCfg&);
SpaResult hansen_spa(const PnlMatrix& candidates, std::span<const f64> benchmark, const BootstrapCfg&);
core::Result<LockboxReceipt> open_lockbox(SealedPanel&&, const OpenRequest&, AuditSink&); // single-use
```

**Tests** (`atx-engine-eval-tests`, `tests/eval/`)
- `eval_trial_registry_test.cpp` (`EvalTrialRegistry_*`): 50 perfectly correlated trials give N_eff ≈ 1; 50 independent trials give ≈ 50. Survives a crash and reopen.
- `eval_multiple_testing_test.cpp` (`EvalFdr_*`, `EvalRomanoWolf_*`, `EvalSpa_*`):
  - BH matches the R `p.adjust` fixture values.
  - On the null, the Romano-Wolf FWER is ≤ α + MC error.
  - Deterministic under a fixed seed.
- `eval_lockbox_open_test.cpp` (`EvalLockboxOpen_*`): a second open is refused with a typed Err, and the receipt hash is bound.
- `eval_min_trl_test.cpp` (`EvalMinTrl_*`): matches the Bailey-LdP paper example.

**Bench:** `bench/eval_multiple_testing_bench.cpp` covers SPA with 1000 candidates × 2520 days × 1000 bootstraps, and registry append/summary at 10^6 trials.

**Acceptance:** DSR fed by the registry is ≥ the raw-N DSR on correlated trials (less over-deflation). All procedures are deterministic. The lockbox is single-use and audited.

---

## Lane 5: Signal-space zoo combiner

**Goal:** replace PnL-proxy weights with forecast-space combination. Closes: `SignalStore`, EWMA-ICIR and Grinold-Kahn weights, Fama-MacBeth ridge, the Kakushadze "Billion Alphas" regression, orthogonalization and marginal IC, time-varying weights with half-life decay, and better covariance targets (LW2003 constant-correlation, LW2020 nonlinear, RMT clip, HRP/NCO).

**New files** (all under `combine/`)
- `signal_store.hpp`: per-alpha, date × instrument z-scored signals plus forward residual returns. mmap-backed.
- `signal_combiner.hpp` + `src/combine/signal_combiner.cpp`
- `orthogonalize.hpp` + `src/combine/orthogonalize.cpp`
- `cov_targets.hpp`: header-only, generic over Eigen. Includes LW2020 analytic nonlinear shrinkage, including the c>1 null-space variant.
- `hrp.hpp`: HRP and NCO.
- `walk_forward_combiner.hpp`: PIT weight path with refit cadence and hysteresis.
- `decay_fit.hpp`: per-alpha IC-by-horizon exponential half-life, taken from `cross_section_ic`.

**Modified (owned):**
- `combine/combiner.hpp`: new `CombineMethod` enumerators appended at the end, so `CombineMethodEnumLayout` stays valid.
- `combine/gate.hpp`: marginal-IC admission mode behind a flag.

**API sketch**
```cpp
template <class C> concept Combiner = requires(C c, const SignalStore& s, FitWindow w) {
  { c.fit(s, w) } -> std::same_as<core::Result<CombineWeights>>; };
struct IcirEwmaCombiner   { f64 half_life; f64 tstat_haircut; ... };
struct GrinoldKahnCombiner{ CovTarget target; ... };            // w = Ω_IC^-1 E[IC]
struct FamaMacBethRidge   { f64 lambda; bool residualize_on_risk; ... };
struct KakushadzeRegression { u32 n_cluster_dummies; ... };     // O(N·M), no inversion
core::Result<Panel> residualize_signal(const Panel& s, const Exposures& B, std::span<const f64> spec_var,
                                       std::span<const Panel* const> pool);
core::Result<std::vector<Panel>> lowdin_orthogonalize(std::span<const Panel* const>);
f64 marginal_ic(const Panel& cand, std::span<const Panel* const> pool, const Panel& fwd);
Eigen::MatrixXd shrink_nonlinear_lw2020(const Eigen::MatrixXd& X_TxN);
WeightPath walk_forward(const SignalStore&, const WalkForwardCfg&, const Combiner auto&);
```

**Tests** (`atx-engine-combine-tests`, `tests/combine/`)
- `combine_signal_combiner_test.cpp` (`SignalCombiner_*`): recovers the known weights of a synthetic 3-signal DGP to 1e-6. Includes a truncation-invariance (fit/apply firewall) test.
- `combine_orthogonalize_test.cpp` (`CombineOrthogonalize_*`): the residual is orthogonal to B and the pool to 1e-10; Lowdin is order-independent.
- `combine_cov_targets_test.cpp` (`CombineCovTargets_*`):
  - LW2020 matches a Matlab or Python reference fixture (checked in as CSV) to 1e-8.
  - On a spiked model, LW2020's Frobenius loss is lower than LW2004's.
- `combine_hrp_test.cpp` (`CombineHrp_*`): matches the López de Prado paper example.
- `combine_walk_forward_test.cpp` (`CombineWalkForward_*`): PIT (no future rows) and hysteresis.
- Existing suites that must pass: `AlphaCombiner`, `CombineMethodEnumLayout` and `AlphaGate*`.

**Bench:** `bench/combine_bench.cpp`: `fit` for N ∈ {100,500,2000} alphas across every method, plus LW2020 at N=2000.

**Acceptance:**
- On the synthetic zoo (`eval/synthetic_alpha`), OOS IR of GK/FMB is higher than PnL-ShrinkageMv.
- Kakushadze at N=2000 fits in under 50 ms Release.
- Existing combiner digests are unchanged.

---

## Lane 6: Portfolio optimizer (costs, speed and discreteness)

**Goal:** a production-grade solve that costs trades correctly. Closes:
- per-name κ_i L1 cost
- Almgren 3/2-power impact via a rotated SOC epigraph
- a short-leg split with borrow fee and a locate box
- adaptive ρ at fixed iterations, rounded to powers of 2
- warm start, and an optional deterministic early exit
- a post-solve pass for minimum trade size, round lots and max-names
- true multi-period Gârleanu-Pedersen (factor-space Riccati, plus H ≤ 3 stacked MPC)
- a fast CI dense-oracle battery

**New files** (all under `risk/`)
- `cost_terms.hpp` + `src/risk/cost_terms.cpp`: builds augmented rows for κ vectors, the 3/2-power cones and the borrow split.
- `discretize.hpp`: the post-solve pass.
- `gp_riccati.hpp`
- `mpc_stack.hpp`
- `admm_schedule.hpp`: the deterministic ρ schedule.

**Modified (owned):**
- `risk/qp_augment.hpp`, `risk/qp_solver.hpp`, `risk/constraints.hpp`, `risk/optimizer.hpp`
- `risk/garleanu_pedersen.hpp`, `src/risk/garleanu_pedersen.cpp`, `risk/multi_horizon.hpp`, `src/risk/multi_horizon.cpp`
- `cost/cost_aware.hpp`, `cost/borrow.hpp`
- `tests/risk_qp_augment_test.cpp`: split into a fast part and a nightly part.

**API sketch**
```cpp
struct TradeCostTerms { std::span<const f64> kappa_lin; std::span<const f64> c_three_halves;
                        std::span<const f64> borrow_fee; std::span<const f64> locate_cap; };
struct AdmmSchedule { std::array<u32,3> refactor_at{25,75,150}; bool pow2_round=true; };
struct WarmStart { std::span<const f64> x0, y0; };
core::Result<Solution> ConstrainedQpSolver::solve(const Problem&, const AdmmSchedule&, const WarmStart* = nullptr);
struct DiscretizeCfg { f64 min_trade_frac; f64 lot_dollars; u32 max_names; };
core::Result<DiscretizedBook> discretize_and_resolve(const Problem&, const Solution&, const DiscretizeCfg&);
core::Result<GpPolicy> gp_riccati(const FactorModel&, f64 gamma, const ImpactDiag&, std::span<const f64> decay_phi);
```

**Tests** (`atx-engine-risk-tests`, `tests/risk/`)
- `risk_cost_terms_test.cpp` (`RiskCostTerms_*`): analytic checks on 2 names. With all costs zero, the S8.5b pin is byte-identical.
- `risk_admm_schedule_test.cpp` (`RiskAdmmSchedule_*`): deterministic across runs, and the KKT residual after the schedule is ≤ 1e-8.
- `risk_discretize_test.cpp` (`RiskDiscretize_*`)
- `risk_gp_riccati_test.cpp` (`RiskGpRiccati_*`): on the 2-asset closed form, the aim portfolio and trade rate match within 1e-8, and H=1 is byte-identical.
- `risk_constraint_dispatch_multi_horizon_test.cpp` (`RiskConstraintDispatchMH_*`): each descriptor alone changes the book or returns a typed Err.
- The dense-oracle comparison is split. `RiskQpAugmentFast.*` runs M ≤ 50 over about 20 cases in under 30 s in the default run. `RiskQpAugmentNightly.*` is excluded by name filter. No CMake label edit is needed; the filter lives in the `ctest -E` of the nightly script.

**Bench:** `bench/optimizer_production_bench.cpp` runs the full solve (Ruiz, 300 iterations, polish) at M ∈ {1000,3000,5000} and K=64, cold vs warm, with the schedule on and off.

**Acceptance:**
- M=3000, K=64: warm solve under 100 ms Release, recorded in the ledger.
- Fast oracle battery passes in under 30 s.
- All existing `Risk*` pins are green.

---

## Lane 7: Risk model (factors, validation and attribution)

**Goal:** a risk model worth trusting in the book. Closes:
- fundamental style factors plus a market intercept, with `bitset<32>`
- a hybrid fundamental + APCA model
- the model-validation scorecard (bias stats, Q-stat, the MinVar optimizer-bias test)
- realized factor P&L attribution

**New files** (all under `risk/`)
- `fundamental_factors.hpp`: BookToPrice, EarningsYield, Growth, Profitability, Leverage, DivYield, ResidVol, ShortInterest and STReversal, from a `FundamentalPanel` that is PIT and lagged by availability.
- `hybrid_factor_model.hpp` + `src/risk/hybrid_factor_model.cpp`: K_s is chosen by Bai-Ng IC or the Marchenko-Pastur edge.
- `model_validation.hpp` + `src/risk/model_validation.cpp`: produces a JSON scorecard.
- `attribution.hpp`: daily factor, specific, cost and borrow split, plus the realized-vs-predicted TE check.

**Modified (owned):** `risk/exposures.hpp`, `src/risk/factor_model.cpp`, `risk/stat_factor_model.hpp` and `risk/shrinkage.hpp`. The shrinkage change calls Lane 5's `combine/cov_targets.hpp` and needs only its header; if Lane 5's header is not merged yet, add it at integration.

**API sketch**
```cpp
using StyleMask = std::bitset<32>;
core::Result<Exposures> build_exposures(const PricePanel&, const FundamentalPanel*, StyleMask, i64 as_of_row);
core::Result<FactorModel> HybridFactorModelBuilder::build(const ReturnPanel&, const Exposures&, const HybridCfg&);
core::Result<ValidationScorecard> validate_risk_model(const RiskModelFactory&, const ReturnPanel&, const ValidationCfg&);
Attribution attribute(std::span<const f64> w, const Exposures&, std::span<const f64> factor_ret,
                      std::span<const f64> asset_ret, f64 cost, f64 borrow);
```

**Tests** (`atx-engine-risk-tests`)
- `risk_fundamental_factors_test.cpp` (`RiskFundamentalFactors_*`): per-date z-scores are cap-weighted mean 0 and sd 1. PIT: moving the availability date changes the exposure. Golden determinism hash.
- `risk_hybrid_model_test.cpp` (`RiskHybridModel_*`): recovers a planted latent factor.
- `risk_model_validation_test.cpp` (`RiskModelValidation_*`): on a correctly specified simulated model, the bias stat falls within the 95% band.
- `risk_attribution_test.cpp` (`RiskAttribution_*`): the components sum to total P&L within 1e-12.
- Existing suites that must pass: `RiskExposures` (5-factor mask byte-identical), `RiskFactorModel` and `RiskStatFactor`.

**Bench:** `bench/risk_model_build_bench.cpp` measures a full model build at 3000 names × 14 factors × 60 industries × 504 days.

**Acceptance:** the scorecard JSON is produced for 3 candidate configs, and the default 5-factor path stays byte-identical.

---

## Lane 8: End-to-end zoo-to-portfolio pipeline (atx-impl + engine book)

**Goal:** discovered alphas reach the constrained, factor-neutral, cost-aware book, and the replay is honest. Closes:
- the `PreferenceSource` seam, replacing the hard reject of any recipe other than slow momentum
- the real factor model plus beta, sector and style constraints in `equity_allocation`
- `ReplayCostModel` (per-name spread, √-impact and participation cap, with the residual carried forward)
- a per-name `BorrowSchedule`
- cost-aware `research_sim`
- `run_all` on policy replay, with the legacy 1-period report retired behind parity tests
- claims and corporate-action replay, with a delisting-return policy (fixes the PCS failure on 2013-05-01)

**New files**
- `atx-engine/include/atx/engine/book/replay_cost.hpp` + `src/book/replay_cost.cpp`
- `atx-engine/include/atx/engine/book/borrow_schedule.hpp`
- `atx-engine/include/atx/engine/book/event_batch_builder.hpp`: `corporate_actions` + `security_transition` → `ReplayEventBatch`.
- `atx-impl/src/preference_source.{hpp,cpp}`
- `atx-impl/src/research_cost_sim.hpp`

**Modified (owned):**
- `book/replay.hpp`, `src/book/replay.cpp`
- `atx-impl/src/{stage_equity_book, equity_allocation, stage_run, stage_report, stage_optimize, replay_report, research_sim.hpp, stage_discover, stage_sweep}.cpp/hpp`
- `atx-impl/CMakeLists.txt`: the only owner.

**API sketch**
```cpp
class ReplayCostModel { public: virtual ~ReplayCostModel() = default;
  virtual TradeCost cost(InstrumentIdx, i64 period, f64 trade_dollars, const LiquidityRow&) const = 0; };
class FlatBpsCost final : public ReplayCostModel {...};           // == today's trade_bps (bit-identical)
class SqrtImpactCost final : public ReplayCostModel { ImpactCfg cfg; f64 max_participation; };
struct BorrowSchedule { f64 fee(InstrumentIdx, i64) const; f64 locate(InstrumentIdx, i64) const; f64 rebate_bps; f64 cash_bps; };
enum class DelistingPolicy : u8 { Abort, CrspDelistReturn, LastMarkZeroReturn };
// atx-impl
class PreferenceSource { public: static core::Result<PreferenceSource> from_combo(const ComboArtifact&, const PanelIdentity&, i64 pit_cutoff);
  core::Result<std::vector<f64>> preference(i64 decision_row) const; };
```

**Tests**
- Engine book group (`atx-engine-book-tests`):
  - `book/book_replay_cost_test.cpp` (`BookReplayCost_*`): `FlatBpsCost` is bit-identical to the current replay; cost is monotone in participation; the participation cap produces a partial fill with the residual carried forward.
  - `book/book_borrow_schedule_test.cpp` (`BookBorrowSchedule_*`): a short without a locate is rejected.
  - `book/book_event_batch_test.cpp` (`BookEventBatch_*`): a scenario with a delisting after a missing held close does not abort.
- atx-impl (globbed):
  - `atx-impl/tests/preference_source_test.cpp` (`PreferenceSource_*`)
  - `atx-impl/tests/zoo_to_book_e2e_test.cpp` (`ZooToBookE2E_*`): the synthetic fixture runs discover → combine → equity-book → replay. Asserts |βᵀw| ≤ tol, |sector net| ≤ cap, net-of-cost P&L < gross, and truncation invariance.
  - `research_cost_sim_test.cpp`: a high-turnover alpha loses admission.
  - Report parity test (`ReplayReportParity_*`): policy replay equals the legacy report on a one-period fixture.

**Bench:** `atx-engine/bench/replay_cost_bench.cpp` covers replay of 3000 names × 2520 days with the sqrt-impact model.

**Acceptance:**
- The e2e test is green.
- The native annual run gets past 2013-05-01.
- One full OOS run reports net Sharpe, turnover and a capacity point at $10m, $100m and $1bn in the ledger.
- The legacy report path is deleted only after the parity tests pass.

---

## 9. Dependencies and integration order

Build lanes 1–8 in parallel against the Lane 0 SHA; they are disjoint by file. Cross-lane wiring happens in a short serial **integration step** after merge. Each item is under about 50 LOC, and none is merged before its producing lane.
1. **L2 → L3:** in `search_driver.cpp`, pass a `SubtreeCache*` into the evaluator, and report `cache_hit_pct` in `SearchResult`.
2. **L4 → L3, L5, combine/gate:** `SearchDriver`, `gate.hpp` and the learn tuning path record into `TrialRegistry`, and DSR uses `summary()`.
3. **L5 → L7:** `shrinkage.hpp` delegates to `cov_targets.hpp`.
4. **L6 + L7 → L8:** `equity_allocation.cpp` uses the Lane 7 hybrid/fundamental model, the Lane 6 `TradeCostTerms` (κ_i taken from the Lane 8 `ReplayCostModel` coefficients, so fitness, optimizer and replay share one cost calibration) and the GP/MPC mode.
5. **L1 → live path:** `loop/signal_source.hpp` swaps `VmSignalSource` rebuilds for `StreamingEngine`. This is a follow-up owned by whoever integrates L1.

**Deferred to phase 2** because it collides with the kernel and VM owners: the missing Alpha191/BRAIN operators (`registry.cpp`, `oracle.hpp`, `ts_ops.hpp`, `cs_ops.hpp`, `vm.hpp`). Schedule them after L1 and L2 merge, as their own lane.

**Per-lane workflow** (house rules):
- Lease a worktree: `powershell scripts\lease-worktree.ps1 -Branch feat/<lane>-<run> -Base <lane0-sha> …`
- Configure with `-DATX_TEST_GROUPS=<own groups>`.
- Loop: `atx-build.ps1 check <file>` → `build atx-engine-<grp>-tests` → anchored `-Ctest -R <Suite>`.
- Benches are built only under `release-bench`.
- Append Release numbers to `atx-vol/docs/LEDGER.md` at integration only, to avoid concurrent appends to the same file.