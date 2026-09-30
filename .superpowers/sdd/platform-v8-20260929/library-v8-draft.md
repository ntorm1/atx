# Library v8.0 and v8.1 pre-registration draft (tasks R-2 and R-7): definitions, static checks, priors

Root appends this to `v8-prereg.md` after review. Declared 2026-09-29 by the read-only draft lane, before any IC, TRAIN,
return or NAV read of any v8 candidate. Nothing was built or run; no data payload was opened.

**Hygiene.** Read: the R-2 / R-7 / F-1 briefs, the v8 literature report and notes, code review v8 signal (2.6, S-11,
S-12), the v7 draft and prereg, the v7.1 library, registry and generators, the engine alpha sources, the field producers
(code only), the F-1 lane report (commit `1e44f90a`, branch `feat/platform-v8-f-20260929`, code and report only) and the
v7.1 spec's field list (names only). Not read: any payload, build-equity output, u pass, fit, card or NAV. The code review
v8 quotes TRAIN statistics of *existing* members (2.2, 2.3, 2.6); seen there, used for nothing below. Literature numbers
(for example Chen-Zimmermann portfolios 2005-2024) are external published or report statistics quoted as priors; none is
a statistic of this platform's data. No 2024+ platform statistic exists in this file.

## 0. Conventions (binding for every candidate)

- **Sign.** `prior_sign` is +1 for every candidate: the literature sign is embedded in the DSL (`-1 *` for long-low), as
  in v6-v7.1. `R` in a form is `rank` or `group_rank(., grp_ff12)` per house rule R1 (value, profitability and investment
  within FF12).
- **Forms.** Slow additions use the v6 fixed form `R(decay_linear(x, 21))`; aggregates and event flags use `R(x)`. One
  variant per hypothesis. Windows from {5, 21, 63, 126, 252}; 231 (+ delay 21) where the registration names it; window 1
  only as the daily return `delay(close, 1)` and the house NaN test `ts_count_nans(x, 1)` (both already in v7.1).
- **Static rules checked** (all read from code; section 1 gives the lines): engine operator exists with the arity used;
  windows are literals; required lookback <= 314 (house, `registry.json` `house_budget.max_prior_bars`) and <= 336 (runner,
  `score_begin - 63` with `score_begin` 399 on the 4-year role); peak compiled slots <= 7 (house) / 8 (dispatch, as the
  recorded qmj_safety exception) / 64 (runner); extra fields <= 5 (house); DSL <= 4,096 bytes; every field in the fields-v9
  manifest (the 63-row list of `scripts/specs/v71.json`) or in a named pending build; every field in the registry fields
  table (generate_library refuses others).
- **Slots and nodes** are from an offline mirror of the compiler (parse order, literal folding, hparam peeling, DAG CSE,
  linearize liveness; `parser.cpp`, `dag.cpp`, `bytecode.cpp`). Validation: on the 48 v7.1 strings the mirror reproduces
  every recorded required lookback and extra-field list, and its library maximum is 8 slots (qmj_safety), equal to the
  exe's recorded `max_compiled_slots` 8 (runbook, Ruling W0-f). Root confirms every row with `--plan-only` (K1) before
  `add-alpha`; a K1 row that differs from this file is reported, and a string that K1 refuses is withdrawn.
- **Withdrawal rule.** A candidate whose data is absent at its wave's freeze is withdrawn at 0 trials (R-7 brief; Ruling
  E-3). A string a checker refuses is rewritten only mechanically (same semantics) and both strings are shown here.

## 1. Static rules: where each rule lives in code

| rule | code | consequence used below |
|---|---|---|
| operator table | `atx-engine/src/alpha/registry.cpp:20-170` (builtins), `:177-211` (W2 ops) | `ts_mean` (:73, arity 2), `ts_mean_mp` (:202, arity 3, `m` peeled), `sign` (:23), `power` (:29), `ts_count_nans` (:102), `group_rank` (:46), `group_cross` (:190) all exist |
| ternary and `==` | `parser.cpp:258-274` (Select); `typecheck.cpp:200-208` (compare needs F64, gives Mask), `:218-235` (Select: Mask condition, F64 branches) | `(x == 2) ? a : 0` is legal on F64 |
| NaN in compare / select | `vm.hpp:52-60`, `:172-177`, `:205-211`: compare is NaN if either side is NaN; Select with NaN condition is NaN | a NaN calendar gives a NaN cell, not 0 |
| Group dtype | `typecheck.hpp:147-153`: any field named `grp_*` is DType::Group | a group field cannot be compared (`typecheck.cpp:203-206`) or selected (`:229-232`) |
| lookback | `typecheck.cpp:598-604`: delay `d + child`; rolling `(d - 1) + child`; W2 ops `(d - 1) + child` (`:411-417`); fields and literals 0 (`:631-636`) | sums below |
| min periods | `lit_ops.hpp:34`: `ts_mean_mp` is NaN if fewer than `m` finite cells, else sum / n; `typecheck.cpp:451-453`: `m` integer in [1, w] | |
| full window | `ts_ops.hpp:25-28`: every non-W2 rolling op is NaN if any cell in the window is NaN; `ts_ops.hpp:101-111`: TsSum and TsMean share the running-sum missing gate | `ts_mean(x, w)` = `ts_sum(x, w) / w` in semantics |
| rank validity | `cs_ops.hpp:21-31`: valid set = non-NaN cells (so +-inf are ranked as extremes); ties take the average rank; an all-tied row is 0.5 | domain guards matter |
| runner lookback | `atx-impl/src/strategy_ic_admission.cpp:206-208`: refuses `lib.lookback > score_begin - 63` | 336 on the 4-year role |
| runner slots | `strategy_ic_library.cpp:148`: `num_slots <= 64`; `:159` library max slots feeds the memory admission | |
| house budget | `atx-impl/strategies/alphas/registry.json` `house_budget`: extra fields 5, slots 7, prior bars 314, DSL 4,096 B, roster 56; enforced by `generate_library.py` `validate_plan` against K1 rows | exceptions live in `libraries/<name>.json` `budget_exceptions` |
| legacy checker | `check_fund_ic_v6.py:49` `ALLOWED_OPS` (no `ts_mean`, no `quantile`, no `min`); `:68` `MAX_EXTRAS = 5`; `:70` `EXTRAS_EXCEPTIONS` | only binds if root's v8 spec keeps `static_check` = this script (v7.1 spec did) |

## 2. Library v8.0 (task R-2): 7 admission trials + 8 re-screens

| rank key | id | theme | tier | turnover [est] | max expected abs(rho) (with) |
|---|---|---|---|---|---|
| 1 | ear_mom_12m | earnings_momentum | B+ | low (.01-.03) | .45-.60 (ear) |
| 2 | op_rd | profitability_quality | A- | very low | .65-.80 (cbop), .70-.80 (roa) |
| 3 | earn_surprise_comp | earnings_momentum | C+ | very low | .80-.95 (each removed member) |
| 4 | dtc_slow | short_interest | B+ | low | .80-.92 (dtc) |
| 5 | fip_id | price_momentum | B | low | .55-.70 (mom_12_1) |
| 6 | pct_accruals | profitability_quality | B- | very low | .50-.65 (accruals) |
| 7 | si_low_io | short_interest | B- | low | .75-.88 (si_ratio) |

Roster order (the admission tie-break) is the brief's order, appended after the parent members: ear_mom_12m,
earn_surprise_comp, op_rd, dtc_slow, pct_accruals, fip_id, si_low_io.

### R2-1 `ear_mom_12m`: 12-month earnings-announcement-window momentum

- **Registration.** earnings_momentum; tier B+; prior sign +1; origin `prior`; 1 trial. Citation: Gerard and Jehl (2025,
  Financial Analysts Journal) "The Many Facets of Stock Momentum"; Chan, Jegadeesh and Lakonishok (1996, JF).
- **Semantics (brief, binding).** Sum over 252 sessions of the market-adjusted return from close a-3 to close a+2 around
  each earnings date a.
- **Paper.** Sum of market-adjusted returns in +/-2-day windows around all earnings announcements of the prior 12 months,
  largest 1,000 US stocks, holdings linear in the score, no skip month.
- **Final DSL = brief draft (unchanged):**
```
rank(ts_mean_mp((((ea_days_since == 2) ? (((close / delay(close, 5)) - 1) - ts_sum(mkt_ret, 5)) : 0)), 252, 126))
```
- **How it reads.** `ea_days_since` = t minus the reaction session a of the latest visible primary 8-K 2.02
  (`research_fields_sec.py:149-152`). On the one session t = a + 2 the cell is close[t] / close[t-5] - 1 (returns of
  sessions a-2..a+2, that is close a-3 to close a+2) minus the sum of `mkt_ret` over the same five sessions; 0 on every
  other session with a visible calendar; NaN where the calendar is NaN (compare and Select propagate NaN). The mean over
  the 252 sessions needs 126 finite cells.
- **Fields.** `ea_days_since` (fields-v9; not in the registry fields table: registry edit E1), `mkt_ret`, `close`.
  2 extra fields. **Lookback** 5 + 251 = 256. **Slots** 5, nodes 17, 113 B.
- **Static check.** PASS: ops rank, ts_mean_mp (m 126 in [1, 252]), delay, ts_sum, ternary, `==`; windows 5, 252; lookback
  256 <= 314; slots 5 <= 7; 2 fields; also passes the legacy allowlist (the ternary and `==` are not identifiers).
- **Deviations.** (1) Mean over finite sessions, not the sum: with a complete window it is sum / 252 (rank-identical);
  with 126-251 finite sessions it is sum / n, a per-session rate. (2) The window is centred on the reaction session
  (atx-db: the next session for an after-close release), not the calendar date; it always contains the reaction day.
  (3) Clock: an event whose 8-K becomes usable after session a+2 is never seen at `ea_days_since == 2` and drops out
  (atx-db: 19-21% of intraday 2.02 8-Ks report a prior-day event; those accepted before 22:00 UTC of a+1 are still
  captured). Foreign private issuers (6-K) and REITs without 2.02 are NaN. (4) Compounded 5-day stock return minus the
  summed equal-weight member market (`mkt_ret`), not a value-weighted index. (5) Top 3,000, not top 1,000.
- **Prior.** Top 1,000 US: 4.90%/yr, t 2.94, 2010-2024; 3.51%/yr at 7.91% risk, t 2.51, 1992-2024; turnover 430%/yr
  against 976% for the latest announcement return; no long-run reversal; survives factor-momentum controls. Haircut
  class: recently published, under ten years out of sample: the in-sample to post-sample step of 35-40% plus an allowance
  for decay to come. The 1,000-3,000 segment is untested. Expected abs(rho) [est]: ear .45-.60 (it contains the latest
  event), mom_12_1 .30-.45, res_mom_12_1 .25-.40, earn_surprise_comp .20-.35, ea_overdue < .10.
- **Sits beside** `ear`. The literature makes it the theme's core and would reduce `ear`; the brief removes only
  sue, droe and chtax, and R-1 already re-grades `ear` to C+. No post-read removal of `ear`.

### R2-2 `earn_surprise_comp`: one composite for the three filing-clock surprise members

- **Registration.** earnings_momentum; tier C+; prior sign +1; origin `prior`; 1 trial. Citation: Bernard and Thomas
  (1989, JAR) SUE; Hou, Mo, Xue and Zhang (2021, RF) change in ROE; Thomas and Zhang (2011, JAR) tax expense surprises;
  momentum-neutral earnings momentum: Novy-Marx (2015, working paper, "Fundamentally, momentum is fundamental
  momentum"); drift absent in large caps: Martineau (2022, CFR).
- **Semantics (brief, binding).** Mean of the ranks of the three v7.1 members sue, droe and chtax, each taken before its
  outer rank; replaces the three as members.
- **Inner expressions, copied verbatim** (asserted byte-equal against `fund_industry_ic_v71.json`: each member is
  exactly `rank(decay_linear(X, 21))`):
  - `X_sue` = `sue`
  - `X_droe` = `((((ni_q / be_lag1q) - (ni_q_lag4 / be_lag1q_lag4)) + (0 * log(be_lag1q))) + (0 * log(be_lag1q_lag4)))`
  - `X_chtax` = `(((txt_q - txt_q_lag4) / at_lag4) + (0 * log(at_lag4)))`
- **Final DSL (brief template with X substituted):**
```
rank(decay_linear((((rank(sue) + rank(((((ni_q / be_lag1q) - (ni_q_lag4 / be_lag1q_lag4)) + (0 * log(be_lag1q))) + (0 * log(be_lag1q_lag4))))) + rank((((txt_q - txt_q_lag4) / at_lag4) + (0 * log(at_lag4))))) / 3), 21))
```
- **Reading of "inner expression" (Ruling R2-a, needed).** X_m is the member's `x` in its registry form
  `R(decay_linear(x, 21))` (the registry's `notes.formula`), so the composite carries the house 21-session decay once,
  outside the mean. The other reading, X_m = `decay_linear(x_m, 21)`, decays twice (mean age of news about 13 instead of
  about 7 sessions [est]; lookback 40, 7 slots):
  `rank(decay_linear((((rank(decay_linear(sue, 21)) + rank(decay_linear(X_droe, 21))) + rank(decay_linear(X_chtax, 21))) / 3), 21))`.
  Recommended: the first; the second adds exactly the staleness that S-11 names as this theme's defect. Root decides
  before the freeze; the other string is never run.
- **Fields.** sue, ni_q, be_lag1q, ni_q_lag4, be_lag1q_lag4, txt_q, txt_q_lag4, at_lag4: **8 extra fields**.
  **Lookback** 20. **Slots** 6, nodes 33, 218 B.
- **Static check.** BREAKS the house rule `max_extra_fields` 5 (registry `house_budget`; `generate_library.validate_plan`;
  legacy `check_fund_ic_v6.py:68`). Everything else PASS (ops rank, decay_linear, log; lookback 20; 6 slots). No
  mechanical rewrite can lower the count: the three members share no field. **Budget exception needed (Ruling R2-b):**
  `libraries/v80.json` `{"id": "earn_surprise_comp", "max_extra_fields": 8, ...}` (and a legacy `EXTRAS_EXCEPTIONS` entry
  if that checker runs). Memory: the runner keeps at most the library's widest candidate resident, so capacity rises from
  6 (q5_eg) to 8: 2 x 8 B x 1,405 x 6,100 cells = about +131 MiB on the 4-year role, on the runbook's about 1,952 MiB,
  under the 2,560 MiB OD-2 cap [arithmetic]. Refused -> withdrawn, and sue, droe, chtax stay.
- **Deviations.** NaN wherever any of the three is NaN (the value_composite rule), so coverage is the intersection of the
  three members' coverage; each X is ranked on each date over its own finite set; decay after the mean of ranks, where
  the members decayed before their rank.
- **Members leaving library v8.0:** `sue`, `droe`, `chtax` (removed from `libraries/v80.json` members; their registry
  entries stay for v7.1). No other member leaves.
- **Prior.** The literature puts latest-surprise drift at an 80-100% haircut in the top 1,000 (Martineau 2022; a
  replication at t 1.43 ex-microcap) and recommends exactly this collapse to one composite with a prior near zero in the
  top 1,000; any value is expected in ranks 1,000-3,000 [est]. Expected abs(rho) [est]: each removed member .80-.95 (the
  code review 2.6 records the three as "about one signal"), nincr .35-.50, ear_mom_12m .20-.35, ear .20-.30. Its
  purpose is weight, not new information: the surprise block goes from three admitted members to one in the theme.
- **Wave caveat.** If the composite is vetoed at admission, the three members are gone too; the wave is judged whole and
  nothing is restored after a read.

### R2-3 `op_rd`: operating profitability with R&D added back

- **Registration.** profitability_quality; tier A- (brief; the literature report grades this item B+: tier drives the
  within-theme weight under R-1's `ew-theme-std-v1`, so root confirms A- before the freeze, Ruling R2-h); prior sign +1;
  origin `prior`; 1 trial. Citation: Novy-Marx and Medhat (2025, NBER WP 33601); Ball, Gerakos, Linnainmaa and Nikolaev
  (2015, JFE).
- **Semantics (brief, binding).** (Operating income TTM + R&D TTM, missing R&D = 0) / total assets, within FF12.
- **Paper.** Novy-Marx-Medhat: REVT - COGS - (XSGA - XRD) - XINT, "the most power predicting returns" (t 5.69 against
  2.57 for cash-based operating profit, subsuming it after 2000), scaled by book equity plus minority interest; Ball et
  al. (2015): REVT - COGS - (XSGA - XRD) over total assets.
- **Brief draft:** `group_rank(decay_linear(((oi_ttm + xrd0_ttm) / at), 21), grp_ff12)` (needs F-1 `xrd0_ttm`).
- **Final DSL (existing fields; the cbop zero-fill idiom and the house asset guard):**
```
group_rank(decay_linear((((oi_ttm + (power(xrd_ttm, (1 - ts_count_nans(xrd_ttm, 1))) - ts_count_nans(xrd_ttm, 1))) / at) + (0 * log(at))), 21), grp_ff12)
```
- **Why the rewrite (Ruling R2-c).** The registered semantics hold exactly: a NaN `xrd_ttm` becomes 0 through
  `power(x, 1 - n) - n`, n = `ts_count_nans(x, 1)`, which relies on `std::pow(NaN, 0) = 1` (registry cbop domain note);
  it is the same zero-fill cbop uses, so op_rd and cbop differ only in the numerator (OI against CFO). v8.0 is "from
  existing fields" and should not wait for fields v10. `+ (0 * log(at))` is the house guard of gpa, roa, cbop (NaN when
  total assets are not positive). Only difference to `xrd0_ttm` (F-1 report: "0 where NaN and sale_ttm is finite"): a
  row with finite oi_ttm and at but NaN sale_ttm gets R&D = 0 here and NaN there.
- **Fields.** oi_ttm, xrd_ttm, at, grp_ff12 (4). **Lookback** 20. **Slots** 5, nodes 18, 153 B.
- **Static check.** PASS (ops group_rank, decay_linear, power, ts_count_nans, log; window 1 only in the NaN test, as
  cbop). Draft also PASS (3 slots) but needs a field absent from fields-v9.
- **Deviations.** OI is EBIT after D&A (no D&A field to add back); interest is not deducted (no interest field); assets,
  not book equity plus minority interest; financials ranked within FF12 Money (op_rd is not in the S-12 list; the
  registration says FF12).
- **Prior.** Haircut class: profitability family 0-30% (the one theme that earned more after publication: 60 against 31
  bps/month). Within-school revision (same author pool as Ball et al. 2016). Expected abs(rho) [est]: cbop .65-.80, roa
  .70-.80, opbe .60-.75, gpa .55-.70, opex_at -.30 to -.50. Sits beside cbop, gpa and opbe (no removal is registered).

### R2-4 `dtc_slow`: days to cover with a 126-session volume denominator

- **Registration.** short_interest; tier B+; prior sign +1; origin `prior`; 1 trial. Citation: Hong, Li, Ni, Scheinkman
  and Yan (2015, NBER WP 21166) "Days to cover and stock returns".
- **Semantics (brief, binding).** Short interest shares / mean daily volume over 126 sessions; short high.
- **Paper.** Days to cover = short ratio / average daily turnover (volume / shares outstanding); the result is the same
  with 1-, 6- or 12-month turnover, so a slow denominator is free.
- **Final DSL = brief draft:**
```
rank(decay_linear((-1 * (si_shares / ts_mean(volume, 126))), 21))
```
- **Static check.** PASS under K1 and generate_library (`ts_mean` is engine op `registry.cpp:73`, arity 2); window 126,
  21; lookback 125 + 20 = 145; slots 5; 1 extra field. BREAKS the legacy allowlist only (`check_fund_ic_v6.py:49` has no
  `ts_mean`). Pre-declared mechanical respelling if root's v8 spec keeps that checker (not a variant: same window, same
  full-window missing gate, `ts_ops.hpp:101-111`; mean = sum / n):
  `rank(decay_linear((-1 * (si_shares / (ts_sum(volume, 126) / 126))), 21))` (145 bars, 5 slots).
  F-1's `vol_126` is not used: it lags one session and has a 63-session minimum (F-1 report), a different string.
- **Deviations.** (1) Raw share volume, not turnover: after a split inside the window, pre-split volume is on the old
  share basis while si_shares is on the new one, so days to cover is overstated by up to the split ratio for up to 126
  sessions [roughly 0.5-1% of name-days, est]. The paper's per-day turnover avoids it; the turnover form
  `(si_shares / shares_out) / ts_mean((volume / shares_out), 126)` would be a different registration (Ruling R2-d:
  default keeps the brief). (2) FINRA semi-monthly short interest, as-of, 45-day staleness. (3) No size-tercile rank
  (the book projects on log dollar volume). (4) A 21-session decay on a semi-monthly series (registered).
- **Prior.** Value-weighted 0.67%/month, t 2.24, 1988-2012; the edge over the short ratio shrinks to 0.25-0.29%/month
  outside the smallest names; drawdown in the 2008 short-sale ban. Haircut class: short interest 60-70%, then minus 10-20
  bps/month borrow (DTC is also listed among signals with under ten years out of sample). Expected abs(rho) [est]: dtc
  .80-.92 (same numerator; FINRA's ADV against a 126-session mean): **redundancy risk** at the .90 screen, where dtc
  (B+, earlier roster) wins; si_ratio .65-.80; sv_flow .20-.35; ftd_fail .20-.30. Sits beside dtc.

### R2-5 `pct_accruals`: percent accruals

- **Registration.** profitability_quality; tier B-; prior sign +1; origin `prior`; 1 trial. Citation: Hafzalla, Lundholm
  and Van Winkle (2011, The Accounting Review) "Percent accruals".
- **Semantics (brief, binding).** (Net income TTM - CFO TTM) / abs(net income TTM); long low.
- **Paper.** (IB - OANCF) / abs(IB); divide by .01 when IB is 0; annual; price >= $5.
- **Brief draft:** `rank(decay_linear((-1 * ((ni_ttm - cfo_ttm) / abs(ni_ttm))), 21))`: static PASS, but a net income of
  exactly 0 gives +-inf (IEEE x / 0), which `decay_linear` carries (its gate tests NaN only) and `rank` ranks as an
  extreme (valid set = non-NaN, `cs_ops.hpp:21-24`) for 21 sessions.
- **Final DSL (house domain guard added; the paper's zero-income case becomes NaN):**
```
rank(decay_linear(((-1 * ((ni_ttm - cfo_ttm) / abs(ni_ttm))) + (0 * log(abs(ni_ttm)))), 21))
```
  `x + 0 * log(d)` is NaN unless d > 0 (`generate_fund_ic_v4.py:413`); here only at ni_ttm = 0.
- **Fields.** ni_ttm, cfo_ttm (2). **Lookback** 20. **Slots** 4, nodes 14, 92 B. **Static check.** PASS.
- **Deviations.** Net income, not income before extraordinary items; TTM on the filing clock, not annual; no $5 filter;
  zero income -> NaN instead of the .01 divisor (USD fields; the paper's .01 is $10k); plain `rank` as registered (the
  `accruals` member is FF12 group-ranked).
- **Prior.** CZ-own 2005-24: ex-microcap .25%/month (t 2.30), value-weighted .21 (t 1.07); spanning alpha t 1.94, R2 .48;
  correlation +.60 with Sloan accruals. Haircut class: generic 65-75%. Expected abs(rho) [est]: accruals .50-.65, cbop
  .30-.45, fscore .20-.30. Sits beside accruals.

### R2-6 `fip_id`: information discreteness ("frog in the pan")

- **Registration.** price_momentum; tier B; prior sign +1; origin `prior`; 1 trial. Citation: Da, Gurun and Warachka
  (2014, RFS) "Frog in the pan: continuous information and momentum".
- **Semantics (brief, binding).** Share of positive days minus share of negative days over the 12-1 window (231 sessions
  ending 21 sessions ago): long names whose past return came in many small steps, short the mirror image.
- **Paper.** ID = sgn(PRET) x (%neg - %pos) over the formation window; low ID = continuous information; used in PRET x ID
  double sorts; six-month momentum rises from -2.07% (discrete) to 5.94% (continuous), 1927-2007.
- **Final DSL = brief draft:**
```
rank(decay_linear(delay(ts_mean(sign(((close / delay(close, 1)) - 1)), 231), 21), 21))
```
- **Identity.** `ts_mean(sign(r), 231)` at t-21 = %pos - %neg over t-251..t-21 = -sgn(PRET) x ID whenever PRET != 0
  (sgn^2 = 1). The registered signal is the paper's ID signed by the direction of the formation return: continuous
  winners rank high, continuous losers low, discrete winners and losers in the middle. It is DGW's interaction with the
  magnitude of PRET dropped (only its sign enters).
- **Static check.** PASS under K1 (ops rank, decay_linear, delay, ts_mean, sign; windows 1, 231 registered, 21; lookback
  1 + 230 + 21 + 20 = 272; slots 4; 0 extra fields). Legacy allowlist: same `ts_mean` note; respelling
  `rank(decay_linear(delay((ts_sum(sign(((close / delay(close, 1)) - 1)), 231) / 231), 21), 21))` (272 bars, 4 slots).
- **Deviations.** sign(0) = 0 (zero-return days count as neither, the paper's first variant); adjusted close (dividends
  in the return); full window: one missing close in 232 sessions gives NaN, as res_mom_12_1.
- **Prior.** No post-2007 or large-cap test found; ex-microcap 12-month momentum was flat 2005-24 (CZ-own Mom12m t .76).
  Haircut class: momentum 60-70% on the firm-specific part. Expected abs(rho) [est]: mom_12_1 .55-.70, res_mom_12_1
  .45-.60, within_ind_mom .45-.60, high_52w .40-.55, smax / smax5 -.10 to -.30. Sits beside the price-momentum members;
  it raises momentum's share of the book (the report's main risk).

### R2-7 `si_low_io`: short interest over institutional ownership

- **Registration.** short_interest; tier B-; prior sign +1; origin `prior`; 1 trial. Citation: Asquith, Pathak and Ritter
  (2005, JFE) "Short interest, institutional ownership, and stock returns"; Nagel (2005, JFE).
- **Semantics (brief, binding).** Short interest over institutional ownership; short high; also the borrow proxy of G-3a.
- **Paper.** APR: within the top short-interest percentile, low institutional ownership (a binding short constraint)
  predicts underperformance; the continuous SI / IO ratio is the utilisation proxy of Nagel and of Hong et al.
- **Final DSL = brief draft:**
```
rank(decay_linear((-1 * ((si_shares / shares_out) / inst_own_share)), 21))
```
- **Fields.** si_shares, shares_out, inst_own_share (fields-v9; `inst_own_share` is not in the registry fields table:
  registry edit E1). `inst_own_share` = 13F inst_shares / shares_out at the quarter-end session, NaN above 2
  (`research_fields_holdings.py:135-143`); it is finite only when a holder row exists, so it is never 0 (no division by
  zero). **Lookback** 20. **Slots** 4, nodes 10, 74 B. **Static check.** PASS.
- **Deviations.** Continuous ratio over all names, not APR's top-1% conditional sort; no IO floor (ranks bound the tail;
  a floor would be a free parameter); 13F is 47-150 days old and filer type is unclassified; shares_out is the 90-day
  lagged vendor count.
- **Prior.** The CZ-own top-1% portfolio (2.60%/month ex-microcap, t 3.24) does not transfer to a continuous rank over
  3,000 names. Haircut class: short interest 60-70%, then minus 10-20 bps/month borrow; low-IO names are hard to borrow
  and the fee is unobserved. Expected abs(rho) [est]: si_ratio .75-.88 (**redundancy risk**; the v7 draft excluded SI / IO
  at >= .85 [est]), dtc .60-.75, ftd_fail .30-.45, inst_best_ideas -.20 to -.35. Sits beside si_ratio.

### R2-8 FF49 regrouping of financial firms (S-12): 8 re-screens, needs one group field

- **How a financial firm is identified.** `grp_ff12 == 11`: FF12 Money = SIC 6000-6999 (French Siccodes12;
  `FF12_NUMBERS` Money 11, `prepare_research_fields.py:307`; `reference_classifications.py:238`). Inside Money, FF49 45
  Banks, 46 Insur, 47 RlEst, 48 Fin (`reference_classifications.py:467-487`). Note: REITs (SIC 6798) are FF49 48 "Fin"
  (Trading) in French's table, not 47; SPACs (6770) and the other non-operating SICs are outside the linked-operating
  universes (`prepare_recent_research.py:88`). A Money SIC that Siccodes49 does not list has no FF49 code.
- **DSL conditional? No.** Group fields are DType::Group by name (`typecheck.hpp:147-153`); a comparison needs F64
  operands (`typecheck.cpp:203-206`), so `grp_ff12 == 11` is refused; Select branches must be F64 (`:229-232`), so
  `c ? grp_ff49 : grp_ff12` is refused; `group_cross(grp_ff12, grp_ff49)` (label g1 x 2^26 + g2) splits every FF12
  industry by FF49 and so changes every non-financial rank; no numeric field marks financials. **A new group field is
  needed: `grp_ff12f49`** (section 8, F-A).
- **DSL change per member.** Replace the single `grp_ff12` token with `grp_ff12f49`; new ids with suffix `_f49`
  (lookback, slots, fields unchanged):

| new id (replaces) | DSL | bars | slots | fields |
|---|---|---|---|---|
| ebit_ev_f49 (ebit_ev) | `group_rank(decay_linear((((oi_ttm / ((me_company + debt) - che)) + (0 * log(oi_ttm))) + (0 * log(((me_company + debt) - che)))), 21), grp_ff12f49)` | 20 | 5 | 5 |
| gpa_f49 (gpa) | `group_rank(decay_linear(((gp_ttm / at) + (0 * log(at))), 21), grp_ff12f49)` | 20 | 4 | 3 |
| cbop_f49 (cbop) | `group_rank(decay_linear((((cfo_ttm + (power(xrd_ttm, (1 - ts_count_nans(xrd_ttm, 1))) - ts_count_nans(xrd_ttm, 1))) / at) + (0 * log(at))), 21), grp_ff12f49)` | 20 | 5 | 4 |
| noa_f49 (noa) | `group_rank(decay_linear(((-1 * (noa / at_lag4)) + (0 * log(at_lag4))), 21), grp_ff12f49)` | 20 | 4 | 3 |
| accruals_f49 (accruals) | `group_rank(decay_linear(((-1 * ((ni_ttm - cfo_ttm) / ((at + at_lag4) / 2))) + (0 * log(((at + at_lag4) / 2)))), 21), grp_ff12f49)` | 20 | 5 | 5 |
| opex_at_f49 (opex_at) | `group_rank(decay_linear((((sale_ttm - oi_ttm) / at) + (0 * log(at))), 21), grp_ff12f49)` | 20 | 4 | 4 |
| asset_growth_f49 (asset_growth) | `group_rank(decay_linear(((-1 * ((at / at_lag4) - 1)) + (0 * log(at_lag4))), 21), grp_ff12f49)` | 20 | 5 | 3 |
| q5_eg_f49 (q5_eg) | `group_rank(decay_linear(((((-0.029 * log((me_company / at))) + (0.516 * (cfo_ttm / at))) + (0.771 * ((ni_q / be_lag1q) - delay((ni_q / be_lag1q), 252)))) + (0 * log(be_lag1q))), 21), grp_ff12f49)` | 272 | 6 | 6 |

- **Static check.** PASS for all eight once `grp_ff12f49` is in the fields manifest and the registry; q5_eg_f49 carries
  q5_eg's recorded exception (6 fields) to the new id.
- **Why new ids (Ruling R2-e).** The registry holds one entry per id and regenerates v7.1 from it; editing `ebit_ev` in
  place changes `fund_industry_ic_v71.json` (test `test_v71_library_byte_identical`). Each `_f49` member takes its
  original's roster position (no change of admission order) and the originals leave v8.0. Consequence: the generator's
  slim recipe counts the eight as new (`admission_trials` 15 for v8.0); the prereg records them as 8 re-screens of
  registered hypotheses, not new trials, as the brief does.
- **Prior.** Unchanged per member (same tiers, citations plus "S-12 FF49 financials"). Ehsani-Harvey-Li: hedging 49
  industries is the best case for value (+0.42 SR, value-weighted, 1963-2020); no source compares FF12 with FF49 for a
  top-3,000 rank book; including financials is worth about +0.03 SR on average (Soebhag et al.). Expected abs(rho) of
  each `_f49` with its original [est]: .90-.98 (only FF12 Money names move).
- **Status.** NEEDS-FIELD. If `grp_ff12f49` is not in the fields build used by the R-2 cell, the regrouping is withdrawn
  from v8.0 (0 re-screens) and the seven candidates run alone; it cannot be added after the R-2 read.

## 3. Library v8.1 (task R-7): 5 admission trials possible, 3 withdrawn

| id | theme | tier | status | turnover [est] | max expected abs(rho) (with) |
|---|---|---|---|---|---|
| comp_eq_iss_5y | investment_issuance | B | NEEDS-FIELD (F-1, built) | very low | .55-.70 (issuance_vendor) |
| coskew_60m | low_risk | B | NEEDS-FIELD (F-1, built) | low | .20-.35 (mom_12_1, negative) |
| tax_book | profitability_quality | B | WITHDRAWN | - | - |
| gscore_lowbm | profitability_quality | B- | NEEDS-FIELD (new producer field) | low | .30-.45 (fscore) |
| night_day | reversal_seasonality | B- | WITHDRAWN | - | - |
| nt_late | filing_events (new) | B- | WITHDRAWN | - | - |
| nonreliance_402 | filing_events (new) | C+ | NEEDS-FIELD (event field, existing stage) | event | < .10 |
| earn_consistency | earnings_momentum | C+ | NEEDS-FIELD (new producer field) | very low | .35-.50 (nincr) |

Roster order: comp_eq_iss_5y, coskew_60m, gscore_lowbm, nonreliance_402, earn_consistency (brief order minus
withdrawals), appended after the v8.0 members.

### R7-1 `comp_eq_iss_5y`: five-year composite equity issuance

- **Registration.** investment_issuance; tier B; prior sign +1; origin `prior`; 1 trial. Citation: Daniel and Titman
  (2006, JF) "Market reactions to tangible and intangible information".
- **Semantics (brief).** 5-year composite equity issuance; long low.
- **Paper.** iota = log(ME_t / ME_t-5y) - r(t-5y, t), r the log total return; low is good.
- **Field (F-1, commit 1e44f90a, not yet merged).** `ceq_iss_5y` = ln(ME_a / ME_b) - ln(P_a / P_b), a = t-1, b = a -
  1,260 sessions; the price terms cancel to ln(q_a / q_b), q = shares / cumulative return factor: split-invariant, with
  dividends counted as payout; domain [-ln 100, ln 100]; finite from about 2017-06, so throughout TRAIN.
- **Final DSL (house issuance form, R1):**
```
group_rank(decay_linear((-1 * ceq_iss_5y), 21), grp_ff12)
```
- **Fields** ceq_iss_5y, grp_ff12 (2). **Lookback** 20. **Slots** 3, nodes 7. **Static check.** PASS once the field is in
  the manifest and registry. No existing field can serve: a DSL five-year version needs `delay(., 1260)`, above the 336
  runner bound; issuance_vendor is the one-year analogue.
- **Deviations.** Within FF12 (Daniel-Titman rank raw); t-1 clock; the house A8 share count (90-490 days old); names
  with under 1,260 sessions of history are NaN, so recent IPOs (heavy issuers) drop out.
- **Prior.** Original t 4.39 (1968-2003). CZ-own 2005-24: ex-microcap .38%/month (t 2.24), value-weighted .56 (t 2.56);
  spanning alpha t 2.68, R2 .36; largest correlations low risk -.34 and profitability +.34. Haircut class: issuance 50-60%.
  Expected abs(rho) [est]: issuance_vendor .55-.70, issuance_xbrl .45-.60, net_payout .35-.50, asset_growth .25-.40.
  Sits beside the issuance members (the report's risk: near-duplicate under the redundancy screen).

### R7-2 `coskew_60m`: Harvey-Siddique coskewness

- **Registration.** low_risk; tier B; prior sign +1; origin `prior`; 1 trial. Citation: Harvey and Siddique (2000, JF)
  "Conditional skewness in asset pricing tests".
- **Semantics (brief).** Coskewness on 60 monthly returns; long low.
- **Field (F-1).** `coskew_60m` = mean(e_i e_m^2) / (sqrt(mean(e_i^2)) mean(e_m^2)) over 60 monthly (21-session) returns
  ending t-1; market = vendor-wide equal-weight daily market; at least 48 of 60 months; raw returns, no risk-free rate.
- **Final DSL (low-risk form):**
```
rank(decay_linear((-1 * coskew_60m), 21))
```
- **Fields** coskew_60m (1). **Lookback** 20. **Slots** 3, nodes 6. **Static check.** PASS once the field is in the
  manifest and registry. Not expressible in the DSL (1,260 sessions > 336).
- **Deviations.** Equal-weight vendor market (paper: value-weighted CRSP); 21-session months; raw returns; 48-month
  minimum (F-1 declared constant).
- **Prior.** Original .30%/month, t 1.96 (1964-1993). CZ-own 2005-24: ex-microcap .39 (t 2.86), value-weighted .34
  (t 2.18); spanning alpha t 3.07, R2 .20; largest correlations momentum -.31, value +.30; the daily one-year version
  fails value-weighted. Haircut class: generic 65-75%. The price-risk-v1 projection (beta252, vol63, ladv63) does not
  remove coskewness. Expected abs(rho) [est]: mom_12_1 -.20 to -.35, value_composite .15-.30, bac .10-.25, smax .10-.20.
  Sits beside bac, smax, smax5.

### R7-3 `tax_book`: WITHDRAWN before any read (0 trials)

- Registration as briefed: profitability_quality, B, +1; Lev and Nissim (2004, The Accounting Review) "Taxable income,
  future earnings, and equity values". Semantics: current tax expense TTM / (statutory rate x net income TTM); long high.
- **Data.** Needs current (or deferred) income tax. atx-db's item catalogue has `current_tax` (1025, us-gaap
  CurrentIncomeTaxExpenseBenefit) and `deferred_tax` (1026) (`atx-db/src/atx_db/seeds/fundamental_items.csv:182-190`),
  but the fundamental-events export that the fields builder reads carries only total income tax (`txt_q`,
  `build_fundamental_events.py:115`). That is OD-6, which Ruling E-3 says this sprint cannot serve.
- **Can an existing field serve? No.** `txt_q` is total tax (current + deferred): total / (rate x NI) is the effective
  tax rate over the statutory rate, because deferred tax offsets exactly the temporary book-tax differences the signal
  measures. It is also quarterly (only txt_q and txt_q_lag4 exist), not TTM.
- For a later sprint: with a `txc_ttm` field the string would be `rank(decay_linear(txc_ttm / (0.21 * ni_ttm) ...))`
  with the paper's NI < 0 rule; the 21% rate is constant across TRAIN (TCJA from 2018), so it cancels in the rank.

### R7-4 `gscore_lowbm`: Mohanram G-score among low book-to-market names

- **Registration.** profitability_quality; tier B-; prior sign +1; origin `prior`; 1 trial. Citation: Mohanram (2005,
  Review of Accounting Studies) "Separating winners from losers among low book-to-market stocks using financial statement
  analysis".
- **Semantics (brief).** Mohanram G-score, 7 of 8 components, inside the bottom book-to-market tercile.
- **Paper.** Among low-BM firms, eight 0/1 signals against industry medians: ROA, CFO / assets, CFO > NI, low ROA
  variability, low sales-growth variability, R&D / assets, capex / assets, advertising / assets.
- **Why not a DSL string.** (a) The two variability signals need several years of quarterly history (Mohanram: four
  years of quarterly data, as recalled; root verifies), at least 504-1,008 sessions of delays, above the 336 runner bound.
  (b) The other inputs alone are 9 or more extra fields (ni_ttm, cfo_ttm, xrd_ttm, capx_ttm, at, be, me_company, a group
  field, ...) against 5. (c) The tercile needs a mask from `quantile()`, which is also outside the legacy allowlist. A
  producer field, as `fscore` is, is the house route.
- **Final DSL (fscore's form, R1):**
```
group_rank(decay_linear(gscore7_lowbm, 21), grp_ff12)
```
  Fields gscore7_lowbm, grp_ff12 (2); lookback 20; slots 3, nodes 5. PASS once the field exists (spec F-C).
- **Deviations.** No advertising item (7 of 8); bottom tercile (brief) where Mohanram uses the lowest BM quintile [as
  recalled; verify]; TTM items on the filing clock; industry medians over bottom-tercile names only by construction.
- **Prior.** Original 1.58%/month, t 9.14, equal-weighted, 1978-2001, with a non-standard lag; replicators get t near 6.
  CZ-own 2005-24: ex-microcap .43 (t 2.52), value-weighted .39 (t 1.85); spanning alpha t 2.11, R2 .27. Haircut class:
  generic 65-75% (Novy-Marx-Medhat: quality composites are spanned by profitability). Expected abs(rho) [est]: fscore
  .30-.45, roa .30-.45, op_rd .30-.45, rd_me .20-.30, si_ratio -.20 to -.30. Coverage about one third of names by
  construction (the report card flags coverage below 80%; it gates nothing). Sits beside fscore.
- **Status.** NEEDS-FIELD; withdrawn at the R-7 freeze if fields v11 does not carry it.

### R7-5 `night_day`: WITHDRAWN before any read (0 trials)

- Registration as briefed: reversal_seasonality, B-; Lou, Polk and Skouras (2019, JFE) "A tug of war: overnight versus
  intraday expected returns"; Akbas, Boehmer, Jiang and Koch (2022, JFE); Barardehi, Bogousslavsky and Muravyev (2026,
  RFS). Semantics: 63-session overnight return minus intraday return. Would-be string: `rank(ts_sum((ret_overnight -
  ret_intraday), 63))` (62 bars, 3 slots, 2 fields; F-1 lags both fields one session).
- **Reason 1: no canonical sign for the registered form.** Akbas et al. sign (+) a frequency count (days with a positive
  overnight and a negative intraday return), not this return difference. Lou-Polk-Skouras sorts give a positive
  close-to-close return for both past overnight winners (+.45%/month) and past intraday winners (+.64%/month), which
  leaves the difference unsigned; Barardehi et al. find momentum in intraday returns and none in overnight returns, which
  implies a negative sign for overnight minus intraday. With no canonical sign the v7 rule R5.5(i) excludes a candidate
  (precedent: inst_breadth_chg in v7).
- **Reason 2: data.** The fields need an open; the research projection has none (`prepare_recent_research.py:71`), and
  F-1 reads it from the vendor source, refusing with `FieldNeedsOpen` if absent (unconfirmed until root's v10 build).
- Also: a close-to-close book captures about one sixth to one quarter of the headline effect (report arithmetic).

### R7-6 `nt_late`: WITHDRAWN before any read (0 trials)

- Registration as briefed: new theme filing_events, B-; Bartov and Konchitchki (2017, Accounting Horizons). Semantics:
  short for 126 sessions after a first NT 10-K or NT 10-Q.
- **Data.** No fields producer reads NT forms: the SEC module keeps only forms starting with "8-K"
  (`research_fields_sec.py:708`). The plan lists NT rows under OD-6; Ruling E-3 withdraws OD-6 members.
- **Can an existing field serve? No.** The `k8_*` fields count 8-Ks only; `ea_overdue` (the expected 2.02 date passed)
  is a different event and already a member. The literature notes say atx-db's SEC stage classifies NT forms; if root
  finds NT rows with an acceptance clock in the pinned stage manifest (metadata only) before the R-7 freeze,
  reinstatement is a new root ruling, and the field would be `nt_first_126` (spec F-E).

### R7-7 `nonreliance_402`: 8-K Item 4.02 non-reliance

- **Registration.** New theme `filing_events`; tier C+; prior sign +1; origin `prior`; 1 trial. Citation: the literature
  notes give only a data-vendor study (sec-api.io: over 8,000 disclosures 2004-2023, -1.1% one day after, -2% over 20
  days, 97% followed by a restatement); no peer-reviewed source is in the notes.
- **Semantics (brief).** Short for 63 sessions after an 8-K Item 4.02.
- **Data.** Item codes are read today: `K8_MATERIAL_ITEMS` includes "4.02" (`research_fields_sec.py:67`, used at
  `:728-729`). **Existing field?** `k8_item_material_21` cannot serve: it pools six items (1.01, 2.01, 2.05, 2.06, 4.02,
  5.02) over 21 sessions. A new field `k8_item402_63` from the same stage file needs no atx-db change (spec F-B).
- **Final DSL (event-flag form R(x), as ea_overdue):**
```
rank((-1 * k8_item402_63))
```
  Fields 1; lookback 0; slots 3, nodes 4. PASS once the field exists.
- **Prior.** Vendor evidence only, hence C+. Haircut class: generic 65-75%. Expected abs(rho) < .10 with every member
  (filing events rank first in the report's orthogonality order).
- **Theme risk (Ruling R7-b).** The flag marks about 1% of names or fewer [est: about 400 disclosures a year
  across all filers, a fraction of them in the top 3,000, each flagged for 63 sessions]. Under R-1's `ew-theme-std-v1` a one-member
  theme is re-ranked to full dispersion and capped at 1 / (2T) of the book, so its weight concentrates on a few dozen
  short names, often hard to borrow. Root decides before the freeze: keep the new theme (brief) or place the member in an
  existing theme (a change of registration). A new theme needs a registry `themes` entry and the fitter's theme list.

### R7-8 `earn_consistency`: earnings consistency

- **Registration.** earnings_momentum; tier C+; prior sign +1 (CZ SignalDoc direction; root confirms the sign column
  before the freeze); origin `prior`; 1 trial. Citation: Alwathainani (2009) (the notes name no journal; replicators used
  the dissertation).
- **Semantics (brief).** 4-year mean of year-on-year EPS growth with same-sign filter.
- **Paper (CZ SignalDoc).** Average growth over the previous 48 months, growth = (EPS - EPS 12 months earlier) / mean(EPS
  12 and 24 months earlier); exclude abs(growth) > 600% and growth whose sign differs from its 12-month lag.
- **Why not a DSL string.** With ni_q, ni_q_lag4, shrs_q, shrs_q_lag4, EPS eight quarters back needs `delay(., 252)` and
  the four-year mean needs delays of 252, 504 and 756: required lookback 1,008 (mirror), which BREAKS the runner rule
  `lookback <= score_begin - 63` = 336 (`strategy_ic_admission.cpp:206-208`) and the house 314. A shorter window would be
  another hypothesis, not a rewrite.
- **Final DSL:** `rank(decay_linear(eps_consist_4y, 21))`; 1 field; lookback 20; slots 3. PASS once the field exists
  (spec F-D).
- **Prior.** Original .36%/month, t 2.67 (1971-2002). CZ-own 2005-24: ex-microcap .23 (t 1.83), value-weighted .46
  (t 1.91); spanning alpha t 2.38, R2 .46; correlation +.46 with composite issuance. Haircut class: generic 65-75%.
  Expected abs(rho) [est]: nincr .35-.50, comp_eq_iss_5y .30-.45, roe_q .20-.35, earn_surprise_comp .20-.30.
- **Status.** NEEDS-FIELD; C+ evidence for a new producer, so root may withdraw it before the freeze at 0 trials.

## 4. Summary

| candidate | wave | status | final DSL sha256 (first 16) | bars | slots | fields | trial cost |
|---|---|---|---|---|---|---|---|
| ear_mom_12m | 8.0 | READY (registry field row E1) | eb4ced22fe5bcff5 | 256 | 5 | 2 | 1 |
| earn_surprise_comp | 8.0 | READY (budget exception R2-b) | 959788e7fdc9f6b0 | 20 | 6 | 8 | 1 |
| op_rd | 8.0 | READY | 211c045b41bd4879 | 20 | 5 | 4 | 1 |
| dtc_slow | 8.0 | READY | 4554a05f99eef281 | 145 | 5 | 1 | 1 |
| pct_accruals | 8.0 | READY | 4711ad7c27bad506 | 20 | 4 | 2 | 1 |
| fip_id | 8.0 | READY | f13685ff95864040 | 272 | 4 | 0 | 1 |
| si_low_io | 8.0 | READY (registry field row E1) | d76f14082ca7255a | 20 | 4 | 3 | 1 |
| FF49 financials (8 `_f49` ids) | 8.0 | NEEDS-FIELD `grp_ff12f49` | see R2-8 | 20-272 | 4-6 | 3-6 | 8 re-screens |
| comp_eq_iss_5y | 8.1 | NEEDS-FIELD (F-1 merge, fields v10) | 269ddd246a363af9 | 20 | 3 | 2 | 1 |
| coskew_60m | 8.1 | NEEDS-FIELD (F-1 merge, fields v10) | de6da2553c0374fc | 20 | 3 | 1 | 1 |
| tax_book | 8.1 | WITHDRAWN (OD-6 current tax) | - | - | - | - | 0 |
| gscore_lowbm | 8.1 | NEEDS-FIELD `gscore7_lowbm` | c41ec275a0934fff | 20 | 3 | 2 | 1 |
| night_day | 8.1 | WITHDRAWN (no canonical sign; open unconfirmed) | - | - | - | - | 0 |
| nt_late | 8.1 | WITHDRAWN (OD-6 NT forms) | - | - | - | - | 0 |
| nonreliance_402 | 8.1 | NEEDS-FIELD `k8_item402_63` | 63a5d1432211bb85 | 0 | 3 | 1 | 1 |
| earn_consistency | 8.1 | NEEDS-FIELD `eps_consist_4y` | 943f41874f2125bc | 20 | 3 | 1 | 1 |

Counts: READY 7; NEEDS-FIELD 6 (5 candidates and the FF49 block); WITHDRAWN 3.

## 5. Trial accounting

- **Admission trials (budget at most 15):** v8.0 = 7. v8.1 = 5 if all four new fields and the F-1 merge land, 2 if only
  F-1 lands. Total 9-12, within 15. Withdrawn candidates cost 0.
- **Re-screens:** 8 (the `_f49` block), counted separately as the brief does; 0 if `grp_ff12f49` is not built. If root
  counted them as admission trials the total would be 17-20, above 15; Ruling R2-e must say which.
- **Construction cells:** 1 per wave (N 42 for R-2 and N 47 for R-7 in the plan's order).
- **Roster:** v8.0 = 48 - 3 + 7 = 52 (the `_f49` ids replace in place). v8.1 = 52 + up to 5 = 57, one over the registry
  `max_roster` 56. Raising it edits `house_budget`, which the v7.1 slim recipe records (`generation.house_budget`), so
  the committed v7.1 recipe would no longer regenerate byte for byte (Ruling R7-a: raise it with a regenerated and
  re-pinned v7.1 slim recipe, or keep v8.1 at 56 by withdrawing one conditional member before the freeze).

Appendix A block (to carry on every result): TRAIN construction cells <N>; admission trials this sprint <k>; window
research-window-v2 (2020-2023); hidden 2024+ unread in this sprint; validation reads before v8: 2 (2023-2024); 2025+
never read.

## 6. Registry and library edits, in the order `add-alpha` should apply them

`add-alpha` (task A-2) adds one entry per call; it has no remove or replace flag, so E3, E4 and E7 are edits of
`libraries/<name>.json` by hand before the lock. No existing theme text is edited (theme texts feed the regenerated v7.1
library bytes); only new themes, fields and alphas are added.

- **E1 (fields table):** add `ea_days_since` (formula `sec-ea-days-since-reaction-v1`, origin fields_v8, producer
  `research_fields_sec.py`) and `inst_own_share` (formula `13f-asof45-io-share-qe-v1`, origin fields_v9, producer
  `research_fields_holdings.py`), each with clock and basis copied from the fields-v9 manifest row.
- **E2 (alphas, v8.0):** `add-alpha --parent v71 --name v80` in this order: ear_mom_12m, earn_surprise_comp, op_rd,
  dtc_slow, pct_accruals, fip_id, si_low_io (section 7). `added_in` v80; `form`: ear_mom_12m `R(x)`,
  earn_surprise_comp `R(decay_linear(mean_k R(x_k), 21))`, the others `R(decay_linear(x, 21))`; `prior_sign_source`
  and `notes{formula, domain, deviation}` from sections 2-3.
- **E3 (libraries/v80.json):** remove `sue`, `droe`, `chtax` from members; add budget exception
  `earn_surprise_comp: max_extra_fields 8` (Ruling R2-b); keep `qmj_safety: max_slots 8` and `q5_eg: max_extra_fields 6`.
- **E4 (only when `grp_ff12f49` is in the fields manifest of the R-2 run):** fields table row `grp_ff12f49`; alphas
  ebit_ev_f49, gpa_f49, cbop_f49, noa_f49, accruals_f49, opex_at_f49, asset_growth_f49, q5_eg_f49 (theme, tier, citation
  of the original plus "S-12 FF49 financials"; added_in v80); in v80 members each replaces its original at the same
  position; move the q5_eg exception to q5_eg_f49.
- **E5:** `generate_library.py --library v80 --plan-json <K1 plan>`; every row must equal section 4 (bars, fields; slots
  as the mirror unless K1 differs, then report); pin library and slim-recipe SHAs in `v8-prereg.md` before the u pass.
- **E6 (fields table, after the lane F merge and fields v10):** `ceq_iss_5y`, `coskew_60m` (formula ids and basis from the
  v10 manifest).
- **E7 (themes, only if nonreliance_402 proceeds as registered):** add `filing_events`: "Adverse SEC filing events: an 8-K
  Item 4.02 non-reliance disclosure within the last 63 sessions; a recent event predicts lower returns." Plus the fitter's
  theme list (as `ownership_flow` in v7).
- **E8 (fields table, when fields v11 carries them):** `k8_item402_63`, `gscore7_lowbm`, `eps_consist_4y`.
- **E9 (alphas, v8.1):** `add-alpha --parent <v80 if R-2 accepted, else v71> --name v81` in order comp_eq_iss_5y,
  coskew_60m, gscore_lowbm, nonreliance_402, earn_consistency, skipping any whose field is absent at the freeze.
- **E10:** R7-a roster decision; then `generate_library.py --library v81 --plan-json ...` and SHA pins.

## 7. `add-alpha` commands (form of plan Appendix B; `PY="C:/Program Files/Python312/python.exe"`)

READY, library v8.0 (run in this order, after E1):

```bash
"$PY" scripts/research_cycle.py add-alpha --id ear_mom_12m --dsl "rank(ts_mean_mp((((ea_days_since == 2) ? (((close / delay(close, 5)) - 1) - ts_sum(mkt_ret, 5)) : 0)), 252, 126))" --theme earnings_momentum --tier B+ --prior-sign 1 --citation "Gerard and Jehl (2025, FAJ) The Many Facets of Stock Momentum; Chan, Jegadeesh and Lakonishok (1996, JF)" --origin prior --parent v71 --name v80
"$PY" scripts/research_cycle.py add-alpha --id earn_surprise_comp --dsl "rank(decay_linear((((rank(sue) + rank(((((ni_q / be_lag1q) - (ni_q_lag4 / be_lag1q_lag4)) + (0 * log(be_lag1q))) + (0 * log(be_lag1q_lag4))))) + rank((((txt_q - txt_q_lag4) / at_lag4) + (0 * log(at_lag4))))) / 3), 21))" --theme earnings_momentum --tier C+ --prior-sign 1 --citation "Bernard and Thomas (1989, JAR); Hou, Mo, Xue and Zhang (2021, RF); Thomas and Zhang (2011, JAR); composite: Novy-Marx (2015, WP); Martineau (2022, CFR)" --origin prior --parent v71 --name v80
"$PY" scripts/research_cycle.py add-alpha --id op_rd --dsl "group_rank(decay_linear((((oi_ttm + (power(xrd_ttm, (1 - ts_count_nans(xrd_ttm, 1))) - ts_count_nans(xrd_ttm, 1))) / at) + (0 * log(at))), 21), grp_ff12)" --theme profitability_quality --tier A- --prior-sign 1 --citation "Novy-Marx and Medhat (2025, NBER WP 33601); Ball, Gerakos, Linnainmaa and Nikolaev (2015, JFE)" --origin prior --parent v71 --name v80
"$PY" scripts/research_cycle.py add-alpha --id dtc_slow --dsl "rank(decay_linear((-1 * (si_shares / ts_mean(volume, 126))), 21))" --theme short_interest --tier B+ --prior-sign 1 --citation "Hong, Li, Ni, Scheinkman and Yan (2015, NBER WP 21166) Days to cover and stock returns" --origin prior --parent v71 --name v80
"$PY" scripts/research_cycle.py add-alpha --id pct_accruals --dsl "rank(decay_linear(((-1 * ((ni_ttm - cfo_ttm) / abs(ni_ttm))) + (0 * log(abs(ni_ttm)))), 21))" --theme profitability_quality --tier B- --prior-sign 1 --citation "Hafzalla, Lundholm and Van Winkle (2011, TAR) Percent accruals" --origin prior --parent v71 --name v80
"$PY" scripts/research_cycle.py add-alpha --id fip_id --dsl "rank(decay_linear(delay(ts_mean(sign(((close / delay(close, 1)) - 1)), 231), 21), 21))" --theme price_momentum --tier B --prior-sign 1 --citation "Da, Gurun and Warachka (2014, RFS) Frog in the pan" --origin prior --parent v71 --name v80
"$PY" scripts/research_cycle.py add-alpha --id si_low_io --dsl "rank(decay_linear((-1 * ((si_shares / shares_out) / inst_own_share)), 21))" --theme short_interest --tier B- --prior-sign 1 --citation "Asquith, Pathak and Ritter (2005, JFE); Nagel (2005, JFE)" --origin prior --parent v71 --name v80
```

Not READY: run only after the named field is in the fields manifest and the registry (strings frozen here):

```bash
# after grp_ff12f49 (E4): the eight _f49 strings of R2-8 with --theme/--tier/--citation of each original
"$PY" scripts/research_cycle.py add-alpha --id comp_eq_iss_5y --dsl "group_rank(decay_linear((-1 * ceq_iss_5y), 21), grp_ff12)" --theme investment_issuance --tier B --prior-sign 1 --citation "Daniel and Titman (2006, JF)" --origin prior --parent v80 --name v81
"$PY" scripts/research_cycle.py add-alpha --id coskew_60m --dsl "rank(decay_linear((-1 * coskew_60m), 21))" --theme low_risk --tier B --prior-sign 1 --citation "Harvey and Siddique (2000, JF)" --origin prior --parent v80 --name v81
"$PY" scripts/research_cycle.py add-alpha --id gscore_lowbm --dsl "group_rank(decay_linear(gscore7_lowbm, 21), grp_ff12)" --theme profitability_quality --tier B- --prior-sign 1 --citation "Mohanram (2005, RAS)" --origin prior --parent v80 --name v81
"$PY" scripts/research_cycle.py add-alpha --id nonreliance_402 --dsl "rank((-1 * k8_item402_63))" --theme filing_events --tier C+ --prior-sign 1 --citation "sec-api.io Item 4.02 study (vendor, 2004-2023)" --origin prior --parent v80 --name v81
"$PY" scripts/research_cycle.py add-alpha --id earn_consistency --dsl "rank(decay_linear(eps_consist_4y, 21))" --theme earnings_momentum --tier C+ --prior-sign 1 --citation "Alwathainani (2009)" --origin prior --parent v80 --name v81
```

If root's v8 spec keeps `static_check` = `check_fund_ic_v6.py`, dtc_slow and fip_id use the `ts_sum(., w) / w`
respellings of R2-4 and R2-6 (decided before the first add-alpha, never after).

## 8. Field specs for the fields lane

All: point in time, no row dated at or after the seal, existing payloads byte-identical under `--reuse` (opt-in module
fields, as F-1 did), a test that the value at t is unchanged when rows after t are mutated.

- **F-A `grp_ff12f49`** (for R2-8; needed before the R-2 cell). Categorical code stored as f64; the `grp_` prefix makes it
  a Group. From the same latest visible valid SIC row, clock and 550-day staleness as `grp_ff12` / `grp_ff49`: if ff12 =
  11 (Money) and ff49 in {45, 46, 47, 48}: 100 + ff49 (145 Banks, 146 Insur, 147 RlEst, 148 Fin); if ff12 = 11 and ff49
  is NaN (a Money SIC that Siccodes49 does not list): 11; otherwise ff12 (1-10, 12); NaN wherever grp_ff12 is NaN. It is
  a cellwise function of the grp_ff12 and grp_ff49 payloads. Tests: non-Money cells equal grp_ff12 bit for bit; the NaN
  pattern equals grp_ff12's; the four Money mappings. Suggested formula id `ff12-money-ff49-v1`.
- **F-B `k8_item402_63`** (for R7-7). Stage `sec_filings`, `eight_k_items.parquet` as the `k8_*` fields: original 8-K
  forms (form starts with "8-K", not an amendment), item "4.02"; event session = the first session whose 22:00 UTC mark
  follows the acceptance (K8 lag rule); value 1 if such a filing became usable within the last 63 sessions (e <= t <
  e + 63), else 0; NaN by the K8 presence rule; primary lines only (LINK_RULE). The existing `_eightk` path with a second
  item mask; `k8_*` payloads unchanged. Suggested formula id `sec-k8-item402-63-v1`.
- **F-C `gscore7_lowbm`** (for R7-4). At session t (fundamentals on the T21 clock): bm = be / me_company (be > 0);
  bottom tercile of bm over the role's member names at t. For names in it, seven indicators against the median of the
  same measure over bottom-tercile names of the same two-digit SIC (`grp_sic2`) at t: ROA (ni_ttm / at) above; CFO / at
  above; CFO > NI; variance of quarterly ROA over the prior 16 fiscal quarters below; variance of quarterly sales growth
  over the prior 16 fiscal quarters below; R&D / at above (missing R&D = 0); capex / at above. Value = number of
  indicators met (0-7); NaN outside the bottom tercile or when an input other than R&D is missing. Root copies the exact
  component definitions (averaging of assets, windows, industry level, quintile against tercile) from Mohanram (2005)
  before the build; the tercile is the registered choice. Suggested formula id `mohanram-g7-lowbm3-sic2-v1`.
- **F-D `eps_consist_4y`** (for R7-8). EPS_q = ni_q / shrs_q per fiscal quarter as known at t; g_q = (EPS_q - EPS_q-4) /
  mean(EPS_q-4, EPS_q-8); value = mean of g over the latest 16 fiscal quarters with at least 12 finite (declared, as
  F-1's 48 of 60); NaN when abs(g) of the latest quarter exceeds 6 or its sign differs from g_q-4's. Root checks the
  denominator rule against the CZ code before the build (quarterly YoY in place of CZ's monthly observations is a
  disclosed deviation). Suggested formula id `alwathainani-eps-consistency-16q-v1`.
- **F-E `nt_first_126`** (only if nt_late is reinstated by ruling). 1 for 126 sessions after the first NT 10-K or NT
  10-Q of the CIK (no NT filing in the prior 365 days), usable at the first session whose 22:00 UTC mark follows the
  acceptance; else 0; NaN without SEC filer presence.
- **Already built (lane F, commit `1e44f90a`):** `ceq_iss_5y`, `coskew_60m` (plus `vol_126`, `xrd0_ttm`, which v8 does not
  use, and `ret_overnight` / `ret_intraday`, which need the open). Fields v10 exceeds the 64-row manifest cap, so it
  needs B-2's cap lift (or a lean manifest) first.

## 9. Rulings root needs before the freeze (each "decision -- why -- cost if wrong")

- **R2-a:** earn_surprise_comp X_m = the members' `x` (single decay) -- the draft's outer `decay_linear` only makes sense on
  undecayed inputs, and a double decay adds staleness -- cost if wrong: the composite is one decay faster than intended.
- **R2-b:** earn_surprise_comp budget exception, 8 extra fields -- no respelling can lower it; about +131 MiB, inside the
  OD-2 cap -- cost if refused: the candidate is withdrawn and sue, droe, chtax stay.
- **R2-c:** op_rd on existing fields with the cbop zero-fill -- same registered semantics, no wait for fields v10 --
  cost if wrong: rows with NaN sale_ttm get R&D = 0 instead of NaN.
- **R2-d:** dtc_slow keeps raw volume (brief) -- one variant per hypothesis -- cost if wrong: split names carry an inflated
  days to cover for up to 126 sessions.
- **R2-e:** FF49 as new `_f49` ids, counted as 8 re-screens, and withdrawn from v8.0 if `grp_ff12f49` is absent at the R-2
  run -- keeps v7.1 byte-identical and the trial budget legible -- cost if wrong: 8 more trials against the 15 cap.
- **R2-f:** static checker of record = K1 (`--plan-only` + `generate_library`); if the legacy checker is kept, use the
  respellings -- cost if wrong: a refused string at add-alpha time.
- **R2-g:** pct_accruals carries the house zero-denominator guard -- a zero income gives +-inf otherwise -- cost if
  wrong: none on non-zero incomes.
- **R2-h:** op_rd tier A- (brief) against B+ (report) -- tier sets the within-theme weight under ew-theme-std-v1 -- cost
  if wrong: op_rd over- or under-weighted in its theme by 0.9 / 0.8.
- **R7-a:** roster cap for v8.1 (57 > 56) -- see section 5.
- **R7-b:** filing_events as a one-member theme -- concentration on a sparse short flag -- cost if wrong: a large share of
  short risk on a few dozen hard-to-borrow names.
- **R7-c:** fields v11 carries F-B (and F-C, F-D if the fields lane builds them); anything absent at the freeze is
  withdrawn at 0 trials.
- Withdrawn now at 0 trials: tax_book (OD-6), night_day (sign; open), nt_late (OD-6).

## 10. Not included, and why

- `ear` is not removed although the literature would reduce it (the brief removes only the three surprise members; R-1
  re-grades it to C+). cbop is not removed although Novy-Marx-Medhat say op_rd subsumes it. Removals after a read are
  not allowed.
- No size-tercile or turnover-based variants of dtc_slow, no IO floor for si_low_io, no PRET-magnitude interaction for
  fip_id: each would be a second variant of a registered hypothesis.
- `auditor_401`, `exch_switch`, `new_listing_age`, cross-firm and 13F-structure families: not in the R-2 / R-7 briefs.
