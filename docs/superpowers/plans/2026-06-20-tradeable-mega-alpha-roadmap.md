# Roadmap — Real, Robust, High-Capacity, Tradeable Alphas + Mega-Alpha Portfolios

**Date:** 2026-06-20 · **Branch base:** `main` @ `52c2d1b` (pipeline-remediation `dbdd784` + alpha101 research `3ea5b5a`)
**Goal (#1 priority):** build atx-impl + atx-engine to discover real, robust, high-capacity, TRADEABLE alphas and assemble them into mega-alpha portfolios.
**Sequencing decision (human, 2026-06-20):** ROBUSTNESS-FIRST — gate the accumulation sweep behind multiple-testing + holdout fixes before scaling it.

---

## 0. Where we are (synthesis of two work streams)

**Stream 1 — pipeline remediation (just merged, `a7a1a73..dbdd784`):** search/diversity defaults fixed; admission gate is now **DSR-based** (deflated Sharpe, the real significance bar) with select/admit aligned; grammar guards; OOS eval reuse; parallel OOS admission (digest-identical); online VM kernels; **library accumulation** via `--library-dir` (`config.hpp:79`, `stage_discover.cpp:85-91`); **combine-from-library** (`stage_combine.cpp:89-117`); opt-in `decorrelate_weights` (`stage_combine.cpp:230-244`).

**Stream 2 — alpha101 signal-conditioning + covariance sizing (just merged, `3ea5b5a`):** proved, on the real 651×14,633 liquid panel, that **cross-sectional covariance sizing** (`w∝Σ⁻¹μ` via `PortfolioOptimizer::solve` + an APCA statistical factor model) beats eq-dollar and beats correlation-blind inverse-IV (which underperformed 2.00→1.61). Conditioning (winsorize+zscore) helps. λ collapses under gross-normalize → one covariance tilt, not a sweep. **All test-harness only** (`atx-impl/tests/alpha101_*`), real-panel, 25-alpha subset.

### What's strong + wired
Search → DSR gate (`deflated_sharpe.hpp:136`, `factory.hpp:126` `cand.dsr>=min_dsr`) → CPCV OOS folds (`cpcv.hpp:175`, `fitness.hpp:292`) → library accumulation → combine-from-library. The statistical-validity spine is genuinely good and deterministic.

### What landed but is UNUSED
- **`run_all` does not consume the accumulated library.** `stage_run.cpp:50-60` never sets `library_dir`; `run` goes discover→combine via loose `.dsl`, single-run/single-seed. The mega-alpha unlock is only reachable via standalone `discover`/`combine` with a shared `--library-dir`.
- **`ResearchDriver`** (across-run mine→admit loop w/ patience + novelty-exhaustion, `factory/research_driver.hpp`) is **unwired** — discover calls `mine_into` once (`stage_discover.cpp:214`). Growing a large library = re-invoking discover by hand per seed.

### What's proven but UNPROMOTED
- The per-date **APCA factor-model builder** is **test code** (`alpha101_riskmodel.hpp:242 build_stat_risk_model`), not an engine API. Production sizing (`stage_optimize`) uses a **diagonal idiosyncratic-only** risk model (`diag_risk.hpp`: X=M×1 zeros, F=I) — no common-factor covariance.
- `PortfolioOptimizer::solve` (`risk/optimizer.hpp:141`) IS a real engine API (Woodbury V⁻¹, `factor_model.hpp:142-145`); the harness consumes it directly.

### The two SILENT LANDMINES the accumulation feature just armed 🚨
1. **No cross-sweep multiple-testing accounting.** DSR's N = one run's `res.trial_count` (`factory.cpp:62`, `search_driver.cpp:1195`). The mega-alpha thesis = sweep K seeds × N candidates into one library; each survivor is deflated as if only N trials occurred. **The sweep — the entire point — is invisible to the one anti-overfitting defense.**
2. **Holdout exhaustion.** One fixed terminal OOS window (`oos_fraction`, `factory.hpp:134`, `mine_into_oos` `factory.hpp:310`) reused every run; accumulate against it K× and it silently inflates. No walk-forward, no rotation, no cross-run embargo.

### The near-empty axis: tradeability / capacity
Cost model exists (`fitness.hpp:151 book_cost_bps`, real ADV-based impact) but **OFF by default** (`target_aum=0`, `config.hpp:66`) and only a Pareto nudge — **never a gate**. `capacity_floor` is a **constant-1.0 stub** (`stage_combine.cpp:237`) → the capacity half of decorrelate is a no-op. No turnover penalty in the objective (only a flat `max_turnover=0.70` cap, `gate.hpp:70`). PBO computed (`pbo.hpp:298`) but only feeds `conviction()`, never gates. Borrow (`cost/borrow.hpp`) + factor-neutralization (`FactorModel::neutralize`) unwired.

---

## 1. Governing principle (the reframe)

> The mega-alpha thesis — accumulate many alphas across many seeds — is **exactly the operation that defeats per-run DSR and exhausts the fixed holdout.** Building the accumulation sweep machine before fixing trial-accounting + holdout-rotation makes a high-throughput **overfit-garbage generator** that looks great in-sample and dies live.

Therefore robustness is not one of four equal workstreams; it is the **gate** on the validity of everything downstream. Phase R lands before the sweep scales.

## 2. North-star metric + acceptance frame

The deliverable is a **net-of-cost OOS Sharpe of the mega-alpha book at a target AUM**, reported alongside:
- **cross-sweep-deflated DSR** (true cumulative trial count, Phase R1),
- **PBO** of the combined book (Phase R3),
- a **capacity curve** (AUM where net edge → 0, `cost/capacity.hpp capacity_point`, Phase T2).

A run is "tradeable-credible" only when all four are reported and the OOS number is computed on a holdout the sweep has not exhausted. Determinism (byte-identical digests, worker-invariant) is preserved throughout — every new path is opt-in/flagged with a byte-identical default, same discipline as the remediation sprint.

---

## 3. Phase R — Robustness foundation (DO FIRST; gates everything)

**R1 — Cross-sweep cumulative trial accounting for DSR.** *Highest-leverage fix in the whole roadmap.*
- Today DSR deflates against one run's `res.trial_count`. Persist a **cumulative trial counter** in the library journal so that when `--library-dir` accumulates across runs, each candidate deflates against the **true cumulative N** of the sweep that produced the pool.
- Touch: `library/` journal (add a durable trial-count field, incremented per run's `res.trial_count`), `factory.cpp:62` (`admit_fit.trial_count`), the DSR call (`fitness.cpp:304`), `deflated_sharpe.hpp:136`. Determinism: the counter is part of the digest; same sweep → same N → byte-identical.
- Acceptance: a K-run sweep deflates survivors against ≈K·N trials, not N; a single run is byte-identical to today (counter starts at this run's N). Open design Q: count distinct candidates vs raw evaluations; how dedup interacts with the counter.

**R2 — Walk-forward / rotating holdout (kill exhaustion).**
- Replace "one fixed terminal holdout reused every run" with a **rotating or advancing OOS window** across the sweep (walk-forward), or accumulate per-run holdouts with cross-run embargo accounting, so no single window is reused K times.
- Touch: `mine_into_oos` (`factory.hpp:310`), `oos_fraction`/`oos_embargo` (`factory.hpp:134`), the sweep driver (Phase M). Respect the panel determinism + the OOS serial==parallel admit-order invariant (Task 5/8.C).
- Acceptance: across a sweep, each alpha's confirming holdout is disjoint-or-embargoed from prior runs'; reported OOS Sharpe is on un-exhausted data.

**R3 — PBO as a recorded gate + OOS-on by default for accumulation.**
- Surface **PBO** per candidate (it's computed, `pbo.hpp:298`) and make it a recorded admission metric (optionally a gate floor). Default the OOS holdout ON for accumulation runs (it's off by default today, `oos_fraction=0`, and when on ~93% of in-sample survivors fail it — it does real work).
- Touch: gate (`gate.hpp`), `config.hpp` (a `--min-pbo` / `--oos-fraction` default for accumulation), conviction plumbing (feeds Phase S3).
- Acceptance: PBO recorded on every admit; accumulation runs validate on a real holdout by default; single-run defaults unchanged unless the flag is set.

## 4. Phase M — Mega-alpha orchestration (wire the landed unlock)

**M1 — Drive accumulation across seeds.** Wire `ResearchDriver` (or a new `sweep` subcommand) to loop discover across K seeds with patience + novelty-exhaustion, accumulating into one `--library-dir`. Touch: `factory/research_driver.hpp`, a new stage or `stage_discover` loop, `stage_run.cpp`. Must carry R1's cumulative counter + R2's rotating holdout.
**M2 — `run_all` consumes the library.** Set `library_dir` in the `run_all` chain (`stage_run.cpp:50-60`) so discover→library→combine flows end-to-end on the accumulated pool; decorrelate-on (`--corr-penalty>0`) for the pool blend. Optionally replace the hand-rolled static blend (`stage_combine.cpp:276-292`) with `CombinedSignalSource` (`combine/combined_source.hpp`) for a single revertable blend object. Acceptance: `run_all` produces a combo over the accumulated pool; determinism preserved (stage digests fold, `stage_run.cpp:78-99`).

## 5. Phase S — Covariance + conviction sizing (promote the proven path)

**S1 — Promote the APCA builder test→engine.** Move `build_stat_risk_model` (`alpha101_riskmodel.hpp`) into `atx-engine/.../risk/` as a real API: PIT-safe causal gather (pin-tested), fit-once-per-date cache (the 651-fit win, `:1076-1089`), LW-shrinkage factor cov (`:135`), specific-var floor for PSD. Acceptance: engine-side unit tests mirror the alpha101 pins (λ=0 ≡ eq-dollar, PIT no-future-read poison test).
**S2 — Wire it into `stage_optimize`.** Replace the diagonal idiosyncratic-only risk model (`diag_risk.hpp`) with the APCA factor model for production mega-alpha sizing via `PortfolioOptimizer::solve`. Gate behind a flag (`--risk-model apca`) with the diagonal default byte-identical. Mind the runtime (per-date APCA fit ≈ 20-40 min/full-panel; subset/active-universe mitigations).
**S3 — Conviction-weighted entry + optional fractional-Kelly.** Thread DSR + PBO + OOS/IS-stability → `ExplainFlag` → `conviction()` (`combine/conviction.hpp`) so weak alphas enter the blend discounted, not at full weight. Optional explicit leverage via `risk/kelly_sizing.hpp` (`f*=Σ⁻¹μ`, reuses the SAME `FactorModel::apply_inverse`, quarter-Kelly default). Per the rentech research guardrail, keep this measured.

## 6. Phase T — Tradeability / capacity (enforce, don't assume)

**T1 — Cost model ON + turnover penalty in the objective.** Default `target_aum>0` for tradeable runs so `book_cost_bps` computes (`fitness.hpp:151`); add a turnover term to the fitness objective (today `raw=wq*diversify*robust`, no turnover term, `fitness.hpp:163`) — not just the 0.70 cap.
**T2 — Real remaining-capacity (kill the 1.0 stub).** Compute per-name remaining capacity from the cost/ADV model to fill `decorrelate_weights`' capacity vector (replace the constant `1.0`, `stage_combine.cpp:237`) so crowding actually fades crowded/illiquid names; surface `capacity_point` (`cost/capacity.hpp`) as the capacity curve for the north-star metric, and optionally an admission/sizing gate.
**T3 — Borrow + factor-neutralization (lower).** Wire `cost/borrow.hpp` short-availability/borrow-cost into the book; wire `FactorModel::neutralize` into the WeightPolicy (`weight_policy.hpp`) alongside the existing dollar/sector neutrality.

## 7. Phase G — Regime (deferred, lowest)

Blocked on the panel `.bin` date-axis format change (no calendar to join the FRED-axis regime `.seg` against the trading-universe panel) — see `docs/superpowers/plans/2026-06-20-combine-regime-pipeline.md`. Research guardrail: the HMM-centric claims were weakly supported (refuted 0-3 of 4); do **not** over-architect around HMMs. Do this after R/M/S/T deliver a tradeable mega-alpha book.

---

## 8. First-sprint scope (recommended) + execution

Per the robustness-first decision, the immediate sprint = **Phase R**, the foundation that makes the sweep trustworthy, plus the **minimum of M** to exercise it:
1. R1 cumulative cross-sweep trial counter (the single most consequential fix).
2. R2 rotating/walk-forward holdout.
3. R3 PBO recorded + OOS-on-by-default for accumulation.
4. M1-lite: a small `sweep` that drives K seeds into `--library-dir` carrying R1+R2 — enough to validate R end-to-end on a few seeds, not yet the full WQ-scale sweep.

Then re-plan M2/S/T as separate sprints once the accumulated library is trustworthy.

**Execution method:** decompose into per-task briefs and run the same **subagent-driven-development** loop used for the remediation branch (fresh implementer → task review spec+quality → fix loop → ledger → final whole-branch review → finishing-a-development-branch). Determinism discipline is non-negotiable: every new path opt-in/flagged, byte-identical default, no unsanctioned golden re-baseline, `oracle.hpp` untouched.

## 9. Open design questions (resolve at brief-time per phase)
- **R1:** cumulative N = distinct candidates or raw evals? interaction with dedup/`Duplicate` re-admits? journal schema + digest folding.
- **R2:** walk-forward windows vs holdout rotation vs per-run-embargo — which preserves the serial==parallel admit-order invariant most cleanly?
- **S2:** APCA per-date fit runtime at full 14.6k universe — subset/active-universe gating, N_active>window precondition handling.
- **Universe:** run `load --exclude-no-sector` GICS prune first to tighten the universe (not yet run)?
- **M2:** adopt `CombinedSignalSource` now, or keep the inline position-stream blend and defer?
