# Task XIMP report: blind repairs and refinements of the v8.0 roster (expansion X)

Lane XIMP, pool 15, branch `feat/platform-v8-ximp2-20261002` from `3c6ae225`. Research and authoring only: no C++ was
written or built, no data was run, nothing was deleted, no repository code changed (this report is the only file).
Declared 2026-10-02 before any X measurement (Ruling PM7-1). Every string below is frozen here; K1 (`--plan-only`
through `add-alpha`) is the checker of record (R2-f).

## Status

| item | content | count | commit |
|---|---|---|---|
| A | defect audit of the 52 v8.0 strings; repairs | 52 checked; 0 in the five named classes; 1 domain-guard defect (A-1) | `6f7bae47` |
| B | refinements (one frozen string per member) | 4 | `c622b4ab` |
| C | library-wide processing variants; rulings, verification, risks | 1 | the commit after `c622b4ab` |

## Hygiene (what was read, what was not)

- Read: `lane-rules.md`; `task-X-briefs.md` (rules, lane XIMP); status 6 sections 1-2; `v8-prereg.md`; `progress.md`
  "PM session 7"; `library-v8-draft.md` (all); `code-review-v8-signal.md` (all); `review-w1-*.md` (finding lists, area B);
  `task-LIB2-report.md` sections 1-7; the v9 draft on `834d5a05` (sections 0-1); sprint plan sections 5 and 13;
  `alphas/registry.json`; `fund_industry_ic_v80.json`; the DSL engine source (`registry.cpp`, `parser.hpp/.cpp`,
  `typecheck.cpp`, `dag.hpp/.cpp`, `bytecode.hpp/.cpp`, `vm.hpp`, `ts_ops.hpp`, `lit_ops.hpp`, `cs_ops.hpp`); field
  producers (`prepare_research_fields.py` FIELDS table, `build_fundamental_events.py` `compute_items`,
  `research_fields_holdings.py` 13F specs); `research_add_alpha.py` and `generate_library.py` (CLI, `--replaces`);
  the DSL strings (no result) of `price_volume_ic96_v2`, `pv_fields_ic121_v3`, `slow_price_volume_24_v1` (prior-exposure check).
- From `C:/atx-wt/pool-2/build-equity` (read only): the fields-v10 manifest entries of `earn_recent`, `ea_days_since`,
  `grp_ff49`, `inst_best_ideas`, `inst_n_holders` (clock, definition, staleness text); `v8-i3-plan-v71.json`, the K1
  `--plan-only` receipt of library v7.1 (`mode: metadata-only-no-payload`: slots, lookback, extra fields, node counts; no
  data statistic), used only to calibrate the compiler mirror below.
- Not opened: any NAV, card, marginal, admission, diagnostics, IC or daily file; nothing dated 2024-01-01 or later.
- Exposure disclosed: `code-review-v8-signal.md` (mandated reading) quotes TRAIN statistics of v7.1 members (section 2.3:
  HAC t of six members, the twelve runner sign conflicts; section 2.6: options_implied IC remark). Seen; used for nothing.
  Every choice below follows a rule stated without outcomes (section B, "selection rule"), and that rule picks the same
  members whatever those statistics say.

## Method

- **Semantics** are taken from code, not from names: op table `atx-engine/src/alpha/registry.cpp:20-211` (arity, peeled
  hparams); full-window NaN gate and decay weights `ts_ops.hpp:25-40` (decay_linear weights 1..n oldest to newest);
  min-periods family `lit_ops.hpp:31-42`; `ts_beta_on(y, x, w)` = OLS slope of y on x `lit_ops.hpp:25-30`; NaN in compare
  / select / max `vm.hpp:125-177`; `power` = `std::pow` `vm.hpp:256-258` (so `pow(NaN, 0) = 1`, the cbop zero-fill);
  rank and group kernels `cs_ops.hpp:21-43`; lookback `typecheck.cpp:598-604` (delay d + child, rolling d - 1 + child) and
  `:411-417` (W2 ops); Group typing `typecheck.cpp:568-571`, `bucket` rails `:468-474`.
- **Clocks and definitions** are taken from the field producers and the fields-v10 manifest (cited per item).
- **Static figures** come from a scratch offline mirror of the compiler (parse order with literal folding, peeled
  hparams, hash-consed DAG in arena order, slot taken before children retire, refcount-0 nodes skipped; node count
  includes dead interned nodes as `Program::unique_nodes` does). Calibration: on the 48 rows of the K1 receipt
  `v8-i3-plan-v71.json` it reproduces num_slots, required_lookback, extra_fields and node_count on 48 of 48; it also
  reproduces every K1-equal row of library-v8-draft section 4 and the R2-8 table (15 rows). The mirror is a scratch tool,
  not committed (precedent: library-v8-draft section 0). A K1 row that differs from a row below is reported; a string K1
  refuses is withdrawn.

## A. Defect audit and repairs

### A.0 Result

All 52 strings were checked against their canonical literature definition and their registered formula on five
classes (sign convention, lag / skip period / window alignment, denominator, domain guard, clock / look-ahead margin).
**No string has a sign, lag, skip-period, denominator or look-ahead-margin defect.** Every registered formula is computed
exactly by its string. One string has a domain-guard defect (A-1). Members whose registered deviation from the canonical
form was forced by a constraint that is now lifted are refinements, not defects; they are listed in section B.

### A-1 `q5_eg_f49`: the year-ago ROE is not domain-guarded (repair, rule 7)

- **Defect line.** `... (0.771 * ((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))), 21), grp_ff12f49)`
- **Proof.** HMXZ (2021) dRoe = Roe(q) - Roe(q-4) with Roe = earnings / one-quarter-lagged book equity; Roe is undefined for
  non-positive book equity, and the house encodes that as "non-positive opening book equity -> NaN" in every ROE it
  computes (roe_q, droe's two terms, earn_surprise_comp's two terms, qmj_safety's EVOL leg). In this string the guard
  `0 * log(be_lag1q)` covers the current Roe only; `delay((ni_q / be_lag1q), 252)` has none. On a cell whose opening book
  equity 252 sessions ago was <= 0 (a firm that has since turned positive) the lagged Roe is negative-denominator garbage
  (or +-inf at exactly 0), dRoe is large and of arbitrary sign, and `decay_linear` (NaN-gated only, `ts_ops.hpp:25-28`)
  carries it for 21 sessions to an extreme `group_rank`. The registry note says "a negative book equity a year ago is not
  guarded"; it was not forced by the budget: the guarded string fits (7 slots, house 7; 6 fields, the recorded q5
  exception).
- **Not a taste.** The repair applies the string's own registered domain rule to the second Roe it computes; no other
  cell changes (on every cell where the lagged BE is positive, `0 * log(x)` adds exactly 0).
- **Corrected frozen string** (`q5_eg_f49g`; lookback 272, slots 7, nodes 33, extra fields 6 (exception inherited), 231 B,
  sha256 `07a61a9eeedf47ba`):
```
group_rank(decay_linear((((((-0.029 * log((me_company / at))) + (0.516 * (cfo_ttm / at))) + (0.771 * ((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))) + (0 * log(delay(be_lag1q, 252)))), 21), grp_ff12f49)
```
- **Materiality.** Rare cells (book equity crossing from <= 0 to > 0 within a year); the extreme rank they get is the harm.
- **Trial accounting (recommendation).** A defect repaired blind: 0 admission trials (`--rescreen`, which also inherits
  q5_eg_f49's 6-field exception through `replace_members`); R-2 stands (the defect touches sparse cells of one member of
  52 and was disclosed in the registration). Ruling XIMP-a.
- **add-alpha line** (`PY="C:/Program Files/Python312/python.exe"`; parent / name / spec = the X baseline):
```bash
"$PY" scripts/research_cycle.py add-alpha --id q5_eg_f49g --dsl "group_rank(decay_linear((((((-0.029 * log((me_company / at))) + (0.516 * (cfo_ttm / at))) + (0.771 * ((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))) + (0 * log(delay(be_lag1q, 252)))), 21), grp_ff12f49)" --theme investment_issuance --tier B --prior-sign 1 --citation "Hou, Mo, Xue and Zhang (2021, RF) q5 expected investment growth, Table I Panel D slopes (tau = 1); S-12 FF49 financials" --origin prior --form "as in the DSL" --formula "q5_eg_f49 with the house ROE domain rule applied to the year-ago ROE as well: 0 * log(delay(be_lag1q, 252))" --domain "non-positive opening book equity now or 252 sessions ago -> NaN; non-positive me_company / at -> NaN (log)" --deviation "as q5_eg (q = ME / AT, Cop = CFO / AT, dRoe vs the as-of ROE 252 sessions ago); repair of the unguarded year-ago ROE (XIMP A-1, rule 7)" --replaces q5_eg_f49 --rescreen --parent <X baseline library> --name <X library> --parent-spec <X baseline cell spec>
```

### A.1 Audit table (52 strings)

Verdict OK = the string computes its registered formula and the registered formula is the canonical definition up to
the deviations the registry discloses. "Disclosed" items are registry deviations, not defects. R = the house rank form.

| # | member | canonical | sign | lag / skip / window | denominator / domain | verdict |
|---|---|---|---|---|---|---|
| 1 | value_composite | mean within-FF12 rank of B/M, E/P, CF/P (Israel-Laursen-Richardson 2021) | + | current ME (Asness-Frazzini 2013) | me_company; negatives kept (registered) | OK |
| 2 | bm | BE/ME (Fama-French 1992) | + | current ME | BE <= 0 -> NaN | OK |
| 3 | ep | E/P (Basu 1977) | + | TTM | E <= 0 -> NaN (HXZ) | OK |
| 4 | cfp | CF/P (LSV 1994) | + | TTM | CF <= 0 -> NaN; CFO for E+DP (disclosed) | OK |
| 5 | fcfp | (CFO - capex)/ME | + | TTM | capex NaN -> NaN | OK |
| 6 | ebit_ev_f49 | enterprise multiple (Loughran-Wellman 2011) | + | TTM | ME + debt - che; EBIT, EV <= 0 -> NaN; EBIT for EBITDA (disclosed) | OK |
| 7 | net_payout | (div + rep - iss)/ME (BMRR 2007) | + | TTM | ME | OK |
| 8 | sp | S/P | + | TTM | S <= 0 -> NaN | OK |
| 9 | rd_me | R&D/ME (CLS 2001) | + | TTM | R&D <= 0 or unreported -> NaN (`0 * log(0)` = NaN) | OK |
| 10 | gpa_f49 | GP/AT (Novy-Marx 2013) | + | TTM | current AT (as the paper) | OK |
| 11 | opbe | OP/BE (Fama-French 2015) | + | TTM | BE <= 0 -> NaN; OI for OP (disclosed) | OK |
| 12 | cbop_f49 | cash-based OP (BGLN 2016) | + | TTM | AT; unreported R&D -> 0 via `pow(NaN, 0) = 1` (`vm.hpp:256-258`) | OK |
| 13 | roe_q | IBQ / BE(q-1) (HXZ 2015) | + | quarter | be_lag1q; BE <= 0 -> NaN | OK |
| 14 | roa | ROA (BBF 2010) | + | TTM for quarterly (disclosed) | current AT (disclosed) | OK |
| 15 | accruals_f49 | (NI - CFO)/avg AT (Sloan 1996; Hribar-Collins 2002) | - embedded | TTM | (at + at_lag4)/2 | OK |
| 16 | fscore | Piotroski 2000 | + | YoY at anchor | producer: ROA and CFO on beginning assets, 9 signals (`build_fundamental_events.py:699-716`) | OK |
| 17 | asset_growth_f49 | AT growth (CGS 2008) | - | YoY | at_lag4 | OK |
| 18 | noa_f49 | NOA / AT(t-1) (HHTZ 2004) | - | YoY | at_lag4; noa = at - che - lt + debt = HHTZ (`build_fundamental_events.py:614`) | OK |
| 19 | issuance_xbrl | log share growth (Pontiff-Woodgate 2008) | - | YoY comparative | weighted shares (disclosed) | OK |
| 20 | issuance_vendor | composite issuance 1y (Daniel-Titman 2006) | - | delay 252 | ME growth / total return, split-invariant | OK |
| 21 | ear | 3-day EAR (CJL 1996; Brandt et al. 2008) | + | `earn_recent * delay(earn_recent, 1)` is 1 only at r+1, so the [r-1, r+1] sum is recorded at r+1, carried 126 | EW mkt_ret (disclosed) | OK |
| 22 | res_mom_12_1 | residual momentum (BHM 2011) | + | t-251..t-21, skip 21 | algebra checked: (sum r - beta sum m) / (sd_r sqrt(1 - rho^2)) | OK (in-window alpha, disclosed) |
| 23 | mom_12_1 | 12-1 (Jegadeesh-Titman 1993) | + | skip 21 | - | OK |
| 24 | ind_mom_12_1 | industry momentum (Moskowitz-Grinblatt 1999) | + | industry mean of 12-1 (registered formula) | EW FF49 (disclosed) | OK |
| 25 | within_ind_mom | industry-relative momentum (APS 2000) | + | 12-1 | - | OK |
| 26 | high_52w | P / 52-week high (George-Hwang 2004) | + | max over 252 incl. t | adjusted close in both terms | OK |
| 27 | bac | betting against correlation (AFGP 2020) | - corr | 3-session sums, 250 sessions | volatility-quintile step not expressible then (disclosed): refinement B-4 | OK |
| 28 | smax | scaled MAX proxy (AFGP; BCW 2011) | - | MAX1 over 21 / sd 252 | sd guard | OK (canonical now `smax5`; note N-2) |
| 29 | si_ratio | SI / SO (APR 2005) | - | FINRA as-of | split basis mismatch (disclosed v3 caveat) | OK |
| 30 | dtc | days to cover (HLNSY 2015) | - | FINRA | FINRA floor 1.00 (field caveat) | OK |
| 31 | si_change | change in SI ratio (RRZ 2016, direction) | - | delay 21 | as si_ratio | OK |
| 32 | ind_adj_rev_5 | industry-adjusted reversal (DLS 2014; Hameed-Mian 2015) | - | 5 sessions (registered; papers 1 month) | FF49 demean | OK (refinement B-2) |
| 33 | seasonality_same_month | lag-12 seasonality (Heston-Sadka 2008) | + | [t-251, t-231] = the 21 sessions one year before [t+1, t+21] | - | OK |
| 34 | iv_rv_spread | IV - RV (negative of Bali-Hovakimian's RV - IV) | + | RV21 x sqrt(252) | IV guard; clean-IV caveat: refinement B-1 | OK |
| 35 | opex_at_f49 | operating leverage (Novy-Marx 2011) | + | TTM | sale - oi proxy (disclosed) | OK |
| 36 | sv_flow | shorting flow (Wang-Yan-Zheng 2020) | - | field t-126..t-1 | FF12 demean | OK |
| 37 | qmj_safety | QMJ safety (AFP 2019) | -beta, -lev, -EVOL | `ts_beta_on(r, mkt_ret, 252)` = slope of r on mkt (`lit_ops.hpp:25-30`) | guards on AT and BE inside the windows | OK |
| 38 | nincr | earnings-increase streak (BEF 1999) | + | 63-session spacing (disclosed) | - | OK |
| 39 | q5_eg_f49 | q5 expected growth (HMXZ 2021) | + | delay 252 = as-of ROE a year ago (disclosed) | **year-ago BE unguarded** | **DEFECT A-1** |
| 40 | smax5 | scaled MAX5 (AFGP 2020) | - | 21 / 21 | sd guard | OK |
| 41 | res_mom_ind | industry-residual momentum (BHM 2011) | + | t-251..t-21, m 116 | - | OK |
| 42 | ins_opp | opportunistic insider trading (CMP 2012) | + net | field t-126..t-1 | - | OK (refinement B-3) |
| 43 | inst_best_ideas | best ideas (CPS 2010) | + | field | - | OK |
| 44 | ftd_fail | fails to deliver (EGMR 2009) | - | field | - | OK |
| 45 | ea_overdue | late announcement (Johnson-So 2018) | - | flag < 0 | - | OK |
| 46 | ear_mom_12m | 12-month EAR momentum (Gerard-Jehl 2025) | + | ret a-2..a+2 recorded at a+2, the first session the window is complete (no margin) | - | OK (note N-3) |
| 47 | earn_surprise_comp | mean rank of SUE, dROE, dTax | + | - | both BEs guarded; at_lag4 guarded | OK |
| 48 | op_rd | (OI + R&D)/AT (Novy-Marx-Medhat 2025; BGLN 2015) | + | TTM | zero-fill R&D | OK |
| 49 | dtc_slow | SI / mean volume 126 (HLNSY 2015) | - | 126 | raw volume split basis (R2-d) | OK |
| 50 | pct_accruals | (NI - CFO)/abs(NI) (HLV 2011) | - | TTM | NI = 0 -> NaN | OK |
| 51 | fip_id | frog in the pan (DGW 2014) | + | 12-1 skip | %pos - %neg = -sgn(PRET) ID | OK |
| 52 | si_low_io | SI / IO (APR 2005; Nagel 2005) | - | - | IO > 0 by construction | OK |

### A.2 Margins and notes that are not string defects

- N-1 (field clocks, not strings). Fundamentals are usable one session after the session whose close follows acceptance
  (`--fund-lag-sessions 1`); SEC fields use acceptance before 22:00 UTC of t-1. Both are one session beyond the house
  point-in-time definition (v6 review m4). No roster string adds a delay on top of a field clock. The fund lag cannot be
  moved alone: the universe build refuses a fields manifest whose `fund_lag_sessions` differs from the SIC lag
  (`prepare_recent_research.py:635-636`), so it is a data-protocol change, not a string repair.
- N-2. `smax` (MAX1 / sd252) was the registered stand-in for AFGP's SMAX while the DSL had no top-k; `smax5` is now the
  canonical form and both are members of low_risk. Keeping, removing or regrading `smax` is a PM decision; it is not a
  refinement and not a defect.
- N-3. `ear_mom_12m`: a 252-session window holds five announcements for the few sessions in which this year's announcement
  arrives 251 or fewer sessions after the same quarter's last year; the paper's "prior 12 months" has the same boundary.
  Not a defect.
- N-4. `iv_atm_21d` is the vendor's earnings-cleaned IV (`prepare_research_fields.py:209-215`; the uncleaned twin
  `atmCenH_*` is excluded, `:283-286`), while the RV leg of `iv_rv_spread` is not cleaned. The string computes its
  registered formula, so this is a refinement (B-1), not a defect.

## B. Refinements (4; each replaces one member, one frozen string, 1 admission trial each)

### B.0 Selection rule (stated without outcomes)

A member is eligible when (i) its own registry note records that the canonical form was blocked by a constraint that has
since been lifted, (ii) a ruling named the refinement as "a separate later trial", (iii) a field caveat makes the
string's two legs measure different things, or (iv) a published news / no-news result says the member's bet reverses on
names where another roster theme holds the opposite side. Eligible: bac (i, the W2 `bucket` op), smax (i, but its
canonical form `smax5` is already a member: N-2), ins_opp (ii, Ruling W2-c), inst_best_ideas (ii, W2-c), iv_rv_spread
(iii, N-4), ind_adj_rev_5 (iv). Blocks that still hold (no refinement possible from fields in house): res_mom_12_1 and
res_mom_ind (36-month FF3 betas > 314 bars), nincr (8 quarters > 314), seasonality (multi-lag, also plan section 13),
cbop and opex_at (no COGS / SG&A / working-capital fields), roa (no one-quarter-lagged assets), q5_eg's q with debt and
value_composite with EBIT/EV (field budget; low marginal value, see B.5). Ranked by prior below; inst_best_ideas dropped
(B.5). Tier, theme and roster position are the replaced member's; `--replaces` keeps the roster size.

| # | new id (replaces) | bars | slots | nodes | extra fields | bytes | sha256 (16) | trial |
|---|---|---|---|---|---|---|---|---|
| B-1 | iv_rv_spread_xe (iv_rv_spread) | 21 | 7 | 31 | 2 (iv_atm_21d, earn_recent) | 243 | `29e7d9cf564cacf2` | 1 |
| B-2 | ind_adj_rev_5_nx (ind_adj_rev_5) | 5 | 6 | 21 | 2 (ea_days_since, grp_ff49) | 155 | `9c1d051d4a6ff79a` | 1 |
| B-3 | ins_opp_buy (ins_opp) | 0 | 3 | 4 | 1 (ins_opportunistic_net) | 35 | `981d01b22ba05cfa` | 1 |
| B-4 | bac_vq (bac) | 272 | 6 | 20 | 1 (mkt_ret) | 174 | `116135c031f95c46` | 1 |

All four pass the house budget without an exception (bars <= 314, runner <= 336; slots <= 7; extra fields <= 5;
DSL <= 4,096 B); every field is in the registry fields table and in the fields-v10 list; every op exists with the arity
used (`registry.cpp:29-32, 45-51, 70-102, 186-203`); `bucket` returns a Group that `group_rank` accepts
(`typecheck.cpp:468-474, 568-571`); the peeled hparams are in range (`ts_std_mp` m 15 <= w 21; `bucket` n 5 in [2, 65535]).

### B-1 `iv_rv_spread_xe`: ex-earnings realized volatility on the RV leg

- **Replaces** `iv_rv_spread` (options_implied, B, the theme's only member).
- **String:**
```
rank((ts_backfill((iv_atm_21d + (0 * log(((iv_atm_21d - 0.02) * (5 - iv_atm_21d))))), 5) - (ts_std_mp((((close / delay(close, 1)) - 1) + (0 * log(power((1 - earn_recent), (1 - ts_count_nans(earn_recent, 1)))))), 21, 15) * 15.874507866387544)))
```
- **How it reads.** The IV leg is byte-identical to the member's. The RV leg drops every session with `earn_recent == 1`
  (the vendor reaction session, earnFlag 0, and the session after, earnFlag 1): `power(1 - e, 1 - n)` with
  n = `ts_count_nans(e, 1)` is `1 - e` for a finite e (0 on an event session, so `0 * log(0)` = NaN removes the return)
  and 1 on a NaN e (`pow(NaN, 0) = 1`, so a missing flag keeps the return). `ts_std_mp(., 21, 15)`: sample std (ddof 1,
  as `stddev`) of the kept returns among the last 21 sessions, NaN with fewer than 15 (`lit_ops.hpp:35`).
- **Why.** The IV field is the vendor's earnings-cleaned ATM IV: the event variance of an announcement inside the option's
  life is removed (N-4). The RV leg is not cleaned: for the 21 sessions after every reaction session (about one third of
  a quarterly reporter's sessions) it carries the announcement-day jump, so the spread is mechanically low in proportion
  to the squared announcement return, unrelated to the Bali-Hovakimian mechanism, and the member's rank jumps twice per
  event (when the event enters and when it leaves the 21-session window). The refinement makes both legs ex-event, using
  the same vendor calendar that cleaned the IV (`earn_recent`, same-date vendor row, known at the session's 22:00 UTC mark).
- **Basis.** Bali and Hovakimian (2009, Management Science 55(11)) "Volatility spreads and expected stock returns":
  returns fall in the realized-minus-implied spread (sign of the member). Earnings events inflate implied volatility before
  and realized volatility after the announcement: Patell and Wolfson (1979, JAE; 1981, JAR); the vendor's cleaned IV
  removes the first; this string removes the second.
- **Prior.** Sign +1 (long high IV - RV), unchanged. Expected abs(rho) with iv_rv_spread [est] .80-.92 (identical outside
  the 21 post-event sessions). Cost traded: turnover **down** (the two event jumps go); breadth unchanged (NaN only below
  15 kept returns; at most 2 sessions per event are dropped). Risk: the "clean IV" reading rests on the producer's own
  evidence ("evidence suggests", calendar vintage unproven); if the IV were not cleaned, the refinement would leave the
  pre-event IV bump in place (the canonical spread has both bumps).
- **Registration.** formula "iv_atm_21d (5-session backfill, v3 guard) minus sqrt(252) x the sample std of the daily
  returns of the last 21 sessions without the vendor earnings reaction session and the session after (earn_recent == 1),
  at least 15 kept"; domain "IV outside (0.02, 5) -> NaN; earn_recent NaN keeps the session; fewer than 15 kept returns
  -> NaN"; deviation "ex-event RV so both legs are ex-event (the vendor IV is earnings-cleaned); Bali-Hovakimian use raw
  IV and raw RV; ts_std_mp(21, 15) instead of stddev(21); vendor calendar vintage unproven (field caveat)".

### B-2 `ind_adj_rev_5_nx`: industry-adjusted weekly reversal without earnings-announcement windows

- **Replaces** `ind_adj_rev_5` (reversal_seasonality, B-).
- **String:**
```
rank(((-1 * group_neutralize(((close / delay(close, 5)) - 1), grp_ff49)) + (0 * log(power((ea_days_since - 4.5), (1 - ts_count_nans(ea_days_since, 1)))))))
```
- **How it reads.** The reversal is the member's own expression. The mask is NaN exactly when the latest visible primary
  8-K 2.02 reaction session lies in the 5-session return window: `ea_days_since` in {0, ..., 4} gives a negative
  `ea_days_since - 4.5`, whose log is NaN; >= 5 gives a positive argument (mask 0); a NaN `ea_days_since` (no visible
  announcement within 200 days, foreign private issuers, REITs without 2.02: fields-v10 manifest staleness text) gives
  `pow(NaN, 0) = 1`, log 0, and the name keeps its reversal value. The FF49 mean is taken over all names before the mask,
  as the papers adjust by the whole industry.
- **Why.** Price moves with public news drift; moves without news reverse (Chan 2003; Savor 2012). An earnings reaction is
  the largest scheduled news event, and the roster's earnings_momentum theme (ear, ear_mom_12m, earn_surprise_comp) holds
  the drift side of those same names while this member holds the reversal side: inside the book the two cancel, so the
  member spends gross and turnover against another theme on exactly the names where the literature says reversal fails.
- **Basis.** Chan (2003, JFE 70(2)) "Stock price reaction to news and no-news: drift and reversal after headlines"; Savor
  (2012, JFE 106(3)) "Stock returns after major price shocks: the impact of information"; reversal definition as the
  member: Da, Liu and Schaumburg (2014, MS); Hameed and Mian (2015, JFQA).
- **Prior.** Sign +1 (reversal embedded), unchanged. Expected abs(rho) with ind_adj_rev_5 [est] about .95 on the common
  names. Cost traded: breadth down by the announcement windows (5 of about 63 sessions of a quarterly reporter, about 8%
  of its name-days in this member); turnover: one exit and one re-entry per event, against fewer netting trades with the
  earnings theme.
- **Clock note.** The 8-K clock is lag 1 (acceptance before 22:00 UTC of t-1), so for a pre-market release the reaction
  session itself is not yet excluded on that one session; the vendor `earn_recent` would close that gap but carries the
  unproven calendar vintage, so the point-in-time-proven SEC clock is used.
- **Registration.** formula "-(5-session return minus its FF49 mean); NaN when the latest visible primary 8-K 2.02
  reaction session is within the window (ea_days_since <= 4)"; domain "ea_days_since NaN keeps the name; masked names are
  NaN (neutral)"; deviation "earnings announcements only (no other news); one-session late exclusion for pre-market
  releases (8-K lag rule); weekly horizon kept as registered".

### B-3 `ins_opp_buy`: opportunistic insider purchases only

- **Replaces** `ins_opp` (ownership_flow, B-).
- **String:**
```
rank(max(ins_opportunistic_net, 0))
```
- **How it reads.** `max(x, 0)` (`vm.hpp:132-137`, NaN stays NaN) keeps the net opportunistic purchases per share
  outstanding over t-126..t-1 and sets net sellers to 0, tied with non-traders.
- **Why.** The registered member is seller-dominated (registry note: per date a median of about 40 buyers against about 530
  sellers), so it mostly shorts names whose opportunistic insiders sold; the literature finds the information in
  purchases. Ruling W2-c kept the net measure and named the buy-only leg "a separate later trial": this is that trial.
- **Basis.** Lakonishok and Lee (2001, RFS 14(1)) "Are insider trades informative?" (purchases predict returns, sales do
  not); Jeng, Metrick and Zeckhauser (2003, REStat 85(2)) (about 6% a year abnormal on purchases, none on sales); the
  opportunistic / routine classification of the field: Cohen, Malloy and Pomorski (2012, JF 67(3)).
- **Prior.** Sign +1 (long opportunistic buyers). Expected abs(rho) with ins_opp [est] .30-.50 (the seller block collapses
  into the tie). Cost traded: breadth down sharply (only net buyers differ from the tie); the short side on net sellers
  goes (lower short financing, no shorting of names the momentum theme often holds long); turnover low (126-session field).
- **Registration.** formula "max(ins_opportunistic_net, 0): net opportunistic purchases per share outstanding over
  t-126..t-1 (W5a field), net sellers and non-traders 0"; domain "NaN as ins_opportunistic_net; non-buyers tie"; deviation
  "buy leg only; net within the window, so a name with both counts only when purchases exceed sales; shares, not dollars".

### B-4 `bac_vq`: betting against correlation within volatility quintiles

- **Replaces** `bac` (low_risk, B+).
- **String:**
```
group_rank(decay_linear((-1 * correlation(ts_sum(((close / delay(close, 1)) - 1), 3), ts_sum(mkt_ret, 3), 250)), 21), bucket(stddev(((close / delay(close, 1)) - 1), 252), 5))
```
- **How it reads.** The decayed signal is bac's own. `bucket(stddev(r, 252), 5)` is a Group label (average-rank quintile
  of the 252-session daily-return volatility over the names with a finite volatility on the date, `lit_ops.hpp:44-46`);
  `group_rank` ranks the signal inside each quintile, so every quintile spans [0, 1] (`cs_ops.hpp:41-43`).
- **Why.** The registered deviation: "the paper ranks correlation within volatility quintiles, which is not expressible
  (group operators need a Group classifier; quantile() yields F64)". The W2 op `bucket` is that classifier. Without the
  conditioning the member's low-correlation side loads on high idiosyncratic-volatility names, which is the opposite
  bet to the rest of the low_risk theme; the book's linear vol63 projection removes only the linear part, after blending.
- **Basis.** Asness, Frazzini, Gormsen and Pedersen (2020, JFE 135(3)) "Betting against correlation: testing theories of
  the low-risk effect" (BAC: sort on volatility, then on correlation within each volatility quintile); volatility from
  one year of daily returns and correlation from overlapping 3-day returns: Frazzini and Pedersen (2014, JFE).
- **Prior.** Sign +1 (long low correlation), unchanged. Expected abs(rho) with bac [est] .80-.90. Cost traded: turnover
  slightly up (a name that crosses a quintile boundary jumps in rank; a 252-session volatility moves slowly); breadth
  unchanged (the volatility and the correlation need the same return history).
- **Registration.** formula "-corr(3-session return sums, 3-session mkt_ret sums, 250), decayed 21, ranked within the
  quintiles of the 252-session daily-return volatility"; domain "NaN where the correlation or the volatility is NaN";
  deviation "as bac (250 sessions, equal-weight member market) plus the AFGP volatility-quintile step".

### B.4 add-alpha lines (strings frozen; parent / name / spec = the X baseline at V8-F, the only substitutions allowed)

```bash
"$PY" scripts/research_cycle.py add-alpha --id iv_rv_spread_xe --dsl "rank((ts_backfill((iv_atm_21d + (0 * log(((iv_atm_21d - 0.02) * (5 - iv_atm_21d))))), 5) - (ts_std_mp((((close / delay(close, 1)) - 1) + (0 * log(power((1 - earn_recent), (1 - ts_count_nans(earn_recent, 1)))))), 21, 15) * 15.874507866387544)))" --theme options_implied --tier B --prior-sign 1 --citation "Bali and Hovakimian (2009, MS) volatility spreads: realized minus implied volatility predicts lower returns; event volatility in options: Patell and Wolfson (1979, JAE; 1981, JAR)" --origin prior --prior-sign-source "Bali-Hovakimian 2009" --form "R(x)" --formula "iv_atm_21d (5-session backfill, v3 guard) minus sqrt(252) x the sample std of the daily returns of the last 21 sessions without the vendor earnings reaction session and the session after (earn_recent == 1), at least 15 kept" --domain "IV outside (0.02, 5) -> NaN; earn_recent NaN keeps the session; fewer than 15 kept returns -> NaN" --deviation "ex-event RV so both legs are ex-event (the vendor IV is earnings-cleaned); Bali-Hovakimian use raw IV and raw RV; ts_std_mp(21, 15) instead of stddev(21); vendor calendar vintage unproven (field caveat)" --replaces iv_rv_spread --parent <X baseline library> --name <X library> --parent-spec <X baseline cell spec>
"$PY" scripts/research_cycle.py add-alpha --id ind_adj_rev_5_nx --dsl "rank(((-1 * group_neutralize(((close / delay(close, 5)) - 1), grp_ff49)) + (0 * log(power((ea_days_since - 4.5), (1 - ts_count_nans(ea_days_since, 1)))))))" --theme reversal_seasonality --tier B- --prior-sign 1 --citation "Chan (2003, JFE) drift after news, reversal after no-news; Savor (2012, JFE); Da, Liu and Schaumburg (2014, MS); Hameed and Mian (2015, JFQA) within-industry reversal" --origin prior --prior-sign-source "Chan 2003; Savor 2012; Da-Liu-Schaumburg 2014" --form "R(x)" --formula "-(5-session return minus its FF49 mean); NaN when the latest visible primary 8-K 2.02 reaction session is within the window (ea_days_since <= 4)" --domain "ea_days_since NaN keeps the name; masked names are NaN (neutral)" --deviation "earnings announcements only (no other news); one-session late exclusion for pre-market releases (8-K lag rule); weekly horizon kept as registered" --replaces ind_adj_rev_5 --parent <X baseline library> --name <X library> --parent-spec <X baseline cell spec>
"$PY" scripts/research_cycle.py add-alpha --id ins_opp_buy --dsl "rank(max(ins_opportunistic_net, 0))" --theme ownership_flow --tier B- --prior-sign 1 --citation "Lakonishok and Lee (2001, RFS) insider purchases, not sales, are informative; Jeng, Metrick and Zeckhauser (2003, REStat); Cohen, Malloy and Pomorski (2012, JF 67(3)) opportunistic insiders" --origin prior --prior-sign-source "Lakonishok-Lee 2001; Jeng-Metrick-Zeckhauser 2003" --form "R(x)" --formula "max(ins_opportunistic_net, 0): net opportunistic purchases per share outstanding over t-126..t-1 (W5a field), net sellers and non-traders 0" --domain "NaN as ins_opportunistic_net; non-buyers tie" --deviation "buy leg only (Ruling W2-c's separate later trial); net within the window; shares, not dollars" --replaces ins_opp --parent <X baseline library> --name <X library> --parent-spec <X baseline cell spec>
"$PY" scripts/research_cycle.py add-alpha --id bac_vq --dsl "group_rank(decay_linear((-1 * correlation(ts_sum(((close / delay(close, 1)) - 1), 3), ts_sum(mkt_ret, 3), 250)), 21), bucket(stddev(((close / delay(close, 1)) - 1), 252), 5))" --theme low_risk --tier B+ --prior-sign 1 --citation "Asness, Frazzini, Gormsen and Pedersen (2020, JFE) betting against correlation within volatility quintiles; correlation of overlapping 3-day returns and 1-year volatility: Frazzini and Pedersen (2014, JFE)" --origin prior --prior-sign-source "Asness-Frazzini-Gormsen-Pedersen 2020" --form "group_rank(decay_linear(x, 21), bucket(sd(ret, 252), 5))" --formula "-corr(3-session return sums, 3-session mkt_ret sums, 250), decayed 21, ranked within the quintiles of the 252-session daily-return volatility" --domain "NaN where the correlation or the volatility is NaN" --deviation "as bac (250 sessions, equal-weight member market) plus the AFGP volatility-quintile step, expressible with the W2 bucket op" --replaces bac --parent <X baseline library> --name <X library> --parent-spec <X baseline cell spec>
```

### B.5 Considered and not proposed

- `inst_best_ideas` per holder (`inst_best_ideas / inst_n_holders`, W2-c's other later trial; 4 slots, needs an
  `inst_n_holders` registry field row): dropped. The field pools all 13F filers, and a single-security filer adds an
  overweight near 1, so the per-holder mean is dominated by names with few holders, one of them concentrated, and by
  small caps whose holders are specialists; it trades the breadth confound for a size and filer-type confound. Weak prior.
- `ind_mom_12_1` without the skip month (Moskowitz-Grinblatt: industries do not reverse at one month): Novy-Marx (2012,
  JFE) puts industry momentum in months 12-7; the literature disagrees on the horizon, so no blind prior.
- `ind_adj_rev_5` at one month (the papers' horizon): a near-variant of `rev_21` (atx-db characteristics screen) and of
  `reversal_21_skip5` (pv libraries) was already measured on platform data; B-2 changes one other thing instead.
- `ear` without the 21-session decay (S-11): undecayed announcement CARs (`earn_car_21/42/63_s1`, pv_fields_ic121_v3)
  were already measured on platform data.
- `q5_eg` with debt in q (HMXZ): 7 extra fields (exception needed) for the predictor with the smallest slope (-0.029).
- `value_composite` with EBIT/EV as a fourth yield: 8 fields; overlaps ebit_ev_f49, already a member.
- `smax`: see N-2.

## C. Library-wide processing variants (1)

### C-1 Value theme ranked within FF49 instead of FF12 (`value-ff49-v1`)

- **Frozen rule.** Every member of the value theme replaces its group label (`grp_ff12`, or `grp_ff12f49` for ebit_ev_f49)
  by `grp_ff49`; nothing else in any string changes. The other themes keep house rule R1 (FF12) as registered.
- **Basis.** Ehsani, Harvey and Li (2023, FAJ 79(3)) "Is sector neutrality in factor investing a mistake?": for value, hedging
  the 49 Fama-French industries is the best case (+0.42 SR, value-weighted, 1963-2020; quoted as the prior of
  library-v8-draft R2-8, registered before any v8 read); intra-industry book-to-market predicts better than the raw ratio
  (Cohen and Polk 1998, WP "The impact of industry factors in asset-pricing tests"; Asness, Porter and Stevens 2000, WP
  "Predicting stock returns using industry-relative firm characteristics"). Mechanical: S-12's argument (ratios of banks,
  insurers, REITs and trading firms are not comparable inside FF12 Money) holds for book, earnings, cash-flow and sales
  yields as much as for EV/EBIT, and FF49 separates Money into its four FF49 industries; R2-8 fixed it for ebit_ev only.
- **Strings** (all lookback 20; every field registered; mirror figures):

| new id (replaces) | slots | nodes | extra fields | bytes | sha256 (16) | DSL |
|---|---|---|---|---|---|---|
| value_composite_v49 (value_composite) | 6 | 19 | 5 | 196 | `09fb156c56c6de79` | `(((group_rank(decay_linear((be / me_company), 21), grp_ff49) + group_rank(decay_linear((ni_ttm / me_company), 21), grp_ff49)) + group_rank(decay_linear((cfo_ttm / me_company), 21), grp_ff49)) / 3)` |
| bm_v49 (bm) | 4 | 11 | 3 | 75 | `a37c3ecabe83b698` | `group_rank(decay_linear(((be / me_company) + (0 * log(be))), 21), grp_ff49)` |
| ep_v49 (ep) | 4 | 11 | 3 | 83 | `0d1975e5a8f11aa5` | `group_rank(decay_linear(((ni_ttm / me_company) + (0 * log(ni_ttm))), 21), grp_ff49)` |
| cfp_v49 (cfp) | 4 | 11 | 3 | 85 | `86aed3229acc7590` | `group_rank(decay_linear(((cfo_ttm / me_company) + (0 * log(cfo_ttm))), 21), grp_ff49)` |
| fcfp_v49 (fcfp) | 3 | 9 | 4 | 75 | `408ba943fe971124` | `group_rank(decay_linear(((cfo_ttm - capx_ttm) / me_company), 21), grp_ff49)` |
| ebit_ev_v49 (ebit_ev_f49) | 5 | 18 | 5 | 143 | `4a2b9ba77881892f` | `group_rank(decay_linear((((oi_ttm / ((me_company + debt) - che)) + (0 * log(oi_ttm))) + (0 * log(((me_company + debt) - che)))), 21), grp_ff49)` |
| net_payout_v49 (net_payout) | 3 | 11 | 5 | 90 | `8b6e8423c1997896` | `group_rank(decay_linear((((dvc_ttm + prstkc_ttm) - sstk_ttm) / me_company), 21), grp_ff49)` |
| sp_v49 (sp) | 4 | 11 | 3 | 87 | `1153ac469a8e35fc` | `group_rank(decay_linear(((sale_ttm / me_company) + (0 * log(sale_ttm))), 21), grp_ff49)` |
| rd_me_v49 (rd_me) | 4 | 11 | 3 | 85 | `bf89d6a635c81096` | `group_rank(decay_linear(((xrd_ttm / me_company) + (0 * log(xrd_ttm))), 21), grp_ff49)` |

- **Code path.** None to write: `research_cycle.py add-alpha --id <new> --dsl <string> --replaces <member> --rescreen`
  with the replaced member's theme, tier, citation (plus "; within FF49: Ehsani, Harvey and Li 2023, FAJ"), form and
  notes, nine calls in roster order (`generate_library.replace_members` keeps the roster position; `--rescreen` records
  the same hypothesis in another peer group, Ruling R2-e). Flag absent = the parent library, byte-identical.
- **Trials.** 9 re-screens (R2-e precedent), 0 admission trials, 1 construction cell; recommended as its own cell, after
  the B wave, so the rule is attributable.
- **Prior.** Positive for value; expected abs(rho) of each `_v49` with its original [est] .85-.95 (Money names move most).
  Cost: coverage. `grp_ff49` is NaN for a SIC that Siccodes49 lists under no industry (manifest: "never 49 Other"), where
  FF12 puts it in Other, so those names lose their value members (the four FF49 members already in the roster share this);
  finer groups are smaller (a few FF49 industries hold under ten names, where ranks are coarse). Mechanical criterion
  beside the prereg rule: the value members' covered member cells under `grp_ff49` counted from the payloads before the
  cell (mechanics, no return read), and planned turnover per unit gross not higher.

### C.1 Considered and not proposed

- Per-theme decay half-life by signal speed (the brief's example). For the filing-clock members the input is a step that
  changes once a quarter; a longer DSL decay only delays it (a monotone step is traded in full either way and the book's
  theta .05 already smooths), and a shorter one is S-11, whose undecayed event form was measured before (B.5). The
  aim-side versions are R-3 and R-4, not accepted. No blind prior either way.
- Fundamental lag 0 (v6 review m4). The fund lag is one flag shared with the universe's SIC clock and the universe build
  refuses a mismatch (N-1); it needs a split flag in `prepare_research_fields.py` and a fields rebuild for one session out
  of about 28 sessions of news age (S-11). Small, and a data-protocol cell rather than a processing rule.
- NaN-tolerant smoothing (`decay_linear_mp` so one missing input does not blank a member for 21 sessions): a numerical
  rule with no published basis.

## Rulings root needs (decision -- why -- cost if wrong)

- **XIMP-a:** A-1 enters the X library as a rule-7 repair at 0 admission trials (`--replaces q5_eg_f49 --rescreen`), and R-2
  stands -- the repair applies the string's own registered domain rule and touches sparse cells; the registration
  disclosed the gap -- cost if wrong: the defect is counted as a variant and the X budget pays 1 trial.
- **XIMP-b:** B-1..B-4 are 4 admission trials from the X budget (XPRE), one variant each, judged as one wave whole as R-2
  was (a vetoed replacement does not bring its original back) -- each is one hypothesis with constants fixed here --
  cost if wrong: a vetoed refinement removes a member that would have stayed; mitigation: XPRE may rule that a vetoed
  replacement restores the original at 0 trials.
- **XIMP-c:** C-1 is 9 re-screens and 1 construction cell, run after the B wave -- R2-e precedent (same hypotheses, another
  peer group) -- cost if wrong: counted as admission trials it costs 9 against the X budget.
- **XIMP-d:** B-1 rests on the producer's reading that `iv_atm_21d` is earnings-cleaned ("evidence suggests") -- the field
  caveat is the only source in house -- cost if wrong: the refined spread keeps the pre-event IV bump of the raw series
  and drops the post-event RV bump (still no worse than the canonical raw spread on the post-event half).
- **XIMP-e:** N-2 (`smax` beside its canonical `smax5`) -- a library-composition call, not a refinement -- cost of no
  decision: the low_risk theme keeps two lottery members.

## How root verifies

1. `git diff 3c6ae225 HEAD --stat` lists only this report: no code, registry, library or spec changed, so there is no
   flag-absent identity to check.
2. Each frozen string's SHA-256 equals the tables (A-1, B.0, C-1; first 16 hex shown).
3. K1: run the add-alpha lines in a scratch copy (they run `--plan-only`, or take `--plan-json`); every K1 row must equal
   the tables (required_lookback, num_slots, extra_fields; node_count including dead interned literals). A differing row
   is reported; a string K1 refuses is withdrawn. Calibration of the mirror these tables come from: 48 of 48 rows of
   `build-equity/v8-i3-plan-v71.json` (slots, lookback, extra fields, node count) and the 15 K1-equal rows of
   library-v8-draft section 4 / R2-8.
4. Fields: every field read is in `registry.json` `fields` and in the fields-v10 list of `scripts/specs/v8/lib-v80.json`
   (`add-alpha` and `generate_library` refuse otherwise).

## Cross-lane edits

None.

## Open risks

- `bucket` inside `group_rank` (B-4) and `power(.., 1 - ts_count_nans(.., 1))` as a NaN-safe mask (B-1, B-2) have not been
  through K1 or the IC runner on data in a roster member; the mirror and the typechecker rails say they compile (v9's
  `stmom` uses `bucket` with `group_rank` the same way, also not yet K1-checked).
- B-2 keeps a one-session gap for pre-market releases (8-K lag rule); B-3 concentrates the member on few names; C-1 loses
  names with no FF49 industry.
- Mandated reading exposed v7.1 TRAIN statistics of several members touched here (bac, iv_rv_spread, ins_opp, value
  members): disclosed under Hygiene; the selection rule (B.0) and the C-1 basis do not use them.
- Static figures are from a scratch mirror, not K1; a K1 difference on the 7-slot strings (A-1, B-1) could push them over
  the house slot budget, in which case they are withdrawn or respelled mechanically before any read.
