# AL-DATA: builder designs for the gated families (doc-only, Ruling W3-18)

Nothing here is built or registered by this lane. Each design is ready for a P10 lane once its data lands, but only
the designs marked **build** should be built.

## How each design is laid out

Every design has five parts:

1. **Registry row.** The house shape (K-P9-1, `atx-engine/tools/field_registry.json`, as in the `gia_13f` /
   `russell_recon` rows): `name`, `kind: engine`, `builder`, `dtype`, `point_in_time`, `spec_text {units, clock,
   staleness, source_columns, definition, domain, non_pit_aspects}`, `requires[]`, `options{group}`, `sources[]`,
   `first_session`, `owner`. `formula_sha256` is computed by the C++ at build time. Every id is at most 24 characters
   (W1-I1).
2. **The K-P9-11 keys.**
   - `source_sample_end` comes from the paper. A value that cannot be verified is left out (ALSIG-1).
   - `predicted_mechanism` and `data_class` (H / W / P / N, lit §3.1) complete them.
3. **The planted-leak probe** the builder's gtest must contain. It must have teeth: a clock that is wrong by one
   session must fail it.
4. **Capacity class.**
5. **Bar verdict.**

Constants are fixed from the paper or by a stated mechanical argument. There is one variant per hypothesis and no
grid.

**The bar.** The bar is lit §3.0: net SR_c >= .4 **and** rho <= .1 to the book, with both halves argued before any
data is read. Recent rulings applied it like this:
- ALSIG-2: `gia_13f` has rho .15, so it was excluded and not registered.
- ALSIG-4: `hf_crowd` has rho .2, so it stays blocked and is never queued without a new ruling.
- ALSIG-5: `russell_recon` has rho about 0, but its SR_c could not be argued, so it was dropped.

At book SR 1.77, a member with SR_c .4 adds +.045 at rho 0 and +.014 at rho .1 (lit §3.0 table).

## Verdict summary

| family | field (builder id) | rho to themes (lit §3.1) | SR_c argued a priori | verdict | build? |
|---|---|---|---|---|---|
| F1 Lazy Prices | `lazy_prices` | <= .1 | about .4-.65 net, centre about .5 [est, below]; decay after publication unverified | **clears on paper**, narrowly | **build** when B1 lands |
| F2 hedge-fund crowding | `hf_crowd` | .2 | not argued | fails the rho half (ALSIG-4) | no |
| F10 intangible value (replace of `bm`) | `bm_int` | .7 to value; HML_INT vs HML_FF correlation .76 [EKP] | the difference portfolio has SR .32 over 1975-2018 [EKP], pre-cost | fails both halves | no |
| F4 put-call parity signal (+ F6, one hypothesis by PM rule) | `pcp_spread` | .3 | predictability "decreases over the sample period" [CW abstract]; weekly turnover | fails both halves | no |
| F5 option / stock volume | `os_ratio` | .2 | - | fails the rho half | no |
| F4 / OD-P9-5 borrow fee (realism) | `borrow_fee_implied` | n/a: a cost-model input, not a signal | n/a | outside the bar | **build** if OD-P9-4 or -5 is bought |

**Only F1 clears on paper.** Its SR_c half rests on the in-sample size of the effect. TRAIN (2020-2023) falls entirely
after the paper's publication (JF 2020), and lit §7 and §8 Q4 leave decay after publication unverified. If the
in-sample Sharpe (centre about .55, gross [est]) decays by more than about a quarter, F1 falls below .4 [arith: .55 x
.73 = .40].

---

## 1. `lazy_prices` (F1, Cohen-Malloy-Nguyen 2020 JF 75(3) [39]): BUILD when ask B1 lands

**Registry row**

```json
{"name": "lazy_prices", "kind": "engine", "builder": "lazy_prices", "dtype": "f64", "point_in_time": true,
 "requires": [], "options": {"group": "sec_text"}, "sources": ["text"], "first_session": null,
 "owner": "engine-only:DEC-5 (P10 lane, design AL-DATA)",
 "spec_text": {
  "units": "cosine similarity in [0, 1] of the full primary document of the line's latest original 10-K or 10-Q to its original filing of the same form family and fiscal period one fiscal year earlier (1 = unchanged; CMN's non-changers are high)",
  "clock": "text-filing-asof-v1: a text row is visible at V = available_at (authenticated EDGAR acceptance, UTC); session t reads, for the line's CIK on the session's role row, the original 10-K / 10-Q row with the latest available_at < date(t-1) 22:00 UTC; NULL available_at never visible; session 0 -> NaN",
  "staleness": "date(t) - date(V) > 400 days -> NaN (stage staleness_days 400); the latest visible filing without a finite doc_sim_cosine -> NaN (no skip-back to an older filing)",
  "source_columns": ["text.cik", "text.accession", "text.form", "text.fiscal_period", "text.report_date", "text.available_at", "text.prior_accession", "text.prior_available_at", "text.doc_sim_cosine"],
  "definition": "Lazy Prices (Cohen, Malloy and Nguyen 2020, JF 75(3)). Value of a line at session t = doc_sim_cosine of its CIK's latest visible original periodic report: term-frequency cosine (house tokenizer, no stemming, no stop words) of the full primary document against the issuer's original filing of the same form family and fiscal period one fiscal year earlier, accepted strictly earlier, gap 180-550 days. Amendments are never the current or the prior filing. Every line of the CIK carries the value. Stage parts dated on or after the research seal are never opened. Formula id lazy-prices-doc-cosine-v1.",
  "domain": [0.0, 1.0], "non_pit_aspects": [], "point_in_time": true}}
```

**Builder.**
- `sources/text.hpp` / `text.cpp`: a `PinnedStage` reader with contract `{"text", "atx.alpha-panel.text-features/v2",
  "<clock rule>", 400, "text-features-v2"}`. It reads only the listed columns, and only from parts whose year is
  before the seal (the counter mirrors `holdings_parts_not_read_sealed`).
- `lazy_prices_field.cpp`: one kernel. It sorts rows by (cik, available_at, accession) and runs an as-of walk over
  `previous_session_marks()`. Lines map to their CIK through the role row's `cik`.
- The lane confirms the CIK column of A3's vendor panel source before coding.

**K-P9-11 keys.**
- `source_sample_end`: **2014**. CMN's sample is 1995-2014; the AL-SIG round-0 reviewer checked it against the paper
  (`task-AL-SIG-report.md:245`). The source is clean for OD-3.
- `predicted_mechanism`: unchanged from the blocked registration: "investors under-react to changes in the text of
  annual and quarterly reports, so firms whose filings barely change outperform the changers ...".
- `data_class`: **P**.

**Planted-leak probe** (`ResearchFieldsLazyPrices.PlantedLeakProbeHasTeeth`; synthetic stage written by pyarrow,
independent numpy oracle):

| case | planted row | what must hold |
|---|---|---|
| (a) clock boundary | filing A accepted exactly at 2021-03-01 22:00:00 UTC; filing B (another CIK) at 21:59:59 | A is **not** visible at session 2021-03-02 (strict `<` against date(t-1) 22:00) and is visible at 2021-03-03; B is visible at 2021-03-02 |
| (b) future sentinel | a 2022 filing with `doc_sim_cosine` = 0.123456 | no session before that filing's V reads 0.123456 |
| (c) amendment | a 10-K/A accepted after A, with similarity 0.0 | it never appears in any value, as current or as prior |
| (d) null clock | a row with NULL `available_at` | never used |
| (e) sealed part | a `year=2024` part listed in the fixture manifest | never opened (counted); the build succeeds |

Teeth: the oracle run with `<=` in place of `<`, or with the mark of session t in place of t-1, must break (a), and
the test asserts that it does (the AL-SIG probe pattern).

**Capacity.** Slow: filings arrive quarterly, the drift lasts 3-18 months, and the registered half-life is 126
sessions. It raises capacity (lit §4.3).

**Bar.**
- rho <= .1 (lit §3.1 F1). The `filing_events` theme overlaps only partly.
- SR_c: CMN's value-weighted alpha is 34-58 bps/month for up to 18 months [V8L], i.e. 4.1-7.0% a year.
  - At 8-12% annual volatility for a value-weighted quintile long/short on a slow non-style characteristic [est], the
    gross Sharpe is .34-.87, centre about .55.
  - Turnover is at most once per filing, so the cost drag is small (< .05 [est]). That gives net SR_c about .4-.65.
  - The lit §6 book-level gain of +.03 to +.06 at rho .1 back-solves to SR_c .50-.64 [arith, lit §3.0 formula].
- Clears on paper, with the decay-after-publication caveat above.

**Registration.** Amend `scripts/specs/p9/blocked/lazy_prices.json`, never a second id. The text is in section 5.

---

## 2. `hf_crowd` (F2, Brown-Howard-Lundblad 2022 RFS 35(7) [46]): designed, NOT to be built

**Registry row (ready, not built)**

```json
{"name": "hf_crowd", "kind": "engine", "builder": "hf_crowd", "dtype": "f64", "point_in_time": true,
 "requires": [], "options": {"group": "13f_holders"}, "sources": ["thirteenf", "adv_filer_type"],
 "first_session": null, "owner": "engine-only:DEC-5 (not built: ALSIG-4)",
 "spec_text": {
  "units": "share in [0, 1] of the line's mapped 13F position value held by hedge-fund filers",
  "clock": "13f-quarter-adv-asof-v1: quarter P visible at V(P) (stage 13f-filed-plus-46h-v1, as gia_13f); a filer's type at P from the latest adv_filer_type vintage of each of its CRDs with available_at < V(P); session t reads the latest P with V(P) < date(t-1) 22:00 UTC, P <= date(t) and date(t) - P <= 150 days; session 0 -> NaN",
  "staleness": "date(t) - P > 150 days -> NaN (stage staleness_days 150); fewer than 5 13F holders -> NaN",
  "source_columns": ["filings.filer_cik", "filings.available_at", "holdings.*  (as gia_13f)", "adv_filer_type.filer_cik", "adv_filer_type.available_at", "adv_filer_type.raum_usd", "adv_filer_type.hedge_fund_gav_usd", "adv_filer_type.withdrawn"],
  "definition": "Hedge-fund crowding (Brown, Howard and Lundblad 2022). Positions of quarter P as gia_13f's position rows (13f-asof45-effective-v1, F13_ROWS_RULE, CUSIP map at P). Filer k is a hedge-fund filer at P iff the sum over its CRDs of hedge_fund_gav_usd >= 0.5 x the sum of raum_usd, over each CRD's latest vintage with available_at < V(P) (a majority of regulatory AUM in hedge funds: a mechanical cut). Value = sum over hedge-fund filers of value_k(i) / sum over all filers of value_k(i); NaN with fewer than 5 holders (gia_13f's minimum). Formula id hf-crowd-adv-share-v1.",
  "domain": [0.0, 1.0], "non_pit_aspects": [], "point_in_time": true}}
```

**K-P9-11 keys.**
- `source_sample_end`: **2016**. BHL's sample is about 1,500 hedge funds over 2004-2016, checked by the AL-SIG
  round-0 reviewer. The sample overlaps the OD-3 block, so the source is **contaminated**.
- `predicted_mechanism`: unchanged from the blocked registration.
- `data_class`: **P**.

**Planted-leak probe.**

| case | planted rows | what must hold |
|---|---|---|
| vintage flip | adviser A is not a hedge-fund filer in vintage v1 (available 2020-04-01) and is one in v2 (available 2021-04-01) | 2020q4 (V around 2021-02-16) classifies A by v1; 2021q1 classifies it by v2 |
| late vintage | a vintage stamped V(P) + 1 ns | never used for P |
| withdrawn adviser | an adviser with ADV-W in 2022-06 | still classified for its 2020-2021 quarters |
| sealed parts | 13F and ADV parts dated 2024 or later | never opened |

Teeth: classifying every quarter by each adviser's last vintage must fail the test.

**Capacity.** Slow: quarterly, registered half-life 63 sessions.

**Bar.** Fails the rho half (.2 > .1). Ruling ALSIG-4 stands: the string stays `blocked_on_data` and is never queued
without a new PM ruling. Its mechanism (pay for tail risk from crowded selling) also adds negative skew to the book.
**Do not build.** Ask B2 is scheduled last.

---

## 3. `bm_int` (F10, Eisfeldt-Kim-Papanikolaou 2022 CFR 11(2) [47]), a `replace` of `bm`: designed, NOT to be built

**Registry row (ready, not built)**

```json
{"name": "bm_int", "kind": "engine", "builder": "bm_int", "dtype": "f64", "point_in_time": true,
 "requires": ["bm", "<the ME field bm divides by>"], "options": {"group": "value"},
 "sources": ["compustat_std", "cpi"], "first_session": null, "owner": "engine-only:DEC-5 (not built: bar)",
 "spec_text": {
  "units": "intangible-adjusted book-to-market (ratio), ranked within FF12 as bm",
  "clock": "funda-asof-v1: per (cik, datadate) the latest compustat_std funda vintage with available_at < date(t-1) 22:00 UTC; fiscal year T = the latest datadate with a visible annual row; a year's CPI-U average visible after the release of its December value; the bm term from the same session",
  "staleness": "bm's staleness; and date(t) - datadate(T) > 400 days -> NaN",
  "source_columns": ["funda.cik", "funda.datadate", "funda.available_at", "funda.xsga", "funda.gdwl", "funda.fin_template", "cpi.value", "cpi.available_at"],
  "definition": "Intangible value (Eisfeldt, Kim and Papanikolaou 2022, baseline): bm_int = bm + (INT_T - GDWL_T) / ME, ME = bm's denominator on the same row, so book equity, ME, coverage and the FF12 ranking stay bm's (one change, PM7-14). INT by perpetual inventory on real SG&A (xsga / CPI-U average of the fiscal year): INT_T = (1 - 0.2) INT_(T-1) + SGA_T, INT_0 = SGA_1 / (0.1 + 0.2) at the first visible fiscal year (delta .2, g .1: EKP section 2.1), reflated to year-T dollars; GDWL_T = gdwl of year T (0 when NaN). A gap year (xsga NaN between visible years) -> NaN (missing is not zero, EKP). Templates without an SG&A line (fin_template bank or insurer): INT = GDWL = 0, so bm_int = bm. Formula id intangible-bm-ekp-v1.",
  "domain": null, "non_pit_aspects": [], "point_in_time": true}}
```

**Constants.** Verified this lane from NBER w28056 (revision of November 2021):
- "INT_it = (1 − δ)INT_it−1 + SG&A_it", with "INT_i0 = SG&A_i1/(g + δ)", "g = 0.1" and "δ = 0.2";
- SG&A is "real SG&A expenditure, calculated by deflating xsga by the consumer price index";
- B_INT subtracts goodwill.

The reflation step and the exact book-equity line are [verify against the CFR appendix]. The design keeps the house
`bm` book equity by construction, so only the INT - GDWL term is new.

**K-P9-11 keys.**
- `source_sample_end`: **2018**. NBER w28056 rev. 2021 says "The sample is from January 1975 to December 2018". The
  typeset CFR text was not read; this is the same standard as AL-SIG's WYZ check. The sample includes 2015-2018, so
  the source is **contaminated** for OD-3.
- `predicted_mechanism`: "book equity omits the capitalised SG&A stock (organisation capital), so B/M misprices
  intangible-intensive firms; adding the stock to book value ranks value more accurately within industries and the
  corrected value premium is larger".
- `data_class`: **W**.

**Planted-leak probe.**

| case | planted rows | what must hold |
|---|---|---|
| restatement | FY2019 `xsga` = 100 (available 2020-02-20), restated to 1000 (available 2021-02-25) | every session before the restatement becomes visible uses 100 |
| CPI release | a CPI value released after the mark | not used |
| sealed part | a `year=2024` funda part | never opened |

Teeth: an as-of read that ignores the clock (latest vintage over all time) must break the first case.

**Capacity.** Slow (annual).

**Bar. Fails.**
- As an add, rho is .7 to value (lit) and HML_INT vs HML_FF correlation is 76% (EKP).
- As a replace, the change acts through the difference portfolio (long HML_INT, short HML_FF). EKP give its Sharpe as
  "0.32 over the full sample" (1975-2018, before costs), below .4.
- Their .62 "in data since 2007" is a sub-window that overlaps OD-3.
- Novy-Marx-Medhat [38] find it spanned by profitability, and X-4 (an FF49 value member change) lost -.058 (lit §0.1).

**Do not build.** Record it as excluded by the bar and do not register it (ALSIG-2 precedent). Ask B3 stays a
platform item.

---

## 4. Option-implied borrow and put-call parity (F4 [50][51], with F5 [54] and F6 [55]): signals NOT built; fee path BUILD if data is bought

**Data.** OD-P9-4 (strike-level end-of-day options: ORATS strikes or OPRA) or OD-P9-5 (securities lending). These are
purchases, not atx-db asks, but they land under the same rules C1-C6 as ask B (`atx-db-request.md`).

| route | per (`security_id`, date) columns | adds |
|---|---|---|
| options | per (expiry, strike): call / put bid, ask, mid, vendor IV, open interest, volume; underlying close; risk-free curve; point-in-time dividend forecast; `snapshot_time`; `available_at` (the snapshot instant, or delivery if later) | `fee_bps` (implied); data for F4-F6 |
| lending | indicative fee (bps), utilisation, lendable value, on-loan value; `available_at` = vendor publication | `fee_bps` (observed) **and** `locate_dollars` (from lendable supply) |

**History.**
- TRAIN needs 2019-12-01 to 2023-12-29.
- The OD-3 block needs 2014-10-01 onward. OD-P9-4 as written covers only 2018-2023, so OD-3's 2015-2017 would run on
  the flat fee. Either buy 2014-2023, or the prereg states that OD-3 uses a different cost model.

### 4a. `borrow_fee_implied`: the realism path (not a signal; outside the bar)

- **Registry row.**
  - `name`, `builder`: `borrow_fee_implied`; `sources`: `["options"]` or `["sec_lending"]`; `group`: `borrow`.
  - `units`: annual borrow fee in bps on short dollars.
  - `clock`: the latest snapshot with `available_at < date(t-1) 22:00 UTC`. It is stamped by the publication instant,
    never by the trade date.
  - `definition` (options route): the implied fee h from put-call parity on near-ATM, short-dated pairs, solving
    C - P = S·e^(-hT) - PV(div) - K·e^(-rT); the median across pairs; `fee_bps = max(h, 0) x 1e4`.
    - The moneyness and tenor cut-offs are fixed from Muravyev-Pearson-Pollet 2025 [51] [verify] before any read.
    - If those cannot be verified, the lane fixes one set blind, with the mechanical argument that short-dated
      near-ATM pairs minimise early-exercise bias.
  - Lending route: the observed fee.
  - A name without data gets NaN, which maps to `default_fee_bps`.
- **Use.**
  - The fee goes into `BorrowSchedule::fee_bps` (`atx-engine/include/atx/engine/book/borrow_schedule.hpp:58-79`;
    `FeeOnceV2`, `rebate_bps` 0).
  - The lending route also fills `locate_dollars`.
  - It sits behind a flag; with the flag absent, today's flat fee is byte-identical.
  - It is registered as a cost-model protocol change, not a hypothesis. It lowers modelled net Sharpe by about .06-.10
    (lit §0 item 5).
- **Probe.**
  - A sentinel fee spike on day d (available d 20:30 UTC) must not reach the grid row of any session at or before d.
  - A lending fee for date d published at d+1 06:00 UTC must be stamped at its publication.
  - Teeth: stamping by trade date must fail.
  - Parts dated 2024 or later are never opened.
- **Capacity.** n/a (cost model). It makes every capacity curve more realistic.

### 4b. `pcp_spread` (F4, with F6 as one hypothesis): NOT built

- **Definition (Cremers-Weinbaum).** Per name-day, the open-interest-weighted mean over matched pairs (same strike,
  same expiry) of IV_call - IV_put; long high, short low, held about a week.
- **K-P9-11 keys.**
  - `source_sample_end`: **unverified**. The CW abstract (Cambridge Core) states no years, SSRN 968237 refused access,
    and lit §5.2(b) only places F4-F5 at "2005-2010". It is left out (ALSIG-1).
  - `data_class`: N.
- **Bar. Fails both halves.**
  - rho is .3 to short_interest (lit §3.1).
  - The abstract itself says "the degree of predictability decreases over the sample period ... consistent with
    mispricing during the earlier years of the study, with a gradual reduction of the mispricing over time".
  - Weekly turnover takes net far below gross.
- F6 (An-Ang-Bali-Cakici, change in call IV minus change in put IV) has rho .3. F5 `os_ratio` (Johnson-So) has rho
  .2. Both fail the rho half.
- **Capacity.** Fast (1-4 weeks); it lowers capacity (lit §4.3).
- **Do not build** any of F4-F6 as signals. Because the signals are out, OD-P9-4 versus -5 is a realism choice only
  (PM question 6 in the report).

---

## 5. Amendment text for the two existing blocked registrations (not applied: this lane writes only `al-data/`)

wave3-carry requires these files to be amended, with no duplicate id across the queue directories. Whoever applies
the text runs `scripts/tests/test_p9_al_sig_registrations.py` afterwards; it pins these documents, including the
ALSIG-4 note. Neither `dsl` nor `dsl_sha256` changes. No new id is proposed: `bm_int` and `pcp_spread` are excluded by
the bar and not registered.

`scripts/specs/p9/blocked/lazy_prices.json`: replace three keys and append one history entry.

```json
"formula": "lazy_prices (engine-only C++ kind, DEC-5; design .superpowers/sdd/platform-p9-20261003/al-data/builder-designs.md section 1): term-frequency cosine of the full primary document of a firm's latest original 10-K or 10-Q to its original filing of the same form family and fiscal period one fiscal year earlier (higher = fewer changes: the non-changers CMN buy), visible from its EDGAR acceptance under the house stage rule, 400-day staleness",
"domain": "[0, 1]; NaN without a same-period prior-year original, past 400 days, or where the text landing does not cover the line",
"blocked_on": "atx-db ask al-data/atx-db-request.md B1 (10-K and 10-Q text accepted 2012-2023 with full-document cosine, year-partitioned, coverage gate), then the engine kind",
"history": [ ...existing entry...,
  {"status": "blocked_on_data", "at": "2026-10-04", "by": "AL-DATA",
   "note": "W3-18 doc-only lane: design and atx-db ask written; bar on paper: clears (rho <= .1; SR_c argued from CMN's value-weighted alpha), decay after publication unverified; no field, not in the queue"}]
```

`scripts/specs/p9/blocked/hf_crowd.json`: append one history entry only.

```json
{"status": "blocked_on_data", "at": "2026-10-04", "by": "AL-DATA",
 "note": "W3-18: design written, not to be built: rho .2 fails the bar (ALSIG-4); the 2004-2016 source sample overlaps the OD-3 block; Form ADV ask deferred (al-data/atx-db-request.md B2)"}
```

If the schema refuses a second history entry with the same status, put the same text in `notes` instead. That
fallback is the applier's call; it changes nothing that is hashed.
