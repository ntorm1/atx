# WQ-explore: does a "WorldQuant" theme improve the primary book? (2026-10-03)

Exploratory, in-sample, off-ledger. Every number below is TRAIN 2020-01-02 .. 2023-12-29 (role `train-2020-2023-lo3`),
measured on the same data the alphas, the variant choices and the weights were chosen on. The owner waived
significance and trial counting for this pass; nothing here is a registered trial and nothing is adoptable as is.
Tree `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Trial ledger `build-equity/trials.jsonl` unchanged
(133 lines, sha `27e40f9f`). No data dated >= 2024-01-01 was opened.

## 1. Answer

**A WQ theme raises the in-sample net Sharpe a little, and the rise cannot be told apart from zero.**

- Primary book (Y-F0, 12 themes, `theme-erc-v1`, primary cost scenario `modeled-1bn-stale5-v1+swap-fin-v1`): net
  Sharpe **1.849**.
- With a 13th theme of Kakushadze (2016) alphas under the same rule, at matched gross: net Sharpe **1.91 to 1.98**
  across six definitions of the theme, i.e. **dSR +0.06 to +0.13**. Memmel SE 0.13 to 0.23; every bootstrap 95%
  interval spans zero; Ledoit-Wolf p 0.50 to 0.63.
- Net annual return rises 0.15 to 0.27 points (5.65% to 5.8-5.9%); max drawdown is equal or lower (2.58% to
  1.97-2.51%). Daily turnover rises 14% to 44% (0.0281 to 0.032-0.040) and total cost by 0.12 to 0.40 points a year.
- Best-looking option: the slow WQ sleeve at a quarter of its equal-risk share (about 3% of blend weight instead of
  13%). dSR **+0.077**, SE 0.052, bootstrap interval [-0.01, +0.17], p 0.12, turnover +3% (0.0281 to 0.0290). This is
  a hand-mixed signal, not a house composition rule (section 6).
- Picking the ten WQ alphas with the highest TRAIN Sharpe, a deliberately optimistic bound, gives only dSR +0.128
  (SE 0.124, p 0.23).
- Folding the same alphas into `reversal_seasonality` instead of a new theme gives dSR +0.026 (SE 0.26): no gain.

How confident: low. The sign is positive in all ten constructions I ran, but they share the same alphas and the same
four years, so that is one observation, not ten. The earlier registered cell X-7 (12 of these strings in the house
form, on the older X-5 parent) measured dSR -0.161 (SE 0.193) and was not accepted; that and this result are both
within one standard error of zero.

What would have to be true for the gain to be real:

1. The WQ sleeve has return left after the 12 existing sleeves. In-sample its intercept on them is 0.1 to 0.2 bp a
   day with HAC t 0.4 to 1.0 (information ratio 0.2 to 0.55): not established.
2. The declared cost scenario is right for the extra turnover. Costs are declared, not fitted.
3. The 43 alphas that need VWAP behave with real VWAP as they do with the (high + low + close) / 3 stand-in. The 35
   exactly transcribed alphas alone give dSR +0.064 (SE 0.15).

My inference, not a measurement: the 101 alphas are mostly a re-expression of short-horizon reversal, with some low
risk and anti-momentum, which the book already holds; the small gain is diversification inside that exposure. It is
worth one registered cell only in the small-share form, and only if a fixed-share rule is acceptable.

Plots: `docs/plans/2026-10-03-wq-explore-equity-curve.png`, `docs/plans/2026-10-03-wq-explore-sharpe-vs-turnover.png`.

## 2. Inventory: what can be computed without look-ahead

All 101 compile and run in the research IC path (DSL VM `dslvm1`, Release exe) on the role's `close`, `raw_close`,
`volume` plus fields-v15 `open_adj`, `high_adj`, `low_adj`, `me_company`, `grp_ff12`, `grp_ff49`, `grp_sic2` (all
flagged point in time in the fields manifest). No operator is missing.

| class | count | status |
|---|---|---|
| S, scale-free | 22 | exact (lane XWQ's transcription, `platform-v8-20260929/xwq_check.py`) |
| L, level-dependent, rebased by `raw_close / close` | 24 | exact (same source) |
| V, needs VWAP | 43 | NOT exact: no VWAP in house (`docs/plans/2026-10-02-v8y-vwap-data-ask.md`); run here with the proxy (high + low + close) / 3 |
| N, price level nested in a time-series op | 12 | NOT exact: each past cross-section uses that day's own prices, not prices re-adjusted as of the evaluation day |

V: 5, 11, 25, 27, 32, 36, 41, 42, 47, 50, 57-59, 61-67, 69-79, 81, 83, 84, 86, 87, 89, 91, 93, 94, 96-98.
N: 1, 3, 4, 13, 15, 16, 29, 31, 68, 80, 88, 92.

Point-in-time notes:

- **Adjusted price level.** `close` is raw close x the vendor cumulative factor. The factor's anchor is not causal:
  at each line's first role session the ratio close / raw_close has median 0.96 and 99th percentile 4,253; at its
  last, median 1.0. So an adjusted level compared across names carries later splits and dividends. Every level term
  is therefore multiplied by `raw_close / close` at the evaluation row (class L, and my translator for V and N).
  Ratios, correlations and time-series ranks are unaffected.
- **Finding D-01** (dollar volume, ADV and VWAP from adjusted close x raw volume) is fixed in the engine
  (`alpha/datafields.hpp`, rule V2: `raw_close * volume`; VWAP = raw close proxy) and does not touch this path: ADV
  here is `ts_mean(raw_close * volume, d)`.
- **Volume** is raw share volume; a split inside a window mixes share bases (rare; as the house reading R2).
- **Industry** is FF12 / FF49 / SIC2 from the SIC of the latest filing visible one session earlier, not the paper's
  classification. **Cap** is `me_company` (90-day lagged shares).
- Paper-level defects kept as printed: #7, #21 compare dollars with shares and #45 correlates over 2 sessions
  (degenerate); #42, #48, #53, #54 are delay-0 alphas traded one session late here; #60, #100 depend on an
  ambiguous argmax direction.
- Under the VM's NaN rules four strings barely exist: #96 (coverage 0.1%), #97 (4%), #84 house form (31%), #88
  house form (43%); #7 has 202 live days. Excluded by my coverage rule (signal on at least half the used names).

Derived expressions: the registry holds 12 `wq_*` strings from cell X-7 (theme `price_volume`, not in the Y-F0
library); they are the house forms of #2, 6, 14, 30, 35, 43, 44, 55, 85, 95, 99, 101 and are covered by this run.
The v9 mining campaign (`build-equity/mine-v9-c1`, 110 templates, 0 admitted) was seeded from field templates, not
from the 101. The legacy `stage_equity_mine` corpus is outside the research IC path. No Y-F0 member is a 101 variant.

## 3. Standalone quality on TRAIN

Factor as admission defines it: centered tied rank, price-risk-v1 residual, gross 1, f = sum(q x next-session
return), execution delay 1; prior sign +1 as printed, no sign fitting. Computed with the fitter's own code; the same
script reproduces the 67 primary members' admission means and turnovers exactly. 1,004 decisions. Two forms of each
alpha: as printed, and the house form `rank(decay_linear(x, 5 | 21))` (21 when the longest window is 10 or more).

| | as printed | house form |
|---|---|---|
| Sharpe, 10th / median / 90th percentile | -0.21 / 0.42 / 0.95 | -0.19 / 0.47 / 0.87 |
| share with Sharpe > 0 | 81% | 83% |
| HAC t > 2 / < -2 | 1 / 0 | 2 / 1 |
| turnover tau, 10th / median / 90th | 0.35 / 0.69 / 1.29 | 0.13 / 0.22 / 0.59 |
| tau <= 0.70 (house limit) | 52 | 96 |
| rank IC, median at 5 / 21 / 63 sessions | 0.003 / 0.003 / 0.002 | 0.002 / 0.003 / 0.002 |
| pass the house screen (tau, veto, redundancy 0.9) and my coverage rule | 47 | 80 |
| mean Sharpe, exact 46 / not exact 55 | 0.47 / 0.34 | 0.45 / 0.35 |

Rejections, as printed: 49 turnover, 2 redundant, 2 low coverage, 1 insufficient. House form: 11 redundant, 5
turnover, 3 low coverage, 1 veto (#7), 1 insufficient.

Best admissible candidates (gross of costs):

| id | class | tau | Sharpe | HAC t | IC 5 / 21 / 63 | rho with blend | nearest member (rho) |
|---|---|---|---|---|---|---|---|
| wq057h | V | 0.31 | 1.11 | 2.10 | .0073 / .0018 / -.0021 | 0.45 | ind_adj_rev_5 (0.76) |
| wq093h | V | 0.13 | 1.08 | 2.24 | .0005 / .0078 / -.0068 | 0.01 | smax5 (0.67) |
| wq066 | V | 0.55 | 1.04 | 1.96 | .0072 / .0042 / .0052 | 0.31 | ind_adj_rev_5 (0.80) |
| wq048h | S, delay-0 | 0.30 | 1.01 | 1.91 | .0055 / .0056 / .0024 | -0.09 | high_52w (-0.28) |
| wq089 | V | 0.56 | 1.00 | 2.01 | .0042 / .0023 / .0016 | 0.24 | ind_adj_rev_5 (0.42) |
| wq032 | V | 0.30 | 1.00 | 1.92 | .0147 / .0182 / .0281 | 0.17 | ind_adj_rev_5 (0.59) |
| wq056h | S | 0.30 | 0.97 | 2.00 | .0045 / .0132 / .0125 | 0.02 | high_52w (-0.55) |
| wq020h | L | 0.62 | 0.94 | 1.83 | -.0035 / -.0083 / -.0045 | 0.00 | ind_adj_rev_5 (0.57) |

No candidate reaches HAC t 2.3. Full table (202 rows, every column): `build-equity/wq-explore/stats/wq_candidates.csv`.

## 4. Relation to the primary book

Theme sleeves are the house's (`theme-erc-v1`: sum of within-theme share x member factor return).

| correlation of a WQ factor return with | as printed, median (10th, 90th) | house form, median (10th, 90th) |
|---|---|---|
| primary blend factor return | 0.25 (-0.08, 0.35) | 0.16 (-0.09, 0.36) |
| primary NAV daily net return | 0.04 (-0.08, 0.08) | 0.02 (-0.07, 0.10) |
| `reversal_seasonality` sleeve | 0.40 (0.00, 0.55) | 0.33 (-0.03, 0.56) |
| `low_risk` sleeve | 0.18 (-0.02, 0.26) | 0.20 (-0.05, 0.29) |
| `price_momentum` sleeve | -0.11 (-0.27, 0.06) | -0.20 (-0.44, 0.11) |
| R2 on those three sleeves, median | 0.19 | 0.23 |
| R2 on all 12 sleeves, median | 0.24 | 0.33 |

- Nearest primary member: `ind_adj_rev_5` for 85 of 101 as printed; in the house form `ind_adj_rev_5` 44, `smax5`
  31, `high_52w` 9. Median largest |rho| with a member 0.53 / 0.57.
- Intercept after regressing on the 12 sleeves: HAC t > 2 for 1 alpha of 101 in each form, t < -2 for 4 and 5.
  That is no more than chance.
- Distinct signals: mean pairwise correlation 0.27 (the paper reports 0.159). The first principal component
  carries 37% (39% house form) of the variance and correlates -0.64 (-0.56) with the reversal sleeve; 3 components
  carry half. Participation ratio 6.4 (5.9). Average-linkage clusters at rho 0.5: 32 (33), of which three hold 58
  (56) alphas; clusters whose nearest sleeve is `reversal_seasonality` hold 82 (65) alphas, `price_momentum`
  (negative sign) 9 (19).
- So: about six effective signals, the largest a reversal factor the book already has; the existing sleeves span a
  quarter to a third of a typical alpha's variance and about half of a WQ sleeve's (table below).

WQ sleeves (theme `price_volume` stands in for "worldquant": the fitter only admits registered themes and the
registry was not edited):

| sleeve | members | Sharpe | HAC t | weighted tau | rho blend | rho reversal / low risk / momentum | R2 on 12 sleeves | intercept HAC t |
|---|---|---|---|---|---|---|---|---|
| slow (house form, tau <= 0.20) | 35 | 0.74 | 1.54 | 0.15 | 0.10 | 0.31 / 0.24 / -0.43 | 0.42 | 0.79 |
| all admissible house form | 80 | 0.82 | 1.61 | 0.26 | 0.28 | 0.53 / 0.29 / -0.36 | 0.52 | 0.43 |
| de-correlated (slowest of each cluster) | 28 | 1.11 | 2.02 | 0.22 | 0.28 | 0.44 / 0.25 / -0.20 | 0.31 | 1.03 |
| exact only | 35 | 0.83 | 1.65 | 0.28 | 0.31 | 0.56 / 0.29 / -0.29 | 0.50 | 0.42 |
| exact and slow | 13 | 0.68 | 1.45 | 0.16 | 0.17 | 0.39 / 0.29 / -0.36 | 0.44 | 0.47 |
| as printed, admissible | 47 | 0.66 | 1.24 | 0.48 | 0.31 | 0.58 / 0.24 / -0.23 | 0.44 | 0.03 |
| top 10 by TRAIN Sharpe (in-sample pick) | 10 | 1.27 | 2.46 | 0.33 | 0.23 | 0.51 / 0.26 / -0.36 | 0.49 | 1.76 |

## 5. Deflated Sharpe

Definition: the house one, `atx-impl/tools/backtest_integrity.py` (`expected_max_sr`, `deflated_sharpe`; the same
formulas as `nav_summ.py`): SR0 = sqrt(V) x [(1 - gamma) PhiInv(1 - 1/N) + gamma PhiInv(1 - 1/(N e))], DSR =
Phi[(SR - SR0) sqrt(T - 1) / sqrt(1 - g3 SR + (g4 - 1) SR^2 / 4)], per-session units. V = 0.0012957, the ledger's
variance of 33 book net Sharpes on research-window-v2 (read only). N shown: 1 (SR0 = 0, the plain probabilistic
Sharpe), the house construction count + 1 = 63, the house total + 1 = 234, and 234 + 202 strings run here = 436.
For single alphas I also show N = 202 with V taken from the 202 strings themselves (0.000774). The house V comes
from book Sharpes, so applying it to a single factor is a convention, not a calibration.

| object | Sharpe | N 1 | N 202, own V | N 63 | N 234 | N 436 |
|---|---|---|---|---|---|---|
| wq057h | 1.11 | 0.989 | 0.406 | 0.307 | 0.148 | 0.101 |
| wq093h | 1.08 | 0.986 | 0.389 | 0.294 | 0.143 | 0.099 |
| wq066 | 1.04 | 0.984 | 0.352 | 0.260 | 0.120 | 0.080 |
| wq048h | 1.01 | 0.980 | 0.331 | 0.242 | 0.110 | 0.073 |
| WQ sleeve, slow | 0.74 | 0.932 | - | 0.108 | 0.039 | 0.024 |
| WQ sleeve, de-correlated | 1.11 | 0.988 | - | 0.307 | 0.150 | 0.103 |
| WQ sleeve, top 10 | 1.27 | 0.996 | - | 0.429 | 0.237 | 0.172 |
| primary book NAV (net) | 1.849 | 1.000 | - | 0.833 | 0.679 | 0.599 |
| 13-theme book, slow | 1.983 | 1.000 | - | 0.896 | 0.772 | 0.699 |
| 13-theme book, slow at quarter share | 1.927 | 1.000 | - | 0.869 | 0.732 | 0.656 |

No WQ alpha or sleeve clears 0.5 at any N above 1. The book's DSR rises with its Sharpe, by construction.

## 6. Does a WQ theme help: blend and NAV

Chain per variant: the P9-B0 argv (u, fit `v4-prior-v1` + `ew-theme-std-v1` + `theme-erc-v1`, w, NAV) with only the
library, output dirs, candidate cache and fitter work dir changed, Release exes run directly. The same script
reproduces the pinned primary bit for bit (`train_combined.f64` `70d863f9`, primary daily CSV `e7eb5720`, capacity
curve `edf1001d`). The fitter admitted all 55 primary members and every WQ member offered in each variant.

Held fixed: role, label role, fields, rule `aim-partial-v5`, theta 0.05, dust, exit rate, neutralisation, cost
scenarios, tier C+ for every WQ string. Matched gross as the parent (Ruling PM6-6): calibrate at L 1.1828, then
L = 1.1828 x 0.986213 / (the cell's all-rows gross); every row below sits at gross 0.9862-0.9864. At fixed L 1.1828
the dSR is 0.001 to 0.010 higher than the matched figure and the return difference is about zero or negative,
because the 13-theme books deploy less gross (0.88 to 0.96).

Primary scenario, matched gross. dSR = variant net Sharpe minus primary; SE Memmel; CI circular block bootstrap;
p Ledoit-Wolf two-sided (all from `nav_summ.paired_stats`). "Diff" = daily net return minus the primary's.

| variant (members) | WQ weight | L | blend factor Sharpe (tau) | net Sharpe | dSR | SE | 95% CI | p | net % / y | max DD % | turnover | cost % / y | diff % / y (Sharpe, rho) |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| primary | - | 1.1828 | 1.909 (0.172) | 1.849 | - | - | - | - | 5.65 | 2.58 | 0.0281 | 1.41 | - |
| slow (35) | 13.0% | 1.2548 | 2.007 (0.160) | 1.983 | +0.133 | 0.229 | -0.27, +0.54 | 0.52 | 5.93 | 1.97 | 0.0347 | 1.62 | +0.27 (0.20, 0.897) |
| all (80) | 10.8% | 1.2451 | 1.885 (0.189) | 1.932 | +0.083 | 0.188 | -0.24, +0.41 | 0.62 | 5.92 | 2.28 | 0.0340 | 1.60 | +0.26 (0.23, 0.931) |
| de-correlated (28) | 17.4% | 1.3237 | 2.044 (0.206) | 1.947 | +0.097 | 0.193 | -0.26, +0.46 | 0.60 | 5.83 | 2.51 | 0.0404 | 1.81 | +0.18 (0.16, 0.927) |
| exact only (35) | 9.1% | 1.2285 | 1.890 (0.189) | 1.914 | +0.064 | 0.150 | -0.18, +0.32 | 0.61 | 5.87 | 2.38 | 0.0325 | 1.55 | +0.22 (0.24, 0.956) |
| exact and slow (13) | 10.0% | 1.2241 | 1.932 (0.160) | 1.923 | +0.074 | 0.172 | -0.23, +0.38 | 0.63 | 5.81 | 2.21 | 0.0320 | 1.53 | +0.15 (0.15, 0.942) |
| as printed (47) | 11.7% | 1.2722 | 1.885 (0.250) | 1.926 | +0.077 | 0.127 | -0.14, +0.30 | 0.50 | 5.84 | 2.50 | 0.0363 | 1.69 | +0.19 (0.24, 0.968) |
| slow folded into reversal (35) | no new theme | 1.2445 | 1.955 (0.120) | 1.875 | +0.026 | 0.259 | -0.46, +0.52 | 0.92 | 5.62 | 1.96 | 0.0339 | 1.58 | -0.03 (-0.02, 0.868) |
| slow at half share | about 6.5% | 1.2059 | 2.005 (0.166) | 1.975 | +0.125 | 0.108 | -0.07, +0.32 | 0.22 | 5.88 | 2.30 | 0.0304 | 1.48 | +0.22 (0.34, 0.977) |
| slow at quarter share | about 3.2% | 1.1914 | 1.966 (0.169) | 1.927 | +0.077 | 0.052 | -0.01, +0.17 | 0.12 | 5.79 | 2.44 | 0.0290 | 1.44 | +0.14 (0.44, 0.995) |
| top 10 by TRAIN Sharpe | 8.2% | 1.2167 | 2.098 (0.201) | 1.977 | +0.128 | 0.124 | -0.07, +0.34 | 0.23 | 6.02 | 2.41 | 0.0314 | 1.52 | +0.37 (0.49, 0.970) |

Cost per traded dollar is unchanged (12.5 to 12.8 bp); cost % / y is gross minus net annual mean, trading plus
financing. The blend factor Sharpe does not move for the "all", "exact" and "as printed" themes (1.885-1.890 against
1.909); their NAV gain comes after the 5% a day trading rule, not from a better instantaneous signal.

Net Sharpe (dSR) in the other four scenarios, matched gross:

| variant | linear 6 bp | terminal adverse | flat 300 financing | engine tiers |
|---|---|---|---|---|
| primary | 2.023 | 0.619 | 1.571 | 1.772 |
| slow | 2.200 (+0.177) | 0.678 (+0.059) | 1.690 (+0.119) | 1.905 (+0.133) |
| all | 2.142 (+0.118) | 0.664 (+0.046) | 1.647 (+0.077) | 1.855 (+0.084) |
| de-correlated | 2.190 (+0.166) | 0.587 (-0.032) | 1.645 (+0.075) | 1.869 (+0.097) |
| exact only | 2.112 (+0.088) | 0.666 (+0.047) | 1.632 (+0.061) | 1.837 (+0.065) |
| as printed | 2.146 (+0.123) | 0.642 (+0.024) | 1.647 (+0.076) | 1.846 (+0.075) |
| folded into reversal | 2.087 (+0.064) | 0.556 (-0.063) | 1.588 (+0.017) | 1.798 (+0.026) |
| slow at quarter share | 2.107 (+0.084) | 0.657 (+0.038) | 1.641 (+0.070) | 1.848 (+0.077) |
| top 10 | 2.172 (+0.149) | 0.743 (+0.125) | 1.702 (+0.131) | 1.898 (+0.126) |

Smallest p in these columns: 0.10 (slow at quarter share, linear). Standard errors as in the main table.

Capacity (primary scenario net Sharpe at NAV x 0.5 / 1 / 2 / 4 / 8):

| variant | x0.5 | x1 | x2 | x4 | x8 |
|---|---|---|---|---|---|
| primary | 1.900 | 1.849 | 1.798 | 1.699 | 1.554 |
| slow | 2.047 | 1.983 | 1.929 | 1.869 | 1.789 |
| all | 1.990 | 1.932 | 1.888 | 1.813 | 1.719 |
| slow at quarter share | 1.975 | 1.927 | 1.873 | 1.778 | 1.647 |
| top 10 | 2.033 | 1.977 | 1.925 | 1.839 | 1.730 |

The 13-theme books lose less Sharpe with size (x8: -0.19 for slow against -0.30 for the primary). I did not
investigate why; treat it as an observation.

Alternatives, which looks best:

1. **Fixed small share.** Best trade of gain against disturbance: correlation with the primary 0.995, turnover +3%,
   the tightest interval. It needs a composition rule that does not exist: I mixed the saved signals, (1 - lambda) x
   primary + lambda x the 13-theme "slow" blend, lambda 0.25 and 0.5, and replayed NAV on the mixture.
2. **New theme under `theme-erc-v1`.** Equal risk gives WQ 9% to 17% of the weight, which is a large allocation for
   a sleeve with HAC t 1.2 to 2.0; point estimate highest for the slow subset, with the widest error.
3. **Low-turnover only.** The slow subset beats "all" and "as printed" on point estimate and adds no more turnover
   than they do; the as-printed theme has tau 0.48 and the highest cost after the de-correlated one.
4. **Folded into `reversal_seasonality`.** No gain: it dilutes the existing reversal members (correlation with the
   primary falls to 0.87) for dSR +0.03.

## 7. Method and exact commands

All from `C:/atx-wt/pool-2`. Scripts: `.superpowers/sdd/platform-p9-20261003/wq-explore/` (below `W/`). Outputs:
`build-equity/wq-explore/` (below `X/`).

```
python W/build_library.py            # lib_wq_all.json (202 strings: 101 as printed + 101 house form), inventory.json
build-equity-rel/bin/atx-equity-strategy-ic.exe --library W/lib_wq_all.json --library-sha256 78e9d232...2ec3 \
  --train build-equity/train-2020-2023-lo3/manifest.json --train-sha256 e1c67101...95f4 \
  --train-fields build-equity/train-2020-2023-lo3-fields-v15 --train-fields-sha256 26fee5ce...3b09 \
  --output X/u-all --max-memory-mib 2560 --min-names 1000 --workers 4 --no-composition --candidate-cache X/cache
python W/wq_factors.py build-equity/p9-b0-yf0-u/summary.json X/f_primary.npz --combined build-equity/p9-b0-yf0-w/train_combined.json
python W/wq_factors.py X/u-all/summary.json X/f_wq.npz
python W/wq_stats.py                 # stats/wq_candidates.csv, stats/summary.json (clusters, DSR)
python W/wq_variants.py              # lib_<variant>.json = primary library + WQ members, variants.json
python W/wq_chain.py primary-rel atx-impl/strategies/fund_industry_ic_v8ysb.json --recipe atx-impl/strategies/fund_industry_ic_v8ysb.recipe.v2.json
python W/wq_chain.py <variant> W/lib_<variant>.json --nav-name nav-L1.1828   # u, fit, w, NAV at L 1.1828
python W/wq_gm.py <variant> ...      # matched-gross NAV into <variant>/nav-gm
python W/wq_blend.py mix slow 0.25 slowmix25 ; python W/wq_chain.py --nav-only slowmix25 X/slowmix25/w/train_combined.json --nav-name nav-L1.1828
python W/wq_blend.py stats           # stats/blend_stats.json
python W/wq_compare.py               # stats/nav_compare.csv, stats/compare_all.txt
python W/wq_plots.py
```

`wq_chain.py` holds the full argv of each stage; every stage's log starts with its argv as JSON. The w pass ran with
`--max-memory-mib 4096` (the primary's 3072 refuses a library with 12-slot strings at admission).

- Transcription: `W/wq_translate.py`. The 46 exact strings are `xwq_check.dsl(n)` unchanged. The other 55 come from
  `atx-impl/tests/fixtures/alpha101.txt` through a parser that tracks each term's price degree and inserts the
  `raw_close / close` rebase wherever a level enters a cross-sectional op, a comparison or a sum of unlike terms; on
  exact rows it reproduces the house strings (#8, #18, #28 identical; #101 algebraically equal).
- Variant rules: slow = admitted house form with tau <= 0.20; de-correlated = the slowest member of each rho-0.5
  cluster; top 10 = highest TRAIN Sharpe (reads returns on purpose). Clusters read return correlations.
- Kept: `X/stats/*`, `X/*/fit`, `X/*/nav*` (summary, daily CSVs, capacity), `X/*/u`, logs, `X/f_*.npz` (about 257 MB).
  Deleted to free disk: the candidate cache (16 GB), my fitter work copy, the saved blend payloads
  (`train_combined.f64`, `.u8`) and daily IC CSVs. `wq_factors.py`, `wq_blend.py` and the NAV steps need the chain
  re-run to recreate them (about 6 minutes for the IC pass, 2 per variant).

## 8. Not done

- No real VWAP: 43 alphas are proxies. No validation or holdout read. No pooled-era fit.
- No registered trial, no cycle or wave command, no PBO, no marginal-IC verb, no scorecards.
- Paired statistics at blend level use flat days as zero; NAV statistics use the 1,005 return rows of each replay.
- The fixed-share option is a hand mixture of two saved blends: the 12 other theme weights move slightly with it.
- I did not test other decay lengths, other tiers, sign flips, or WQ members spread across several themes.
- The capacity difference and the gap between unchanged blend factor Sharpe and higher NAV Sharpe are unexplained.

## 9. Caveats

- In-sample only: alphas, variants, clusters, ERC weights and results all use TRAIN 2020-2023 (four years, one
  regime mix including 2020).
- Multiple testing not controlled: 202 strings, 8 theme definitions, 2 mixtures, 2 leverage settings each. The DSR
  table shows how fast single-alpha evidence disappears with N.
- Costs and financing are declared scenarios, not fitted; the WQ books trade 3% to 44% more.
- The paper's alphas were built on 2010-2013 data with holding periods of 0.6 to 6.4 days; this book moves 5% a day
  toward target, so most of their speed is not used.
- Off-ledger: nothing here counts toward or against the house trial count; a registered cell would add trials.
- Prior evidence against: cell X-7, dSR -0.161 (SE 0.193), `docs/plans/2026-10-02-platform-v8-status-7.md`.
