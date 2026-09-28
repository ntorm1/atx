# Mega-alpha scorecard and alpha DSL (as of 2026-09-27)

Integration checkout `C:/atx-wt/pool-2`, branch `feat/aes-codex-integration-20260925`. All numbers are TRAIN 2020-2022 unless marked VAL. Primary cost scenario **S2** = `modeled-1bn-stale5-v1` impact costs at $1bn NAV x `swap-fin-v1` financing; daily rebalance (cadence 1); 252 sessions/yr; Sharpe on excess returns. Sources: `build-equity/mega-nav-v5-t40-summ-n13-v2.{txt,json}` (nav_summ over the 13 v5 cells), `.superpowers/sdd/mega-alpha-20260926/task-T40-report.md`, the ledger `progress.md`, handoff 4.

## 1. Headline

| | cell | S2 net SR | gross SR | mean gross lev | status |
|---|---|---|---|---|---|
| Best deployable book (passes R6' mechanics) | `v5-ew-t.05-d.1-fixed-L1.279` | **+0.712** | 1.167 | 1.002 | only cell with gross in [.90, 1.05], abs net <= .02, tau limits met |
| Highest TRAIN net SR | `v51-ew-t.05-d.1-fixed` | **+0.759** | 1.193 | 0.781 | under-deployed (gross .78); fails mechanics |
| v5 reference (R5') | `v5-ew-t.05-d.1-fixed` | **+0.742** | 1.177 | 0.782 | under-deployed (gross .78); fails mechanics |
| Best out-of-sample (VAL 2023-2024, trial #2) | v4.1 `b2 f.25` (frozen 2026-09-27) | **+0.641** | 0.802 | 0.235 | a 24%-gross book (ruling R-1), not a $1bn deployment |
| VAL trial #1 | v3 (b1 f1) | -1.27 | | 0.763 | TRAIN 1.81 -> VAL -1.27 (overfit) |

**Objective (net SR >= 1.0 at $1bn S2 with gross ~1) is not met.** Max TRAIN net +0.759; the deployable book nets +0.712. No freeze proposed; validation trial #3 unspent (owner gate U1).

## 2. All 13 v5 TRAIN cells (S2 primary)

Cell names: `<composition>-t<theta>-d<dust>-<rate>[-L<aim leverage>]`; `v51` = library v5.1 (v4 + `opex_at`). Rule aim-partial-v5, neutralization price-risk-v1. dSR is paired vs the reference cell (Memmel SE; Ledoit-Wolf studentized block-21 bootstrap, 2000 draws). "DSR x-cell" uses V[SR_n] across the 13 near-duplicate cells (SR0 .150 ann). "DSR Lo" uses the plan section 4.E Lo sampling-variance null at N = 13 (SR0 .985 ann), computed for four cells. Gross/net leverage are means over every daily row (the gate definition).

| cell | net SR | gross SR | HAC t | mu/yr | vol/yr | MDD | 2020 / 2021 / 2022 | gross lev | net lev | tau mean/p95 | cost/GMV-tau | dSR (SE) | LW 95% (p) | DSR x-cell | DSR Lo | mechanics |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| `v5-ew-t.05-d.1-fixed` | +0.742 | 1.177 | 1.46 | +2.53% | 3.41% | 3.3% | +0.2% / +3.9% / +3.4% | 0.782 | +0.0106 | 0.0433/0.0602 | 0.00095 | - | - | 0.839 | .342 | FAIL |
| `v5-ew-t.03-d.1-fixed` | +0.692 | 1.053 | 1.35 | +2.23% | 3.23% | 3.2% | -0.2% / +3.8% / +3.1% | 0.709 | +0.0065 | 0.0379/0.0580 | 0.00080 | -0.050 (0.080) | [-0.237, +0.137] (0.587) | 0.818 |  | FAIL |
| `v5-ew-t.08-d.1-fixed` | +0.681 | 1.193 | 1.34 | +2.44% | 3.57% | 3.5% | +0.1% / +3.8% / +3.4% | 0.844 | +0.0140 | 0.0490/0.0618 | 0.00109 | -0.061 (0.076) | [-0.237, +0.116] (0.485) | 0.814 |  | FAIL |
| `v5-ew-t.05-d0-fixed` | +0.724 | 1.178 | 1.43 | +2.48% | 3.42% | 3.3% | +0.3% / +3.9% / +3.1% | 0.781 | +0.0104 | 0.0464/0.0640 | 0.00095 | -0.018 (0.027) | [-0.078, +0.043] (0.520) | 0.832 |  | FAIL |
| `v5-ew-t.05-d.1-per-name` | +0.546 | 0.990 | 1.06 | +1.82% | 3.34% | 3.3% | -0.2% / +3.1% / +2.5% | 0.739 | +0.0079 | 0.0430/0.0666 | 0.00098 | -0.195 (0.096) | [-0.394, +0.003] (0.053) | 0.748 |  | FAIL |
| `v5-ew-t.05-d.1-fixed-L1.279` | +0.712 | 1.167 | 1.40 | +3.11% | 4.37% | 4.3% | +0.1% / +5.0% / +4.1% | 1.002 | +0.0148 | 0.0438/0.0609 | 0.00128 | -0.030 (0.013) | [-0.055, -0.005] (0.018) | 0.827 | .324 | PASS |
| `v5-aim-t.05-d.1-fixed` | +0.616 | 1.012 | 1.20 | +2.26% | 3.66% | 3.8% | -1.8% / +5.2% / +3.3% | 0.836 | +0.0116 | 0.0364/0.0552 | 0.00105 | -0.126 (0.112) | [-0.353, +0.102] (0.259) | 0.783 |  | FAIL |
| `v5-aim-t.03-d.1-fixed` | +0.643 | 0.983 | 1.24 | +2.25% | 3.50% | 3.7% | -1.7% / +5.1% / +3.4% | 0.775 | +0.0080 | 0.0330/0.0543 | 0.00089 | -0.099 (0.141) | [-0.398, +0.199] (0.506) | 0.795 |  | FAIL |
| `v5-aim-t.08-d.1-fixed` | +0.572 | 1.026 | 1.12 | +2.16% | 3.78% | 3.9% | -1.9% / +5.1% / +3.2% | 0.884 | +0.0148 | 0.0401/0.0549 | 0.00119 | -0.170 (0.119) | [-0.406, +0.065] (0.161) | 0.761 |  | FAIL |
| `v5-aim-t.05-d0-fixed` | +0.618 | 1.035 | 1.21 | +2.28% | 3.69% | 3.8% | -1.5% / +5.2% / +3.2% | 0.836 | +0.0112 | 0.0401/0.0599 | 0.00104 | -0.124 (0.121) | [-0.357, +0.109] (0.289) | 0.784 |  | FAIL |
| `v5-aim-t.05-d.1-per-name` | +0.449 | 0.864 | 0.87 | +1.58% | 3.52% | 3.9% | -2.0% / +4.3% / +2.4% | 0.794 | +0.0078 | 0.0363/0.0588 | 0.00110 | -0.293 (0.131) | [-0.561, -0.026] (0.032) | 0.693 | .183 | FAIL |
| `v5-aim-t.05-d.1-fixed-L1.279` | +0.607 | 1.022 | 1.18 | +2.84% | 4.69% | 4.9% | -2.3% / +6.6% / +4.2% | 1.072 | +0.0160 | 0.0371/0.0569 | 0.00141 | -0.135 (0.112) | [-0.359, +0.089] (0.220) | 0.778 |  | FAIL |
| `v51-ew-t.05-d.1-fixed` | +0.759 | 1.193 | 1.49 | +2.59% | 3.41% | 3.3% | +0.3% / +3.9% / +3.5% | 0.781 | +0.0107 | 0.0434/0.0603 | 0.00095 | +0.017 (0.011) | [-0.006, +0.040] (0.137) | 0.846 | .353 | FAIL |

## 3. Cost and financing stresses (net SR by scenario)

| cell | S1 linear 6 bps | **S2 modeled $1bn** | S2 x engine-tiers | S2 x flat-300 | S3 terminal-adverse (K = 1) |
|---|---|---|---|---|---|
| `v5-ew-t.05-d.1-fixed` | +0.933 | **+0.742** | +0.651 | +0.458 | -0.102 |
| `v5-ew-t.03-d.1-fixed` | +0.836 | **+0.692** | +0.608 | +0.432 | -0.090 |
| `v5-ew-t.08-d.1-fixed` | +0.924 | **+0.681** | +0.588 | +0.386 | -0.203 |
| `v5-ew-t.05-d0-fixed` | +0.926 | **+0.724** | +0.634 | +0.443 | -0.118 |
| `v5-ew-t.05-d.1-per-name` | +0.758 | **+0.546** | +0.459 | +0.216 | -0.338 |
| `v5-ew-t.05-d.1-fixed-L1.279` | +0.924 | **+0.712** | +0.619 | +0.428 | -0.136 |
| `v5-aim-t.05-d.1-fixed` | +0.798 | **+0.616** | +0.515 | +0.316 | -0.235 |
| `v5-aim-t.03-d.1-fixed` | +0.780 | **+0.643** | +0.548 | +0.353 | -0.149 |
| `v5-aim-t.08-d.1-fixed` | +0.797 | **+0.572** | +0.466 | +0.253 | -0.301 |
| `v5-aim-t.05-d0-fixed` | +0.809 | **+0.618** | +0.516 | +0.319 | -0.215 |
| `v5-aim-t.05-d.1-per-name` | +0.667 | **+0.449** | +0.350 | +0.091 | -0.434 |
| `v5-aim-t.05-d.1-fixed-L1.279` | +0.811 | **+0.607** | +0.507 | +0.303 | -0.234 |
| `v51-ew-t.05-d.1-fixed` | +0.950 | **+0.759** | +0.668 | +0.471 | -0.087 |

S3 writes delisted holdings off adversely; delisting returns are not modelled in S1/S2 (lane parked: T33a NO-GO, owner gate U3).

## 4. How the alphas become the book

1. **Library** `atx-impl/strategies/fund_industry_ic_v4.json` (sha `daa9663e`, 37 candidates); v5.1 = `fund_industry_ic_v5.json` (sha `9e5ea08c`, 38 = v4 + `opex_at`). Each candidate is one DSL expression evaluated daily per name by the atx-engine alpha DSL VM.
2. **Admission** `v4-prior-v1` (TRAIN only): literature prior sign, never flipped by data; rejected if redundant (|rho| > .90 with a stronger member), turnover over limit, or insufficient data. 31 of 37 admitted (v5.1: 32 of 38).
3. **Composition** `ew-theme-v1` (W_ew `9a9c949a`): 9 themes at 1/9 each, equal weight within a theme across its admitted members. Alternative `ew-theme-aim-v1` (W_aim `54f823c1`) scales each member by its Garleanu-Pedersen aim gain g_k (theta .05); it was worse on TRAIN in all 5 paired cells.
4. **Combined signal**: per candidate, unsigned centered tied rank over members, times prior sign, weighted sum (`train_combined.json`, C_ew `24a6cc76`). Neutralization context: `price-risk-v1;beta252-min126-all-instrument-market;vol63-min32;ladv63;used=member&present&ok;z-sample-sd-clip5;min50;pivot1e-8;qr-basis;fwd=r[d+2]-valid-else-0;v1`.
5. **Construction** `aim-partial-v5`: desired = tied rank of the combined signal, price-risk-v1 neutralized, gross 1, non-members 0; each session next_i = cur_i + theta (L x desired_i - cur_i) unless |L x desired_i - cur_i| <= dust / N_d. Deployable book: theta .05, dust .1, L 1.279, fixed rate.

## 5. Alpha table (library v5.1 = v4 + opex_at)

w_ew = weight in the reference composition (`ew-theme-v1`, library v4); g_k = aim gain (theta .05); w_aim = weight in `ew-theme-aim-v1`; w_ew51 = weight in `ew-theme-v1` on library v5.1. HAC t and tau are the TRAIN admission statistics of the standalone neutralized gross-1 factor (forward r[d+2]; tau = daily one-way turnover). `reject_redundant` names the member it duplicates.

| id | theme | tier | prior sign | status | HAC t | tau | w_ew | g_k | w_aim | w_ew51 |
|---|---|---|---|---|---|---|---|---|---|---|
| `chtax` | earnings_momentum | B | +1 | admitted | 0.57 | 0.0273 | 0.0278 | 0.813 | 0.0278 | 0.0278 |
| `droe` | earnings_momentum | B | +1 | admitted | 0.39 | 0.0275 | 0.0278 | 0.833 | 0.0285 | 0.0278 |
| `ear` | earnings_momentum | B+ | +1 | admitted | -0.78 | 0.0327 | 0.0278 | 0.753 | 0.0258 | 0.0278 |
| `sue` | earnings_momentum | C+ | +1 | admitted | 0.32 | 0.0275 | 0.0278 | 0.847 | 0.0290 | 0.0278 |
| `asset_growth` | investment_issuance | C+ | +1 | admitted | 0.75 | 0.0190 | 0.0556 | 0.936 | 0.0641 | 0.0556 |
| `issuance_vendor` | investment_issuance | A- | +1 | reject_redundant (net_payout) | -0.19 | 0.0195 | 0.0000 | 0.964 | 0.0000 | 0.0000 |
| `issuance_xbrl` | investment_issuance | A | +1 | reject_redundant (net_payout) | 0.74 | 0.0192 | 0.0000 | 0.949 | 0.0000 | 0.0000 |
| `noa` | investment_issuance | B | +1 | admitted | 2.07 | 0.0151 | 0.0556 | 0.976 | 0.0669 | 0.0556 |
| `low_beta` | low_risk | B- | +1 | admitted | -0.62 | 0.0476 | 0.0278 | 0.975 | 0.0334 | 0.0278 |
| `low_ivol` | low_risk | B- | +1 | admitted | -1.90 | 0.0753 | 0.0278 | 0.899 | 0.0308 | 0.0278 |
| `low_max` | low_risk | B- | +1 | admitted | -0.46 | 0.0887 | 0.0278 | 0.854 | 0.0293 | 0.0278 |
| `lowvol_ind` | low_risk | B- | +1 | admitted | -1.89 | 0.0267 | 0.0278 | 0.975 | 0.0334 | 0.0278 |
| `iv_rv_spread` | options_implied | B | +1 | admitted | 0.85 | 0.0898 | 0.1111 | 0.565 | 0.0774 | 0.1111 |
| `high_52w` | price_momentum | A- | +1 | admitted | -0.23 | 0.0521 | 0.0278 | 0.890 | 0.0305 | 0.0278 |
| `ind_mom_12_1` | price_momentum | B | +1 | admitted | 1.83 | 0.0426 | 0.0278 | 0.926 | 0.0317 | 0.0278 |
| `mom_12_1` | price_momentum | B+ | +1 | admitted | 1.54 | 0.0334 | 0.0278 | 0.918 | 0.0314 | 0.0278 |
| `within_ind_mom` | price_momentum | C+ | +1 | admitted | 0.19 | 0.0302 | 0.0278 | 0.912 | 0.0312 | 0.0278 |
| `accruals` | profitability_quality | C+ | +1 | admitted | 1.24 | 0.0184 | 0.0159 | 0.951 | 0.0186 | 0.0139 |
| `cfoa` | profitability_quality | A | +1 | admitted | 0.78 | 0.0181 | 0.0159 | 0.975 | 0.0191 | 0.0139 |
| `fscore` | profitability_quality | C+ | +1 | admitted | 0.24 | 0.0283 | 0.0159 | 0.920 | 0.0180 | 0.0139 |
| `gpa` | profitability_quality | A | +1 | admitted | 1.25 | 0.0146 | 0.0159 | 0.985 | 0.0193 | 0.0139 |
| `opbe` | profitability_quality | A- | +1 | admitted | -0.16 | 0.0191 | 0.0159 | 0.978 | 0.0191 | 0.0139 |
| `opex_at` | profitability_quality | B | +1 | admitted | 3.20 | 0.0142 | 0.0000 |  | 0.0000 | 0.0139 |
| `roa` | profitability_quality | B+ | +1 | admitted | -0.03 | 0.0189 | 0.0159 | 0.974 | 0.0191 | 0.0139 |
| `roe_q` | profitability_quality | A- | +1 | admitted | 0.67 | 0.0241 | 0.0159 | 0.907 | 0.0177 | 0.0139 |
| `ind_adj_rev_5` | reversal_seasonality | B- | +1 | admitted | 1.19 | 0.1808 | 0.0556 | 0.358 | 0.0245 | 0.0556 |
| `seasonality_same_month` | reversal_seasonality | C+ | +1 | admitted | 1.40 | 0.0938 | 0.0556 | 0.487 | 0.0333 | 0.0556 |
| `dtc` | short_interest | B+ | +1 | admitted | -0.56 | 0.0381 | 0.0370 | 0.935 | 0.0427 | 0.0370 |
| `si_change` | short_interest | B- | +1 | admitted | -0.91 | 0.0915 | 0.0370 | 0.449 | 0.0205 | 0.0370 |
| `si_ratio` | short_interest | B+ | +1 | admitted | -1.27 | 0.0325 | 0.0370 | 0.972 | 0.0444 | 0.0370 |
| `bm` | value | B | +1 | reject_redundant (cfp) | 0.60 | 0.0178 | 0.0000 | 0.980 | 0.0000 | 0.0000 |
| `cfp` | value | B+ | +1 | admitted | 0.91 | 0.0192 | 0.0278 | 0.964 | 0.0330 | 0.0278 |
| `ebit_ev` | value | B+ | +1 | reject_redundant (cfp) | 0.36 | 0.0197 | 0.0000 | 0.961 | 0.0000 | 0.0000 |
| `ep` | value | B | +1 | reject_redundant (cfp) | 0.87 | 0.0199 | 0.0000 | 0.955 | 0.0000 | 0.0000 |
| `fcfp` | value | B+ | +1 | admitted | 0.44 | 0.0195 | 0.0278 | 0.960 | 0.0329 | 0.0278 |
| `net_payout` | value | A | +1 | admitted | 0.16 | 0.0189 | 0.0278 | 0.966 | 0.0331 | 0.0278 |
| `rd_me` | value | B- | +1 | admitted | 0.91 | 0.0243 | 0.0278 | 0.978 | 0.0335 | 0.0278 |
| `sp` | value | B | +1 | reject_redundant (cfp) | 0.92 | 0.0155 | 0.0000 | 0.986 | 0.0000 | 0.0000 |

## 6. Alpha DSL strings (verbatim from `fund_industry_ic_v5.json`)


### earnings_momentum

Earnings news: SUE, change in ROE, change in tax expense and the 3-day earnings announcement return; good news predicts continuation.

- **`chtax`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Thomas and Zhang (2011, JAR) tax expense surprises
  ```
  decay_linear(rank((((txt_q - txt_q_lag4) / at_lag4) + (0 * log(at_lag4)))), 21)
  ```
- **`droe`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Hou, Mo, Xue and Zhang (2021, RF) change in ROE; Balakrishnan, Bartov and Faurel (2010, JAE)
  ```
  decay_linear(rank(((((ni_q / be_lag1q) - (ni_q_lag4 / be_lag1q_lag4)) + (0 * log(be_lag1q))) + (0 * log(be_lag1q_lag4)))), 21)
  ```
- **`ear`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Chan, Jegadeesh and Lakonishok (1996, JF) earnings announcement return; Brandt, Kishore, Santa-Clara and Venkatachalam (2008, WP)
  ```
  decay_linear(rank(ts_backfill((ts_sum((((close / delay(close, 1)) - 1) - mkt_ret), 3) + (0 * log((earn_recent * delay(earn_recent, 1))))), 126)), 21)
  ```
- **`sue`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Bernard and Thomas (1989, JAR); Livnat and Mendenhall (2006, JAR); filing-clock lag, decay: Martineau (2022, CFR)
  ```
  decay_linear(rank(sue), 21)
  ```

### investment_issuance

Low asset growth, low net operating assets and low share issuance (XBRL and split-neutral vendor), ranked within FF12 industry; low investment and issuance predict higher returns.

- **`asset_growth`** (admitted; w_ew 0.0556; w_ew51 0.0556) - Cooper, Gulen and Schill (2008, JF) asset growth (weak in big stocks: Fama and French 2008)
  ```
  decay_linear(group_rank(((-1 * ((at / at_lag4) - 1)) + (0 * log(at_lag4))), grp_ff12), 21)
  ```
- **`issuance_vendor`** (reject_redundant; w_ew 0.0000; w_ew51 0.0000) - Daniel and Titman (2006, JF) composite equity issuance over one year; Pontiff and Woodgate (2008, JF)
  ```
  decay_linear(group_rank((-1 * log((((shares_out * raw_close) / close) / delay(((shares_out * raw_close) / close), 252)))), grp_ff12), 21)
  ```
- **`issuance_xbrl`** (reject_redundant; w_ew 0.0000; w_ew51 0.0000) - Pontiff and Woodgate (2008, JF) share issuance; Daniel and Titman (2006, JF); present in big stocks (Fama and French 2008)
  ```
  decay_linear(group_rank((-1 * log((shrs_q / shrs_q_lag4))), grp_ff12), 21)
  ```
- **`noa`** (admitted; w_ew 0.0556; w_ew51 0.0556) - Hirshleifer, Hou, Teoh and Zhang (2004, JAE) net operating assets
  ```
  decay_linear(group_rank(((-1 * (noa / at_lag4)) + (0 * log(at_lag4))), grp_ff12), 21)
  ```

### low_risk

Low beta, low idiosyncratic volatility, low MAX and within-industry low volatility; low risk predicts higher risk-adjusted returns.

- **`low_beta`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Frazzini and Pedersen (2014, JFE) betting against beta
  ```
  decay_linear(rank((-1 * ((correlation(ts_sum(((close / delay(close, 1)) - 1), 3), ts_sum(mkt_ret, 3), 250) * stddev(((close / delay(close, 1)) - 1), 252)) / stddev(mkt_ret, 252)))), 21)
  ```
- **`low_ivol`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Ang, Hodrick, Xing and Zhang (2006, JF) idiosyncratic volatility
  ```
  decay_linear(rank((-1 * (stddev(((close / delay(close, 1)) - 1), 21) * signedpower(abs((1 - (correlation(((close / delay(close, 1)) - 1), mkt_ret, 21) * correlation(((close / delay(close, 1)) - 1), mkt_ret, 21)))), 0.5)))), 21)
  ```
- **`low_max`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Bali, Cakici and Whitelaw (2011, JFE) MAX
  ```
  decay_linear(rank((-1 * ts_max(((close / delay(close, 1)) - 1), 21))), 21)
  ```
- **`lowvol_ind`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Asness, Frazzini and Pedersen (2014, FAJ) low-risk investing without industry bets
  ```
  decay_linear(rank(group_neutralize((-1 * stddev(((close / delay(close, 1)) - 1), 252)), grp_ff12)), 21)
  ```

### options_implied

Implied-minus-realized volatility spread; high implied volatility relative to realized volatility predicts higher returns.

- **`iv_rv_spread`** (admitted; w_ew 0.1111; w_ew51 0.1111) - Bali and Hovakimian (2009, MS) volatility spreads: realized minus implied volatility predicts lower returns
  ```
  decay_linear(rank((ts_backfill((iv_atm_21d + (0 * log(((iv_atm_21d - 0.02) * (5 - iv_atm_21d))))), 5) - (stddev(((close / delay(close, 1)) - 1), 21) * 15.874507866387544))), 21)
  ```

### price_momentum

12-1 momentum, industry 12-1 momentum, within-industry momentum and nearness to the 52-week high; winners continue.

- **`high_52w`** (admitted; w_ew 0.0278; w_ew51 0.0278) - George and Hwang (2004, JF) 52-week high
  ```
  decay_linear(rank((close / ts_max(close, 252))), 21)
  ```
- **`ind_mom_12_1`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Moskowitz and Grinblatt (1999, JF) industry momentum
  ```
  decay_linear(rank(group_mean(((delay(close, 21) / delay(close, 252)) - 1), grp_ff49)), 21)
  ```
- **`mom_12_1`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Jegadeesh and Titman (1993, JF); 12-1 as in Fama-French UMD; crash risk: Daniel and Moskowitz (2016, JFE)
  ```
  decay_linear(rank(((delay(close, 21) / delay(close, 252)) - 1)), 21)
  ```
- **`within_ind_mom`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Asness, Porter and Stevens (2000, WP) within-industry momentum
  ```
  decay_linear(rank(group_neutralize(((delay(close, 21) / delay(close, 252)) - 1), grp_ff49)), 21)
  ```

### profitability_quality

Profitability and quality (gross, operating, cash, ROE, ROA, low accruals, F-score) and operating leverage (operating costs to assets), ranked within FF12 industry; high quality and high operating leverage predict higher returns.

- **`accruals`** (admitted; w_ew 0.0159; w_ew51 0.0139) - Sloan (1996, TAR); Hribar and Collins (2002, JAR) cash-flow-statement accruals; decay: Green, Hand and Soliman (2011, MS)
  ```
  decay_linear(group_rank(((-1 * ((ni_ttm - cfo_ttm) / ((at + at_lag4) / 2))) + (0 * log(((at + at_lag4) / 2)))), grp_ff12), 21)
  ```
- **`cfoa`** (admitted; w_ew 0.0159; w_ew51 0.0139) - Ball, Gerakos, Linnainmaa and Nikolaev (2016, JFE) cash-based operating profitability
  ```
  decay_linear(group_rank(((cfo_ttm / ((at + at_lag4) / 2)) + (0 * log(((at + at_lag4) / 2)))), grp_ff12), 21)
  ```
- **`fscore`** (admitted; w_ew 0.0159; w_ew51 0.0139) - Piotroski (2000, JAR) F-score
  ```
  decay_linear(group_rank(fscore, grp_ff12), 21)
  ```
- **`gpa`** (admitted; w_ew 0.0159; w_ew51 0.0139) - Novy-Marx (2013, JFE) gross profitability (works in large caps; replicated by Hou, Xue and Zhang 2020)
  ```
  decay_linear(group_rank(((gp_ttm / at) + (0 * log(at))), grp_ff12), 21)
  ```
- **`opbe`** (admitted; w_ew 0.0159; w_ew51 0.0139) - Fama and French (2015, JFE) operating profitability (RMW)
  ```
  decay_linear(group_rank(((oi_ttm / be) + (0 * log(be))), grp_ff12), 21)
  ```
- **`opex_at`** (admitted; w_ew 0.0000; w_ew51 0.0139) - Novy-Marx (2011, RF) operating leverage; proxy opex = sale_ttm - oi_ttm (includes D&A) because fields-v6 has no opex_ttm
  ```
  decay_linear(group_rank((((sale_ttm - oi_ttm) / at) + (0 * log(at))), grp_ff12), 21)
  ```
- **`roa`** (admitted; w_ew 0.0159; w_ew51 0.0139) - Balakrishnan, Bartov and Faurel (2010, JAE); Chen, Novy-Marx and Zhang (2011, WP)
  ```
  decay_linear(group_rank(((ni_ttm / at) + (0 * log(at))), grp_ff12), 21)
  ```
- **`roe_q`** (admitted; w_ew 0.0159; w_ew51 0.0139) - Hou, Xue and Zhang (2015, RFS) q-factor ROE
  ```
  decay_linear(group_rank(((ni_q / be_lag1q) + (0 * log(be_lag1q))), grp_ff12), 21)
  ```

### reversal_seasonality

Industry-adjusted short-term reversal and same-calendar-month seasonality.

- **`ind_adj_rev_5`** (admitted; w_ew 0.0556; w_ew51 0.0556) - Da, Liu and Schaumburg (2014, MS); Hameed and Mian (2015, JFQA) within-industry reversal
  ```
  decay_linear(rank((-1 * group_neutralize(((close / delay(close, 5)) - 1), grp_ff49))), 21)
  ```
- **`seasonality_same_month`** (admitted; w_ew 0.0556; w_ew51 0.0556) - Heston and Sadka (2008, JFE) seasonality
  ```
  decay_linear(rank(((delay(close, 224) / delay(close, 245)) - 1)), 21)
  ```

### short_interest

FINRA short interest ratio, days to cover and one-month change in short interest; heavy or rising shorting predicts lower returns.

- **`dtc`** (admitted; w_ew 0.0370; w_ew51 0.0370) - Hong, Li, Ni, Scheinkman and Yan (2015, WP) days to cover
  ```
  decay_linear(rank((-1 * si_dtc)), 21)
  ```
- **`si_change`** (admitted; w_ew 0.0370; w_ew51 0.0370) - Rapach, Ringgenberg and Zhou (2016, JFE) short interest predicts lower returns (direction)
  ```
  decay_linear(rank((-1 * ((si_shares / shares_out) - delay((si_shares / shares_out), 21)))), 21)
  ```
- **`si_ratio`** (admitted; w_ew 0.0370; w_ew51 0.0370) - Asquith, Pathak and Ritter (2005, JFE); Boehmer, Huszar and Jordan (2010, JFE)
  ```
  decay_linear(rank((-1 * (si_shares / shares_out))), 21)
  ```

### value

Price-scaled fundamentals (book, earnings, cash flow, free cash flow, EBIT/EV, net payout, sales, R&D), ranked within FF12 industry; high value predicts higher returns.

- **`bm`** (reject_redundant; w_ew 0.0000; w_ew51 0.0000) - Rosenberg, Reid and Lanstein (1985, JPM); Fama and French (1992, JF); robust pre-1963 (Linnainmaa and Roberts 2018), weak 2017-2020
  ```
  decay_linear(group_rank(((be / me_company) + (0 * log(be))), grp_ff12), 21)
  ```
- **`cfp`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Lakonishok, Shleifer and Vishny (1994, JF) cash flow to price; operating cash flow as in Desai, Rajgopal and Venkatachalam (2004, TAR)
  ```
  decay_linear(group_rank(((cfo_ttm / me_company) + (0 * log(cfo_ttm))), grp_ff12), 21)
  ```
- **`ebit_ev`** (reject_redundant; w_ew 0.0000; w_ew51 0.0000) - Loughran and Wellman (2011, JFQA) enterprise multiple (works in large caps)
  ```
  decay_linear(group_rank((((oi_ttm / ((me_company + debt) - che)) + (0 * log(oi_ttm))) + (0 * log(((me_company + debt) - che)))), grp_ff12), 21)
  ```
- **`ep`** (reject_redundant; w_ew 0.0000; w_ew51 0.0000) - Basu (1977, JF); Fama and French (1992, JF) E/P for positive earnings
  ```
  decay_linear(group_rank(((ni_ttm / me_company) + (0 * log(ni_ttm))), grp_ff12), 21)
  ```
- **`fcfp`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Lakonishok, Shleifer and Vishny (1994, JF); free cash flow yield (Hackel, Livnat and Rai 1994, FAJ)
  ```
  decay_linear(group_rank(((cfo_ttm - capx_ttm) / me_company), grp_ff12), 21)
  ```
- **`net_payout`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Boudoukh, Michaely, Richardson and Roberts (2007, JF) net payout yield
  ```
  decay_linear(group_rank((((dvc_ttm + prstkc_ttm) - sstk_ttm) / me_company), grp_ff12), 21)
  ```
- **`rd_me`** (admitted; w_ew 0.0278; w_ew51 0.0278) - Chan, Lakonishok and Sougiannis (2001, JF) R&D to market equity
  ```
  decay_linear(group_rank(((xrd_ttm / me_company) + (0 * log(xrd_ttm))), grp_ff12), 21)
  ```
- **`sp`** (reject_redundant; w_ew 0.0000; w_ew51 0.0000) - Barbee, Mukherji and Raines (1996, FAJ) sales to price
  ```
  decay_linear(group_rank(((sale_ttm / me_company) + (0 * log(sale_ttm))), grp_ff12), 21)
  ```

## 7. Trial accounting (Appendix A)

```
Trial accounting (TRAIN 2020-2022 only):
  v3 era: admission 48 + 121; composition 4 + 7; construction 14.
  since run #1: libraries v4 (37), v4.2 (40), v5.1 (38); compositions v4, v4.2, ew-theme-aim-v1, v5.1 x2;
  construction v4 1 + v4.1 grid 5 + v4.2 2 + v5 grid 10 + 2 L re-run + 1 v5.1; studies T26 5 paper books, T16, T28 audit.
  validation: #1 (v3, book level), #2 (v4.1, 24%-gross book). Per-candidate VAL statistics: never read.
  DSR inputs: N = 13, V[SR_n] = 3.083e-05 (cross-cell; Lo null REF .342), skew = -1.279, kurtosis = 14.532.
```
