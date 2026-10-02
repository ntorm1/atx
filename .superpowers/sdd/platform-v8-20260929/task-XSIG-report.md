# Task XSIG report: the X screen set (expansion X, new signals orthogonal by construction)

Lane XSIG, pool 12, branch `feat/platform-v8-xsig-20261002` from `3c6ae225`. Python and documentation only; synthetic
data only. Declared 2026-10-02, before any IC, TRAIN, return, turnover or NAV read of any candidate (this lane has no
trials; Ruling PM7-1: X measurements start after V8-F and integration 8, under `v8x-prereg.md`). This file is the
registration of the X screen set.

## Status

| item | status | commit |
|---|---|---|
| Static and semantic check of the frozen strings (`xsig_check.py`) | DONE | `b30311a3` |
| Registration (this report: candidates, registry rows, add-alpha lines, rulings) | DONE | this commit |

Test line: `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/xsig_check.py` -> PASS
(16 recorded compiler rows reproduced, 52 v8.0 strings parsed, 5 candidates within the house budget, 2 semantic checks
on synthetic panels, every cell equal to a direct implementation of the registered definition). A mutation probe (6
plausible algebra mistakes: wrong lag, a sign flip, the one-quarter run kept, an unsigned split, a shifted window, the
sign of the count) failed the semantic checks every time.

## 1. Result in one paragraph

The X screen set has **5 candidates**, not 12. Two are new and read fields already in fields v10 that no roster member
reads (`k8_intensity` on `k8_count_63`, `inst_persist` on `inst_own_chg_q`); three are carried byte for byte from the
v9 draft (`834d5a05`): the two FIELD-BUILT items the brief names (`earn_season`, `nt_late`) and one conditioning form
with a published basis (`stmom`). Every other unread field was examined and fails on sign, horizon, redundancy or prior
exposure (section 6). The finding agrees with LIB3's: the price and volume space is already measured (74 pv
hypotheses plus the atx-db characteristics screen), and the in-house non-price sources are mostly read; new families
now need new data (XDATA's list). No new field builder was needed: both new candidates read fields that exist.

## 2. The screen set, ranked by prior of marginal contribution

Ranking criterion (blind): published evidence strength x orthogonality to the v8.0 roster (and to R-7 / R-12) x how
much of the effect the book's slow construction can capture (aim-partial, trade fraction .05: a target change is about
two thirds traded after 21 sessions, 1 - .95^21 = .66) x breadth. Not ranked by anything measured.

| rank | id | theme | tier | origin of the registration | status | bars | slots | nodes | extra fields | DSL sha256 (16) | turnover [est] |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `stmom` | price_momentum | B- | v9 draft C-1 (LIB3), carried verbatim | READY (fields v9+) | 41 | 5 | 21 | shares_out | `06dc6238d7e94089` | mid-high |
| 2 | `earn_season` | reversal_seasonality | B- | v9 draft C-4 (LIB3), carried verbatim | FIELD-BUILT (draft fields v13) | 0 | 5 | 11 | ea_days_to_expected, earn_season_rank | `64a0be8f2c71efd1` | mid |
| 3 | `k8_intensity` | filing_events | C+ | **new (this lane)** | READY (fields v10) | 209 | 5 | 15 | k8_count_63 | `0b7d6cdeb6c2f86e` | very low |
| 4 | `inst_persist` | reversal_seasonality | C+ | **new (this lane)** | READY (fields v10) | 272 | 6 | 34 | inst_own_chg_q | `ea338c04bdff78ef` | low |
| 5 | `nt_late` | filing_events | B- | v9 draft C-3 (LIB3), carried verbatim | FIELD-BUILT (draft fields v13) | 0 | 3 | 4 | nt_first_126 | `bafc4e3a93204e59` | event |

All five pass the house budget (bars <= 314 and the runner's 336; slots <= 7; extra fields <= 5; DSL <= 4,096 B) with
no exception. Static figures come from the offline compiler mirror in `xsig_check.py` (calibrated: it reproduces all
16 recorded rows of library-v8-draft section 4, task-LIB2 section 1 and the v9 draft section 1, and `qmj_safety`'s 8
slots); K1 (`--plan-only` through add-alpha) is the checker of record (R2-f). Roster order (admission tie-break) is the
rank order above, appended after the X parent's members.

Excluded by the brief and not re-registered: R-7 (comp_eq_iss_5y, coskew_60m, gscore_lowbm, nonreliance_402,
earn_consistency) and R-12 (iv_vol_of_vol, day_rev_freq, exch_switch).

## 3. The two new candidates

### X-1 `k8_intensity`: information intensity from Form 8-K filing frequency (rank 3)

- **Registration.** Theme `filing_events` (Ruling XSIG-a); tier C+; prior sign +1 (literature sign embedded: long
  low intensity); origin `prior`; 1 admission trial. Citation: Zhao, X. (2017), "Does Information Intensity Matter
  for Stock Returns? Evidence from Form 8-K Filings", Management Science 63(5), 1382-1404.
- **Canonical definition and sign (abstract, read from IDEAS / RePEc).** A firm's Form 8-K filing frequency proxies
  its information intensity; "firms with higher information intensity experience lower future returns and lower
  future volatilities"; the marginal return effect is larger at low intensity and high prior volatility; an
  intensity-based long-short portfolio earns 4.3% a year, 4.4% after the Fama-French three factors and momentum;
  interpreted through noisy rational expectations and estimation risk. **Sign: long low 8-K frequency, short high.**
  Sample: 8-K filings 1994-2009 (as summarised by a citing paper; not verified from the paper itself).
- **Frozen DSL:**
```
rank(decay_linear((-1 * (((k8_count_63 + delay(k8_count_63, 63)) + delay(k8_count_63, 126)) + delay(k8_count_63, 189))), 21))
```
- **How it reads.** `k8_count_63` at t counts the distinct original 8-K accessions of the CIK usable at sessions e with
  e <= t < e + 63 (`research_fields_sec.py`, formula `sec-k8-count63-v1`). The four terms cover the disjoint blocks
  [t-62, t], [t-125, t-63], [t-188, t-126], [t-251, t-189], so the sum is exactly the count of original 8-Ks usable in
  the last 252 sessions (checked cell by cell on a synthetic panel in `xsig_check.py`). Negated, house 21-session
  decay, plain cross-sectional rank (Zhao sorts unconditionally).
- **Constants (registered blind).** 252 sessions = one year, the paper's annual frequency (the exact counting window of
  the paper is not verified here: declared); 63 = the field's own window; 21 = the house slow form.
- **Fields.** `k8_count_63` (in fields v10, `scripts/specs/v8/lib-v80.json`; not in the registry fields table:
  registry row L2 below). 1 extra field.
- **Domain / NaN.** NaN unless the CIK has a visible original 8-K within 365 days before the t-1 mark at each of t,
  t-63, t-126 and t-189 (the field's presence rule): foreign private issuers (6-K filers) and non-filers are NaN. The
  stage starts in 2009, so the 252-session count exists throughout TRAIN.
- **Deviations.** (1) Rolling 252 sessions, daily, house decay (paper: portfolio formation on a fixed window, monthly).
  (2) Original 8-K family forms (8-K, 8-K12B, 8-K12G3, 8-K15D5), no amendments; co-registrant 8-Ks count for each
  registrant; the 8-K 2.02 earnings releases count (about four a year for every domestic filer). (3) No size control
  inside the signal: the book's `price-risk-v1` projection (beta252, vol63, ladv63) carries it.
- **Orthogonality by construction.** Data source: SEC 8-K filing counts. No v8.0 member reads an 8-K count; the
  `ea_*` members (ear_mom_12m, ea_overdue) read the 8-K 2.02 *calendar* (timing of one event type), and R-7's
  `nonreliance_402` reads one item as a sparse 63-session flag. Horizon: a one-year information-flow rate, which no
  theme measures. Nearest roster members by economics: the issuance / investment members (financing and M&A 8-Ks
  raise the count) and the low-risk members (Zhao: high intensity predicts low volatility, so the long leg leans to
  more volatile names, part of which vol63 projects out). Expected abs(rho) [est]: issuance_xbrl, issuance_vendor,
  asset_growth_f49 +.10 to +.25 (same direction: short heavy corporate activity); bac, smax, qmj_safety -.10 to -.25;
  nonreliance_402, ea_overdue < .10; every other member < .10. Not a re-statement: no member's input contains a filing
  count.
- **Prior.** Haircut class: generic 65-75% (single study, published 2017, sample ending 2009, no replication found).
  Breadth: every domestic 8-K filer, so it is the only broad member of `filing_events` (the others are sparse flags,
  R7-b). Turnover [est]: very low (a yearly count, smoothed).

### X-2 `inst_persist`: institutional trade persistence (rank 4)

- **Registration.** Theme `reversal_seasonality` (Ruling XSIG-b); tier C+; prior sign +1 (embedded: long persistently
  sold, short persistently bought); origin `prior`; 1 admission trial. Citation: Dasgupta, A., A. Prat and M. Verardo
  (2011), "Institutional Trade Persistence and Long-Term Equity Returns", Journal of Finance 66(2), 635-653.
- **Canonical definition and sign (read from the authors' copy, LSE).** Net trade d_i,t = percentage change of the
  shares held by the aggregate 13F institutional portfolio between quarters t-1 and t; each quarter a stock is a net
  buy when d_i,t is above the cross-sectional median and a net sell when below (footnote 5: the same with the sign of
  d, and with the change in shares scaled by shares outstanding). Trade persistence = the number of consecutive
  quarters, up to and including t, in which the stock is a net buy (positive) or a net sell (negative), capped at +-5;
  +-1 is consolidated to 0. "Persistent institutional trading negatively predicts long-term returns: persistently
  sold stocks outperform persistently bought stocks" after about two years; not subsumed by past returns or other
  characteristics; concentrated in stocks below the NYSE size tercile, robust to dropping price < $5 and the smallest
  NYSE decile. Value-weighted DGTW / five-factor returns of 15-22 bp a month (three-quarter persistence) and 19-24 bp
  (four-quarter) for holding periods of two years or more, 1983-2004; 25-50 bp in 1994-2004, insignificant in
  1983-1993. **Sign: long persistently sold, short persistently bought.**
- **Frozen DSL:**
```
rank(decay_linear((((((((max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 252)), 0) + 1) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 189)), 0)) + 1) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 126)), 0)) + 2) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 63)), 0)) * sign((0.5 - rank(inst_own_chg_q)))), 21))
```
- **How it reads.** S = sign(0.5 - rank(inst_own_chg_q)): +1 below the cross-sectional median of the quarter's change
  in institutional ownership (a net sell), -1 above it (a net buy), 0 at it (average-tie ranks are percentiles in
  [0, 1], so the median of an odd valid set is exactly 0.5). c_k = max(S * delay(S, 63k), 0) = 1 when the split 63k
  sessions back is on the same side. Horner form L = c1 (2 + c2 (1 + c3 (1 + c4))) is DPV's run length: 0 for a
  one-quarter run, else 2..5 (a broken c_k stops the run). Signal = L * S = -persistence; house decay; plain rank.
  The Horner order keeps 6 peak slots (a buy chain and a sell chain computed separately needed 9). `xsig_check.py`
  evaluates the string on a synthetic quarterly panel and matches a direct DPV implementation in every cell and in the
  NaN pattern (persistence values seen: -5..-2, 0, 2..5).
- **Constants (registered blind).** One quarter = 63 sessions (252 / 4); runs up to 5 quarters and +-1 -> 0 (paper);
  the median split (paper's main rule); 21 = the house slow form.
- **Fields.** `inst_own_chg_q` (fields v10; W5b formula `13f-asof45-io-chg-q-v1` = inst_own_share(P) -
  inst_own_share(P-1), each at its own quarter-end session; not in the registry fields table: row L2 below). 1 extra
  field.
- **Domain / NaN.** NaN where any of the five quarterly splits is NaN (the field is NaN without consecutive-quarter
  rows or after the 150-day 13F staleness), so a stock needs five quarters of 13F changes. Coverage arithmetic on the
  4-year role (warm-up from 2018-06-01, score from 2020-01-01): the first change is visible about 2018-11 (2018Q3 at
  its 45-day deadline), so the five-quarter chain plus the decay is finite from about 2019-12, before the score window.
- **Deviations.** (1) Net trade = change in the IO fraction (the paper's footnote-5 robustness definition), not the
  percentage change in institutional shares. (2) Quarter lags are 63 sessions, not the exact anchor quarters: calendar
  quarters hold 61-64 sessions, so near a quarter boundary a lag can read the same or the second-previous quarter on
  about 1-3 sessions per quarter (Ruling XSIG-d; the decay smooths it). (3) All 13F filers (as the paper's CDA/Spectrum
  13F data, but today's 13F universe includes the large passive managers, so "institutional buying" includes index
  flows). (4) Continuous rank over the role universe, daily, house decay (paper: persistence portfolios held 1-10
  quarters). (5) A quarter without a 13F row of the security is forward-filled by the field (its change repeats).
- **Orthogonality by construction.** Data source: the multi-quarter path of 13F ownership changes (no member reads
  `inst_own_chg_q`; `inst_best_ideas` reads one quarter's conviction level and `si_low_io` the IO level). Horizon: a
  five-quarter demand path predicting two-year returns; no theme uses a multi-quarter ownership path. Nearest roster
  members by economics (DPV Table I: persistently sold stocks have lower past returns, higher B/M, E/P, CF/P, S/P and
  lower earnings growth): mom_12_1, res_mom_12_1, high_52w -.15 to -.30; value_composite, bm, ep +.15 to +.30;
  ear_mom_12m, earn_surprise_comp -.10 to -.20; inst_best_ideas -.05 to -.15; si_low_io < .10 [all est]. Not a
  re-statement: DPV report the effect survives past returns and valuation controls; its input is ownership changes,
  not prices or accounts. The v7 draft excluded the one-quarter change in IO (Sias-Starks-Titman 2006 + against
  Edelen-Ince-Kadlec 2016 -); this is a different hypothesis with one sign at the long horizon (DPV's abstract itself
  separates the positive short-term one-quarter herding result from the negative long-term persistence result).
- **Prior.** Haircut class: generic 65-75% (published 2011, sample ending 2004, effect concentrated in small stocks,
  no post-2004 test found). Turnover [est]: low (inputs move once a quarter; smoothed).

## 4. The three carried candidates (registration unchanged from the v9 draft, `834d5a05`)

Strings, tiers, themes, citations, deviations and LIB3 rulings (LIB3-a, -c, -d) are exactly the v9 draft's; the SHA
check is in `xsig_check.py`. Only the X-specific ranking argument is added here.

- **`stmom`** (C-1; Medhat and Schmeling 2022, RFS 35(3), 1480-1526). Last month's return within the top tenth of
  last month's turnover inside its return decile; long heavily traded winners, short heavily traded losers.
  Orthogonality: no member conditions on trading activity, and no member uses the last month's return as a
  continuation signal (mom_12_1 and res_mom_12_1 skip it; ind_adj_rev_5 reverses five days). Nearest: ind_adj_rev_5
  -.15 to -.35, mom_12_1 .05-.20 [est]. Why rank 1: the strongest published evidence of the set (+1.37% a month,
  t 4.74, in the top turnover decile; strongest 1991-2018 and among the largest, most liquid stocks; positive in 22
  markets; persistent for 12 months, so the slow book can hold it). Against: monthly turnover in the most crowded
  theme, and library-v7-draft section 4's cost exclusion of turnover-conditioned short-term variants (LIB3-c).
- **`earn_season`** (C-4; Chang, Hartzmark, Solomon and Soltes 2017, RFS 30(1), 281-323). Centred EarnRank of the
  coming fiscal quarter among the last 20 quarters of `ni_q`, while the expected announcement is 0..21 sessions ahead.
  Orthogonality: the seasonal ranking of earnings levels, which no member reads (members use changes and surprises:
  earn_surprise_comp, nincr; or returns: ear, ear_mom_12m); seasonality_same_month is return seasonality. Expected
  abs(rho) < .10 with every member, seasonality_same_month .05-.15 [est]. Why rank 2: published evidence (value-weighted
  55 bp a month, t 3.14) in a thin theme; the payoff arrives at the announcement, when the slow book holds about two
  thirds of the target. Against: in-sample years unverified; the field is defined from about 2019Q4, so early TRAIN is
  sparse.
- **`nt_late`** (C-3; Bartov and Konchitchki 2017, Accounting Horizons 31(4)). Short for 126 sessions after a first NT
  10-K / NT 10-Q. Orthogonality: SEC NT forms, read by no member; ea_overdue and nonreliance_402 .05-.15 [est]. Why rank
  5: well under 1% of names flagged, a short-only sleeve on names that are often hard to borrow, so the marginal
  contribution is small even though the overlap is near zero.

## 5. Registry rows and add-alpha lines (strings frozen)

**L1 themes.** No new theme and no theme text edit (LIB3-a). `filing_events` is the v8.1 theme (E7, text widened by
E-42); `k8_intensity` and `nt_late` join it under its current text. If `filing_events` is not in the registry and the
fitter's theme order at the X freeze (R-7 not run, or `nonreliance_402` withdrawn), Ruling XSIG-a applies.

**L2 fields table** (rows as the v9 rows; root copies `clock` and `basis` from the fields manifest row of the build of
record and checks them against these drafts; `earn_season_rank` and `nt_first_126` come with the draft fields v13 build,
as the v9 draft section 4 says):
```json
"k8_count_63": {"formula_id": "sec-k8-count63-v1", "origin": "fields_v8", "producer": "atx-engine/tools/research_fields_sec.py", "clock": "sec-acceptance-lag1-v1: a source row is usable at role session t iff available_at (UTC, the EDGAR acceptance resolved by atx-db acceptance-per-file-clock-v1) < 22:00 UTC of session t-1 on the session calendar (role sessions inside the role range, the NYSE rule calendar nyse-rule-v1 outside it); rows available on or after 2024-01-01 are dropped; sessions are counted on the same calendar", "basis": "distinct original 8-K accessions of the CIK that became usable within the last 63 sessions (usable from session e: counted at t for e <= t < e + 63; a filing is usable from the session after its event session, the first session whose 22:00 UTC mark follows its acceptance); NaN unless the CIK has a visible original 8-K with available_at within 365 days before the 22:00 UTC mark of t-1 (formula id sec-k8-count63-v1; producer atx-engine/tools/research_fields_sec.py, fields-v8)"},
"inst_own_chg_q": {"formula_id": "13f-asof45-io-chg-q-v1", "origin": "fields_v9", "producer": "atx-engine/tools/research_fields_holdings.py", "clock": "13f-quarter-asof45-v1: the quarter P becomes visible at V(P) = max available_at (stage 13f-filed-plus-46h-v1: filing_date 00:00 UTC + 46 h) over every 13F filing of P filed by its deadline (effective or not, notices included), uniform across securities; session t reads the latest quarter P with V(P) < date(t-1) 22:00 UTC and date(t) - P <= 150 days in which the security has an agg_asof45 row (forward fill across a quarter without a row); every 13F field of the session reads that same anchor quarter (a NaN there stays NaN, no per-field skip-back)", "basis": "inst_own_share(P) - inst_own_share(P-1) for the anchor quarter P and the preceding calendar quarter P-1, each at its own quarter-end session (share-basis invariant); NaN when either is NaN (formula id 13f-asof45-io-chg-q-v1; producer atx-engine/tools/research_fields_holdings.py, fields-v9)"}
```

**L3 alphas** (written by the add-alpha lines below; `added_in` = the X library name):
```json
{"id": "k8_intensity", "dsl": "rank(decay_linear((-1 * (((k8_count_63 + delay(k8_count_63, 63)) + delay(k8_count_63, 126)) + delay(k8_count_63, 189))), 21))", "theme": "filing_events", "tier": "C+", "prior_sign": 1, "citation": "Zhao (2017, Management Science 63(5)) Does information intensity matter for stock returns? Evidence from Form 8-K filings", "prior_sign_source": "Zhao 2017", "form": "R(decay_linear(x, 21))", "origin": "prior", "notes": {"formula": "-(number of original 8-K filings usable in the last 252 sessions) = -(k8_count_63 + its delays by 63, 126 and 189 sessions: four disjoint 63-session blocks); long low information intensity", "domain": "NaN unless the CIK is an 8-K filer (a visible original 8-K within 365 days) at t, t-63, t-126 and t-189; foreign private issuers (6-K) are NaN", "deviation": "rolling 252 sessions daily with the house decay (paper: fixed-window sorts, monthly; its exact counting window unverified); original 8-K family forms, co-registrant filings counted for each registrant, earnings 8-Ks included; no size control inside the signal (the book's price-risk-v1 projection carries it)"}, "added_in": "<X name>"}
{"id": "inst_persist", "dsl": "rank(decay_linear((((((((max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 252)), 0) + 1) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 189)), 0)) + 1) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 126)), 0)) + 2) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 63)), 0)) * sign((0.5 - rank(inst_own_chg_q)))), 21))", "theme": "reversal_seasonality", "tier": "C+", "prior_sign": 1, "citation": "Dasgupta, Prat and Verardo (2011, JF 66(2)) Institutional trade persistence and long-term equity returns", "prior_sign_source": "Dasgupta-Prat-Verardo 2011", "form": "R(decay_linear(x, 21))", "origin": "prior", "notes": {"formula": "-DPV trade persistence: net sell / buy = below / above the cross-sectional median of the quarter's change in 13F ownership (inst_own_chg_q); persistence = consecutive same-side quarters including the current one at 63-session lags, capped at 5, a one-quarter run = 0; long persistently sold, short persistently bought", "domain": "NaN when any of the five quarterly splits is NaN (no consecutive-quarter 13F rows, 150-day staleness); 0 at the median and for one-quarter runs", "deviation": "net trade = change in the IO fraction (the paper's robustness definition), not the percentage change in institutional shares; quarter lags of 63 sessions, not exact anchor quarters (1-3 sessions per quarter boundary can read the same or the second-previous quarter); all 13F filers including passive managers; continuous rank, daily, house decay (paper: persistence portfolios held 1-10 quarters)"}, "added_in": "<X name>"}
```

**add-alpha lines.** `PY="C:/Program Files/Python312/python.exe"`. Run after L2, in this order (the rank order).
Substitutions allowed: `<X parent>`, `<X name>`, `<X parent spec>`, `<X fields dir>` (the build that carries fields
v10 plus the draft v13 fields); every other byte is frozen. The first, second and fifth lines are the v9 draft section 4
lines with only those substitutions.
```bash
"$PY" scripts/research_cycle.py add-alpha --id stmom --dsl "rank(decay_linear(((group_rank((ts_sum(volume, 21) / shares_out), bucket(((close / delay(close, 21)) - 1), 10)) > 0.9) ? (rank(((close / delay(close, 21)) - 1)) - 0.5) : 0), 21))" --theme price_momentum --tier B- --prior-sign 1 --citation "Medhat and Schmeling (2022, RFS 35(3)) Short-term Momentum" --origin prior --prior-sign-source "Medhat-Schmeling 2022" --form "R(decay_linear(x, 21))" --formula "centred rank of the 21-session return for names in the top tenth of 21-session turnover within their 21-session-return decile (bucket), 0 elsewhere; long heavily traded winners, short heavily traded losers" --domain "NaN where the 21-session return or turnover is NaN; non-selected names are 0 (neutral)" --deviation "role breakpoints, not NYSE; continuous rank inside the high-turnover set, not the decile corner; rolling 21 sessions with house decay, not calendar months; turnover = raw volume / 90-day-lagged shares_out (split inside the window distorts); equal-weighted rank" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
"$PY" scripts/research_cycle.py add-alpha --id earn_season --dsl "rank((((ea_days_to_expected >= 0) && (ea_days_to_expected <= 21)) ? (earn_season_rank - 10.5) : 0))" --theme reversal_seasonality --tier B- --prior-sign 1 --citation "Chang, Hartzmark, Solomon and Soltes (2017, RFS 30(1)) Being surprised by the unsurprising: earnings seasonality and stock returns" --origin prior --prior-sign-source "Chang-Hartzmark-Solomon-Soltes 2017" --form "R(x)" --formula "centred EarnRank (mean ascending rank of the same fiscal quarter among quarters U-23..U-4 of ni_q) while the expected announcement is 0..21 sessions ahead, else 0; long historically high quarters" --domain "NaN where the calendar or the 20-quarter history is NaN; 0 outside the window" --deviation "net income, not split-adjusted EPS; house expected date and a 0..21-session window, not the predicted calendar month; no decay" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
"$PY" scripts/research_cycle.py add-alpha --id k8_intensity --dsl "rank(decay_linear((-1 * (((k8_count_63 + delay(k8_count_63, 63)) + delay(k8_count_63, 126)) + delay(k8_count_63, 189))), 21))" --theme filing_events --tier C+ --prior-sign 1 --citation "Zhao (2017, Management Science 63(5)) Does information intensity matter for stock returns? Evidence from Form 8-K filings" --origin prior --prior-sign-source "Zhao 2017" --form "R(decay_linear(x, 21))" --formula "-(number of original 8-K filings usable in the last 252 sessions) = -(k8_count_63 + its delays by 63, 126 and 189 sessions: four disjoint 63-session blocks); long low information intensity" --domain "NaN unless the CIK is an 8-K filer (a visible original 8-K within 365 days) at t, t-63, t-126 and t-189; foreign private issuers (6-K) are NaN" --deviation "rolling 252 sessions daily with the house decay (paper: fixed-window sorts, monthly; its exact counting window unverified); original 8-K family forms, co-registrant filings counted for each registrant, earnings 8-Ks included; no size control inside the signal (the book's price-risk-v1 projection carries it)" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
"$PY" scripts/research_cycle.py add-alpha --id inst_persist --dsl "rank(decay_linear((((((((max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 252)), 0) + 1) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 189)), 0)) + 1) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 126)), 0)) + 2) * max((sign((0.5 - rank(inst_own_chg_q))) * delay(sign((0.5 - rank(inst_own_chg_q))), 63)), 0)) * sign((0.5 - rank(inst_own_chg_q)))), 21))" --theme reversal_seasonality --tier C+ --prior-sign 1 --citation "Dasgupta, Prat and Verardo (2011, JF 66(2)) Institutional trade persistence and long-term equity returns" --origin prior --prior-sign-source "Dasgupta-Prat-Verardo 2011" --form "R(decay_linear(x, 21))" --formula "-DPV trade persistence: net sell / buy = below / above the cross-sectional median of the quarter's change in 13F ownership (inst_own_chg_q); persistence = consecutive same-side quarters including the current one at 63-session lags, capped at 5, a one-quarter run = 0; long persistently sold, short persistently bought" --domain "NaN when any of the five quarterly splits is NaN (no consecutive-quarter 13F rows, 150-day staleness); 0 at the median and for one-quarter runs" --deviation "net trade = change in the IO fraction (the paper's robustness definition), not the percentage change in institutional shares; quarter lags of 63 sessions, not exact anchor quarters (1-3 sessions per quarter boundary can read the same or the second-previous quarter); all 13F filers including passive managers; continuous rank, daily, house decay (paper: persistence portfolios held 1-10 quarters)" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
"$PY" scripts/research_cycle.py add-alpha --id nt_late --dsl "rank((-1 * nt_first_126))" --theme filing_events --tier B- --prior-sign 1 --citation "Bartov and Konchitchki (2017, Accounting Horizons 31(4)) SEC filings, regulatory deadlines, and capital market consequences" --origin prior --prior-sign-source "Bartov-Konchitchki 2017" --form "R(x)" --formula "-1{a first NT 10-K or NT 10-Q (none in the prior 365 days) became usable within the last 126 sessions} (nt_first_126)" --domain "NaN for non-domestic or absent periodic filers; a binary flag: flagged names tie at the bottom" --deviation "126 sessions, not the paper's months; NT 10-K and NT 10-Q pooled; continuous rank of a flag" --parent <X parent> --name <X name> --parent-spec <X parent spec> --fields <X fields dir>
```

Expected K1 rows (`--plan-only` at add-alpha): stmom 41 bars, 5 slots, extra `shares_out`; earn_season 0 / 5,
`ea_days_to_expected`, `earn_season_rank`; k8_intensity 209 / 5, `k8_count_63`; inst_persist 272 / 6, `inst_own_chg_q`;
nt_late 0 / 3, `nt_first_126`. A K1 row that differs is reported; a string K1 refuses is rewritten only mechanically
(same semantics) or withdrawn at 0 trials.

**Withdrawal rule.** A candidate whose field is absent from the X fields build is withdrawn at 0 trials: earn_season
and nt_late need the draft fields v13 (`prepare_research_fields_draft.py`, FIELDS-V9 report section 3, after the
v8 freeze gate); k8_intensity and inst_persist need only fields v10 and their L2 rows; stmom needs nothing new.

**Trial accounting.** Up to 5 admission trials in the X budget (fixed in `v8x-prereg.md`, lane XPRE). Registering the
three carried candidates in X spends their trials once; the v9 wave must not register them again (Ruling XSIG-e).

## 6. Considered and not included (each examined blind)

Fields in fields v10 that no roster member reads, and why each is not a candidate:

| field | reason |
|---|---|
| `ins_cluster_buy`, `ins_net_buy_ratio`, `ins_n_buyers`, `ins_n_sellers` | Form 4, the source of `ins_opp`; library-v7-draft section 4: a second insider variant of one hypothesis (Cohen-Malloy-Pomorski: routine trades are noise); the cluster flag also lives 21 sessions, too fast for the book |
| `inst_breadth_chg` | published signs conflict (Chen-Hong-Stein 2002 + against Lehavy-Sloan 2008 -); withdrawn in v7 at 0 trials |
| `inst_own_chg_q` (one quarter) | v7: Sias-Starks-Titman + against Edelen-Ince-Kadlec -. Used here only in DPV's multi-quarter form, which has one sign |
| `inst_n_holders` | investor recognition (Merton 1987; Bodnaruk-Ostberg 2009) is a holder count that mostly measures size, which ladv63 projects out; no clean US large-cap sign |
| `k8_item_material_21`, `k8_days_since_any` | pooled items / time since any 8-K: no canonical definition or sign (v7: conditioning only) |
| `ea_window_pre5` | earnings-announcement premium: reported gone in the US (Heitz-Narayanamoorthy-Zekhnini, WP), and a 5-session window the book cannot trade (v7: no EAP variant faster than a month) |
| `ea_delay_days` | Johnson-So 2018: priced at the announcement, no drift prior (v7); ea_overdue holds the pre-announcement state |
| `ea_time_of_day`, `ea_window_post3` | an interaction with `ear` (DellaVigna-Pollet 2009), not a member; a window flag without a hypothesis |
| `regsho_threshold_days63` | persistent fails: the fails-to-deliver family of `ftd_fail` (v7: conditioning only) |
| `sv_offexchange_share126` | FINRA short volume over consolidated volume: nearly `sv_ratio126`'s numerator; no signed prior (v7) |
| `iv_atm_63d`, `iv_atm_126d` | IV term structure and level are measured hypotheses (`pv_fields_ic121_v3`: iv_term_63_21, iv_term_126_21, iv_level_21; atx-db iv_term_slope): prior exposure (v9 draft section 0) |
| `vol_126` | volume / turnover level: plan section 13 |
| `xrd0_ttm`, `lt`, `noa_lag4`, `grp_sic2` | fundamental variants of value / investment members (same source, same horizon): not orthogonal by construction; grouping refinements are lane XIMP's |
| `ret_overnight`, `ret_intraday` (other forms) | close-to-close signs conflict (Lou-Polk-Skouras overnight momentum against Aboody et al. 2018 reversal; night_day withdrawn in v8); intraday-return momentum (Barardehi-Bogousslavsky-Muravyev) is a refinement of mom_12_1 (XIMP) |
| `ceq_iss_5y`, `coskew_60m`, `k8_item402_63`, `exch_up_365d` | R-7 / R-12 |

Families with a published basis that the in-house data could support, rejected on the quality bar:

| family | reason |
|---|---|
| `ind_leadlag` (v9 C-2, Hou 2007) | C+: no post-2004 large-cap test, SIC-peer effects "small, short-lived" (Hoberg-Phillips 2018); a one-month undecayed signal at the 7-slot limit; a practitioner would not expect it to add to this book |
| v9 NEEDS-DATA (lazy_prices, tnic_mom, conn_rev, fund_fit, tax_book, iv_skew) | need data not in house (lane XDATA) |
| Dividend-month premium (Hartzmark-Solomon 2013, JFE) | a one-month calendar effect: at trade fraction .05 the book reaches about two thirds of the target only at the month's end [arithmetic] |
| Pre-announcement reversal (So-Wang 2014, JFE); short-sale flow (Diether-Lee-Werner 2009) | 2-5 session horizons, untradeable at the book's speed; the second is the sv family |
| News versus no-news drift (Chan 2003, JFE) with the 8-K as the news clock | overlaps stmom's mechanism (information-heavy months continue); an 8-K is a coarse news indicator (most firms file in most months) |
| Short-term institutions' ownership (Yan-Zhang 2009, RFS) | a churn classification of 13F filers is a heavy new builder for a 1980-2003 result with no later test; candidate for a later wave if XDATA ranks it |
| Industry concentration (Hou-Robinson 2006, JF) | HHI over the top-3,000 universe only measures concentration among listed large firms; C+ |
| R&D increases (Eberhart-Maxwell-Siddique 2004, JF) | fundamentals at the value / profitability horizon, near rd_me and op_rd |

## 7. Rulings root needs before the X freeze (decision -- why -- cost if wrong)

- **XSIG-a (filing_events):** `k8_intensity` and `nt_late` join `filing_events` under its E-42 text; if the theme is
  not in the registry and in the fitter's theme order (`V7_APPENDED_THEMES`, finding R6B-O-2) at the X freeze, root
  adds it with the E-42 text before the first add-alpha -- SEC filing behaviour is the family both read, and
  `k8_intensity` gives the theme the cross-sectional breadth its sparse flags lack (R7-b) -- cost if wrong: one more
  theme in T (each theme's share 1 / T shrinks).
- **XSIG-b (inst_persist's theme):** `reversal_seasonality`, by mechanism (a reversal of institutional demand
  pressure), not `ownership_flow`, whose text says informed buying predicts *higher* returns while DPV's persistent
  buying predicts *lower* ones -- themes group mechanisms (si_low_io reads 13F and sits in short_interest) -- cost if
  wrong: the member is grouped away from its data family.
- **XSIG-c (roster cap):** worst case v8.0 52 + v8.1 5 + v8.2 3 + X 5 = 65 > 64 (R7-a cap) -- mechanical -- cost if
  wrong: none for inference; raise the cap with the X library or register fewer (R-12 runs only if R-6 is accepted;
  without it the worst case is 62).
- **XSIG-d (inst_persist's quarter lags):** 63-session lags in the DSL (READY on fields v10, no build) rather than an
  exact anchor-quarter field -- one variant, chosen blind; a 13F producer change would move the `13f` fingerprint or
  need a new kind -- cost if wrong: on about 1-3 sessions per quarter boundary a run is one quarter too long or too
  short, smoothed by the 21-session decay.
- **XSIG-e (carried registrations):** stmom, earn_season and nt_late are registered in X with the v9 draft's frozen
  strings and LIB3's rulings (LIB3-a, -c, -d); the v9 wave does not register them again -- one hypothesis, one trial --
  cost if wrong: a double count in the trial ledger.
- **XSIG-f (turnover):** stmom and earn_season add turnover [est: monthly and quarterly position changes]; if
  `v8x-prereg.md` keeps "book turnover not higher" as a wave criterion (as R-7 / R-12), they are judged with the wave
  -- XPRE decides -- cost if wrong: a wave rejected for two fast members.

## 8. How root verifies

1. `"C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-v8-20260929/xsig_check.py` from the repository
   root prints the calibration line, the five candidate rows (bars, slots, nodes, bytes, extra fields, registry field
   rows needed, full SHA-256) and `xsig_check: PASS`. It reads only `registry.cpp`, `registry.json`,
   `libraries/v80.json` and `scripts/specs/v8/lib-v80.json`; the semantic checks use seeded synthetic panels.
2. K1 at add-alpha must print the rows in section 5.
3. Identity: this lane changed no code path: `git diff --stat 3c6ae225..HEAD` lists only
   `.superpowers/sdd/platform-v8-20260929/xsig_check.py` and this report, so every accepted output is byte-identical
   by construction.

## 9. Hygiene (what was read)

- Read: lane-rules.md; task-X-briefs.md (rules for every X lane, Lane XSIG); status 6 sections 1-2; v8-prereg.md;
  progress.md "PM session 7"; library-v8-draft.md (all, including sections 0, 4, 5, 8, 10); task-LIB2-report.md;
  task-F-3-report.md; `scripts/specs/v8/lib-v80.json` (fields v10 list, gate, inputs); the registry (themes, fields,
  the 52 v8.0 member strings); `libraries/v80.json`; the v9 draft and the LIB3 report on `834d5a05` (via `git show`) and
  the v9 field module's stage reads; `registry.cpp` (operator table), `bytecode.cpp`, `dag.cpp`, `typecheck.hpp`,
  `cs_ops.hpp` comments; `op_catalog.cpp`; the field producers' specs in `research_fields_sec.py` and
  `research_fields_holdings.py` (definitions and code, no payload); library-v7-draft section 4; the DSL ids of the three
  pv libraries (no result); `fit_composition_weights.py` theme lists; `research_add_alpha.py` usage.
- Opened under `C:/atx-wt/pool-2/build-equity/`: only `train-2020-2023-lo3/manifest.json`, for its date fields
  (`warmup_start` 2018-06-01, `score_begin` 399, 1,405 dates). Nothing else there (no NAV, card, marginal, admission or
  diagnostics file). No payload of any kind.
- Web (definitions, signs, samples and published statistics only): Zhao (2017) abstract (IDEAS / RePEc, INFORMS
  search summaries); Dasgupta-Prat-Verardo (2011) authors' copy (LSE). Both samples end before 2024 (2009, 2004).
- **Disclosures (hidden-data rule).** (1) The registry's `ea_overdue` notes print TRAIN coverage figures (share of
  flagged cells, a 2020-05-01 peak share), and a grep of the v7 W5a / W5b reports printed field coverage shares: no
  return, IC, Sharpe or turnover statistic; seen, used for nothing. (2) The session scratchpad is shared with other
  lanes and holds their run logs; I listed its file names once and opened only files I wrote. (3) No statistic dated
  2024-01-01 or later was read; nothing was ranked by anything measured on platform data.

## Cross-lane edits

None. No file outside `.superpowers/sdd/platform-v8-20260929/` changed.

## Deviations from the brief

- 5 candidates, not up to 12: the quality bar (a practitioner must expect a candidate to add to the v8.0 book) left no
  more in the in-house data (section 6).
- No new field builder: both new candidates read fields already in fields v10; the carried ones read built fields.
  Hence no new fields-spec entry and no field tests; the lane's tests are the static and semantic checks of the
  strings (`xsig_check.py`).

## Open risks

- `k8_intensity` rests on one study (sample ending 2009, no replication found); 8-K frequency is partly a size and
  corporate-activity proxy, so after the book's projections its residual may be small; the long leg leans to more
  volatile names.
- `inst_persist`'s published effect sits in small stocks and in 1994-2004; today's 13F universe is dominated by passive
  managers, whose flows make "institutional buying" partly mechanical; it overlaps value (+) and momentum (-) [est].
- `earn_season` and `nt_late` depend on the draft fields v13 build after the freeze gate; their fields are unrun on
  data (FIELDS-V9 report).
- `stmom` carries the house cost precedent (LIB3-c) and raises turnover.
- The static figures are the mirror's; K1 at add-alpha decides.
