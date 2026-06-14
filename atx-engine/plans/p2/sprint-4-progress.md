# Sprint S4 (p2) — Genetic Search & Robust Signal Pipeline — Implementation Progress

**Status:** 🟡 OPEN — 2026-06-14 (S4.1 shipped; suite 1302 pass / 5 disabled)
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
no-look-ahead / boundary-pin coverage. **Re-enable at S4 close** (or run with `--gtest_also_run_disabled_tests`).
New baseline: **1281 pass / 5 disabled** (the prior 2 + these 3).

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
| S4.0 | Marker + ledger + recon | 🟡 | — | — | impl plan frozen; baseline 1284 pass / 2 disabled; §0 seams confirmed at base SHA |
| S4.1 | Multi-objective NSGA-II Pareto selection (`pareto.hpp` + 4 seams + boundary pin) | ✅ | `1ec0fcd` | 21 | `pareto.hpp` (`dominates`/`fast_nondominated_sort`/`crowding_distance`, NaN→−∞, canon tie-break); `FitnessReport.objectives[{wq,diversify,robust}]` re-projection (`kMaxObjectives=5`); 4 seams rewired (`ObjectiveMode::{ScalarRaw,MultiObjective}`, default MultiObjective); **boundary pin `0xa83f0d3e0b41a18d` byte-identical** in ScalarRaw; DetPool {1,2,4,8} invariant both modes; S3.5 generation wired behind `seed_from_grammar` (default off, axis `0xFFFF…` disjoint from reproduction streams); fitness cache value `u64→CachedScore`; 1302 pass / 5 disabled |
| S4.2 | Behavioral / phenotypic diversity (`behavior.hpp` + novelty objective) | ⬜ | — | — | — |
| S4.3 | Cost-aware mining fitness (route `cost_aware.hpp`/`capacity.hpp` into the objective) | ⬜ | — | — | — |
| S4.4 | Robustness battery (synthetic recovery + regime/walk-forward + lockbox) | ⬜ | — | — | — |
| S4.5 | E2E robust pipeline + bench + close | ⬜ | — | — | — |

Legend: ⬜ not started · 🟡 in progress · ✅ done (header + tests + `/W4 /WX` + `/fp:precise` clean).

## Numbers (filled on close)
- Boundary pin (S4.1/S4.5): collapsed-objective `ResearchReport.digest` == pre-S4 search digest on the frozen fixture.
- NSGA-II differential (S4.1): `fast_nondominated_sort` vs naive O(N²) dominance reference agree; DetPool {1,2,4,8}-worker digest invariance.
- Behavioral novelty (S4.2): hash-distant/behaviorally-redundant vs hash-similar/behaviorally-distinct ranking flip.
- Net-of-cost (S4.3): strong-but-expensive alpha Pareto-dominated at `target_aum>0`, ranking flips back at `target_aum=0`.
- Robustness (S4.4): synthetic recovery corr > r*; matched noise admits ~0; out-of-regime-fail alpha rejected; lockbox read pre-S8 fails.
- E2E (S4.5): noise panel grows robust library ~0 across seeds; byte-identical replay (version_id); mining throughput vs scalar baseline.
