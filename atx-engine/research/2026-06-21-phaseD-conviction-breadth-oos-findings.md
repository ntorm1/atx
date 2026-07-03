# Phase D Findings — Conviction Weighting, Breadth Instrumentation, and the Road to Tradeable Alphas

**Date:** 2026-06-21
**Branch:** `main` (local, unpushed)
**Phase D commits:** `cf17cbd` (C2.1-fix) → `648c060` (D1.2) → `5054be9` (D3a) → `af59837` (D3b) → `7e6591a` (D3c) → `028668f` (NEW-1 doc)
**Final whole-branch review:** READY-TO-MERGE (opus). All defaults byte-identical; `oracle.hpp`/search digest/`version_id` untouched.

---

## TL;DR

- **The Phase D machinery works end-to-end on real ORATS data.** `--conviction`, `--walk-forward`, and breadth telemetry all fire correctly through `combine → optimize → report`, with deterministic, byte-identical default paths.
- **Conviction weighting measurably improves the held-out result:** OOS Sharpe **−1.12 → −0.41**, full-series Sharpe **0.044 → 0.482**, net PnL **10×**. It reweights toward alphas that hold up out-of-sample.
- **Breadth is real and useful:** 30 admitted alphas collapse to **N_eff = 8.76 independent bets** — the crowding the metric was built to expose.
- **But we cannot yet claim a tradeable number, for three concrete reasons:**
  1. **OOS Sharpe is still negative** — the alphas overfit (`oos_pbo = 0.79`) on a short (~2-year) smoke panel.
  2. **The cost model is unwired** (`total_pnl_cost = 0`, no `--cost-bps` flag) — so every reported Sharpe is **gross, not net-of-cost**.
  3. **Turnover is ~74%/week** — the opposite of the low-turnover goal.
- **Diagnosis:** the *combine / eval / telemetry* layer is solid and trustworthy. The bottleneck for the #1 goal is **alpha-generation quality** plus **cost/capacity realism** — not the combiner. We can now *measure* robustness (breadth, DSR, PBO, walk-forward, conviction); we are not yet *generating* alphas that pass those measures out-of-sample, nor *charging* them realistic costs.

---

## 1. What shipped this sprint

| Commit | Task | Summary |
|---|---|---|
| `cf17cbd` | C2.1-fix | ControlBlock under-alignment UB / page+8 segfault fix (cherry-picked from the atxpy branch onto main; main had the live crash). Sprint 1 complete on main. |
| `648c060` | D1.2 | Opt-in `--conviction`: after the combiner fits, scale each alpha's weight by a per-alpha conviction score computed **at combine time from that alpha's own PnL stream** (deflated-Sharpe over real higher moments + first/second-half Sharpe stability; PBO term dropped, `w_dsr`+`w_stability` renormalized to 1), then renormalize Σ\|w\|=1. No sidecar, no library-format change. |
| `5054be9` | D3a | Breadth telemetry (recorded-only, no flag): `breadth_effective_n` (N_eff via the eigenvalue participation ratio of the alpha-return covariance), `breadth_realized_ir` (Sharpe of the weighted-blend PnL), `breadth_implied_ic` (= IR/√N_eff). Fundamental Law of Active Management, `IR = IC·√breadth`. |
| `af59837` | D3b | Opt-in `--walk-forward <k>`: expanding-window k-fold combiner re-fit over `[fit_begin, np)`, records per-fold OOS Sharpe + mean. `combo.bin` byte-identical even with the flag on (WF re-fits are scratch). Extracted a shared `blend_window_sharpe` helper. |
| `7e6591a` | D3c | Doc: `--capacity-floor` is a no-op placeholder under the constant-1.0 capacity stub (real per-name capacity is Phase B1). |
| `028668f` | NEW-1 | Doc: `--walk-forward` OOS scores the **base** combiner fit, not the post-conviction book. |

Each task passed an individual spec+quality review; the whole branch passed a final review as READY-TO-MERGE. Determinism contract held throughout (flag-absent paths and the breadth telemetry are byte-identical; only sanctioned re-baseline remains the library `version_id`).

---

## 2. The real-data experiment

**Setup.** Fresh gated discovery on the real ORATS **smoke** partition (`C:/atx-run/panel_smoke.bin`), then `combine → optimize → report`, baseline vs `--conviction --walk-forward 4`.

- Discover (gated, accumulating): `population=80 generations=10`, gates `min-sharpe=0.05 min-split-sharpe=0 min-dsr=0 min-fitness=0 max-turnover=0.85 max-pool-corr=0.7 oos-fraction=0.25`. Result: **admitted=30 / evaluated=611**, `reject_hist=30,0,385,155,2,39`, **`oos_pbo=0.786`**.
- Combine: `--method bounded --holdout-frac 0.25` → fit `[0,378)` days, OOS `[378,504)`; weekly rebalance → 76 IS / 25 OOS weekly periods.

**Results (same 30 alphas, same panel, same holdout):**

| Metric | Baseline | `--conviction --walk-forward 4` |
|---|---|---|
| **portfolio_oos_sharpe** (held-out) | **−1.123** | **−0.410** |
| portfolio_sharpe (full series) | 0.044 | **0.482** |
| portfolio_is_sharpe | — | 0.706 |
| total_pnl_net | 0.0033 | **0.0333** |
| **total_pnl_cost** | — | **0.000** ⟵ cost model unwired |
| avg_turnover / oos_turnover | — | 0.736 / **0.749** |
| oos_max_drawdown | — | 0.020 |
| max / p95 / median participation % | 16.7 | 14.1 / 3.1 / 0.44 |
| breadth N_eff (over 30 alphas) | 8.76 | 8.76 |
| breadth realized_ir / implied_ic (in-sample) | 3.01 / 1.02 | 3.22 / 1.09 |
| walk-forward OOS Sharpe (4 folds, **base fit**) | — | [2.46, 5.09, −0.67, 2.33], mean **2.30** |

(Combine digests differ — baseline `20b6052028b788ef`, conviction `76cdbb27c96a462a` — confirming conviction shifted the weights; the default-path digest is unchanged from pre-D1.2.)

---

## 3. What worked

1. **Determinism held under real load.** Flag-absent combine reproduces the pre-Phase-D bytes; conviction-on and WF-on are deterministic across re-runs; `combo.bin` is byte-identical with `--walk-forward` on.
2. **Conviction weighting improves out-of-sample behavior.** OOS Sharpe −1.12 → −0.41 and full-series 0.044 → 0.482. The mechanism does what it should: it down-weights alphas whose edge doesn't persist (deflated-Sharpe + half-stability) and up-weights the survivors. This is the first direct evidence the conviction score carries OOS-relevant signal.
3. **Breadth measurement is real and informative.** 30 alphas → 8.76 effective independent bets. That ~3.4× compression is exactly the crowding the metric exists to surface; an operator can now see that "30 alphas" is really ~9 bets.
4. **Walk-forward exposes instability.** The 4-fold OOS Sharpes [2.46, 5.09, −0.67, 2.33] show one genuinely negative fold — the kind of regime fragility a single holdout hides.
5. **The implied-IC diagnostic flags overfit.** `breadth_implied_ic ≈ 1.0` is implausibly high for a real book (realistic single-name IC is ~0.02–0.05); it correctly screams "in-sample overfit," corroborated by `oos_pbo = 0.79`.

---

## 4. What didn't work / gaps this run exposed

1. **Absolute OOS Sharpe is negative.** IS Sharpe 0.71 vs OOS −0.41 is a textbook overfit, driven by a short (~2-year) panel and permissive gates (we deliberately loosened them to admit ≥10 alphas). Conviction reduces the damage but cannot create OOS edge that isn't in a 2-year sample.
2. **The cost model is unwired — "net of cost" is currently vacuous.** `total_pnl_cost = 0.000`, and there is **no `--cost-bps` / cost-rate flag** in the CLI. The `pnl_net` / `total_pnl_cost` plumbing exists in the report, but nothing populates a non-zero cost. **Every Sharpe reported above is effectively gross.** This is a measurement-integrity gap: we cannot claim a *net-of-cost* number until costs are charged. (Same family as the previously-audited "capacity/turnover orphaned" gap.)
3. **Turnover is far too high for the goal.** `avg_turnover ≈ 0.74`, `oos_turnover ≈ 0.75` — ~74% of gross churned per weekly rebalance. The #1 goal explicitly wants **low** turnover; nothing in the current discover/combine path constrains it beyond a loose `--max-turnover` admission gate.
4. **Capacity is measured but not enforced.** Participation telemetry (max 14%, p95 3.1%) and N_eff exist, but `--capacity-floor` is a no-op placeholder (D3c) — the book is not actually sized to a target AUM under a real cost/ADV model. "High-capacity, tradeable at AUM" is unprovable today.
5. **Stored alpha sets are provenance-fragile.** The prior `alphas_v4` (17 alphas, used for `combo_v4`) will not re-evaluate on either `panel_enriched.bin` or the full `panel.bin` — a referenced field resolves to a Group classifier ("operator requires a numeric (f64) primary operand; got a Group classifier"), i.e. panel/expression schema drift. Discovered alphas are not portable across panel schema revisions without the exact panel they were mined on.
6. **Walk-forward OOS measures the base fit, not the shipped book** (NEW-1). With `--conviction` on, `walk_forward_oos_sharpe` reflects the bare combiner method, not the conviction-weighted book that ships, so it is not directly comparable to `breadth_realized_ir`.
7. **Selection inflation across the sweep is only partially accounted.** Run-level CSCV-PBO exists (W4b) and per-run DSR deflates by N-trials, but a search-wide multiple-testing correction across all sweep runs is still thin — admitted Sharpes remain optimistically biased.

---

## 5. Diagnosis — where the real bottleneck is

The combine / eval / telemetry layer is **not** the problem. It is deterministic, reviewed, and now instrumented to *measure* every robustness property we care about (deflated Sharpe, PBO, breadth/N_eff, walk-forward stability, conviction, participation/crowding).

The bottleneck for "real, robust, high-capacity, high-Sharpe, low-turnover alphas" is two-fold:

- **Alpha-generation quality.** We are mining alphas that look good in-sample and fail out-of-sample on short data. We have the *measurements* to reject them; we are not yet *producing* a population that survives those measurements OOS. This needs more data (longer panels), stricter and multiple-testing-aware gates, and explicit diversity pressure.
- **Cost/capacity realism.** Without a wired transaction-cost model and capacity enforcement, no reported number is tradeable — it is gross, capacity-blind, and turnover-unconstrained. The infrastructure seams exist (`pnl_net`, participation, `--capacity-floor`, `cost/capacity.hpp`); they are not connected.

In short: **we built the instruments this sprint; next we must feed them realistic inputs and charge realistic costs.**

---

## 6. Capability inventory (where we are now)

**Have (working, deterministic):**
- 6-stage pipeline (load/panel/discover/sweep/combine/optimize/report) + `run_all`, with library accumulation and IS/OOS holdout split.
- Gated discovery with DSR / split-sample-stability / Sharpe / turnover / pool-correlation admission floors; persistent library + dedup.
- Conviction weighting from per-alpha PnL (D1.2).
- Breadth / IR = IC·√N_eff decomposition (D3a).
- Walk-forward k-fold OOS telemetry (D3b).
- Opt-in crowding de-correlation (`--corr-penalty`).
- Run-level CSCV-PBO gate (W4b), capacity/ADV admission filters (W2), low-vol capacity universe (W5/W6).

**Missing / unwired (blocks "tradeable"):**
- Transaction-cost model (no `--cost-bps`; `total_pnl_cost = 0`). **Highest-leverage gap.**
- Capacity enforcement / AUM sizing (`--capacity-floor` is a placeholder; `cost/capacity.hpp` not wired into combine — Phase B1).
- Turnover control in search/combine (EMA-hold / holding-period / turnover penalty in the objective, not just an admission gate).
- Explicit alpha diversity pressure (signal-family coverage) to raise N_eff.
- Conviction-aware walk-forward (NEW-1) and robust conviction weight-sum (NEW-2).
- Search-wide multiple-testing correction across sweep runs.
- Long-panel discovery runs (only smoke/short panels have been mined end-to-end recently).

---

## 7. Recommended next steps (prioritized toward the #1 goal)

**P0 — Measurement integrity (do first; without these no number is trustworthy):**
1. **Wire a transaction-cost model.** Add a cost rate (e.g. `--cost-bps`, or per-name ADV/spread-based cost) so `pnl_net = pnl_gross − turnover·cost` is genuinely net. Until this lands, no Sharpe in this engine is net-of-cost. *(Smallest, highest-leverage change; turns every existing number honest.)*
2. **Search-wide multiple-testing correction.** Extend the per-run DSR/PBO to deflate by the *total* trials across the whole sweep, so admitted Sharpes aren't selection-inflated.

**P1 — Alpha quality (the actual bottleneck):**
3. **Longer/richer panel + strict gates.** Re-discover on `panel_research.bin` (or full `panel.bin`) with tightened floors (`min-split-sharpe`, `max-pbo`, `min-dsr`) so only OOS-robust alphas are admitted. Trade count for survival.
4. **Turnover control.** Add an EMA-hold / minimum-holding-period / turnover penalty to the discovery objective and/or combine, to drive turnover from ~74%/wk toward a tradeable target (e.g. <20%/wk). Directly serves "low-turnover."
5. **Diversity for breadth.** Search across distinct signal families (price, volume/liquidity, options-IV surface, fundamentals; cross-sectional vs time-series) and penalize pool correlation harder, so N_eff rises toward the alpha count — `IR = IC·√N_eff` rewards independent bets.

**P2 — Capacity realism ("high-capacity, tradeable at AUM"):**
6. **Wire real capacity** (`cost/capacity.hpp` ADV/participation) into combine so `--capacity-floor` actually fades capacity-limited names and the book is sized to a target AUM (Phase B1; closes the D3c placeholder).
7. **Conviction-aware walk-forward** (NEW-1): re-apply the per-fold conviction transform inside each WF fold so `walk_forward_oos_sharpe` reflects the shipped book — the honest OOS estimator for the conviction-weighted mega-alpha.

**P3 — Robustness hardening (after the above):**
8. NEW-2: make the conviction weight-sum robust (`w_stability = 1 − w_dsr`, or relax the engine's exact-equality assert).
9. Regime/HMM conditioning (D2) + persisted panel calendar (D0) — conditional sizing once the base alphas are individually robust.

**Suggested next sprint (minimum to a credible positive net-of-cost low-turnover OOS Sharpe):** P0.1 (wire cost) + P1.3 (long panel, strict gates) + P1.4 (turnover control). That is the shortest path from "the machinery works" to "here is a tradeable number."

---

## Appendix — reproducibility

Artifacts under `C:/atx-run/_phaseD_oos/`. Real ORATS smoke panel `C:/atx-run/panel_smoke.bin`; 30 admitted alphas in `sm_alphas/` (discover digest `6dd1fe0b41fd0781`).

```
# discover (gated, accumulate)
atx-impl discover --gated --panel panel_smoke.bin --seed 7 --population 80 --generations 10 --workers 8 \
  --min-sharpe 0.05 --min-split-sharpe 0 --min-dsr 0 --min-fitness 0 --max-turnover 0.85 \
  --max-pool-corr 0.7 --oos-fraction 0.25 --alpha-out sm_alphas --library-dir sm_lib --seed-expr ... (9 seeds)
# -> admitted=30 evaluated=611 oos_pbo=0.786

# baseline:    combine --method bounded --holdout-frac 0.25                          (digest 20b6052028b788ef)
# conviction:  combine --method bounded --holdout-frac 0.25 --conviction --walk-forward 4  (digest 76cdbb27c96a462a)
# then:        optimize --combo <combo>.bin --books-out <books>.bin ; report --books <books>.bin --combo <combo>.bin

# baseline report:    portfolio_oos_sharpe = -1.122855  (portfolio_sharpe 0.044464)
# conviction report:  portfolio_oos_sharpe = -0.409552  (portfolio_sharpe 0.482281, is 0.706298,
#                     avg_turnover 0.736, oos_turnover 0.749, total_pnl_cost 0.000, N_eff 8.76,
#                     WF folds [2.46,5.09,-0.67,2.33] mean 2.30)
```

**Caveats:** (1) `total_pnl_cost = 0` — figures are gross, not net-of-cost (cost model unwired). (2) Smoke partition (~2 yr); OOS is 25 weekly periods — too short for a robust conclusion. (3) `walk_forward_oos_sharpe` is the base combiner fit, not the conviction-weighted book.
