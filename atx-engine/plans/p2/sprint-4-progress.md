# Sprint S4 (p2) — Genetic Search & Robust Signal Pipeline — Implementation Progress

**Status:** ✅ CLOSED — 2026-06-14 (S4.0–S4.5 shipped; full suite **1345 pass / 5 disabled**; `/W4 /permissive- /WX` + `/fp:precise` clean; merged `--no-ff` into `main`)
**Worktree:** `C:\Users\natha\atx-wt\p2-s4` (isolated — one branch per worktree, no shared-branch race)
**Branch:** `feat/p2-s4-genetic-search`
**Base:** `main @ 2a500f1` (docs commit freezing the S4 plan; code-identical to the S3 merge `65f4ccb`)
**Source plan:** [`sprint-4-genetic-search-robust-signal-pipeline-implementation-plan.md`](sprint-4-genetic-search-robust-signal-pipeline-implementation-plan.md)
**Spec:** [`sprint-4-genetic-search-robust-signal-pipeline.md`](sprint-4-genetic-search-robust-signal-pipeline.md)
**Build env:** `cmd /c "call C:\Users\natha\atx-wt\p2-s4-env.cmd && <cmd>"` (VS2022 + vcpkg + sccache + ninja);
preset `dev`; target `atx-engine-tests`; exe `build\bin\atx-engine-tests.exe`.

**Dev-loop speed (2026-06-14):** full suite was ~251 s, half of it three `MultiHorizonIntegration`
augmented-QP tests (`R1_FullPipelineDeterministicByteIdentical` 42 s, `R2_TruncationInvarianceNoLookAhead`
54 s, `R3_AugmentedConstraintsExactEveryPeriod` 31 s — each invokes the augmented QP over a full 4-period
schedule, ~20 s/run). `DISABLED_`-prefixed for the S4 dev loop (suite → ~124 s); the fast siblings
`R2_TrajectoryIsPureFunctionOfCurrentAlpha` + `R7_DegenerateReducesToMultiPeriodByteIdentical` keep the
no-look-ahead / boundary-pin coverage. Left disabled at close per the user's directive (suite fast);
**tracked in the ROADMAP backlog** — optimize the augmented-QP `MultiHorizonOptimizer::run` perf (~20 s/run is
the real cost), then re-enable. The dev-loop `--gtest_filter` path is unaffected; run with
`--gtest_also_run_disabled_tests` for full coverage. New baseline: **1281 pass / 5 disabled** (the prior 2 + these 3).

---

## §0 As-built recon (the plan's §0, confirmed at base SHA) — the seams each unit edits

Recon re-run at base (`2a500f1`, code == `65f4ccb`) against `factory::{fitness, search_driver, research_driver,
op_catalog, mutation, generate}`, `combine::{combiner, gate, store}`, `eval::{deflated_sharpe, cpcv, pbo}`,
`cost::{cost_aware, capacity}`, `library::Library`, `parallel::DetPool`. Confirmed file:line:

1. **The scalar objective is consumed at exactly four seams** — the NSGA-II swap points:
   `search_driver.cpp:180` (`fit = rep->raw` cache), `:228` (`selection = fitness − penalty` novelty fold),
   `:397` (`tournament_pick` `selection >`), `:378–379` (`raw_ordered_indices`, `fitness >` + canon tie-break).
   `Scored` = `{Genome genome; f64 fitness; f64 selection;}` (`search_driver.hpp:187–191`). Nothing else touches the
   scalar — seed axis, digest fold, DetPool fan-out, mutation are objective-agnostic. **The swap is surgical.**
2. **The three factors are already separate** — `FitnessReport` (`fitness.hpp:116–124`) carries
   `{wq, redundancy, diversify, robust, raw, dsr, haircut_sharpe}`; `raw = core.wq * diversify * core.robust`
   (`fitness.cpp:221`), `diversify = clamp(1−redundancy,0,1)` (`fitness.cpp:220`). The objective vector
   `(wq, diversify, robust)` is a re-projection — no new fitness math.
3. **Cost model exists** — `cost_aware.hpp:114` `round_trip_cost_bps(cc, participation, sigma)`;
   `capacity.hpp:30` `capacity_for_book(...)`, `CapacityPoint{aum, net_edge_bps}` (`:107`),
   `kCapacityAdvWindow=20` (`:116`). S4.3 **wires** it as the 4th objective; builds no model.
4. **NSGA-II absent** — grep `include/` finds zero `nondominated_sort`/`pareto`/`nsga`/`crowding`. S4.1 ships
   engine-local `factory/pareto.hpp` (O(N²), canonical-id tie-break). Pattern-B L7 lift recorded.
5. **Generation (S3.5) not wired into seeding** — `generate.hpp:203` `generate_genome(cfg, lib, rng)` +
   `GenConfig` (`:50`) exist but the search loop never calls them (gen-0 fill at `search_driver.cpp:100–104`).
   S4.1 wires grammar-typed generation into the initial population + immigration.
6. **`op_swap` live + contract-safe** (S3.4): `enable_op_swap{true}` (`search_driver.hpp`), `validate_node_contract`
   ⇒ analyze-valid ⟹ VM-safe for every mutation/crossover/generation path. S4 reproduction inherits no-abort for free.
7. **Determinism axis** `(master, run, gen, idx)`: `seed_for(master,gen,idx)` (`search_driver.hpp:143`),
   `seed_for_run(master,run)` (`research_driver.hpp:140`), `DetPool::parallel_for(n, body)` (`det_pool.hpp:93`)
   worker-count-invariant. `ResearchReport{digest, manifest_version_id}` (`research_driver.hpp:115`) the replay witness.
8. **No lockbox primitive** — S4.4 builds the seal; embargo width from `CpcvConfig.embargo=0.01` (`cpcv.hpp:94`).
9. **p2 S1 optimizer landed** — S4.5 book backtest uses it (p1 S7 fallback documented).
10. **Driver entry is `ResearchReport run(const ResearchConfig&)`** (`research_driver.hpp:181`), not `mine_into`
    (plan named it `mine_into`); S4.5 routes the multi-objective + robustness gate through `run`.

---

## Unit ledger

| Unit | Title | Status | SHA | Tests | Notes |
|---|---|---|---|---|---|
| S4.0 | Marker + ledger + recon | ✅ | `c32cd85` | — | impl plan frozen; baseline 1284 pass / 2 disabled; §0 seams confirmed at base SHA |
| S4.1 | Multi-objective NSGA-II Pareto selection (`pareto.hpp` + 4 seams + boundary pin) | ✅ | `1ec0fcd` | 21 | `pareto.hpp` (`dominates`/`fast_nondominated_sort`/`crowding_distance`, NaN→−∞, canon tie-break); `FitnessReport.objectives[{wq,diversify,robust}]` re-projection (`kMaxObjectives=5`); 4 seams rewired (`ObjectiveMode::{ScalarRaw,MultiObjective}`, default MultiObjective); **boundary pin `0xa83f0d3e0b41a18d` byte-identical** in ScalarRaw; DetPool {1,2,4,8} invariant both modes; S3.5 generation wired behind `seed_from_grammar` (default off, axis `0xFFFF…` disjoint from reproduction streams); fitness cache value `u64→CachedScore`; 1302 pass / 5 disabled |
| S4.2 | Behavioral / phenotypic diversity (`behavior.hpp` + novelty objective) | ✅ | `06876cb` | 15 | `behavior.hpp`: descriptor = `FitnessCore.oos_pnl` (canon-cacheable), `behavioral_distance = 1−|corr|` (reuses `combine::pairwise_complete_corr`; RankIc fork via exhaustive `BehaviorMetric`), `BehavioralArchive` FIFO(cap 64), `novelty = mean k-nearest over population∪archive`; per-generation novelty pass → `objectives[3]`, `n_objectives=4`, **distinct from `diversify`**; gate `MultiObjective && novelty_w>0` (off → pin holds); archive updated with rank-0 front in canon-id order. **Pin `0xa83f0d3e0b41a18d` byte-identical**; DetPool {1,2,4,8} invariant. Headline flip: hash-distant/behaviorally-identical (canon 1.0 / behav 0.0) vs hash-adjacent/orthogonal (canon 0.016 / behav ≈1.0). Noise re-cal seed 51→91 (kMinDsr=0.80 bar untouched). 1317 pass / 5 disabled |
| S4.3 | Cost-aware mining fitness (route `cost_aware.hpp`/`capacity.hpp` into the objective) | ✅ | `286efc8` | 6 | `FitnessCfg.{target_aum,cost::CalibratedCost}`; `FitnessReport.cost_bps`; `book_cost_bps` = book-aggregate of `cost::round_trip_cost_bps(cost, participation_i, σ_i)` (participation `(AUM·\|w_i\|/price_i)/ADV_i`, ADV/σ off date-major panel `close`×`volume`, last-rebalance positions); `objectives[4] = −cost_bps` (negated, pure-max), **fixed-slot scheme** (0 wq,1 diversify,2 robust,3 novelty,4 −cost); novelty pass → `n_objectives = max(·,4)`; cost-on gate `target_aum>0`, **`target_aum=0` pure no-op → pin `0xa83f0d3e0b41a18d` byte-identical**; canon-cacheable in `CachedScore`; DetPool {1,2,4,8} invariant. Hand-check 369.65 bps; flip: cost-on → strong/expensive & weak/cheap mutually non-dominated, cost-off → expensive strictly dominates. **kMaxObjectives=5 now full** (S4.4 folds into `robust` or bumps the constant). 1323 pass / 5 disabled |
| S4.4a | Robustness measurement — synthetic-alpha recovery (`eval/synthetic_alpha.hpp`) + regime/walk-forward survival (`eval/regime_slice.hpp`) + `RobustnessVerdict` | ✅ | `15808cc` | 10 | `generate_synthetic_panel(seed, PlantedSpec{expr\|factor, beta, σ})` (fwd-returns = β·planted + noise), `recovery_correlation`; `regime_labels` vol-tercile partition, `per_regime_sharpe<3>`, `walk_forward_sharpe`, `RobustnessVerdict{regime_sharpe[3], walk_forward_sharpe[], worst_regime/window, recovery_corr, is_robust}` = `worst_regime≥min ∧ worst_window≥min`. **Proof:** β>0 panel admits 4 survivors (mean \|corr\| 0.31, best 0.51) vs β=0 noise admits 0 (r*=0.20); high-vol-collapse alpha rejected, all-regime alpha passes; walk-forward-collapse also rejected. OOS fit/apply firewall. No production edits → pin untouched. GA proof ~2.2 s. 1333 pass / 5 disabled |
| S4.4b | Lockbox reservation/seal (`eval/lockbox.hpp`) + `research_driver` robustness-gate hook | ✅ | `ee3d881` | 8 | `reserve_lockbox(panel, frac=0.20, embargo)` → `SealedPanel{visible() → Panel over [0, lockbox_begin−embargo), field_cross_section() Result-seal + …_or_trap() ATX_ASSERT-seal, content_address wyhash}`; terminal-frac lockbox + cpcv embargo gap; **no open API (S8.2 opens once)**; content-addressed, no RNG. Proof: upstream fitness+combine clean over `visible()`, sealed read traps (both forms incl. `EXPECT_DEATH`), identity round-trips, edge cases. **research_driver hook**: `ResearchConfig.robustness_gate{false}`+`robustness_cfg`, single guarded `if` after digest fold (dead when off) → **pin + all ResearchEngine digests byte-identical**; live gating deferred to S4.5. +`Panel::field_name(usize)` additive accessor. 1341 pass / 5 disabled |
| S4.5 | E2E robust pipeline + bench + close | ✅ | `378c523` | 4 | `factory/robust_pipeline.hpp` `RobustResearchDriver`: reserve+seal lockbox → inner `ResearchDriver` over `SealedPanel.visible()` (multi-objective + novelty + cost + **robustness gate ON**) → `AlphaCombiner::fit` ShrinkageMv over the robust subset → **p2 S1** `MultiPeriodOptimizer::run` book backtest (p1 S7 fallback documented). Robustness-gate fill in `research_driver.cpp` (`screen_run_robustness`: per-survivor `regime_labels`+`robustness_verdict`, records counts; combine/book filter to verdict-passers). **Proofs:** noise admits 0 across {41,47,53,59} (60-seed probe; bar kMinDsr=0.80 untouched); β=0.06 synthetic admits 10 robust survivors vs β=0 noise 0; **{1,2,4,8} replay byte-identical** (digest+vid); **pipeline boundary-pin equivalence** — collapsed pipeline digest `0xc5300fdc11b56e8c`/vid `3086061925` == plain `ResearchDriver` on the same fixture. Bench front-0 grows 3→5→9→17→24. E2E suite ~4.9 s. Pin `0xa83f0d3e0b41a18d` + ResearchEngine digests byte-identical. 1345 pass / 5 disabled |

Legend: ⬜ not started · 🟡 in progress · ✅ done (header + tests + `/W4 /WX` + `/fp:precise` clean).

## Numbers (filled on close)
- **Boundary pin (S4.1 → carried S4.2/S4.3/S4.4b → S4.5):** ScalarRaw `SearchResult.digest` == `0xa83f0d3e0b41a18d` byte-identical through every unit; the S4.5 pipeline-level equivalence pin (collapsed pipeline digest `0xc5300fdc11b56e8c` == plain `ResearchDriver` on the same fixture) and all `ResearchEngine` digests held byte-identical.
- **NSGA-II differential (S4.1):** `fast_nondominated_sort` vs brute-force O(N²) dominance oracle agree; no front-k genome dominated by any front<k; crowding boundaries `+inf`; DetPool {1,2,4,8}-worker digest invariance (ScalarRaw + MultiObjective).
- **Behavioral novelty (S4.2):** ranking flip — hash-distant/behaviorally-identical pair (canon 1.0 / behav 0.0) vs hash-adjacent/orthogonal pair (canon 0.016 / behav ≈1.0); behavioral objective inverts the structural ranking. Archive FIFO(64) canon-id insert/evict; {1,2,4,8} invariant.
- **Net-of-cost (S4.3):** hand-check `book_cost_bps` = 369.65 bps for a known participation/σ; cost-ON → strong/expensive & weak/cheap mutually non-dominated; cost-OFF → expensive strictly dominates (gross order restored). `target_aum=0` pure no-op.
- **Robustness (S4.4):** synthetic recovery — β>0 admits 4 survivors (mean |corr| 0.31, best 0.51) vs β=0 noise 0 (r*=0.20); high-vol-collapse + walk-forward-collapse alphas rejected, all-regime alpha passes; lockbox sealed read traps (Result `Err` + `ATX_ASSERT` death test), seal identity round-trips, S8-only open.
- **E2E (S4.5):** noise grows robust library 0 across {41,47,53,59}; β=0.06 synthetic admits 10 robust survivors (book over 48 periods) vs β=0 noise 0; {1,2,4,8} replay byte-identical (digest+`manifest_version_id`); bench multi-objective vs scalar throughput (NSGA-II O(N²) overhead at pop 16/32), Pareto front-0 grows 3→5→9→17→24 then saturates.

## Residuals lifted to ROADMAP backlog
- Optimize the augmented-QP `MultiHorizonOptimizer::run` perf (~20 s/run); then re-enable the 3 `DISABLED_` `MultiHorizonIntegration` R1/R2/R3 tests.
- S4.5 book stage uses a K=1 placeholder SPD `FactorModel` (single market factor) — wire a trailing-fit risk model over the visible panel.
- Robust subset is re-screened in the pipeline combine/book stage (the append-only lifecycle journal records but does not un-admit); a future sprint could wire a true pre-admit robustness gate inside `mine_into` so the library itself is robust-only.
- `bench/*_bench.cpp` build only under `-DATX_BUILD_BENCH=ON` (default off) — unchanged convention.
