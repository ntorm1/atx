# Module p8 — Mega-Alpha Activation & Book Assembly

**Last reviewed:** 2026-07-02
**Started:** planned; no sprint open yet
**Source:** post-p7 engine survey (2026-07-02, three-agent recon: engine-maturity map, p7
landed-vs-deferred audit, research-corpus mining) + the measured-results docs
(`atx-impl/research/2026-06-27-tradeable-alpha-results.md`,
`2026-06-22-pipeline-degenerate-alpha-failure-analysis.md`,
`atx-engine/research/2026-06-21-phaseD-conviction-breadth-oos-findings.md`) + master ROADMAP
CIO directive ("stop adding capability until the benchmark runs; wire what exists").
**Goal:** convert the *feature-complete but orphaned* engine into a *validated production
mega-alpha book* — wire the built-and-tested risk-model, meta-book, nonlinear/regime combiner,
and cost/capacity machinery into the runnable `atx-impl` pipeline, fix the correctness bugs that
make every net/capacity number dishonest, and produce the first end-to-end deflation-surviving
book-level scorecard on real data.

---

## The one fact that defines this module

**The machine is built. Its most valuable subsystems are not plugged in.** A three-agent survey
of the post-p7 tree (2026-07-02) found a consistent pattern across risk, combination, and
portfolio construction: the sophisticated capability the north star needs *already exists in the
`atx-engine` library, tested and green*, but is **orphaned from the runnable `atx-impl`
pipeline**. Grepping `atx-impl/src` for the load-bearing symbols returns **zero hits**:

| Built + tested in `atx-engine` (evidence) | What `atx-impl` actually runs today (evidence) |
|---|---|
| **S8 Barra factor covariance** — `FactorModelBuilder`, robust Huber-IRLS cross-sectional regression, EWMA (split vol/corr half-lives), Newey-West, Menchero-Wang-Orr MC eigenfactor de-biasing, VRA, APCA statistical factors, Ledoit-Wolf, MP/RMT eigen-clip, Woodbury inverse; ~1133 tests (`risk/factor_model.hpp`, `risk/stat_factor_model.hpp`, `risk/shrinkage.hpp`, `risk/eigen_adjust.hpp`, `p1/sprint-8a/8b-progress.md`) | **A diagonal per-name variance** — `diagonal_risk_model(research)` (`atx-impl/src/stage_optimize.cpp:202`); the combiner fits weights on a **raw MLE covariance** `mle_covariance` (`stage_combine.cpp:755`). The optimizer cannot see cross-sectional correlation at all. |
| **`fund::MetaAllocator`** — inverse-vol / ERC (Spinu log-barrier) / **HRP** risk-budgeting + fractional-Kelly vol-target; **`fund::MetaBook`** — two-pass sleeve netting, trailing cross-sleeve risk budget, Euler attribution, Meucci effective-bets (`fund/meta_allocator.hpp`, `fund/meta_book.hpp`; 1411 impl lines, 100+ tests) | **No sleeve/meta-book layer** — a single linear `AlphaCombiner` over one combined panel. `MetaAllocator`/`MetaBook` appear nowhere in `atx-impl/src`. |
| **`learn::StackingCombiner`** — GBT + elastic-net alpha-of-alphas on positions, regime-conditional, deflation-gated vs a linear base (`learn/ensemble.hpp`, `learn/gbt.hpp` 595 lines, `learn/hmm.hpp` log-space Baum-Welch PIT posterior); 200+ tests | **A single linear blend** — `ShrinkageMv/Equal/Rank/IC/BoundedRegression` (`stage_combine.cpp`). `StackingCombiner`/`fit_stack`/HMM `regime_posterior_at` appear nowhere in `atx-impl/src`. |
| **`risk::dead_factor`** (Kakushadze-Yu holdings-overlap endogenous crowding factors), **`cost::temp_perm`** (√-law temp+perm impact), **`risk::garleanu_pedersen`** (aim-portfolio partial-trade), **`risk::capacity`** curve, **`alpha::cluster_panel`** (RMT-cleaned data-driven sectors) | none of the above wired into the runnable book. Impact is charged only as **post-hoc report telemetry**, never in the selection objective; `total_pnl_cost = 0.000` in the last measured run — **every reported Sharpe is gross**. |

This is corroborated by the measured failures — Phase-D: "the combine/eval/telemetry layer is
solid… the bottleneck is alpha-generation quality plus **cost/capacity realism**"; turnover
~74%/week; `oos_pbo = 0.79`; 30 admitted alphas collapse to `N_eff = 8.76` (crowding). And by the
master ROADMAP CIO directive: *"stop adding capability until the benchmark runs — the machine is
feature-complete; the binding constraint is real-data evidence, not more layers."*

**p8 is the realization of that directive, not a violation of it.** It adds **zero** new signal
families and **zero** new search capability. It plugs in what is already built, fixes the
correctness bugs that corrupt the numbers, and runs the honest scorecard.

---

## Companion docs

| Doc | Covers |
|---|---|
| `sprint-1-risk-model-covariance.md` | S1 — retire the diagonal/MLE covariance; wire the S8 Barra factor model + dead-alpha crowding factors + factor/industry neutralization into combine & optimize |
| `sprint-2-mega-alpha-metabook.md` | S2 — wire `fund::MetaAllocator` (HRP/ERC + fractional-Kelly) + `fund::MetaBook` (two-pass sleeve netting, cross-sleeve risk, Euler attribution) as the mega-alpha assembly stage |
| `sprint-3-nonlinear-regime-combine.md` | S3 — wire `learn::StackingCombiner` (GBT/elastic-net alpha-of-alphas) + PIT HMM regime posterior → `RegimeCombiner`; deflation-gated vs the linear base |
| `sprint-4-cost-capacity-correctness.md` | S4 — fix the P0 cost/capacity/execution correctness bugs; charge √-law temp+perm impact in the *selection* objective; first-class capacity curve; Garleanu-Pedersen partial-trade turnover control |
| `sprint-5-wire-deflate-validate.md` | S5 — thread all new knobs + the deferred p7-S7 CLI hub; make DSR/PBO/cumulative-N deflation *blocking* + wire `GateDeflation` into `library::verdict_for`; robustness battery; V1 mega-book scorecard |
| `TRACKER.md` | live per-sprint status (created at first kickoff) |

**Pending (created at sprint kickoff/close):** per-sprint `sprint-N-progress.md` ledgers; `p8.md`
user reference at module close.

**Sibling modules:** **p7** — Production Alpha Book + High-Performance DSL Pipeline,
[../p7/ROADMAP.md](../p7/ROADMAP.md) (direct predecessor). **p6** — Tradeable-Alpha Uplift,
[../p6/ROADMAP.md](../p6/ROADMAP.md). Master cross-module truth: [../ROADMAP.md](../ROADMAP.md).

---

## Strategic positioning

p8 claims the identity **"activation, not construction."** Where p1/p2 *built* the vendor-grade
covariance, the ML/stacking combiner, and the fund meta-book, and p6/p7 *hardened the discovery
loop and the honest-selection gates*, p8's entire job is to **connect the built pieces into one
runnable production book and measure it honestly.** Every unit is either (a) wiring tested engine
code into `atx-impl`, or (b) a correctness fix that makes the resulting numbers trustworthy. The
only genuinely-new code is thin adapters, new deploy stages, and the robustness battery.

| Dimension | p1/p2 (build) | p6/p7 (harden discovery) | **p8 target (activate + validate)** |
|---|---|---|---|
| Risk model in the book | built (S8), orphaned | untouched | **factor covariance live in combine+optimize** |
| Combination | linear combiner built; stacking/meta-book built, orphaned | conviction/Kelly wired (p7-S5) | **HRP/ERC meta-book + nonlinear/regime combiner live** |
| Cost/capacity | impact model + capacity curve built | cost-aware *gates* (p6-S4), capacity vector (p7-S4) | **impact in the selection objective; correct capacity; net-of-cost is the number** |
| Honesty | DSR/PBO/CPCV built | deflation *gates* built (p7-S1) | **deflation blocking in selection + library; robustness battery; V1 scorecard** |
| Deliverable | green library tests | green dev-panel smoke | **a real book-level net-of-cost, deflation-surviving, capacity-bounded scorecard** |

When scope-creep argues, this table governs: **if a proposal is a new signal, a new search
operator, or a new estimator, it does not ship in p8.** p8 only wires, corrects, and validates
what already exists.

---

## Phase 0 — Foundation (what p6/p7 shipped; the orphan/correctness gaps p8 closes)

**Solid and merged (p6 + p7 S1–S6, on `main`):**
- p6 — eval/VM perf, factory admission refactor, turnover-aware search + seed-elitism, cost-aware
  *gates* (`rt_cost_bps`/`min_holding_days`), panel augmentation (Alpha101 fields), sign-correct
  downstream book, CLI threading + capstone harness.
- p7 — deflation gates capability (`GateDeflation`: `min_dsr`/`max_pbo`/split-stable, `914ae7f`),
  breadth families (FINRA short-interest / IV-surface / liquidity, `60547ff`), eval-VM online
  kernels + cross-instrument parallelism (`4a2113b`), EMA-decay WeightPolicy + per-alpha capacity
  vector + `CapacityScorecard` (`a149156`), fractional-Kelly + conviction telemetry (`d95ce04`),
  incremental panel append + provenance (`4c795fb`). Determinism contract holds (inert defaults;
  byte-identical no-flag path; `oracle.hpp` frozen).

**Built + tested but ORPHANED from the runnable pipeline (the p8 wiring surface):** S8 Barra factor
covariance + Ledoit-Wolf/RMT cleaning; `dead_factor` endogenous crowding; `fund::MetaAllocator`
(HRP/ERC) + `fund::MetaBook`; `learn::StackingCombiner` + HMM PIT posterior; `cost::temp_perm`
impact + `risk::garleanu_pedersen` + `alpha::cluster_panel` RMT sectors. (Grep of `atx-impl/src` →
zero hits for every one of these.)

**Correctness gaps that corrupt the scorecard (each maps to a sprint):**

| Gap (evidence) | Closes in |
|---|---|
| Optimizer + combiner run on a **diagonal / raw-MLE** covariance (`stage_optimize.cpp:202`, `stage_combine.cpp:755`); the S8 factor model is unreachable → book cannot de-lever crowded bets, control tracking error, or size for capacity | **S1** |
| No fund-level sleeve allocation or netting (single MVO over one combined panel); `MetaAllocator`/`MetaBook` orphaned → no robust portfolio-of-books, no cross-sleeve turnover netting | **S2** |
| Combination is a single **linear** blend; `StackingCombiner` + HMM regime posterior orphaned → no nonlinear interaction capture, no regime adaptation | **S3** |
| Impact charged into the NSGA cost objective but **not the ScalarRaw `raw` selection scalar** (`factory/fitness.cpp:416`, `search_driver.cpp:1308`; default `target_aum=0` off) → the shipped ranking is gross (`total_pnl_cost=0.000`); capacity participation **unit bug copied to 3 sites** (`risk/capacity.hpp:238` + `factory/fitness.cpp:518` + `stage_combine.cpp:289,301`, shares ÷ dollar-ADV); cap-clip-renorm **breaks dollar-neutrality** (no post-demean: `risk/optimizer.hpp:389`, `loop/weight_policy.hpp:319`); limit orders can **fill through their limit** (`exec/execution_sim.hpp:292,374`); absent instruments retain **stale executable volume** (`loop/market.hpp:113`); borrow **built but never called** (`cost/borrow.hpp:142`, zero call sites) | **S4** |
| Deflation gates are a **library-only capability not wired into `library::verdict_for`** (`library.hpp:408`; p7-S1 carry-forward — dead code on every live caller); main's cascade fold is **byte-safe-looser-with-N**, so deflation is **not blocking in the NSGA selection column** (`kObjDeflation=6`, `fitness.hpp:353`) and **PBO is advisory, not un-admitting**; no automated robustness battery; **V1 scorecard never run** | **S5** |

---

## The sprints (disjoint file ownership ⇒ parallel agent streams)

S1–S4 own **disjoint** file sets and dispatch in parallel waves; S5 owns the shared CLI hub +
library seam + validation harness and runs **last** (mirrors the p6/p7 S1–S6 ∥ / S7-last
contract). `alpha/oracle.hpp` is untouchable by every sprint. The four hub files —
`atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}` — are **reserved for S5**;
feature sprints expose new behavior via engine config-struct fields (inert defaults) and their own
new/owned stage files, exactly as p6/p7 did.

| # | Sprint | Goal metric (small-test gate) | Owns (exclusive) |
|---|---|---|---|
| **S1** | Risk-model covariance spine | optimizer/combiner consume a factor covariance; dead-alpha factors raise variance on crowded directions; group-neutral reachable; goldens byte-identical off-path | `risk/factor_model.{hpp,cpp}` (builder adapter), `risk/{stat_factor_model,shrinkage,eigen_adjust,specific_risk,psd_repair,dead_factor,exposures}.hpp` (wiring), `data/factor_model_artifact.hpp`; NEW `atx-impl/src/stage_riskmodel.{cpp,hpp}` + `atx-impl/src/stage_optimize.cpp` (covariance-source swap behind inert `RiskModelConfig`) + `diag_risk.hpp` (route) + tests |
| **S2** | Mega-alpha meta-book | ≥2 sleeves compose into one fund book; HRP/ERC weights sum-to-1 positive; cross-sleeve netting reduces gross turnover vs naive concat; Euler attribution sums to fund total | `fund/{meta_book,meta_allocator,cross_sleeve_risk,netting,sleeve}.hpp` (wiring adapters), `combine/combined_source.hpp`; NEW `atx-impl/src/stage_metabook.{cpp,hpp}` + `atx-impl/tests/…` |
| **S3** | Nonlinear & regime-aware combination | stacking beats linear base OOS-after-DSR on an interaction fixture or is not admitted; regime posterior modulates weights PIT-safely; inert default = today's linear blend | `learn/{ensemble,gbt,elastic_net,hmm,learned_source}.hpp` (wiring), `combine/regime_combiner.hpp`, `combine/combiner.hpp` (new `CombineMethod::Stack`/`RegimeStack` enumerators, appended); `atx-impl/src/stage_combine.cpp` (method dispatch) + `atx-impl/src/stage_regime.cpp` + tests |
| **S4** | Cost, capacity & execution correctness | capacity participation dimensionally correct; dollar-neutral book stays net≈0 after cap-renorm; limit never fills through; impact in selection flips a high-turnover winner to net-negative rejection; capacity curve monotone | `risk/{capacity,optimizer,garleanu_pedersen}.hpp`, `cost/{temp_perm,cost_aware,capacity,calibration,borrow}.hpp`, `loop/{market,weight_policy,backtest_loop}.hpp`, `exec/execution_sim.hpp`, `factory/fitness.cpp` (impact-in-selection objective only); `atx-impl/src/stage_report.cpp` (capacity curve) + tests |
| **S5** | Wire, deflate & validate | new knobs round-trip; deflation blocks in selection + library; robustness battery rejects a noise-control artifact; dev-panel smoke green; V1 scorecard produced | `atx-impl/src/{config.hpp,config.cpp,stage_discover.cpp,stage_run.cpp}` (hub), `library/library.hpp` (`GateDeflation` → `verdict_for`), `factory/factory.cpp` + `factory/fitness.hpp` (cumulative-N in selection), NEW `eval/robustness_battery.hpp`, NEW `scripts/build-megaalpha-book.ps1` + research doc |

> **Ownership reconciliation (binding):** both S1 and S4 touch the risk module but **disjoint
> files** — S1 owns `factor_model`/`stat_factor_model`/`dead_factor` (what covariance), S4 owns
> `capacity`/`optimizer`/`garleanu_pedersen` (renorm correctness, impact, capacity). `stage_optimize.cpp`
> is **S1-owned** (covariance-source swap); the optimizer *renorm* fix lives in `risk/optimizer.hpp`
> (S4, engine header) — different file from the atx-impl stage. `stage_combine.cpp` is **S3-owned**
> for p8 (method dispatch); S1's combiner-covariance change is exposed as a `combine`-consumed
> `FactorModelArtifact` the combiner reads, not a `stage_combine` edit. `factory/fitness.cpp` is
> **S4-owned** (impact-in-selection); `factory/factory.cpp` + `factory/fitness.hpp` are **S5-owned**
> (cumulative-N selection deflation). No two sprints edit the same file.
>
> **Two cross-sprint seams the authors surfaced (binding):** (1) the capacity-participation unit
> bug (Phase-0 S4 row) is a **3-way copy** — `risk/capacity.hpp` + `factory/fitness.cpp` (both
> S4-owned) **and** `atx-impl/src/stage_combine.cpp:~289,301` (**S3-owned**). S4 fixes the two
> S4-owned sites and ships the exact fix + shared fixture as a **seam for S3** to land the third
> when it touches `stage_combine.cpp`. (2) `loop/backtest_loop.hpp` carries S4's one-line
> borrow-accrual settle-sequence wire — added to S4's owned set above.
>
> **Branch discipline (binding):** the checked-out `feat/warehouse-parity` worktree is **stale vs
> `main`** for at least `combine/gate.hpp` and `factory/factory.cpp` (main already carries p7-S1's
> `GateDeflation` + the cumulative-N cascade fold; the worktree does not). Every p8 sprint
> **branches from `main`**; the sprint plans' file:line anchors are verified against `main` and
> re-confirmed at kickoff — never against the warehouse-parity worktree.

Each sprint file decomposes into 4–7 ledger units (`SN-0..SN-k`) per
[../docs/sprint.md](../docs/sprint.md), with per-unit marker/commit discipline and the
implementation-quality handoff block in every subagent brief.

---

## Shared determinism contract (every sprint)

p8 inherits the p6/p7 **opt-in / default-byte-identical** contract. Every output-changing
capability is gated behind a new engine-config field defaulting to today's value; the pinned
goldens stay UNCHANGED on the default path (`NsgaSearch.ScalarRaw_ReproducesGoldenDigest`,
`FactoryOos.MineIntoOffPathDigestUnchanged`, OOS goldens; `oracle.hpp` untouched). Each opt-in
ships four test classes — (a) off-path byte-identity, (b) on-path RED→GREEN, (c) twice-run,
(d) seq==parallel where an admission path is touched. S5's build profile turns the opt-ins on as
an explicit non-default profile, never a golden re-baseline. **Correctness fixes** (S4's P0 bugs)
are the one exception: they change the number *because the old number was wrong* — each ships with
(a) a RED test that encodes the correct behavior and fails on the buggy code, (b) a documented
before/after on a hand-built fixture where the right answer is known by construction, and (c) a
note in the ledger that the golden which encoded the bug is re-baselined *with the bug-fix commit
SHA as the authority*. No silent golden drift.

**Validation discipline (inherited from p7, binding):** no hour-long production run as a sprint
gate. Every sprint proves its claim on (1) unit tests on tiny deterministic fixtures, (2) a
dev-panel smoke ≤5 min. The full-panel prod book is the single operator **V1 milestone** below,
run once after S1–S4 land.

**Process (all sprints):** never `git add -A` (the tree carries unrelated dirty/untracked files —
stage explicit paths); never push; commit trailer EXACTLY
`Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. New unit ⇒ new test file under the
auto-globbed test dir. Performance/impact claims require a recorded before/after line.

---

## Validation milestone V1 (operator, out-of-loop)

After S1–S4 land (the assembled book) and S5 threads the hub, the operator runs the full mega-book
*once*, overnight, to read the real north-star number — the only hour-long run in p8:

```
build-megaalpha-book.ps1 -Profile prod -Stage augment,discover     # canonical-screened panel
build-megaalpha-book.ps1 -Profile prod -Stage riskmodel,combine,metabook,optimize,report
```

Output → an `atx-impl/research/<date>-megaalpha-book-results.md` scorecard: book-level
**net-of-10bps** OOS Sharpe, DSR (cumulative-N), PBO, CPCV, walk-forward, **capacity curve** (edge
vs AUM zero-crossing under √-impact), `N_eff`/IR breadth, and the robustness-battery pass/fail
matrix. If the bar is missed, the reject-histogram + battery-failure dominant bucket names the next
module's target. Honest null is valid.

---

## North star (p8 acceptance)

On the capacity-screened real panel, an **assembled mega-book** (≥5 admitted, decorrelated,
HRP/ERC-allocated, conviction-sized sleeves combined through the factor-covariance optimizer) that
is simultaneously: **net-of-10bps** book-level OOS Sharpe **> 1.0**, DSR > 0 under
cumulative-sweep deflation, PBO < 0.5, turnover < 0.20/day (cross-sleeve netted), capacity curve
positive at ≥ $100M AUM, survives the robustness battery (sub-universe / alternate-neutralization /
noise-control / parameter-perturbation) — OR a documented frontier naming the binding constraint.
Measured only at V1, never in a sprint loop.

---

## Sequencing

1. **Wave 1 (parallel) — the spine + the honesty floor:** **S1** (factor covariance — the
   precondition to trusting any combination) and **S4** (cost/capacity/execution correctness — the
   precondition to trusting any *number*). Disjoint files; dispatch together.
2. **Wave 2 (parallel) — the mega-alpha:** **S2** (HRP/ERC meta-book — the combination centerpiece)
   and **S3** (nonlinear + regime combiner). Both benefit from S1's covariance but do not hard-block
   on it (`MetaAllocator`/`StackingCombiner` take Ω as an input; if S1 has not landed they build a
   sleeve/alpha-return covariance locally and note the dependency).
3. **Wave 3:** **S5** — thread the hub, make deflation blocking, run the battery, produce V1.

**If you can only do one slice:** **S1.** Without a real covariance the optimizer is blind to
correlation and the book cannot be trusted or sized. If two: add **S4** — without honest cost every
reported Sharpe is gross and the ranking is wrong. If three: add **S2** — the meta-book is the
literal mega-alpha. **S3** is the nonlinear multiplier; **S5** is the composition + the verdict.

---

## Future-work backlog (roadmap-only; lifted from p7 residuals + p8 deferrals)

- **p7-S7 CLI carry-forwards** (subsumed by p8-S5): thread `--short-interest`/`--augment-out`,
  `--kelly-fraction`/`--kelly-max-gross`, `--incremental-panel`, and the S1–S6 knobs through the
  CLI hub; wire `GateDeflation` into `library::verdict_for`. p8-S5 completes these as part of the
  hub pass. (Cross-module note: p7-S7 is otherwise empty; p8-S5 absorbs its scope.)
- **NCO (Nested Clustered Optimization)** as the meta-allocation successor to HRP/ERC — uses S1's
  cleaned covariance + clustering; reduces estimation error further (López de Prado 2019a). Deferred
  to a p8-S2 stretch unit or the next module; HRP/ERC ship first.
- **Meta-labeling + triple-barrier + sample-uniqueness weighting** (López de Prado) — a genuinely
  greenfield second-stage classifier that sizes/filters the primary signal. Big-ticket but new
  build, not wiring; deferred to a successor module unless it fits as an S3 stretch.
- **RMT-cleaned data-driven clustering** (`alpha/cluster_panel.hpp`) wired as `IndClass.cluster`
  neutralization — built producer, unproven-over-GICS; ships with an honest head-to-head. Deferred.
- **SIMD intrinsics** for `cs_rank`/`zscore` + hot Ts loops — carried from p7; open only after a
  bench baseline shows the auto-vectorizer ceiling.
- **Survivorship / delisted-symbol recovery** (`data/universe.hpp`) — correctness; needs a
  delisted-name security master with exit dates. Carries the scorecard caveat until done.
- **True GICS industry/subindustry ingestion** (`alpha/augment.hpp` I5-HOOK) — data dependency;
  sub-industry neutralization is a stub until real SIC/NAICS lands.
- **persistence-v2 Dev→UAT→PROD promotion + decay monitor** — the "operate the book" branch, after V1.
- **Pre-existing risk `RobustPipelineE2E` failures** (`NoiseGrowsRobustLibraryByZero`,
  `SyntheticPanelAdmitsRobustSurvivors`) — reproduce on base predating p7; must be triaged before V1.

---

## Anti-roadmap (explicitly NOT in p8)

1. **No new signal families / search operators / estimators** — p8 wires and validates what exists;
   breadth was p7-S2 and is the line. (This is the CIO "stop adding capability" directive, enforced.)
2. **No golden re-baseline except for a corrected P0 bug** — the build profile is always an explicit
   opt-in; `oracle.hpp` frozen; a golden that encoded a bug is re-baselined only with the fix commit.
3. **No hour-long production run as a sprint gate** — V1 is the only full-panel run, operator-driven.
4. **No live broker / order-routing / LOB matching** — research engine only (inherited).
5. **No distributed/cross-machine execution** — single-box; intra-process DetPool only (inherited).
6. **No HMM as the spine** — regime conditioning stays a guarded, optional combine overlay (S3),
   never the primary book (inherited from p7).
7. **No meta-labeling/NCO/RMT-clustering in the critical path** — these are backlog stretch items;
   p8 ships the HRP/ERC + linear-vs-stacking book first, honestly measured.

---

## Strategic decisions — to be resolved by what ships

- **Diagonal vs factor covariance as the production default.** p8-S1 wires the factor model behind
  an inert `RiskModelConfig` default (= diagonal, byte-identical). Whether the factor model becomes
  the *default* is resolved at V1: if the factor-covariance book measurably beats the diagonal book
  on net OOS Sharpe + capacity, S5's prod profile flips the default and the ROADMAP records it.
- **Linear vs stacking as the production combiner.** Resolved at V1 by the OOS-after-DSR comparison
  S3 builds in: the stacking combiner ships as prod default *only if* it beats the linear base on
  the held-out deflated metric; otherwise it stays an opt-in and the linear book ships.
- **MVO vs HRP/ERC as the sleeve allocator.** Resolved at V1 by the net-of-cost, capacity-adjusted
  book comparison; the more robust allocator on the real panel wins the default.

## Recommended sequencing (one-liner for the time-boxed reader)

Do **S1** (covariance) and **S4** (honest cost) first — they make the book trustworthy and the
numbers real. Then **S2** (meta-book) is the mega-alpha; **S3** (nonlinear/regime) is the
multiplier; **S5** composes them, makes deflation blocking, and runs the verdict.

Sprint discipline: same as p0 — see [../docs/sprint.md](../docs/sprint.md). Implementation quality
(mandatory for every coding unit): [../docs/implementation-quality.md](../docs/implementation-quality.md).
