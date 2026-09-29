# atx-engine inventory and gap review v8 (task P1c)

Reviewer: Fable 5.1 (P1c, read-only), 2026-09-29. Tree `C:/atx` main @ `7fbfc379`. Nothing edited, built or run.
- **Read:** atx-engine headers and sources of all 22 modules (header contracts, registries, entry points), bench/ and
  its one baseline JSON (synthetic timings), atx-engine/tools/*.py outlines, atx-impl/src/strategy_*.{cpp,hpp},
  the stage_equity_mine / stage_discover / stage_combine headers, trial_ledger.hpp, atx-impl/tools/*.py outlines,
  scripts/research_cycle.py, scripts/specs/v71.json (pins and flags only), platform-20260928/{progress, plan-v7,
  code-review-v7, task-W2 / U2 / L9 reports}, docs/plans/2026-09-24-alpha-engine-review-findings.md, engine docs.
- **Hygiene:** no statistic or data file dated 2023 or later, and nothing named validation / VAL / holdout, was opened.
  Two engine docs (docs/LEDGER.md, docs/QUANT_PLATFORM_SWARM_STATUS.md) contain prose results of the 2013-2019 panel
  pipeline; only candidate and admission counts from them appear here. Timings and memory are TRAIN 2020-2022 figures
  already recorded in code-review-v7.md and progress.md.
- **Not verified:** anything that needs a build or a run. Every throughput estimate is arithmetic on recorded numbers
  and is marked (est.).
- Effort: S < 1 day, M 1-3 days, L > 3 days. LOC = `wc -l` of include + src.

**Headline**
1. The mega path uses a small part of the engine. Its include closure is `alpha/{bytecode,vm}`,
   `factory/ic_research`, `data/strategy_data`, `parallel/det_pool`, `book/replay_cost`, `cost/borrow_tiers`,
   `eval/hac`. Nothing from combine, risk, learn, library, validation, regime, fund, store, loop or exec is called in
   production code.
2. Automated alpha search exists and is tested (GP, grammar, racing, dedup, registry: 17 k LOC, 377 tests). It runs
   only on the old yearly-context panels. It cannot run on a research role + fields store today.
3. The fields store holds 63 fields and the runner refuses more than 64. The cap is an impl bitmask, not an engine
   limit.
4. Every candidate is compiled and evaluated alone. No sub-expression is shared across the 48 alphas, and the engine's
   subtree cache and union-DAG evaluator cannot be used with the membership mask the mega path needs.
5. One whole-panel VM arena (624 MB at 7 slots) fits under the 1,536 MiB rule, a second does not, so candidates cannot
   run in parallel. The engine IC kernel and the runner both cap workers at 4 of 16 logical cores.

## 1. Capability matrix

| module | provides | LOC | tests | used by mega path (call site) | v8 opportunity |
|---|---|---|---|---|---|
| alpha | DSL front end, batch VM, StreamingEngine, oracle, fusion, SubtreeCache, cluster groups, Alpha101 augment | 18,565 | 724 | partly: compile `strategy_ic_runner.cpp:588-589`; Engine ResearchFast + pools + mask `:2040-2048`. Unused: StreamingEngine, SubtreeCache, fusion, compile_batch, cluster_panel, augment | O3, O6, O9 |
| factory | GP search, grammar sampler, mutation / crossover, param search, fitness, racing, dedup, IC screen, residual IC objective | 17,078 | 377 | partly: research IC only `strategy_ic_runner.cpp:2113,2197,2260` | O8 (mining) |
| eval | CPCV (row and date), CSCV PBO, DSR / PSR, MinTRL, Harvey-Liu haircut, BH / BY / Holm, Romano-Wolf, SPA / RC, TrialRegistry + ONC clusters, lockbox, robustness battery, HAC | 12,205 | 284 | partly: HAC only `strategy_nav_replay.cpp:28,40` | O8 (registry), O7 |
| combine | AlphaStore, gate, 5 PnL-space fits, 4 signal-space combiners, HRP / NCO, walk-forward weights, orthogonalize / marginal IC, decay fit, SignalCube, crowding, conviction | 6,097 | 207 | no (`strategy_ic_composition.cpp` is its own rank blend) | O4 |
| risk | FactorModel, exposures, EWMA + NW covariance, specific risk, eigen adjust, VRA, hybrid APCA, bias scorecard, attribution, QP (fast + ADMM), cones, GP aim, Riccati, MPC, discretize, capacity | 19,393 | 499 | no; one test compares with `cov_ewma` (`strategy_risk_model_test.cpp:30`) | O10 |
| cost | cost surface, calibration, spread estimators, FIM adjustment, borrow tiers / accrual, capacity, optimizer cost terms | 2,958 | in core | partly: `borrow_tiers` `strategy_nav_replay.cpp:27,65`, `strategy_spo.cpp:1137` | O10 |
| book | scheduled replay, replay cost models, borrow schedule, security transitions, claims, decay monitor, report | 6,267 | 128 | partly: `ReplayCostModel` / `SqrtImpactCost` `strategy_nav_replay.cpp:376-389`, `strategy_cost_v2.hpp:57` | decay monitor (low) |
| data | role reader, PIT dataset / align, catalog, PIT universe, corporate actions, ORATS loader, FINRA SI, fundamental fields, exposure panel | 13,237 | 294 | partly: `read_strategy_role` `strategy_ic_runner.cpp:2099` | O1 |
| parallel | DetPool, batch eval (A), union DAG (B), thread / process executors, shm worker, eval / mine / backtest / CPCV workloads | 6,968 | 136 | partly: DetPool, 4 workers `strategy_ic_runner.cpp:2122` | O2, O3 |
| learn | elastic net, GBT, TCN / GRU, autoencoder, HMM, stacking | 12,376 | 211 | no | none (plan-v7: no fitted weights on 3 y) |
| library | persistent alpha store, dedup index, correlation index, lifecycle, manifest | 3,291 | 68 | no (JSON libraries + Python generators) | none now |
| store | SQLite catalog, run recorder, promotion, progress | 1,810 | 40 | no | none |
| loop, exec, bus, clock, event, portfolio | event-driven backtest loop, execution simulator, weight policy, accounting | 4,055 | in core (386) | no | none |
| fund | sleeves, meta allocator, cross-sleeve risk, netting | 2,081 | 60 | no | none |
| regime | macro series loader, `regime_*` panel columns | 661 | 20 | no | conditioning fields (low; needs a seal) |
| validation | look-ahead, survivorship and overfit-gate assertions | 173 | 1 file | no | reuse in O6 tests |
| quant | calendar, OSI, Black-Scholes | 384 | 16 | no | none |

Engine total: 127,726 LOC (include 73,813, src 53,913), tests 132,266, bench 7,758. atx-impl: src 46,647 (strategy_*
18,633; stage_* 17,985; other 10,029), tests 51,424.

## 2. Answers by question

### Q1. Alpha DSL and VM

| topic | finding | where |
|---|---|---|
| operators | 91 named functions = 74 builtin rows (5 are aliases) + 17 literature rows; 105 opcodes, ids frozen by static_assert; plus infix arithmetic, comparisons, logic, ternary | `registry.cpp:20,179`; `registry.hpp:227-230` |
| categories | element-wise 9; cross-sectional 17; rolling time-series 38; OU rolling 4; stateful recurrences 4; record ops 2; literature 17 (regressions 3, min-periods 8, top-k, count, 2 group builders, 2 packs) | `registry.cpp` |
| types | Shape {Scalar, CrossSection, Panel} x DType {F64, Mask, Group}; records with named pins. A field is Group iff its name starts `grp_` or `IndClass.` | `registry.hpp:52-66`; `typecheck.hpp:141-151` |
| NaN policy | IEEE arithmetic; min / max NaN if either side is NaN; rolling ops need a full window with no NaN, except delay / delta, ts_backfill, ts_count_nans and the `*_mp` family; running sums treat inf as missing; out-of-universe cells load as NaN | `vm.hpp:49-63`; `ts_ops.hpp:22-47,89-96` |
| lookback | shift ops d + child, rolling ops (d-1) + child, u16 (max 65,535); program value = max over roots. Stateful recurrences add 0 but depend on the panel's first date | `typecheck.hpp:15-21`; `typecheck.cpp:597-603` |
| slots | refcount-driven free list; `num_slots` = peak live values; each slot is one whole-panel f64 buffer | `bytecode.hpp:16-30,93` |
| batch vs streaming | StreamingEngine is bit-exact to the batch VM in the same EvalMode after warm + step. State is carried per instruction (rings, deques, Welford, sliding lanes, recurrence state). It refuses non-Const scalar operands. It has a universe row but no separate eligibility row and no KernelPolicy | `streaming_engine.hpp:26-40,72-76` |
| sharing across candidates | none in the mega path. Each candidate is its own single-root Program (`:589-590`) run by `vm->evaluate(program)` (`:2048`). `(close/delay(close,1))-1` is recomputed by every candidate that reads it, on every cache miss | `strategy_ic_runner.cpp` |
| why the engine cache is off | `Engine::evaluate_nodes` drops the cache when a cross-section mask is set: the key has no mask field | `vm.hpp:633-634`; `subtree_cache.hpp:70` |
| SIMD | element-wise kernels are plain loops left to the auto-vectorizer; xsimd in some Ts and IC kernels. The build is SSE2: `vm_fp_flavor` is empty (no `_avx2`, `_fma`) | `vm.hpp:39-46`; `strategy_ic_runner.cpp:122-132` |
| threading | inside one op only: Ts ops split by instrument columns, Cs ops by date bands, on one DetPool; bit-identical for any worker count | `vm.hpp:443-459,690-697` |
| determinism | AuditExact (default, oracle bit-exact) and ResearchFast (online variance, sliding decay / regression, co-moments, Neumaier sums; tolerance). The mega path runs ResearchFast and never re-scores in AuditExact, although the VM contract says discoveries are re-scored before publication | `vm.hpp:311-327`; `strategy_ic_runner.cpp:45,2041` |

Where the IC-runner limits come from:

| limit | value | enforced in | layer |
|---|---|---|---|
| fields per store and per library | 64 | `strategy_ic_runner.cpp:511,737` (u64 residency bitmask) | impl |
| extra fields per candidate | 5 | `check_fund_ic_v6.py:68`; the runner charges `8 * capacity` B/cell (`:638`) | Python rule over an impl memory charge |
| peak slots per candidate | 7 (house), 64 (hard) | `generate_fund_ic_v71.py:63`; `strategy_ic_runner.cpp:590` | Python / impl |
| lookback | 314 (house), `score_begin - 63` = 336 (runner), 65,535 (engine) | `generate_fund_ic_v4.py:53`; `strategy_ic_runner.cpp:617`; `typecheck.cpp:155` | Python / impl / engine |
| warm-up | `score_begin >= 383` | `strategy_ic_runner.cpp:616`; `prepare_recent_research.py:457` | impl + tool |
| panel shape | dates <= 4,096, instruments <= 20,000 | `strategy_ic_runner.cpp:616` | impl |
| library | 256 candidates, 32 families, DSL <= 4,096 B | `strategy_ic_runner.cpp:573,583` | impl |
| workers | 1..4 | `ic_screen.cpp:287`; `strategy_ic_runner.cpp:2348` | engine + impl |

The engine itself imposes only the u16 window and the IC worker cap. 5 / 7 / 314 are one memory limit expressed three
ways: `cells * (72 + 8 * slots) + cells * (8 * capacity + 1)` must fit in 1,536 MiB (`strategy_ic_runner.cpp:631-638`).

### Q2. Automated search: see section 4.

### Q3. Combination

| method | file | engine tests | used by mega path |
|---|---|---|---|
| equal weight, rank average | `combine/combiner.hpp:97-99` | combine_combiner_test | no; ew-theme-v1 is in `fit_composition_weights.py` + `strategy_ic_composition.cpp` |
| IC weighted (window Sharpe proxy) | `combiner.hpp:100` | combine_combiner_test | no |
| Ledoit-Wolf shrunk mean-variance | `combiner.hpp:101` | combine_combiner_test | no |
| ridge in PC space, bounded | `combiner.hpp:102` | combine_combiner_test | no |
| IC-EWMA with t haircut; Grinold-Kahn; Fama-MacBeth ridge; Kakushadze regression | `signal_combiner.hpp` | combine_signal_combiner_test, combine_w0e0a_hac_tstat_test | no |
| shrinkage targets: LW 2004, LW 2003, LW 2020 nonlinear, RMT clip | `cov_targets.hpp` | combine_cov_targets_test (numpy fixtures, 1e-8) | no |
| hierarchical: HRP, NCO | `hrp.hpp` | combine_hrp_test | no |
| online: walk-forward refit with embargo and hysteresis | `walk_forward_combiner.hpp` | combine_walk_forward_test | no |
| regime-conditional; stacking (GBT meta-model) | `regime_combiner.hpp`; `learn/ensemble.hpp` | combine_regime_combiner_test, regime_stack_wire_test, learn tests | no (old `stage_combine.cpp` only) |
| Bayesian model averaging | absent | - | - |

Plan-v7 D8 keeps ew-theme-v1, so the combiners are not a v8 wiring target. The useful unused parts are
`orthogonalize.hpp` (`residualize_signal`, `marginal_ic`) and `decay_fit.hpp`: signal-space and tested.

### Q4. Risk, portfolio, cost: duplication

| function | engine | impl / Python | state |
|---|---|---|---|
| factor risk model | `risk/factor_model.cpp`, `fundamental_factors.hpp`, `hybrid_factor_model.cpp`, `cov_ewma.cpp`, `specific_risk.cpp` | `strategy_risk_model.cpp` (1,238 lines, atx-risk-v1.1) restates the EWMA + NW estimator recursively | parallel implementation; equality with `cov_ewma` is tested |
| bias scorecard | `risk/model_validation.cpp` | `strategy_risk_verb.cpp` (1,046) | parallel |
| optimiser | `risk/optimizer.hpp`, `qp_solver.cpp` (ADMM), `qp_factor_admm.cpp`, `mpc_stack.hpp`, `gp_riccati.hpp` | `strategy_spo.cpp` (1,496, FISTA with exact prox) | parallel; different algorithm and cost terms |
| price exposures, neutralisation | `data/price_exposure_provider.cpp`, `exposure_panel.cpp` | `strategy_price_exposures.cpp` and Python `PricePanel` / `neutralization_basis` in `fit_composition_weights.py` | three implementations (v7 finding C1, still open) |
| impact laws | `cost/fim_adjustment.hpp`, `book/replay_cost.hpp` (sqrt impact) | `strategy_cost_v2.hpp` (KO, FIM) derives from the engine `ReplayCostModel` | extension, plus a second FIM form |
| centered ranks | - | 3 copies (v7 C1) | open |

`garleanu_pedersen.hpp`, multi-horizon MPC, `discretize.hpp` and `risk/capacity.hpp` have no mega-path caller. The
engine review register lists defects in the engine optimiser and factor model (R-01..R-19; their current status was
not checked here), so moving the impl versions into the engine is safer than switching the book to the engine ones.

### Q5. Validation and evaluation

| method | C++ (engine) | Python | mega path uses |
|---|---|---|---|
| CPCV, purged and embargoed | `eval/cpcv.hpp`, `cpcv_date.hpp` (session units) | - | neither |
| CSCV PBO | `eval/pbo.cpp` | `backtest_integrity.py cscv_pbo` | Python |
| DSR, PSR, MinTRL | `eval/deflated_sharpe.hpp`, `min_trl.hpp` | `backtest_integrity.py` | Python |
| effective N (ONC clusters) | `eval/trial_clusters.hpp`, `TrialRegistry::accounting` | `backtest_integrity.py onc, effective_trials` | Python |
| FDR / FWER, Romano-Wolf, SPA, Reality Check (block bootstrap) | `eval/multiple_testing.cpp` | - | neither |
| trial ledger | `eval/trial_registry.cpp` (binary, content-addressed); `atx-impl/src/trial_ledger.cpp` (hash-chained JSONL, old stages) | `backtest_integrity.py ledger_*` (JSONL, construction cells) | Python |
| IC + HAC | `factory/ic_screen.cpp`; `eval/cross_section_ic.cpp` (old stage) | `alpha_report_card.py` | C++ `ic_screen` |
| walk-forward | weights path (`walk_forward_combiner`), survival slices (`regime_slice`), rotating OOS windows (`FactoryConfig::oos_n_windows`), risk scorecard walk | - | none |

The Python integrity tools live under `.superpowers/sdd/mega-alpha-20260926/studies/`, a sprint-state directory, not
the code tree. No driver evaluates the book over 2012-2022 in folds. The frozen book has no fitted weights (prior
signs, ew-theme), so a fold driver is a data build plus a spec loop, not new statistics (O7).

### Q6. Regime, decay monitor, library

| unit | what exists | used |
|---|---|---|
| regime | CSV loader for 11 macro series (VIX, curve, credit OAS, NFCI), broadcast as `regime_*` columns | old `stage_regime.cpp` only |
| `book/decay_monitor.hpp` | Page-Hinkley down test gated by MinTRL, PSR drop check, cost-flood discriminator, lifecycle controller | no. `atx-impl/tools/book_monitor.py` has its own CUSUM (M2) |
| library | persistent store, SimHash correlation index (V2 two-sided), lifecycle journal | old stages only |

### Q7. Parallel execution

| item | finding |
|---|---|
| executors | DetPool (threads, barrier, lowest-index error); ThreadExecutor and ProcessExecutor behind `IExecutor`; `atx-shm-worker` registers Test, Eval, Mine, Backtests, Cpcv (`shm_worker_main.cpp:30-34`) |
| candidate-level parallelism | `parallel_evaluate` (one Program per worker engine) and `global_dag_evaluate` (union DAG, level-scheduled chunks) exist and are tested for bit identity at 1 / 2 / 4 / 8 workers |
| why the mega path cannot use them | (1) memory: one engine arena is (8 * 7 + 40) B x 6.50 M cells = 624 MB, so two engines plus role (169 MB), labels (196 MB) and fields (267 MB) exceed 1,536 MiB; (2) neither API takes the eligibility mask (`set_cross_section_mask` exists only on `Engine`, `vm.hpp:434`); (3) composition accumulates in library order and field residency is a sequential Belady plan (`strategy_ic_runner.cpp:180-188`); (4) the worker cap of 4 |
| processes | the runner sums RSS over the process tree (`run_bounded_research.py:116-126`), so worker processes do not escape the rule |
| what would allow 16 cores | evaluation whose memory does not scale with dates (O6), or an owner ruling on the cap for mining runs |

### Q8. Data and fields

| step | how | cost |
|---|---|---|
| role | `prepare_recent_research.py` projects close, volume, cumulReturnFactor only (`:71`); base fields are close, raw_close, volume (`strategy_ic_runner.cpp:58`). Open / high / low are in the vendor schema (`prepare_tickerhistory.py:37`) but not in the role | 2.6 s, 320 MiB (U2) |
| field | a producer function in `prepare_research_fields.py` (3,034 lines) or a field module (`research_fields_sec.py` 939, `research_fields_holdings.py` 1,221) writes `<name>.f64` date-major plus a manifest row with clock, staleness and PIT flag | 49.6 MiB per field; 40 fields 41 s; 63 fields 121 s (progress) |
| clocks | per-group declared lag and staleness cap (fundamentals 200 / 400 d, groups 550 d, FINRA 45 d); visible at t iff clock < date(t-1) 22:00 UTC | in the builder |
| reuse | `--reuse` hardlinks a field when formula id, producer code AST hash and input SHAs match. Field-module fields are never reused (`prepare_research_fields.py:501-502`) | 22 fields recomputed per rebuild |
| engine C++ data plane | `data/dataset.hpp` (pit_delay), `align.hpp` (as-of join), `fundamental_fields.hpp`: not on the mega path | - |

Adding one raw dataset = one producer (100-300 lines of Python) + tests + a manifest entry + a fields rebuild + a
library delta. With 63 fields built, the second new field is refused until O1.

### Q9. Benchmarks

34 files in `atx-engine/bench` (VM kernels, WQ101 battery, batch / parallel eval, factory and search throughput,
executors, combine, cost, optimiser scale, risk build, replay cost, learn). One recorded baseline exists:
`bench/baselines/alpha_throughput.json` (2026-09-23, Release, 16 logical CPUs, shared host). The `equity-bench` preset
was never written (swarm status). WQ101 battery, 70 alphas on 2,520 x 500, mean of 3:

| strategy | 1 thread | 4 | 8 | 16 |
|---|---|---|---|---|
| A: one Program per alpha | 27.1 s, 3.3 M cells/s | 8.2 s, 10.8 M | 5.8 s, 15.4 M | 5.1 s, 17.4 M |
| B: union DAG | 24.0 s, 3.7 M | 6.6 s, 13.3 M | 4.0 s, 22.4 M | 3.4 s, 25.6 M |
| B fused | 20.0 s, 4.4 M | 5.8 s, 15.2 M | 3.5 s, 25.0 M | 3.0 s, 29.2 M |
| warm subtree cache | - | - | 0.157 s, 564 M | - |

- Shared sub-expressions in that battery: 57.8% of nodes.
- Kernel numbers from lane reports (noisy host): rank at 3,000 names 23-27 ns/cell; ts_rank(20) 18 ns; corr 20 ns;
  decay_linear 9 ns; risk build 3,000 x 14 x 60 x 504 in 622 ms.
- Mega path, measured (code-review-v7 S3): 39 candidates on 6.50 M cells, VM 22.4 s = 11.3 M cells/s at 4 workers,
  the same rate as strategy A at 4 threads. IC 10.4 s, composition 7.0 s.
- Reading: union DAG + fusion + 16 workers is about 2.6x the current VM rate on a cold pass (est.). Going from 4 to
  16 threads alone gives 1.6x (A) to 1.9x (B). The warm cache is 50x, on entries that cost 52 MB each at mega size.

### Q10. Size, duplication, dead weight

| 15 largest engine TUs | lines | 15 largest atx-impl TUs | lines |
|---|---|---|---|
| src/factory/factory.cpp | 2,365 | src/strategy_nav_replay.cpp | 2,732 |
| include/alpha/vm.hpp (header-only) | 2,205 | src/stage_equity_mine.cpp | 2,596 |
| src/factory/search_driver.cpp | 2,165 | src/strategy_ic_runner.cpp | 2,568 |
| src/eval/trial_registry.cpp | 1,930 | src/stage_equity_ic.cpp | 2,274 |
| src/book/replay.cpp | 1,518 | src/stage_combine.cpp | 1,799 |
| src/data/point_in_time_universe.cpp | 1,496 | src/stage_equity_universe.cpp | 1,569 |
| include/alpha/ts_ops.hpp | 1,456 | src/stage_discover.cpp | 1,514 |
| src/eval/cross_section_ic.cpp | 1,399 | src/strategy_spo.cpp | 1,496 |
| src/risk/qp_solver.cpp | 1,387 | src/strategy_live.cpp | 1,413 |
| src/learn/gbt.cpp | 1,252 | src/strategy_risk_model.cpp | 1,238 |
| src/risk/factor_model.cpp | 1,217 | src/strategy_target_replay.cpp | 1,197 |
| src/alpha/oracle.cpp | 1,180 | src/config.cpp | 1,184 |
| src/risk/qp_factor_admm.cpp | 1,145 | src/strategy_runner.cpp | 1,060 |
| include/alpha/streaming_engine.hpp | 1,125 | src/strategy_risk_verb.cpp | 1,046 |
| src/factory/execution_objective.cpp | 1,045 | src/stage_report.cpp | 1,037 |

Largest test TUs: strategy_nav_replay_test 2,812; eval_cross_section_ic_test 2,855; data_point_in_time_universe_test
2,661; strategy_ic_runner_test 2,496; alpha_eval_perf_test 2,407; combine_test 2,355.

| item | evidence | effect | change |
|---|---|---|---|
| one impl library for two pipelines | `atx-impl-core` holds every stage_* and strategy_* source (`atx-impl/CMakeLists.txt:4-66,190`); the four mega exes link it | a wide rebuild compiles 18 k lines of stage_* code the mega path never calls | split `atx-impl-strategy` from `atx-impl-pipeline` (S) |
| monolithic engine library | 138 sources in one target (`atx-engine/CMakeLists.txt`) | wide rebuilds 171-269 s (v7 S3) | object libraries per module; mega exes link alpha, factory IC, role reader, parallel, book cost (M) |
| header-only VM | `vm.hpp` + `ts_ops.hpp` + `cs_ops.hpp` + `lit_ops.hpp` + `streaming_engine.hpp` about 6,000 lines; `vm.hpp` is included by 19 source / header files and 68 test TUs | each VM edit recompiles all of them and resets the source pin (`strategy_ic_runner.cpp:106`) | none now; batch VM edits |
| Python DSL front end | `generate_fund_ic_v*.py` carry their own parser, lookback and peak-slot estimator (`generate_fund_ic_v71.py:310-314`); 10 generators, 7,504 lines with tests | can diverge from the engine typechecker | O5 |
| three trial ledgers | engine `TrialRegistry`, impl `trial_ledger.cpp`, Python JSONL | no single N | section 4.4 defines the bridge |
| integrity tools in a sprint directory | `.superpowers/sdd/mega-alpha-20260926/studies/` | not versioned with the code they judge | move to `atx-impl/tools` (S) |
| older tool | `strategy_runner.cpp` (1,060) + `tools/equity_strategy.cpp` | superseded by the ic / targets / risk verbs | confirm with owner, then retire |

## 3. Top 10 wiring opportunities (value / effort, best first)

| # | what to wire | files | glue needed | risk | acceptance test | value / effort |
|---|---|---|---|---|---|---|
| O1 | Lift the 64-field cap; add open, high, low (and a raw typical-price proxy) as fields | `strategy_ic_runner.cpp:180-188,507-546,737,1129`; `prepare_recent_research.py:71`; `prepare_research_fields.py` | replace the u64 residency mask by a fixed bitset (256); manifest row cap to 256; 3 producers under the adjusted / raw basis rule of LEDGER D0 | mixed price bases (D-03); each field is 50 MiB of disk on a 93%-full drive | a 65-field manifest is admitted; the v7.1 u pass is byte-identical; the field plan is unchanged for libraries with <= 64 fields | H / S |
| O2 | Raise the worker cap from 4 to 12-16 | `ic_screen.cpp:287`; `ic_research.hpp`; `strategy_ic_runner.cpp:349,651-652,2348`; spec `--workers` | constants plus the admission envelope per worker (8 MiB stack + scratch) | none on bits (DetPool contract); about + 100 MB at 16 workers | u / w outputs byte-identical at 4, 8, 16 workers; VM, IC and composition seconds recorded | M / S |
| O3 | Share sub-expressions across candidates: mask-aware SubtreeCache, then union DAG | `subtree_cache.hpp:70`; `vm.hpp:633-634`; `global_dag_eval.hpp:367,586`; `strategy_ic_runner.cpp:2048` | add a mask digest (and the kernel policy) to `SubtreeKey`; pass the mask to the per-worker engines; charge the byte budget in `admit()` | each entry is 52 MB, so the budget holds few entries; a wrong key serves wrong bits | cold v7.1 u pass byte-identical with the cache on; hits reported; `GlobalDagStats.computed` < node count | M (cycle), H (mining) / S-M |
| O4 | Orthogonality read-out: marginal IC of each candidate after residualising on the library composite and the theme composites | `combine/orthogonalize.hpp`; `alpha_report_card.py`; the saved `train_combined` payload | a C++ verb streaming by date over the cached candidate payload and P <= 11 pool panels; HAC t, not IID (E-03) | per-date decomposition cost grows with P (E-10): keep P small | synthetic: a candidate equal to pool + noise has marginal IC 0 within 2 SE; a planted orthogonal signal is recovered; one card column per member | H / S-M |
| O5 | Engine static check replaces the Python DSL grammar and budgets | `strategy_ic_runner.cpp --plan-only`; `generate_fund_ic_v*.py`; `check_fund_ic_v6.py` | emit per-candidate lookback, num_slots, fields and node count as JSON; the checker reads it | none on results | the plan JSON equals the generator's static table for v7.1 (48 rows); the generator's parser is deleted | M / S |
| O6 | Date-blocked evaluation: memory independent of history | `streaming_engine.hpp`; `strategy_ic_runner.cpp:611-651,2006-2058`; `strategy_ic_composition.cpp`; `ic_screen.cpp` | an eligibility row in `CrossSection`; blocks of 128-256 dates with a lookback halo, or StreamingEngine state carried across blocks; IC labels and composition streamed by date | ResearchFast online state differs if a block restarts it, so bits change unless state is carried; stateful ops anchor at the first date | the v7.1 u pass is byte-identical to the whole-panel pass; peak RSS < 600 MiB; a 2,770-date role is admitted | H / L |
| O7 | Walk-forward fold driver for the frozen book, 2012-2022 | `scripts/research_cycle.py`; `prepare_recent_research.py`; `prepare_research_fields.py`; `nav_summ.py` | a `folds` list in the spec (role + fields + NAV per window; signs and weights pinned by the prior / ew-theme rule); one ledger line per fold. Per-fold 3-year roles work today without O6 | pre-2018 data gaps (FINRA SI from 2017-12, D-20); every fold read is a trial; admission statistics stay those of TRAIN | the 2020-2022 fold reproduces the v7.1 cell byte for byte; each fold has a receipt and a ledger line; the pre-registration names the folds before any read | H / M |
| O8 | Mining verb on a research role (section 4) | new `atx-impl/src/strategy_mine.{cpp,hpp}`, `tools/equity_strategy_mine.cpp`; `factory/search_driver.cpp:321`; `op_catalog.cpp:83`; `eval/trial_registry.hpp`; `fit_composition_weights.py` | role + fields panel adapter; eligibility mask in the search engines; IC fitness; registry; `mined-v1` admission | data mining on a window read 37 times; memory; the racing defect A-06 | section 4.7 | H / L |
| O9 | Statistical peer groups as a Group field | `alpha/cluster_panel.hpp`, `cluster_field.hpp`; fields builder | a producer writing `grp_cluster` (rolling RMT-cleaned correlation clusters) with a declared clock | clusters are fitted on returns: the window must end at t-1; labels are unstable over time | the label at t is unchanged when rows > t are mutated; a cluster-relative reversal candidate compiles and runs | M / M |
| O10 | One exposures / factor-return kernel for C++ and Python; then move atx-risk-v1.1 and spo into the engine | `strategy_price_exposures.cpp`; `fit_composition_weights.py:537-676`; `strategy_risk_model.cpp`; `risk/cov_ewma.cpp` | an `exposures` verb writing the per-decision basis and the member factor returns once per role; the fitter and the mining fitness read it | admission factors may change in the last bits (QR vs Cholesky) | admission.json identical for v7.1; one rank helper left | M / M |

Root items, not ranked: adopt the Release build after the quiet-host timing (v7 A5; identity already passed). AVX2 /
FMA is a new VM identity and needs a new cache and re-pinned goldens. Run the library once in AuditExact to measure the
ResearchFast difference. Make field-module fields reusable.

## 4. Automated alpha discovery: readiness and proposed design

### 4.1 What exists

| component | file | state |
|---|---|---|
| evolutionary loop: tournament selection, elitism, immigrants, stagnation stop, adaptive operator rates | `factory/search_driver.hpp:123-330` | tested; seeded by (master_seed, generation, index); byte-identical replay |
| multi-objective selection (NSGA-II fronts + crowding) | `factory/pareto.hpp`; `ObjectiveMode::MultiObjective` | tested; default |
| typed grammar sampler, valid by construction | `factory/generate.hpp` | tested; 8 productions; fields set in `GenConfig`; defaults name OHLCV + `IndClass.*` |
| mutations: op_swap, field_swap, jitter_const (window / scale), wrap_in_op; subtree crossover; constant search | `factory/mutation.hpp`, `crossover.hpp`, `param_search.hpp` | tested |
| operator catalogue | `factory/op_catalog.cpp:83` | builtin ops only: the 17 literature ops and all hparam / record ops are excluded |
| dedup: structural hash, semantic rewrite, output fingerprint | `canonical.hpp`, `rewrite.hpp`, `fingerprint.hpp` | tested |
| novelty: behavioural archive, k-nearest distance on a PnL or rank-IC profile | `factory/behavior.hpp` | tested |
| fitness: `raw = wq * diversify * robust`, diversify = 1 - mean abs corr to pool; DSR; capacity and turnover objectives | `factory/fitness.hpp` | tested; PnL-stream based |
| residual IC objective: WLS residual on 7 styles + FF49, tied Spearman, HAC t at h = 21, 63, 126, half-life, rank autocorrelation | `factory/objective_ic.hpp`; `FitnessObjectiveRule::ResidualHacIcV2` | tested on synthetic inputs; needs a `data::ExposurePanel` |
| IC equivalence screen (rejects practical nulls) | `factory/ic_screen.hpp` | tested; null rejection rate not qualified (LEDGER 2026-09-26) |
| multi-fidelity racing | `factory/fidelity.cpp:94` | date-strided rungs change the alpha (A-06, still in `strided_panel`) |
| trial accounting | `eval/trial_registry.hpp`: content-addressed, windows, family / theme tags, screened-only records, ONC cluster N, chain head | tested; used by equity-mine |
| gates: BY, Romano-Wolf, family blend as hypothesis K + 1 | `eval/multiple_testing.hpp`; `stage_equity_mine.hpp:21-27` | tested |
| across-run driver, lockbox, robustness battery, process workload | `research_driver.hpp`, `eval/lockbox.hpp`, `robustness_battery.hpp`, `workload_mine.hpp` | tested |

### 4.2 Where it runs today

| entry | input | window | result on record |
|---|---|---|---|
| `atx-impl equity-mine` | yearly panel contexts + PIT membership image | refuses any session >= `--seal` (default 2020-01-01); default budget 6 GB (`stage_equity_mine.cpp:1577`) | 2013-2019 run: 2,523 candidates, 0 admitted, 819 s, peak 0.96 GiB (LEDGER 2026-09-26) |
| `atx-impl discover`, `sweep` | APNL panel artifact, SQLite store | lockbox fraction of the panel | synthetic and development only |
| research role + fields-v9 | - | - | not runnable |

`mine::mine_train` takes any `alpha::Panel` + membership mask + window (`stage_equity_mine.hpp:395`), so the core is
reusable. The command line, the context stitcher and the scorer are tied to the old pipeline.

### 4.3 Gaps to close (glue)

| id | gap | size |
|---|---|---|
| G1 | role + fields panel adapter. The code exists privately in the IC runner (`dsl_panel` `strategy_ic_runner.cpp:1543`, `FieldResidency` `:1621`); lift it to a shared function | S |
| G2 | eligibility mask in search engines: `search_driver.cpp:321` builds engines without `set_cross_section_mask` | S |
| G3 | fitness on the mega IC kernel (`evaluate_research_ic`, same labels, guard and membership as the u pass) instead of PnL streams; this also removes the 52 MB position copies (A-14) | M |
| G4 | orthogonality term from O4 (signal space) or O10 (factor-return space) | S after O4 |
| G5 | operator catalogue: add the min-periods and regression ops; deny trade_when, hump, kalman_level, ou_filter, kalman, split2 as `check_fund_ic_v6.py:64` does | S-M |
| G6 | racing without date strides: instrument stride only, or a contiguous sub-window | S |
| G7 | registry persisted per campaign; chain head copied into the cycle ledger; resume across bounded runs (`search_progress.hpp`) | M |
| G8 | admission rule for candidates without a prior (`screen_v4` rejects them as `no_prior`) | M |
| G9 | memory: one engine at a time; field-swap over 63 fields thrashes residency. Search in theme batches of <= 5 resident extras | design |

### 4.4 Proposed design `mine-v1`

**Windows (to pre-register before any read).**

| option | discover | confirm | needs | comment |
|---|---|---|---|---|
| A (recommended) | 2012-03 to 2019-12 | TRAIN 2020-2022, one read of K promoted candidates | roles + fields for 2012-2019 (the O7 data build) | the confirm window is untouched by the search; short-interest and short-volume families cannot be searched before 2018 |
| B (soonest) | TRAIN 2020-2021 | TRAIN 2022 | G1-G8 only | weak: 252 confirm sessions; TRAIN already carries 37 construction cells |

**Search space.**
- Fields: close, raw_close, volume, the fields-v9 list, and O1's open / high / low. Group fields: grp_sic2, grp_ff12,
  grp_ff49 (later grp_cluster).
- Operators: the builtin catalogue minus the six denied ops, plus `*_mp`, `ts_resid_on`, `ts_beta_on`, `cs_resid_on`,
  `ts_topk_mean`, `bucket`, `group_cross`.
- Windows from a fixed set {5, 10, 21, 42, 63, 126, 252}; no continuous window jitter in stage 1.
- Shape: the root is `rank(...)`, `group_rank(..., g)` or `group_neutralize(..., g)`; depth <= 4; house budget
  unchanged (314 bars, 7 slots, 5 extras, 4,096 B).
- Stage 1: exhaustive enumeration of depth <= 2 templates (a field or a field ratio, one transform, one window, one
  normaliser). Stage 2: NSGA-II from the stage-1 front, with mutation and crossover inside the same grammar.

**Fitness (discover window only; labels h = 5, 21, 63; decision lag 1).**

| term | definition | source |
|---|---|---|
| f1 signal | HAC t of the mean daily rank IC at h = 21 of the signal residualised on the price-risk exposures and FF12 | `objective_ic`, or the mega neutraliser (O10) |
| f2 orthogonality | HAC t of the marginal IC after residualising on the library composite and the 10 theme composites; max abs rho to any member is reported | O4 |
| f3 turnover | standalone rank-book turnover tau, hard limit .70 (the v4 limit); objective = lag-5 rank autocorrelation | `ObjectiveIcResult.mean_rank_autocorrelation` |
| f4 coverage, parsimony | paired-name fraction >= .8; node count as an objective | existing |
| selection | NSGA-II on (f1, f2, -tau, -nodes); racing scalar = min(f1, f2) | `pareto.hpp` |

**Budget (fixed in the pre-registration).** Stage 1 <= 12,000 expressions; stage 2 = 8 generations x 1,000 = 8,000.
Total N <= 20,000. Cost (est.): 0.57 s VM + 0.27 s IC per candidate on 6.50 M cells at 4 workers (from the v6.1 cold
pass), so 20,000 full evaluations take 4.7 h; with instrument-stride racing (first rung on 1 / 4 of the names), O2
and O3, about 1-1.5 h. One 180 s bounded run holds about 180 full evaluations, so mining needs either resume across
100+ runs or an owner ruling for a longer cap.

**Trial accounting.**
- Every expression that reaches the VM is recorded: full evaluations with their daily IC series; racing drop-outs and
  IC-screen rejections as screened-only records.
- Kind = MinerExpr. Config hash = semantic canonical hash of the DSL + window + role SHA + fitness recipe SHA.
  Family = stage. Theme = field theme.
- One registry file per campaign, reopened with its chain-head anchor on every resumed run.
- The cycle ledger gets one line per campaign: n_raw, ONC cluster N, registry chain head, discover window.
- DSR on the discover window uses cluster N over all trials. The confirm test uses K (promoted) + 1 (family blend).

**Admission hand-off.** v4-prior-v1 cannot take these candidates: it orients by a literature prior and vetoes only
below t = -2. New rule `mined-v1`:

| check | value |
|---|---|
| promotion from discover | f1 HAC t >= 3.0 and f2 HAC t >= 2.0; cluster-N DSR >= .95; tau <= .70; greedy de-duplication at abs rho .70; K <= 50 |
| sign | frozen from the discover window |
| confirm (one read) | same sign; HAC t >= 2.0 on the neutralised factor return; BY-adjusted one-sided p <= .10 over K + 1 hypotheses |
| redundancy | abs rho <= .70 to every admitted library member (v4 uses .90) |
| weight | all admitted mined candidates form one theme `mined` in ew-theme (at most 1 of 11 theme shares) |
| after admission | a normal construction cell, N + 1, paired against the parent cell |

Power: a single alpha needs an annualised factor-return Sharpe of t / sqrt(years): 1.06 for t = 3 on 8 years, 1.15 for
t = 2 on 3 years. Few candidates will pass. The family blend (hypothesis K + 1) is the more likely admission.

### 4.5 Risks

| risk | control |
|---|---|
| search fitness and admission measure different things (A-04) | G3: both use the mega IC kernel and lag-1 labels |
| racing on strided dates (A-06) | G6 |
| undercounted trials (I-14) | screened-only records; registry N reconciled with the search counters in the receipt |
| size / liquidity proxies win (I-13; the 2013-2019 run) | f1 is a residual IC; f2 removes what the library already holds |
| ResearchFast bits | re-score promoted candidates in AuditExact before the confirm read |

### 4.6 Order of work
O1 and O2 (S each) -> O4 (usable at once on the 10 v7.1 candidates that were not admitted) -> G1, G2, G3, G5, G6, G7
(the mining verb) -> dry run on synthetic planted signals -> O7 data build -> option A.

### 4.7 Acceptance test for O8
- Synthetic role with 3 planted signals and 1 planted copy of a library member: the 3 are promoted, the copy is
  rejected by f2, and no pure-noise expression is promoted at the declared thresholds in 5 seeds.
- Same seed twice: identical registry chain head and identical promoted list, at 1 and 4 workers.
- Registry n_raw equals evaluated + racing-rejected + screen-rejected in the run receipt.
- Peak RSS <= 1,536 MiB on the lo1 role with 5 resident extras.

## 5. Engine limits that block v8 goals

| limit | today | blocks | fix |
|---|---|---|---|
| field count | 63 built, 64 allowed (impl bitmask) | any second new dataset; OHLC | O1 |
| price fields | close, raw_close, volume only | intraday-range, gap and overnight families; most of the WQ101 battery | O1 |
| lookback | 336 on the TRAIN role (house 314) | FF3 residual momentum (503), nincr (797), multi-year seasonality (504 / 756) | rebuild the role with an earlier warm-up (a tool argument) once memory allows, or O6 |
| memory | 1,536 MiB rule; admitted 1,449 MB at 6.50 M cells, 7 slots, 5 extras | longer warm-up, more slots, candidate parallelism, pre-2020 history in one role (about 4 GiB, v7 A6) | O6; or a role with the never-member instrument columns removed (saving not measured) |
| history length | roles are 3-year blocks; shape cap 4,096 dates | 2012-2022 evaluation in one pass | O7 with per-fold roles now; O6 later |
| workers | 4 | 12 idle logical cores | O2 |
| run time | 180 s per bounded run | mining (about 180 evaluations per run) | resume (G7) or an owner ruling |
| candidate isolation | no sharing across candidates; engine cache off under a mask | cold passes and mining throughput | O3 |
| eligibility in parallel and streaming APIs | only `Engine` has the mask | union DAG, parallel_evaluate, SearchDriver, StreamingEngine on a role | G2, O3, O6 |
| search operator set | builtin ops only | mined expressions cannot use min-periods or regression ops | G5 |
| admission rule | prior-based only | any data-mined candidate | G8 |
| float width | f64 slots and payloads only | 2x memory and disk against f32 | not proposed: changes every bit and every pin |
