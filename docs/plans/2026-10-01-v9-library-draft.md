# v9 prior-class library wave (lane LIB3, wave AG): candidates, definitions, static checks, priors

Declared 2026-10-01 by lane LIB3 (pool 11, branch `feat/platform-v8-lib3-20261001`, from `67f04389`), before any IC,
TRAIN, return or NAV read of any candidate. Registration text for **v9** only: the v8 trial program is untouched (N <= 51,
prereg rule 10, no new v8 cell). Nothing was built or run; no data payload was opened. Root compile-checks every string
with K1 (`--plan-only`) before any `add-alpha`.

## 0. Conventions (binding for every candidate)

- **Sign.** `prior_sign` +1 for every candidate; the literature sign is embedded in the DSL (`-1 *` for long-low), as in
  v6-v8.2. Forms: slow additions `R(decay_linear(x, 21))`; event flags, aggregates and event-window signals `R(x)`.
  `R` = `rank`, or `group_rank(., grp_ff12)` for profitability (house rule R1). One variant per hypothesis.
- **Windows** from {5, 21, 63, 126, 252}; a paper window only where the registration names it.
- **Themes.** Only existing themes: `PRIOR_THEMES` (`fit_composition_weights.py:254-262`: value, profitability_quality,
  investment_issuance, earnings_momentum, price_momentum, low_risk, short_interest, reversal_seasonality,
  options_implied, ownership_flow) plus `filing_events` (v8.1, E7 / E-42; appended last, PM4-11). **No theme text is
  edited:** theme texts are written into every generated library (`generate_library.py:364`, `families[].description`;
  e.g. `fund_industry_ic_v71.json`), so a text edit moves the bytes of v7.1 and v8.x libraries. Members join an existing
  theme under its current text, as every v8.0 member did (library-v8-draft section 6). A new theme is a ruling (LIB3-a).
- **Evidence rule (stricter than v8).** A candidate's sign and definition come from the paper. "Not dead" evidence must
  come from a published or working-paper source whose sample ends before 2024-01-01. The house literature note's
  CZ-own statistics (`research_notes/.../new_alpha_families.md`, sample 2005-01..2024-12) are **not** used as evidence
  or to choose anything here (their window contains 2024). Sample periods I could not confirm are marked unverified.
- **Prior-exposure rule.** A hypothesis already measured on platform data is out, near-variants included: the 74
  hypotheses of `price_volume_ic96_v2`, `pv_fields_ic121_v3`, `slow_price_volume_24_v1` (DSL strings read, no result),
  the atx-db TRAIN IC screen of `characteristics/` (names read from `ALPHA_PANEL.md` section C; results not read), and
  every v4-v8.2 member or registered candidate.
- **Plan section 13 binds:** multi-lag same-month seasonality, Amihud, turnover (level), long-term reversal, price delay,
  XFIN, repurchase flags, more quality composites and intangible-adjusted book are out.
- **Withdrawal rule.** A candidate whose field is absent at its wave's freeze is withdrawn at 0 trials; a string K1
  refuses is rewritten only mechanically (same semantics), else withdrawn.
- **Static figures** (bars, slots, nodes, extra fields, bytes, SHA) come from a scratch offline mirror of the compiler
  (parse order; literal and subtree CSE; peeled hparams: `m` of `*_mp`, `n` of `bucket`; output slot taken before inputs
  are freed; lookback = delay d + child, rolling (w - 1) + child). Calibration: it reproduces every recorded figure of
  the 15 rows of library-v8-draft section 4 and task-LIB2 section 1 (bars, slots, nodes where stated, DSL SHA-256) and
  the recorded 8 slots of `qmj_safety` (v7.1 library maximum). The mirror is a scratch tool, not committed. K1 is the
  checker of record (R2-f); a K1 row that differs is reported, a refused string is withdrawn.

## 1. Summary

| # | id | theme (existing) | family new to the book? | tier | status | needs | bars | slots | nodes | extra fields | bytes | DSL sha256 (16) | turnover [est] |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| C-1 | `stmom` | price_momentum | yes: turnover-conditioned short-horizon continuation | B- | **READY** | close, volume, shares_out | 41 | 5 | 21 | 1 | 178 | `06dc6238d7e94089` | mid-high |
| C-2 | `ind_leadlag` | price_momentum | yes: cross-firm (big-to-small) diffusion | C+ | **READY** | close, me_company, grp_ff49 | 21 | 7 | 24 | 2 | 393 | `d2d9b4d24b474944` | high |
| C-3 | `nt_late` | filing_events | yes: late-filing notice | B- | **NEEDS-FIELD** | `nt_first_126` (sec_filings `events.parquet`) | 0 | 3 | 4 | 1 | 25 | `bafc4e3a93204e59` | event |
| C-4 | `earn_season` | reversal_seasonality | yes: seasonality of earnings levels | B- | **NEEDS-FIELD** | `earn_season_rank` (fundamentals stage), `ea_days_to_expected` | 0 | 5 | 11 | 2 | 99 | `64a0be8f2c71efd1` | mid |
| C-5 | `lazy_prices` | filing_events | yes: periodic-filing text | B- | **NEEDS-DATA** | `lp_sim_cos` (atx-db `text/`, landing 7%) | 0 | 2 | 2 | 1 | 16 | `8a175c874813e0c8` | very low |
| C-6 | `tnic_mom` | price_momentum | yes: product-market (text) peers | B- | **NEEDS-DATA** | `tnic_peer_ret21` (atx-db `classification_tnic/`, landing 7%) | 0 | 2 | 2 | 1 | 21 | `0be5696997a181a7` | high |
| C-7 | `conn_rev` | reversal_seasonality | yes: common-fund-ownership pressure | C+ | **NEEDS-DATA** | `conn_ret63` (atx-db `nport/`, not built) | 0 | 3 | 4 | 1 | 23 | `4a1cffe83a007531` | mid-high |
| C-8 | `fund_fit` | ownership_flow | yes: predictable fund-flow pressure | C+ | **NEEDS-DATA** | `fit_exp_q` (atx-db `nport/`, not built) | 20 | 3 | 4 | 1 | 33 | `46a02ba90aad79b1` | low |
| C-9 | `tax_book` | profitability_quality | yes: tax accounts (level) | C+ | **NEEDS-DATA** | `txc_ttm` (atx-db `fundamentals_notes/`, not built) | 20 | 6 | 14 | 3 | 105 | `1b76612cee2479bb` | very low |
| C-10 | `iv_skew` | options_implied | yes: shape of the IV surface | C+ | **NEEDS-DATA** | `iv_put_otm_21d` (no source column registered; v7 D8) | 20 | 4 | 8 | 2 | 60 | `1dc4382ae845968f` | low-mid |

All rows pass the house budget (bars <= 314, runner <= 336; slots <= 7; extra fields <= 5; DSL <= 4,096 B).
`ind_leadlag` sits at the slot limit (7): ruling LIB3-g. Counts: READY 2, NEEDS-FIELD 2, NEEDS-DATA 6.

Finding behind the low READY count: the price-and-volume space is mostly already measured on platform data (74
hypotheses in three pv libraries plus the atx-db characteristics screen, which includes Gervais-Kaniel-Mingelgrin
`abn_turnover`, `rev_21`, `turnover_21`, the IV term and change measures). New families now come mainly from data the
book does not read yet: filing text, text peers, fund holdings, tax notes, the option surface.

## 2. Candidates

### C-1 `stmom`: short-term momentum among heavily traded stocks (READY)

- **Registration.** price_momentum; tier B-; prior sign +1; origin `prior`. Citation: Medhat, M. and M. Schmeling
  (2022), "Short-term Momentum", Review of Financial Studies 35(3), 1480-1526. Sample: US common non-financial
  NYSE/AMEX/Nasdaq shares, July 1963 - December 2018; 22 developed markets.
- **Paper (read from the authors' accepted manuscript).** Conditional decile sorts with NYSE breakpoints, first on last
  month's return r(1,0), then on last month's share turnover TO(1,0) within each return decile; value-weighted, monthly.
  Within the highest-turnover decile winners minus losers earn +1.37% a month (t 4.74): short-term momentum (STMOM);
  within the lowest-turnover decile -1.41% (t -7.13): reversal. STMOM persists for 12 months, survives conservative
  transaction-cost estimates (half-spread method) and is strongest among the largest, most liquid, most covered stocks.
  **Sign:** long heavily traded last-month winners, short heavily traded last-month losers.
- **Post-2004 evidence.** Extended CRSP subsamples: 1991/07-2018/12 earns 2.03% a month (t 4.80), the strongest of the
  three eras; returns "consistently positive" apart from 1975-79. Positive in all 22 international markets. No
  post-publication (2022-2023) test found. Not a section 13 family: turnover enters as a conditioning variable, not as a
  level signal (the dead "turnover" family is the level).
- **Final DSL:**
```
rank(decay_linear(((group_rank((ts_sum(volume, 21) / shares_out), bucket(((close / delay(close, 21)) - 1), 10)) > 0.9) ? (rank(((close / delay(close, 21)) - 1)) - 0.5) : 0), 21))
```
- **How it reads.** r = 21-session return; `bucket(r, 10)` (lit op, Group classifier, average-rank deciles) is the first
  sort; `group_rank(turnover, bucket)` ranks 21-session turnover within each return decile (the conditional second
  sort); names above 0.9 (top tenth of their return decile) take their centred cross-sectional return rank, every other
  name 0 (neutral); house 21-session decay; outer rank. Compare F64 -> Mask, Select(Mask, F64, F64) as `day_rev_freq`.
  NaN wherever r or turnover is NaN (full-window rule on `ts_sum`, `delay`, `decay_linear`).
- **Deviations.** (1) Breakpoints over the role's names, not NYSE. (2) Continuous centred return rank inside the
  high-turnover set instead of the decile-10 minus decile-1 corner. (3) Rolling 21 sessions daily with the house decay
  (paper: calendar month, monthly rebalance, one-month hold). (4) Turnover = raw 21-session volume / `shares_out` (A8
  90-day lagged, restated to the session basis): a split inside the window distorts it, as `dtc_slow` (R2-d). (5) Equal
  weight in the rank, not value weight.
- **Expected overlap by construction [est].** `ind_adj_rev_5` -.15 to -.35 (opposite sign on heavily traded names,
  overlapping windows); `mom_12_1` .05-.20 (12-1 skips the month STMOM uses; the paper reports moderate correlations
  with price and earnings momentum that do not span it); `day_rev_freq` < .10; `ear_mom_12m` .05-.15. Non-zero on about
  a tenth of names per session plus the decay tail [arithmetic].
- **Breadth.** No member conditions on trading activity; the book holds only short-term reversal at this horizon.
- **Prior exposure.** Not among the 74 pv-library hypotheses. atx-db screened `rev_21` and `turnover_21` as standalone
  characteristics (results not read); their conditional interaction was never screened (ruling LIB3-d).
- **House precedent.** library-v7-draft section 4 excluded "short-term reversal variants (turnover-conditioned,
  residual)" on cost grounds (Novy-Marx-Velikov 2016). STMOM is the high-turnover continuation counterpart, and the paper
  reports it survives costs (ruling LIB3-c).

### C-2 `ind_leadlag`: big-firm industry return leads small firms (READY)

- **Registration.** price_momentum; tier C+; prior sign +1; origin `prior`. Citation: Hou, K. (2007), "Industry
  Information Diffusion and the Lead-lag Effect in Stock Returns", Review of Financial Studies 20(4), 1113-1138.
  Chen-Zimmermann SignalDoc `IndRetBig`: sample 1972-2001, t 11.0.
- **Paper / replication definition.** Slow diffusion of industry information is a leading cause of the lead-lag effect;
  it is predominantly intra-industry, stronger in small, less competitive and neglected industries, and driven by slow
  reaction to negative news (abstract). SignalDoc: "Average monthly return of the 30% largest companies by market value
  of equity in the same Fama-French 48 industry. Exclude the largest 30% of companies". **Sign:** long small firms whose
  industry's big firms rose.
- **Post-2004 evidence: indirect only.** Parsons, Sabbatucci and Titman (2020, RFS 33(10), 4721-4770) report geographic
  lead-lag returns of 5-6% a year, "half that observed for industry lead-lag effects", and that industry lead-lag is
  strongest among small, thinly traded, low-coverage stocks (abstract; sample years unverified). Hoberg and Phillips
  (2018, JFQA) find SIC-based peers give "only small, short-lived momentum profits". No direct post-2004 test in large
  caps was found: hence C+, and root may withdraw it at 0 trials (LIB3-e).
- **Final DSL:**
```
rank(((group_mean((((close / delay(close, 21)) - 1) * (((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1)))), grp_ff49) / group_mean((((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1))), grp_ff49)) + (0 * log((1 - (((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1))))))))
```
- **How it reads.** B = 1 for the top 30% of `me_company` within the FF49 industry, else 0, NaN where the 21-session
  return r is NaN (`+ 0 * r` aligns the valid sets). `group_mean(r * B) / group_mean(B)` = the equal-weighted mean r of
  the industry's big firms, broadcast to every valid member (`cs_ops.hpp` CsMeanG: the aggregate is over the valid set,
  written to each valid member); 0 / 0 = NaN when the industry has no big firm. `0 * log(1 - B)` is NaN for big firms
  (log 0 = -inf), so only the small 70% are ranked, as Hou excludes the big firms from the traded set.
- **Deviations.** FF49 for FF48; equal-weighted big-firm mean; rolling 21 sessions (the replication's monthly
  horizon, slower than Hou's weekly tests); within-industry percentile 0.7 over the role's names; no decay (`R(x)`).
- **Expected overlap [est].** `ind_adj_rev_5` +.15 to +.35 (a small firm lagging a rising industry is an
  industry-relative loser), `ind_mom_12_1` .05-.20, `tnic_mom` .20-.40 (same family), `mom_12_1` < .10. Coverage about
  70% of names.
- **Breadth.** The book has no linked-firm signal: `ind_mom_12_1` is the firm's own industry 12-1 return.
- **Static.** 7 slots by the mirror: at the house limit (LIB3-g). A leaner spelling without the NaN alignment term
  changes the denominator when a big firm's r is NaN (not the same semantics), so it is not offered.

### C-3 `nt_late`: short after a first late-filing notice (NEEDS-FIELD)

- **Registration.** filing_events; tier B- (the v8 R7-6 registration, withdrawn at 0 trials for data); prior sign +1;
  origin `prior`. Citation: Bartov, E. and Y. Konchitchki (2017), "SEC Filings, Regulatory Deadlines, and Capital
  Market Consequences", Accounting Horizons 31(4) (December issue per the authors' school release; pages unverified);
  2,115 firms over a nine-year period (years unverified).
- **Paper.** Prices drop as soon as a firm files Form NT, even when management says it will meet the extended deadline
  (about 2% over five days for NT 10-Q filers meeting it, over 4% for those missing it, NYU Stern release); 36% of the
  87% who promise to file within the grace period fail; "the price decline can continue for months, even if the firms
  meet the extended filing deadline". **Sign:** short after a first NT filing.
- **Post-2004 evidence.** The nine-year sample is in the SOX era (years unverified); no post-publication test found.
- **Data now exists.** v8 withdrew this because no producer read NT forms. The `sec_filings` stage the K8 fields already
  pin carries `events.parquet` with NT 10-K, NT 10-Q and NT 20-F rows and their EDGAR acceptance clock
  (`ALPHA_PANEL_SEC.md`, stage `sec_filings`): a new reader in the SEC module, no atx-db change.
- **Field `nt_first_126`** (spec section 3, F-L1). **DSL** (v8 F-E semantics):
```
rank((-1 * nt_first_126))
```
- **Deviations.** 126 sessions (the registered v8 semantics) rather than the paper's months; NT 10-K and NT 10-Q pooled;
  continuous rank of a flag (flagged names tie at the bottom, as `nonreliance_402`).
- **Expected overlap [est].** < .10 with every member; `ea_overdue` .05-.15 (a late 10-Q often comes with a late
  earnings release); `nonreliance_402` .05-.15 (restatements cause NT filings). Flagged share well under 1% of names.
- **Breadth.** A filing event no member uses.

### C-4 `earn_season`: earnings seasonality in the expected announcement window (NEEDS-FIELD)

- **Registration.** reversal_seasonality (a recurring calendar pattern; LIB2's `day_rev_freq` precedent for joining
  this theme without a text edit); tier B-; prior sign +1; origin `prior`. Citation: Chang, T., S. Hartzmark, D. Solomon
  and E. Soltes (2017), "Being Surprised by the Unsurprising: Earnings Seasonality and Stock Returns", Review of
  Financial Studies 30(1), 281-323. Compustat quarterly and CRSP NYSE/AMEX/Nasdaq common stock, price >= $5; sample
  years unverified (the 2014 NBER-conference draft I read does not state them in the extracted text).
- **Paper (definition read from the 2014 draft).** For quarter t, rank the 20 quarterly EPS (excluding extraordinary
  items, split-adjusted) of quarters t-23..t-4, all 20 required; EarnRank = mean rank of quarters t-4, t-8, t-12, t-16,
  t-20 (the same fiscal quarter). High EarnRank = this quarter is historically large. Firms with an expected
  announcement in the top EarnRank quintile earn a four-factor alpha of 65 bp a month against 31 bp for the bottom (EW
  long-short 34.7 bp, t 3.13); value-weighted long-short 55 bp (t 3.14); no effect outside predicted announcement months;
  analysts' forecast errors are more positive in high-seasonality quarters. **Sign:** long high EarnRank.
- **Post-2004 evidence.** In-sample years unverified (the paper's Borders example runs 1995-2010). No post-publication
  test found. Risk [inference]: the level of the US earnings-announcement premium has disappeared after 2004
  (Heitz-Narayanamoorthy-Zekhnini 2020, working paper); that is the base return of announcers, not the seasonality
  spread this candidate trades.
- **Field `earn_season_rank`** (F-L2). **DSL:**
```
rank((((ea_days_to_expected >= 0) && (ea_days_to_expected <= 21)) ? (earn_season_rank - 10.5) : 0))
```
- **How it reads.** While the expected announcement (`ea_days_to_expected`, v9 field, registry row exists) is 0..21
  sessions ahead, the centred EarnRank (neutral mean 10.5); 0 elsewhere; NaN where the calendar or the rank is NaN. The
  window ends when the announcement becomes usable (the field then points at the next quarter).
- **Deviations.** Net income `ni_q`, not split-adjusted EPS (the paper's footnote 3: results robust to alternative
  earnings measures; avoids split restatement across 20 quarters); the house expected-date rule (yoy_364) and a 0..21
  session window instead of the predicted calendar month; no decay.
- **Expected overlap [est].** `ear_mom_12m`, `earn_surprise_comp`, `ea_overdue` < .10 (different ordering; overdue
  names are 0 here); `seasonality_same_month` .05-.15.
- **Breadth.** The calendar of earnings levels; members use changes and surprises only.

### C-5 `lazy_prices`: long non-changers of 10-K language (NEEDS-DATA)

- **Registration.** filing_events; tier B-; prior sign +1; origin `prior`. Citation: Cohen, L., C. Malloy and Q. Nguyen
  (2020), "Lazy Prices", Journal of Finance 75(3), 1371-1415. Sample: quarterly and annual filings of US firms
  1995-2014.
- **Paper.** Changes in the language and construction of reports signal future operations; short "changers", long
  "non-changers" earns up to 188 bp a month in alpha (abstract); changes about the executive team, litigation and risk
  factors are the most informative; returns arrive when the news emerges later. **Sign:** long high similarity.
- **Post-2004 evidence.** 2005-2014 is inside the sample. Post-publication: not verified (a web summary of a
  replication on uncertainty-language similarity reports the same ordering; unverified).
- **Field `lp_sim_cos`** (D-L1): built from atx-db stage `text/`. **DSL:** `rank(lp_sim_cos)`.
- **Deviations.** 10-K family only (the paper uses 10-K and 10-Q; atx-db lands no 10-Q); three sections (business, risk,
  MD&A) not the whole document; cosine only (the paper also reports Jaccard, MinEdit, Simple); landing starts with 2018
  filings, so the first comparison is in 2019; plain rank.
- **Expected overlap [est].** < .15 with every member; `fscore`, `accruals` .05-.15.
- **Breadth.** Periodic-filing text: a source no member reads.
- **Data status.** Landing stopped at 7% of scope; atx-db must finish it and rebuild (`ALPHA_PANEL_TEXT.md`).

### C-6 `tnic_mom`: text-based industry (TNIC) peer momentum (NEEDS-DATA)

- **Registration.** price_momentum; tier B-; prior sign +1; origin `prior`. Citation: Hoberg, G. and G. Phillips (2018),
  "Text-Based Industry Momentum", JFQA 53(6), 2355-2388. Sample years unverified (TNIC from 10-K product text).
- **Paper.** "Shocks to less visible peers generate economically large momentum profits and are stronger than own-firm
  momentum variables. More visible traditional SIC-based peers generate only small, short-lived momentum profits"
  (abstract); the focus is peer links that do not share SIC codes. **Sign:** long firms whose text peers rose.
- **Post-2004 evidence.** In-sample (10-K text from the mid-1990s; years unverified). No post-publication test found.
- **Field `tnic_peer_ret21`** (D-L2). **DSL:** `rank(tnic_peer_ret21)`.
- **Deviations.** Equal weights over peers and the non-SIC-3 split are the house reading of "less visible peers"; root
  copies the paper's weights and visibility split before the build (unverified here); 21-session window; no decay.
- **Expected overlap [est].** `ind_leadlag` .20-.40, `ind_mom_12_1` .10-.30, `mom_12_1` .05-.20.
- **Breadth.** Product-market links from text; the book's peer information is FF49 industry means only.

### C-7 `conn_rev`: cross-stock reversal through common fund ownership (NEEDS-DATA)

- **Registration.** reversal_seasonality; tier C+; prior sign +1; origin `prior`. Citation: Anton, M. and C. Polk
  (2014), "Connected Stocks", Journal of Finance 69(3), 1099-1127. Sample 1980-2008; stocks above the NYSE median
  market capitalisation; active mutual funds (index, tax-managed and international funds screened out).
- **Paper (read from the authors' copy).** FCAP_ij = sum over common funds f of (S_fi P_i + S_fj P_j) / (S_i P_i +
  S_j P_j) at each quarter end; a stock's connected return is the return on the portfolio of stocks abnormally connected
  to it, over the past three months. The CS strategy (buy low own and low connected return, sell high/high) earns a
  five-factor alpha of 76 bp a month (t 4.96), over 71% from the long side; the version ignoring the own return (low
  minus high connected return) earns 36 bp a month (t 4.13), distinct from short-term reversal. **Sign:** long low
  connected return.
- **Post-2004 evidence.** 2005-2008 inside the sample only; no post-publication test found: C+.
- **House precedent.** library-v7-draft section 4 excluded "connected-stock reversal" on cost grounds (LIB3-c).
- **Field `conn_ret63`** (D-L3). **DSL:** `rank((-1 * conn_ret63))`.
- **Deviations.** N-PORT fund series (2019q4 on) instead of CRSP mutual-fund holdings; the paper's "abnormal
  connection" rule and active-fund screen copied before the build (LIB3-h); all role names, not only above-median.
- **Expected overlap [est].** `ind_adj_rev_5` .10-.30; `fund_fit` -.10 to -.30 (past versus expected flow pressure).
- **Breadth.** Ownership-network price pressure; no member uses holdings links between stocks.

### C-8 `fund_fit`: expected flow-induced trading by mutual funds (NEEDS-DATA)

- **Registration.** ownership_flow; tier C+; prior sign +1; origin `prior`. Citation: Lou, D. (2012), "A Flow-Based
  Explanation for Return Predictability", Review of Financial Studies 25(12), 3457-3489. Sample unverified.
- **Paper.** Aggregating flow-induced trading across mutual funds gives demand shocks with a significant, temporary price
  impact; because flows are predictable, "the expected part of flow-induced trading positively forecasts stock and
  mutual fund returns in the following year, which are then reversed in subsequent years" (abstract). **Sign:** long
  high expected flow-induced buying (one-year horizon).
- **Post-2004 evidence.** Not verified: C+.
- **Field `fit_exp_q`** (D-L4). **DSL:** `rank(decay_linear(fit_exp_q, 21))`.
- **Deviations.** N-PORT (2019q4 on, one public report per fund quarter) instead of CRSP / Thomson holdings; the flow
  forecast copied from Lou (2012) before the build, the declared fallback is the series' latest reported quarterly flow
  rate (LIB3-h).
- **Theme fit.** The `ownership_flow` text speaks of informed owners; this is uninformed flow pressure (LIB3-b).
- **Expected overlap [est].** `mom_12_1` .20-.40 (Lou: flows partly explain momentum); `inst_best_ideas` .10-.25.
- **Breadth.** Fund-flow pressure; `ownership_flow` holds two members (insiders, 13F conviction).

### C-9 `tax_book`: tax-to-book income (NEEDS-DATA)

- **Registration.** profitability_quality; tier C+ (v8 R7-3 registered B; lowered here on the evidence below); prior
  sign +1; origin `prior`. Citation: Lev, B. and D. Nissim (2004), "Taxable Income, Future Earnings, and Equity Values",
  The Accounting Review 79(4), 1039-1074. Sample 1973-2000 (SignalDoc, t 3.9).
- **Paper / replication.** Current (federal + foreign) income tax, else total minus deferred, over statutory rate times
  net income; CZ: set to 1 when net income < 0 and tax > 0; long high. The ratio predicts five-year earnings changes
  before and after SFAS 109 (1993); its link to subsequent returns is reported for the pre-SFAS 109 period; SignalDoc:
  "it only works in the subsample 1973-1992 ... 1993-2000 works as long as you drop 1998".
- **Post-2004 evidence.** None published found. The house plan (section 5, D14) records "keeps alpha after 2004
  (researcher's own computation)"; that computation's sample runs to 2024-12 and is not used here. Hence C+; root may
  withdraw before any read (LIB3-e).
- **Field `txc_ttm`** (D-L5). **DSL:**
```
group_rank(decay_linear(((ni_ttm > 0) ? (txc_ttm / ni_ttm) : (0.21 + (0 * log(txc_ttm)))), 21), grp_ff12)
```
- **How it reads.** Positive income: current tax over income (the statutory 21% is constant over TRAIN, TCJA 2018, so it
  cancels in the rank); non-positive income with positive current tax: 0.21 (the "set to 1" rule in unscaled units);
  non-positive income without positive tax: NaN (`0 * log` guard); FF12 group rank (R1).
- **Deviations.** Net income, not income before extraordinary items; TTM on the filing clock, not annual.
- **Expected overlap [est].** `ep` .20-.35, `roa` / `op_rd` .20-.40, `chtax` .10-.20, `accruals` .10-.25.
- **Breadth.** Tax accounts as a level (book-tax conformity); `chtax` uses tax only as a change.

### C-10 `iv_skew`: implied volatility smirk (NEEDS-DATA)

- **Registration.** options_implied; tier C+; prior sign +1; origin `prior`. Citation: Xing, Y., X. Zhang and R. Zhao
  (2010), "What Does the Individual Option Volatility Smirk Tell Us About Future Equity Returns?", JFQA 45(3), 641-662.
  Sample: all firms with listed options, 1996-2005.
- **Paper.** SKEW = IV of an OTM put minus IV of an ATM call; the steepest-smirk stocks underperform the flattest by
  10.9% a year (FF3-adjusted), for at least six months; steep smirks precede bad earnings news. **Sign:** long low skew.
- **Post-2004 evidence and its limit.** Fu, Arisoy, Shackleton and Umutlu (2016), "Option-Implied Volatility Measures
  and Stock Return Predictability" (journal and sample unverified): skew keeps predictive power before and after 2008.
  Against: Muravyev, Pearson and Pollet (2025, JFE 172), "Why Does Options Market Information Predict Stock Returns?":
  predictability from IV spread and skew falls by at least two-thirds when high-borrow-fee stocks are excluded
  (abstract; sample unverified). The signal is largely a borrow-fee proxy, overlapping `short_interest` and partly not
  capturable net of fees: C+.
- **Field `iv_put_otm_21d`** (D-L6). **DSL:** `rank(decay_linear((-1 * (iv_put_otm_21d - iv_atm_21d)), 21))`.
- **Deviations.** The vendor's ATM-centre clean IV (`atmCenI_21d`) as the ATM leg, not the ATM call; 21-session constant
  maturity; house decay.
- **Expected overlap [est].** `iv_rv_spread` .15-.30; `si_ratio` / `dtc` / `si_low_io` .10-.25; `iv_vol_of_vol` .10-.25.
- **Breadth.** The shape of the IV surface; members use the ATM level and its volatility only.

## 3. Fields to build (NEEDS-FIELD) and data asks (NEEDS-DATA)

Every field: point in time; no row with `available_at` at or after `research_window.SEAL` (counted); primary lines
(LINK_RULE); opt-in module field (absent from every existing recipe, so all existing payloads stay byte-identical under
`--reuse`); a look-ahead probe (rows after the t-1 mark mutated: rows 0..t bit-identical), as task-LIB2 section 2.

| id | for | stage (source) | rule and point-in-time clock | formula id (suggested) |
|---|---|---|---|---|
| F-L1 `nt_first_126` | C-3 | atx-db `sec_filings/events.parquet` (pinned already by the K8 fields) | original NT 10-K / NT 10-Q rows (no amendments); event usable from the first session whose 22:00 UTC mark follows `available_at` (K8 lag rule); "first" = no NT form of the CIK visible in the prior 365 days; value 1 for e <= t < e + 126, else 0; NaN unless the CIK is a domestic periodic filer (a visible original 10-K or 10-Q within 400 days; NT 20-F filers NaN); never revised | `sec-nt-first365-126-v1` |
| F-L2 `earn_season_rank` | C-4 | the fundamentals stage read for `ni_q` and `eps_consist_4y` (F-D path, `research_fields_v8.py`) | A = latest fiscal quarter with visible `ni_q` (T21 clock); U = A + 1; `ni_q` of quarters U-23..U-4 as known at t, matched within +-20 days, ties to the later (Ruling E-30), all 20 finite; ascending average ranks 1..20; value = mean rank of U-4, U-8, U-12, U-16, U-20; domain [3, 18]; NaN otherwise. Defined from about 2019Q4 (events from 2014) | `chss-earnrank-ni20q-v1` |
| D-L1 `lp_sim_cos` | C-5 | atx-db `text/features.parquet` (landing at 7%: ask atx-db to finish the 10-K landing for TRAIN filers and rebuild) | latest visible 10-K row (as-of on CIK by `available_at` < 22:00 UTC of t-1, stale after 400 days, atx-db consumer rule) with `prior_gap_days` in [300, 430]; mean of the finite `business_`, `risk_`, `mdna_sim_cosine` (at least 2 of 3) | `cmn-lazy-10k-cos3-v1` |
| D-L2 `tnic_peer_ret21` | C-6 | atx-db `classification_tnic/pairs` (same landing) + role close | TNIC-3 pairs only (fallback excluded), each set used from its `available_at` (< 22:00 UTC of t-1) until the next year's set, at most 550 days (atx-db consumer rule); peers outside the firm's latest SIC-3; equal-weighted mean of peers' 21-session returns ending t-1 (`price-close-lag1` clock); at least 3 peers; weights and visibility split copied from Hoberg-Phillips before the build | `hp-tnic3-nonsic-peerret21-v1` |
| D-L3 `conn_ret63` | C-7 | atx-db `nport/` holdings (build not run) + role close | per calendar quarter: FCAP over common active fund series (index-fund screen declared before the build); connected set by Anton-Polk's abnormal-connection rule (copied before the build); FCAP-weighted mean 63-session return of connected stocks ending t-1; holdings usable from their acceptance (`nport-acceptance-v1`), stale after 180 days | `ap-connected-ret63-nport-v1` |
| D-L4 `fit_exp_q` | C-8 | atx-db `nport/` holdings and `fund_flows` (build not run) | sum over series of latest visible shares held x expected next-quarter flow rate, over `shares_out`; flow forecast per Lou (2012), fallback the latest reported quarterly flow rate; same clock and staleness as D-L3 (atx-db already computes realized `flow_induced_shares`) | `lou-expected-fit-nport-v1` |
| D-L5 `txc_ttm` | C-9 | atx-db `fundamentals_notes/notes_wide.parquet` (code done, not built) | `tax_current` (else `tax_total` - `tax_deferred`), year-to-date flows to TTM; clock = SUB acceptance (else filed + 46 h); an amendment is its own row, latest visible wins; stale after 400 days; USD after the fundamentals engine's FX rule (D4), else NaN | `fsn-tax-current-ttm-v1` |
| D-L6 `iv_put_otm_21d` | C-10 | none registered: the vendor file behind `iv_atm_*` exposes `atmCenI_*`, `atmCenH_*`, `nEarnCnt_*`, `earnFlag` (`prepare_research_fields.py` TH specs); ask atx-db / the vendor for a put-wing IV | 21-session constant-maturity OTM-put IV (XZZ: K/S in [0.80, 0.95], nearest 0.95; or a 25-delta put); TH clock (same-date vendor row), domain [0.02, 5] | `th-iv-put-otm-21d-v1` |

READY candidates need no new field and no registry field row (`shares_out`, `me_company`, `grp_ff49`, `ni_ttm`,
`grp_ff12`, `iv_atm_21d`, `ea_days_to_expected` are registry fields).

## 4. Registry rows and `add-alpha` lines (strings frozen; root applies in v9)

The registry has no off-by-default draft entries (`add-alpha` registers directly), so, as LIB2 did, the registration
lives here and in `task-LIB3-report.md`; no registry or default list is touched. Substitutions allowed: `<v9 parent>`,
`<v9 name>`, `<parent spec>`, `<fields dir>`; every other byte is frozen. `PY="C:/Program Files/Python312/python.exe"`.

```bash
"$PY" scripts/research_cycle.py add-alpha --id stmom --dsl "rank(decay_linear(((group_rank((ts_sum(volume, 21) / shares_out), bucket(((close / delay(close, 21)) - 1), 10)) > 0.9) ? (rank(((close / delay(close, 21)) - 1)) - 0.5) : 0), 21))" --theme price_momentum --tier B- --prior-sign 1 --citation "Medhat and Schmeling (2022, RFS 35(3)) Short-term Momentum" --origin prior --prior-sign-source "Medhat-Schmeling 2022" --form "R(decay_linear(x, 21))" --formula "centred rank of the 21-session return for names in the top tenth of 21-session turnover within their 21-session-return decile (bucket), 0 elsewhere; long heavily traded winners, short heavily traded losers" --domain "NaN where the 21-session return or turnover is NaN; non-selected names are 0 (neutral)" --deviation "role breakpoints, not NYSE; continuous rank inside the high-turnover set, not the decile corner; rolling 21 sessions with house decay, not calendar months; turnover = raw volume / 90-day-lagged shares_out (split inside the window distorts); equal-weighted rank" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id ind_leadlag --dsl "rank(((group_mean((((close / delay(close, 21)) - 1) * (((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1)))), grp_ff49) / group_mean((((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1))), grp_ff49)) + (0 * log((1 - (((group_rank(me_company, grp_ff49) > 0.7) ? 1 : 0) + (0 * ((close / delay(close, 21)) - 1))))))))" --theme price_momentum --tier C+ --prior-sign 1 --citation "Hou (2007, RFS 20(4)) Industry information diffusion and the lead-lag effect in stock returns; Chen-Zimmermann IndRetBig" --origin prior --prior-sign-source "Hou 2007" --form "R(x)" --formula "equal-weighted 21-session return of the top 30% of FF49 industry members by me_company, given to the other 70%; long small firms whose industry's big firms rose" --domain "NaN for big firms (traded set excludes them), where the 21-session return is NaN, and in industries without a big firm (0/0)" --deviation "FF49 for FF48; equal-weighted big-firm mean; rolling 21 sessions; within-industry percentile 0.7 over role names; no decay" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id nt_late --dsl "rank((-1 * nt_first_126))" --theme filing_events --tier B- --prior-sign 1 --citation "Bartov and Konchitchki (2017, Accounting Horizons 31(4)) SEC filings, regulatory deadlines, and capital market consequences" --origin prior --prior-sign-source "Bartov-Konchitchki 2017" --form "R(x)" --formula "-1{a first NT 10-K or NT 10-Q (none in the prior 365 days) became usable within the last 126 sessions} (nt_first_126)" --domain "NaN for non-domestic or absent periodic filers; a binary flag: flagged names tie at the bottom" --deviation "126 sessions, not the paper's months; NT 10-K and NT 10-Q pooled; continuous rank of a flag" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id earn_season --dsl "rank((((ea_days_to_expected >= 0) && (ea_days_to_expected <= 21)) ? (earn_season_rank - 10.5) : 0))" --theme reversal_seasonality --tier B- --prior-sign 1 --citation "Chang, Hartzmark, Solomon and Soltes (2017, RFS 30(1)) Being surprised by the unsurprising: earnings seasonality and stock returns" --origin prior --prior-sign-source "Chang-Hartzmark-Solomon-Soltes 2017" --form "R(x)" --formula "centred EarnRank (mean ascending rank of the same fiscal quarter among quarters U-23..U-4 of ni_q) while the expected announcement is 0..21 sessions ahead, else 0; long historically high quarters" --domain "NaN where the calendar or the 20-quarter history is NaN; 0 outside the window" --deviation "net income, not split-adjusted EPS; house expected date and a 0..21-session window, not the predicted calendar month; no decay" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id lazy_prices --dsl "rank(lp_sim_cos)" --theme filing_events --tier B- --prior-sign 1 --citation "Cohen, Malloy and Nguyen (2020, JF 75(3)) Lazy Prices" --origin prior --prior-sign-source "Cohen-Malloy-Nguyen 2020" --form "R(x)" --formula "mean cosine similarity of the business, risk and MD&A sections of the latest 10-K to the prior 10-K (lp_sim_cos); long non-changers" --domain "NaN without a visible annual comparison (prior gap 300-430 days) or with fewer than 2 sections; stale after 400 days" --deviation "10-K only (no 10-Q); three sections, not the whole document; cosine only; comparisons from 2019 filings" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id tnic_mom --dsl "rank(tnic_peer_ret21)" --theme price_momentum --tier B- --prior-sign 1 --citation "Hoberg and Phillips (2018, JFQA 53(6)) Text-based industry momentum" --origin prior --prior-sign-source "Hoberg-Phillips 2018" --form "R(x)" --formula "equal-weighted 21-session return of TNIC-3 peers outside the firm's SIC-3 (tnic_peer_ret21); long firms whose text peers rose" --domain "NaN with fewer than 3 peers with finite returns or no visible TNIC set (550-day staleness)" --deviation "equal weights and the non-SIC-3 split are the house reading of less visible peers; 21 sessions; no decay" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id conn_rev --dsl "rank((-1 * conn_ret63))" --theme reversal_seasonality --tier C+ --prior-sign 1 --citation "Anton and Polk (2014, JF 69(3)) Connected stocks" --origin prior --prior-sign-source "Anton-Polk 2014" --form "R(x)" --formula "-(FCAP-weighted 63-session return of stocks abnormally connected through common active fund ownership) (conn_ret63); long low connected return" --domain "NaN without a visible N-PORT quarter (180-day staleness) or without connected stocks" --deviation "N-PORT series instead of CRSP fund holdings; connection rule and active-fund screen as copied before the build; all role names" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id fund_fit --dsl "rank(decay_linear(fit_exp_q, 21))" --theme ownership_flow --tier C+ --prior-sign 1 --citation "Lou (2012, RFS 25(12)) A flow-based explanation for return predictability" --origin prior --prior-sign-source "Lou 2012" --form "R(decay_linear(x, 21))" --formula "expected flow-induced trading: shares held by each fund series x its expected next-quarter flow rate, summed, over shares_out (fit_exp_q); long expected fund buying" --domain "NaN without a visible N-PORT quarter (180-day staleness) or shares_out" --deviation "N-PORT from 2019q4; flow forecast per Lou, fallback latest reported flow rate; house decay" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id tax_book --dsl "group_rank(decay_linear(((ni_ttm > 0) ? (txc_ttm / ni_ttm) : (0.21 + (0 * log(txc_ttm)))), 21), grp_ff12)" --theme profitability_quality --tier C+ --prior-sign 1 --citation "Lev and Nissim (2004, TAR 79(4)) Taxable income, future earnings, and equity values" --origin prior --prior-sign-source "Lev-Nissim 2004" --form "R(decay_linear(x, 21))" --formula "current income tax TTM / net income TTM (statutory 21% cancels in the rank); 0.21 when income <= 0 and current tax > 0; long high" --domain "NaN when income <= 0 and current tax <= 0, or either is NaN" --deviation "net income, not before extraordinary items; TTM filing clock; FF12 group rank" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
"$PY" scripts/research_cycle.py add-alpha --id iv_skew --dsl "rank(decay_linear((-1 * (iv_put_otm_21d - iv_atm_21d)), 21))" --theme options_implied --tier C+ --prior-sign 1 --citation "Xing, Zhang and Zhao (2010, JFQA 45(3)) What does the individual option volatility smirk tell us about future equity returns?" --origin prior --prior-sign-source "Xing-Zhang-Zhao 2010" --form "R(decay_linear(x, 21))" --formula "-(21-session OTM put IV minus ATM centre IV); long flat smirks" --domain "NaN where either IV is NaN or outside [0.02, 5]" --deviation "vendor ATM-centre clean IV as the ATM leg, not the ATM call; constant maturity; house decay" --parent <v9 parent> --name <v9 name> --parent-spec <parent spec> --fields <fields dir>
```

Field rows (registry `fields`, added with the field build that carries them): `nt_first_126`, `earn_season_rank`,
`lp_sim_cos`, `tnic_peer_ret21`, `conn_ret63`, `fit_exp_q`, `txc_ttm`, `iv_put_otm_21d`, each with `formula_id`,
`origin` (the fields version that first carries it), `producer`, `clock` and `basis` copied from its manifest row.

## 5. How root compile-checks (K1)

1. READY strings now: run the two `add-alpha` lines above in a scratch tree against fields v9 (or later). Step 3 of
   add-alpha runs `build-equity/bin/atx-equity-strategy-ic.exe --plan-only --library <lib> --library-sha256 <sha>
   --train build-equity/train-2020-2023-lo3/manifest.json --train-sha256 <sha> --train-fields
   build-equity/train-2020-2023-lo3-fields-v9 --train-fields-sha256 <sha>` (`generate_library.exe_plan` argv; metadata
   only) and validates the house budget. Expected K1 rows: `stmom` bars 41, slots 5, extra field `shares_out`;
   `ind_leadlag` bars 21, slots 7, extra fields `me_company`, `grp_ff49`.
2. The other eight: K1 refuses an unknown field, so each runs only after its field is in a fields manifest and the
   registry; strings are frozen here; expected rows in section 1.
3. A row that differs from section 1 is reported; a refused string is rewritten mechanically or withdrawn (section 0).

## 6. Rulings root needs (decision -- why -- cost if wrong)

- **LIB3-a (themes):** no theme text edit; members join the themes of section 1 under their current texts -- theme texts
  are in generated library bytes (v7.1 and v8.x identity) -- cost if wrong: some members sit under a text that does not
  name them. Alternative: a new theme `cross_firm_links` for `ind_leadlag` + `tnic_mom` (one more theme in T).
- **LIB3-b (ownership_flow):** `fund_fit` joins `ownership_flow`, whose text says "informed"; FIT is uninformed flow
  pressure -- the nearest existing theme -- cost if wrong: a member under a mechanism its theme text does not describe;
  alternative: withdraw it.
- **LIB3-c (cost precedent):** library-v7-draft section 4 excluded turnover-conditioned short-term variants and
  connected-stock reversal on cost grounds. `stmom` (the paper reports it survives costs and persists 12 months) and
  `conn_rev` are registered against that precedent; `ind_leadlag` is also a one-month signal -- the cost-aware
  construction (spo-v3) prices their trades -- cost if wrong: admission trials on members the costs then neutralise.
- **LIB3-d (prior exposure):** atx-db's TRAIN IC screen covered `rev_21` and `turnover_21` standalone (results not read
  by this lane); `stmom` is their conditional interaction, never screened -- a different hypothesis with a different
  sign rule -- cost if wrong: a small selection effect from components seen by atx-db.
- **LIB3-e (evidence gaps):** `ind_leadlag`, `tax_book`, `fund_fit` and `conn_rev` have no verified post-2004 test
  (section 2); kept at C+ so the tier carries the doubt -- root may withdraw any of them before any read at 0 trials --
  cost if wrong: up to 4 trials on weak priors.
- **LIB3-f (roster):** v8.2 worst case 60 + 10 = 70 > 64 (R7-a cap) -- the cap is mechanical -- cost if wrong: none for
  inference; the v9 wave either raises the cap or registers fewer members.
- **LIB3-g (slots):** `ind_leadlag` uses 7 slots by the mirror, the house limit; if K1 reports 8, a budget exception
  (the `qmj_safety` precedent) or withdrawal -- cost if wrong: one refused string.
- **LIB3-h (definitions to copy):** the Anton-Polk abnormal-connection rule and active-fund screen, the Lou flow
  forecast and the Hoberg-Phillips peer weights are copied from the papers before the field builds; the fallbacks in
  section 3 are declared now so nothing is chosen after a read -- cost if wrong: a field mis-specified against its paper.

## 7. Considered and not included

| family / paper | reason (source) |
|---|---|
| Earnings-announcement premium: Frazzini and Lamont (2007, NBER WP 13090); Barber, De George, Lehavy and Trueman (2013, JFE 108(1), 1991-2010); Savor and Wilson (2016, JF) | the US premium "has disappeared ... in recent years" (Heitz, Narayanamoorthy and Zekhnini 2020, working paper; journal status unverified); READY via `ea_window_pre5`, but dead |
| High-volume return premium: Gervais, Kaniel and Mingelgrin (2001, JF 56(3), NYSE 1963-1996); Kaniel, Ozoguz and Starks (2012, JFE, 41 countries) | atx-db screened `abn_turnover` (GKM) on TRAIN, and the pv libraries screened `volume_shock_5_63`, `volume_spike_21_126`, `dollar_volume_shock`: measured on our data |
| IV term structure, IV change, IV-RV ratio | screened (`pv_fields_ic121_v3`; atx-db `iv_term_slope`, `iv_change_21`, `iv_rv_ratio`) |
| Put-call IV spread (Cremers and Weinbaum 2010, JFQA); option-to-stock volume (Johnson and So 2012, JFE, 1996-2008) | IV spread largely a borrow-fee proxy (Muravyev, Pearson and Pollet 2025, JFE 172); O/S is a weekly signal and needs option volume (v7 D8: none); `iv_skew` is the one options candidate |
| Geographic lead-lag: Parsons, Sabbatucci and Titman (2020, RFS 33(10)) | needs point-in-time headquarters (issuer profile is a non-PIT snapshot; cover-page addresses not built); same family as C-2 / C-6; sample unverified |
| Shared analyst coverage: Ali and Hirshleifer (2020, JFE) | no analyst data in any atx-db source |
| Customer momentum (Cohen and Frazzini 2008, JF); complicated firms (Cohen and Lou 2012, JFE) | customer links absent; segments stage not built; same family as C-2 / C-6 |
| Mutual-fund fire sales (Coval and Stafford 2007, JFE); dumb money (Frazzini and Lamont 2008, JFE) | same flow family as C-7 / C-8; three-year flows exceed N-PORT history (2019q4 on) inside TRAIN |
| Pension underfunding: Franzoni and Marin (2006, JF 61(2), 1980-2002) | no post-2002 evidence found; SFAS 158 (2006) moved funded status onto the balance sheet [recalled; unverified] |
| Distress / failure probability (Campbell, Hilscher and Szilagyi 2008, JF), O-score | a quality / safety composite (section 13: more quality composites are spanned) |
| IPO or listing age (Ritter 1991, JF; Loughran and Ritter 1995, JF) | underperformance concentrated in small non-VC IPOs and similar to size / book-to-market matches (Brav and Gompers 1997, JF) [recalled; unverified] |
| Activist 13D filings (Brav, Jiang, Partnoy and Thomas 2008, JF) | the return is at the filing with no later drift reported [house notes could not attribute; unverified] |
| Intraday-return momentum (Barardehi, Bogousslavsky and Muravyev) | a decomposition of `mom_12_1` (a variant); needs the vendor open |
| Revenue surprise (Jegadeesh and Livnat 2006, JFE) | an earnings-momentum variant; atx-db screened `sale_growth_q` |
| Innovative efficiency (Hirshleifer, Hsu and Li 2013, JFE 107(3)) | patent data and assignee-to-CIK links absent |
| Retail order imbalance (Boehmer, Jones, Zhang and Zhang 2021, JF) | needs TAQ trades; weekly horizon |
| Section 13 families | out by rule |

## 8. Sources (web, read 2026-10-01)

- Medhat-Schmeling: https://openaccess.city.ac.uk/id/eprint/31278/ (accepted manuscript read for definitions,
  Table I, subsamples)
- Hou: https://ideas.repec.org/a/oup/rfinst/v20y2007i4p1113-1138.html ; SignalDoc (IndRetBig, Tax):
  https://github.com/OpenSourceAP/CrossSection/blob/master/SignalDoc.csv
- Parsons-Sabbatucci-Titman: https://ideas.repec.org/a/oup/rfinst/v33y2020i10p4721-4770..html
- Hoberg-Phillips: https://ideas.repec.org/a/cup/jfinqa/v53y2018i06p2355-2388_00.html
- Bartov-Konchitchki: https://www.stern.nyu.edu/experience-stern/faculty-research/late-financial-filings-come-at-a-cost
- Chang-Hartzmark-Solomon-Soltes: https://ideas.repec.org/a/oup/rfinst/v30y2017i1p281-323..html ; 2014 draft
  https://www2.nber.org/conferences/2014/BEf14/Chang_Hartzmark_Solomon_Soltes.pdf
- Cohen-Malloy-Nguyen: https://www.nber.org/papers/w25084
- Anton-Polk: https://personal.lse.ac.uk/polk/research/ConnectedStocks.pdf
- Lou: https://researchonline.lse.ac.uk/id/eprint/46328/ ; https://ideas.repec.org/a/oup/rfinst/v25y2012i12p3457-3489.html
- Lev-Nissim: SignalDoc (above); https://business.columbia.edu/sites/default/files-efs/imce-uploads/dnissim/publications/taxable%20income.pdf (summary only)
- Xing-Zhang-Zhao: https://ideas.repec.org/a/cup/jfinqa/v45y2010i03p641-662_00.html
- Muravyev-Pearson-Pollet: https://ideas.repec.org/a/eee/jfinec/v172y2025ics0304405x25001618.html
- Fu-Arisoy-Shackleton-Umutlu: https://gcris.yasar.edu.tr/handle/123456789/7730 (search summary only)
- Heitz-Narayanamoorthy-Zekhnini: https://www.iimb.ac.in/ARC2020/Papers/The_Disappearing_Earnings_Announcement_Premium.pdf (abstract via search)
- Barber et al.: https://lbsresearch.london.edu/id/eprint/391 ; Frazzini-Lamont: https://www.nber.org/papers/w13090
- Gervais-Kaniel-Mingelgrin: https://rodneywhitecenter.wharton.upenn.edu/wp-content/uploads/2014/04/9901.pdf
- Johnson-So: https://ideas.repec.org/a/eee/jfinec/v106y2012i2p262-286.html
- Franzoni-Marin: https://ideas.repec.org/a/bla/jfinan/v61y2006i2p921-956.html
- Hirshleifer-Hsu-Li: https://ideas.repec.org/a/eee/jfinec/v107y2013i3p632-654.html

## 9. Hygiene

- Read: lane rules; the AG brief (sections "What does not change", "Lane LIB3"); task-LIB2 report; plan sections 5, 9
  (R-2, R-7), 13; progress entries R2-a..h, R7-a..c, E-36, E-42 (and PM4-11 for the theme order); the registry (48
  alphas, 43 fields, 10 themes); `PRIOR_THEMES`; the DSL catalogue (`registry.cpp`) and its typing / NaN rules
  (`typecheck.cpp`, `cs_ops.hpp`, `lit_ops.hpp`, `ts_ops.hpp`); field lists v9 (63, `scripts/specs/v8/base-lo3.json`),
  v10 / v11 (task-F-3 report), v12 (task-LIB2); field specs in `research_fields_sec.py` and `prepare_research_fields.py`
  (definitions only); library-v8-draft; library-v7-draft sections 2 and 4; the DSL strings (no results) of the three pv
  libraries; the v8 literature note `new_alpha_families.md`; atx-db docs in `C:/atx/atx-db/docs` (read-only):
  `ALPHA_PANEL.md` section C (names), `ALPHA_PANEL_SEC.md` stage `sec_filings`, `ALPHA_PANEL_OWNERSHIP.md` N-PORT and
  13D sections and a status grep, `ALPHA_PANEL_NOTES.md` stage `fundamentals_notes/` and its build state,
  `ALPHA_PANEL_TEXT.md` lines 1-118 and a status grep.
- Not read: any payload, anything under `build-equity/`, any IC, card, u pass, fit or NAV; atx-db `ALPHA_PANEL.md`
  section "TRAIN-only IC screen" and `ALPHA_PANEL_METRICS.md`; the results section of `ALPHA_PANEL_TEXT.md`.
- **Disclosures (hidden-data rule).** (1) `new_alpha_families.md` prints CZ-own long-short statistics for 2005-01 to
  2024-12 (external CRSP portfolio returns, not platform data); seen; not used as evidence or to choose (section 0).
  (2) `ALPHA_PANEL_SEC.md` prints per-year Form 25 delisting-cause counts for 2024-2026 and a 2024-06-30 filer count;
  (3) the `ALPHA_PANEL_OWNERSHIP.md` status grep printed 13D parse counts for documents of 2024-12..2026-09. Both are
  filing-metadata counts, no return or signal statistic; used for nothing. (4) Two 2025 / 2020 sources (Muravyev et al.,
  Heitz et al.) were read as abstracts only; their sample ends are unverified; they are used only to lower or exclude,
  never to choose a candidate for a measured strength.
- No candidate was ranked by anything measured; order in section 1 is by status, then family.
