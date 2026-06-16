# Alpha Pipeline Reference

**A map of the atx-engine alpha pipeline — Panel → DSL → backtest → fitness → search → robustness → lockbox → public entry points.**

This is a **reference, not a tutorial**: it states *what exists, where (`file:line`), and why*. Every factual claim about behavior is anchored to a `file:line` at the current worktree HEAD (`cfcd27b`). It distills the as-built code; for the *theory* behind each stage it cross-links the research deep-dives rather than restating them. Paths are relative to the repo root.

**Cross-linked deep-dives** (read these for the *why*, not the *where*):
- DSL / expression substrate — [`atx-engine/research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md)
- backtest loop + execution sim — [`atx-engine/research/backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md)
- covariance / risk model — [`atx-engine/research/covariance-matrix-construction-massive-universe-deep-dive.md`](../../research/covariance-matrix-construction-massive-universe-deep-dive.md)
- search / factory lineage — [`atx-engine/research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md), [`atx-engine/research/renaissance-worldquant-deep-dive.md`](../../research/renaissance-worldquant-deep-dive.md)

> Naming caveat read this before trusting any "obvious" symbol name. The plan/prose uses `frictionless_sim` and `build_augmented_panel` as shorthand; **neither is an as-built header symbol**. The simulator is `exec::ExecutionSimulator` ([`exec/execution_sim.hpp:183`](../../include/atx/engine/exec/execution_sim.hpp)); the augmentation is `alpha::with_datafields` ([`alpha/datafields.hpp:153`](../../include/atx/engine/alpha/datafields.hpp)). `frictionless_sim` appears only as a per-test fixture helper, never in `include/`.

---

## 1. Panel representation + the DSL/expression substrate (parse → analyze → Genome)

### 1.1 Panel — the date×instrument data plane

`alpha::Panel` ([`alpha/panel.hpp:77`](../../include/atx/engine/alpha/panel.hpp)) is the immutable, date-major (`t*instruments + i`) field store every alpha consumes. Build via `Panel::create` ([`panel.hpp:88`](../../include/atx/engine/alpha/panel.hpp)) — one f64 column per named field, plus a `{0,1}` universe mask (empty ⇒ all in-universe).

| Accessor | `panel.hpp:line` | Returns |
|---|---|---|
| `field_id(name)` | [132](../../include/atx/engine/alpha/panel.hpp) | `Result<FieldId>` resolve by name |
| `field_all(FieldId)` | [161](../../include/atx/engine/alpha/panel.hpp) | whole date-major column span |
| `field_name(FieldId)` | [145](../../include/atx/engine/alpha/panel.hpp) | the field's name |
| `num_fields()` | [120](../../include/atx/engine/alpha/panel.hpp) | field count |
| `in_universe(date, inst)` | [168](../../include/atx/engine/alpha/panel.hpp) | membership at a cell |

The evaluator output is `alpha::SignalSet` ([`panel.hpp:269`](../../include/atx/engine/alpha/panel.hpp)) — the per-alpha date-major value vectors that the digest and the fitness path consume.

### 1.2 The DSL substrate: lex → parse → analyze → compile → evaluate

The expression language is a 5-stage pipeline. (Theory: [`alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md).)

1. **Lex** — `alpha::lex` ([`alpha/lexer.hpp:151`](../../include/atx/engine/alpha/lexer.hpp)): `Result<vector<Token>>` from source text.
2. **Parse** — `alpha::parse_program` ([`alpha/parser.hpp:473`](../../include/atx/engine/alpha/parser.hpp)) (and `parse_expr`, [`parser.hpp:456`](../../include/atx/engine/alpha/parser.hpp)) → an `Ast` ([`parser.hpp:130`](../../include/atx/engine/alpha/parser.hpp)), an arena of `Expr` nodes ([`parser.hpp:72`](../../include/atx/engine/alpha/parser.hpp)) in topological (post-)order. Resolves op names against the op `Library`.
3. **Analyze / typecheck** — `alpha::analyze` ([`alpha/typecheck.hpp:263`](../../include/atx/engine/alpha/typecheck.hpp)) → an `Analysis` ([`typecheck.hpp:233`](../../include/atx/engine/alpha/typecheck.hpp)) carrying per-node `TypeInfo` ([`typecheck.hpp:54`](../../include/atx/engine/alpha/typecheck.hpp)) (shape/dtype/required-lookback). A single forward pass — the arena's topological order makes recursion unnecessary.
4. **Compile** — the analyzed AST lowers to a `Program` of `Instr` ([`alpha/bytecode.hpp:81`](../../include/atx/engine/alpha/bytecode.hpp) / [`bytecode.hpp:68`](../../include/atx/engine/alpha/bytecode.hpp)). Common-subexpression elimination uses the `Dag` ([`alpha/dag.hpp:133`](../../include/atx/engine/alpha/dag.hpp)).
5. **Evaluate** — `alpha::Engine::evaluate` ([`alpha/vm.hpp:223`](../../include/atx/engine/alpha/vm.hpp), class `Engine` at [`vm.hpp:206`](../../include/atx/engine/alpha/vm.hpp)) runs a `Program` over a `Panel` → `SignalSet`.

The **op registry** is `alpha::Library` ([`alpha/registry.hpp:369`](../../include/atx/engine/alpha/registry.hpp)); each op is an `OpSig` row ([`registry.hpp:276`](../../include/atx/engine/alpha/registry.hpp)) keyed by an `OpCode` ([`registry.hpp:74`](../../include/atx/engine/alpha/registry.hpp)). `Library::find` ([`registry.hpp:377`](../../include/atx/engine/alpha/registry.hpp)) resolves a name; the ctor ([`registry.hpp:373`](../../include/atx/engine/alpha/registry.hpp)) registers the built-ins. This `Library` (the *DSL grammar*) is borrowed run-wide and **must outlive every produced genome** (its rows are aliased by `Expr::op`).

### 1.3 Genome — the mined candidate

A search candidate is a **Genome** (`factory::Genome`, [`factory/genome.hpp`](../../include/atx/engine/factory/genome.hpp)) = `Ast` + `Analysis` (the parsed program plus its type/lookback analysis) — *not* an `ISignalSource`. The Factory header records this explicitly ([`factory/factory.hpp:53`](../../include/atx/engine/factory/factory.hpp): "A mined candidate is a Genome (Ast + Analysis), not an ISignalSource"). The genome is canon-hashable (its `canon_hash` drives F6 dedup, below).

---

## 2. Backtest / sim kernel (signal → weights → fills → PnL, the cost model)

### 2.1 Signal → weights

`WeightPolicy::to_target_weights` ([`loop/weight_policy.hpp:197`](../../include/atx/engine/loop/weight_policy.hpp)) maps a raw cross-sectional signal to target portfolio weights: winsorize → transform (rank/zscore) → optional group-demean → dollar-neutralize → gross L1-normalize. This is the deterministic "alpha → book weights" map the fitness path and the backtest loop share.

### 2.2 Weights → fills → PnL

- **Execution simulator** — `exec::ExecutionSimulator` ([`exec/execution_sim.hpp:183`](../../include/atx/engine/exec/execution_sim.hpp)) turns target orders into fills with modeled market impact; `settle_pending` ([`execution_sim.hpp:250`](../../include/atx/engine/exec/execution_sim.hpp)) materializes the `FillPayload`s for a timestamp. This is the engine's real sim kernel (the `sim_` borrow the Factory / drivers carry).
- **Backtest loop** — `loop::BacktestLoop<Cap>` ([`loop/backtest_loop.hpp:168`](../../include/atx/engine/loop/backtest_loop.hpp)); `run()` ([`backtest_loop.hpp:216`](../../include/atx/engine/loop/backtest_loop.hpp)) drives the event loop and returns a `BacktestResult` whose equity curve is the PnL. (Theory: [`backtest-loop-execution-sim-deep-dive.md`](../../research/backtest-loop-execution-sim-deep-dive.md).)

### 2.3 The cost model

The calibrated impact/slippage coefficients are `cost::CalibratedCost` ([`cost/calibration.hpp:110`](../../include/atx/engine/cost/calibration.hpp)) — a `√`-impact temporary channel `(Y, δ)` + permanent channel `(γ)` wrapping `exec::ImpactCfg`/`exec::SlippageCfg`, with fit provenance. The representative modeled round-trip cost is `cost::round_trip_cost_bps` ([`cost/cost_aware.hpp:114`](../../include/atx/engine/cost/cost_aware.hpp)) — it applies the *same* impact formula the simulator uses (no second model; see [`cost_aware.hpp:26`](../../include/atx/engine/cost/cost_aware.hpp)) and is what the fitness `−cost` objective is priced from (§3).

---

## 3. Fitness + the objective vector slots

`factory::FitnessReport` ([`factory/fitness.hpp:182`](../../include/atx/engine/factory/fitness.hpp)) is one candidate's scored result; `factory::pool_aware_fitness` ([`fitness.hpp:291`](../../include/atx/engine/factory/fitness.hpp)) computes it against the current pool. The search ranks by the **objective vector** `objectives` ([`fitness.hpp:191`](../../include/atx/engine/factory/fitness.hpp), `std::array<f64, kMaxObjectives>`, `kMaxObjectives = 5` at [`fitness.hpp:85`](../../include/atx/engine/factory/fitness.hpp)). The fixed-slot scheme is documented at [`fitness.hpp:161`–171](../../include/atx/engine/factory/fitness.hpp):

| Slot | Meaning | Source field / line |
|---|---|---|
| 0 | **wq** — OOS WorldQuant fitness, mean over CPCV TEST folds | `wq` ([`fitness.hpp:183`](../../include/atx/engine/factory/fitness.hpp)); doc [146](../../include/atx/engine/factory/fitness.hpp) |
| 1 | **diversify** — `clamp(1 − redundancy, 0, 1)` (F7 marginal-contribution weight) | `diversify` ([`fitness.hpp:185`](../../include/atx/engine/factory/fitness.hpp)); doc [148](../../include/atx/engine/factory/fitness.hpp) |
| 2 | **robust** — `clamp(wq_on(weak_universe)/max(wq,eps), 0, 1)` | `robust` ([`fitness.hpp:186`](../../include/atx/engine/factory/fitness.hpp)); doc [149](../../include/atx/engine/factory/fitness.hpp) |
| 3 | **behavioral-novelty** — mean k-nearest behavioral distance over population ∪ archive | computed by the SearchDriver novelty pass (§4) from `descriptor` ([`fitness.hpp:193`](../../include/atx/engine/factory/fitness.hpp)) |
| 4 | **−cost** — `−cost_bps` (NEGATED so pareto's pure-max dominance prefers cheaper) | `cost_bps` ([`fitness.hpp:190`](../../include/atx/engine/factory/fitness.hpp)); doc [165–166](../../include/atx/engine/factory/fitness.hpp) |

`n_objectives` ([`fitness.hpp:192`](../../include/atx/engine/factory/fitness.hpp)) is how many leading slots are live: 3 in the base config; slot 3 (novelty) and slot 4 (cost) activate without a layout change. Slot 4 is set only when `FitnessCfg.target_aum > 0` ([`fitness.hpp:204`](../../include/atx/engine/factory/fitness.hpp)) — at `target_aum == 0` the cost objective is a pure no-op (`cost_bps == 0`, no 5th objective), keeping every existing digest byte-identical. The `descriptor` ([`fitness.hpp:193`](../../include/atx/engine/factory/fitness.hpp)) is the candidate's realized OOS PnL profile — the phenotype the novelty objective is computed from, canon-cacheable.

---

## 4. Search / mining (`SearchDriver` loop, NSGA-II, behavioral archive, dedup, `SearchResult.digest`)

`factory::SearchDriver` drives the per-generation mine loop ([`factory/search_driver.hpp`](../../include/atx/engine/factory/search_driver.hpp)). One generation ([`search_driver.hpp:14`–17](../../include/atx/engine/factory/search_driver.hpp)):

1. **Dedup (F6)** — keep only genomes whose `canon_hash` is not already in the canon set ([`search_driver.hpp:14`](../../include/atx/engine/factory/search_driver.hpp)); `dedup_pct` is recorded in the result ([`search_driver.hpp:166`](../../include/atx/engine/factory/search_driver.hpp)).
2. **Score** — `pool_aware_fitness` per fresh candidate (§3); compile failures are dropped from the digest.
3. **Rank — NSGA-II** — `assign_pareto_ranks` over the `objectives` vector via the primitives in `factory/pareto.hpp` (`ObjMatrix`, NSGA-II non-dominated fronts; included at [`search_driver.hpp:94`](../../include/atx/engine/factory/search_driver.hpp)). The candidate carries its front `rank` ([`search_driver.hpp:236`](../../include/atx/engine/factory/search_driver.hpp)).
4. **Behavioral novelty** — `factory::BehavioralArchive` + `behavioral_distance` ([`factory/behavior.hpp`](../../include/atx/engine/factory/behavior.hpp), included at [`search_driver.hpp:86`](../../include/atx/engine/factory/search_driver.hpp)); the FIFO archive cap is `SearchConfig.behavior_archive_cap` (default 64, [`search_driver.hpp:136`](../../include/atx/engine/factory/search_driver.hpp)), the k-nearest count `behavior_k`. The per-generation novelty pass fills objective slot 3.

The result is `factory::SearchResult` ([`search_driver.hpp:162`](../../include/atx/engine/factory/search_driver.hpp)); its `digest` ([`search_driver.hpp:163`](../../include/atx/engine/factory/search_driver.hpp)) is the **F1/F2 byte-identical run fingerprint** — the per-generation `signal_set_digest` (§7) folded with the generation index, worker-count-invariant ([`search_driver.hpp:54`–56](../../include/atx/engine/factory/search_driver.hpp)).

`factory::Factory` ([`factory/factory.hpp:163`](../../include/atx/engine/factory/factory.hpp)) wraps the search into the **mine → gate → admit** capstone (§8).

---

## 5. Robustness battery (CPCV, regime / walk-forward `RobustnessVerdict`, lockbox)

- **CPCV** — combinatorial purged cross-validation folds: `eval::cpcv_folds` ([`eval/cpcv.hpp:175`](../../include/atx/engine/eval/cpcv.hpp)). The TEST folds are the OOS partitions the `wq` objective averages over.
- **Regime + walk-forward** — `eval::RobustnessVerdict` ([`eval/regime_slice.hpp:138`](../../include/atx/engine/eval/regime_slice.hpp)) carries `regime_sharpe`, `walk_forward_sharpe`, `full_sample_sharpe`, `worst_regime_sharpe`, `worst_window_sharpe`, `recovery_corr`, and `is_robust` ([`regime_slice.hpp:139`–146](../../include/atx/engine/eval/regime_slice.hpp)). The `ResearchDriver` robustness gate screens each admitted survivor with one verdict over its stored OOS PnL ([`research_driver.hpp:211`+](../../include/atx/engine/factory/research_driver.hpp)).
- **Lockbox** — `eval::SealedPanel` ([`eval/lockbox.hpp:168`](../../include/atx/engine/eval/lockbox.hpp)): the full panel is sealed; mining only ever sees `visible()`, and a read into the sealed terminal region traps (`field_cross_section_or_trap`, [`lockbox.hpp:202`](../../include/atx/engine/eval/lockbox.hpp)). `RobustResearchDriver::run` reserves it via `eval::reserve_lockbox` ([`robust_pipeline.hpp:180`](../../include/atx/engine/factory/robust_pipeline.hpp)).

---

## 6. Deflation / overfitting guards (deflated Sharpe at running trial count, PBO)

- **Deflated Sharpe (F4)** — `eval::deflated_sharpe` ([`eval/deflated_sharpe.hpp:136`](../../include/atx/engine/eval/deflated_sharpe.hpp)). The selection-adjustment parameter is `N`, the **running trial count** ([`deflated_sharpe.hpp:137`](../../include/atx/engine/eval/deflated_sharpe.hpp)): every distinct candidate the search scores increments `FitnessCfg.trial_count` ([`fitness.hpp:199`](../../include/atx/engine/factory/fitness.hpp)), and a higher `N` lowers `dsr` — the admission statistic `FitnessReport.dsr` ([`fitness.hpp:188`](../../include/atx/engine/factory/fitness.hpp), doc [152–153](../../include/atx/engine/factory/fitness.hpp)). The Factory deflation bar is `cand.dsr >= cfg.min_dsr`, applied *before* library admit ([`factory.hpp:197`–199](../../include/atx/engine/factory/factory.hpp)).
- **PBO** — probability of backtest overfitting (CSCV): `eval::pbo_cscv` ([`eval/pbo.hpp:298`](../../include/atx/engine/eval/pbo.hpp)).

---

## 7. Determinism (`signal_set_digest`, `seed_for`, golden pins)

- **`signal_set_digest`** — `parallel::signal_set_digest` ([`parallel/digest.hpp:44`](../../include/atx/engine/parallel/digest.hpp)): a canonical-order wyhash over a `SignalSet`, equal **iff** the two sets are bit-identical in shape, root order, names, and every value's raw f64 bits ([`digest.hpp:41`–42](../../include/atx/engine/parallel/digest.hpp)). The same primitive that pins the search run also pins the real-data Panel (see the data-ingestion reference §5).
- **`seed_for`** — `factory::detail::seed_for` ([`search_driver.hpp:182`](../../include/atx/engine/factory/search_driver.hpp)): the F1 per-`(master, gen, idx)` seed, a fixed SplitMix mix that depends on **nothing else** — never worker/thread/time/address ([`search_driver.hpp:178`–181](../../include/atx/engine/factory/search_driver.hpp)). This is why the run digest is worker-count-invariant.
- **Golden pins** — two idioms. (a) A *golden-constant* pin: a literal digest asserted by a test (e.g. the real-data Panel's `0x2a22a873483d9157`, data-ingestion reference §5). (b) An *equivalence* (boundary) pin: `RobustResearchDriver::run` with the robustness gate OFF must replay byte-identically to a plain `ResearchDriver::run` on the same visible panel ([`robust_pipeline.hpp:189`](../../include/atx/engine/factory/robust_pipeline.hpp)) — no golden constant, just F1 equivalence.

---

## 8. The public entry points (what a user calls)

Three entry points, increasing in scope. Each **borrows** its inputs for the driver's lifetime (library grown in place; the DSL `Library` and `Panel` must outlive the driver and every produced genome).

| Entry point | Declaration | Scope |
|---|---|---|
| `Factory::mine` | [`factory/factory.hpp:187`](../../include/atx/engine/factory/factory.hpp) | mine → P4 gate → S1 deflation → grow an ephemeral `combine::AlphaStore` |
| `ResearchDriver::run` | [`factory/research_driver.hpp:208`](../../include/atx/engine/factory/research_driver.hpp) | budget-bounded mine → admit → repeat into the persistent `library::Library`, with the robustness gate |
| `RobustResearchDriver::run` | [`factory/robust_pipeline.hpp:177`](../../include/atx/engine/factory/robust_pipeline.hpp) | seal lockbox → inner research run on `visible()` → combine → book |

### Minimal call sequence

```cpp
// 1. Op grammar + research Panel + sim + weight policy + admission gate (all borrowed).
alpha::Library dsl;                       // alpha/registry.hpp:369  (built-in ops registered)
alpha::Panel    panel = /* real-data Panel — see data-ingestion-reference.md §5 */;
exec::ExecutionSimulator sim = /* ... */; // exec/execution_sim.hpp:183
WeightPolicy policy = /* ... */;          // loop/weight_policy.hpp
combine::AlphaGate gate = /* ... */;      // the P4 admission screen

// 2a. One mine pass into an ephemeral pool:
combine::AlphaStore pool;
factory::Factory factory(/*lib*/, panel, sim, policy);   // factory.hpp:178 (ctor)
FactoryReport rep = factory.mine(cfg, pool, gate);       // factory.hpp:187 — rep.digest is F1/F2

// 2b. OR the full budget-bounded research loop into a persistent library:
library::Library lib;
factory::ResearchDriver driver(lib, dsl, panel, sim, policy, gate); // research_driver.hpp:199
ResearchReport rrep = driver.run(cfg);                              // research_driver.hpp:208

// 2c. OR the robust pipeline (lockbox-sealed mine → combine → book):
factory::RobustResearchDriver rdriver(lib, dsl, /*full_panel*/panel, sim, policy, gate); // robust_pipeline.hpp:168
auto robust = rdriver.run(cfg);   // robust_pipeline.hpp:177 — Result<RobustReport>
```

Determinism contract for all three: **same cfg + same starting library/pool contents ⇒ byte-identical report digest** (F1/F2), folded from `signal_set_digest` + `seed_for` (§7).
