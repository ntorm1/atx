# Sprint S4 (p2) — Genetic Search & Robust Signal Pipeline — Implementation Plan

**Status:** frozen at kickoff (to open: `feat/p2-s4-genetic-search`, base `main @ 65f4ccb` — the S3 merge).
**Spec (the *what*):** [`sprint-4-genetic-search-robust-signal-pipeline.md`](sprint-4-genetic-search-robust-signal-pipeline.md) ·
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md) ·
**Quality bar:** [`../docs/implementation-quality.md`](../docs/implementation-quality.md) ·
**C++ authority:** [`../../../.agents/cpp/agent.md`](../../../.agents/cpp/agent.md) ·
**Engine deltas:** [`../../../.agents/atx-engine/agent.md`](../../../.agents/atx-engine/agent.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
§1/§5/§9 (formulaic mining, correlation = marginal contribution, the factory) ·
[`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md) §3.3 (GP/RL miners) ·
[`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
§7 (ruthless OOS discipline). **Methods:** Deb et al. 2002 (NSGA-II), Lehman–Stanley 2011 (novelty search),
Bailey/López de Prado 2014/2017 (DSR, the lockbox/holdout discipline).

This document is the frozen *how*. The spec is the *what* and does not change; this plan records the as-built
reconciliation against the post-S3 `main`, the per-unit algorithms, and the gates. Where the spec left a metric/fork
"decide at kickoff," this plan resolves it (§0, §4).

---

## §0 — As-built reconciliation (recon fixes)

Recon was run at base `main @ 65f4ccb` (S3 merged) against `factory::{fitness, search_driver, research_driver,
op_catalog, mutation, crossover, genome, generate}`, `combine::{gate, metrics, combiner, store}`,
`eval::{deflated_sharpe, cpcv, pbo}`, `cost::{calibration, capacity, cost_aware, temp_perm}`, `library::Library`,
`learn::hmm`, `risk::vol_regime`, `parallel::DetPool`. The findings pin the exact seams each unit edits and **align the
spec to what S3 actually delivered.**

### 0.1 The scalar objective is consumed at exactly four seams — those are the NSGA-II swap points
The miner's objective is a single `f64`. `factory/fitness.cpp:221` computes `raw = wq * diversify * robust`; the search
caches and ranks on it at four — and only four — sites:
1. **`search_driver.cpp:180`** — `fit = rep->raw` (per-genome scalar cached after `pool_aware_fitness`).
2. **`search_driver.cpp:228`** — `scored[i].selection = scored[i].fitness - penalty` (novelty folds the scalar into a
   scalar `selection`).
3. **`search_driver.cpp:397`** — `tournament_pick`: `scored[cand].selection > scored[best].selection` (selection).
4. **`search_driver.cpp:379`** — `raw_ordered_indices`: `scored[a].fitness > scored[b].fitness`, canon-`less` tie-break
   (elitism).

`Scored` is `{Genome genome; f64 fitness; f64 selection;}` (`search_driver.hpp:187`). S4.1 turns the scalar into a
**vector** `objectives[]` plus `nsga_rank`/`crowding` on `Scored`, and replaces the four compares with
dominance/(rank,crowding) compares. **Nothing else touches the scalar** — the seed axis, the digest fold, the DetPool
fan-out, and the mutation operators are objective-agnostic, so the swap is surgical.

### 0.2 The three multiplied factors are already computed separately — the vector is free
`FitnessReport` (`fitness.hpp:116`) carries `{wq, redundancy, diversify, robust, raw, dsr, haircut_sharpe}`. `wq`,
`diversify` (= `clamp(1−redundancy,0,1)`), and `robust` are each computed and stored **before** the `raw` multiply
(`fitness.cpp:177/220/180–190`). So the S4.1 objective vector `(wq, diversify, robust)` is a re-projection of fields
that already exist — no new fitness computation, only a new struct field that exposes them as `std::array<f64,K>`. `raw`
is **retained** as the boundary-pin collapse target (§1.1).

### 0.3 The cost model already exists — S4.3 routes it, it does not build it
`cost/cost_aware.hpp` ships the S6-calibrated cost surface today:
- `round_trip_cost_bps(cc, participation, sigma)` (`cost_aware.hpp:114`) — the single structural model
  `temp = Y·σ·p^δ`, `perm = 0.5·γ·σ·p`, `rt = 2·(temp+slip)+perm`, in bps.
- `cost_adjusted_fitness(metrics, rt_cost_bps)` (`cost_aware.hpp:169`) = `raw_fitness − turnover·rt_cost_bps·0.1`.
- `capacity_for_book(weights, panel, sim, aum_grid)` (`capacity.hpp:30`) → `CapacityPoint{aum, net_edge_bps}` with
  per-name participation `(AUM·|w_i|/price_i)/ADV_i` over `adv20` (`kCapacityAdvWindow=20`).

So the spec's "route S6-calibrated cost into the mining objective" is a **wiring** job: derive the candidate's
participation at a `target_aum` (the capacity machinery), call `round_trip_cost_bps`, and add `−rt_cost_bps` as the
objective vector's 4th component. **No new cost model.** (Today cost is *not* in the mining objective at all — it lives
only in the downstream gate/optimizer knobs via `CostKnobs` (`cost_aware.hpp:93`); S4.3 promotes it to a first-class
*mining* objective.)

### 0.4 NSGA-II / nondominated-sort is absent from atx-core — the engine ships the deterministic kernel
Grep of `atx-core/include` finds **no** `nondominated_sort` / `pareto` / `nsga` / `crowding` primitive. So S4.1's
`fast_nondominated_sort` + `crowding_distance` ship **engine-local** in a new `factory/pareto.hpp`, deterministic O(N²)
(per-generation N is small — `population` defaults 16, `search_driver.hpp:101`). Recorded as a **Pattern-B L7 lift**
(`nondominated_sort`) for later promotion to atx-core; the engine-local kernel is the proof-of-correctness reference
regardless. This matches the spec's anticipated "engine-side O(N²) deterministic fallback."

### 0.5 Regime labeling is available two ways — ship the vol partition, record the HMM fork
- **HMM (p1 S5):** `learn/hmm.hpp` ships `baum_welch(obs, cfg)` (`hmm.hpp:252`) and the PIT forward filter
  `regime_posterior_at(fitted, obs, d) → VecX` (`hmm.hpp:296`, reads only `obs[0..d]`, truncation-invariant). A
  per-date `argmax` over the posterior gives a hard regime label.
- **Volatility regime (S8):** `risk/vol_regime.hpp::vol_regime_multiplier` (`vol_regime.hpp:149`) emits a market-wide
  `λ²` plus per-date bias stats — a volatility-state signal.

S4.4's regime survival ships a **deterministic volatility-tercile partition** (per-date cross-sectional/market vol →
{low, mid, high}; no model fit, no RNG, trivially PIT) as the default slicer, with the HMM posterior `argmax` recorded
as the richer fork (it requires a fitted `Hmm`, an extra dependency the battery can opt into). This honors the spec's
"reusing the `p1` S5 HMM **or** a volatility partition."

### 0.6 Generation (S3.5) exists but is not yet wired into population seeding — S4 adopts it
`factory/generate.hpp` (S3.5) ships `generate_genome(cfg, lib, rng) → Result<Genome>` (`generate.hpp:203`,
analyze-valid by construction, rejection ≈ 0 vs control) and `GenConfig{max_lookback, max_depth, numeric_fields,
group_fields}` (`generate.hpp:50`). But `SearchDriver` still seeds its initial population from caller-supplied parsed
genomes / mutates existing ones — **`generate.hpp` is not called anywhere in the search loop**. **Adjustment vs the
spec:** S4.1 wires grammar-typed generation into the **initial population** (and the immigration/diversity-injection
path), so the search starts from a valid-by-construction, type-diverse population instead of hand-seeds. This is the
"raises generation yield, cuts wasted eval" payoff S3.5 measured, now realized in the loop. (The spec assumed this
seam already closed; it is not — S4 closes it.)

### 0.7 `op_swap` is live and contract-safe — the full mutation set is available
S3.4 set `enable_op_swap = true` (`search_driver.hpp:117`) and added `validate_node_contract` to `analyze_call`, so
**analyze-valid ⟹ VM-safe for every mutation/crossover/generation path**. `mutate_one` (`search_driver.cpp:310`) now
draws `op_swap | field_swap | jitter_const` (fixed mod-3, op_swap gated by the now-true flag). S4's multi-objective
reproduction inherits a contract-safe offspring guarantee for free: every child the search produces analyzes-clean or
is cleanly rejected — **no VM abort is reachable from the search loop.** S4's determinism + no-abort proofs build on
this; no re-hardening needed.

### 0.8 The determinism discipline is `(master, run, gen, idx)` resolved before any RNG draw
`seed_for(master, gen, idx)` (`search_driver.hpp:139`, SplitMix64) and `seed_for_run(master, run)`
(`research_driver.hpp:140`) compose the full seed axis; `DetPool::parallel_for(n, body(i, worker_id))`
(`det_pool.hpp:93`) is worker-count-invariant by construction (each `i` claimed once, no shared float reduction). **The
load-bearing S4 rule:** every NSGA-II sort, crowding-distance order, behavioral-distance order, and tie-break resolves
in **canonical id order established BEFORE any RNG draw** (the `p1` F1/F2 discipline) — so the digest stays
worker-count-invariant. The `ResearchReport.digest` + `manifest_version_id` (`research_driver.hpp:115`) are the
byte-identical-replay witnesses.

### 0.9 No lockbox mechanism exists — S4.4 builds the seal; the embargo width is inherited
There is no terminal-holdout / seal primitive in the engine. The CPCV `embargo` fraction
(`cpcv.hpp:CpcvConfig.embargo=0.01`) and its `embargo_len = ceil(h·N)` (`cpcv.hpp:40–48`) provide the embargo width
S4.4's lockbox reuses. S4.4 ships a new `eval/lockbox.hpp` reservation + seal; **no upstream stage may read the
reserved region**, proven by an `EXPECT_DEATH`/`Result`-error assertion. S8.2 opens it exactly once.

### 0.10 The S4.5 book backtest has the p2 S1 optimizer available (it merged before S3)
`main` carries the p2 S1 multi-horizon optimizer (`risk/optimizer*`, `risk/multi_period*`, landed pre-S3). So S4.5's
"multi-horizon book backtest" uses the S1 constrained optimizer directly; the `p1` S7 multi-period book is the
documented fallback if an S1 API gap surfaces at kickoff. **Adjustment vs the spec's "if S1 has landed":** it has.

---

## §1 — Design rules (the search-deepening + robustness contract)

1. **Boundary pin: bit-for-bit reduction to the proven scalar miner.** Collapse the objective vector to its single
   `raw` component, novelty units off (`novelty_w = 0` / behavioral objective off), cost off (`target_aum = 0`) ⇒
   NSGA-II fast-nondominated-sort becomes a total order by `raw`, crowding is unused, and tournament/elitism reduce
   **bit-for-bit** to `p1` S3's loop. A regression test asserts the `ResearchReport.digest` of the collapsed S4 search
   equals the pre-S4 search digest on a frozen fixture. **This is S4's load-bearing non-regression anchor.**
2. **Determinism is a type, not a hope.** Every multi-objective sort, crowding order, behavioral distance, archive
   update, and tie-break resolves in canonical id order **before** any RNG draw (§0.8). The seed is in the artifact;
   the digest is worker-count-invariant across `{1,2,4,8}` workers (a `DetPool` fan-out test per stochastic unit).
3. **Robustness is proven by a battery, not asserted.** No alpha is trusted on full-sample OOS alone — it must
   recover a planted signal, survive out-of-regime + walk-forward, and net positive after S6 cost. Each proof is a
   reusable harness with a known ground truth (synthetic) or a held-out slice (regime/lockbox), not a single number.
4. **Cost is a first-class mining objective.** The factory selects net-of-cost (§0.3), not gross-then-deduct. A
   strong-but-expensive alpha is *dominated* once cost enters the Pareto vector, demonstrated by a fixture.
5. **No look-ahead, ever.** Every fitness/robustness eval is OOS through the `p1` S1 fit/apply firewall + CPCV
   purge/embargo (`cpcv.hpp:40`); walk-forward windows never train on their own test slice; the lockbox is sealed
   (§0.9) — no upstream read until S8.
6. **No snooping.** DSR/PBO deflation over the running trial count stays in the admission path (`fitness.cpp:192`,
   `deflated_sharpe.hpp:136`, N = `FitnessCfg.trial_count`); the lockbox is the ultimate anti-snooping control.
7. **Differential correctness on the new combinatorics.** `fast_nondominated_sort` ships an obviously-correct naive
   O(N²) dominance reference and a differential/property test (no genome in front *k* is dominated by any in front
   *>k*; a brute-force front assignment agrees).
8. **Additive, never mutative on the proven core.** No DSL/operator changes (that was S3). No existing
   fitness/gate/library semantics change except behind a default-off toggle whose off-state is the boundary pin. The
   `p1` scalar search remains reachable and byte-identical.
9. **No hot-path allocation in the inner loop.** NSGA-II scratch (front/rank/crowding buffers) is sized once per
   generation and reused; behavioral descriptors reuse `FitnessCore.oos_pnl` (`fitness.hpp:155`); the cold paths
   (archive growth, synthetic-panel generation, lockbox reservation) may allocate.

---

## §2 — File structure

### 2.1 atx-core / Pattern-B requests (recorded; engine ships the fallback)
- **L7 `nondominated_sort`** (S4.1): deterministic fast-non-dominated-sort + crowding-distance over a small objective
  matrix. Engine ships the fallback in `factory/pareto.hpp` (O(N²), canonical-id tie-break); recorded for promotion to
  `atx-core` L7. Accelerator, not blocker.

### 2.2 Engine files (this sprint builds/edits these)
| Unit | New | Edited |
|---|---|---|
| S4.0 | `plans/p2/sprint-4-progress.md` | — (marker only) |
| S4.1 | `factory/pareto.hpp` (`fast_nondominated_sort`, `crowding_distance`, `dominates`), `tests/factory_pareto_test.cpp`, `tests/factory_nsga_search_test.cpp` | `factory/fitness.hpp` (`FitnessReport.objectives[]`), `factory/search_driver.hpp` (`Scored.{objectives,rank,crowding}`, `SearchConfig.objective_mode`), `factory/search_driver.cpp` (the four §0.1 seams + initial-population seed via `generate.hpp` §0.6) |
| S4.2 | `factory/behavior.hpp` (signal-profile distance + behavioral archive), `tests/factory_behavior_test.cpp` | `factory/search_driver.{hpp,cpp}` (behavioral-novelty objective replaces canonical-hash `novelty_penalize`; archive threaded through generations) |
| S4.3 | `tests/factory_cost_aware_fitness_test.cpp` | `factory/fitness.{hpp,cpp}` (`cost_bps` objective via `cost_aware.hpp`/`capacity.hpp`), `factory/fitness.hpp` (`FitnessCfg.{target_aum, cost}`) |
| S4.4 | `eval/synthetic_alpha.hpp` (planted-signal panel), `eval/regime_slice.hpp` (vol-tercile labels + per-regime re-eval), `eval/lockbox.hpp` (PIT reservation + seal), `tests/eval_synthetic_recovery_test.cpp`, `tests/eval_regime_survival_test.cpp`, `tests/eval_lockbox_test.cpp` | `factory/research_driver.hpp` (optional robustness-gate hook) |
| S4.5 | `factory/robust_pipeline.hpp` (mine→multi-obj gate→admit→combine→book), `tests/robust_pipeline_e2e_test.cpp`, `bench/robust_search_bench.cpp` | `factory/research_driver.{hpp,cpp}` (route multi-objective + robustness + cost through `mine_into`) |

Tests/benches are auto-globbed (`tests/*_test.cpp`, `bench/*_bench.cpp`, CONFIGURE_DEPENDS) — **do not hand-edit
CMakeLists.txt**. New header-only `*.hpp` are included by the tests directly; any new `src/*.cpp` TU is picked up by the
existing engine glob — confirm at first build.

### 2.3 Tests (one per unit, `atx-engine/tests/<name>_test.cpp`)
`factory_pareto_test.cpp` + `factory_nsga_search_test.cpp` (S4.1), `factory_behavior_test.cpp` (S4.2),
`factory_cost_aware_fitness_test.cpp` (S4.3), `eval_synthetic_recovery_test.cpp` + `eval_regime_survival_test.cpp` +
`eval_lockbox_test.cpp` (S4.4), `robust_pipeline_e2e_test.cpp` (S4.5). Each: happy path, boundaries (single-genome
front, all-equal objectives, empty archive, window = 1, single regime), the load-bearing invariant proof (boundary pin
/ dominance correctness / recovery bar / seal), determinism (same seed ⇒ byte-identical; `DetPool` worker-invariance),
and `EXPECT_DEATH` for any new `ATX_ASSERT` precondition (the lockbox seal especially).

### 2.4 Ledger
`sprint-4-progress.md` — opened in S4.0 (marker), one row per unit with SHA + test count + the measured numbers
(boundary-pin digest match, recovery correlation, net-of-cost flip, mining throughput, Pareto-front evolution) on
close. Structure per [`../docs/sprint.md`](../docs/sprint.md) (copy `phase-4c-progress.md`).

---

## §3 — Cross-cutting gates (every coding unit) + handoff block

**Gates (a unit is done only when ALL hold):**
- TDD: failing GoogleTest first, for the right reason, then implement.
- **Boundary pin green** for every search-touching unit: the collapsed-objective digest equals the `p1`/pre-S4 search
  digest on the frozen fixture (S4.1–S4.3, S4.5).
- Determinism: seeded paths replay byte-identically; the digest is invariant across `{1,2,4,8}` `DetPool` workers; all
  ties resolve in canonical id order before any RNG draw.
- Differential correctness where applicable: `fast_nondominated_sort` vs the naive dominance reference.
- No look-ahead: fit/apply firewall + CPCV purge/embargo on every eval; the lockbox seal proven unreadable upstream.
- `/W4 /permissive- /WX` + `/fp:precise` clean; clang-format clean. **Do not run clang-tidy** (disabled repo-wide).
- Functions ≤ ~60 lines, exhaustive switches (no `default` over enums), `const`/`constexpr`/`noexcept`/`[[nodiscard]]`,
  `Result<T>` at boundaries, zero hot-path alloc.
- Existing suite stays green per unit (baseline at base SHA: **1284 pass / 2 disabled**).
- Commit `feat(s4-M): …` (or `fix`/`refactor`) with the
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` trailer. **Do not push.** Stage explicit
  pathspecs (never `git add -A`).

**Handoff block (paste into every coding sub-agent brief):**
> Worktree `C:\Users\natha\atx-wt\p2-s4` (branch `feat/p2-s4-genetic-search`, base `main @ 65f4ccb`). Build env wrapper
> `C:\Users\natha\atx-wt\p2-s4-env.cmd` (VS2022 + vcpkg + sccache + ninja; mirror `p2-s3-env.cmd`); run
> `cmd /c "call C:\Users\natha\atx-wt\p2-s4-env.cmd && <cmd>"`. Build: `cmake --build --preset dev --target
> atx-engine-tests`. Run a suite: `build\bin\atx-engine-tests.exe --gtest_filter=<Suite>.*`. C++ authority:
> `.agents/cpp/agent.md` (no UB, TDD, `Result<T>`, exhaustive switches, ≤60-line fns, zero hot-path alloc). Engine
> deltas: `.agents/atx-engine/agent.md`. **The scalar→vector swap touches exactly four seams** (`search_driver.cpp`
> :180/:228/:379/:397) — keep the seed axis, digest fold, and DetPool fan-out untouched. The boundary pin
> (collapse to `raw`, novelty/cost off ⇒ pre-S4 digest) is the regression anchor — write it FIRST. NaN out-of-set,
> never impute. No CMakeLists edits (auto-glob). Marker-commit pattern is mandatory: commit before stopping or work is
> lost.

---

## §4 — Architecture & algorithms (per-unit)

### 4.0 S4.0 — Marker + ledger + as-built recon
Create the worktree + `p2-s4-env.cmd` (off-OneDrive, sccache-warm — mirror S3). Open `sprint-4-progress.md`, freeze
scope (the six units below), record base SHA `65f4ccb`. The recon IS §0 of this plan — confirm the four scalar seams,
the cost hook, the generate.hpp gap, and the boundary-pin fixture still hold at base SHA, and that the baseline suite
is green (**1284/2-disabled**). Marker commit `docs(s4-0): open sprint-4 ledger + recon`. No production code.

### 4.1 S4.1 — Multi-objective NSGA-II Pareto selection
**New `factory/pareto.hpp`** (header-only, deterministic):
- `dominates(const std::span<const f64> a, b) -> bool`: `a` dominates `b` iff `a[k] >= b[k]` for all `k` AND `a[k] >
  b[k]` for some `k` (maximization; the cost component is pre-negated so higher == better). NaN objective ⇒ treated as
  −∞ (a NaN-objective genome is dominated by any finite one — never poisons a front).
- `fast_nondominated_sort(const ObjMatrix& O, span<const usize> canon_order) -> vector<u16> fronts`: Deb et al. 2002
  §III-A, O(N²): per genome compute domination count `n_p` and dominated set `S_p`; front 0 = `{p : n_p == 0}`; peel
  iteratively. **Determinism:** iterate genomes in `canon_order` (canonical id order), so equal-objective genomes land
  in a fixed front sequence.
- `crowding_distance(const ObjMatrix& O, span<const usize> front, span<const usize> canon_order) -> vector<f64>`:
  per objective, sort the front by that objective (canonical-id tie-break), boundary members → `+inf`, interior →
  Σ normalized neighbor gaps. Order-fixed.

**Edit `factory/fitness.hpp`:** `FitnessReport` gains `std::array<f64, kMaxObjectives> objectives` + `u8 n_objectives`,
filled `{wq, diversify, robust}` (S4.1) — a re-projection of existing fields (§0.2); `raw` retained.

**Edit `factory/search_driver.hpp`:** `Scored` gains `std::array<f64,K> objectives; u16 rank; f64 crowding;`.
`SearchConfig` gains `enum class ObjectiveMode { ScalarRaw, MultiObjective } objective_mode{MultiObjective};` and
`kMaxObjectives`.

**Edit `factory/search_driver.cpp` (the four §0.1 seams):**
- `:180` — cache `scored.objectives = rep->objectives` (and keep `fitness = rep->raw` for `ScalarRaw`/boundary pin).
- assign `rank`/`crowding` once per generation from `pareto.hpp` (a new `assign_pareto_ranks(scored, canon_order)`),
  BEFORE any reproduction RNG draw.
- `:397` `tournament_pick` — compare `(rank asc, crowding desc)` instead of `selection`.
- `:379` `raw_ordered_indices` → `pareto_ordered_indices`: sort by `(rank asc, crowding desc, canon_less)`; elitism
  keeps the first front (then crowding) up to `elites`.
- **Initial population (§0.6):** seed generation 0 from `generate_genome(gen_cfg, dsl, rng)` (grammar-typed,
  analyze-valid by construction) for any unfilled population slots, replacing/augmenting caller hand-seeds — the
  immigration path reuses the same generator for diversity injection.

**Boundary pin (write FIRST):** with `objective_mode = ScalarRaw`, `assign_pareto_ranks` collapses to a total order by
`raw` (single-objective dominance == numeric order), crowding unused, and the two compares reduce to the pre-S4
`scored[a].fitness > scored[b].fitness`. The regression test asserts the collapsed S4 search produces a
**byte-identical `ResearchReport.digest`** to the pre-S4 `SearchDriver` on a frozen seed+panel fixture.

**Differential / property tests (`factory_pareto_test.cpp`):** brute-force front membership vs `fast_nondominated_sort`
agree; no front-*k* genome dominated by any front-*>k* genome; crowding boundaries `+inf`; all-equal objectives → one
front, canonical-id-stable; single genome → front 0. `factory_nsga_search_test.cpp`: the boundary pin + a 4-worker
`DetPool` digest-invariance run + a 2-objective fixture where the Pareto front is genuinely > 1 (multi-objective is
non-vacuous: a high-`wq`/low-`diversify` and a low-`wq`/high-`diversify` genome co-occupy front 0).

### 4.2 S4.2 — Behavioral / phenotypic diversity
**New `factory/behavior.hpp`:**
- `behavioral_descriptor(const FitnessReport&|FitnessCore&) -> span<const f64>`: the candidate's OOS PnL profile,
  already materialized as `FitnessCore.oos_pnl` (`fitness.hpp:155`) — zero extra eval.
- `behavioral_distance(a, b) -> f64`: `1 − |corr(a, b)|` over the held-out profile (reuses the corr machinery the gate
  already uses; deterministic, NaN-complete-case). **Fallback fork (documented):** rank-IC profile distance, selected
  by a `behavior.hpp` enum if the PnL-corr metric proves too collinear with the marginal-corr `diversify` objective.
- `class BehavioralArchive`: a bounded (capacity `C`) FIFO of past-elite descriptors; `novelty(desc, population,
  archive, k) -> f64` = mean distance to the *k* nearest in `population ∪ archive`. Updated deterministically each
  generation: insert the current front in canonical id order, evict oldest past `C`. No RNG.

**Edit `search_driver`:** replace the canonical-hash `novelty_penalize` (`:216–228`, `canonical_distance` = Hamming/64,
`search_driver.hpp:177`) with a **behavioral-novelty objective** added to the NSGA-II vector — `{wq, diversify, robust,
behavioral_novelty}`. Keep marginal-corr `diversify` (redundancy *to the admitted pool*) AND behavioral novelty
(diversity *within the live population* + archive) as **distinct** objectives — they answer different questions
(don't-duplicate-the-library vs don't-collapse-the-search). Thread the archive through `SearchDriver` state, updated
between generations.

**Headline fixture (`factory_behavior_test.cpp`):** two genomes that hash-distantly but compute near-identical signals
(structural metric calls them "novel", behavioral calls them "redundant") and two that hash-similarly but behave
distinctly — assert the behavioral objective ranks them opposite to the canonical-hash metric (the spec's exit
criterion). Plus determinism + archive-eviction order tests.

### 4.3 S4.3 — Cost-aware mining fitness
**Edit `factory/fitness.{hpp,cpp}`** (reuse `cost::*` verbatim — §0.3):
- `FitnessCfg` gains `f64 target_aum{0.0}` (the recorded artifact knob; `0` == cost objective off, the boundary pin)
  and a `cost::CalibratedCost cost{}`.
- In `pool_aware_fitness`, after the candidate eval, derive per-name participation at `target_aum` from the candidate's
  positions + `adv20` (the `capacity.hpp` participation form `(AUM·|w_i|/price_i)/ADV_i`), the window σ, then
  `rt_cost_bps = book-aggregate over round_trip_cost_bps(cfg.cost, participation_i, sigma_i)` (`cost_aware.hpp:114`).
  `FitnessReport` gains `f64 cost_bps`.
- The S4.1 objective vector gains a 4th (or 5th with S4.2) component `−cost_bps` (negate so the maximizing dominance
  treats cheaper as better). When `target_aum == 0`, the component is omitted (vector shrinks) — the boundary pin
  chain holds.

**Net-of-cost fixture (`factory_cost_aware_fitness_test.cpp`):** a strong-but-expensive alpha (high `wq`, high
turnover → high `rt_cost_bps`) and a slightly-weaker cheap alpha; assert that with `target_aum > 0` the expensive one
is **Pareto-dominated** (or ranks below) the cheap one, whereas at `target_aum = 0` the ranking flips back to the
gross order — the "miner now rejects a strong-but-expensive alpha it previously admitted" demonstration. Plus: the
cost number matches a hand-computed `round_trip_cost_bps` for a known participation/σ.

### 4.4 S4.4 — Robustness battery
*(The heaviest unit; if it exceeds ~3 test files / runs long at kickoff, sub-split into **S4.4a** synthetic-recovery +
regime-survival and **S4.4b** lockbox, renumbering per `sprint.md`. Recorded in the ledger if taken.)*

**(a) Synthetic-alpha recovery — `eval/synthetic_alpha.hpp`.**
`generate_synthetic_panel(seed, PlantedSpec{signal_expr|factor, beta, noise_sigma}, dims) -> Panel`: build a panel
whose forward returns `= beta · planted_signal(t) + noise`, the planted signal being a known DSL expression's output
(or a latent factor) over deterministic noise. Harness `recovery_correlation(library, planted_signal) -> f64`:
correlation of the admitted survivors' OOS PnL with the planted signal. **Proof:** the GA (S4.1–S4.3) over the
synthetic panel admits survivors whose recovery correlation exceeds a bar `r*`, AND a matched **noise-only** panel
(`beta = 0`, same seed/dims) admits ~0 under the same deflated gate. The deepened, ground-truth version of `p1` S3's
noise-rejection. Deterministic, seeded.

**(b) Regime / walk-forward survival — `eval/regime_slice.hpp`.**
`regime_labels(panel, n_regimes=3) -> vector<u8>`: a deterministic **volatility-tercile partition** — per date, the
market (or cross-sectional median) realized vol over a trailing window → terciles {low, mid, high} (§0.5; HMM
`regime_posterior_at` argmax the recorded richer fork). `per_regime_sharpe(alpha_pnl, labels) -> array<f64,K>` +
`walk_forward_sharpe(alpha_pnl, n_windows)`. **Robustness verdict:** an alpha is robust iff its per-regime OOS Sharpe
stays above `min_regime_sharpe` in **every** regime AND across rolling walk-forward windows (not just full-sample OOS).
Admission gains a `RobustnessVerdict` the S4.5 gate consumes. **Fixture:** an alpha engineered to pass full-sample OOS
but collapse in the high-vol regime is demonstrably rejected; one robust across all three passes. Every slice is OOS
(fit/apply firewall); no window trains on its own test slice.

**(c) Lockbox reservation — `eval/lockbox.hpp`.**
`reserve_lockbox(panel, frac, embargo_len) -> SealedPanel`: carve the terminal contiguous most-recent `frac` (default
0.20) with an embargo gap (`cpcv` embargo width, §0.9); the returned `SealedPanel` exposes **only** `[0, lockbox_begin
− embargo_len)` to every accessor; any read into `[lockbox_begin, T)` returns `Err`/trips `ATX_ASSERT` (the seal).
S4.4 establishes the reservation + seal as a PIT boundary nothing upstream may read; **S8.2 opens it exactly once.**
The boundary is content-addressed by panel dates (no RNG). **Proof (`eval_lockbox_test.cpp`):** every upstream
stage (mine/fitness/gate/combine) over a `SealedPanel` runs clean; a direct read of the sealed region fails
(`EXPECT_DEATH` / `Result` error); the seal's identity round-trips (the same reservation reproduces byte-identically).

### 4.5 S4.5 — E2E robust signal-generation pipeline + bench + close
**New `factory/robust_pipeline.hpp`** — the deterministic orchestration:
**mine** (S4.1–S4.3 multi-objective search over the S3 substrate, generation-seeded from `generate.hpp`, on the
**non-lockbox** region of a `SealedPanel`) → **multi-objective gate** (Pareto admission + DSR deflation at the running
trial count + the S4.4 `RobustnessVerdict`: synthetic-recovery-style + out-of-regime survival + net-of-cost) →
**library admit** (`library::Library::admit`, `library.hpp:141` — dedup + corr-neighbor + lifecycle) → **combine**
(`combine::AlphaCombiner::fit`, `combiner.hpp:467`, default `ShrinkageMv`) → **multi-horizon book backtest** (p2 S1
optimizer, §0.10; `p1` S7 fallback). Threaded through `ResearchDriver::run` (extend `mine_into` to carry the objective
vector + robustness gate, or wrap it in a `RobustResearchDriver`).

**Proofs (`robust_pipeline_e2e_test.cpp`):**
- pure-noise panel grows the robust library by **~0 across seeds** (the deepened non-vacuousness);
- a real-signal/synthetic panel admits survivors passing synthetic-recovery + out-of-regime + cost;
- **byte-identical replay**: the full run's `ResearchReport.digest` + `manifest_version_id` reproduce across runs and
  across `{1,2,4,8}` workers;
- **boundary pin**: collapse (scalar raw, novelty/cost off, robustness gate in report-only mode) ⇒ the run digest
  matches the pre-S4 `ResearchDriver` run (the regression anchor at the pipeline level).

**Bench (`bench/robust_search_bench.cpp`, Debug upper-bound):** mining throughput under the multi-objective sort vs the
`p1` scalar baseline (the NSGA-II O(N²) overhead at `population = 16/32`); admitted-robust/hour; Pareto-front size over
generations (the front should grow then stabilize — the search exploring the frontier). Never cited as release figures.

**Close ceremony** (per `sprint.md` §"Sprint close"): lift residuals into the ROADMAP backlog; flip the S4 ROADMAP
status rows to ✅; bump `Last reviewed`; write "What S4 proves / Next sprint priorities"; write/update the `p2` user
reference if S4 shipped user-facing surface; merge the worktree into `main` (`--no-ff`); **do not push.**

---

## Exit criteria
Mirror the spec's exit criteria (verbatim authority is the spec). Concretely green when:
- NSGA-II multi-objective selection ships; the single-objective degenerate limit reduces **bit-for-bit** to `p1` S3's
  seeded loop (the boundary pin, digest-identical); determinism + worker-count-invariant digest preserved.
- Behavioral/phenotypic novelty (signal-space distance + archive) replaces/augments canonical-hash novelty; a fixture
  shows it prefers a behaviorally-distinct alpha the structural metric called "redundant" (and vice-versa).
- Cost-aware mining prices S6-calibrated impact at a `target_aum`; a fixture shows the miner now rejects a
  strong-but-expensive alpha it previously admitted (net-of-cost selection).
- Synthetic-alpha recovery proven (GA recovers a planted signal above a bar; matched noise admits ~0 under the same
  gate); regime/walk-forward survival gates on out-of-regime survival (a full-OOS-pass / out-of-regime-fail alpha is
  rejected); the lockbox is reserved + sealed (a pre-S8 read of the region fails).
- The E2E pipeline runs mine→gate→admit→combine→book deterministically; a pure-noise panel grows the robust library by
  ~0 across seeds; a seeded run replays byte-identically (`version_id` included).
- Bench recorded; `/W4 /permissive- /WX` + clang-format clean; test file per unit; baseline suite stays green.

## Invariants this sprint must prove
**Robustness** (the new `p2` invariant — proven by the battery, not asserted). **Determinism** (NSGA-II sort, crowding,
behavioral distance, archive, all tie-breaks resolve in canonical id order before any RNG; seed in the artifact; digest
worker-count-invariant). **No look-ahead** (fit/apply firewall + CPCV purge/embargo on every eval; lockbox sealed;
walk-forward never trains on its test slice). **No snooping** (DSR/PBO deflation in the admission path; lockbox the
terminal control). **Honest cost** (first-class mining objective, not post-hoc). **Differential correctness** (NSGA-II
sort vs naive O(N²) dominance reference).

## Dependencies
**Upstream:** S3 (the widened substrate + fixed/enabled `op_swap` + grammar-typed `generate.hpp`), `p1` S1
(`eval::{deflated_sharpe, cpcv, pbo}`, `AlphaMetrics`), `p1` S3 (`factory::{SearchDriver, fitness, OpCatalog, mutation,
crossover, genome}`), `p1` S4/S4b (`library::Library`, `ResearchDriver`, `combine::{AlphaStore, AlphaCombiner, gate,
metrics}`), `p1` S5 (`learn::hmm` regime posteriors — optional richer regime fork), `p1` S6 (`cost::{cost_aware,
capacity, calibration}`), `p1` S2 (`parallel::DetPool`). `p2` S1 (multi-horizon optimizer — landed; S4.5 book backtest).
**atx-core (Pattern-B edge):** **L7 `nondominated_sort`** (S4.1 — engine-side O(N²) deterministic fallback shipped;
lift recorded).

## Explicitly NOT in this sprint
- **No new operators / DSL changes** (all S3). S4 searches the S3-widened space; it does not extend the language.
- **No new signal families** (deep-learning alphas are S5; learned/ML alphas already exist, `p1` S5).
- **No opening the lockbox** — S4.4 *reserves and seals*; the single terminal open is S8.2. Touching it here voids the
  anti-snooping guarantee.
- **No live/optimal execution** — the S4.5 book backtest uses the `p0` `ExecutionSimulator` + S6 cost to *gate
  net-of-cost admission*; it does not schedule child orders (delegated to broker algorithms, `p2` anti-roadmap #2).
- **No distributed mining** — S4 mines on the `p1` S2 single-box `DetPool`; fanning across machines is S7.

## Baton → next
S4 produces a library that is not just *populated* but *proven robust* — survivors that recover known signal, survive
out-of-regime + walk-forward, and net positive after S6 cost, with a sealed lockbox waiting. That library feeds **S5/S6**
(new signal/data families, each now robustness-gated by this battery) and is the substrate for **S8**, which runs the
whole pipeline at scale and opens the lockbox exactly once — the terminal proof that the factory finds robust alpha.

## References
- Spec: [`sprint-4-genetic-search-robust-signal-pipeline.md`](sprint-4-genetic-search-robust-signal-pipeline.md).
- Research: [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md) §1/§5/§9;
  [`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md) §3.3;
  [`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
  §7.
- As-built (base `main @ 65f4ccb`): `factory::{fitness(.hpp:116/199),search_driver(.cpp:180/228/379/397),
  research_driver,op_catalog,generate(.hpp:203)}`, `combine::{gate(.hpp:67/83),metrics(.hpp:56),combiner(.hpp:467),
  store}`, `eval::{deflated_sharpe(.hpp:136),cpcv(.hpp:94),pbo(.hpp:298)}`, `cost::{cost_aware(.hpp:114/169),
  capacity(.hpp:30)}`, `library::Library(.hpp:141)`, `learn::hmm(.hpp:296)`, `parallel::DetPool(.hpp:93)`.

## §8 — Self-review (pre-implementation)
- [ ] No placeholder/TBD left unresolved (the spec's forks resolved here: behavioral metric = PnL-profile `1−|corr|`
  with rank-IC fallback; behavioral novelty ships as its own objective, not folded into `diversify`; regime slicer =
  vol-tercile partition with HMM fork; book backtest = p2 S1 optimizer).
- [ ] Every unit maps to a concrete new file or a pinned edit seam (the four §0.1 scalar seams; the §0.3 cost hook; the
  §0.6 generation seam).
- [ ] The boundary pin is concrete and written FIRST (collapse to `raw` ⇒ pre-S4 digest, per unit and at the pipeline).
- [ ] Determinism is rooted (canonical-id tie-break before RNG; DetPool worker-invariance test per stochastic unit).
- [ ] Cost reuses `cost::*` verbatim (§0.3) — no new cost model; `target_aum=0` is the off-switch.
- [ ] The lockbox seal is enforced (read before S8 fails) — not just documented.
- [ ] NSGA-II ships an obviously-correct naive reference + differential (no front-k dominated by front->k).
- [ ] No DSL/operator changes; no CMakeLists edits (auto-glob); no push.
- [ ] S4.4 sub-split contingency (S4.4a/S4.4b) recorded if the battery balloons past the unit budget.
```
