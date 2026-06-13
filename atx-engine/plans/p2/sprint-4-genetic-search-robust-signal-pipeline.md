# Sprint S4 — Genetic Search & Robust Signal Pipeline (sprint spec)

**Status:** ⏳ proposed (not open). Depends on **S3** (the widened substrate it searches), **p1 S1** (the deflated gate
+ CPCV + PerfMetrics), **p1 S3/S4b** (`factory::{SearchDriver, fitness, ResearchDriver}` + `library::Library`),
**p1 S6** (calibrated cost). The spine's **second half** and `p2`'s headline deliverable.
**Roadmap:** [`ROADMAP.md`](ROADMAP.md) · **Discipline:** [`../docs/sprint.md`](../docs/sprint.md)
**Grounded in:** [`../../research/worldquant-systems-deep-dive.md`](../../research/worldquant-systems-deep-dive.md)
§1/§5/§9 (formulaic mining, correlation = marginal contribution, the factory) ·
[`../../research/alpha-expression-dsl-deep-dive.md`](../../research/alpha-expression-dsl-deep-dive.md) §3.3 (GP/RL
expression-tree miners) ·
[`../../research/renaissance-technologies-systems-deep-dive.md`](../../research/renaissance-technologies-systems-deep-dive.md)
§7 (ruthless OOS discipline, "trust slowly"). Method references: Deb et al. 2002 (NSGA-II), Lehman-Stanley 2011
(novelty search), Bailey/López de Prado 2014/2017 (deflated Sharpe, the lockbox/holdout discipline).

---

## Why this sprint

S3 widened the *language*. S4 makes the *search* smart enough to exploit it, and — the headline — **proves the alpha it
admits is actually robust.** This is the sprint `p2` exists for.

The as-built `p1` genetic miner (`factory::SearchDriver` + `pool_aware_fitness`) is a clean, seeded, deterministic
evolutionary loop — but it is a *shallow optimizer of a shallow objective*, and it never proved robustness:

1. **The objective is a scalar collapse.** Fitness is `raw = wq × diversify × robust` — three real signals (WorldQuant
   fitness, marginal-correlation diversification, sub-universe robustness) **multiplied into one number** the search
   hill-climbs. A multiplicative collapse hides trade-offs: a wildly-diversifying weak alpha and a strong-but-redundant
   one can score identically, and the search can't see the Pareto frontier between objectives. This is exactly the
   **multi-objective** problem NSGA-II solves.

2. **Novelty is structural, not behavioral.** Diversity pressure is **canonical-hash distance** (Hamming distance of
   structural hashes). Two expressions that compute nearly the same *signal* but hash differently look "novel"; two
   that hash similarly but behave differently look "redundant." The population can collapse onto one behavioral motif
   while looking structurally diverse. Real novelty is **phenotypic** — distance in *signal/return space*.

3. **Cost is a turnover-floor proxy.** Mining fitness prices turnover as a `max(turnover, 0.125)` floor inside the WQ
   formula — not the **S6-calibrated impact at a target AUM**. An alpha that looks great gross can be net-negative at
   scale; the miner never sees that until much later. Cost must enter the **mining** objective.

4. **Robustness was never proven, only non-vacuousness.** `p1` S3 showed a pure-noise panel admits ~0 vs a real-signal
   panel under the deflated gate — necessary, but not sufficient. It never showed an admitted alpha **survives
   out-of-regime, walk-forward, a sealed holdout, or realistic cost**. That gap is the whole point of `p2`.

S4 fixes both halves. The **search** becomes multi-objective (NSGA-II Pareto selection over {return, diversification,
robustness, cost}), behaviorally diverse (signal-return-profile novelty + a behavioral archive), and cost-aware
(S6-calibrated κ in the objective). The **pipeline** then *proves* robustness with the full battery — synthetic-alpha
recovery, regime/walk-forward survival, and a sealed final lockbox — culminating in the E2E mine → multi-objective
gate → library admit → combine → multi-horizon book backtest that demonstrates the populated library holds **real,
survivable, net-of-cost** alpha.

The non-negotiable: in the degenerate limit (collapse the Pareto front to the lone `raw` objective, novelty off, cost
off), S4's search must reduce **bit-for-bit** to `p1` S3's seeded loop — the regression anchor against the proven
miner.

---

## Scope — units

### S4.0 — Marker + ledger + as-built recon
Open `sprint-4-progress.md`, freeze scope, base SHA. As-built recon against `factory::{search_driver, fitness,
research_driver, op_catalog}`: map the current `pool_aware_fitness` (`wq`/`diversify`/`robust`/`raw`/`dsr`), the
`SearchDriver` generation loop (select → reproduce → elitism → novelty-penalize), the seed axis
`(master, run, gen, idx)`, and the `library::admit` path the survivors flow into. Identify the exact seams where the
scalar `raw` is consumed (elitism, selection) — those become the multi-objective swap points.

### S4.1 — Multi-objective Pareto selection (NSGA-II)
Replace the scalar `raw = wq × diversify × robust` with a **vector objective** `(return, diversification, robustness,
cost)` and a **deterministic NSGA-II** selection: fast non-dominated sorting into Pareto fronts + crowding-distance
within a front (Deb et al. 2002). Elitism keeps the first front; selection is binary-tournament on (rank, crowding).
**Determinism is load-bearing:** non-dominated sorting and crowding-distance ties resolve in **canonical id order**
(established before any RNG draw, the `p1` F1/F2 discipline); the digest stays worker-count-invariant. **Boundary pin:**
with the objective vector collapsed to the single `raw` component (novelty/cost units off), selection reduces
bit-for-bit to `p1` S3's tournament. *atx-core Pattern-B edge: L7 `nondominated_sort`; engine-side O(N²) deterministic
sort fallback (the per-generation N is small).*

### S4.2 — Behavioral / phenotypic diversity
Add a **behavioral novelty** objective/pressure measured in **signal space**, not structural-hash space: distance
between a candidate's signal-return profile and its nearest neighbors (and a **behavioral archive** of past elites, so
the population can't cycle). Distance metric provisional: signal-return-profile **correlation** over a held-out window
(cheap, deterministic, reuses the corr machinery; rank-IC profile is the documented fallback fork). Feeds the
diversification objective in S4.1's vector (or stands as its own front dimension — decide at kickoff). Replaces /
augments the as-built canonical-hash `novelty_penalize`. Deterministic, seeded.

### S4.3 — Cost-aware mining fitness
Route the **S6-calibrated cost** into the mining objective: the `cost` component of the S4.1 objective vector is the
realized **impact + turnover cost at a target AUM** (the S6 √-impact temp/perm split + capacity curve), not the
`max(turnover, 0.125)` floor. An alpha's mining score is **net-of-cost** — the factory now prefers a slightly-weaker
alpha that trades cheaply over a strong one that decays under its own impact. The target AUM is a recorded config knob
(part of the artifact). Reuses `cost::*` and the S6 capacity machinery verbatim (no new cost model).

### S4.4 — Robustness battery
The three out-of-sample proofs, each a reusable harness:
- **Synthetic-alpha recovery.** Generate synthetic panels with a *planted* known alpha (a controlled signal + noise);
  prove the GA **recovers** it (the admitted survivors correlate with the planted signal above a bar) **and** that a
  matched pure-noise panel admits ~0 under the same deflated gate. The deepened, controlled version of `p1` S3's
  non-vacuousness check — now with a *known ground truth* to recover, not just noise to reject.
- **Regime / walk-forward survival.** Re-evaluate admitted alphas across **regimes** (bull / bear / high-vol — sliced
  by a regime label, reusing the `p1` S5 HMM or a volatility partition) and across **rolling walk-forward windows**;
  an alpha is "robust" only if it survives **out-of-regime** and across windows, not just on full-sample OOS. Admission
  gains a robustness gate: out-of-regime survivors only.
- **Lockbox reservation.** Carve a **never-touched terminal holdout** (a single contiguous most-recent N% of the
  panel, embargo width inherited from `p1` S1 CPCV) and **seal it** — the entire factory/library/search is blind to it
  until the one S8 terminal open. S4.4 establishes the reservation discipline + the seal (a PIT boundary nothing
  upstream may read); S8 opens it exactly once.

### S4.5 — E2E robust signal-generation pipeline + bench + close
The end-to-end proof: **mine** (S4.1–S4.3 multi-objective search over the S3 substrate) → **multi-objective gate**
(Pareto admission + deflation + the S4.4 robustness gate) → **library admit** (`p1` S4 persistent store, dedup,
corr-neighbor, lifecycle) → **combine** (`combine::AlphaCombiner`) → **multi-horizon book backtest** (`p2` S1
optimizer, if landed; else the `p1` S7 multi-period book) — fully deterministic and replayable. The deliverable is the
demonstration that the populated library, optimized into a book, holds **robust, net-of-cost** alpha: a pure-noise run
grows the robust library by ~0 across seeds, a real-signal run admits survivors that pass synthetic-recovery +
out-of-regime + cost, and the whole run replays byte-identically (manifest `version_id` included). **Bench:** mining
throughput under the multi-objective sort (vs the `p1` scalar baseline), admitted-robust/hour, the Pareto-front size
over generations. **Close** ceremony.

---

## Exit criteria

- Multi-objective NSGA-II selection ships; in the single-objective degenerate limit it reduces **bit-for-bit** to
  `p1` S3's seeded loop (the boundary pin); determinism + worker-count-invariant digest preserved.
- Behavioral/phenotypic novelty (signal-space distance + archive) replaces/augments canonical-hash novelty; a fixture
  shows it prefers a behaviorally-distinct alpha that the structural metric called "redundant" (and vice-versa).
- Cost-aware mining fitness prices S6-calibrated impact at a target AUM; a fixture shows the miner now rejects a
  strong-but-expensive alpha it previously admitted (net-of-cost selection demonstrated).
- **Synthetic-alpha recovery** proven: the GA recovers a planted signal above a bar, and a matched noise panel admits
  ~0 under the same gate (ground-truth recovery, not just noise rejection).
- **Regime/walk-forward survival** proven: admission gates on out-of-regime survival; an alpha that passes full-sample
  OOS but fails out-of-regime is demonstrably rejected.
- **Lockbox** reserved + sealed: a PIT-enforced terminal holdout no upstream stage can read (proven by an assertion
  that any read of the lockbox region before the S8 open fails); the seal carries to S8.
- The E2E pipeline runs mine→gate→admit→combine→book deterministically; a pure-noise panel grows the robust library by
  ~0 across seeds; a seeded run replays byte-identically (`version_id` included).
- Bench recorded (mining throughput under multi-objective sort, admitted-robust/hour, Pareto-front evolution);
  `/W4 /permissive- /WX`, clang-tidy + clang-format clean; test file per unit.

## Invariants this sprint must prove

- **Robustness (the new `p2` invariant).** No alpha is trusted on full-sample OOS alone — it must pass
  synthetic-recovery + out-of-regime/walk-forward + net-of-cost. Proven by the battery, not asserted.
- **Determinism.** NSGA-II sort, crowding distance, behavioral distance, and all tie-breaks resolve in canonical id
  order before any RNG draw; the seed is in the artifact; the digest is worker-count-invariant (the `p1` F1/F2/S2
  discipline carried forward).
- **No look-ahead.** Every fitness/robustness evaluation is OOS through the `p1` S1 fit/apply firewall + CPCV
  purge/embargo; the lockbox is sealed (no upstream read until S8); walk-forward windows never train on their own
  test slice.
- **No snooping.** Deflation (DSR/PBO over the running trial count) stays in the admission path; the lockbox is the
  ultimate anti-snooping control (one terminal open, never tuned against).
- **Honest cost.** Cost is a first-class *mining* objective (S4.3), not a post-hoc deduction.
- **Differential correctness.** The NSGA-II sort ships an obviously-correct reference (naive O(N²) dominance) and a
  differential test against any optimized variant.

## Dependencies

- **Upstream:** S3 (the widened substrate + fixed `op_swap` + grammar-typed generation), p1 S1 (`eval::{deflated_sharpe,
  cpcv, pbo}`, `PerfMetrics`), p1 S3 (`factory::{SearchDriver, fitness, OpCatalog, mutation, crossover}`), p1 S4/S4b
  (`library::Library`, `ResearchDriver`), p1 S5 (HMM regime labels for the regime slices — or a volatility partition),
  p1 S6 (`cost::*`, capacity curves), p1 S2 (`parallel::DetPool` for batch eval). Optionally `p2` S1 (multi-horizon
  optimizer) for the book backtest in S4.5 — falls back to `p1` S7's multi-period book if S1 hasn't landed.
- **atx-core (Pattern B edge):** **L7 `nondominated_sort`** (S4.1 — engine-side O(N²) deterministic fallback).

## Explicitly NOT in this sprint

- **No new operators / DSL changes.** That is all S3. S4 searches the S3-widened space; it does not extend the
  language.
- **No new signal families.** Deep-learning alphas are S5 (and must pass *this* sprint's robustness battery when they
  land); learned/ML alphas already exist (p1 S5).
- **No opening the lockbox.** S4.4 *reserves and seals* it; the single terminal open is S8.2. Touching it here would
  void the anti-snooping guarantee.
- **No live/optimal execution.** The book backtest in S4.5 uses the `p0` `ExecutionSimulator` + S6-calibrated cost to
  *gate net-of-cost admission* — it does not schedule child orders or model microstructure (delegated to broker
  execution algorithms, `p2` anti-roadmap #2).
- **No distributed mining.** S4 mines on the `p1` S2 single-box pool; fanning the search across machines is S7
  (the digest-invariant substrate S4's determinism depends on).

## Baton → next

S4 produces a library that is not just *populated* but *proven robust* — survivors that recover known signal, survive
out-of-regime and walk-forward, and net positive after S6 cost, with a sealed lockbox waiting. That library is the
input to **S5/S6** (new signal/data families, each now robustness-gated by S4's battery) and the substrate for **S8**,
which runs the whole pipeline at scale and opens the lockbox exactly once — the terminal proof that the factory finds
robust alpha.
