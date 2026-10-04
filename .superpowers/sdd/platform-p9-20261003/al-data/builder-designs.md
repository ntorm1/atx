# AL-DATA: builder designs for the gated families (doc-only, W3-18; fix round 2)

This lane builds and registers nothing. Under ruling ALD-4, **no family clears the bar**, so nothing here is built by
default in P10.
- The designs stay ready in case the PM or the owner overrides that.
- The borrow-fee path (section 4a) is realism, not a signal. It is built only if the owner buys OD-P9-4 / -5 and the
  PM queues it.

## How each design is laid out

1. **Registry row** in the house shape (K-P9-1, `atx-engine/tools/field_registry.json`, as in the `gia_13f` /
   `russell_recon` rows). `formula_sha256` is computed by the C++. Every id is at most 24 characters.
2. **K-P9-11 keys.** `source_sample_end` is taken from the paper, and a value that cannot be verified is left out
   (ALSIG-1). `predicted_mechanism` and `data_class` complete them.
3. **Planted-leak probe** with teeth.
4. **Capacity class.**
5. **Bar verdict** (section 0).

Constants are fixed from the paper or by a stated mechanical argument. There is one variant and no grid.

## 0. The bar under ruling ALD-4, with every band derived

**Bar** (lit §3.0): net SR_c >= .4 **and** rho <= .1 to the book, both argued before any data.
- The rho values are the literature review's rho to *themes* (lit §3.1, [est]). They are used as rho to the book by
  the convention of ALSIG-2 / ALSIG-4.
- This file quotes no statistic of the book.

**Formulas used for every band.**
- Annual return = 12 x the monthly figure, or 52 x the weekly figure.
- Gross SR = annual return / annual volatility sigma.
- Cost drag in SR units = 2 · T · c / sigma, where:
  - T = one-way turnover of each leg per year;
  - c = one-way cost per dollar traded;
  - 2 = two legs.
- **Post-publication factor h = 1 - .58 = .42.** Lit §2.5, from McLean-Pontiff 2016 [32], where the lit tags it [mem].
  - Ruling ALD-4 applies h to the literature's return whenever TRAIN lies entirely after the source's publication,
    unless post-publication sample evidence is cited.
  - **One dating rule for every family (review Rn1).** "Publication" is the first public circulation: the working
    paper where its date is verified, else the journal issue. TRAIN starts 2020-01-01.
  - Costs are not haircut.
- Net SR_c = h x gross - drag when ALD-4 applies; otherwise gross - drag.

### F1 Lazy Prices (`lazy_prices`)

**Publication.** The working paper circulated before TRAIN (review n2); the journal issue is JF 75(3), June 2020. By
the dating rule, TRAIN lies entirely after publication, so h applies (as ALD-4 rules for F1). No post-publication
sample is cited: lit §7 and §8 Q4 leave it open.

| quantity | value | derivation |
|---|---|---|
| return | 4.08-6.96% a year | value-weighted 34-58 bps/month [V8L, lit §3.1] x 12 |
| sigma | 8-12% a year | [est]: no volatility in the lit or in the abstract |
| gross SR | .34 / .87 / centre .55 | 4.08 / 12; 6.96 / 8; 5.52 / 10 |
| drag | 0 to .05 | T 0-2 per leg per year [est: a name's similarity changes only at its own filings, at most 4 a year, and CMN's drift lasts 3-18 months]; c up to .125% [est]: 2 x 2 x .00125 / .10 = .05 |
| net, no haircut | .29-.87, centre .50 | .34 - .05; .87 - 0; .55 - .05 |
| **net, ALD-4** | **.09-.37, centre .18** | .42 x .34 - .05 = .09; .42 x .87 = .37; .42 x .55 - .05 = .18. ALD-4's own .12-.37 applies .42 to the net band; the verdict is the same |
| what it would take to clear | gross >= .95-1.07 | (.4 + drag) / .42, which is above the top of the stated band |
| rho | <= .1 | lit §3.1 F1: passes |

**Verdict: fails the SR half.** `lazy_prices` stays `blocked_on_data` and is never queued without a new PM ruling,
the same as `hf_crowd`.

### F2 hedge-fund crowding (`hf_crowd`)

- **rho .2 > .1** (lit §3.1 F2). It fails on that alone (ALSIG-4).
- **SR half: not derivable.**
  - The literature gives a direction only ("the most crowded long portfolios have the highest returns") and a
    book-level guess (+.02 to +.04, lit §6).
  - Back-solving that guess would be circular, so it is not used.
  - Publication: RFS 35(7), 2022. The working-paper date is not verified here, so by the dating rule the journal
    issue counts: TRAIN is not entirely after it, and h does not apply. If a pre-2020 working paper were verified, h
    would apply. The verdict is the same either way: F2 fails on rho, and its SR half is not derivable.
- **Data.** The house stage has no point-in-time fund-type input before 2023 (section 2).
- **Verdict: fails.**

### F4 put-call parity signal (`pcp_spread`); F5 (`os_ratio`); F6

- **F4.**
  - rho .3, so it fails.
  - Return: "50 basis points per week" (CW abstract) = 26% a year in sample. CW was published in JFQA in 2010, so
    TRAIN lies entirely after it: x .42 = 10.9% a year.
  - Cost: a weekly rebalance with T = 26 per leg per year [est: half of each leg replaced weekly] and c = .125% gives
    2 x 26 x .00125 = 6.5% a year, so about 4.4% a year net.
  - sigma is not given, so no SR can be derived. CW also report that predictability decays within their own sample.
- **F5.** rho .2, so it fails. 1.47%/month (lit §3.1) = 17.6% a year; JFE 2012, so x .42 = 7.4% a year. The same
  weekly cost of 6.5% a year leaves about .9% a year. sigma is not given.
- **F6.** rho .3, so it fails. The literature gives no magnitude ([mem]).
- **Verdict: all fail.**

### F10 intangible value (`bm_int`, a replace of `bm`)

- **The SR half is enough to decide.**
  - The replace acts through the difference portfolio, long HML_INT and short HML_FF.
  - EKP (NBER w28056 rev. 2021) give "2.11% annually, with a standard deviation of only 6.53%", so SR = .323, and the
    paper states "0.32 over the full sample" (1975-2018), before costs.
  - .32 < .4 before any haircut.
  - By the dating rule, publication is the NBER working paper of November 2020 (the CFR article is 2022). TRAIN is
    not entirely after it, so h is not required. If it were applied, .32 x .42 = .14.
  - The annual-rebalance cost would lower it further; it is not quantified here.
- **The 2007-2018 figure (.62)** is a sub-window. The bar is read on the full-sample figure, as for F1.
- **rho.** The rho of the difference portfolio to the book has no literature number and is not used. The add
  framing's .7 / .76 (HML_INT vs HML_FF, 76.2%) does not apply to a replace.
- **Verdict: fails.**

### Summary

| family | field | rho (theme, as book) | net SR_c a priori (derived above) | verdict |
|---|---|---|---|---|
| F1 | `lazy_prices` | <= .1 | .09-.37 (ALD-4) | **fails** (SR); never queued without a new ruling |
| F2 | `hf_crowd` | .2 | not derivable | fails (rho; ALSIG-4) |
| F4 (+F6) | `pcp_spread` | .3 | not derivable; about 4.4% a year net return | fails (rho) |
| F5 | `os_ratio` | .2 | not derivable; about .9% a year net return | fails (rho) |
| F10 | `bm_int` | n/a (replace) | .32 gross, full sample | fails (SR) |
| realism | `borrow_fee_implied` | n/a | n/a | outside the bar |

---

## 1. `lazy_prices` (F1, Cohen-Malloy-Nguyen 2020 JF 75(3) [39]): designed, NOT built (ALD-4)

**Registry row (ready)**

```json
{"name": "lazy_prices", "kind": "engine", "builder": "lazy_prices", "dtype": "f64", "point_in_time": true,
 "requires": [], "options": {"group": "sec_text"}, "sources": ["text"], "first_session": null,
 "owner": "engine-only:DEC-5 (not built: ALD-4)",
 "spec_text": {
  "units": "cosine similarity in [0, 1] of the full primary document of the line's latest original 10-K or 10-Q to its original filing of the same form family and fiscal period one fiscal year earlier (1 = unchanged; CMN's non-changers are high)",
  "clock": "text-filing-asof-v1: a text row is visible at V = available_at (authenticated EDGAR acceptance, UTC); session t reads, for the line's CIK on the session's role row, the original 10-K / 10-Q row with the latest available_at < date(t-1) 22:00 UTC; NULL available_at never visible; a row whose prior_available_at >= available_at -> NaN (counted); a row stamped on or after the research seal is refused; session 0 -> NaN",
  "staleness": "date(t) - date(V) > 400 days -> NaN (stage staleness_days 400); the latest visible filing without a finite doc_sim_cosine -> NaN (no skip-back)",
  "source_columns": ["text.cik", "text.accession", "text.form", "text.fiscal_period", "text.report_date", "text.available_at", "text.prior_accession", "text.prior_available_at", "text.doc_sim_cosine"],
  "definition": "Lazy Prices (Cohen, Malloy and Nguyen 2020, JF 75(3)), fixed before data (ALD-1): value = doc_sim_cosine of the line's CIK's latest visible original periodic report, the term-frequency cosine of the full primary document against the issuer's original filing of the same form family and fiscal period one fiscal year earlier, accepted strictly earlier, gap 180-550 days. Amendments are never current or prior. Every line of the CIK carries the value. Stage parts dated on or after the seal are never opened. Formula id lazy-prices-doc-cosine-v1.",
  "domain": [0.0, 1.0], "non_pit_aspects": [], "point_in_time": true}}
```

**Builder.**
- A `sources/text.hpp` / `.cpp` reader on `PinnedStage`, with a counter of sealed parts not opened.
- One kernel TU: an as-of walk over `previous_session_marks()`.
- The lane confirms the CIK column of A3's vendor panel before coding.

**K-P9-11 keys.**
- `source_sample_end`: 2014 (CMN 1995-2014, checked by the AL-SIG round-0 reviewer).
- `predicted_mechanism`: as registered.
- `data_class`: P.

**Planted-leak probe** (`ResearchFieldsLazyPrices.PlantedLeakProbeHasTeeth`; pyarrow fixture, numpy oracle):

| case | planted row(s) | what must hold |
|---|---|---|
| (a) clock boundary | filing A accepted exactly at 2021-03-01 22:00:00 UTC; filing B at 21:59:59 | A is not visible at 2021-03-02 and is visible at 2021-03-03; B is visible at 2021-03-02 |
| (b) future sentinel | a filing with `doc_sim_cosine` = 0.123456 | it is never read before its V |
| (c) amendment | a 10-K/A | never current or prior |
| (d) null clock | a row with NULL `available_at` | never used |
| (e) sealed part | a `year=2024` part | never opened (counted) |
| (f) inverted prior | a row with `prior_available_at >= available_at` | NaN (counted), never its value |
| (g) row-level seal | a row stamped 2024-01-01 00:00 UTC inside a `year=2023` part | the build is **refused** (as `research_fields_gold.py:28-29`: "refused, never dropped silently") |

Teeth: the oracle with `<=` in place of `<`, or with the mark of t or t-2 in place of t-1, breaks (a).

**Capacity.** Slow (half-life 126 sessions).

**Bar.** Fails (section 0).

---

## 2. `hf_crowd` (F2, Brown-Howard-Lundblad 2022 RFS 35(7) [46]): designed, NOT built

**Source correction (review M4).** The house already has stage `thirteenf_filer_type` (main
`alpha_panel/filer_type.py:1-25`, `adv.py:1-26`). It gives:
- a dated `filer_type` per (13F filer CIK, quarter), with value `hedge_fund` among the types;
- `filer_type_pit`, which repeats the rules on evidence published by the 13F deadline only, with no bulk Schedule D
  and no later roster (`filer_type.py:20-24`).

Its clocks matter here:
- Bulk Schedule D rows are available at the zip's Last-Modified, 2025-09-26 / 2026-05-01 (`adv-bulk-publication-v1`,
  `adv.py:25-26`). Schedule D gross asset value is therefore invisible to TRAIN.
- Monthly rosters carry private-fund counts by type only from 2023 (`adv.py:9-10`).

So the earlier GAV-share definition has no point-in-time input inside TRAIN before 2023. The design now reads the
house stage.

**Registry row (ready)**

```json
{"name": "hf_crowd", "kind": "engine", "builder": "hf_crowd", "dtype": "f64", "point_in_time": true,
 "requires": [], "options": {"group": "13f_holders"}, "sources": ["thirteenf", "thirteenf_filer_type"],
 "first_session": null, "owner": "engine-only:DEC-5 (not built: ALSIG-4)",
 "spec_text": {
  "units": "share in [0, 1] of the line's mapped 13F position value held by filers typed hedge_fund",
  "clock": "13f-quarter-filer-type-pit-v1: quarter P visible at V(P) (13f-filed-plus-46h-v1, as gia_13f); a filer's type at P = its thirteenf_filer_type filer_type_pit label for P, used only when that label's available_at < V(P); session t reads the latest P with V(P) < date(t-1) 22:00 UTC, P <= date(t), date(t) - P <= 150 days; session 0 -> NaN",
  "staleness": "date(t) - P > 150 days -> NaN; fewer than 5 13F holders -> NaN",
  "source_columns": ["filings.filer_cik", "filings.available_at", "holdings.* (as gia_13f)", "thirteenf_filer_type.filer_cik", "thirteenf_filer_type.period", "thirteenf_filer_type.filer_type_pit", "thirteenf_filer_type.available_at"],
  "definition": "Hedge-fund crowding (Brown, Howard and Lundblad 2022): value = sum over filers k with filer_type_pit = hedge_fund of value_k(i) / sum over all filers of value_k(i), positions as gia_13f's position rows; unclassified filers count in the denominator only; NaN with fewer than 5 holders. Formula id hf-crowd-filer-type-pit-v1.",
  "domain": [0.0, 1.0], "non_pit_aspects": [], "point_in_time": true}}
```

The column names of `thirteenf_filer_type` (`period`, `filer_type_pit`) must be confirmed against the stage before
coding.

**K-P9-11 keys.**
- `source_sample_end`: 2016 (BHL 2004-2016). The sample overlaps the OD-3 block, so the source is contaminated.
- `predicted_mechanism`: as registered.
- `data_class`: P.

**Probe.**
- A filer whose `filer_type_pit` changes between quarters.
- A label stamped at V(P) + 1 ns is never used for P.
- A filer typed only through bulk Schedule D (stamped 2025) is never `hedge_fund` inside TRAIN.
- Sealed parts are never opened.
- Teeth: using the non-PIT `filer_type` fails the bulk case.

**Capacity.** Slow.

**Bar.** Fails (section 0; ALSIG-4).

---

## 3. `bm_int` (F10, Eisfeldt-Kim-Papanikolaou 2022 CFR 11(2) [47]), a replace of `bm`: designed, NOT built

**Registry row (ready)**

```json
{"name": "bm_int", "kind": "engine", "builder": "bm_int", "dtype": "f64", "point_in_time": true,
 "requires": ["bm", "<the ME field bm divides by>"], "options": {"group": "value"},
 "sources": ["compustat_std", "cpi"], "first_session": null, "owner": "engine-only:DEC-5 (not built: bar)",
 "spec_text": {
  "units": "intangible-adjusted book-to-market (ratio), ranked within FF12 as bm",
  "clock": "funda-asof-v1: per (cik, datadate) the latest compustat_std funda vintage with available_at < date(t-1) 22:00 UTC; fiscal year T = the latest datadate with a visible annual row; a year's CPI-U average visible after the release of its December value; the bm term from the same session",
  "staleness": "bm's staleness; and date(t) - datadate(T) > 400 days -> NaN",
  "source_columns": ["funda.cik", "funda.datadate", "funda.available_at", "funda.xsga", "funda.xrd", "funda.gdwl", "funda.fin_template", "cpi.value", "cpi.available_at"],
  "definition": "Intangible value (EKP 2022 baseline): bm_int = bm + (INT_T - GDWL_T) / ME, ME = bm's denominator on the same row (book equity, ME, coverage and FF12 ranking stay bm's: one change, PM7-14). SGA_T = xsga_T + xrd_T when xrd is reported as a separate element (house XBRL items: fund_items.py:93, :104), else xsga_T, so the flow matches EKP's Compustat XSGA, which includes R&D. INT by perpetual inventory on real SGA (deflated by the fiscal year's CPI-U average): INT_T = (1 - 0.2) INT_(T-1) + SGA_T, INT_0 = SGA_1 / (0.1 + 0.2) at the first visible fiscal year (delta .2, g .1: EKP section 2.1), reflated to year-T dollars; GDWL_T = gdwl of year T (0 when NaN). A gap year (xsga NaN between visible years) -> NaN. Templates without an SG&A line (bank, insurer): INT = GDWL = 0, so bm_int = bm. Formula id intangible-bm-ekp-v1.",
  "domain": null, "non_pit_aspects": [], "point_in_time": true}}
```

**Open point.** The B3 convention report (`atx-db-request.md` B3 (iii)) decides whether some issuers' filed SG&A
element already includes R&D. If so, the `+ xrd` rule must name how those issuers are detected before any data is
read.

**Verified constants** (NBER w28056 rev. Nov 2021): delta .2, g .1, `INT_i0 = SG&A_i1/(g + δ)`, CPI deflation,
goodwill subtracted, and the sample "January 1975 to December 2018". The reflation step is [verify against the CFR
appendix].

**K-P9-11 keys.**
- `source_sample_end`: 2018. The sample includes 2015-2018, so the source is contaminated; that is a flag for OD-3,
  not a bar criterion.
- `predicted_mechanism`: "book equity omits the capitalised SG&A stock, so B/M misprices intangible-intensive firms;
  adding it ranks value more accurately within industries and the corrected value premium is larger".
- `data_class`: W.

**Probe.**
- A FY2019 `xsga` restated from 100 to 1000 (restatement available 2021-02-25): every earlier session uses 100.
- A CPI value released after the mark is not used.
- A `year=2024` part is never opened.
- Teeth: a clock-blind as-of read breaks the first case.

**Capacity.** Slow.

**Bar.** Fails the SR half (section 0).

---

## 4. Option-implied borrow and put-call parity (F4 [50][51], with F5 [54] and F6 [55])

**Data.** OD-P9-4 (strike-level end-of-day options) or OD-P9-5 (securities lending). Choosing between them is the
owner's money decision (ALD-2). Either one lands under rules C1-C6 of `atx-db-request.md`.

| route | per (`security_id`, date) | fills |
|---|---|---|
| options | per (expiry, strike): call / put bid, ask, mid, IV, open interest, volume; underlying close; risk-free curve; point-in-time dividend forecast; `snapshot_time`; `available_at` | `fee_bps` (implied) |
| lending | fee (bps), utilisation, lendable, on-loan; `available_at` = vendor publication | `fee_bps` and `locate_dollars` |

**History.** TRAIN needs 2019-12-01 to 2023-12-29. The OD-3 block needs 2014-10 onward, or the prereg states that
OD-3 runs on the flat fee.

### 4a. `borrow_fee_implied`: realism (outside the bar)

- **Registry row.**
  - `name` / `builder`: `borrow_fee_implied`; `sources`: `["options"]` or `["sec_lending"]`; `group`: `borrow`.
  - `units`: annual bps on short dollars.
  - `clock`: the latest snapshot with `available_at < date(t-1) 22:00 UTC`, stamped by its publication instant, never
    by the trade date.
  - `definition` (options route): the implied fee h from put-call parity on near-ATM, short-dated pairs, solving
    C - P = S·e^(-hT) - PV(div) - K·e^(-rT); the median across pairs; `fee_bps = max(h, 0) x 1e4`.
    - Moneyness and tenor cut-offs are fixed from [51] [verify] before any read, or else blind, with the
      early-exercise argument.
  - Lending route: the observed fee.
- **Use.**
  - The fee goes into `BorrowSchedule::fee_bps` (`borrow_schedule.hpp:58-79`; `FeeOnceV2`, `rebate_bps` 0).
  - **A cell with no data gets `default_fee_bps` written into it by the builder.** `fee()` returns the grid cell
    (`:77-79`), and validation rejects a non-finite or negative fee (`:85-86`, `:102`).
  - The lending route also fills `locate_dollars`.
  - It sits behind a flag; with the flag absent, today's flat fee is byte-identical.
  - It is registered as a cost-model protocol change.
- **Probe.**
  - A fee spike on day d (available d 20:30 UTC) reaches no grid row at or before d.
  - A lending fee published at d+1 06:00 UTC is stamped at its publication.
  - Teeth: stamping by trade date fails.
  - Sealed parts are never opened.

### 4b. `pcp_spread` (F4, with F6 as one hypothesis): NOT built

- **Definition (CW).** Per name-day, the open-interest-weighted mean over matched pairs of IV_call - IV_put; long high,
  short low, held about a week.
- **K-P9-11 keys.**
  - `source_sample_end`: unverified. The abstract states no years and SSRN returned 403, so it is left out (ALSIG-1).
  - `data_class`: N.
- **Capacity.** Fast.
- **Bar.** Fails (section 0). F5 `os_ratio` and F6 also fail.

---

## 5. Spec amendments for root (ALD-2, ALD-3, ALD-4, ALD-11, ALD-12; not applied by this lane)

- **5.1 and 5.2: root applies them at the wave-2 gate (content-only; ALD-12 (c)).** One spec commit (ALD-2). No `dsl`
  or `dsl_sha256` changes, no test line changes, and no digest pins these files.
- **5.3: NOT applied in P9. P10 note (ALD-11): it moves `formula_sha256` `787a8d87…a699`** (the `russell_recon`
  registry row, `atx-engine/tools/field_registry.json:3398`). Through P9 the row declares the presence aspect only.
- No new id is proposed: `bm_int`, `pcp_spread` and `os_ratio` are excluded by the bar and not registered (ALD-1).

The text below is exact: root copies it and makes no other change. The same text is in `task-AL-DATA-report.md`,
"Fix round 2", item 5, which also gives each key's current (before) value and line.

**Why no history entry is appended (changed from round 1).** `scripts/tests/test_p9_al_sig_registrations.py:156`
pins `[h["status"] for h in doc["history"]] == ["blocked_on_data"]`, which is exactly one entry. A second entry would
fail it. So the AL-DATA note goes into `notes`, and `history` is left as it is.

**What the registration test still requires.**
- `notes` must keep matching `horizon class (fast|medium|slow), half-life (\d+) sessions \(PM8-5` (`:43`, `:65-68`).
  Both new `notes` values keep the existing text as their prefix.
- `hf_crowd`'s `notes` must keep "ALSIG-4" and "never queued without a new PM ruling" (`:162-163`).
- Every changed key must stay a non-empty string (`scripts/wave_manifest.py:233-235`).

**5.1 `scripts/specs/p9/blocked/lazy_prices.json`: replace the values of these five keys. Root applies at the wave-2
gate (content-only).**
- Leave every other key unchanged, including `history`.
- `deviation` is new against round 1. Its old text ("no field name, kind or constant is chosen beyond the paper's
  similarity sign until it lands") contradicts ALD-1, which fixed the definition before any data.

```json
"formula": "lazy_prices (engine-only C++ kind, DEC-5; design .superpowers/sdd/platform-p9-20261003/al-data/builder-designs.md section 1): term-frequency cosine of the full primary document of a firm's latest original 10-K or 10-Q to its original filing of the same form family and fiscal period one fiscal year earlier (higher = fewer changes: the non-changers CMN buy), visible from its EDGAR acceptance under the house stage rule, 400-day staleness; fixed before data (ALD-1)",
"domain": "[0, 1]; NaN without a same-period prior-year original, past 400 days, or where the text landing does not cover the line",
"deviation": "the field is not built: PM ruling ALD-4 (it fails the bar on paper); the definition was fixed before any data under PM ruling ALD-1 (full-document cosine of original filings, same-period prior-year comparator, 400-day staleness); the atx-db text landing held about 7% of 10-K / 10-Q at the literature review (lit section 3.1 F1)",
"blocked_on": "atx-db ask al-data/atx-db-request.md B1 (low priority) and a new PM ruling on the bar (ALD-4)",
"notes": "horizon class slow, half-life 126 sessions (PM8-5: CMN's 3-18 month drift, the filing_events theme's); data class P; blocked_on_data: the 10-K / 10-Q text landing and its engine reader (lit section 6 row lazy_prices, P9DATA -> P9SIG); rho to the themes .1 or less (lit 3.1 F1); PM ruling ALD-4 (AL-DATA, 2026-10-04): on post-publication expectations (literature return x (1 - .58)) the a-priori net SR_c is .09-.37 < .4, so it fails the bar; it stays a blocked_on_data registration and is never queued without a new PM ruling; design al-data/builder-designs.md section 1",
```

The trailing comma follows the JSON of the file, where `notes` is followed by `"lane"`.

**5.2 `scripts/specs/p9/blocked/hf_crowd.json`: replace the value of `notes` only. Root applies at the wave-2 gate
(content-only).** Every other key, including `formula` and `history`, is unchanged.

```json
"notes": "horizon class slow, half-life 63 sessions (PM8-5: quarterly holdings, the ownership_flow theme's); data class P; blocked_on_data: the Form ADV private-fund flag joined to the 13F filer CIK (lit section 6 row hf_crowd, P9DATA -> P9SIG); rho about .2 to momentum and inst_best_ideas (lit 3.1 F2), above the .1 bar, check against inst_best_ideas before any screen; PM ruling ALSIG-4: it stays a blocked_on_data registration and is never queued without a new PM ruling on the bar; AL-DATA (W3-18, ALD-5, 2026-10-04): design written, not built; the 2004-2016 source sample overlaps the OD-3 block; the house source would be the atx-db thirteenf_filer_type stage (filer_type_pit), which has no point-in-time fund-type input before 2023; the Form ADV join is deferred (al-data/atx-db-request.md, B2 dropped); design al-data/builder-designs.md section 2",
```

**5.3 `russell_recon`: a second `non_pit_aspects` entry (ALD-3). P10 note (ALD-11): NOT applied in P9; it moves
`formula_sha256` `787a8d87…a699`.** This is a C++, registry and test change, not a JSON spec edit. The text below is
kept for P10.

(i) `atx-engine/src/research/fields/russell_recon_field.cpp`.
- After line 59 (`  // in time. The flag and the aspect keep the field out of every IC run (look-ahead guard).`), insert:

```cpp
  // Ruling ALD-3 (AL-DATA): the stage's eligibility and company inputs use 2026 snapshots (values).
```

- On line 69, replace `      "present are point in time."};` with `      "present are point in time.",` followed by these
  lines:

```cpp
      "values: the house indexes stage (atx-db alpha_panel.indexes, snapshot) selects members "
      "on the rank-date panel through security_type, is_adr, is_adr_likely, is_lp and "
      "is_operating and groups the selected lines into companies by the panel cik; the panel "
      "takes security_type from the Nasdaq Trader directory snapshot of 2026-09-18 whenever the "
      "dated FINRA type is NULL (no FINRA row, or one older than the staleness limit), unknown "
      "or common_unverified, ORs that snapshot's ETF flag into is_etf, which gates is_operating "
      "(alpha_panel.panel SECURITY_FLAGS, OPERATING_FLAGS), and takes a line's company (CIK) "
      "from the identity link table, the snapshot-run-backfill-v1 link tier included, whose "
      "choice of CIK comes from the 2026-09-20 SEC ticker snapshot and which covers only lines "
      "alive at that snapshot; the CIK also keys is_operating; so a line's membership in R1000P "
      "or R2000P at a 2018-2023 rank date, the company ranks and the p1000 breakpoint, and "
      "therefore the line's +1, -1 or 0 or its NaN, can depend on its survival to 2026 and on "
      "its 2026 classification and CIK (post-seal information)."};
```

The concatenated string, which is the registry row's second `non_pit_aspects` element, reads:

> values: the house indexes stage (atx-db alpha_panel.indexes, snapshot) selects members on the rank-date panel
> through security_type, is_adr, is_adr_likely, is_lp and is_operating and groups the selected lines into companies by
> the panel cik; the panel takes security_type from the Nasdaq Trader directory snapshot of 2026-09-18 whenever the
> dated FINRA type is NULL (no FINRA row, or one older than the staleness limit), unknown or common_unverified, ORs
> that snapshot's ETF flag into is_etf, which gates is_operating (alpha_panel.panel SECURITY_FLAGS,
> OPERATING_FLAGS), and takes a line's company (CIK) from the identity link table, the snapshot-run-backfill-v1 link
> tier included, whose choice of CIK comes from the 2026-09-20 SEC ticker snapshot and which covers only lines alive
> at that snapshot; the CIK also keys is_operating; so a line's membership in R1000P or R2000P at a 2018-2023 rank
> date, the company ranks and the p1000 breakpoint, and therefore the line's +1, -1 or 0 or its NaN, can depend on
> its survival to 2026 and on its 2026 classification and CIK (post-seal information).

The block quote wraps the one string for reading. It has no line breaks, and single spaces join the words.

(ii) `atx-engine/tools/field_registry.json`, the `russell_recon` row.
- Re-derive it with the AL-SIG round-trip (`task-AL-SIG-report.md:320`):
  - set `spec_text.non_pit_aspects` to the two strings of the C++ literal;
  - recompute `formula_sha256` as the C++ `formula_sha256(spec)`;
  - check with `field_registry.validate`, write with `field_registry.dump`, and re-run `--check`.
- `formula_sha256` moves from `787a8d87…a699`. The field was never built, so the row stays at revision 1.
- gtest `ResearchFieldsAlSigFixture.CommittedRowsPinTheSpecs`
  (`atx-engine/tests/research/research_fields_al_sig_fixture_test.cpp:112-143`) confirms the fingerprint and the spec
  text.

(iii) `atx-engine/tests/research/research_fields_al_sig_fixture_test.cpp`.
- Line 140: `  ASSERT_EQ(russell.non_pit_aspects.size(), 1U);` becomes
  `  ASSERT_EQ(russell.non_pit_aspects.size(), 2U);`.
- After line 142, insert:

```cpp
  EXPECT_EQ(russell.non_pit_aspects[1].rfind("values: ", 0), 0U);
  EXPECT_NE(russell.non_pit_aspects[1].find("snapshot-run-backfill-v1"), std::string::npos);
```

- Lines 135-136 (comment) become:

```cpp
  // Ruling ALSIG-7: gia_13f is point in time; russell_recon's presence is not (the house stage's
  // row set looks ahead to a delisting), declared as a "presence: ..." aspect; ruling ALD-3 adds a
  // "values: ..." aspect (the stage's 2026 directory and backfill-CIK inputs).
```

(iv) `scripts/tests/test_p9_al_sig_registrations.py`. It also pins one aspect today (`:109`), which round 1 missed.
- Line 106 becomes:
  `    # Rulings ALSIG-7 and ALD-3. The registry row declares the look-ahead exactly ("presence: ..." and "values: ...").`
- Line 109 becomes:
  `    assert row["point_in_time"] is False and len(aspects) == 2 and aspects[0].startswith("presence: ")`
- After line 111, insert:

```python
    assert aspects[1].startswith("values: ")                 # ruling ALD-3: the 2026 directory and backfill CIK
    for phrase in ("2026-09-18", "snapshot-run-backfill-v1", "is_operating"):
        assert phrase in aspects[1], phrase
```

These test edits are deliberate rule changes under ruling ALD-3. They are not edits to make a test pass. But 5.3 does
move one pinned value: the registry row's `formula_sha256` (`787a8d87…a699`, `field_registry.json:3398`), which the
source calls the "formula fingerprint pinned by the registry check and the AL-SIG gtests"
(`russell_recon_field.cpp:20-21`) and which the build checks (`registry.cpp:288-291`). It also moves the SHA-256 of
the registry file that later research-fields receipts record. Under ALD-11 that makes 5.3 a P10 note.

(v) **Proof.**
- Build `atx-engine-research-fields-tests` and `atx-research-fields` through `scripts/research-build.ps1`.
- Run gtest `ResearchFieldsAlSig*:ResearchFieldsRegistry*`.
- Run pytest `scripts/tests/test_p9_al_sig_registrations.py`, then `atx-engine/tools/test_field_registry.py`, in
  separate invocations.

The prefix `values:` follows the house aspect labels. The entry also moves presence, as its text says.
