# Library v7 pre-registration draft (P3): wave 1 and wave 2 candidates

Root appends this text to `v7-prereg.md` after its own review. Declared 2026-09-28, before any IC, TRAIN or return read
of any v7 candidate. Read-only research; nothing was built or run.

**Hygiene.** No validation, VAL or 2023+ statistic was read, and no v7 candidate has a return statistic to read. The
mandated `v6-literature.md` quotes TRAIN HAC t of some v5 members (opbe, roa, low-risk, SI); they were seen there and
used for nothing. The fields-v7 manifest was read for names and coverage only; the ALPHA_PANEL docs for schemas,
clocks and gaps only.

## 0. Conventions (binding for every candidate)

- **Spelling and sign.** `R` stands for `((close / delay(close, 1)) - 1)`, written out in full in every string. The
  sign is embedded (higher = long; `prior_sign` +1); the raw direction and its paper go to the recipe, as in v6.1.
- **Forms and windows.** Slow additions: v6 fixed form `R(decay_linear(x, 21))`, R = `rank` or
  `group_rank(., grp_ff12)` per rule R1. Aggregates (`ins_*`) and event flags (`ea_*`): `R(x)`, like `sv_flow`. One
  variant per hypothesis; windows are canonical or house only (21, 63k, 231 + delay 21, 252); no window/sign search.
- **New-op spellings** follow `task-W2-brief.md`: `ts_topk_mean(x, w, k)`, `ts_beta_on(y, x, w)`,
  `ts_mean_mp(x, w, m)`, and `ts_std_mp(x, w, m)` (the brief names only `ts_mean_mp`; `ts_std_mp` is the analogous
  name). If W2 lands different spellings or argument order, root rewrites the strings mechanically before the freeze.
  The semantics stated here are the registration, so a rewrite is not a variant; if W2's semantics differ, the
  candidate is withdrawn.
- **Static budget** (v6 generator rules): at most 314 prior bars, 7 estimated peak slots and 5 extra fields, except
  `q5_eg` at 6 (ruling §3.5a). A candidate the static checker refuses is **withdrawn** (NEEDS_NEW_FIELD), never
  re-specified. A withdrawal before any read lowers the trial count.
- **Rank key within a wave** = evidence (A 1, B+ .8, B .7, B- .55, C+ .4; v6-literature scale) x turnover (very low
  1.0, low .9, moderate .6, high .3) x (1 - max expected abs(rho)). [est] = my estimate (about +/-50%); "t n/v" = not
  transcribed here, root copies it from the paper before the freeze. Tiers use the generator's TIER_RANK set (A..C+).

## 1. Wave 1: fields-v7 plus W2 ops (5 candidates, 5 admission trials)

| rank | id | theme | tier | turnover [est tau/day] | max expected abs(rho) (with) | score |
|---|---|---|---|---|---|---|
| 1 | qmj_safety | low_risk | B | low (.02-.03) | .35-.45 (bac) | .38 |
| 2 | nincr | earnings_momentum | C+ | very low (.01-.02) | .30-.40 (droe, sue) | .26 |
| 3 | q5_eg | investment_issuance | B | low (~.02) | .60-.70 (cbop) | .22 |
| 4 | smax5 | low_risk | B+ | moderate (.06-.09) | .55-.65 (smax) | .19 |
| 5 | res_mom_ind | price_momentum | B | low (~.03) | .65-.80 (res_mom_12_1, within_ind_mom) | .16 |

**W1-1 `qmj_safety`: QMJ safety, three components.** Cross-section; raw direction +1 (safe = long); tier B; 1 trial.
```
(((rank(decay_linear((-1 * ts_beta_on(((close / delay(close, 1)) - 1), mkt_ret, 252)), 21)) + rank(decay_linear(((-1 * (debt / at)) + (0 * log(at))), 21))) + rank(decay_linear((-1 * stddev(((ni_q / be_lag1q) + (0 * log(be_lag1q))), 252)), 21))) / 3)
```
- **Definition.** Equal-weight mean of the ranks of -beta (BAB), -leverage (LEV) and -ROE volatility (EVOL): the AFP
  safety z-score in rank form, in the `value_composite` idiom. 5 extra fields (mkt_ret, debt, at, ni_q, be_lag1q);
  about 273 bars.
- **Deviations** (in the recipe): beta is a 252-session daily OLS slope (not FP's 5-year 3-day correlation x 1-year
  vol ratio); LEV = debt / at (AFP add minority interest and preferred); EVOL = std of the as-of quarterly ROE over 252
  sessions (4-5 quarters under the bound); IVOL omitted (low_ivol removed in v6; price-risk-v1 projects out vol63);
  O-score and Z-score omitted (no current assets or liabilities, working capital or retained earnings in fields-v7).
  Caveat: price-risk-v1 (beta252) largely projects out the beta leg, as it did low_beta; LEV and EVOL carry the rest.
- **Prior.** Asness-Frazzini-Pedersen (2019 RAS; 2013 WP Table VI): US safety factor 1956-2012 earns 0.23%/mo excess
  (t 2.06) and 0.57%/mo 4-factor alpha (t 7.97); QMJ 4-factor alpha 0.66%/mo (t 10.20). Expected abs(rho) [est]: bac
  .35-.45; smax .2-.3; cbop and gpa .15-.25; others < .2.

**W1-2 `nincr`: consecutive year-on-year quarterly earnings increases, capped at 5.** Cross-section; raw +1; tier C+;
1 trial.
```
rank(decay_linear((max(sign((ni_q - ni_q_lag4)), 0) * (1 + (delay(max(sign((ni_q - ni_q_lag4)), 0), 63) * (1 + (delay(max(sign((ni_q - ni_q_lag4)), 0), 126) * (1 + (delay(max(sign((ni_q - ni_q_lag4)), 0), 189) * (1 + delay(max(sign((ni_q - ni_q_lag4)), 0), 252))))))))), 21))
```
- **Definition.** I_k = 1 if the as-of quarterly net income 63k sessions ago beat its year-ago quarter;
  nincr = I0 (1 + I1 (1 + I2 (1 + I3 (1 + I4)))), in 0-5. 2 extra fields; about 273 bars.
- **Deviations.** Capped at 5 quarters: Barth-Elliott-Finn allow 8, and a delay of 441 exceeds the 314 bound. Net
  income replaces EPS (Chen-Zimmermann use IBQ). The 63-session delays approximate fiscal quarters; filing-lag jitter
  can repeat a quarter.
- **Why not W2's `ts_count_increases`.** On an as-of forward-filled series it counts session-to-session increases,
  which are 0 between filings. nincr counts positive year-on-year changes, not increases of a level.
- **Prior.** Barth-Elliott-Finn (1999 JAR): increase streaks earn a valuation premium. Green-Hand-Zhang (2017 RFS),
  non-microcaps 1980-2014: one of two characteristics still independent after 2003, post-2003 t ≈ 1.8 (secondary
  report; verify).
- **Counter-evidence and rho.** Han-He-Rapach-Zhou (2018 WP): univariate value-weighted slope insignificant, hence
  tier C+. Expected abs(rho) [est]: droe .3-.4; sue .3-.4; chtax .2-.3; ear about .1.

**W1-3 `q5_eg`: Hou-Mo-Xue-Zhang expected investment growth, published slopes, no fitting.** Within FF12 (R1); raw +1;
tier B; 1 trial.
```
group_rank(decay_linear(((((-0.031 * log((me_company / at))) + (0.53 * (cfo_ttm / at))) + (0.802 * ((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))), 21), grp_ff12)
```
- **Definition.** E[d1 I/A] = -0.031 log(q) + 0.530 Cop + 0.802 dRoe: time-series average Fama-MacBeth slopes, tau = 1,
  from HMXZ NBER w24709 Table 1 Panel D, 1963-2016 (t -5.86, 12.82, 7.75). Root confirms them against RoF 2021
  Table 1; if they differ, the RoF values are used (not a variant). Nothing is estimated on TRAIN. 6 extra fields
  (me_company, at, cfo_ttm, ni_q, be_lag1q, grp_ff12); about 273 bars.
- **Deviations:**
  - q = ME / AT: HMXZ add DLTT + DLC; debt is dropped for the field budget. log(q) has the smallest slope and HMXZ call
    q "negligible on its own".
  - Cop = CFO / AT: HMXZ use the income-statement Ball et al. form; fields-v7 has no COGS, SG&A or working-capital
    items.
  - dRoe = as-of ROE minus its value 252 sessions ago (HMXZ: minus the four-quarter-lagged ROE).
  - No per-predictor 1-99% winsorization: the DSL `winsorize` is a member-masked cross-sectional op and would bring
    back the 21-session blackout inside the decay; the final FF12 rank bounds each name's influence.
  - Financials kept (HMXZ drop them), ranked within FF12 Money. A negative BE a year ago is not guarded.
- **Prior.** HMXZ (2021 RoF): Eg factor 0.84%/mo, t 10.27, 1967-2018 (w24709: 0.82%, t 9.81, 1967-2016); the
  high-minus-low d1 decile earns 1.06%/mo (t 6.25). Cop dominates; factor correlations are .7 with the Cop factor and
  .44 with the dRoe factor.
- **Expected abs(rho)** [est]: cbop .6-.7 (Cop term); droe .45-.55; cfp .35-.45; asset_growth .2-.3.

**W1-4 `smax5`: scaled MAX with MAX5, as in the paper.** Cross-section; raw -1; tier B+; 1 trial. New op:
ts_topk_mean.
```
rank(decay_linear((-1 * ((ts_topk_mean(((close / delay(close, 1)) - 1), 21, 5) / stddev(((close / delay(close, 1)) - 1), 21)) + (0 * log(stddev(((close / delay(close, 1)) - 1), 21))))), 21))
```
- **Definition.** Mean of the 5 largest daily returns over 21 sessions / 21-session volatility, the AFGP definition
  (MAX5 over the last month / volatility over the last month). 0 extra fields; about 43 bars.
- **Relation to `smax`.** `smax` (MAX1 / 252-session vol) stays byte-identical. The v6 review marked MAX5 missing
  (NEEDS_NEW_FIELD). With a 21-session denominator, smax5 is a shape (lottery) measure, not a volatility level.
- **Prior.** Asness-Frazzini-Gormsen-Pedersen (2020 JFE): SMAX returns positive and robust to BAB (US; t n/v).
  Bali-Cakici-Whitelaw (2011 JFE): high-minus-low MAX deciles differ by more than 1%/mo, 1962-2005.
- **Expected abs(rho)** [est]: smax .55-.65; ind_adj_rev_5 .15-.25; bac .15-.25; iv_rv_spread .1-.2.

**W1-5 `res_mom_ind`: residual momentum vs FF49 industry, standardized.** Cross-section; raw +1; tier B; 1 trial.
New ops: min-periods variants.
```
rank(decay_linear(delay((ts_mean_mp((((close / delay(close, 1)) - 1) - group_mean(((close / delay(close, 1)) - 1), grp_ff49)), 231, 116) / ts_std_mp((((close / delay(close, 1)) - 1) - group_mean(((close / delay(close, 1)) - 1), grp_ff49)), 231, 116)), 21), 21))
```
- **Definition.** Mean / std of the FF49 industry-excess daily return over the 12-1 window: the residual of a
  one-factor industry model with loading 1. 1 extra field (grp_ff49); about 273 bars.
- **Why min-periods.** The industry mean is a member-masked cross-sectional op; full-window ts ops would blank the
  231-session window after any membership gap, which was the v6 blocker. m = 116 = ceil(w/2), the house convention
  from `sv_ratio126` (63 of 126).
- **Deviation.** BHM use FF3 residuals with 36-month betas, beyond the bound. W2's `ts_resid_on` fits an in-window
  intercept, which absorbs the formation drift, so it cannot express BHM within 314 bars.
- **Prior.** Blitz-Huij-Martens (2011 JEF), 1930-2009: about 2x the risk-adjusted profit of total-return momentum
  (t n/v); Blitz-Hanauer-Vidojevic (2020) confirm it for idiosyncratic momentum.
- **Expected abs(rho)** [est]: res_mom_12_1 .65-.8; within_ind_mom .65-.8; mom_12_1 .5-.6; high_52w about .4.

## 2. Wave 2: W5a/W5b fields (5 candidates, 5 trials; 1 withdrawn and 2 conditioning inputs at 0 trials)

**New theme `ownership_flow`:** "Informed-owner flows from SEC ownership filings: opportunistic insider net buying
(Form 4) and 13F manager conviction; informed buying predicts higher returns."
- **Justification.** A new source (Form 4, 13F) and a new mechanism (informed ownership), with expected abs(rho) < .25
  to every existing theme. v6-literature §4: independent themes, not more members, are what raise the EW book SR.
- **Cost.** ew-theme-v1 moves from 9 to 10 themes (1/9 to 1/10 each) when a member is admitted, and the fitter's
  `V4_THEMES` needs one entry (§3.5c).
- **Other placements.** FTD goes to `short_interest` (same shorting-constraint / overpricing economics). The
  earnings-date members go to `earnings_momentum`, the earnings-event home of `ear`.

| rank | id | theme | tier | turnover [est] | max expected abs(rho) (with) | score |
|---|---|---|---|---|---|---|
| 1 | ins_opp | ownership_flow (new) | B- | low (.01-.03) | .10-.20 (mom_12_1, negative) | .42 |
| 2 | inst_best_ideas | ownership_flow (new) | C+ | very low (.01-.02) | .20-.30 (mom_12_1, liquidity) | .30 |
| 3 | eap_8k | earnings_momentum | B | high (.08-.15) | about .1 (ear, seasonality) | .19 |
| 4 | ftd_fail | short_interest | C+ | moderate (.04-.08) | .25-.40 (si_ratio, dtc) | .16 |
| 5 | ea_overdue | earnings_momentum | C+ | high (event, sparse) | about .1 (sue, droe) | .11 |

**W2-1 `ins_opp`: opportunistic insider net buying (Cohen-Malloy-Pomorski).** Raw +1; tier B-; 1 trial.
```
rank((ins_opportunistic_net / shares_out))
```
- **Definition.** Net open-market purchases minus sales (shares) by insiders W5a classifies as opportunistic (routine =
  traded in the same calendar month in each of the prior 3 years), over the trailing 126 sessions, per share
  outstanding. If W5a already scales by shares_out, the division is dropped (mechanical).
- **Clock and history.** Visible from Form 4 acceptance, not the trade date. Form 4 starts 2015q1, so the
  classification is valid from 2018 and TRAIN is covered. No decay: the 126-session aggregate is its own smoothing.
- **Prior.** CMP (2012 JF), 1986-2007: the opportunistic long-short earns 82 bps/mo value-weighted abnormal return
  (t n/v); routine trades earn about 0. Literature-v7 notes thinner evidence after 2007.
- **Expected abs(rho)** [est]: mom_12_1 -.1 to -.2 (contrarian insiders); value members .05-.15; issuance about .1.

**W2-2 `inst_best_ideas`: 13F aggregate conviction (Cohen-Polk-Silli).** Raw +1; tier C+; 1 trial.
```
rank(decay_linear(inst_best_ideas, 21))
```
- **Definition.** Sum over 13F holders of max(0, manager weight - market weight), forward filled, visible from the
  filing date + 46 h.
- **Deviations.** All 13F filers are pooled, because filer type is unclassified (CPS use active mutual funds). The
  field aggregates overweights instead of taking each manager's single best idea. Passive holders contribute about 0.
- **Prior.** CPS (2010 WP), US active funds 1991-2005: best ideas beat the market by about 1-2.5%/quarter (t n/v);
  Anton-Cohen-Polk (2021 WP) report 2.8-4.5%/yr. Crowding tail risk: Brown-Howard-Lundblad (2022 RFS).
- **Expected abs(rho)** [est]: mom_12_1 .1-.2; si_ratio and dtc about -.1.

**W2-3 `eap_8k`: earnings-announcement premium on the 8-K 2.02 calendar.** Raw +1; tier B; 1 trial.
```
rank(max(sign((21.5 - ea_days_to_expected)), 0))
```
- **Definition.** 1 if the expected next announcement (yoy_364, known in advance) falls within 21 sessions: the
  Frazzini-Lamont one-month window, the only one slow enough to survive theta .05.
- **Disclosed repeat.** This is the v4.2 `eap` hypothesis (vendor earnFlag + 63) on new dates. Its v4.2 result was not
  read; this is a new trial.
- **Prior.** Savor-Wilson (2016 JF): scheduled announcers earn 9.9%/yr abnormal (t n/v); Frazzini-Lamont (2007);
  Barber-De George-Lehavy-Trueman (2013 JFE) find it globally.
- **Warning.** Expected capture is low under aim-partial (mean lag about 19 sessions); the literature calls this a
  "timing tilt". It is kept only because the brief asks for it. **Optional:** root may drop it before the freeze,
  never after a read.

**W2-4 `ftd_fail`: fails-to-deliver intensity, a shorting-constraint proxy.** Raw -1; tier C+; 1 trial.
```
rank((-1 * ftd_shares_ratio21))
```
- **Definition.** Fails summed over 21 sessions / shares_out, visible from the SEC publication date.
- **Sign logic:**
  - (a) Constraint (-). Fails concentrate where borrowing is scarce or costly: market makers fail instead of paying
    specials (Evans-Geczy-Musto-Reed 2009 RFS). Binding short constraints let prices run high and later returns fall
    (Miller 1977). Autore-Boulton-Braga-Alves (2015 FinRev) find threshold-level FTD stocks overvalued, then
    reversing (t n/v).
  - (b) Informed shorting that cannot borrow (-): "Informed short selling, fails-to-deliver, and abnormal returns"
    (JEF 2016).
  - (c) Reg SHO close-out buy-ins push prices up over days (+). At our horizon of weeks to months, (a) and (b)
    dominate, so the prior is negative.
- **Harvestability.** The short leg is mostly borrow fee (Muravyev-Pearson-Pollet 2025). Net value is expected in the
  long leg (not holding high-FTD names); the S2 swap-fin-v1 tiers must price the shorts.
- **Data caveats.** FTD rows carry `vintage_risk` (re-posted without history); the universe excludes ETF and ADR
  operational fails.
- **Expected abs(rho)** [est]: si_ratio .25-.4; dtc .2-.35; sv_flow .15-.25; iv_rv_spread .1-.2.

**W2-5 `ea_overdue`: late relative to the expected date (Johnson-So).** Raw -1; tier C+; 1 trial.
```
rank((-1 * max(sign(((ea_days_since + ea_days_to_expected) - 94.5)), 0)))
```
- **Registered predicate.** Overdue = the current quarter's expected date has passed with no 2.02 visible since.
- **Implementation.** Under the atx-db rule `ea_days_to_expected` = min next_expected_date >= d. When a firm misses its
  date, the next expected date jumps a quarter, and ea_days_since + ea_days_to_expected moves from about 63 to about
  126. The cut, 94.5 = 1.5 quarters, is a fixed midpoint, not tuned.
- **Alignment rule.** If W5a publishes a signed days-to-expected (negative when overdue), root respells the predicate
  as 1{ea_days_to_expected < 0} (mechanical).
- **Prior.** Johnson-So (2018 JFQA): later-than-expected scheduling precedes worse news, which prices reflect only at
  the announcement (t n/v). Bagnoli-Kross-Watts (2002 JAR): about 1 cent/share below consensus per day of delay, and
  lower announcement returns.
- **Data limit.** The 8-K is our only clock. We do not have the advance scheduling notice, so only the overdue state
  is observable in real time.

**Withdrawn before any read (0 trials): `inst_breadth_chg`** (13F breadth change). The published signs conflict:
- Chen-Hong-Stein (2002 JFE) find +: the bottom breadth-change decile underperforms the top by 6.38% over 12 months
  (4.95% risk-adjusted), mutual funds 1979-1998.
- Lehavy-Sloan (2008 RAS), using 13F institution counts (our data type), find **future returns negatively related**
  to changes in recognition.
- Our 13F filers are unclassified, so CHS's mutual-fund-only setting cannot be matched. With no canonical sign,
  R5.5(i) excludes it; it stays a descriptive report-card field.

**Conditioning inputs only (0 trials, no library member):**
- **8-K material-event flag** (`k8_item_material_21`: items 1.01/2.01/2.05/2.06/4.02/5.02): (i) a report-card
  sub-universe split of every member's IC, flag vs no flag; (ii) a monitor field. Any use in signals or construction
  (for example, freezing new entries for 21 sessions after M&A or non-reliance) needs its own pre-registration.
- **Reg SHO threshold days** (`regsho_threshold_days63`): a sparse flag (a rank ties more than 95% of names), so not a
  member. Reserved as a borrow-tier input (special tier for shorts) in a separately registered cost trial.

## 3. Trial plan

**3.1 Counts.** The cross-cell N continues from 32, after C1-C3.

| wave | library id | roster | new | admission | composition | construction | cross-cell N after |
|---|---|---|---|---|---|---|---|
| 1 | fund_industry_ic_v7w1 | 44 = 39 + 5 | 5 | 5 | +1 | +1 | 33 |
| 2 | fund_industry_ic_v7w2 | 49 = 44 + 5 | 5 | 5 | +1 | +1 | 34 |

v7 adds 10 admission trials. Withdrawals before any read lower the counts, and root logs the final counts (R5.1).

**3.2 Admission `v4-prior-v1`, unchanged.**
- **Rules** (the `fit_composition_weights.py` constants): prior orientation s = +1; reject below 250 finite TRAIN
  days; reject tau above .70; veto on HAC t below -2.0 (NW lag 5); redundancy greedy at abs(rho) <= .90, ordered by
  (tier_rank, roster_order) against every admitted member, v6.1's included. New members come last in roster order.
- **v6.1 members** keep their v6.1 status: identical DSL bytes on identical payloads, 39 L1 cache hits. Any miss is an
  identity failure: the run aborts and no trial is charged.
- **R5.5 items** (iii) PnL correlation < .7, (iv) top-1000 sub-universe and (v) margin >= 2x cost are reported
  descriptively by the W3 report card. They are not gates in v7.

**3.3 Composition `ew-theme-v1`, rule unchanged.** 1 / (themes with >= 1 admitted member), split equally within a
theme; no mean or covariance estimation. New members dilute their theme's existing members equally. The D8
decay-and-vol theme weighting stays a separate single trial.

**3.4 Gate per wave: one cell, sign-only.**
- **Cell.** Library x ew-theme-v1 refit on lo1 x the frozen baseline construction, with L fixed at 1.247:
  aim-partial-v5 (theta .05, dust .1, fixed), delta orders, exit .05, locate-in-aim, liquidity cache, price-risk-v1.
- **Accept** when paired S2 net dSR > 0 (Memmel SE and LW CBB reported), R6' mechanics hold, and S2 net >= 1.0. The
  parent is the v6.1 cell `mega-nav-v61u-ew-t.05-d.1-fixed-obdelta-x.05-loc-L1.247` for wave 1; for wave 2, the
  wave-1 cell if accepted, else v6.1.
- **Freeze** still needs cell-count DSR >= .95, with effective-N DSR (ONC) reported beside it.
- **All-or-nothing.** A wave is accepted or rejected whole: no post-read removal of members, and a rejected wave's
  members are not re-proposed in v7.
- **Data scope.** TRAIN 2020-2022 on lo1 only. The linked-operating-v2 role (W5b) is a separate universe question.

**3.5 Rulings root needs before the freeze** (budgets, not trials):
- (a) **q5_eg at 6 extra fields.** One more resident f64 column ≈ 8 B x 1,155 x 5,627 ≈ 52 MB [est], against the
  L1 u-pass peak of 856 MiB under the 1,536 MiB rule. If refused, q5_eg is withdrawn and a producer field `eg_hmxz`
  becomes the wave-3 route.
- (b) **Generator MAX_ROSTER from 48 to 56** for wave 2 (a generator budget; the runner accepts n <= 256).
- (c) **Append `ownership_flow` to the fitter's V4_THEMES.** The v6.1 and v7w1 compositions must stay byte-identical
  with the appended list.

**3.6 SHA pinning.**
- `generate_fund_ic_v7w1.py` re-derives v6.1 through `generate_fund_ic_v61.py` (which pins v6 at `5ee66d13...`) and
  asserts library `db35c2769f6c13a8d9d5b2897a9e6968855bffbe6214ff96a12f6869cb59f9b5` and recipe
  `9bf278a6f3dfa78edd506ab3e499b2e8dd1ce34899752fe81df573e65550f547`. `generate_fund_ic_v7w2.py` pins the v7w1 bytes
  the same way.
- Before a wave's first IC run, root writes into v7-prereg.md the wave library's SHA-256 (exact JSON bytes) and the
  generator's SHA-256 (LF). Every IC, fit and NAV receipt must carry that SHA.
- Any byte change after a read means a new library id and new trials. `--check` verifies the committed bytes.

**3.7 Identity requirements** (before any wave cell runs):
- (i) The 39 v6.1 candidate entries are a byte-identical prefix of the v7w1 candidate region (same encoder; the
  generator asserts it, as v61 did for v6), and v7w1's 44 entries are a prefix of v7w2's. Only the descriptions of
  themes with new members change, plus the fields list for wave 2.
- (ii) fields-v8 = fields-v7 plus the W5a/W5b fields, with the 41 existing payload SHAs unchanged.
- (iii) The wave IC pass reproduces the 39 v6.1 rows of orientations.json and train_daily_ic.csv byte for byte.
- (iv) The v6.1 cell (library v6.1 on fields-v8) reproduces its S2 daily CSV bit for bit.

**3.8 Order.** Wave 1 needs only W2 and runs once W2 is merged and root has aligned the spellings. Wave 2 runs after
W5a and W5b land and fields-v8 is built. Neither wave changes a construction parameter.

## 4. What is not included, and why

- **Post-publication decay.** Predictability falls 58% after publication and 26% out of sample (McLean-Pontiff 2016
  JF); US declines are -62% equal-weighted and -66% value-weighted (Jacobs-Müller 2020 JFE). Anomalies above 50%
  monthly turnover rarely survive costs (Novy-Marx-Velikov 2016). Excluded on these grounds: short-term reversal
  variants (turnover-conditioned, residual), raw IVOL or MAX1 additions, connected-stock reversal, and any EAP variant
  faster than one month.
- **No consensus (D5).** No analyst SUE, revisions, dispersion or consensus surprises.
- **No borrow fee or utilisation (D4 is proxy only).** No fee or CME signals. SI / IO as a utilisation member is
  excluded: expected abs(rho) with si_ratio is at least .85 [est], and v6.1 already holds four SI members; SI / IO is
  kept for the borrow-tier cost input. Modelled net stays optimistic until name-level fees exist (MPP 2025).
- **No GICS or NAICS (U5).** Industry signals stay on FF12 and FF49.
- **FPI 6-K gap.** FPIs announce on 6-K without item codes, so eap_8k and ea_overdue are NaN for FPIs and most ADRs
  (backfill tier about 55-60% covered). They rank domestic filers only, a disclosed coverage bias.
- **Other data gaps.** No options skew, OI or volume (D8), so no skew, delta-IV or call-put members. No VWAP or trade
  count, no index add/drop, and buyback announcements are not parsed.
- **Conflicting or unsignable priors:**
  - `inst_breadth_chg` (§2);
  - institutional demand as a change in IO: Sias-Starks-Titman 2006 (+) vs Edelen-Ince-Kadlec 2016 (-);
  - post-announcement `ea_delay_days`: Johnson-So find it priced at the announcement, so there is no drift prior;
  - `ea_time_of_day`: an interaction with `ear` (DellaVigna-Pollet 2009), not a standalone member.
- **One variant per hypothesis.** `ins_net_buy_ratio` (all insiders; CMP find routine trades are noise);
  `ins_cluster_buy` (a second insider variant); `sv_offexchange_share126` (FINRA files have no per-venue split, and
  there is no signed prior).
- **Redundant.** `bac_vq`, BAC within vol quintiles, is now expressible with `bucket`, but its expected abs(rho) with
  bac is .85-.92 [est], and price-risk-v1 vol63 already conditions on volatility.
- **Beyond the 314-bar bound, or fields not in fields-v7/W5:** BHM FF3 36-month residual momentum, Heston-Sadka
  multi-lag seasonality, 5-year composite issuance, XFIN, intangible value, income-statement CbOP and q5 Cop. The D6
  items (cogs, xsga, rect, invt, ap, drev) now exist in atx-db, so a wave-3 fields lane can deliver these. Also out:
  chempia (no employee counts) and 13F confidential holdings (amendment linkage not in the W5b fields).
- **Never.** IC-, MV- or ML-fitted member or theme weights on 3 years of data (literature-v7 S1, S8).
