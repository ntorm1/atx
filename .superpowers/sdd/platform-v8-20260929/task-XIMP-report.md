# Task XIMP report: blind repairs and refinements of the v8.0 roster (expansion X)

Lane XIMP, pool 15, branch `feat/platform-v8-ximp2-20261002` from `3c6ae225`. Research and authoring only: no C++ was
written or built, no data was run, nothing was deleted, no repository code changed (this report is the only file).
Declared 2026-10-02 before any X measurement (Ruling PM7-1). Every string below is frozen here; K1 (`--plan-only`
through `add-alpha`) is the checker of record (R2-f).

## Status

| item | content | count | commit |
|---|---|---|---|
| A | defect audit of the 52 v8.0 strings; repairs | 52 checked; 0 in the five named classes; 1 domain-guard defect (A-1) | see final reply |
| B | refinements (one frozen string per member) | 4 | see final reply |
| C | library-wide processing variants | 1 | see final reply |

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
