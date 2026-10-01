# Task LIB2 report: library v8.2 candidates for the three remaining admission trials (Ruling E-38)

Lane LIB2, pool 8, branch `feat/platform-v8-lib2-20260930` from `fd2ff7a8`. Python only; synthetic tests only.
Declared 2026-09-30 before any IC, TRAIN, return or NAV read of any candidate. This file is the registration of R-12's
screen set (Ruling E-38: slot 51, run only if R-6 is accepted; the three remaining admission trials, 13-15 of 15).

## Status

| item | status | commit |
|---|---|---|
| Field `exch_up_365d` (holdings kind `xsw`) + tests | DONE | `b63edb31` |
| Registration (this report: candidates, registry rows, add-alpha lines, fields v12) | DONE | this commit |

Tests: `test_research_fields_holdings_xsw.py` 10 passed; the field suite (F3's 13 files + the new one) 158 passed, 6
subtests; all of `atx-engine/tools` 251 passed, 6 subtests (`"C:/Program Files/Python312/python.exe" -m pytest -q -p
no:cacheprovider .` in `atx-engine/tools`).

## 1. The three candidates (registration)

Chosen blind under library-v8-draft section 0 (canonical definition, literature sign embedded, prior_sign +1, one DSL
string, tier, citation), preferring the themes with the fewest members after v8.1 (options_implied 1, reversal_seasonality
2, ownership_flow 2; filing_events 0-1) and inputs the atx-db alpha panel docs show as available (`iv_atm_*`, `open` /
`ret_overnight` / `ret_intraday`, `exchange` from the FINRA security master: ALPHA_PANEL.md stages P and X,
ALPHA_PANEL_METRICS.md section 1). Each uses a data source and horizon no roster member uses.

| order | id | theme | tier | sign | data source | horizon | bars | slots | nodes | fields | DSL sha256 (16) |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `iv_vol_of_vol` | options_implied (1 member) | B- | +1 (long low VOV) | vendor ATM implied vol `iv_atm_21d` (fields v4) | 21-session IV path | 40 | 5 | 13 | 1 | `c5ecec15fbdb4807` |
| 2 | `day_rev_freq` | reversal_seasonality (2) | B- | +1 (long frequent reversals) | vendor open: `ret_overnight`, `ret_intraday` (F-1, fields v10) | 21-session count | 40 | 4 | 12 | 2 | `a5416c4ea13d4422` |
| 3 | `exch_switch` | filing_events (0-1; text widened to filing and listing events, Ruling E-42) | C+ | +1 (short recent up-listers) | FINRA listing market (security master, `exch_up_365d`, fields v12) | 365-day event | 0 | 3 | 4 | 1 | `f433b32af008e74c` |

Roster order (admission tie-break), appended after the parent's members: iv_vol_of_vol, day_rev_freq, exch_switch.
Static figures from an offline mirror of the compiler (parse order, literal CSE, hparam peeling, post-order slot
retirement) that reproduces every row of library-v8-draft section 4 (bars, slots, extra fields, DSL SHA, stated node
counts: 12 of 12). All PASS the house budget (bars <= 314, runner <= 336; slots <= 7; extra fields <= 5; DSL <= 4,096 B)
with no exception. K1 (`--plan-only` through add-alpha) is the checker of record (R2-f); a K1 row that differs is
reported, a refused string is withdrawn.

### C-1 `iv_vol_of_vol`: vol-of-vol (uncertainty about risk)

- **Registration.** options_implied; tier B-; prior sign +1; origin `prior`; 1 trial. Citation: Baltussen, van Bekkum
  and van der Grient (2018, JFQA 53(4), 1615-1651) "Unknown unknowns: uncertainty about risk and stock returns".
- **Paper (definition read from the paper, section "Vol-of-Vol Measure and Data").** VOV_1M(i, t) = population standard
  deviation of daily ATM IV over days t-19..t divided by the mean ATM IV over the same days; IV = average of the call and
  put closest to ATM (OptionMetrics); at least 12 non-missing days; monthly horizon. Sign: high vol-of-vol stocks
  underperform low vol-of-vol stocks (about 8% a year, abstract; US 1996-2014 and European markets), distinct from at least
  20 known predictors.
- **Final DSL:**
```
rank(decay_linear(((-1 * (ts_std_mp(iv_atm_21d, 21, 12) / ts_mean_mp(iv_atm_21d, 21, 12))) + (0 * log(ts_std_mp(iv_atm_21d, 21, 12)))), 21))
```
- **How it reads.** `ts_std_mp(., 21, 12)`: sample std of the finite IV cells among the last 21 sessions, NaN with fewer
  than 12 (`lit_ops.hpp:35`); `ts_mean_mp` the same count rule. `iv_atm_21d` is already NaN outside [0.02, 5] (producer
  domain), so the mean is positive. `+ (0 * log(s))` (house domain guard, as R2-g) makes a flat window (s = 0) NaN.
- **Deviations.** (1) 21 sessions with 12 required (paper 20 days, 12 required; house windows). (2) Sample std (ddof 1)
  where the paper uses 1/20: a constant factor on full windows (rank-identical), up to about 2% relative with 12-20 cells.
  (3) Vendor 21-session constant-maturity clean ATM IV (ORATS via TickerHistory3) instead of OptionMetrics' nearest-ATM
  call/put average. (4) Flat-IV windows NaN (a stale vendor series is not zero uncertainty; the paper has no such rule).
  (5) House 21-session decay on a monthly measure (paper: monthly rebalance). (6) Top 3,000 by dollar volume.
- **Prior.** Haircut class: generic 65-75% (published 2018, under ten years out of sample). Expected abs(rho) [est]:
  iv_rv_spread .10-.25, smax / smax5 / bac .10-.30, qmj_safety .10-.20 (VOV is scaled by the IV level, so it is not a
  volatility level). Turnover [est]: low-mid. Sits beside iv_rv_spread (the theme's only member).
- **Not the platform's earlier IV grid.** pv_fields_ic121_v3 tested IV level, IV term ratio, IV change and IV-RV; the
  atx-db characteristics (iv_term_slope, iv_change_21, iv_rv_ratio) were IC-screened by atx-db. VOV is in neither.

### C-2 `day_rev_freq`: frequency of positive daytime reversals ("tug of war")

- **Registration.** reversal_seasonality; tier B-; prior sign +1; origin `prior`; 1 trial. Citation: Akbas, Boehmer,
  Jiang and Koch (2022, JFE 145(3), 850-875) "Overnight returns, daytime reversals, and future stock returns".
- **Paper.** A higher frequency within a month of positive overnight returns followed by negative trading-day returns
  (a persistent tug of war between overnight noise traders and daytime arbitrageurs, who overcorrect) predicts higher
  future returns; the predictability arrives mostly in the next month's overnight returns; the mirror pattern (negative
  overnight, positive daytime) has no predictive relation. This is the canonical-signed frequency form that the draft's
  R7-5 names; it is a different hypothesis from the withdrawn `night_day` (a return difference with no agreed sign).
- **Final DSL:**
```
rank(decay_linear(ts_mean_mp((((ret_overnight > 0) && (ret_intraday < 0)) ? 1 : 0), 21, 15), 21))
```
- **How it reads.** `ret_overnight[t]` = adjusted open of session t-1 over the adjusted close of t-2, minus 1;
  `ret_intraday[t]` = close over open of session t-1 (F-1 fields, `price-close-lag1-v1`): the pair is one session. A
  compare is NaN on a NaN operand and `&&` is NaN on a NaN operand (`vm.hpp:52-60`), so a session counts 1, 0 or not at
  all; Select gives 1/0 (`typecheck.cpp:200-235`: compare F64 -> Mask, `&&` Mask x Mask -> Mask, Select(Mask, F64, F64)).
  `ts_mean_mp(., 21, 15)`: share of the finite sessions, NaN with fewer than 15.
- **Deviations.** (1) Rolling 21 sessions, not the calendar month; share of valid sessions (count / n; rank-identical
  to the count on full windows). (2) Vendor open (TickerHistory3), not the opening auction print; the overnight return
  carries a factor step dated t-1. (3) One-session lag from the field clock. (4) House 21-session decay. (5) Min 15
  valid sessions (declared; the AHXZ month-of-daily-data convention).
- **Data.** Needs `ret_overnight`, `ret_intraday` in fields v11 (F-1 opt-in; ALPHA_PANEL.md: TickerHistory3 carries
  `open`). If the v10/v11 build stops with `FieldNeedsOpen`, `day_rev_freq` is withdrawn at 0 trials (withdrawal rule).
- **Prior.** Haircut class: recently published (2022), under ten years out of sample: the 35-40% in-sample to post-sample
  step plus decay. Expected abs(rho) [est]: ind_adj_rev_5 .05-.20, smax / smax5 .05-.20, mom_12_1 and fip_id < .15.
  Turnover [est]: mid. Sits beside ind_adj_rev_5 and seasonality_same_month.

### C-3 `exch_switch`: post-listing drift after a move up to NYSE / NYSE American

- **Registration.** Theme `filing_events` (Ruling E-42: joins the existing theme, its text widened to filing and
  listing events; no new theme; integration 6 part A edit, the lane registered a new theme `listing_events`); tier C+;
  prior sign +1; origin `prior`; 1 trial. Citation: Dharan and
  Ikenberry (1995, JF 50(5)) "The long-run negative drift of post-listing stock returns"; Chen-Zimmermann `ExchSwitch`.
- **Paper (library-v8-draft section 10 "not included"; lit notes F15).** Indicator for a move from AMEX or Nasdaq to
  NYSE, or Nasdaq to AMEX, within the past year; short. Original .46%/month, t 3.61, about 3,000 events 1962-1990.
- **Final DSL (event-flag form R(x), as ea_overdue, nonreliance_402):**
```
rank((-1 * exch_up_365d))
```
- **Field.** `exch_up_365d` (formula `finra-listing-up-switch365-v1`, built in this lane, section 2).
- **Deviations.** (1) Listing market from FINRA short-interest rows at their semi-monthly dissemination, so a move shows
  up to about 15 days late and is dated by the dissemination, not CRSP exchange codes. (2) 365 calendar days. (3) A move
  that keeps the vendor line counts, including a de-SPAC moving from Nasdaq to NYSE; one with a new vendor line is not
  seen. (4) Moves involving NYSE Arca, Cboe BZX or OTC are not events. (5) Continuous rank over the role universe: the
  flagged names tie at the bottom, every other name ties above (as nonreliance_402).
- **Prior.** Lit notes (CZ-own, external, gross, monthly; none is a statistic of this platform's data): 2005-24 ex-microcap
  .71%/month (t 2.48), value-weighted .61 (t 2.06); spanning alpha .61 (t 2.18) with R2 .11, the lowest of every buildable
  candidate the notes list (orthogonality rank 1 with the filing events). Haircut class: generic 65-75%; few events, a
  short-only sleeve, likely hard-to-borrow names. Expected abs(rho) < .10 with every member [est]; flagged share about
  1-2% of names [est].
- **Theme risk (as R7-b).** A one-member theme under `ew-theme-std-v1` is re-ranked and capped at 1 / (2T); it adds a
  theme to T. Ruling LIB2-a below. Ruled by E-42: no new theme; `exch_switch` joins `filing_events` (E7, v8.1:
  `nonreliance_402`), so no theme is added to T.

## 2. Field `exch_up_365d` (commit `b63edb31`)

`atx-engine/tools/research_fields_holdings.py`, new opt-in kind `xsw` (the holdings module already pins the
`security_master` stage for `regsho_threshold_days63`, so no new option and no atx-db change):

- `KIND_STAGES["xsw"] = ("security_master",)`, `PRODUCERS["xsw"] = ("build_xsw",)`, `KIND_CHECK_KEY["xsw"] =
  "listing_switch"`; declared constants `XSW_WINDOW_DAYS = 365`, `XSW_MAX_GAP_DAYS = 45` (= REGSHO_NAME_MAX_AGE_DAYS),
  `XSW_VENUE_OF_CLASS` (NNM/SC -> nasdaq, AMEX -> amex, NYSE -> nyse, anything else other), `XSW_UP`; rule text
  `XSW_RULE` (`finra-listing-up-switch-v1`); spec `HOLD_FIELDS["exch_up_365d"]`.
- `xsw_events(col, dd, ven)`: the rule over rows in visibility order (available_at, dissemination_date, venue, file
  order): first row sets (date, venue); a later-dated row advances and is an up-switch when the move is in `XSW_UP` and
  the rows are at most 45 days apart; a same-date row with another venue makes the venue other (conflict); an older-dated
  row is ignored (out of order). Returns per-row flags, the state after each row, counts.
- `build_xsw(ctx, names, stage)`: reads `finra_names.parquet` through the pinned Stage; drops rows with
  `available_at >= SEAL_NS` (research_window seal, counted) and off-role or unmapped lines; streams sessions: rows with
  `available_at < date(t-1) 22:00 UTC` are applied; value = NaN when the line's latest visible row is more than 45 days
  old (market unknown) or the visible rows do not reach 410 days back (short history; session 0 NaN), else
  1{date(t) - latest up-switch date <= 365}. Checks: `source_checks.holdings.listing_switch` (rows, rows_sealed,
  rows_on_role, outcome counts, flagged member cells, lines with an up-switch); entry extras `nan_reasons_member_cells`.
- `build_all` dispatches the kind (orchestration, outside every producer closure).
- Point in time: every event is decided from rows visible at its own arrival and never revised; a value at t reads
  only rows with `available_at < date(t-1) 22:00 UTC`.

Tests `atx-engine/tools/test_research_fields_holdings_xsw.py` (synthetic stage 2019-05..2021-08, role 2020-03..2021-08;
no row dated 2024 or later):
- the rule on hand-made sequences (switch, down move, 45 vs 46-day gap, conflict, out of order, other venues);
- every cell against an independent per-session replay, plus planted cells: 0 -> 1 at the first session that sees the
  switch, 1 -> 0 after 365 days, a 77-day gap is no switch and is NaN while stale, a late republished row makes the
  switch visible on the next row's date, the conflict and the late row give no event, Arca is no event, stale -> NaN;
- look-ahead probe: every row at or after the t*-1 mark mutated and new switch rows added (one exactly at the mark):
  rows 0..t* bit-identical, later rows differ; the same probe catches a one-session look-ahead injected into
  `prev_marks` (rows 0..t* then differ);
- seal: a patched `SEAL_NS` inside the fixture equals the world without the later rows and the replay at that seal, and
  differs from the unsealed output; `hold.SEAL == research_window.SEAL`;
- reuse: a self `--reuse` copies the field byte for byte (`reused_from.inputs` = the stage pin); another
  security_master pin recomputes it; fingerprints: an edit in `xsw_events`, `build_xsw` or `XSW_WINDOW_DAYS` moves only
  `xsw`, an edit in `build_regsho` or `REGSHO_NAME_MAX_AGE_DAYS` moves only `regsho`;
- refusals (missing stage, wrong pin, no output), the CLI, manifest entry and checks.

## 3. Registry edits (root applies before add-alpha)

**L1 themes** (Ruling E-42, integration 6 part A edit: no new theme; the existing `filing_events` text of E7 is widened
to filing and listing events; the fitter reads the registry's themes table, `fit_composition_weights.prior_themes`, and
its theme list takes `filing_events` under E7, so v8.2 adds no theme to either). The lane's new `listing_events` row is
withdrawn. Replacement text for the `filing_events` row:
```json
"filing_events": "Adverse filing and listing events: an 8-K Item 4.02 non-reliance disclosure within the last 63 sessions, or a move of the listing up to NYSE or NYSE American (from Nasdaq or NYSE American) within the last 365 days; a recent event predicts lower returns."
```

**L2 fields table** (rows as the v9 rows; `ret_*` after the F-1 merge and fields v10, as E6; `exch_up_365d` after
fields v12):
```json
"ret_overnight": {"formula_id": "price-ret-overnight-lag1-v1", "origin": "fields_v10", "producer": "atx-engine/tools/research_fields_price.py", "clock": "price-close-lag1-v1: row t reads vendor observations dated on or before session t-1 only, each known at its 22:00 UTC close mark", "basis": "adjusted open of session t-1 over the adjusted close of session t-2, minus 1 (vendor TickerHistory3 open and close, cumulReturnFactor chained by factor-break-v1); NaN without a finite positive open, observations at both sessions, or inside a kept_gap step, or when |log| > 1.5 or > |log raw| + 0.10 (formula id price-ret-overnight-lag1-v1; producer atx-engine/tools/research_fields_price.py, fields-v10)"},
"ret_intraday": {"formula_id": "price-ret-intraday-lag1-v1", "origin": "fields_v10", "producer": "atx-engine/tools/research_fields_price.py", "clock": "price-close-lag1-v1: row t reads vendor observations dated on or before session t-1 only, each known at its 22:00 UTC close mark", "basis": "close over open of session t-1, minus 1 (vendor TickerHistory3; the factor cancels within a session); NaN without a finite positive open or when |log| > 1.5 (formula id price-ret-intraday-lag1-v1; producer atx-engine/tools/research_fields_price.py, fields-v10)"},
"exch_up_365d": {"formula_id": "finra-listing-up-switch365-v1", "origin": "fields_v12", "producer": "atx-engine/tools/research_fields_holdings.py", "clock": "finra-names-up-switch-asof-v1: FINRA name rows (security_master finra_names, PIT at their dissemination) usable at session t iff available_at < 22:00 UTC of t-1; events decided as rows become visible, never revised", "basis": "1 when the line's latest visible up-switch of its FINRA listing market (Nasdaq -> NYSE, NYSE American -> NYSE, Nasdaq -> NYSE American; consecutive rows at most 45 days apart) has a dissemination date within 365 days of date(t), else 0; NaN when the latest visible name row is older than 45 days or the name history is shorter than 410 days (formula id finra-listing-up-switch365-v1; producer atx-engine/tools/research_fields_holdings.py, fields-v12)"}
```
(`iv_atm_21d` is already a registry field.)

**L3 alphas**: written by the add-alpha lines below (`added_in` = v82). The resulting rows:
```json
{"id": "iv_vol_of_vol", "dsl": "rank(decay_linear(((-1 * (ts_std_mp(iv_atm_21d, 21, 12) / ts_mean_mp(iv_atm_21d, 21, 12))) + (0 * log(ts_std_mp(iv_atm_21d, 21, 12)))), 21))", "theme": "options_implied", "tier": "B-", "prior_sign": 1, "citation": "Baltussen, van Bekkum and van der Grient (2018, JFQA 53(4)) Unknown unknowns: uncertainty about risk and stock returns", "prior_sign_source": "Baltussen-van Bekkum-van der Grient 2018", "form": "R(decay_linear(x, 21))", "origin": "prior", "notes": {"formula": "-VOV, VOV = sample std of iv_atm_21d over the last 21 sessions / its mean over the same sessions (each with at least 12 finite cells); long low vol-of-vol", "domain": "NaN with fewer than 12 finite IV cells in 21 sessions (iv_atm_21d is NaN outside [0.02, 5]) and on a flat window (std 0: 0 * log(0) is NaN)", "deviation": "21 sessions with 12 required (paper: 20 days, 12 required); ddof 1 (paper 1/20); vendor 21-session constant-maturity clean ATM IV (paper: OptionMetrics nearest-ATM call/put average); flat windows NaN; house 21-session decay on a monthly measure; top 3,000"}, "added_in": "v82"}
{"id": "day_rev_freq", "dsl": "rank(decay_linear(ts_mean_mp((((ret_overnight > 0) && (ret_intraday < 0)) ? 1 : 0), 21, 15), 21))", "theme": "reversal_seasonality", "tier": "B-", "prior_sign": 1, "citation": "Akbas, Boehmer, Jiang and Koch (2022, JFE 145(3)) Overnight returns, daytime reversals, and future stock returns", "prior_sign_source": "Akbas-Boehmer-Jiang-Koch 2022", "form": "R(decay_linear(x, 21))", "origin": "prior", "notes": {"formula": "share of the last 21 sessions (at least 15 with both returns finite) with a positive overnight return followed by a negative intraday return of the same session (ret_overnight > 0 and ret_intraday < 0); a frequent tug of war predicts higher returns", "domain": "a session counts only when both returns are finite (compare and && propagate NaN); NaN with fewer than 15 such sessions; withdrawn at 0 trials if the fields lack the vendor open (FieldNeedsOpen)", "deviation": "rolling 21 sessions, not the calendar month; vendor open, not the auction print (a factor step dated t-1 sits in the overnight return); one-session field lag; house 21-session decay"}, "added_in": "v82"}
{"id": "exch_switch", "dsl": "rank((-1 * exch_up_365d))", "theme": "filing_events", "tier": "C+", "prior_sign": 1, "citation": "Dharan and Ikenberry (1995, JF 50(5)) The long-run negative drift of post-listing stock returns; Chen-Zimmermann ExchSwitch", "prior_sign_source": "Dharan-Ikenberry 1995", "form": "R(x)", "origin": "prior", "notes": {"formula": "-1{the FINRA listing market moved up (Nasdaq -> NYSE, NYSE American -> NYSE, Nasdaq -> NYSE American) on a row disseminated within the last 365 days} (exch_up_365d, finra-listing-up-switch365-v1); prior sign negative", "domain": "NaN when the listing market is unknown (latest visible name row older than 45 days) or the name history is shorter than 410 days; a binary flag: switched names tie at the bottom rank, the rest tie above", "deviation": "FINRA short-interest market class at its semi-monthly dissemination (up to about 15 days after the move) instead of CRSP exchange codes; 365 calendar days; moves that keep the vendor line count (de-SPACs included), moves with a new line are not seen; Arca, BZX and OTC moves are not events; continuous rank, not an event-time portfolio"}, "added_in": "v82"}
```

## 4. `add-alpha` command lines (strings frozen; `PY="C:/Program Files/Python312/python.exe"`)

Run after L1 and L2, in this order. `--parent` / `--parent-spec` = the last accepted library and cell at R-12 (written
here for an accepted v8.1; otherwise v80 or v71 and that cell's spec, as E9); `--fields` = the fields v12 dir as built.
Only those two substitutions are allowed; every other byte is frozen. (Ruling E-42, integration 6 part A: the
exch_switch line's `--theme listing_events` was changed to `--theme filing_events`; nothing else in the lines moved.)

```bash
"$PY" scripts/research_cycle.py add-alpha --id iv_vol_of_vol --dsl "rank(decay_linear(((-1 * (ts_std_mp(iv_atm_21d, 21, 12) / ts_mean_mp(iv_atm_21d, 21, 12))) + (0 * log(ts_std_mp(iv_atm_21d, 21, 12)))), 21))" --theme options_implied --tier B- --prior-sign 1 --citation "Baltussen, van Bekkum and van der Grient (2018, JFQA 53(4)) Unknown unknowns: uncertainty about risk and stock returns" --origin prior --prior-sign-source "Baltussen-van Bekkum-van der Grient 2018" --form "R(decay_linear(x, 21))" --formula "-VOV, VOV = sample std of iv_atm_21d over the last 21 sessions / its mean over the same sessions (each with at least 12 finite cells); long low vol-of-vol" --domain "NaN with fewer than 12 finite IV cells in 21 sessions (iv_atm_21d is NaN outside [0.02, 5]) and on a flat window (std 0: 0 * log(0) is NaN)" --deviation "21 sessions with 12 required (paper: 20 days, 12 required); ddof 1 (paper 1/20); vendor 21-session constant-maturity clean ATM IV (paper: OptionMetrics nearest-ATM call/put average); flat windows NaN; house 21-session decay on a monthly measure; top 3,000" --parent v81 --name v82 --parent-spec <last accepted cell spec> --fields <fields v12 dir>
"$PY" scripts/research_cycle.py add-alpha --id day_rev_freq --dsl "rank(decay_linear(ts_mean_mp((((ret_overnight > 0) && (ret_intraday < 0)) ? 1 : 0), 21, 15), 21))" --theme reversal_seasonality --tier B- --prior-sign 1 --citation "Akbas, Boehmer, Jiang and Koch (2022, JFE 145(3)) Overnight returns, daytime reversals, and future stock returns" --origin prior --prior-sign-source "Akbas-Boehmer-Jiang-Koch 2022" --form "R(decay_linear(x, 21))" --formula "share of the last 21 sessions (at least 15 with both returns finite) with a positive overnight return followed by a negative intraday return of the same session (ret_overnight > 0 and ret_intraday < 0); a frequent tug of war predicts higher returns" --domain "a session counts only when both returns are finite (compare and && propagate NaN); NaN with fewer than 15 such sessions; withdrawn at 0 trials if the fields lack the vendor open (FieldNeedsOpen)" --deviation "rolling 21 sessions, not the calendar month; vendor open, not the auction print (a factor step dated t-1 sits in the overnight return); one-session field lag; house 21-session decay" --parent v81 --name v82 --parent-spec <last accepted cell spec> --fields <fields v12 dir>
"$PY" scripts/research_cycle.py add-alpha --id exch_switch --dsl "rank((-1 * exch_up_365d))" --theme filing_events --tier C+ --prior-sign 1 --citation "Dharan and Ikenberry (1995, JF 50(5)) The long-run negative drift of post-listing stock returns; Chen-Zimmermann ExchSwitch" --origin prior --prior-sign-source "Dharan-Ikenberry 1995" --form "R(x)" --formula "-1{the FINRA listing market moved up (Nasdaq -> NYSE, NYSE American -> NYSE, Nasdaq -> NYSE American) on a row disseminated within the last 365 days} (exch_up_365d, finra-listing-up-switch365-v1); prior sign negative" --domain "NaN when the listing market is unknown (latest visible name row older than 45 days) or the name history is shorter than 410 days; a binary flag: switched names tie at the bottom rank, the rest tie above" --deviation "FINRA short-interest market class at its semi-monthly dissemination (up to about 15 days after the move) instead of CRSP exchange codes; 365 calendar days; moves that keep the vendor line count (de-SPACs included), moves with a new line are not seen; Arca, BZX and OTC moves are not events; continuous rank, not an event-time portfolio" --parent v81 --name v82 --parent-spec <last accepted cell spec> --fields <fields v12 dir>
```

Gate of the R-12 cell: admitted = the three ids minus any withdrawn at the freeze; require any; sign_agrees (as the
r7 template). Acceptance per prereg rule 5 and Ruling E-36 (marginal IC gates nothing).

## 5. Fields v12 (spec entry)

v12 = v11 + `exch_up_365d` (74 fields; manifest cap 1,024 after B-2). Argv delta from the v11 build:
```
--fields <v11 list>,exch_up_365d
--reuse <v11 dir> --reuse-sha256 <v11 manifest sha> --reuse-hardlink
```
No new option: the field reads the `--security-master` / `--security-master-sha256` stage that v9's argv already passes
for `regsho_threshold_days63`. Expected `--reuse` counts from v11: **reused 73, computed 1** (the four v7 holdings
kinds' producer fingerprints are unchanged against `fd2ff7a8`: 13f `cab3b9b4...`, ftd `9b2f42b6...`, regsho
`cc4cf935...`, svx `b1ceebb3...`; xsw new `3cf03c85...`; no builder, SEC, price or v8 module code changed).
Coverage expectation: finite from 410 days after the first visible FINRA name row; the panel's FINRA rows reach 2018
(ALPHA_PANEL_METRICS section 1), so all of TRAIN 2020-2023 is covered.

## 6. Trial accounting and roster

- Admission trials: v8.0 7 + v8.1 5 = 12; v8.2 = 3 (13, 14, 15 of 15). A candidate withdrawn before the read costs 0
  (day_rev_freq if `ret_*` are absent; exch_switch if fields v12 is not built; any string K1 refuses).
- Construction cell: R-12 = N 51 (Ruling E-38), only if R-6 is accepted; otherwise registered for v9.
- Roster: v8.1 at most 57 + 3 = 60 <= 64 (Ruling R7-a's cap 64; LIB2-b asks that it holds for v8.2).

## 7. Rulings root needs before the freeze (decision -- why -- cost if wrong)

- **LIB2-a:** `exch_switch` opens the new one-member theme `listing_events` -- the filing_events text (E7) is specific
  to Item 4.02 and no theme text is edited; the alternative is a ruling that re-words filing_events before v8.1's freeze
  -- cost if wrong: one more theme in T (each theme's share 1 / T shrinks; the member is capped at 1 / (2T), as R7-b).
  **Ruled (E-42):** the alternative: `exch_switch` joins the existing `filing_events` theme, its text widened to
  filing and listing events; no new theme (sections 1, 3 and 4 edited at integration 6 part A).
- **LIB2-b:** the roster cap 64 (R7-a) holds for v8.2 -- mechanical -- cost if wrong: none for inference.
- **LIB2-c:** iv_vol_of_vol's flat-window guard and ddof 1 -- a flat vendor IV is stale data, the ddof factor is
  rank-neutral on full windows -- cost if wrong: a few stale-IV names drop out; up to 2% relative distortion on windows
  with missing IV.
- **LIB2-d:** day_rev_freq stands or falls with `ret_overnight` / `ret_intraday` in the fields of record (F-1, OD-6
  open) -- withdrawal rule -- cost if wrong: none (0 trials).
- **LIB2-e:** registry field rows L2 for `ret_overnight` / `ret_intraday` (E6 named only ceq_iss_5y, coskew_60m) --
  generate_library refuses unregistered fields -- cost if wrong: add-alpha refuses day_rev_freq.

## 8. How root verifies

1. `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider test_research_fields_holdings_xsw.py
   test_research_fields_holdings.py test_prepare_research_fields_module_reuse.py` in `atx-engine/tools` (31 passed), or
   the whole directory (251 passed, 6 subtests).
2. Identity (flag absent): no builder or other module changed. `test_research_fields_holdings.ByteIdentity` now runs the
   full recipe with every holdings field including `exch_up_365d` and checks every other field's bytes, entries and
   checks unchanged; a build without `exch_up_365d` in `--fields` never opens the xsw path (opt-in kind). The v9 63-field
   module reuse recipe is unchanged (the new field is left out, as k8_item402_63).
3. Fingerprints: `prepare_research_fields.module_fingerprints(hold, <source>, builder_source())` on
   `git show fd2ff7a8:atx-engine/tools/research_fields_holdings.py` and on the working tree: 13f, ftd, regsho, svx equal.
4. Static figures: K1 `--plan-only` rows at add-alpha must read bars 40 / 40 / 0, extra fields 1 / 2 / 1, slots <= 7.

## 9. Hygiene (what was read)

- Read: the ALPHA briefs, lane rules, progress.md rulings (E-30, E-36..E-40), v8-prereg.md, library-v8-draft.md (all),
  task-F-3 and F-1 reports, the v8 literature notes `new_alpha_families.md`, the v7 draft exclusions and wave-2 prereg
  section 1, field builder and DSL engine code, the registry. atx-db (read-only, as directed): `ALPHA_PANEL.md` and
  `ALPHA_PANEL_METRICS.md` sections 1 and 1b (field coverage shares of member cells). **Disclosure:** the section 1 table
  prints per-year coverage columns for 2024, 2025 and 2026 (availability shares of non-return fields, no return
  statistic); seen because the document was assigned, used for nothing. Not opened: METRICS sections 2-5 (TRAIN
  distributions and audits), `ALPHA_PANEL_IC.md` (atx-db's TRAIN IC screen), any payload, build-equity output, IC, card
  or NAV. The v7 wave-2 prereg section 1 shows TRAIN coverage and shape of v7 members (not candidates): used for nothing.
- Web (definitions only): the Akbas et al. abstract (search summaries) and the VOV paper's measure section (sample
  1996-2014). No platform statistic and no return statistic dated 2024 or later was read; the CZ-own figures quoted for
  exch_switch come from the lane's literature notes (external, as the draft's priors).
- Synthetic tests only, dated 2019-2021; no data run.

## Cross-lane edits

- `atx-engine/tools/research_fields_holdings.py` (W5b / C-3): new kind `xsw` (constants, spec, `xsw_events`,
  `build_xsw`, one dispatch block in `build_all`, docstring line). No existing producer closure reaches the additions.
- `atx-engine/tools/test_prepare_research_fields_module_reuse.py`: `HOLD_V9` leaves the opt-in field out of the 63-field
  v9 recipe (as `SEC_V9` does for k8_item402_63); the check-key loop follows `HOLD_V9`.

## Open risks

- `exch_switch` is sparse (about 1-2% of names flagged [est]) and in TRAIN may be driven by the 2020-2021 de-SPAC wave
  (moves that keep the vendor line); a short-only flag on likely hard-to-borrow names.
- FINRA rows before June 2021 are FINRA's republication: their market class is as republished (vintage risk).
- day_rev_freq depends on the vendor open (OD-6 unconfirmed until the v10 build); the overnight return is the vendor's,
  not the auction print.
- iv_vol_of_vol uses the vendor's clean ATM IV (earnings effect removed by the vendor, calendar vintage unproven; see the
  iv_atm_21d caveats); VOV computed on a smoothed IV may be lower than on raw quotes.
- The static figures are the offline mirror's; K1 at add-alpha decides.
