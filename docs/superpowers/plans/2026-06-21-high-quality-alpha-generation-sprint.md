# Sprint Plan — High-Quality Alpha Generation

**Date:** 2026-06-21
**Goal:** produce **real, robust, high-capacity, high-Sharpe, low-turnover** tradeable alphas and a downstream mega-alpha with a **positive net-of-cost out-of-sample Sharpe at target AUM**.
**Branch base:** `main` (Phase D complete: conviction / breadth / walk-forward telemetry shipped).
**Method:** subagent-driven-development (SDD) on local `main`, per the standing directive.

---

## Framing — what the code already has (the central insight)

The Phase D real-data run proved the *measurement* layer works but exposed that the engine reports **gross** Sharpe (cost = 0), churns ~74%/week, and overfits on short panels. Investigation shows the engine already contains most of the machinery for high-quality generation — **it is dormant, not missing**:

| Capability | Status in code | Anchor |
|---|---|---|
| Transaction-cost model (temp/perm impact, calibrated) | **Built, turned OFF** (`round_trip_cost_bps`; optimize hardcodes rate 0) | `cost/cost_aware.hpp:114`, `cost/calibration.hpp`, `stage_optimize.cpp:202` (`cost.round_trip_cost_bps = 0.0`), `:78` (`cost_bps[s]=0`) |
| Per-alpha cost in the search objective | **Built, OFF** (`-cost_bps` objective dim; `book_cost_bps`) | `factory/fitness.hpp:151,203` (`objectives = {wq, diversify, robust, novelty, -cost_bps, -node_count}`) |
| Diversity pressure in the objective | **Built, low weight** (`diversify = 1 − mean\|corr-to-pool\|`) | `factory/fitness.hpp:18,125` (`corr_to_pool`) |
| Sub-universe robustness re-eval | **Built, OFF** (needs `weak_panel`; else robust=1.0) | `factory/factory.hpp:163-171` (`weak_panel`) |
| Split-sample stability gate | **Built, OFF** (`min_split_sharpe = −inf`) | `factory/factory.hpp:138` |
| Run-level CSCV-PBO gate | **Built, OFF** (`max_pbo = 1.0`) | `factory/factory.hpp:152` |
| Walk-forward holdout in discovery | **Built** (`oos_n_windows`/`oos_window`) | `factory/factory.hpp:179-180` |
| Signal-decay DSL ops (turnover control) | **Built** (`decay_linear`/`ema`/`wma`) | `alpha/ts_ops.hpp:564,578,661` |
| Capacity / participation model | **Built, not wired to combine** (constant-1.0 stub) | `cost/capacity.hpp`, `stage_combine.cpp:342` (D3c) |
| Multiple-testing N (DSR trial count) | **Per-run only** (cross-sweep uncorrected) | `factory/factory.hpp:187`, `fitness.hpp` (dsr at running N) |
| Stateful position-decay weight policy | **NOT built** (architectural gap) | `loop/weight_policy.hpp:96-98` |

**Therefore this sprint is mostly *activation + tuning*, not greenfield build.** The two true gaps are (a) search-wide multiple-testing correction and (b) capacity wiring into combine.

---

## Global Constraints (binding — copy into every task brief)

- **Determinism is sacred.** `oracle.hpp` MUST NOT be touched. The F1 search digest stays byte-identical on the default (flag-absent) path.
- **The ONLY sanctioned re-baseline is the library manifest `version_id`.** Tasks that change *admission* (T2, T3 if it changes defaults, T4 objective changes) alter which alphas are admitted → they re-baseline `version_id`, which is allowed — but they must be **opt-in behind a flag** so the no-flag path stays byte-identical, and they must NEVER change the search digest / `oracle.hpp`.
- **Net-of-cost requires T1.** No task may claim a "net" number until T1 lands; before that, label all Sharpes "gross."
- NEVER `git add -A`; stage explicit paths. Commit trailer EXACTLY `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`. Do NOT push.
- Pre-existing failures out of scope: `AtxImplPanel.BuildsPanelFromSegments`, `Alpha101Orats.*`, `AlphaSlotPoolDeathTest.*`, `AlphaVm_ZeroAlloc.*`, `RegimeCombineDeathTest.*` (Release/NDEBUG death tests).

---

## Canonical Universe & Rebalance (BINDING for every run in this sprint)

All discovery/combine/optimize/report runs MUST use this universe and cadence. This is not new code — every knob is already CLI-wired; it is a **panel-rebuild + run-config** requirement (the universe screen is applied at panel build, `build_history_panel` → `UniverseConfig`, so the panel must be rebuilt with these settings; the pre-built `panel_*.bin` were built with defaults and do NOT match).

**Universe — single-name US equities with a GICS sector, rolling/per-date membership:**

| Requirement | Knob (`UniverseConfig`, `data/universe.hpp:92`) | CLI flag | Value |
|---|---|---|---|
| Single-name stock w/ GICS code (exclude ETFs / sector-less) | `require_sector` (`:99` "exclude names with no GICS/SIC sector — a single-stock requirement") | `--require-sector` (`config.cpp:34`) | ON |
| close > 1 | `min_price` (`:97`, membership iff `raw_close > min_price`) | `--min-price` (`config.cpp:157`) | `1.0` |
| adv20 > $25M | `min_adv_usd` (`:94`) + `adv_window` (`:93`) | `--min-adv-usd` + `--adv-window` | `25000000` + `20` |
| No count cap (rolling, breadth-friendly) | `top_n_by_adv` (`:96`) | `--top-n-by-adv` | `0` |

Membership is recomputed per date (causal trailing ADV, per-date `raw_close > 1`, per-date sector presence) — i.e. already a **rolling** universe; a name enters/leaves as its liquidity/price/sector cross the bounds. Use `--compact-universe` (`config.cpp:35`) to drop never-in-universe columns for memory/eval cost (lossless; off the digest path only if explicitly the legacy default — verify it doesn't change in-universe signal).

**Rebalance — DAILY:**
- `--rebalance daily` (`stage_optimize.cpp:57,63` → `step = 1`; default is weekly `step = 5`). Phase D ran weekly; all sprint runs use **daily**. Turnover figures (T4) and net-of-cost (T1) MUST be measured on the daily schedule.

**Implication for the harness:** the first step of any sprint run is `load` + `panel` (or `run_all`) **rebuilding the panel from the ORATS partition with the four universe flags above**, then discovery/combine on that panel, then `optimize --rebalance daily`. Do NOT reuse `panel_smoke.bin`/`panel_research.bin`/`panel.bin` as-is — rebuild with this screen so the universe is exactly single-name-GICS / close>1 / adv20>$25M.

---

## Phase 0 — Measurement integrity (do FIRST; without these no number is trustworthy)

### T1 — Wire the transaction-cost model (net-of-cost becomes real)
**Why:** every Sharpe today is gross (`total_pnl_cost = 0`). This is the single highest-leverage change — it makes all existing and future numbers honest.
**Anchors:**
- `atx-impl/src/stage_optimize.cpp:202` — `cost.round_trip_cost_bps = 0.0` (hardcoded; the MultiPeriodOptimizer path).
- `atx-impl/src/stage_optimize.cpp:78,132-133,181` — position-mode branch fills `cost_bps[s] = 0` and writes it to the book.
- `atx-engine/include/atx/engine/cost/cost_aware.hpp:114` — `round_trip_cost_bps(const CalibratedCost& cc, f64 ref_participation, f64 ref_sigma)` (the real model: `temp = Y·σ·p^δ`, `perm = 0.5·γ·σ·p`).
- `atx-engine/include/atx/engine/cost/calibration.hpp` — `CalibratedCost` coefficients (calibratable from fills, or default coeffs).
- `atx-impl/src/stage_report.cpp:289-305` — `sharpe_of` over `rep.pnl_net`; report already reads `cost_bps` per period from the book meta.
**Design:**
- **Step 1 (minimal, ship first):** add a flat `--cost-bps <round_trip_bps>` config field. In optimize, set `cost.round_trip_cost_bps = cfg.cost_bps` (MPO path) AND fill the position-mode `cost_bps[s] = cfg.cost_bps` so `pnl_net[s] = pnl_gross[s] − turnover[s]·(cost_bps/1e4)`. Verify report's `total_pnl_cost` then reflects it.
- **Step 2 (calibrated, follow-up within the task or next):** allow `--cost-model calibrated` to use `cost_aware::round_trip_cost_bps` with a `CalibratedCost` (default coefficients if no calibration file), keyed on per-name participation/σ — the capacity-aware cost. This is the AUM-realistic path.
- Also flip ON the search-side cost term: pass a non-zero cost config so `fitness.hpp` `book_cost_bps` populates the `-cost_bps` objective dim (the search starts preferring cheaper-to-trade alphas).
**Determinism:** default `cost_bps = 0` ⇒ byte-identical to today. `> 0` opts in. Optimize/report digests are not golden-pinned to cost; combine digest untouched. `oracle.hpp` untouched.
**Tests:** `cost_bps>0` ⇒ `total_pnl_cost>0`, `portfolio_oos_sharpe(net) < portfolio_oos_sharpe(gross)`, and the gap scales with turnover; `cost_bps=0` ⇒ byte-identical book.

### T2 — Search-wide multiple-testing correction (selection-bias integrity)
**Why:** the deflated-Sharpe N is the **per-run** trial count, so an alpha admitted from run 7 of a 20-run sweep is deflated as if only run 7's trials existed — selection inflation across the sweep is uncorrected. This is *the* reason in-sample winners die OOS.
**Anchors:**
- `atx-engine/include/atx/engine/factory/factory.hpp:187` — `evaluated = res.trial_count` (per-run N).
- `factory/fitness.hpp` — `dsr = deflated_sharpe(sr, T, skew, exkurt, N, var)` at "the running trial count N".
- `atx-engine/include/atx/engine/factory/research_driver.{hpp,cpp}` — the sweep loop; already accumulates cross-run telemetry (C2.2 `cross_run_*`).
- `atx-engine/include/atx/engine/eval/deflated_sharpe.hpp` — `deflated_sharpe(..., N, ...)`.
**Design:** add an opt-in `--sweep-wide-deflation` that makes `ResearchDriver` thread a **cumulative** trial count (sum of `res.trial_count` over runs so far, or total planned) into each run's admission-DSR N, so the deflation reflects the whole search. Keep the per-run path as default.
**Determinism:** opt-in flag; default unchanged (byte-identical, per-run N). When ON it changes admission ⇒ **re-baselines `version_id`** (sanctioned) but NOT the search digest / `oracle.hpp`.
**Tests:** cumulative N ≥ per-run N; with the flag, the admitted set is a subset of (or equal to) the per-run admitted set; DSR strictly decreases as N grows; twice-run identical.

---

## Phase 1 — Alpha-generation quality (the core of this sprint)

### T3 — Robust-by-construction discovery profile (long panel + strict gates + walk-forward)
**Why:** Phase D admitted 30 alphas with `oos_pbo=0.79` because gates were loosened to get a multi-alpha book on a 2-year panel. The fix is more data + stricter, robustness-aware admission — most of which already exists and is merely OFF.
**Anchors (all already in `FactoryConfig`):** `min_dsr=0.5` (`factory.hpp:127`), `min_split_sharpe=−inf` → finite (`:138`), `max_pbo=1.0` → `<1` (`:152`), `oos_fraction`/`oos_embargo` (`:159-162`), `weak_panel` sub-universe robustness (`:163-171`), `oos_n_windows`/`oos_window` walk-forward holdout (`:179-180`). CLI: `--min-dsr`, `--min-split-sharpe`, `--max-pbo`, `--oos-fraction` already parse.
**Design:**
- **Sub-task 3a (wiring, if needed):** confirm `--oos-n-windows`/`--oos-window` and a `--weak-universe`/sub-universe selector are CLI-exposed and threaded into `FactoryConfig`. If not, expose them (small additive flags). The weak_panel is the W4a robustness activator — turning it on makes `robust` a real multiplier in fitness instead of 1.0.
- **Sub-task 3b (the run):** a reproducible harness that (i) **rebuilds the panel from the ORATS partition with the Canonical Universe** (`--require-sector --min-price 1.0 --min-adv-usd 25000000 --adv-window 20`) — NOT a reuse of the pre-built panels; then (ii) runs discovery on that panel with the **strict profile**: `min-dsr 0.5`, `min-split-sharpe > 0`, `max-pbo 0.5`, `oos-fraction 0.25`, walk-forward windows ≥ 3, weak-universe ON, multi-seed sweep, `--sweep-wide-deflation` (T2); then (iii) combine→optimize with **`--rebalance daily`**→report. Trade alpha *count* for alpha *survival*.
- **Sub-task 3c:** an admitted-set quality report (per-alpha DSR, split-stability, PBO, OOS Sharpe) so we can see survival, not just count.
**Determinism:** turning gates ON changes admission ⇒ opt-in via flags / a named profile; `version_id` re-baseline only. No search-digest / oracle change.
**Tests:** with the strict profile on a fixture, admitted alphas all satisfy `dsr≥0.5 ∧ split-stable ∧ run-PBO≤0.5`; the admitted set shrinks vs the loose profile; determinism holds.

### T4 — Turnover control in generation (low-turnover goal)
**Why:** ~74%/week is untradeable. The DSL already has decay ops; the search just isn't pushed to use them, and turnover is only a *ceiling* (admission gate), not a *minimization objective*.
**Anchors:** `alpha/ts_ops.hpp:564` `TsDecayLinear`, `:578` `TsEma`, `:661` `TsDecayExp`; `factory/fitness.hpp` wq turnover floor 0.125 + objective vector (add/strengthen a turnover term); `--max-turnover` gate; per-period turnover def `Σ_i|w_t−w_{t-1}|`.
**Design (three levers, ordered by effort):**
1. **Seed + mutate with decay ops:** add decay-wrapped seed templates (`decay_linear(<expr>, d)`, `ema(<expr>, n)`) and ensure mutation can wrap subtrees in a decay op, so smoothing is reachable in the search space. Smoothing the signal smooths the positions ⇒ lower turnover, near-zero Sharpe cost.
2. **Turnover in the objective:** add a turnover-penalty / `−turnover` objective dimension (or raise the wq turnover floor) so among equal-Sharpe candidates the search prefers the lower-turnover one. Opt-in weight.
3. **Tighten `--max-turnover`** in the strict profile (T3).
4. **(Stretch) Stateful position-decay WeightPolicy:** the architectural gap at `weight_policy.hpp:96` — an EMA-smoothed target-weight policy that damps rebalance churn. Bigger change; defer unless levers 1-3 are insufficient.
**Determinism:** objective/seed changes are opt-in / re-baseline `version_id`; default byte-identical.
**Tests:** with decay seeding + turnover penalty, the admitted book's mean turnover drops materially (e.g. <30%/wk) at comparable gross Sharpe; determinism holds.

### T5 — Diversity / breadth pressure (raise N_eff → IR = IC·√N_eff)
**Why:** Phase D's 30 alphas were really 8.76 independent bets. The Fundamental Law says IR scales with √(independent breadth) — more *orthogonal* alphas is the highest-quality lever for Sharpe.
**Anchors:** `factory/fitness.hpp:18` `diversify = 1 − mean|corr-to-pool|` (already in `raw`); `corr_to_pool` (`:125`); `--max-pool-corr` gate; D3a `breadth_effective_n` telemetry.
**Design:**
1. **Up-weight `diversify`** in the objective so the search actively hunts uncorrelated alphas (currently it's one multiplicative term).
2. **Tighten `--max-pool-corr`** so near-duplicates are rejected at admission.
3. **Seed across distinct signal families:** price (close/open), volume/liquidity (ADV), options-IV surface (the `atmCenI_*` fields), fundamentals (`market_cap`), cross-sectional vs time-series — so the search draws independent sources, not 30 variants of price momentum.
4. **Use D3a `N_eff` as a run acceptance metric:** report the admitted book's N_eff; target N_eff ≥ (say) 0.6·count.
**Determinism:** opt-in weights / `version_id` re-baseline; default unchanged.
**Tests:** admitted book N_eff rises vs the baseline run on the same panel; max pairwise pool corr ≤ the tightened bound.

---

## Phase 2 — Capacity realism ("high-capacity, tradeable at AUM")

### T6 — Wire real capacity into combine (close the D3c placeholder)
**Why:** `--capacity-floor` is a no-op (constant-1.0 stub); the book is not sized to a real AUM under liquidity limits. Without this, "high-capacity at AUM" is unprovable.
**Anchors:** `stage_combine.cpp:342` (constant-1.0 capacity vector → `decorrelate_weights`); `cost/capacity.hpp` (per-name capacity / participation from ADV); `combine/crowding.hpp` `decorrelate_weights` capacity_floor path; `fitness.hpp:138-151` `book_cost_bps` participation math (`part = target_aum·|w|/price / adv`); `--target-aum` flag.
**Design:** at combine time, compute the per-name remaining-capacity vector from ADV/participation (`cost/capacity.hpp`) at `--target-aum`, and pass it (instead of the constant 1.0) into `decorrelate_weights` so `--capacity-floor` actually fades capacity-limited names; report book capacity / max participation at AUM.
**Determinism:** opt-in (`--capacity-floor > 0` AND real-capacity mode); default off byte-identical.
**Tests:** capacity scaling reduces weight on illiquid names; participation at AUM respects the floor; default path unchanged.

---

## Phase 3 — Robustness hardening (after the above produce a positive number)

### T7 — Conviction-aware walk-forward + conviction weight-sum
- **NEW-1:** re-apply the per-fold conviction transform inside the D3b walk-forward loop (`stage_combine.cpp` WF block) so `walk_forward_oos_sharpe` reflects the *shipped* conviction-weighted book, making it the honest OOS estimator. Opt-in behind `--conviction`; default unchanged.
- **NEW-2:** make the conviction weight-sum robust — set `w_stability = 1 − w_dsr` (after `w_pbo=0`) in the D1.2 block, or relax the engine's exact-equality assert in `conviction()`, so a future default-weight change can't abort a Debug run.
**Tests:** WF OOS with conviction on differs from the base-fit WF and matches the shipped book's character; conviction weight-sum holds for arbitrary default weights.

---

## Sequencing

```
PHASE 0 (integrity — unblocks honest measurement)
  T1 cost model ──┐         (highest leverage; makes every number net)
  T2 sweep-wide deflation   (independent of T1)
        │
        ▼
PHASE 1 (generation quality — the heart of the sprint; T3/T4/T5 can overlap)
  T3 strict robust profile ──► long-panel discovery runs
  T4 turnover control (decay seeding + objective)
  T5 diversity / breadth pressure
        │
        ▼
PHASE 2  T6 capacity wiring (AUM realism)
        │
        ▼
PHASE 3  T7 conviction-aware WF + weight-sum hardening
```

Run order rationale: T1 first (no honest number without it); T2 alongside (selection integrity). Then T3+T4+T5 are the quality core — each opt-in, each re-baselining `version_id` only. T6 makes it AUM-real. T7 hardens.

---

## Acceptance (sprint exit)

A **single reproducible run** — panel rebuilt with the **Canonical Universe** (single-name GICS, close>1, adv20>$25M, rolling), strict-profile multi-seed discovery (T3) with sweep-wide deflation (T2), decay/turnover control (T4) and diversity pressure (T5), combined with `--conviction`, **`--rebalance daily`**, sized at `--target-aum` with real capacity (T6) and **`--cost-bps` charged (T1)** — that reports:

1. **Positive `portfolio_oos_sharpe` NET of cost** on a held-out window the search never saw, on the **daily** schedule.
2. **Daily turnover below a tradeable target** (e.g. < 10%/day ≈ low weekly churn) in the OOS window — measured daily, not weekly.
3. **Breadth `N_eff` materially above the naive count-collapse** of the Phase-D baseline.
4. **Admission corrected for search-wide multiple testing** (every admitted alpha clears the strict DSR/split/PBO bars at cumulative N).
5. Determinism intact: default (flag-absent) paths byte-identical; `oracle.hpp`/search-digest untouched; only `version_id` re-baselined by the opt-in admission changes.

If the OOS Sharpe is still ≤ 0 after T1-T5 on the full panel, that is itself a decisive finding: the signal sources in the current DSL/field set do not carry net-of-cost OOS edge, and the next sprint must expand the *data* (new fields/datasets, e.g. the FINRA short-interest the user is exploring) rather than the search.

---

## Notes for the SDD controller

- T1 and T6 touch `stage_optimize`/`stage_combine` + the cost/capacity engine headers — standard model implementers, opus review (determinism-sensitive).
- T2 touches `research_driver` + DSR N — opus implementer + review (admission semantics, re-baseline).
- T3 is mostly run-configuration + a small harness + (maybe) CLI flag exposure — cheap implementer for wiring, then a *run* (not a code task) for 3b/3c.
- T4/T5 touch the search objective (`fitness.hpp`) + seed templates — standard implementer, opus review (objective changes re-baseline `version_id`; must stay opt-in).
- Every admission-changing task MUST keep the no-flag path byte-identical and prove it with the existing factory golden-digest tests staying green.
