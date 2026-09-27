# Mega-alpha parent handoff 2 — stopped at the owner's request

Prepared 2026-09-26 (evening, America/New_York) by the resumed parent. **Revised 2026-09-27 for the
owner's cadence/turnover ruling (§2, §2a, §5, §6, §8, §9).** Where any older document, brief or ledger
entry mentions a 30%/month turnover target, monthly-budget-v2 as a candidate policy, or cadence 5 as the
construction, this revision wins.

This supersedes `2026-09-26-mega-alpha-parent-handoff.md` as the current checkpoint. That file is still
the reference for older history, pins and the v1-v4 runs. Rolling ledger:
`.superpowers/sdd/mega-alpha-20260926/progress.md` (top section = this session). Task briefs, reports,
reviews and studies are in the same directory.

## 1. Stop state

The owner wrote: "stop here and write a detailed handoff markdown file for the next parent agent + a
goal prompt". All child agents were stopped with TaskStop:

- T2 fix round
- T5 fix round
- T7
- T9
- the T1 review

No build or numerical process owned by this session is running. Every worktree listed below is
tracked-clean. Do not resume from this document alone; the owner's next prompt authorizes resuming.

## 2. Objective and standing rules (revised 2026-09-27 by the owner)

**The deliverable is a real out-of-sample NAV backtest of a mega-alpha portfolio:** many high-quality
atx-engine alpha-DSL subalphas, admitted, signed and weighted on TRAIN only, combined into one
market-neutral book that is rebalanced daily, simulated at $1bn NAV with declared costs and borrow,
frozen, then run once on validation (2023-2024). Every task is judged by whether it moves that run
closer. Work that only improves diagnostics for configurations we will not ship is out of scope.

Targets (measured on the frozen validation NAV run, primary scenario S2):

- annualized NET Sharpe >= 1 after S2 trading costs and `swap-fin-v1` financing (§2b), on excess
  returns (252 sessions/yr);
- **daily rebalancing**: a decision every session (`--cadence 1`), executed at the next session by the
  NAV simulation. US equities are an efficient market and the measured alpha decays within days;
  monthly or weekly cadences are not candidates for the deliverable. They may appear only as labeled
  diagnostics;
- **combined-book turnover** (§2a): mean daily one-way turnover <= 20% of GMV and p95 <= 30% of GMV,
  deployment session excluded;
- **individual alphas** may turn over faster, up to 70% of GMV per day standalone, on the assumption
  that combining them nets opposing trades. That assumption is measured, not assumed (netting ratio,
  §2a);
- thousands of stocks (~3000/day liquidity cohort); $1bn NAV; 2020+ data.

**The 30%/calendar-month turnover target is retired.** It is not a deal breaker, a selection
constraint or a pass/fail flag. Keep reporting monthly turnover for continuity only. The NAV summary's
`months_le_0.30` / `meets_turnover_target_*` fields are legacy reporting until T4 adds the daily fields.

These are targets to measure honestly. **None is met yet.**

Priorities, in order:

1. alpha quality (more and better subalphas, especially orthogonal sources);
2. composition (TRAIN-fit weights);
3. daily construction (neutralize + no-trade band, tuned for net Sharpe under the daily turnover limits);
4. the NAV evaluator and runtime that make the above measurable.

Exhaustive corporate-action registration and detailed realism stay deferred.

Rules:

- Subagent-driven development on Opus 5.5 child agents; **not TDD** (write postimplementation fixtures).
- Root alone builds and runs real data.
- Every real run <= 180 s under `scripts/run_bounded_research.py`.
- No pushes, warehouse writes or broker actions. Never mutate `C:/atx` (read-only inspection is okay).
- Keep 2025+ reserved.
- Select signs, weights and policies on TRAIN (2020-2022) only. Validation (2023-2024) is used once,
  per frozen configuration.
- Owner mid-session instruction: **do not let RAM limits slow progress; if blocked, improve
  efficiency / make progress incremental** instead of waiting.
- The daily turnover limits and the per-alpha ceiling are declared now, before any NAV result. Do not
  relax or tighten them after seeing results.

## 2a. Turnover standard and definitions (web research 2026-09-27)

**Metric.** Daily one-way turnover `tau_t = sum_i |fill$_{i,t}| / GMV_t`, where
`GMV_t = sum_i |h_{i,t}|` (long $ + short $) before trading at t. This is the same quantity as
WorldQuant's "dollars traded / booksize". The NAV replay's current `one_way_turnover` is
`sum|fills| / NAV_pre`; at the target's gross-1 book (sum|w| = 1) the two coincide up to drift. T4 adds
the GMV-denominated column so drift does not matter. Some sources use half this value (buys only or
lesser of purchases/sales). Numbers below use the sum-of-|fills| convention where the source's convention
is known: WorldQuant data is already (buys+sells)/(long+short), and the mutual-fund rates are doubled.
The practitioner rule is quoted as-is.

**What is standard for US equity long/short:**

| Source | What it measures | Daily turnover |
| --- | --- | --- |
| Kakushadze & Tulchinsky 2016, *Performance v. Turnover: A Story by 4,000 Alphas* (J. Investment Strategies 5(2); arXiv 1509.08110) | 4,000 real WorldQuant US-equity alphas (dollar-neutral, liquid stat-arb universe); turnover = (buys+sells) $ / (long+short) $ | min 5.4%, decile edges 10.9 / 16.6 / 21.7 / 25.9 / **29.7 (median)** / 33.8 / 38.0 / 44.0 / 60.7%, max 149%; holding periods 0.7-19 days. Return has no significant dependence on turnover; cents-per-share ~ 1/turnover |
| Kakushadze 2016, *101 Formulaic Alphas* (Wilmott; arXiv 1601.00991) | 101 real alphas, US equities | holding 0.6-6.4 days; mean pairwise correlation 15.9% |
| WorldQuant BRAIN alpha submission tests | single-alpha admission window | turnover 1%-70% per day |
| Avellaneda & Lee 2010, *Statistical Arbitrage in the U.S. Equities Market* (Quant. Finance 10(7)) | PCA/ETF-residual stat arb | daily EOD signals, ~7-day mean-reversion time, 5 bps per trade, 2+2 leverage, net Sharpe ~1.4-1.5 (1997-2007) |
| Practitioner stat-arb guide (quantt.co.uk) | risk/ops rule of thumb | "limit daily turnover to 30% of the portfolio"; slower books 20-40%/week (~4-8%/day); holding 1-20 days |
| Equity-market-neutral mutual funds (SEC 497K, FY2010: Rydex EMN 287%/yr, American Century EMN 140%/yr) | slow end of the spectrum; SEC rate = lesser of purchases or sales / average assets, doubled here | ~1.1-2.3%/day. **The retired 30%/month (~1.4%/day) target sat here.** |

The Kakushadze papers state explicitly that the traded object is the combined "mega-alpha", not the
individual alphas, because combining not-too-correlated alphas crosses opposing trades internally and
cuts cost. See also Kakushadze 2014, *Combining Alpha Streams with Costs* (arXiv 1405.4716), and
Kakushadze & Yu 2017, *Decoding Stock Market with Quant Alphas* (arXiv 1708.02984). The second suggests
suppressing the weights of high-turnover alphas when the combined turnover is too high. Gârleanu &
Pedersen 2013 (J. Finance 68(6)) justify partial trading toward an aim portfolio that overweights
slower-decaying signals.

**Declared limits (owner ruling 2026-09-27):**

- **Mega-alpha book:** mean `tau` <= 20%/day and p95 `tau` <= 30%/day over the scored window, deployment
  session excluded; forced exits count. 20%/day is the middle of the mid-frequency range after netting,
  below the median single alpha (~30%). 30%/day matches the common practitioner ceiling. At $1bn gross,
  20% is ~$200M/day; S2's 1%-of-ADV participation cap still rations small names, and S2 cost, not the
  ceiling, should decide how fast we trade.
- **Individual alphas:** standalone `tau_k` (neutralized gross-1 book, daily full rebalance, TRAIN)
  <= 70%/day. That is the BRAIN ceiling and about the 90th percentile of the 4,000-alpha sample. An alpha
  above it must be smoothed in DSL (decay / time-series mean) before admission, not dropped silently.
- **Netting ratio** `NR = tau_book(daily full, no band) / sum_k w_k tau_k` (w = normalized composition
  weights), reported on TRAIN for every blend. NR < 1 is the netting assumption; report it whatever its
  value.
- **If the book breaches the ceiling on TRAIN,** use levers in this order, each preregistered before
  use: (1) no-trade band width, (2) partial trade fraction toward target, (3) turnover-suppressed
  composition weights. Never change the cadence.
- Current evidence (diagnostic Python; Sharpe on the 2022 TRAIN holdout, turnover averaged over all
  TRAIN; flat 6 bps + 300 bps borrow, no impact): the MV blend rebalanced daily in full ran
  ~169%/month, about **8%/day**, already inside the ceiling. It earned
  gross/net Sharpe 1.19/0.56, against 0.69/0.31 when forced into ~28%/month. The retired monthly budget
  was the binding constraint.

## 2b. Financing and borrow model (research 2026-09-27; replaces flat 300 bps as primary)

**How a PB portfolio swap is financed.**

- The fund posts its NAV as collateral, which earns about the benchmark (SOFR / OBFR / Fed Funds).
- **Long swap:** the fund receives the total return and pays benchmark + a long spread on the notional.
- **Short swap:** the fund pays the total return (dividends included) and receives benchmark minus a
  short spread minus the stock borrow fee. The PB borrows and sells the hedge itself and passes the loan
  cost through the rate, which can go negative for hard-to-borrow names.
- Physical PB economics are the same: margin debit at benchmark + spread; short rebate = benchmark −
  spread − fee.

For a dollar-neutral book the benchmark cancels, up to r × net exposure, and the collateral earns
about the benchmark. So the excess return is:

`gross alpha − s_L × long$ − sum_i (s_S + fee_i) × short$_i`

The NAV replay's "cash 0%, no rebate" is excess-return accounting, so **its structure is right**. What
is wrong is the level, and two missing pieces.

**What the current flat 300 bps on shorts gets wrong:**

- **Too harsh for most names.** The typical GC fee is about 30 bps (S3 Partners, 2022-23); D'Avolio
  (JFE 2002) found 17 bps on 91% of borrowed names in 2000-01. 83-84% of US short interest is GC, and
  only ~5% of short interest is in names above a 1% fee.
- **Too lenient exactly where our signals short.** Specials are ~9% of borrowed names, averaging 4.3%
  with a tail to tens of percent (D'Avolio). High-fee names are 12% of stock-dates. Across 162 anomalies,
  the long/short return comes from the short leg, and it is about zero after borrow fees (Muravyev,
  Pearson & Pollet, J. Finance 80(6) 2025). A flat rate hides the names that reversal, illiquidity,
  IVOL and SI alphas short.
- **No long financing spread.** A TRS runs about benchmark + 30-75 bps. There is also no PB spread on
  the short leg.
- **Day count.** USD financing legs accrue ACT/360; the replay uses /365. That understates by ~1.4%,
  which is minor.

**Declared primary `swap-fin-v1`** (owner-directed, before any NAV result exists):

- **Long leg:** 40 bps/yr on long $.
- **Short leg:** 20 bps/yr PB spread + a per-name tier fee on short $. GC all-in is therefore about
  50 bps.
- **Tiers:** as of each decision, via engine `estimate_borrow_tier`
  (`atx-engine/include/atx/engine/cost/borrow_tiers.hpp`). Flags: market cap < $1bn, raw price < $5,
  SI / shares_out > 10%, first seen < 365 days. 0 flags → GC, 1 → warm, 2+ → special.
- **Tier rates:** recipe overrides GC 30 / warm 100 / special 500 bps, from the empirical anchors above.
  The engine defaults (27.5 / 300 / 2,750 bps) are the stress.
- **Predictors:**
  - market cap: `mktcap_lagged`, else `shares_out` × raw close;
  - SI ratio: `si_shares / shares_out`. shares_out >= float, so this understates the ratio (declared);
  - IPO age: first present session in the role. Names present at the role start count as seasoned;
  - any missing predictor → warm, counted.
- **Locate:** no new or increased shorts in special-tier names (no-locate assumption). A short that
  migrates into special is charged at the special rate until it exits.
- **Accrual:** ACT/360 on calendar days.
- **Stress scenarios:**
  - `flat-300-v0`: the legacy scenario, 300 bps on all shorts, no long spread;
  - `engine-tiers-v1`: engine default tier rates, with the same spreads and the same locate rule.
- **Report per scenario:** financing $ by leg and tier, short-$ share by tier, blocked short $, and the
  missing-predictor count.

Rough size at gross 1, assuming 85/10/5% of short $ in GC/warm/special: long 0.5 × 40 = 20 bps; short
0.5 × (20 + ~60) = 40 bps. That is ~60 bps/yr of NAV against 150 bps under flat 300. It will be less
favorable if the alphas lean into hard-to-borrow shorts, and the locate block can also remove gross
alpha. **Do not pre-credit this. Measure it.** The TRAIN study's net 0.56 used flat 300 and is not
comparable.

## 3. Worktrees and Git state at stop

Every worktree listed here is tracked-clean. Root builds only in pool-2.

| Worktree | Branch @ HEAD | Content / status |
| --- | --- | --- |
| pool-2 (root, lease `aes-codex-integ-20260925`) | `feat/aes-codex-integration-20260925` @ `c615ce36` (+ this handoff commit) | Integration. All imports below. |
| pool-3 | `feat/mega-alpha-weights-20260926` @ `aad9779a` | T9 was stopped with NO commits. Redo from brief. (Its lease file still names the old run; T3 was done here earlier.) |
| pool-4 | `feat/mega-alpha-runner-fields-20260926` @ `6d85ac2a` | T7 was stopped with NO commits. Redo from brief. |
| pool-5 | `feat/mega-alpha-nav-20260926` @ `8e25992d` | T2 source, imported. The fix round was stopped with no commit. |
| pool-7 (leased `mega-alpha-lib-20260926`) | `feat/mega-alpha-library-v2-20260926` @ `f2d5fb97` | **T5 fix round 1 committed, NOT imported, NOT reviewed.** |
| pool-8 (leased `mega-alpha-fields-20260926`) | `feat/mega-alpha-fields-20260926` @ `cf36d83c` | T6 imported. |
| pool-6 | baseline | leave alone |

Old branches (`feat/w0-*`) in pools 3/4/5 are intact. The new task branches were cut from root HEADs.
Pools 7 and 8 have a failed default `dev` configure: the atx-vol install target is missing. This does
not matter; they are source-only lanes.

## 4. Completed this session (source -> root import, evidence)

| Element | SHAs | Evidence |
| --- | --- | --- |
| VM arena release + fixture | `8527a839`->`050c0efc`, `23f1541b`->`bfb6b859` | Build `recent-strategy-targets-v1` 36.62 s Jobs2 (6 TUs/5 links). IC 35/35 in 5.141 s; target replay 7/7 in 0.198 s. |
| Pool-3 v3/v4 archive | `58c21bb0`->`470eb1b6` | 46/46 files verified. |
| TRAIN saved-blend export v6 | exe `03607890...` @ `bfb6b859` | v5 stopped on host RAM (preserved). v6 completed in 94.70 s at 899 MB. Orientations (canonical `4a3e8004`), planned targets and daily IC byte-identical to v2. Blend manifest `51740eff...`. |
| Fixed policy comparison + frozen validation | report `.superpowers/sdd/strategy/2026-09-26-saved-blend-policy-qualification.md`, archive `saved-blend-policy-20260926/`, commit `5c9cbaed` | Audit `build-equity/audit-target-replay.py` (SHA `d8e5fb27`) PASS: 2,268 exact baseline f64 values. Budget-v2 mean monthly target change: TRAIN 35.32%->30.66%; frozen VAL 34.44%->30.24% (forced-exit breaches disclosed). Gross 0.90->0.85. |
| T3 price-risk exposures + neutralize | `e4a869b7`->`b9cf4023`, CMake `429cbe43` | Build 21.9 s; target tests 15/15. Review APPROVE (0C/0I/8 minor, `task-T3-review.md`). **Task complete.** |
| T1 candidate cache + pinned weights | `09a18ec3`/`a7fd1c02` -> `af8c38ee`/`445e828d` | Build 32.3 s; IC tests 42/42 in 9.68 s. The real run `mega-v1-train-cache-b` completed in 71.16 s: 37 cache hits + 11 cold, outputs byte-identical to v6. **Task review was started and then stopped: redo it.** |
| SHA-256 Debug optimization | root `6d85ac2a` (`atx-core/CMakeLists.txt`, scoped `/O2` on `sha256.cpp`) | Cache write 2.5 s -> 0.24 s per 52 MB; role load 9.4 s -> 1.4 s; save 4.35 s -> 0.33 s. |
| T5 library v2 (original) | `2c92d658`->`c47ffdaa` | 96 candidates, SHA `0c7f3059...`; `--plan-only` OK (8 slots, lookback 314). **Review found 4 Important issues. Do NOT measure `c47ffdaa`'s library.** |
| T2 NAV replay ($1bn, S1/S2/S3, stale-carry K=5) | `8e25992d`->`c4ba9c80`, CMake `97e6b392` | Build 31.6 s; target tests 24/25. **One fixture fails:** `StrategyNavReplay.ConstantPricesNoCostReproducesTargetReplayPlanned` at test :309, expects `forced_turnover > 0` and gets 0. The fix round was not done. |
| T6 PIT field producer | `cf36d83c`->`c615ce36` (`atx-engine/tools/prepare_research_fields.py`, SHA `892bd33f...`) | Synthetic 9/9. Real TRAIN run `build-equity/recent-fast-train-2020-2022-v1-fields-v1/` completed in 22.03 s, peak 604 MB, manifest SHA `519fc9b2064bb74aa8ca885c09a87f428393708e1e9ac71bb9ea67bfcbb6a875`. **The validation-role fields run has not been done.** |

TRAIN field member coverage:

| Field | Coverage |
| --- | --- |
| `si_shares`, `si_dtc` | 99.93% |
| `iv_atm_21d`, `iv_atm_63d`, `iv_atm_126d` | 92.9% |
| `earn_recent` | 99.97% |
| `shares_out` | 99.67% |
| `mktcap_lagged`, `size_grp` | 75.1% |
| `is_common` | 100% |
| `mkt_ret` | 99.88% |

Caveats:

- `is_common` is not PIT (2026 snapshot); use it only as a coarse filter.
- The spine has mild survival leakage.
- IV vendor vintage is unproven.
- FINRA data before 2021-06 is a later republication.
- `hv_*` was dropped: the vendor HV duplicates IV.

Ledger/doc commits: `ff1b499e`, `bb5bc25b`, `7871c3cc`, `e44a44d5`, `b23d463e`, `aad9779a`, plus this
handoff.

## 5. Key measured findings (the honest state)

All-days observed-component rough return of the fixed 48-alpha blend:

- gross annualized ratio about 0.36 on TRAIN (in-sample signs) and 0.34-0.38 on VAL;
- about 1.9%/yr at about 5% volatility on VAL.

Only a third of days are return-complete in the target proxy (up to 232 missing names/day), so that
proxy cannot give Sharpe. **This is why the NAV replay exists.**

The blend has negative market beta: -0.12 TRAIN, -0.20 VAL.

**TRAIN-only studies** (scripts in `.superpowers/sdd/mega-alpha-20260926/studies/`; diagnostic Python,
not the evaluator):

- Neutralizing against beta252/vol63/logADV63 raises gross from 0.36 to 0.55 at cadence 5 / fraction 0.25.
- Per-candidate neutralized daily factor returns, with signs and weights fit on 2020-21 and a 2022
  holdout, give these holdout gross Sharpe values:

  | Composition | Holdout gross SR |
  | --- | --- |
  | Equal | 0.72 |
  | Inverse-vol | 0.84 |
  | MV-shrink 0.9 | 1.23 |
  | MV-shrink 0.9, nonneg-clipped | 1.28 |

  Mean pairwise factor correlation is 0.18. Seven composition methods were compared on the 2022
  TRAIN holdout; count those as composition trials.
- Construction on the MV blend (`construct_study.py`). Sharpe is on the 2022 holdout. Turnover is
  averaged over all TRAIN sessions; the daily column is monthly / 21. Costs per session:
  `sum|dw| x 6 bps` flat, plus `short$ x 300 bps / 252`. There is no market impact, participation cap
  or $1bn sizing (weights only), and forced exits are not charged. Estimated drag at daily full:
  ~1.2%/yr trading + ~1.5%/yr borrow. S2 adds sqrt impact on top of the same 6 bps, so expect a lower
  daily net on S2.

  | Construction | Gross / net SR | Turnover/month | ~Turnover/day |
  | --- | --- | --- | --- |
  | **Daily full (cadence 1, fraction 1)** | **1.19 / 0.56** | 169% | **8.0%** |
  | Cadence 5, full | 1.07 / 0.49 | 131% | 6.2% |
  | Cadence 1, fraction .10 | 0.70 / 0.19 | 77% | 3.7% |
  | Cadence 21, full | 0.58 / 0.10 | 78% | 3.7% |
  | Cadence 5, fraction .25 | 0.60 / 0.13 | 56% | 2.7% |
  | Cadence 5, fraction .25, no-trade band 1/N | 0.69 / 0.31 | 28% | 1.3% |

- **Conclusions (revised 2026-09-27):**
  - The alpha decays within days. Every slower construction lost net Sharpe. Daily full rebalancing was
    the best net result (0.56) at ~8%/day, well inside the new 20%/day ceiling. Forcing the book into
    30%/month threw away about half the net Sharpe; that budget is retired.
  - Control trade size inside the daily cadence with a no-trade band. The band beat partial adjustment at
    cadence 5, and daily partial adjustment (fraction .10) was poor. **A daily band has not been
    measured yet.**
  - Flat 300 bps borrow costs about 0.3 Sharpe. That rate is wrong in both directions (§2b) and is
    replaced by `swap-fin-v1`.
  - These use linear 6 bps. S2 (sqrt impact, 1% ADV cap at $1bn) has not been measured at daily
    cadence; it is the real test of how fast we can trade.
  - The remaining gap to net 1.0 is **alpha quality** (more orthogonal sources: SI, IV, then
    fundamentals) plus **cost-aware daily construction**.
- Preregistered choices (ledger rulings; the construction line was revised by the owner 2026-09-27):
  - composition `mv-shrink-0.9-nonneg-v1`, fit on full TRAIN;
  - construction: **cadence 1 (daily)**, neutralize `price-risk-v1`, plus a no-trade band with
    `band_multiple` in {0, 0.5, 1, 2} (x 1/N) at fraction 1.0. Pick one on TRAIN by S2 net Sharpe,
    subject to the §2a limits. That is 4 construction trials; record them;
  - financing (revised 2026-09-27): primary `swap-fin-v1` (§2b). Stresses: `flat-300-v0` and
    `engine-tiers-v1`. This replaces the earlier "add an SI-tiered scenario next to flat 300 bps".

## 6. Pending tasks — critical path to the out-of-sample run (briefs in `.superpowers/sdd/mega-alpha-20260926/`)

The end state is item 11. Items 3-8 are disjoint lanes. Dispatch them in parallel as soon as T2 is
fixed; root alone builds and runs data.

1. **T2 fix round** (pool-5): fix the single failing fixture described above. Decide whether it is a
   fixture problem (no forced exit in the synthetic data) or a code problem. Rebuild with
   `atx-equity-strategy-targets,atx-impl-strategy-target-tests`, then dispatch a T2 task review.
   Design: `nav-backtest-design.md`. Accepted design deviations are in `task-T2-report.md`:
   - the daily CSV has 756 rows with a `return_observation` flag;
   - participation p95 is a histogram upper bound;
   - ex-deployment turnover flags are added.
2. **First NAV run: TRAIN only, daily.** Run `atx-equity-strategy-targets nav --combined ... --role ...
   --rule baseline-v1 --cadence 1 --trade-fraction 1` on the TRAIN v6 blend (`51740eff`). It serves as
   the evaluator smoke test and the first S2 read at daily cadence. Report:
   - S1/S2/S3 net Sharpe, HAC t, drawdown, calendar-year returns;
   - daily turnover mean/p95, from the daily CSV until T4 lands;
   - capacity: capped fills, unfilled $, participation;
   - stale/write-off events.

   **Do not run validation NAV on the v3 blend, on monthly-budget-v2, or on any cadence > 1.** Those
   configurations will not ship, and validation is spent once, on the frozen daily mega-alpha (item 11).
   A cadence-5 TRAIN run is allowed only as a labeled diagnostic.
3. **T4** (brief `task-T4-brief.md`, revised 2026-09-27, NOT dispatched): must work at cadence 1.
   - neutralize (`price-risk-v1`, exposures computed once per decision, skip rebalancing if
     amplification > 5 or excluded share > 0.5);
   - `--band-multiple`, in the target and NAV replay;
   - GMV-denominated daily turnover column and summary stats (mean/median/p95/max, ex-deployment);
   - declared daily ceiling flags (`--daily-turnover-mean-max 0.20`, `--daily-turnover-p95-max 0.30`).

   Intended for the T2 agent after its fix round.

   **3b. T10 financing model** (brief `task-T10-brief.md`, NOT dispatched): implement `swap-fin-v1`
   plus the `flat-300-v0` and `engine-tiers-v1` stresses in the NAV replay (§2b):
   - per-name as-of borrow tier from role fields (`nav --fields PATH --fields-sha256 SHA`, T6
     manifest);
   - long spread;
   - ACT/360 accrual;
   - special-tier locate block;
   - financing breakdown by leg and tier.

   It edits the same files as T4, so it goes to the same owner right after T4 (sequential, not
   parallel). It needs the T6 fields for each role it runs on. Item 2's smoke run may use the legacy
   scenario; every run after T10 uses `swap-fin-v1` as primary.
4. **T5 fix round 1 review**: pool-7 `f2d5fb97` fixes I1-I4 (split-adjusted volume, `mkt_ret` field
   instead of `vec_avg`, residual-Sharpe momentum, non-duplicative illiquidity/ivol/MAX) and the
   minors. Unreviewed. The report's "Fix round 1" section may be incomplete (the agent was stopped
   while appending). Review it, then import. Its library needs T7 because it references `mkt_ret`.
5. **T6 validation fields**: run the producer for the validation role, with the same command shape as
   TRAIN (see `task-T6-report.md`). Then review T6. Item 11 needs these fields.
6. **T7** (brief `task-T7-brief.md`; restart on pool-4 at `6d85ac2a` or the new root HEAD): runner
   `--train-fields/--validation-fields`; load only referenced fields; the cache key covers the fields
   manifest for field-referencing candidates.
7. **T9** (brief `task-T9-brief.md`, addendum 2026-09-27; restart on pool-3):
   `atx-impl/tools/fit_composition_weights.py` implementing `mv-shrink-0.9-nonneg-v1` on full TRAIN from
   cached signals, producing pinned weights JSON. It also reports each candidate's standalone daily
   turnover `tau_k`, flags any above 70%/day, and reports `sum_k w_k tau_k` for the netting ratio (§2a).
8. **T1 task review**: redo it; the package is `review-T1.diff`.
9. **Library v3 + quality admission.** Add short-interest families (SI ratio, days-to-cover, delta SI),
   implied-vol families (level, term slope, IV change, IV vs realized from close) and a size tilt to the
   fixed v2. Choose families from priors and freeze them before measurement. Freeze an admission screen
   with the library; it runs on TRAIN only and every admission decision counts as a trial in the
   ledger:
   - standalone `tau_k` <= 70%/day (a faster alpha is smoothed in DSL before the freeze, not dropped);
   - oriented neutralized factor-return Sharpe > 0 on 2020-21, with the same sign on the 2022 holdout;
   - |correlation| <= 0.7 with an already-admitted alpha's factor returns. When a pair is closer, keep
     the one with the higher 2020-21 Sharpe.
10. **TRAIN mega-alpha.**
    1. Run with the cache. The runner chunks, so a guard stop leaves cached signals: rerun.
    2. Fit weights (T9) and save the blend with pinned weights.
    3. Run NAV at cadence 1 with neutralize + band in {0, 0.5, 1, 2}/N.
    4. Pick the setting by S2 net Sharpe subject to the §2a limits. Report the netting ratio.
    5. If no setting meets the limits, use the §2a lever order.
    6. Freeze: library SHA, weights SHA, orientations, construction, and the financing scenarios
       (`swap-fin-v1` primary, two stresses; already declared in §2b).
11. **Validation once: the deliverable.**
    1. Produce the T6 validation fields.
    2. Run the validation runner with the frozen orientations and pinned weights, and save the blend.
    3. Run NAV with the frozen construction.
    4. Report S2 net Sharpe, HAC t, drawdown, calendar-year returns, daily turnover mean/p95 against
       the limits, netting ratio, capacity diagnostics, financing by leg and tier, and the two financing
       stresses. Report pass or fail
       honestly.

    2025+ stays reserved until the owner authorizes a final test.
12. Next tier: fundamentals and industry via CIK (~65% coverage, 2020 export seal).

## 7. Tooling notes

- **Build:** `powershell -File C:/atx-wt/pool-2/build-equity/mega-build.ps1 -Tag <new> -Targets "a,b"`.
  The helper is ignored; it sets cwd to pool-2 (the wrapper's wrong-tree guard), admits by RAM, picks
  Jobs 2/3/4 and writes `build-equity/mega-<tag>-receipt.json`. Receipt tags already used:
  - `t1-a` (failed argument split)
  - `t1-b`
  - `t3-a`
  - `sha-o2`
  - `t2-a`

  Configured provenance updates only on reconfigure, so record the source SHA separately.
- **Guard Python:** `C:/Program Files/Python312/python.exe`. The bash `python` is the atx-db venv
  without psutil.
- **DLL PATH** for native runs: `C:/atx-cache/vcpkg_installed/x64-windows/debug/bin;.../bin`.
- **Ruling:** the real-run guard uses `--min-free-mib 512` (RSS 1536 MiB, 180 s unchanged), per the
  owner's RAM instruction. The earlier 768 MiB floor stopped v5 on host memory owned by other processes.
- **Candidate cache:** `build-equity/mega-candidate-cache/<role-sha>/` holds all 48 v1 TRAIN signals
  (~2.5 GB). Library v2/v3 reruns reuse the v1 entries through identical `dsl_sha256`.
- **IC run budget:** a warm 48-candidate TRAIN run takes about 71 s, of which IC is about 29 s and
  composition about 14 s. A 96-candidate run takes about 125 s warm. If composition or IC become the
  bottleneck, optimize those stages; do not wait on RAM.
- **Target replay audit:** `build-equity/audit-target-replay.py` (reviewed, SHA `d8e5fb27`).

## 8. Rulings made this session (each: decision — cost if wrong)

- Guard free floor 768 -> 512 MiB — possible host paging; no correctness impact.
- New task branches were cut from root HEADs in pools 3/4/5, keeping the old branches — none.
- NAV design accepted: stale-carry K=5, S2 $1bn sqrt-impact primary, flat 300 bps registered borrow,
  no cost grid — some cost realism is deferred.
- T2 deviations accepted (756 daily rows with flag, p95 upper bound, ex-deployment flags) — cosmetic.
- Role extra-fields path (Python producer + runner load) instead of a new price projection, and a
  `mkt_ret` field instead of changing the VM `vec_avg` opcode — v2/v3 depend on T7.
- T5 review findings fixed before any measurement — none; no data spent.
- Composition `mv-shrink-0.9-nonneg-v1` and construction neutralize + band chosen on a TRAIN-internal
  2022 holdout (7 composition methods compared) — mild TRAIN-holdout selection; validation untouched.
- An SI-tiered borrow scenario is to be added next to flat 300 bps — must be declared before viewing
  results. **Superseded 2026-09-27 by `swap-fin-v1` (§2b).**
- Scoped `/O2` on `atx-core/src/sha256.cpp` — one TU; Debug CRT/asserts unchanged.

Rulings added 2026-09-27 (owner-directed revision):

- **Owner:** daily rebalancing (cadence 1) is required. The 30%/month turnover target is retired.
  Combined book: mean <= 20% and p95 <= 30% of GMV per day. Individual alphas: <= 70% per day. The
  netting ratio is reported. — S2 cost drag at daily cadence is larger than at monthly; the band and
  partial-fraction levers exist for that, and S2 measures it honestly.
- Validation NAV runs on the v3 blend and on monthly-budget-v2 / cadence-5 policies are cancelled (was
  pending item 2). — We lose a comparison number for configurations that will not ship; the deliverable
  is unaffected, and validation trials are saved.
- The preregistered daily construction set is band in {0, 0.5, 1, 2}/N at fraction 1.0. — It may miss
  a better partial-fraction setting; lever (2) stays available if the limits are breached.
- Library v3 admission defaults: `tau_k` <= 70%/day, sign stable 2020-21 -> 2022, |rho| <= 0.7. — This
  may drop a useful correlated alpha; MV-shrink would have down-weighted it anyway.
- **Owner-directed:** financing primary `swap-fin-v1` (§2b) replaces flat 300 bps, declared before any
  NAV result. — If the tier map misclassifies names, fees are wrong for those names; the two stresses
  bound this, and there is no lender fee data to fit against (declared-unfitted).
- Disclosure: the turnover change was made before any NAV result exists, and it was informed by the
  TRAIN construction study. The v3 blend's validation target-proxy numbers (monthly policies, §5) were
  read in an earlier session. Record them as prior use of 2023-2024 in the final report; they do not
  involve the daily mega-alpha that will be frozen.

## 9. Interpretation limits

- There is no NAV Sharpe yet: T2 is built but one fixture fails, and it has not run on real data.
- No daily-cadence NAV run and no netting ratio exist yet. Every daily-cadence number above is a
  linear-cost Python diagnostic on the TRAIN 2022 holdout.
- Target-proxy returns cover a non-random third of days.
- Python studies are diagnostics that approximate the C++ path.
- Liquidity cohort, not certified common stock; vendor vintage unverified; no $1bn capacity claim.
- The objective is NOT met.
