# R-1 gross investigation: why all-rows gross goes from .982 (B0c) to 1.067 (R-1)

Read-only. Tree `C:/atx-wt/pool-2` at `97a98978` (clean). No file in the repository changed, nothing built, no
data job run. The only computations were two read-only scratch scripts over existing outputs (below). No return,
P&L, NAV level, Sharpe or IC statistic of R-1 was read (see section 6).

## 1. Verdict: (b) DESIGN

The code does what the registration says. The registered construction (fixed `L 1.247`, partial move theta .05
toward `L x desired`, plan line 672) gives R-1's slower aim a higher book gross. So R-1 fails the registered
mechanics criterion (all-rows gross in [.90, 1.05]) as registered. I found no defect in the composition (Python
fitter or C++ kernel) or in the NAV path.

The extra gross is **not** a scale effect. The NAV re-ranks the combined score and sets the desired target to gross 1,
so no composite scale reaches the book. It is a **speed** effect. The book follows the aim at 5% of the gap per day,
so its gross is the aim's gross times a tracking ratio, and that ratio rises when the aim moves more slowly.
ew-theme-std-v1 makes the aim much slower: the day-over-day L1 change of the gross-1 target falls by 35%. The
tracking ratio therefore goes from .787 to .856 of L, and 1.247 x .856 = 1.067.

The extra gross is in the **planned target book**, not in holdings drifting above target. In both cells executed
gross / planned gross is 1.0011.

## 2. Code path (file:line, tree `97a98978`)

Composition (w pass):
- `atx-impl/tools/composition_rules.py:74-82`: RULE_TEXT, the registration verbatim.
  - `:183-189` `tier_weights`: w_k = score_k / (T x sum of the theme's scores); rules 1 and 3.
  - `:192-215` `member_cap`: rule 4, the excess pro rata to other themes' uncapped members, repeated to a fixed point.
- `atx-impl/src/strategy_ic_composition.cpp:290-318` `add_standardised`: each member's centred tied rank,
  times sign x w_k, is summed into its theme plane. NaN means no member present (rule 1, fixed divisor, missing
  member = 0).
- `:337-343` `finish`: `cb::add_group_rerank(plane_t, ..., W_theme, out.signal)` per theme.
- `atx-engine/include/atx/engine/combine/group_rerank.hpp:71-86`: each theme composite is re-ranked (centred tied
  rank) over names with a present member, then added as W_theme x rank (rules 2 and 3).
- W_theme = the sum of the theme's capped w_k (rule 4).
- All of this matches RULE_TEXT. The output is `train_combined.f64`, the NAV's `--combined`.

Target and sizing (NAV, `atx-equity-strategy-targets nav --rule aim-partial-v5 --aim-leverage 1.247
--trade-fraction .05 --dust-multiple .1 --exit-rate .05 --neutralize price-risk-v1 --locate-in-aim
--order-basis delta --warm-start-sessions 60`):
- **Same NAV run except the input.** The B0c and R-1 NAV argv (receipts) differ only in `--combined`, its sha256 and
  `--output`. Both use the same executable sha256.
- `atx-impl/src/strategy_target_replay.cpp:208-242`: `member_ranks` + `demean_gross_one` set the desired target to
  the centred tied rank of the combined score over members, demeaned, **gross 1**. Any composite scale is erased here.
- `:467-470`: locate-in-aim sets special-tier negative desired weights to 0.
- `:471-500` with `atx-impl/src/strategy_price_exposures.cpp:575-580`: price-risk-v1 takes the OLS residual and
  **rescales it to the entry gross** (`scale = stats.gross / stats.residual_gross`). Neutralisation does not change
  gross.
- `atx-impl/src/strategy_nav_replay.cpp:970`: current weights = held dollars / post-trade NAV.
- `:977` `plan_weights` calls `strategy_nav_v7.cpp:428-430, 481, 530`. That reaches `detail::update_weights` (no
  scaler, no v6, no spo), which runs `strategy_target_replay.cpp:279-333` `aim_partial_weights`:
  - `:310` aim = `aim_leverage x desired`;
  - `:316` next = current + theta x gap, or keep the weight if |gap| <= .1 / N_d (dust);
  - nonmembers decay 5% per day;
  - `:325` planned gross.
- There is **no gross normalisation, gross cap or re-scaling of the book anywhere after this point**. The only
  scale is L x a gross-1 target.
- `strategy_nav_replay.cpp:995` writes planned_gross to the CSV. `:1021` sets
  gross_leverage = (long$ + short$) / NAV after EXECUTE. That is the mechanics metric.

So the book is close to an exponential moving average (EMA) of the aims, with weight .05 per day (mean lag about 19
sessions). Its gross is L x (gross of desired) x the tracking ratio. The tracking ratio depends only on how
persistent the desired target is from day to day.

## 3. Evidence (mechanics numbers only)

### 3a. NAV, S2 daily CSV, whitelisted mechanics columns

Command (scratchpad):
```
rtk proxy python C:/Users/natha/AppData/Local/Temp/claude/c--atx/519f2078-c0ed-4462-b555-5bf9a7bd256c/scratchpad/mech_daily.py
```
The script keeps only the WHITE list of columns: session_ns, decision, rebalance, executed, one_way_turnover,
fills, capped_fills, planned_*, applied_fraction, gross/net_leverage, held/stale_names, cash_ratio,
one_way_turnover_gmv, neutralize*, banded_names, blocked_short_names, member_*. It drops every return, NAV and dollar
column and exits on any session at or after 2024-01-01.

All 1,006 rows, 2020-01-02 to 2023-12-29:

| metric | B0c | R-1 | R-1 / B0c |
|---|---|---|---|
| gross_leverage (executed, mean all rows) | .981964 | 1.067164 | 1.0868 |
| planned_gross (rule's plan, mean) | .978605 | 1.063556 | 1.0868 |
| gross_leverage / planned_gross (mean per row) | 1.00114 | 1.00111 | equal |
| planned_discretionary turnover | .031537 | .024292 | .770 |
| planned_forced (exits) | .002464 | .002553 | 1.036 |
| one_way_turnover_gmv (executed tau) | .034036 | .024533 | .721 |
| banded (dust-kept) names per rebalance | 207.5 | 272.6 | 1.314 |
| held_names | 1,918.2 | 1,918.9 | 1.000 |
| applied_fraction (theta) | .049901 | .049901 | equal |
| capped_fills per day (1% ADV) | 7.82 | 7.15 | .914 |
| blocked_short_names per day | 1.36 | 1.38 | ~equal |
| neutralize applied / not-attempted | 1004 / 2 | 1004 / 2 | equal |
| neutralize_amplification (mean) | 1.0995 | 1.1191 | (rescaled to entry gross: no gross effect) |
| net_leverage | +.0036 | +.0047 | |

Gross by year:

| cell | 2020 | 2021 | 2022 | 2023 |
|---|---|---|---|---|
| B0c | .9612 | .9955 | .9821 | .9893 |
| R-1 | 1.0419 | 1.0886 | 1.0622 | 1.0761 |

- The rise is uniform across years; no single episode drives it.
- Gross quantiles:
  - B0c: p05 .906, p50 .983, p95 1.069, max 1.128; rows above 1.05: 109 of 1006.
  - R-1: p05 .989, p50 1.070, p95 1.152, max 1.213; rows above 1.05: 668 of 1006.
- Row 0 (score_begin, after the 60-session warm start): .9257 vs 1.0153. The ratio, 1.097, is already the
  steady-state ratio, so the warm start is not the cause.

Reading of these numbers:
- **Executed vs planned.** Executed gross equals planned gross x 1.001 in both cells, so fills, the ADV cap, stale
  marks and price drift add nothing to the difference. The extra 8.7% is all in the rule's planned book.
- **Same limiters and caps.** theta, dust, exits, neutralisation and locate bind the same way in both cells.
- **The book sits closer to its aim.** Discretionary turnover / theta = ||aim - w||_1 falls from about .63 to about
  .49 of NAV, and more names sit inside the dust band (273 vs 208).

### 3b. NAV summary, named mechanics keys only

Command: an inline python that loads each `summary.json` and prints only these keys for the primary scenario
`modeled-1bn-stale5-v1+swap-fin-v1`:
- `locate_in_aim.zeroed_special_short_aims`;
- `warm_start.score_begin_gross_leverage`;
- `construction.v5.{aim_leverage, theta, dust_multiple, exit_rate, rate, decisions, mean_gross, mean_held_share,
  mean_net}`;
- `construction.{mean_banded_names_per_rebalance, neutralize_amplification_median/max,
  neutralize_applied/skipped_decisions, rebalanced_decisions}`;
- `exposure.{mean_gross_leverage, max_gross_leverage, mean_held_names, min_cash_ratio}`;
- `daily_turnover_gmv.{mean, p95}`.

Before that I printed the key tree, names only, with no values, to find the paths.

| key | B0c | R-1 |
|---|---|---|
| v5 aim_leverage / theta / dust / exit / rate | 1.247 / .05 / .1 / .05 / fixed | identical |
| v5 mean_gross (planned) | .98055 | 1.06567 |
| v5 mean_held_share | 1.0378 | 1.0381 |
| exposure mean / max gross | .98188 / 1.12825 | 1.06708 / 1.21281 |
| zeroed_special_short_aims (total) | 58,246 | 64,172 |
| neutralize skipped decisions | 0 | 0 |
| tau mean / p95 | .03407 / .03901 | .02456 / .02976 |

R-1 sets about 10% more special-tier short aims to zero. That lowers R-1's aim gross slightly, so it works
**against** the rise; the aim gross is not where the extra gross comes from.

### 3c. The target itself: persistence and a replica of the move rule

Command (scratchpad):
```
rtk proxy python C:/Users/natha/AppData/Local/Temp/claude/c--atx/519f2078-c0ed-4462-b555-5bf9a7bd256c/scratchpad/target_persistence.py
```

Inputs, for both cells:
- `train_combined.f64`, `train_combined_member.u8` and `train_combined_sessions.i64` (the NAV's `--combined` input:
  `mega-v8-b0bw-train-ew-1` for B0c, `mega-v8-r1w-train-std-1` for R-1; identical membership, sha c61f5b62 for both);
- the IC runner's `train_planned_targets.csv`, planned_turnover and planned_gross only.

The script asserts every session is before 2024-01-01. It does not open `train_daily_ic.csv` or any summary.

For each date it rebuilds the NAV's desired target exactly as `strategy_target_replay.cpp:208-242` does (centred tied
rank over members, demeaned, gross 1). It then runs the aim-partial-v5 move: theta .05, L 1.247, dust .1/N, exit .05,
60-session warm start. It leaves out neutralisation, locate, prices and fills.

Score rows, 2020-2023:

| metric | B0c | R-1 | ratio |
|---|---|---|---|
| daily L1 change of the gross-1 desired target | .2207 | .1436 | **.651** |
| rank correlation of desired, lag 1 | .9592 | .9847 | |
| rank correlation of desired, lag 5 | .8630 | .9334 | |
| replica book gross (mean) | 1.0459 | 1.1379 | **1.0879** (NAV actual: 1.0868) |
| replica tracking ratio (book gross / aim gross 1.247) | .839 | .912 | 1.088 |
| replica gross at the first scored row | .9867 | 1.0845 | 1.099 (NAV: 1.097) |
| replica book effective names (gross^2 / sum w^2) | 1,371 | 1,396 | 1.018 |
| replica max abs w / gross | .00126 | .00116 | .92 |
| tied share and zero share of the combined score | 0 / 0 | 0 / 0 | |
| IC runner planned gross (w pass: cadence 5, fraction .25, gross-1 target, no drift) | .8180 | .8829 | **1.079** |
| IC runner planned turnover | .02828 | .02170 | .768 |

Reading of the replica:
- From the combined score and the move rule alone, it gives R-1 / B0c gross 1.0879. The NAV gives 1.0868.
- The level offset of about .06 is common to both cells. It comes from what the replica leaves out: locate-zeroed
  aims and the neutralised residual, which moves somewhat more from day to day.
- Neither the replica nor the IC runner involves the NAV simulator, prices or fills. The IC runner's own
  partial-move plan, written by the w pass before the NAV ran, already shows +7.9% gross.
- Concentration hardly moves: the target is rank-linear in both cells, and effective names and the max weight are
  slightly *less* concentrated in R-1. The extra gross is spread across the book, not in a few names.

### 3d. Why the R-1 aim is slower: the weights file, theme masses (weights only)

| theme | members | W_theme B0b/B0c (ew-theme-v1) | W_theme R-1 (std, capped) |
|---|---|---|---|
| profitability_quality | 8 | .100 | .1079 |
| value | 5 | .100 | .1079 |
| earnings_momentum | 5 | .100 | .1079 |
| investment_issuance | 4 | .100 | .1079 |
| short_interest | 4 | .100 | .1079 |
| price_momentum | 4 | .100 | .1079 |
| low_risk | 3 | .100 | .1079 |
| reversal_seasonality | 2 | .100 | .0950 (ind_adj_rev_5 capped .05) |
| ownership_flow | 2 | .100 | .1000 (both capped .05) |
| options_implied | 1 | .100 | .0500 (iv_rv_spread capped .05) |

Under ew-theme-v1, a multi-member theme enters the blend as an average of imperfectly correlated member ranks
(missing members count as 0). Its cross-sectional dispersion is therefore below its 1/T mass, while a one-member theme
enters at full dispersion. Rule 2's re-rank gives every theme full dispersion, and rule 4's cap halves the
one-member options theme and caps the 5-day reversal member. Effective weight therefore moves away from the fast
members (iv_rv_spread, ind_adj_rev_5) toward the slow fundamental and 12-month themes.

This per-theme attribution is my inference from the registered rule and the weights; I did not measure the speed of
each theme. The aggregate slow-down (-35% daily target change) is measured (3c). The plan expected the turnover
effect ("turnover -5 to -10%", task-R-1-brief); what actually happened is -28% executed tau. Under fixed L the same
slow-down raises gross.

## 4. Why this is not a defect

- **Composition.** Python and C++ follow RULE_TEXT rules 1-5 line by line (section 2).
- **Weights.** The weights file sums to 1 (7 x .10786 + .095 + .10 + .05). W_theme moves away from 1/T only where
  rule 4 says it does ("the excess goes pro rata to the other themes").
- **NAV.** The registered construction is `aim = L x desired`, `next = current + theta (aim - current)`, with no other
  gross step: recipe text at `strategy_nav_replay.cpp:1487-1488` and code-review-v8-signal.md:77. The run used
  exactly that, at the registered L 1.247 (plan :672: "Construction cells run at L 1.247 unless the task changes
  it"). R-1's task does not change L.
- **Where L came from.** L 1.247 = 1 / post-ramp gross .8019 of the v6 ew-theme-v1 book, declared single-shot
  (`docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md:31, :282, :453`). It is calibrated on that aim's tracking ratio,
  not on a composite scale. The v8 signal review already recorded the property: "Realised gross is .774 of the aim's
  gross (.965 / 1.247): the aim moves faster than the book can follow" (`code-review-v8-signal.md:223`).
- **Mechanics gate.** v8 `mechanics` (`docs/plans/mega-alpha-v8-pitch.config.json`):
  `mean_gross_leverage_all_rows` between .90 and 1.05. 1.0672 fails it, and post-ramp 1.0735 would fail too.
- **No fix is proposed.** No code line contradicts a registered sentence, so there is no blind fix. Re-deriving L
  after this read is excluded by the plan (:963 non-goals; status note "Re-deriving L after a read is not an option
  (plan 13)").

## 5. Consequence the PM may want before ruling (inference, not a recommendation)

- At fixed L 1.247 the gross ceiling leaves B0c 1.05 / .982 = +6.9% of headroom.
- R-1's slower aim (-35% daily target change, -28% executed tau) cost +8.7% gross.
- Any registered cell that works by slowing the aim moves gross the same way: R-3 persistence gains, R-4 hold band
  (registered "turnover at least 15% lower"), and R-10 / R-11 if they run.
- [est] A cell that cuts executed turnover by roughly 20% or more through a slower aim, at L 1.247, is at risk of the
  same mechanics fail.
- The R-1 criterion (turnover per unit gross) and the gross ceiling are two faces of one mechanism under a fixed L.
- Under the PM's own option (a) in progress.md, R-1 is ledgered rejected at N 41: mechanics false whatever dSR is.

## 6. Hidden-data and return-read compliance

Of R-1's outputs I opened only:
- **NAV run receipt** (`...-run/receipt.json`): printed `command` and compared `executable_sha256`.
- **S2 daily CSV:** through `mech_daily.py`, which extracts only the whitelisted mechanics columns listed in 3a. No
  return, NAV, P&L or dollar column was printed.
- **NAV `summary.json`:** the key-name tree (names only, no values), then only the named mechanics keys listed in 3b.
- **w pass:** `train_combined.json` (manifest: dims, semantics, file hashes, cell counts), `train_combined.f64`,
  `train_combined_member.u8` and `train_combined_sessions.i64` (the score that becomes the targets),
  `train_planned_targets.csv` (planned_turnover and planned_gross only).
- **Weights:** `composition_weights.json`.

Not opened:
- R-1's `capacity_curve.csv`, `capacity/`, events CSVs and other scenario CSVs, `v7_extras.json`,
  `v7_transfer_coefficient.csv`, `recipe.json` of the NAV;
- the w pass's `train_daily_ic.csv`, `summary.json` and `orientations.json`;
- the card outputs and the monitor.

**No return, P&L, NAV level, Sharpe or IC statistic of R-1 was read.** Every session read is before 2024-01-01; both
scripts assert this, and the last session is 2023-12-29. `atx-db/` was not touched.

## Follow-up: L per cell

Same rules as above. One new scratch script: `l_linearity.py`, run as
```
rtk proxy python C:/Users/natha/AppData/Local/Temp/claude/c--atx/519f2078-c0ed-4462-b555-5bf9a7bd256c/scratchpad/l_linearity.py
```
It reads only:
- the S2 daily CSV columns gross_leverage, planned_gross, planned_turnover, capped_fills, banded_names and
  one_way_turnover_gmv of the two v6u cells at L 1 and L 1.247 (TRAIN 2020-2022);
- the B0c and R-1 combined scores, for the replica.

It asserts the 2024 seal. No return was read.

### Q1. Where L enters, and whether a spec can carry another L today

L is a spec key, `nav.leverage`, not a hard-coded value.
- **Where it is set.** It is set once in the chain root `scripts/specs/v8/base-lo3.json:199` (`"leverage": "1.247"`).
  `:207-208` hold the flag pair `"--aim-leverage", "{leverage}"` in `nav.flags`. B0c (`base-b0c.json`, parent
  base-lo3) and R-1 (parent B0c) inherit it unchanged. B0c's value is 1.247, as its NAV receipt argv shows.
- **Substitution.** `scripts/research_cycle.py:1165`:
  `flags = [str(nav.get("leverage")) if f == "{leverage}" else f for f in nav["flags"]]`. That is the only reader
  in the cycle scripts. `:1010` lets a `ref` section override it; templates pop `ref`.
- **Executable.** `atx-impl/src/strategy_nav_replay.cpp:3005` (`--aim-leverage` → `target.aim_leverage`) parses it.
  `strategy_target_replay.cpp:121-126` validates it: **aim_leverage must be in [1, 2]**, otherwise the run is
  refused.
- **No code change is needed.** A template sets L with `"change": {"set": {"nav.leverage": "1.1477", "nav.output":
  "<new name>"}}`. `research_spec.py:186-188` applies any dotted key, and `:205-206` requires a new nav.output
  anyway. No executable, cycle script or tool script changes (PM5-21 holds).
  - An alternative is `change.flags.nav {"--aim-leverage": "..."}` (`research_spec.py:126-159`). It replaces the
    `{leverage}` placeholder with a literal. `nav.leverage` is the cleaner key.
- **What changes and what does not.** The template's spec digest and lock change as usual. Nothing in nav_summ,
  backtest_integrity, book_monitor or research_ledger reads L. The report tools read L from the NAV summary
  (`mega_report/pitch.py:461`, `report.py:790`: `scen.construction.v5.aim_leverage`), so a per-cell L renders
  correctly. The `L1.247` in output names is cosmetic.

### Q2. Is planned gross linear in L?

Yes, to about 0.1% over a 25% range of L.
- **Move rule.** The rule is linear in L: aim = L x desired and next = current + theta (aim - current), from a flat
  warm-start book (`strategy_target_replay.cpp:310, 316`).
- **Not affected by L:** neutralising (on the gross-1 desired, rescaled to entry gross, before L), locate-in-aim
  zeroing (sign only), and the locate floor min(current, 0), which scales with the book.
- **Non-linear terms**, all second order:
  - the dust band .1/N_d and the exit snap band, which are fixed in absolute weight (`:286-296`). At higher L, fewer
    names are dusted, so gross is slightly super-linear.
  - the 1% ADV cap per session at execution, which binds more often at higher L;
  - impact-cost and P&L drift of NAV, which act through weights = held / NAV.

Measured:

| test | L ratio | gross ratio | implied exponent (1 + eps) |
|---|---|---|---|
| v6u real NAV, same combined and flags, L 1 vs 1.247 (756 rows): planned gross | 1.24700 | 1.24791 (per row p05 1.2449, p95 1.2513) | 1.0033 |
| same pair, executed gross_leverage | 1.24700 | 1.24790 | 1.0033 |
| replica, R-1 score, L 1 vs 1.247 | 1.24700 | 1.24889 | 1.0068 |
| replica, R-1 score, L 1.1474 vs 1.247 | .92013 | .91966 | 1.0061 |
| replica, B0c score, L 1 vs 1.247 | 1.24700 | 1.24861 | 1.0058 |

In the same v6 pair, what moves non-linearly is not gross:
- executed tau per GMV is L-invariant (ratio 1.003);
- capped fills rise x1.69;
- dust-kept names fall x.81;
- executed/planned gross stays 1.001.

Estimate of L for R-1:
- **One-shot**, planned all-rows: L' = 1.247 x .978605 / 1.063556 = **1.14740**. On executed gross, the gate's own
  statistic: 1.247 x .981964 / 1.067164 = 1.14744.
- **Corrected** with G proportional to L^(1+eps), eps in [.0033, .0068] (the table):
  L' = 1.247 x .920126^(1/(1+eps)) = **1.1477 (range 1.1477-1.1480)**.
- **Error.** Using the one-shot 1.1474 instead would give planned gross about .9783, against the target .9786.
  Either value lands within about .0005 of B0c's gross, which is negligible against the [.90, 1.05] gate.
- **Per year.** The R-1/B0c gross ratio is stable (1.084, 1.094, 1.082, 1.088 in 2020-2023), so a single L'
  also matches by year to within about 1%.

### Q3. Which artifact gives planned gross before the NAV, and can it be re-run at a trial L?

- **The w-pass file has no L in it.**
  - Path: `build-equity/<ic.w_output>-1/train_planned_targets.csv` (here `mega-v8-r1w-train-std-1/...`), column
    `planned_gross` (and `planned_turnover`), one row per decision.
  - Writer: `strategy_ic_runner.cpp:484-491`, from `strategy_ic_composition.cpp:345-376`.
  - Its rule is fixed: cadence 5, fraction .25 (`strategy_ic_composition.hpp:17-18`, no CLI flag), a gross-1
    target, and no L, neutralising, locate, dust, exits or drift (recipe string `strategy_ic_admission.cpp:47`).
  - So it cannot be re-run at a trial L; it is only a proxy for the speed of the composition.
  - For R-1 it gives a ratio of 1.0794 against the NAV's 1.0868, i.e. L' = 1.247 / 1.0794 = 1.1553, which predicts
    R-1 gross of about .989 (+0.7% against B0c).
  - It also misses every NAV-side construction change: R-4 hold band, R-5 ADV cap, R-6 spo and R-8 leave it
    unchanged. It can calibrate composition cells only, and only approximately.
- **The `nav` phase is the only phase that reads L.** A NAV-only re-run at a trial L works through the existing
  cycle:
  - a template that sets only `nav.leverage` and `nav.output` resumes fields, u, fit, card and w as done;
  - `run --stop-after nav` then runs one bounded NAV. R-1's NAV took 42.3 s, peak 586 MiB, including the
    capacity-curve pass (cap 180 s / 1,536 MiB).
  - The bounded runner captures the child's stdout (`research_cycle.py:1620-1621, 1678`), so no statistic reaches
    the console.
  - **Caution:** the NAV still writes returns to disk. `summary.json`, the daily CSVs, and `<nav>-run/stdout.log`,
    which prints "net Sharpe" per book (`strategy_nav_replay.cpp:2660`), must not be opened. Read mechanics only,
    as for R-1.
- **For R-1 no calibration run is needed.** Its L 1.247 mechanics already exist, and gross is linear (Q2). One NAV at
  L' = 1.1477 is the cell itself, with expected planned all-rows gross .9786 +/- .0005 (B0c: .9786).
- **For a future cell** the blind procedure would be two NAV runs, the first read for mechanics only. Whether such a
  calibration NAV counts as a trial is a PM ruling.
- A price-free exact replica (my scratch `target_persistence.py` / `l_linearity.py`) or the exe's `target replay`
  verb would avoid the first NAV. But the first is not a registered tool (making it one is a tool change barred by
  PM5-21), and the second is a bare executable on data, which the CELLS brief forbids.

### Q4. What else L feeds, and what a per-cell L would make non-comparable

- **Trade cost: convex, the only material non-invariance of net Sharpe.**
  - Cost per traded dollar rises with trade size (v6: 13.55 bps at L 1.247 vs 12.82 at L 1, elasticity about .25;
    `docs/plans/2026-09-28-mega-alpha-v6-handoff-5.md:51`).
  - The ADV cap binds more at higher L (capped fills x1.69 for L x1.247).
  - So "Sharpe is invariant to L" (plan :963) holds for gross, not exactly for net. A child at a lower L saves
    about 2% of cost per dollar at 8% lower L.
  - Under gross matching, child and parent hold the same gross dollars. That makes the paired net dSR and the costs
    more comparable than at fixed L, where R-1 would be judged at +8.7% gross.
- **Financing and borrow:** linear in position dollars (spreads and tier fees). Cash earns 0% with no short rebate
  (`strategy_nav_replay.hpp:24`). Borrow tiers and locate do not read L. No distortion beyond scale.
- **Capacity curve (E-29; the R-3 2x and R-5 4x criteria):** trade size = m x L x NAV / N, so "net Sharpe at m x NAV"
  compares books of different gross dollars when L differs. Gross matching aligns them; fixed L does not.
- **PM5-11 criterion (R-1, R-10, R-11): it scales as 1/L.**
  - `tau_gmv_mean` is already per unit GMV (`strategy_nav_replay.cpp:1175-1176`: traded / pretrade gross dollars;
    `nav_summ.py:271`). Dividing it again by `mean_gross_leverage_all_rows` makes the statistic scale as 1/L
    (v6 pair: tau_gmv x1.003 when L x1.247).
  - So R-1's current pass (.0230 vs .0347) is partly the 8.7% extra gross.
  - At matched gross, the estimate is .0246 / .979, about .025 vs .0347: still a pass.
- **Mechanics limits:**
  - The thresholds ([.90, 1.05], |net| <= .02, tau .20 / .30) are constants; none reads L.
  - Net leverage scales with L.
  - The NAV's turnover limits are per GMV, so L-invariant.
- **Warm start:** the A-3 refusal (`strategy_nav_replay.cpp:1103`) fires only on zero gross at score_begin.
  Unaffected; gross at score_begin scales with L.
- **Range:** the exe refuses L outside [1, 2] (`strategy_target_replay.cpp:121-123`). A cell slow enough that gross
  matching would need L < 1 (tracking ratio above about .98 of the aim) cannot be formed without an exe change.
  R-1 needs 1.148.
- **Later cells read the inherited L:**
  - R-6 spo-v3: w_aim = L x desired, gamma calibrated on L x desired, default gross budget = L, sanity bound
    1.5 x L (`strategy_spo.cpp:419-426, 1275-1277, 1495-1506`; template: bound 2L).
  - R-5 ADV cap: Q ADV / (L NAV) (`strategy_target_replay.cpp:424`).
  - R-8: L_t clipped to [.8 L, 1.25 L], with L = the parent's --aim-leverage.
  - All of these inherit `nav.leverage` through the template chain, so they follow a per-cell L consistently. But
    their registered constants were declared at L 1.247; a ruling should say they read the parent's L.
- **Unchanged:** ledgered cells and the W0-4 re-runs stay at 1.247. The DSR cross-trial variance uses Sharpe, which
  is L-invariant to first order.
