# Code review v8: signal-to-book path (task P1b, sprint platform-v8)

- Reviewer: Claude Fable 5.1 (read-only). Date 2026-09-29.
- Tree: `C:/atx` main. Reviewed code = `7fbfc379`; HEAD is `c1439641` (an atx-db commit); `git diff --stat 7fbfc379 HEAD -- atx-impl
  atx-engine scripts atx-core` is empty and those trees are clean.
- Read (code): `atx-impl/src/strategy_{ic_composition,target_replay,price_exposures,nav_replay,nav_v7,spo,cost_v2,risk_model,ic_runner}`,
  `atx-impl/tools/{fit_composition_weights,alpha_report_card}.py`, `atx-impl/strategies/fund_industry_ic_v71{.json,.recipe.json}`,
  `scripts/specs/v71.json`, `atx-engine/tools/{research_fields_holdings,prepare_recent_research}.py` (clocks only).
- Read (state): platform-20260928 `progress.md`, `v7-prereg.md`, `library-v7-draft.md`, `library-v7-wave2-prereg.md`,
  `literature-v7.md` S1-S2, task reports R2, W1, W1b, L4, F3, U2; mega-alpha-20260926 `v6-code-review-{signal,exec}.md`; scorecard v6.
- Read (TRAIN evidence, `C:/atx-wt/pool-2/build-equity`): `mega-weights-v71-ew/{admission,composition_weights}.json`,
  `mega-cards-v71/card-*.json`, `mega-monitor-v71/monitor.json` (keys), the v7.1 cell's `recipe.json` and `summary.json`,
  `mega-nav-v70-lo3-spo-v2-G1.0/v7_extras.json`.
- Hygiene: nothing was edited, built or run. No file dated 2023 or later and no validation / holdout file was opened; no daily
  or events CSV was opened. Ledger and scorecard text was read through a filter that drops lines naming 2023, 2024 or VAL.
  One historical validation figure (fitted v3 weights) sits in the lever table of the mandated `v6-code-review-signal.md`:
  seen, not used, not quoted. I made no measurement: numbers below are copied from existing TRAIN artefacts or are
  arithmetic on them, marked [est] when they are my estimate.

## 0. Headline

- The book is one rank-linear, equal-weight-by-rank portfolio of about 1,850 names. Nothing in the accepted path uses
  volatility, the risk model, liquidity or evidence strength to size a name, a member or a theme. Every lever below is a
  consequence of that.
- Cost drag is .404 SR (1.809 gross to 1.405 net): trading .277 (linear .123, impact .154) and financing .125 (short .077,
  long .047). Financing cannot be cut by construction. Turnover and impact per dollar can.
- The largest single weight (.10) is `iv_rv_spread`, a one-member theme with standalone turnover .36/day. Because theme
  composites are summed without re-standardisation, its effective share is about 14.7%, not 10% [est]. Members with
  standalone turnover >= .24/day hold 22% of the nominal weight.
- Admission measures a one-day, zero-lag, daily-rebalanced factor return. The book holds a theta .05 average (mean lag 19
  sessions plus one fill session). No statistic in the pipeline is measured at the horizon the book trades.
- No shrink by evidence exists. Tier is used only to order the redundancy pass. Six admitted members with a negative TRAIN
  HAC t hold 16.5% of the weight; `ins_opp` (.05) weighs four times `cbop` or `opex_at` (.0125).
- The risk model atx-risk-v1.1 is read by `spo` only. The cheapest honest use is a target-tracking optimiser (implied alpha
  from the aim-partial target), which removes the vol tilt of lesson G2 by construction.
- SR is invariant to L except for impact convexity. Re-deriving L is a return and vol choice, not a Sharpe lever; do not
  spend a trial on it.
- Every accepted v7 step sits inside one SE (paired SE .04 to .12). At that power no single lever below is detectable on
  three years. Bundle structural changes, add a mechanical acceptance criterion, and get more history.
- No new High correctness defect was found. One High exposure remains, already disclosed: terminal returns (section 4, C-1).

## 1. Executive ranking

Effects are my estimates [est] at $1bn S2 unless a source is given. Trials = construction cells added to N (37 today).

| # | id | finding / lever | net SR | capacity | turnover | conf. | effort | trials | files |
|---|---|---|---|---|---|---|---|---|---|
| 1 | S-1 | Theme composites not re-standardised; one-member theme over-weighted; add member cap | 0 to +.05 | + | -5 to -10% | med | S-M | 1 (bundle with S-4) | `strategy_ic_composition.cpp:182-196,235-239`; `fit_composition_weights.py:1399-1406` |
| 2 | S-2 | Fast sleeves at full aim weight; GP aim gain built but never run on a v6+ library | -.05 to +.05 (1x), + at >= 2x | + | -15 to -30% | med | 0 code (aim) / M (rate) | 1 each | `fit_composition_weights.py:1180,1409`; `strategy_target_replay.cpp:226-280` |
| 3 | S-6 | No ADV-relative holding cap in aim-partial-v5 | -.02 to -.08 (1x), +.05 to +.15 (4x) | ++ | 0 | med | S | 1 | `strategy_target_replay.cpp:184-205,360-403`; `strategy_nav_replay.cpp:654-713` |
| 4 | S-8 | Target-tracking optimiser (implied alpha from the aim) | 0 to +.10 | + | -10 to -20% cost/$ | low-med | M | 1 | `strategy_spo.cpp:976-1050,1020,1164`; `strategy_spo.hpp:141` |
| 5 | S-4 | No shrink by evidence; tier unused in weights | 0 to +.05 | 0 | 0 | low-med | S | 0 (inside S-1 cell) | `fit_composition_weights.py:193,1366,1399` |
| 6 | S-5 | Rank-linear sizing, no vol scaling; TC reference is desired/sigma | -.05 to +.10 | + | impact/$ -5 to -10% | low | S | 1 | `strategy_target_replay.cpp:184-205`; `strategy_cost_v2.cpp:303` |
| 7 | S-3 | Admission horizon differs from the traded horizon | 0 (descriptive) | 0 | 0 | high (fact) | S | 0, then 1 | `fit_composition_weights.py:888-889,952-968,1324-1396` |
| 8 | S-10 | No per-size-tier budget; alpha in small names, capacity in large | - at 1x, + at 4x | ++ | 0 | low | M | 1 | `strategy_target_replay.cpp:184-205`; cards `ic_by_size_tercile` |
| 9 | S-7 | Risk model unused by the accepted rule; vol-targeted L, theme risk budgets | +-.05 | 0 | + small | low | S-M | 1 each | `strategy_nav_v7.cpp:312-384`; `strategy_spo.cpp:889` |
| 10 | S-11 | Event sleeves carry news about 28 sessions old | 0 to +.05 | 0 | + small | low-med | S | 1 + 4 admission | library `:542-600`; `prepare_research_fields.py` fund lag |
| 11 | S-12 | Financials ranked inside FF12 Money on EV / asset ratios | 0 to +.03 | 0 | 0 | low | S | 1 + up to 8 admission | library `:302,366`; recipe `within_industry` |
| 12 | S-13 | Power: accepted steps inside one SE; history is the lever | gate, not SR | 0 | 0 | high | data | 1 (owner gate) | `progress.md` v7.0, U-lo3, v7.1 entries |
| 13 | S-9 | Cost components and execution assumption (facts, no lever found) | 0 | 0 | 0 | high | - | 0 | `strategy_nav_replay.cpp:1822-1849`; cell `summary.json` |

## 2. Facts from the code and the TRAIN evidence

### 2.1 Pipeline, in order (answers Q4, Q5, Q6, Q9)

| step | what the code does | evidence |
|---|---|---|
| member signal | DSL value per name; most slow members are `R(decay_linear(x, 21))`, R = `rank` or `group_rank(., grp_ff12)` | library candidates |
| per-member transform | centred tied rank in [-.5, +.5] over members with a finite signal; no z-score, no winsorisation (the rank bounds it) | `strategy_ic_composition.cpp:26-36,182-196` |
| missing value | contributes 0 (neutral); no redistribution under ew-theme-v1 (weights file is schema v1, no theme block) | `:172,191`; `composition_weights.json` schema |
| combination | `blend_i = sum_k w_k s_k r_k,i` with pinned weights and pinned signs (+1) | `:131,191` |
| scale by coverage | a name with 3 of 38 members has a smaller blend and lands near the middle of the final rank. It is not rescaled | `:186-193` |
| desired target | centred tied rank of the blend over all members, demeaned, gross 1: rank-linear weights, max about 2/N | `strategy_target_replay.cpp:184-205` |
| locate-in-aim | special-tier negative aims set to 0 before the projection | `:368-372` |
| neutralisation | OLS residual of the combined target on [1, z beta252, z vol63, z ladv63], unweighted, z clipped at 5, rescaled to entry gross. Applied once to the combined target, not per sleeve. No industry term in the accepted rule | `strategy_price_exposures.hpp:26-30,111-130`; recipe `price_risk` |
| market for beta | equal-weight mean over all panel instruments, members or not (v6 exec F6, unchanged) | `strategy_price_exposures.cpp:119-146` |
| sizing | `next = current + theta (L desired - current)`, theta .05, L 1.247; no vol term, no name cap, no ADV holding cap | `strategy_target_replay.cpp:226-280` |
| dust / exit | gap <= .1/N keeps the weight (194 names per decision); nonmembers decay 5%/day and snap to 0 inside the band | `:232-266`; summary `construction` |
| orders | delta orders in decision-NAV dollars; fill at the close of d+1; 1% ADV cap per session at execution only | `strategy_nav_replay.cpp:654-713,893-932` |
| netting | implicit: one combined signal, one book. Sleeves never trade separately | `strategy_ic_composition.cpp:191` |
| L vs vol | L = 1 / mean gross of the L 1 book (C4). It sets realised gross .965. It has no relation to vol: 4.13% is what falls out | recipe `aim_leverage`; ledger P6-F2 |

Industry ids `price-risk-ind-v1/-v2` exist (`strategy_target_replay.hpp:16-28`) and were rejected on sign in v6 (C5): vol per
unit gross fell 19%, gross SR rose 1%, so the cost drag per unit vol rose (.329 to .413 SR) and net fell .07. This is the
general rule for every vol-reducing lever: net SR = gross SR - (cost per unit gross) / (vol per unit gross). A lever that
lowers vol must raise gross SR by more than the rise in that ratio.

### 2.2 Theme structure and implied member weights (Q2)

The fitter computes `1 / (T x n_theme)` over admitted members (`fit_composition_weights.py:1399-1406`); the C++ default is the
same rule over families (`strategy_ic_composition.cpp:129-136`). Admitted counts are from `admission.json`. The task brief's
counts (value 9, earnings 7, low_risk 5) are roster counts or differ from the file; the file is used here.

| theme | admitted | member weight | composite SD factor [est] | effective share [est] | note |
|---|---|---|---|---|---|
| value | 5 | .0200 | .68 | 10.0% | ep, cfp, fcfp, sp redundant |
| profitability_quality | 8 | .0125 | .60 | 8.8% | |
| investment_issuance | 4 | .0250 | .60 | 8.8% | issuance_vendor redundant |
| earnings_momentum | 5 | .0200 | .57 | 8.4% | nincr redundant |
| price_momentum | 4 | .0250 | .69 | 10.1% | mom_12_1, res_mom_ind redundant |
| low_risk | 3 | .0333 | .63 | 9.2% | qmj_safety vetoed |
| short_interest | 4 | .0250 | .70 | 10.3% | si_change vetoed |
| reversal_seasonality | 2 | .0500 | .70 | 10.3% | both fast |
| options_implied | 1 | .1000 | 1.00 | 14.7% | fast, one vendor field |
| ownership_flow | 2 | .0500 | .65 | 9.5% | both weak on TRAIN |

SD factor = mean Spearman correlation of a member with its own theme composite (cards `correlation.signal.themes`). For an
equal-weight composite C of ranks, SD(C) = SD(rank) x mean corr(member, C), so a theme's dispersion in the blend scales
with this factor. Return-free second moments; Spearman for Pearson is the approximation.

TRAIN sleeve PnL correlations (cards, theme composites) show two blocks: {value, investment} against {momentum, earnings,
profitability, low_risk, options}; value vs momentum about -.5, options vs low_risk +.58, options vs value -.43. Five of ten
themes sit in the second block, so equal theme weight is about 50% one block.

### 2.3 Admission (Q3)

| item | what the fitter does | line |
|---|---|---|
| sleeve book | unsigned centred tied rank over used rows with a finite signal, two-pass projection on the price-risk basis, gross 1 | `:952-965` |
| return | `f_k(d) = sum_i q_i(d) r_i(d+2)`: one session, entry d+1, no lag, rebalanced in full every day | `:888,967` |
| missing forward return | set to 0 (v6 m1, unchanged) | `:889` |
| veto | Newey-West t of the mean of f_k, Bartlett, lag 5, below -2.0 | `:183,1324-1335,1358` |
| other checks | fewer than 250 live days; tau above .70 | `:1352-1355` |
| redundancy | greedy in (tier_rank, roster order); reject when abs rho > .90 with an admitted member over common TRAIN days | `:1366-1385` |
| order dependence | yes. Ties inside a tier go to roster order, and wave members are last, so nincr lost to sue and res_mom_ind to res_mom_12_1 by position | `:1366`; recipe P1 |
| member that subtracts | kept at full weight unless t < -2 | by rule |
| tier in weights | not used. `ew_theme_weights` takes the theme list only | `:1399-1406` |
| sign conflicts | 12 members have a runner rank-IC21 sign that differs from the prior (or is 0): ep, ebit_ev, rd_me, gpa, opbe, fscore, noa, bac, si_change, iv_rv_spread, opex_at, ins_opp | `admission.json` `sign_conflicts` |

Admitted with negative TRAIN HAC t: ins_opp -0.51 (.05), si_ratio -1.13 (.025), ftd_fail -0.75 (.025), ear -0.74 (.02),
high_52w -0.54 (.025), ea_overdue -0.49 (.02). Sum of weights .165.

### 2.4 Horizons (Q1, Q10)

| quantity | horizon | source |
|---|---|---|
| runner IC | cumulative h = 5, 21, 63 from entry d+1 | `strategy_ic_runner.cpp:253,2108` |
| orientation field | `train-rank-ic21`, a runner contract only; signs are pinned by the prior | recipe `orientation` |
| admission | one session, zero lag | 2.3 |
| book | theta .05: weight on the aim of j sessions ago is .05 x .95^j, mean lag 19, plus one fill session; one-way holding about 26 to 29 sessions | recipe; spo-v2 extras `shadow.holding_sessions` 29.2 |
| card decay | lagged one-day rank IC for h = 1..63, already in every card | `alpha_report_card.py` docstring `decay` |

Turnover drivers, standalone tau per day (admission table, neutralised sleeves): ind_adj_rev_5 .586, iv_rv_spread .360,
seasonality_same_month .290, ea_overdue .240, smax5 .096, smax .084, ftd_fail .082; every other admitted member .020 to .051.
Weighted by member weight, the four fastest members are 22% of the weight and about 75% of the blend's raw rank turnover [est].

Per-sleeve smoothing controls that exist: the DSL `decay_linear` per member; one book theta; a per-name rate span
(`per-name-v1`, liquidity based). The GP decay weight 1/(1 + 19 phi_k) was built as `ew-theme-aim-v1` (`aim_gain`, `:1180`)
and tested on library v5 only, where every member carried the 21-session decay. It has not been run since the fast members
lost their decay in v6. `ew-theme-v6` (fast members at 1/3 weight, bundled with two other changes) cut turnover per unit
gross by about 30% and was rejected on sign with SE .297.

### 2.5 Costs and execution (Q9)

v7.1 cell, S2, TRAIN, from `summary.json`:

| component | dollars (3 y, $1bn) | per year | SR units (vol 4.13%) |
|---|---|---|---|
| linear (5 bps half spread + 1 bps commission) | 16.35M | .51% | .123 |
| impact (.6 sigma sqrt(q / ADV)) | 20.42M | .64% | .154 |
| short financing (20 bps + tier fee 30 / 100 / 500) | 10.30M | .32% | .077 |
| long financing (40 bps) | 6.34M | .20% | .047 |
| total | 53.4M | 1.66% | .402 |

- Per traded dollar: 6.0 bps linear + 7.5 bps impact = 13.5 bps. Impact is 55% of trading cost.
- Short dollars by tier: GC 80.2%, warm 19.2%, special 0.6%. Warm shorts pay 34% of the short financing.
- Execution: decide after the close of d, fill at the close of d+1, first return row d+2. No same-day close is used for both
  signal and fill. There is no open or VWAP field (atx-db: not available), so a faster fill cannot be modelled today.
- Participation cap 1% ADV per session: 6,198 capped fills of 1,252,604 (.49%); unfilled $184M of about $27bn traded.
- About 1,660 fills per day at a mean of about $23k. No minimum ticket or per-order cost is modelled (v6 exec F10).

### 2.6 Alpha definitions by theme (Q1)

| theme | near-duplicates (factor abs rho) | coverage / blackout | deviation that likely costs IC |
|---|---|---|---|
| value | value_composite holds bm, ep, cfp and bm is also a member: bm counts 1.33 times. composite-bm .88, composite-ebit_ev .89 | ebit_ev .55, rd_me .36: unscored by the runner | EV ratios for financials inside FF12 Money (S-12); no winsorisation needed (ranks) |
| profitability_quality | roe_q-opbe .81, roa-roe_q .77, cbop-roa .71 | gpa .60, opbe .71, fscore .53, opex_at .74: unscored | gpa, cbop, accruals, opex_at for banks and insurers; opex proxy includes D&A |
| investment_issuance | issuance_xbrl-net_payout .90 across themes: issuance is held twice | q5_eg .83 | q5_eg dRoe uses `delay(., 252)` not the lag-4 fields that droe uses; q without debt |
| earnings_momentum | sue-droe .88, sue-chtax .83: three members, about one signal | ear 1.00 by backfill 126 | `decay_linear 21` on a step function plus fund lag 1: news about 28 sessions old in the book (S-11); ear abnormal return vs EW market |
| price_momentum | res_mom-within_ind .84, res_mom-ind_mom .81 | high_52w needs 252 full sessions (v6 m2) | acceptable |
| low_risk | smax-iv_rv_spread .67, smax5-high_52w .69 | fine | beta and vol legs are projected out by design; what is left is correlation and lottery shape |
| short_interest | si_ratio-dtc .79, ftd_fail-si_ratio .75 | fine | shares_out (90-day lagged vendor count) stands in for float |
| reversal_seasonality | none | fine | rolling 21-session window 231 to 252 sessions back, not the calendar month: a rank that changes every day (tau .29), then blurred over about 20 sessions by theta |
| options_implied | none (one member) | ts_backfill 5 | ATM IV minus RV21, not the call-put spread; vendor clean IV; raw rank IC at 21 and 63 sessions is negative on TRAIN while the neutralised one-day t is +1.04 |
| ownership_flow | ins_opp-bm .73 (PnL) | fine | ins_opp: 66.6% zeros, 30.6% sellers, 2.8% buyers, so it is a short-the-sellers signal; the paper's alpha is in purchases |

Six admitted members (gpa, opbe, ebit_ev, rd_me, fscore, opex_at; weight .09) have no runner IC, no decay curve and no
size split, because the runner keeps a date only when paired names are at least 80% of eligible names
(`alpha_report_card.py:85,550`). They trade with pinned sign +1.

### 2.7 Universe (Q11)

- Accepted v7.1 cell runs on role lo1. Role lo3 was accepted on library v7.0. V7-F (v7.1 on lo3) is pre-registered and not run.
- 820,393 unlinked member cells remain on the top-3000 base (U2: ETF / fund 41%, backfill-only link 29%, no CIK 18%, ADR 9%).
  This is a data ask to atx-db, not a code lever.
- The code has no per-tier weighting: one cross-sectional rank for every member (`strategy_ic_composition.cpp:182-196`,
  `strategy_target_replay.cpp:184-205`). The DSL has `bucket` and `group_rank`, so a tier rank is expressible per member
  but not at the combination or the target.
- Cards (rank IC at h 21, small / mid / large tercile of me_company): value_composite .074 / .029 / .008; issuance_xbrl
  .080 / .030 / .025; high_52w .059 / .008 / .001; roe_q .051 / .012 / .016; cbop .051 / .018 / .018. Alpha is in the small
  tercile. Net SR falls 1.448 / 1.405 / 1.357 / 1.236 / 1.082 at .5 / 1 / 2 / 4 / 8 x $1bn.

## 3. Findings in detail

### S-1 Theme composites are summed without re-standardisation
- Evidence: `blend += sign x weight x rank` per member (`strategy_ic_composition.cpp:191`). A theme's composite is the mean of
  n imperfectly correlated ranks, so its dispersion shrinks with n and with low within-theme correlation. Table 2.2:
  options_implied 1.00 against .57 to .70 elsewhere.
- Consequence: the one-member theme gets about 1.5 times the effective weight of the average theme and 1.76 times that of
  earnings_momentum. It is also the second fastest member.
- Change: composition `ew-theme-std-v1`. Per date and theme, take the present-weighted mean of the theme's signed ranks
  (the existing themed planes, `:174-179,235-239`), re-rank it across members with at least one present member, then add
  `W_theme x rank`. The weights file uses schema v2 with a `theme_standardise` key. Add a member cap: no member above
  `1 / (2T)`; the excess goes pro rata to the other themes.
- Note: the themed path also redistributes inside a theme. That is the ew-theme-v6 mechanism. Declare it on or off; I
  propose off (missing stays neutral), so the cell changes one thing.
- Pre-registration: declare rule text, cap, parent cell, everything held fixed. Identity: with the re-rank and the cap off
  the blend is byte-identical to ew-theme-v1. Acceptance: paired S2 net dSR > 0 AND mechanics AND planned turnover per
  unit gross not higher. Cost: 1 composition + 1 construction cell.

### S-2 Fast sleeves take full weight in the aim
- Evidence: 2.4. ind_adj_rev_5 rank IC .0104 at h 5 and .0015 at h 21 (card); the book reaches a new aim with a mean lag of
  19 sessions plus one fill session. Realised gross is .774 of the aim's gross (.965 / 1.247): the aim moves faster than
  the book can follow.
- Option A, no code: `--composition ew-theme-aim-v1` on library v7.1 (`fit_composition_weights.py:1180-1186,1409-1418`). The
  gain is measured from rank autocorrelation only. The v6 m7 objection (gain measured on an already smoothed signal) no
  longer applies to the four fast members.
- Option B, fast alpha as timing: keep the fast members out of the aim and use them to set the per-name rate. The seam
  exists: `update_weights(..., per_name_rate)` (`strategy_target_replay.cpp:295-308`), rates in [0, 1] per name. Rule:
  `theta_i = theta x clip(1 + kappa x s_i x sign(gap_i), .5, 1.5)`, s = the fast composite's centred rank times 2. A trade
  the fast signal agrees with goes faster; one it disagrees with waits. Needs a second saved blend (fast composite) as an
  input to the NAV verb. About 80 LOC plus tests. Note `v7::plan` refuses rates together with aim-partial-v6 or spo
  (`strategy_nav_v7.cpp:319-322`); this rule is aim-partial-v5 with rates, which is allowed.
- Pre-registration: A: theta .05 fixed, gains clipped [.05, 1] as coded; acceptance as S-1 plus net SR at 2x NAV not lower.
  B: kappa .5 declared, no grid. One cell each. Run A first; run B only if A is rejected.

### S-3 Admission is not measured at the traded horizon
- Evidence: 2.3, 2.4. For a slow member the one-day factor return is a fair proxy. For a fast member it overstates what a
  theta .05 book can hold. The veto can therefore pass a member whose lagged contribution is zero or negative.
- Change, step 1 (no trial): add `ic_theta = sum_h theta (1 - theta)^(h-1) m(h)` to each card from the decay curve it already
  holds, and `f_theta` (the factor return of the theta-averaged sleeve book) to the admission table as a report-only column.
- Change, step 2 (one trial, later): screen `v4-prior-v3` = v4-prior-v1 with the veto on `f_theta`. Declared before step 1 is
  read, or not at all: once the column has been seen, the choice of rule is no longer prior to the data.
- Cost: step 1 zero. Step 2: all 48 candidates are re-screened (disclose 48 admission tests), 1 composition, 1 cell.

### S-4 No shrink by evidence
- Evidence: 2.3. `TIER_GRADES` orders the greedy pass only (`:193,1366`).
- Change: inside the S-1 composition, within-theme weights proportional to the tier score already declared in
  `library-v7-draft.md` section 0 (A 1, B+ .8, B .7, B- .55, C+ .4; A- is not on that scale and must be declared, for
  example .9). Literature grades, not TRAIN statistics.
- Not proposed: weights from TRAIN t or IC. 30 of 38 members have abs t below 1.5 and the fitted-weight precedent is negative.
- Cost: none beyond the S-1 cell if bundled. Bundling loses attribution; at SE .1 to .3 attribution is not available anyway.

### S-5 Sizing ignores volatility, and the TC metric assumes it does not
- Evidence: weights are rank-linear (`strategy_target_replay.cpp:184-205`). The transfer coefficient is
  `corr(desired / sigma, w)` (`strategy_cost_v2.cpp:303`, called at `strategy_nav_v7.cpp:380`). The reported .70 to .76 is
  measured against a reference the construction does not aim at.
- Under Grinold-Kahn (constant IC in standardised returns) the target is z / sigma. The current target z gives a name's
  risk in proportion to sigma. Lesson G2 was the opposite error inside a binding gross cap (alpha times sigma).
- Change: flag `--size inv-vol-v1 --size-power .5`: `desired_i x (sigma_med / sigma_i)^p` before the projection, sigma = the
  decision-window SD clipped to its [p5, p95], then the projection and gross 1. About 25 LOC in `form_desired`.
- Expected: vol per unit gross down 10 to 15%, impact per dollar down (impact is proportional to sigma), TC up by
  definition. Cost drag per unit vol rises (2.1 rule), so net SR can fall. Low prior. One cell; p is declared, not searched.

### S-6 No holding cap relative to ADV
- Evidence: aim-partial-v5 has no cap; spo has `--adv-cap-q` (`strategy_spo.cpp:1134`). Max weight about 2L / N = .135% of NAV,
  $1.35M at $1bn. For a member at the role's ADV floor ($5M) that is about 27% of ADV and 27 sessions to exit at the
  1% cap [est]. The capacity curve's fall at 4x and 8x is this.
- Change: `--adv-hold-q Q`: after the projection clip `|desired_i| <= Q x ADV_i / (L x NAV)` and redistribute the clipped
  mass pro rata inside the same side; one pass, then report the residual breach. About 40 LOC in `form_desired`.
- Pre-registration: Q .10 declared; primary acceptance on S2 net SR at 4x NAV (capacity curve pass), secondary: net SR at
  1x not lower by more than one paired SE, S3 not lower. One cell.

### S-7 The risk model is not used by the accepted construction
- Evidence: `RiskStore` is opened only for `--rule spo-*` (`strategy_nav_v7.cpp:737-739`); the aim-partial-v5 path goes
  `v7::plan` to `detail::update_weights` (`:328,376,442`).
- Cheapest uses, in order of cost:

| use | change | trial | prior |
|---|---|---|---|
| Ex-ante vol and factor split of the accepted book per decision | `risk --book-weights holdings.csv` exists; add style and industry shares of variance to `bias_summary.json` | 0 | fact finding; tells which of the rows below can matter |
| Vol-targeted L | in `v7::plan` scale a copy of `desired` by `L_t = clip(sigma* / (b x sigma_hat_t), .8 L, 1.25 L)`, sigma_hat from `book_variance` (`strategy_spo.cpp:889`) on the gross-1 current book, b = declared bias 1.15, updated every 21 sessions | 1 | SR +-.05; stabilises vol; R6' gross gate must be restated as an ex-ante vol band |
| Theme risk budgets | fitter rule `rb-theme-v1`: W_theme proportional to 1 / sigma_k of the theme's sleeve series, capped [.5, 2] x EW | 1 | literature R1.2 / D8; second moments only, but in-sample |
| Style neutralisation with the full set | residual of `desired` on risk-model X columns for size, beta, residual vol, liquidity, leverage, industries | 1 | low: price-risk-v1 covers three of them and C5 (industry) lost |
| Specific-risk sizing | S-5 with sigma = sqrt(D) | same cell as S-5 | low |

- Read from the spo-v2 cell (no new read): the aim-partial-v5 shadow book has mean ex-ante vol 3.58% at gross .96; the spo
  book 1.80% at gross .97. The accepted book's risk is therefore mostly factor risk (specific risk of 1,850 near-equal
  names is below 1% [est]). Name-level risk sizing moves little; theme-level and factor-level choices move vol.

### S-8 spo: alpha scaling alternatives and a target-tracking optimiser
- The alpha is one line: `b.alpha[j] = gk_alpha(ic, r.specific[i], z, alpha_h)` (`strategy_spo.cpp:1020`), z = desired over its
  member SD (`:1003`). The CLI can change ic and h only; both rescale every name alike. sigma is hard-wired.

| form | alpha | what the code needs | behaviour under a binding gross cap |
|---|---|---|---|
| current (G2) | IC sigma_i z_i / sqrt(h) | - | tilts to high specific vol, low ADV |
| flat | IC sigma_med z_i / sqrt(h) | one enum, median of `r.specific` | picks the top abs z names; still a lasso on about half the members |
| implied from the aim | gamma x Sigma x w_aim, w_aim = L x desired | the risk product X(F(X'w)) + D w (the solver's gradient; `book_variance` `:889` uses the same contraction) | cost-free optimum is the aim itself, gross L: the cap does not bind, no tilt |
| tracking form | min (gamma/2)(w - w_aim)' Sigma (w - w_aim) + cost(w - w0) / H + financing | same as the row above; it is the same problem | same |

- A target-tracking optimiser needs: (1) `--spo-alpha implied-aim`; (2) w_aim on the optimised names from the shared
  neutralised `desired`; (3) gamma = S_prior / sigma_aim with S_prior declared (for example 1.0) and sigma_aim the aim's
  ex-ante vol at the first decision: no return is fitted; (4) the gross cap set to the sanity bound so it is slack;
  (5) diagnostics: ex-ante tracking error to the aim, share of names at the trade limit; (6) an identity test: zero costs
  and no trade limits return w_aim to 1e-8.
- Why it can pay: the rule then decides which gaps to close first by risk per dollar of cost. aim-partial closes every gap
  at the same 5%. Effort M, about 150 to 250 LOC plus tests.
- Process: ruling spo-b closed the spo line for v7. A v8 cell needs a new pre-registration and counts as spo trial 3.

### S-10 Size tiers
- Change: `--tier-budget large:.4,mid:.35,small:.25` on terciles of decision-window ADV: rank inside each tier, gross per tier
  fixed, then the projection. About 60 LOC in `desired_target` and `form_desired`.
- Acceptance on the capacity curve (4x), as S-6. S-6 and S-10 address the same thing; run S-6 first and S-10 only if S-6
  is rejected.

### S-11 Event sleeves are stale
- Age of earnings news in the book: clock 1 + fill 1 + decay-linear mean lag 6.7 + theta mean lag 19 = about 28 sessions [est].
- Change: library v8 respells sue, droe, chtax, ear as `rank(x)` without the decay (the form the fast members already
  have); fund lag 0 stays a separate data change (v6 m4). 4 admission trials, 1 composition, 1 cell.

### S-12 Financials
- Change: for ebit_ev, gpa, cbop, noa, accruals, opex_at, asset_growth, q5_eg use `grp_ff49` inside FF12 Money (banks,
  insurance, real estate, trading are separate FF49 groups) or NaN for SIC 6000-6999. Up to 8 admission trials plus a cell.
  Bundle with S-11 as one library revision.

### S-13 Power
- v7.0 +.073 (SE .123), U-lo3 +.019 (SE .042), v7.1 +.093 (SE .091). Three sign-only accepts in a row have probability 1/8
  under no effect, before the correlation between cells.
- Proposals: (a) every v8 cell carries a second, mechanical acceptance criterion declared in advance; (b) bundle structural
  changes; (c) one owner-gated read of the frozen v7.1 book on pre-2020 history (atx-db D7: prices from 2012-03,
  fundamentals from 2010). It is out of sample for everything decided on 2020-2022 and is the only lever that moves the
  DSR gate without raising SR. It is a data decision, not part of this review's trial list.

### S-9 No lever found
- S2 stays the primary scenario; the v6 exec review's verdict on its realism stands and is not repeated.
- Warm-tier shorts (19% of short dollars, 34% of short financing): a fee-aware aim would need name-level fees (not
  available). The Reg SHO threshold field is reserved for a separately registered cost trial (library draft section 2).

## 4. Correctness risks (Q12)

| id | rating | risk | evidence | status / action |
|---|---|---|---|---|
| C-1 | High | Terminal returns: a held name that disappears is carried 5 sessions and written off at its last mark with no haircut; delisting returns are off by default | summary `missing.written_off` 300 events, $180.6M gross exposure, PnL 0; `prepare_recent_research.py:36-41,939`; v6 S2 1.182 vs S3 .337 | known and disclosed. Run the accepted book once on a role built with `--delisting-returns` (roles lo2 / lo3 support it). Sign is not known in advance: longs and shorts both delist |
| C-2 | Medium | Admission and card PnL set a missing forward return to 0 | `fit_composition_weights.py:889`; card `book` | v6 m1, unchanged. Fix together with C-1 |
| C-3 | Medium | Beta is measured against the equal-weight mean of all 5,627 panel instruments, including non-members and unlinked lines | `strategy_price_exposures.cpp:119-146` | v6 F6, unchanged. Residual cap-weighted beta is unmeasured; the risk verb can report it (S-7 row 1) |
| C-4 | Medium | `iv_rv_spread` (.10 nominal, 14.7% effective) rests on vendor clean IV whose earnings-calendar vintage is unproven | v6 m5; library `:798` | unchanged; S-1's cap halves the exposure |
| C-5 | Medium | Six admitted members (weight .09) are never scored by the runner and have no card evidence; monitor M2 cannot watch them | cards `runner.horizons.*.valid_dates` 0; `alpha_report_card.py:85,550` | lower the card's coverage rule for a report-only IC, or state them as unmonitored |
| C-6 | Medium | Sequential sign-only acceptance inside one SE | S-13 | process, not code |
| C-7 | Low-Med | The replay starts flat on 2020-01-02 and ramps at theta: gross is about 64% of steady state after 20 sessions and 95% after 60 [est], so January and February 2020 are scored under-invested; L was calibrated on all rows | recipe `theta`; v6 F5 | a warm start on the role's pre-2020 sessions (399 exist) would remove it. It changes the return series: declare it as a protocol cell |
| C-8 | Low | A special-tier name zeroed by locate-in-aim can come back negative from the projection under price-risk-v1 (the hold mask exists only for the industry ids); the plan-level block then floors it | `strategy_target_replay.cpp:379-383`; `strategy_nav_replay.cpp:808-818`; 890 blocked name-decisions | net +.0046, inside the gate; no action |
| C-9 | Low | `ea_overdue` carries a one-session false flag on .67% of cells (8-K acceptance lag) | ruling W2-b | disclosed |

Checked and clean (not repeated from v6): fill at d+1 and first return row d+2; labels `close[d+1+h] / close[d+1]`; liquidity
window `[t-w, t)`; borrow tiers from rows <= d. New in v7 and checked: W5a / W5b fields use `available_at < date(t-1)
22:00 UTC` (`research_fields_holdings.py:23,64`), 13F quarters become visible at filing date + 46 h over filings made by
the deadline (`:83-92`), FTD and short-volume windows end at t-2 (`:117`). These clocks are conservative: they cost IC,
they do not leak.

## 5. Proposed v8 signal / construction trials, in order

At most 8, because each raises N in the deflated Sharpe ratio (37 today). Trial 0 items are descriptive and cost nothing.

| order | trial | change (one thing) | parent | declared before the read | acceptance | N after |
|---|---|---|---|---|---|---|
| 0 | descriptive | card `ic_theta`; admission column `f_theta`; risk-verb variance split of the accepted book; turnover attribution by theme (planned turnover of each theme's own aim) | - | that none of them gates or selects anything | none | 37 |
| 1 | V7-F | library v7.1 on role lo3, fields-v9 rebuilt on lo3 | better of v7.1-lo1 and v7.0-lo3 | already pre-registered | as registered | 38 |
| 2 | composition v8 (S-1 + S-4) | theme re-rank, member cap 1/(2T), tier-scored within-theme weights | trial 1 winner | rule text, cap, tier scores, redistribution off | dSR > 0 AND mechanics AND turnover per unit gross not higher | 39 |
| 3 | GP aim gain (S-2 A) | `ew-theme-aim-v1` gains applied on top of trial 2's weights | trial 2 winner | theta .05, clip [.05, 1], lags as coded | dSR > 0 AND mechanics AND net SR at 2x NAV not lower | 40 |
| 4 | ADV holding cap (S-6) | `--adv-hold-q .10` | trial 3 winner | Q, redistribution rule | net SR at 4x higher AND net SR at 1x not lower by more than one SE AND S3 not lower | 41 |
| 5 | target-tracking optimiser (S-8) | `--rule spo-v3 --spo-alpha implied-aim`, S_prior 1.0, H 20 | trial 4 winner | every flag, risk pin, identity test result | dSR > 0 AND mechanics AND cost per traded dollar not higher | 42 |
| 6 | inverse-vol sizing (S-5) | `--size inv-vol-v1 --size-power .5` | trial 4 winner (aim-partial) | p, sigma definition, clip | dSR > 0 AND mechanics AND impact per dollar not higher | 43 |
| 7 | library v8 (S-11 + S-12) | event sleeves without decay; FF49 inside Money | current book | DSL strings, 12 admission trials | wave accepted whole: dSR > 0 AND mechanics | 44 |
| 8 | vol-targeted L (S-7) | `L_t` from the risk model, bias 1.15, 21-session update | current book | sigma*, bounds, bias, the restated gate | realised vol inside [.8, 1.2] sigma* in each year AND dSR not lower by more than one SE | 45 |

Rules for the list:
- Trials 2 to 4 are ordered so that each later one runs on a lower-turnover, better-balanced aim. If one is rejected the
  next runs on the last accepted cell.
- Trial 3 is dropped, not retried with S-2 B, unless the owner opens a new registration.
- Trials 6 to 8 are optional. If the budget is 5 cells, stop after trial 5.
- A delisting-returns cell (C-1) and a warm-start cell (C-7) are protocol corrections. If run, they replace the baseline
  for every later comparison and must be declared as such before any read; each adds 1 to N.
- Not proposed: re-deriving L; fitted member or theme weights; any per-member drop based on a TRAIN statistic; a finer
  theta or dust grid; industry neutralisation again.

## 6. Not verified

- Every effect size: no run was made. The ranges are priors and arithmetic on published TRAIN aggregates.
- The turnover share of the fast members inside the combined book. The 75% figure is a weighted sum of standalone sleeve
  turnovers, not an attribution; trial 0 provides the attribution.
- The factor versus specific split of the accepted book's variance (inferred from the spo-v2 shadow diagnostics on v7.0-lo3).
- The holding-to-ADV distribution of the accepted book (no holdings were read); the 27% figure is a bound at the ADV floor.
- Vendor vintages (clean IV, earnFlag) and the identity bridge's `available_at` provenance, as in the v6 review.
- S3 for the v7.1 cell (the daily CSV was not opened).
