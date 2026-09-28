# Task V6-L report: alpha library v6 (`generate_fund_ic_v6.py` -> `fund_industry_ic_v6.json`)

**Status:** DONE_WITH_CONCERNS. Implementer: Opus 5.5, pool-5, branch `feat/mega-alpha-v6-l-20260927` from base
`04e9d5bc` (pool-2 HEAD; supersedes `e0dfb8c7` in the brief). Nothing was built. No real data were run. No TRAIN
return statistic was used to choose a definition, sign, window or roster position, and nothing dated 2023 or later
was read.

| item | value |
|---|---|
| commit | `9d302e4c325e68b7b961b5650d2171c5b6326762` (single commit on top of `04e9d5bc`) |
| `fund_industry_ic_v6.json` sha256 | **`5ee66d137c60527c4b00f948e8b15c04f2bddc789e988b660a4254a6df6e896a`** (29,795 B) |
| `fund_industry_ic_v6.recipe.json` sha256 | `36c084522237b756d2847bc3e589855fafe589bba55b5111642c444afecfc72b` (115,751 B) |
| roster | 38 (limit 48): 5 new, 31 smoothing-changed, 2 byte-identical, 5 v5.1 ids removed |
| static maxima | prior bars 272 (bound 314), peak slots 7 (limit 7), extra fields/candidate 5 (limit 5), DAG nodes 26 |
| parent pins | v5.1 library `9e5ea08c`, recipe `a26670b0` (re-derived from their generator and asserted) |

Files (all under `atx-impl/strategies/`):
- `generate_fund_ic_v6.py`: the generator. `--check` verifies the committed bytes. It uses the same JSON
  canonicalisation as v5 (`json.dumps(indent=2, ensure_ascii=True, allow_nan=False) + "\n"`). The static validator is
  a private copy of v4's with 2 registry rows added (`power`, `ts_count_nans`). Both rows are cross-checked against
  `registry.cpp` and the typecheck classes ("registry cross-check ok (16 operators)").
- `fund_industry_ic_v6.json` + `fund_industry_ic_v6.recipe.json`. The recipe holds the lineage (per id:
  `prior_sign_source`, `smoothing_form`, `v51_change`), the templates, `v6_changes`, `needs_new_field`,
  `lowvol_ind_argument` and the static validation.
- `check_fund_ic_v6.py`: a pure-Python check that does not depend on the generator. It verifies unique ids and DSL
  strings, prior sign +1 present, theme/tier/citation, that every field is in the given manifest, the op allowlist
  and denylist, roster <= 48, DSL <= 4096 B and <= 5 extras. It also prints the diff against v5.1.
- `test_generate_fund_ic_v6.py`: 15 pytest cases on synthetic data. They reuse the v4 numpy mirror of the VM,
  extended with `power`, `ts_count_nans`, `ts_regression` and a member-masked Cs op.

## 1. What changed vs library v5.1 (one variant per hypothesis, all prior-signed)

| brief item | v6 result |
|---|---|
| price_momentum: residual momentum | **added `res_mom_12_1`**: 12-1 standardized market-model residual return (Blitz-Huij-Martens 2011). This is the closest expressible proxy. mkt_ret is the only price-risk-v1 exposure that is a return series in the DSL. vol63 and ladv63 are characteristics. FF49 industry returns would need a member-masked `group_mean` inside a 231-session window. The base is algebraically identical to v4.2's `res_mom_12_1` (beta = rho sd_r / sd_m), respelled to fit 7 slots; v4.2's spelling needs 8. Inserted before `mom_12_1`. |
| profitability_quality: CbOP (BGLN 2016) | **`cfoa` -> `cbop`** = (cfo_ttm + R&D) / at within FF12, with unreported R&D set to 0. This is the cash-flow-statement approximation. `cfoa` cited the same paper, so keeping both would give two variants of one hypothesis. It takes `cfoa`'s slot. |
| value: composite value | **added `value_composite`** = mean within-FF12 rank of decayed B/M, E/P and CF/P. Numerators keep their sign. **`ebit_ev` is not included**: the 4 ratios need 8 extra fields against the budget of 5. Inserted before `bm`. |
| value: intangible-adjusted value | not added. It needs `xsga_ttm` (SG&A) plus a perpetual-inventory R&D history. See section 3. |
| investment_issuance: 5-year CI / XFIN, else 1-year net share issuance | not added. 5-year CI needs 1260 bars. The fallback (1-year net share issuance) is already present twice: `issuance_xbrl` (Pontiff-Woodgate) and `issuance_vendor` (1-year Daniel-Titman CI). XFIN needs 6 extras. See section 3. |
| reversal_seasonality: Heston-Sadka multi-lag | not added. Lags 24 and 36 need 504 and 756 bars (bound 314; the role starts 2018-06-01). `seasonality_same_month` (lag 12) is kept. See section 3. |
| low_risk: replace low_beta / low_ivol / low_max with BAC and SMAX | **`low_beta` -> `bac`** (-corr of overlapping 3-day returns with the member market, 250 sessions; Asness-Frazzini-Gormsen-Pedersen 2020). **`low_max` -> `smax`** (-MAX1(21) / sd(252)). **`low_ivol` removed.** |
| low_risk: lowvol_ind only if not spanned by vol63 | **removed**. The structural argument is in section 2.3. |
| short_interest: long-window FINRA shorting flow | not added. fields-v6 has no daily short-sale volume. See section 3. |
| earnings_momentum: EAR-centred | the event-date field (`earn_recent`) exists, and **`ear` already is** the [-1,+1] market-adjusted return around the vendor reaction session, carried to the next announcement (<= 126 sessions). It is kept with the smoothing change only. A second EAR variant would break one-variant-per-hypothesis. |
| smoothing | see section 2. |

v6 roster by theme: value (9) `value_composite, bm, ep, cfp, fcfp, ebit_ev, net_payout, sp, rd_me`;
profitability_quality (8) `gpa, opbe, cbop, roe_q, roa, accruals, fscore, opex_at`; investment_issuance (4);
earnings_momentum (4); price_momentum (5) `res_mom_12_1, mom_12_1, ind_mom_12_1, within_ind_mom, high_52w`; low_risk
(2) `bac, smax`; short_interest (3); reversal_seasonality (2); options_implied (1). The 9 families are unchanged:
theme merging and dropping belong to V6-W.

Tiers of the new members follow the v6-literature grades: residual momentum B+, CbOP A-, BAC/SMAX B+. The composite
takes cfp's B+.

Roster-order rule (this sets the (tier, roster) redundancy order of prereg R3; no statistic is used):
- A replacement takes the slot of the member it replaces.
- An addition that the literature ranks as the preferred construction of a cluster goes directly before the
  incumbent it upgrades. `value_composite` goes before `bm` (Israel-Laursen-Richardson 2021: composites average out
  ratio noise). `res_mom_12_1` goes before `mom_12_1` (Blitz-Huij-Martens 2011).

## 2. Smoothing (brief rule; review I3 / I5)

### 2.1 Blackout rule

`vm.hpp:425-441` (`set_cross_section_mask`, set to `decision_member` by `strategy_ic_runner.cpp:1605`) makes every
Cs op NaN for non-members. Ts ops are full-window any-NaN -> NaN (`ts_ops.hpp:25-26`). So `decay_linear(R(x), 21)`
blanks a name for 21 sessions after any membership gap. Field loads keep non-member history.

The switch to `R(decay_linear(x, 21))` removes the blackout **iff x holds no Cs op**. The generator derives this
from the parsed base (`has_cs_op`), and a test transcribes the brief independently:
- **Switched (27 v5.1 members):** bm, ep, cfp, fcfp, ebit_ev, net_payout, sp, rd_me, gpa, opbe, roe_q, roa,
  accruals, fscore, opex_at, asset_growth, noa, issuance_xbrl, issuance_vendor, sue, droe, chtax, ear, mom_12_1,
  high_52w, si_ratio, dtc.
- **Byte-identical:** `ind_mom_12_1` (group_mean inside) and `within_ind_mom` (group_neutralize inside). Their x is
  itself masked, so the blackout binds under either order. The test demonstrates this.
- The test `test_switch_removes_the_membership_blackout` checks the behaviour on a synthetic panel with a
  one-session membership gap. The old form is NaN for 21 sessions. The new form is NaN on the gap day only.

### 2.2 Fast sleeves

Fast sleeves are members with standalone tau >= .08 in `build-equity/mega-weights-v51-ew/admission.json` (sha
`4f06bd18`). Only id and tau were read from that file. The members are ind_adj_rev_5 (.1808), seasonality_same_month
(.0938), si_change (.0915) and iv_rv_spread (.0898); low_max (.0887) is replaced.

- They become `R(x)` with no decay.
- **`seasonality_same_month` is re-centred** from `delay(224)/delay(245)` to the undecayed same-month-last-year
  window `delay(231)/delay(252)`. v4 had shifted the window 7 sessions only to offset the s21 decay's centre lag.
  This is disclosed in concerns.

The additions take `R(decay_linear(x, 21))` (the s21 default; they have no measured tau). The composite decays each
ratio before its rank.

### 2.3 Why lowvol_ind is spanned (structural, return-free)

lowvol_ind = -(v - m), where v = sd(ret, 252) and m is its FF12 mean. price-risk-v1 projects the target on z(vol63)
(plus beta252 and ladv63). Cross-sectionally vol252 and vol63 measure the same level.

Write v = m + w (w orthogonal to m). The residual of s = -w on v is -(var m / var v) w + (var w / var v) m.
Within-industry dispersion dominates cross-sectional volatility. So the prior-consistent -w part is mostly
projected out. What survives loads positively on the industry-mean volatility m: a long-high-volatility-industry
bet, which is the opposite of the low-risk prior (AFP 2014). Hence spanned, and removed. The same text is in the
recipe as `lowvol_ind_argument`.

## 3. Needs new field (or a budget / engine ruling); nothing was invented

| hypothesis | blocker | needed |
|---|---|---|
| Heston-Sadka (2008) seasonality at annual lags 12, 24, 36 | lags 24/36 need 504/756 prior bars. That is above the 314-bar DSL bound (the runner admits <= score_begin - 63 = 336 on TRAIN role v2) and before the role's first session (2018-06-01). | producer field e.g. `seas_same_month_y2_y5` (mean same-calendar-month return, annual lags 2..5, PIT), or a role with >= 820 warm-up sessions |
| Long-window FINRA shorting flow (Wang-Yan-Zheng 2020), 63-126 session mean of short volume / volume | fields-v6 has FINRA short *interest* (`si_shares`, `si_dtc`) only | `finra_short_volume` (daily consolidated Reg SHO short-sale volume, PIT after publication, volume share basis); then `rank(decay_linear(-ts_mean(finra_short_volume / volume, 126), 21))` |
| Intangible-adjusted value (Eisfeldt-Kim-Papanikolaou 2022) | no SG&A; the R&D perpetual inventory needs about 10 years of R&D | `xsga_ttm` + a producer intangible-capital field (R&D at 15%, 30% of SG&A at 20% depreciation) |
| 5-year composite issuance (Daniel-Titman 2006) | needs 1260 prior bars | producer `ci_5y` (or a 5-year shares history). The 1-year fallback is already in the library. |
| XFIN (Bradshaw-Richardson-Sloan 2006) | expressible as `group_rank(decay_linear(-((sstk_ttm - prstkc_ttm - dvc_ttm + debt - delay(debt, 252)) / at), 21), grp_ff12)`, but that is **6 extra fields** (budget 5), and the debt change is a balance proxy | a ruling allowing 6 extras, or producer `dltis_ttm`/`dltr_ttm`/`xfin_ttm` |
| Composite value incl. EBIT/EV | 8 extra fields | a ruling, or a producer `ev_company` field |
| CbOP income-statement form (BGLN 2016) | no COGS, SG&A, working-capital items or interest expense | `cogs_ttm`, `xsga_ttm`, 6 working-capital balances (+ lags), `xint_ttm` |
| SMAX with MAX5 (mean of the 5 largest daily returns) | no top-k order-statistic op in the DSL. This is an engine op, not a field. | `ts_topk_mean` op or a producer `max5_21d` |

Budget arithmetic for a ruling, from the `admit()` formula and `breadth-check-v5.md` §4:
- v5.1 `--plan-only` is 1,449,071,914 B (1,381.9 MiB), against the IC runner's `--max-memory-mib 1536`.
- v6 has the same maxima (7 slots, 5 extras, 38 candidates), so its required_bytes are unchanged.
- Each extra field-capacity step adds cells x 8 B, about 52.0 MB (1155 x 5627 cells).
- Capacity 6 gives about 1,431 MiB, which is still under 1536.

## 4. Candidate table (all 38; DSL verbatim from `fund_industry_ic_v6.json`)

"prior sign" is the embedded sign: every candidate has prior_sign +1. The raw literature direction and its
author-year source are also in the generator's `PRIOR_SIGN_SOURCES`.

| # | id | theme | tier | prior sign (raw dir; source) | DSL | change vs v5.1 | citation |
|---|---|---|---|---|---|---|---|
| 1 | `value_composite` | value | B+ | +1 (raw +1; Fama-French 1992; Lakonishok-Shleifer-Vishny 1994; Israel-Laursen-Richardson 2021) | `(((group_rank(decay_linear((be / me_company), 21), grp_ff12) + group_rank(decay_linear((ni_ttm / me_company), 21), grp_ff12)) + group_rank(decay_linear((cfo_ttm / me_company), 21), grp_ff12)) / 3)` | added | Fama and French (1992, JF); Lakonishok, Shleifer and Vishny (1994, JF); Israel, Laursen and Richardson (2021, JPM) composite value; Asness and Frazzini (2013, JPM) current price |
| 2 | `bm` | value | B | +1 (raw +1; Rosenberg-Reid-Lanstein 1985; Fama-French 1992) | `group_rank(decay_linear(((be / me_company) + (0 * log(be))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Rosenberg, Reid and Lanstein (1985, JPM); Fama and French (1992, JF); robust pre-1963 (Linnainmaa and Roberts 2018), weak 2017-2020 |
| 3 | `ep` | value | B | +1 (raw +1; Basu 1977) | `group_rank(decay_linear(((ni_ttm / me_company) + (0 * log(ni_ttm))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Basu (1977, JF); Fama and French (1992, JF) E/P for positive earnings |
| 4 | `cfp` | value | B+ | +1 (raw +1; Lakonishok-Shleifer-Vishny 1994) | `group_rank(decay_linear(((cfo_ttm / me_company) + (0 * log(cfo_ttm))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Lakonishok, Shleifer and Vishny (1994, JF) cash flow to price; operating cash flow as in Desai, Rajgopal and Venkatachalam (2004, TAR) |
| 5 | `fcfp` | value | B+ | +1 (raw +1; Lakonishok-Shleifer-Vishny 1994; Hackel-Livnat-Rai 1994) | `group_rank(decay_linear(((cfo_ttm - capx_ttm) / me_company), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Lakonishok, Shleifer and Vishny (1994, JF); free cash flow yield (Hackel, Livnat and Rai 1994, FAJ) |
| 6 | `ebit_ev` | value | B+ | +1 (raw +1; Loughran-Wellman 2011) | `group_rank(decay_linear((((oi_ttm / ((me_company + debt) - che)) + (0 * log(oi_ttm))) + (0 * log(((me_company + debt) - che)))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Loughran and Wellman (2011, JFQA) enterprise multiple (works in large caps) |
| 7 | `net_payout` | value | A | +1 (raw +1; Boudoukh-Michaely-Richardson-Roberts 2007) | `group_rank(decay_linear((((dvc_ttm + prstkc_ttm) - sstk_ttm) / me_company), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Boudoukh, Michaely, Richardson and Roberts (2007, JF) net payout yield |
| 8 | `sp` | value | B | +1 (raw +1; Barbee-Mukherji-Raines 1996) | `group_rank(decay_linear(((sale_ttm / me_company) + (0 * log(sale_ttm))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Barbee, Mukherji and Raines (1996, FAJ) sales to price |
| 9 | `rd_me` | value | B- | +1 (raw +1; Chan-Lakonishok-Sougiannis 2001) | `group_rank(decay_linear(((xrd_ttm / me_company) + (0 * log(xrd_ttm))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Chan, Lakonishok and Sougiannis (2001, JF) R&D to market equity |
| 10 | `gpa` | profitability_quality | A | +1 (raw +1; Novy-Marx 2013) | `group_rank(decay_linear(((gp_ttm / at) + (0 * log(at))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Novy-Marx (2013, JFE) gross profitability (works in large caps; replicated by Hou, Xue and Zhang 2020) |
| 11 | `opbe` | profitability_quality | A- | +1 (raw +1; Fama-French 2015) | `group_rank(decay_linear(((oi_ttm / be) + (0 * log(be))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Fama and French (2015, JFE) operating profitability (RMW) |
| 12 | `cbop` | profitability_quality | A- | +1 (raw +1; Ball-Gerakos-Linnainmaa-Nikolaev 2016) | `group_rank(decay_linear((((cfo_ttm + (power(xrd_ttm, (1 - ts_count_nans(xrd_ttm, 1))) - ts_count_nans(xrd_ttm, 1))) / at) + (0 * log(at))), 21), grp_ff12)` | replaces cfoa | Ball, Gerakos, Linnainmaa and Nikolaev (2016, JFE) cash-based operating profitability (subsumes accruals); operating profitability excludes R&D: Ball, Gerakos, Linnainmaa and Nikolaev (2015, JFE) |
| 13 | `roe_q` | profitability_quality | A- | +1 (raw +1; Hou-Xue-Zhang 2015) | `group_rank(decay_linear(((ni_q / be_lag1q) + (0 * log(be_lag1q))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Hou, Xue and Zhang (2015, RFS) q-factor ROE |
| 14 | `roa` | profitability_quality | B+ | +1 (raw +1; Balakrishnan-Bartov-Faurel 2010) | `group_rank(decay_linear(((ni_ttm / at) + (0 * log(at))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Balakrishnan, Bartov and Faurel (2010, JAE); Chen, Novy-Marx and Zhang (2011, WP) |
| 15 | `accruals` | profitability_quality | C+ | +1 (raw -1; Sloan 1996) | `group_rank(decay_linear(((-1 * ((ni_ttm - cfo_ttm) / ((at + at_lag4) / 2))) + (0 * log(((at + at_lag4) / 2)))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Sloan (1996, TAR); Hribar and Collins (2002, JAR) cash-flow-statement accruals; decay: Green, Hand and Soliman (2011, MS) |
| 16 | `fscore` | profitability_quality | C+ | +1 (raw +1; Piotroski 2000) | `group_rank(decay_linear(fscore, 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Piotroski (2000, JAR) F-score |
| 17 | `asset_growth` | investment_issuance | C+ | +1 (raw -1; Cooper-Gulen-Schill 2008) | `group_rank(decay_linear(((-1 * ((at / at_lag4) - 1)) + (0 * log(at_lag4))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Cooper, Gulen and Schill (2008, JF) asset growth (weak in big stocks: Fama and French 2008) |
| 18 | `noa` | investment_issuance | B | +1 (raw -1; Hirshleifer-Hou-Teoh-Zhang 2004) | `group_rank(decay_linear(((-1 * (noa / at_lag4)) + (0 * log(at_lag4))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Hirshleifer, Hou, Teoh and Zhang (2004, JAE) net operating assets |
| 19 | `issuance_xbrl` | investment_issuance | A | +1 (raw -1; Pontiff-Woodgate 2008) | `group_rank(decay_linear((-1 * log((shrs_q / shrs_q_lag4))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Pontiff and Woodgate (2008, JF) share issuance; Daniel and Titman (2006, JF); present in big stocks (Fama and French 2008) |
| 20 | `issuance_vendor` | investment_issuance | A- | +1 (raw -1; Daniel-Titman 2006) | `group_rank(decay_linear((-1 * log((((shares_out * raw_close) / close) / delay(((shares_out * raw_close) / close), 252)))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Daniel and Titman (2006, JF) composite equity issuance over one year; Pontiff and Woodgate (2008, JF) |
| 21 | `sue` | earnings_momentum | C+ | +1 (raw +1; Bernard-Thomas 1989) | `rank(decay_linear(sue, 21))` | smoothing: decay(R(x)) -> R(decay(x)) | Bernard and Thomas (1989, JAR); Livnat and Mendenhall (2006, JAR); filing-clock lag, decay: Martineau (2022, CFR) |
| 22 | `droe` | earnings_momentum | B | +1 (raw +1; Hou-Mo-Xue-Zhang 2021) | `rank(decay_linear(((((ni_q / be_lag1q) - (ni_q_lag4 / be_lag1q_lag4)) + (0 * log(be_lag1q))) + (0 * log(be_lag1q_lag4))), 21))` | smoothing: decay(R(x)) -> R(decay(x)) | Hou, Mo, Xue and Zhang (2021, RF) change in ROE; Balakrishnan, Bartov and Faurel (2010, JAE) |
| 23 | `chtax` | earnings_momentum | B | +1 (raw +1; Thomas-Zhang 2011) | `rank(decay_linear((((txt_q - txt_q_lag4) / at_lag4) + (0 * log(at_lag4))), 21))` | smoothing: decay(R(x)) -> R(decay(x)) | Thomas and Zhang (2011, JAR) tax expense surprises |
| 24 | `ear` | earnings_momentum | B+ | +1 (raw +1; Chan-Jegadeesh-Lakonishok 1996; Brandt-Kishore-Santa-Clara-Venkatachalam 2008) | `rank(decay_linear(ts_backfill((ts_sum((((close / delay(close, 1)) - 1) - mkt_ret), 3) + (0 * log((earn_recent * delay(earn_recent, 1))))), 126), 21))` | smoothing: decay(R(x)) -> R(decay(x)) | Chan, Jegadeesh and Lakonishok (1996, JF) earnings announcement return; Brandt, Kishore, Santa-Clara and Venkatachalam (2008, WP) |
| 25 | `res_mom_12_1` | price_momentum | B+ | +1 (raw +1; Blitz-Huij-Martens 2011) | `rank(decay_linear(delay((((ts_sum(((close / delay(close, 1)) - 1), 231) / stddev(((close / delay(close, 1)) - 1), 231)) - (correlation(((close / delay(close, 1)) - 1), mkt_ret, 231) * (ts_sum(mkt_ret, 231) / stddev(mkt_ret, 231)))) / signedpower(abs((1 - (correlation(((close / delay(close, 1)) - 1), mkt_ret, 231) * correlation(((close / delay(close, 1)) - 1), mkt_ret, 231)))), 0.5)), 21), 21))` | added | Blitz, Huij and Martens (2011, JEF) residual momentum; Blitz, Hanauer and Vidojevic (2020, JEF) idiosyncratic momentum; Grundy and Martin (2001, RFS) |
| 26 | `mom_12_1` | price_momentum | B+ | +1 (raw +1; Jegadeesh-Titman 1993) | `rank(decay_linear(((delay(close, 21) / delay(close, 252)) - 1), 21))` | smoothing: decay(R(x)) -> R(decay(x)) | Jegadeesh and Titman (1993, JF); 12-1 as in Fama-French UMD; crash risk: Daniel and Moskowitz (2016, JFE) |
| 27 | `ind_mom_12_1` | price_momentum | B | +1 (raw +1; Moskowitz-Grinblatt 1999) | `decay_linear(rank(group_mean(((delay(close, 21) / delay(close, 252)) - 1), grp_ff49)), 21)` | unchanged (x holds a Cs op) | Moskowitz and Grinblatt (1999, JF) industry momentum |
| 28 | `within_ind_mom` | price_momentum | C+ | +1 (raw +1; Asness-Porter-Stevens 2000) | `decay_linear(rank(group_neutralize(((delay(close, 21) / delay(close, 252)) - 1), grp_ff49)), 21)` | unchanged (x holds a Cs op) | Asness, Porter and Stevens (2000, WP) within-industry momentum |
| 29 | `high_52w` | price_momentum | A- | +1 (raw +1; George-Hwang 2004) | `rank(decay_linear((close / ts_max(close, 252)), 21))` | smoothing: decay(R(x)) -> R(decay(x)) | George and Hwang (2004, JF) 52-week high |
| 30 | `bac` | low_risk | B+ | +1 (raw -1; Asness-Frazzini-Gormsen-Pedersen 2020) | `rank(decay_linear((-1 * correlation(ts_sum(((close / delay(close, 1)) - 1), 3), ts_sum(mkt_ret, 3), 250)), 21))` | replaces low_beta | Asness, Frazzini, Gormsen and Pedersen (2020, JFE) betting against correlation; correlation of overlapping 3-day returns: Frazzini and Pedersen (2014, JFE) |
| 31 | `smax` | low_risk | B+ | +1 (raw -1; Asness-Frazzini-Gormsen-Pedersen 2020; Bali-Cakici-Whitelaw 2011) | `rank(decay_linear((-1 * ((ts_max(((close / delay(close, 1)) - 1), 21) / stddev(((close / delay(close, 1)) - 1), 252)) + (0 * log(stddev(((close / delay(close, 1)) - 1), 252))))), 21))` | replaces low_max | Asness, Frazzini, Gormsen and Pedersen (2020, JFE) scaled MAX (SMAX); Bali, Cakici and Whitelaw (2011, JFE) MAX |
| 32 | `si_ratio` | short_interest | B+ | +1 (raw -1; Asquith-Pathak-Ritter 2005) | `rank(decay_linear((-1 * (si_shares / shares_out)), 21))` | smoothing: decay(R(x)) -> R(decay(x)) | Asquith, Pathak and Ritter (2005, JFE); Boehmer, Huszar and Jordan (2010, JFE) |
| 33 | `dtc` | short_interest | B+ | +1 (raw -1; Hong-Li-Ni-Scheinkman-Yan 2015) | `rank(decay_linear((-1 * si_dtc), 21))` | smoothing: decay(R(x)) -> R(decay(x)) | Hong, Li, Ni, Scheinkman and Yan (2015, WP) days to cover |
| 34 | `si_change` | short_interest | B- | +1 (raw -1; Rapach-Ringgenberg-Zhou 2016 (direction)) | `rank((-1 * ((si_shares / shares_out) - delay((si_shares / shares_out), 21))))` | smoothing: decay removed (fast sleeve) | Rapach, Ringgenberg and Zhou (2016, JFE) short interest predicts lower returns (direction) |
| 35 | `ind_adj_rev_5` | reversal_seasonality | B- | +1 (raw -1; Da-Liu-Schaumburg 2014; Hameed-Mian 2015) | `rank((-1 * group_neutralize(((close / delay(close, 5)) - 1), grp_ff49)))` | smoothing: decay removed (fast sleeve) | Da, Liu and Schaumburg (2014, MS); Hameed and Mian (2015, JFQA) within-industry reversal |
| 36 | `seasonality_same_month` | reversal_seasonality | C+ | +1 (raw +1; Heston-Sadka 2008) | `rank(((delay(close, 231) / delay(close, 252)) - 1))` | smoothing: decay removed (fast sleeve, window re-centred) | Heston and Sadka (2008, JFE) seasonality |
| 37 | `iv_rv_spread` | options_implied | B | +1 (raw +1; Bali-Hovakimian 2009) | `rank((ts_backfill((iv_atm_21d + (0 * log(((iv_atm_21d - 0.02) * (5 - iv_atm_21d))))), 5) - (stddev(((close / delay(close, 1)) - 1), 21) * 15.874507866387544)))` | smoothing: decay removed (fast sleeve) | Bali and Hovakimian (2009, MS) volatility spreads: realized minus implied volatility predicts lower returns |
| 38 | `opex_at` | profitability_quality | B | +1 (raw +1; Novy-Marx 2011) | `group_rank(decay_linear((((sale_ttm - oi_ttm) / at) + (0 * log(at))), 21), grp_ff12)` | smoothing: decay(R(x)) -> R(decay(x)) | Novy-Marx (2011, RF) operating leverage; proxy opex = sale_ttm - oi_ttm (includes D&A) because fields-v6 has no opex_ttm |

Removed v5.1 ids:
- `cfoa`: replaced by `cbop`.
- `low_beta`: replaced by `bac`.
- `low_max`: replaced by `smax`.
- `low_ivol`: replaced by bac/smax, per prereg V6-L.
- `lowvol_ind`: spanned (section 2.3).

## 5. Checks run (no real data)

- `python atx-impl/strategies/check_fund_ic_v6.py --manifest C:/atx-wt/pool-2/build-equity/recent-fast-train-2020-2022-v2-fields-v6/manifest.json`
  (the fields-v6 TRAIN manifest, sha `32565c32`, 40 fields; metadata only) -> exit 0:
  `check: ok (ids unique, prior signs present, fields in manifest, no forbidden ops, roster 38 <= 48)`.
  - Diff vs v5.1: added 5 `[value_composite, cbop, res_mom_12_1, bac, smax]`; removed 5
    `[cfoa, low_beta, low_ivol, low_max, lowvol_ind]`; changed 31; unchanged 2 `[ind_mom_12_1, within_ind_mom]`.
  - Operators used: abs, correlation, decay_linear, delay, group_mean, group_neutralize, group_rank, log, power,
    rank, signedpower, stddev, ts_backfill, ts_count_nans, ts_max, ts_sum.
  - Fields used: 37 (36 research fields + close/raw_close; `volume` is declared, unused).
- `python atx-impl/strategies/generate_fund_ic_v6.py --check`: ok (bytes above; registry cross-check ok; grp_
  typing T22 checked).
- `pytest atx-impl/strategies/test_generate_fund_ic_v6.py atx-impl/strategies/test_generate_fund_ic_v5.py`: 25
  passed. The v6 file covers:
  - determinism, and tamper rejection by `--check`;
  - roster edits vs an independent transcription of the brief;
  - forms per id;
  - budget;
  - the fitter's `load_priors` on the v6 library + recipe (themes within V4_THEMES, signs all +1);
  - causality and lookback for all 38 candidates on the synthetic panel;
  - blackout removal;
  - cbop's zero-fill;
  - `res_mom_12_1` equal to v4.2's base (rtol 1e-9);
  - composite = mean of the three within-FF12 ranks;
  - smax/bac orientation;
  - the check script (pass, plus a forbidden op, prior_sign -1, missing field and duplicate id).

## 6. Root command lines (v51_train.sh-style; for the controller's `studies/v6_train.sh`)

These use the same conventions as `v51_train.sh`: they run from `C:/atx-wt/pool-2`, and every input is pinned.

```bash
PY="C:/Program Files/Python312/python.exe"
BR="scripts/run_bounded_research.py --seconds 180 --max-rss-mib 1536 --min-free-mib 512"
IC=build-equity/bin/atx-equity-strategy-ic.exe; NAV=build-equity/bin/atx-equity-strategy-targets.exe
L=atx-impl/strategies/fund_industry_ic_v6.json;         LS=5ee66d137c60527c4b00f948e8b15c04f2bddc789e988b660a4254a6df6e896a
LR=atx-impl/strategies/fund_industry_ic_v6.recipe.json; LRS=36c084522237b756d2847bc3e589855fafe589bba55b5111642c444afecfc72b
R2=build-equity/recent-fast-train-2020-2022-v2/manifest.json; R2S=210fff9687aa6b16e74c65104d77a916d708dca6986e77b06bac27556c48d1de
FD=build-equity/recent-fast-train-2020-2022-v2-fields-v6;     FS=32565c3212a0b06a4a0a1185aabf07aea2fc043906ff767e8489233a0ddfd7a8
CC=build-equity/mega-candidate-cache
U=build-equity/mega-v6l-train-u; WORK=build-equity/mega-fit-work-v6l; W=build-equity/mega-weights-v6l-ew; WT=build-equity/mega-v6lw-train-ew

# 0. metadata-only native compile (no payload): parse/typecheck of power / ts_count_nans, slots, lookback, bytes
$IC --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS \
  --max-memory-mib 1536 --min-names 1000 --workers 4 --plan-only
#    expect max_compiled_slots <= 7, required_lookback 272, required_bytes ~= v5.1 (1,449,071,914)

# 1. u pass (admission inputs), i = 1..3 bounded passes, resumable through the candidate cache
"$PY" $BR --output $U-run$i --bind $IC --bind $L --bind $R2 --bind $FD/manifest.json -- $IC --library $L \
  --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS --output $U-$i \
  --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC
#    then: O=$U-$i/orientations.json OS=$(sha256 O) S=$U-$i/summary.json SS=$(sha256 S)

# 2. fit = admission v4-prior-v1 (38 trials) + the one composition (COMP = the V6-W rule, e.g. ew-theme-v6; ew-theme-v1 as the paired reference), j = 1..3
"$PY" $BR --output $W-run$j --bind $L --bind $LR --bind $R2 --bind $O --bind $S -- "$PY" \
  atx-impl/tools/fit_composition_weights.py --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S \
  --orientations $O --orientations-sha256 $OS --runner-summary $S --runner-summary-sha256 $SS \
  --orientation prior --screen v4-prior-v1 --composition "$COMP" --recipe $LR --recipe-sha256 $LRS \
  --work-dir $WORK --max-seconds 150 --output $W

# 3. weighted pass (C = train_combined.json), i = 1..3
"$PY" $BR --output $WT-run$i --bind $IC --bind $R2 --bind $FD/manifest.json --bind $L --bind $W/composition_weights.json -- \
  $IC --library $L --library-sha256 $LS --train $R2 --train-sha256 $R2S --train-fields $FD --train-fields-sha256 $FS \
  --output $WT-$i --max-memory-mib 1536 --min-names 1000 --workers 4 --save-combined --candidate-cache $CC \
  --composition-weights $W/composition_weights.json --composition-weights-sha256 $WS

# (WS, C, CS, N as recorded / named in v51_train.sh: WS = sha256 of $W/composition_weights.json, C = $WT-$i/train_combined.json)
# 4. nav, the declared cell (the V6-C best construction when stacked; here the v5 reference cell)
"$PY" $BR --output $N-run --bind $NAV --bind $C --bind $FD/manifest.json -- $NAV nav --combined $C --combined-sha256 $CS \
  --role $R2 --role-sha256 $R2S --fields $FD/manifest.json --fields-sha256 $FS --output $N --rule aim-partial-v5 \
  --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1 --daily-turnover-mean-max .20 \
  --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824
"$PY" .superpowers/sdd/mega-alpha-20260926/studies/nav_summ.py --weights $W/composition_weights.json \
  --reference build-equity/mega-nav-v5-ew-t.05-d.1-fixed $N
```

Expected cache behaviour: 36 of 38 DSL strings are new (all but `ind_mom_12_1` and `within_ind_mom`). The u pass
therefore recomputes almost everything and will likely use all 3 bounded 180 s passes; v4's first u pass is the
reference timing.

Trial accounting (prereg V6-L): admission 38 (= roster), composition 1, construction cell 1.

## 7. Reading hygiene (disclosed)

- **Read:** the brief's reading list: prereg v6 section, v6-literature §2-4, v6-code-review-signal (I2/I3/I5),
  scorecard §5-6, generators v4/v4.2/v5, the fields-v6 manifest field list, and the DSL/VM sources.
- **Scorecard §5:** it prints per-candidate TRAIN HAC t, so those numbers were seen. They were not used. Every choice
  is traceable to the literature, the brief, VM semantics or field metadata.
- **`admission.json`:** only `id` and `tau` were extracted, via a script that printed those two columns.
- **v4.2:** `res_mom_12_1`'s v4.2 TRAIN results were not read.
- **Not read:** no 2023+ path or file.

## 8. Concerns

1. **Plan-budget truncations.** The brief's composite (4 ratios) and XFIN do not fit the <= 5 extras ruling (T27),
   so `ebit_ev` is out of the composite and XFIN is not added. The arithmetic (section 3) says capacity 6 fits the
   1536 MiB runner budget (about 1,431 MiB). If the controller raises the cap to 6, XFIN fits (DSL given in section
   3). The composite with ebit_ev would need 8.
2. **`cfoa` removed, not kept.** The brief says "add" CbOP. I replaced `cfoa` because it cites the same BGLN 2016
   hypothesis (one variant per hypothesis), and the two would likely be |rho| > .9. To revert, drop `'cfoa'`
   from `REPLACE` and set `INSERT_BEFORE['cfoa'] = 'cbop'`. `accruals` is kept (Sloan 1996 is a separate hypothesis), although
   BGLN find CbOP subsumes it.
3. **cbop's R&D zero-fill uses `power(x, 1 - n) - n` with `n = ts_count_nans(x, 1)`.** It relies on
   `std::pow(NaN, 0) == 1`, which C99 Annex F requires. `vm.hpp` Pow is a plain `std::pow` (also in the fused and
   oracle paths). I could not run the native VM. Two things would expose a UCRT deviation:
   - step 0 (`--plan-only`) confirms only that it compiles;
   - after the u pass, cbop coverage should match `cfp`/`accruals` coverage. If coverage falls to R&D reporters only,
     the zero-fill failed.
4. **Seasonality window re-centred** (224/245 -> 231/252) when its decay was removed. This goes beyond the literal
   "remove the decay". To revert, keep delay 224/245.
5. **BAC's volatility-quintile conditioning is not expressible.** Group ops need a Group-typed classifier, and
   `quantile()` returns F64. It is delegated to price-risk-v1's vol63 regressor. BAC is low beta at matched
   volatility, so its content after the beta252 + vol63 projection is second-order: the same structural issue as
   review I2. SMAX survives better (it is a ratio to volatility). This matters only if V6-W keeps the low_risk
   theme, which prereg V6-W (a) drops.
6. **SMAX uses MAX1, not MAX5** (no top-k op). The brief attributes scaled MAX to Novy-Marx-Medhat. The literature
   note and I cite Asness-Frazzini-Gormsen-Pedersen 2020 for SMAX (plus Bali-Cakici-Whitelaw 2011). As far as I
   know, Novy-Marx-Medhat 2025 is the defensive-equity/profitability context, not the source of the definition.
7. **res_mom_12_1 is residual to the market only**, fit in-window: the score is the standardized formation-window
   alpha, as in v4.2. It is not Fama-French-3 residuals estimated over the prior 36 months.
8. **Roster order.** Inserting `value_composite` before `bm` and `res_mom_12_1` before `mom_12_1` lets them win the
   redundancy pass against incumbents of equal tier: cfp is likely rejected if |rho| > .9. This is literature-based,
   but it is a choice.
9. **The blackout is not fixed for `ind_mom_12_1` and `within_ind_mom`**, because the brief forbids touching them.
   An unauthorised alternative that would fix it is `rank(group_neutralize(decay_linear(mom, 21), grp_ff49))` (the
   decay inside the group op).
10. **`value_composite` is NaN when any of its 3 ratios is NaN**, so its coverage is at most that of the
    intersection. There is no NaN-skipping mean without adding slots.
11. **Not verified natively:** the compiled slot count (the estimate is 7), and the typecheck of `power` /
    `ts_count_nans` with panel operands. Run step 0 before the u pass.
