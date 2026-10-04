# Request to the atx-db owner (P9 lane AL-DATA, 2026-10-04)

- **From:** atx P9 sprint, lane AL-DATA. This is a doc-only lane (Ruling W3-18).
- **To:** the atx-db owner (stage owner MKT for part A; TXT, fundamentals and 13F owners for part B).
- **Ready to hand over as is.** P9 edits nothing under `atx-db/` and opens no atx-db data. Every citation below was
  read from source with `git show <ref>:<path>` inside `C:/atx-wt/pool-2`.
  - The pool-2 working tree (HEAD `f6444417`, branch `feat/platform-p9-20261003`) has no `atx-db/src/atx_db/alpha_panel/`
    package: `git ls-tree HEAD` lists 0 files there.
  - Lines are therefore cited at two committed refs: `main` = `dec03454` and `feat/tier1-v3-warehouse` = `0e2fa077`
    (shown as "wh").
  - Your working tree in `C:/atx` was not read. Re-locate lines there by function name.
- **One rule runs through every item.** A row stamped `available_at = A` may hold only facts that were public at A.
  The engine reads a stage row at role session t only if `available_at < date(t-1) 22:00 UTC`, where t-1 is the
  previous role session (`atx-engine/include/atx/engine/research/fields/sources/stage.hpp:44-46`).
  - TRAIN is `[2020-01-01, 2024-01-01)`.
  - Nothing below needs any row dated 2024-01-01 or later to build a TRAIN field.

Priority order: **A** (small, protects existing consumers), then **B1** (the one family that clears our bar on paper),
then **B3** (platform part only), then **B2** (deferred: its only consumer is blocked by our bar).

---

## A. ALSIG-7: the indexes stage looks ahead (Russell and S&P-like proxies)

### A.1 Where (stage `indexes`, `atx-db/src/atx_db/alpha_panel/indexes.py`, function `intervals()`)

| what | main `dec03454` | wh `0e2fa077` |
|---|---|---|
| docstring promise: "`available_at` of a membership = the rank date's 22:00 UTC mark (every input is known by then)" | `:33-34` | `:33-34` |
| `last_session` per line, loaded from `security_master/lines.parquet` | `:185-187` | `:198-200` |
| `last_session` = `max(tradingDate)` over the whole vendor history (`alpha_panel/security_master.py`) | `:140` | `:167` |
| `mark` = rank date at `C.MARK_HOUR_UTC` (22, `common.py:35` on main) | `:272` | `:285` |
| `end` = next recon_date, cut to `last_session` when that is earlier | `:274-277` | `:287-290` |
| **the line is dropped** when `end < eff` (eff = first session after recon_date) | `:278-279` | `:291-292` |
| each kept row is written with `effective_to = end` and `available_at = mark` | `:280-283` | `:293-296` |
| `recon_summary`: `alive_prev` filters prior members by `last_session > recon_date`; the row is stamped `mark` | `:284`, `:294` | `:297`, `:307` |
| existing test that pins `effective_to` = last session on a row stamped at the rank date (`atx-db/tests/test_alpha_panel_indexes.py`) | - | `:62-77`, assertion `:72` |

`intervals()` serves `R1000P`, `R2000P`, `R3000P` and `SP500P`. Every finding below applies to all four.

### A.2 What goes wrong

1. **Presence (ALSIG-7; high).** Take a line selected on rank date R whose `last_session` falls before R's
   `effective_from`.
   - The line is never written (`:278-279` / wh `:291-292`).
   - Every other line of R is stamped available at the rank date 22:00 UTC.
   - A reader at any session after the rank date therefore sees the line missing, and the gap reveals a delisting that
     has not happened yet. The window runs from the rank date + 2 sessions to the session before recon_date, so the
     reveal comes 4-7 weeks early.
   - The engine field `russell_recon` reads the gap as NaN instead of +1 / -1 / 0. Its registry row therefore carries a
     `non_pit_aspects` entry, and the IC runner refuses it (`atx-impl/src/strategy_ic_admission.cpp:583-586` at
     pool-2 `f6444417`).
2. **Values: `effective_to` (medium; found while reading the same function).**
   - A kept line whose `last_session` falls before the next recon_date gets `effective_to = last_session`, on a row
     stamped at the rank date. Reading `constituents.parquet` as of that stamp gives the exact future delisting date,
     up to about 13 months early.
   - For the 2023 reconstitution (rank date 2023-04-28), `effective_to` is computed from the 2024 recon date and from
     any `last_session` in 2024 or later. So a row dated inside TRAIN carries a post-2023 fact.
   - `russell_recon` reads only `index_id, security_id, rank_date, recon_date, available_at`, so it is not hit. Any
     consumer that reads `effective_to` as of its stamp is.
3. **Aggregates: `recon_summary` (low).** `additions`, `deletions`, `turnover_oneway` and `turnover_cap` are computed
   with `alive_prev` and stamped at the rank date. Between the rank date and recon_date they count prior members whose
   delisting has not happened yet.

### A.3 The point-in-time rule we ask for (exact)

- **R1. Selection vintage.** The constituents rows of reconstitution R that are stamped at the rank-date mark hold the
  complete member set selected on the rank-date snapshot.
  - No line is dropped.
  - No column of these rows depends on `security_master.last_session` or on any session after the rank date.
  - A line whose last session falls before `effective_from` keeps its row, with `effective_from` as scheduled.
- **R2. Scheduled end.** In that vintage, `effective_to` is the scheduled end: the last session on or before the next
  reconstitution's recon_date in the published schedule (`RUSSELL`, `sp_schedule`; these are calendar constants). It
  is NULL for the last reconstitution.
- **R3. Exit vintage.** A line's early exit is a separate fact, stamped when it becomes knowable.
  - Form: either a new vintage row of `(index_id, security_id, recon_date)` with `effective_to` = the line's last
    session, or an `exits.parquet` file with columns `(index_id, security_id, recon_date, last_session, available_at)`.
  - Stamp: `available_at` = 22:00 UTC of the first calendar session after the line's last session, which is the first
    close at which the missing print can be seen.
  - Consumers take the latest vintage with `available_at <` their mark, the same as the `compustat_std` as-of grouping.
- **R4. Summary.** `recon_summary` counts stamped at the rank date use only rank-date member sets, plus exits already
  stamped before the rank date. Realised counts that need later exits go in separate columns stamped at the recon-date
  mark or later.
- **R5. Daily membership (defensive).** The membership row of session s carries `available_at` = max(rank-date mark,
  22:00 UTC of the session before s). Then a selection by stamp alone can never reveal that a line is absent from a
  future session.
- **R6. Version.** The output meaning changes, so bump the rules to `russell-proxy-v2` and `sp500-proxy-v2`.
  - The engine reader pins `russell-proxy-v1` today
    (`atx-engine/include/atx/engine/research/fields/sources/russell_proxy.hpp:32-33`).
  - It refuses a manifest rule list that names two versions of one family (`sources/stage.hpp:55-59`), so the bump
    is what tells consumers the meaning changed.
- **Effect on TRAIN.** Under R1-R3, no row stamped before 2024-01-01 depends on a fact dated 2024-01-01 or later.
  Today the 2023 reconstitution's `effective_to` does.

### A.4 A test that catches it (atx-db pytest, synthetic, pure `intervals()`, no data)

Add `test_intervals_are_point_in_time_at_their_stamp` to `atx-db/tests/test_alpha_panel_indexes.py`.

**Fixture.** A calendar holding the sessions named below, and three R2000P reconstitutions:

| reconstitution | rank date | recon date | effective from |
|---|---|---|---|
| R1 | 2020-05-08 | 2020-06-26 | - |
| R2 | 2021-05-07 | 2021-06-25 | 2021-06-28 |
| R3 | 2022-05-06 | 2022-06-24 | - |

R2's members:

| line | `last_session` | role |
|---|---|---|
| X (sid 7) | 2021-06-01 | the planted line: delists between the rank date and the effective date |
| Z (sid 8) | 2021-09-15 | delists after the effective date, before the next recon |
| Y (sid 9) | None | alive |

**Assertions.**
1. **The planted member still appears.** A row `(R2000P, 7, recon 2021-06-25)` exists, stamped `available_at` =
   2021-05-07 22:00, with `effective_from` 2021-06-28. *Fails today* (`:291-292`).
2. **Scheduled ends.** In the rank-date vintage, `effective_to` of X, Z and Y is the scheduled end (the last session
   on or before 2022-06-24). It is not 2021-06-01 or 2021-09-15. *Fails today* (the `:72` behaviour).
3. **Exit stamps.** X's exit is stamped 2021-06-02 22:00 UTC and Z's 2021-09-16 22:00 UTC. No exit row is stamped
   earlier.
4. **Censoring invariance (the generic check).**
   - Cuts: T in {2021-05-07 22:00, 2021-05-20 22:00, 2021-06-15 22:00, 2021-10-01 22:00}.
   - First run: `intervals()` on the full `last_session` map.
   - Second run: the same map censored at T. Any line whose exit stamp would be at or after T gets `None`.
   - For each T, the rows and the `recon_summary` rows with `available_at < T` must be equal across the two runs.
   - *Fails today at T = 2021-05-20:* X is missing in the full run and present in the censored run.
5. **S&P-like proxy.** The same checks hold for one `SP500P` quarterly pair.

**Teeth.** On the current code, 1, 2 and 4 fail. The existing assertion at `test_alpha_panel_indexes.py:72`
(`effective_to == 2020-06-30` on a row stamped 2020-05-08 22:00) pins channel 2. You rewrite it as part of the v2
meaning. That is a deliberate rule change with a new rule version, not an edit of a pinned value to make a test pass.

### A.5 Acceptance on arrival

These checks are run by root from the manifest and receipt. They read no row values and nothing stamped 2024 or later.

- The manifest `rule` lists `russell-proxy-v2` and `sp500-proxy-v2`, and no v1.
- In the receipt, `counts_on_recon` for each `(index_id, recon_date)` equals that reconstitution's rank-date-vintage
  row count (nothing dropped).
- The new atx-db test is green in your suite.

The engine follow-up is P9/P10's job, not yours:
- re-pin the stage and move `kIndexesStage` to v2;
- drop the `russell_recon` `non_pit_aspects` entry and re-derive its row;
- add a fixture gtest in which a line delisting between the rank date and the effective date reads its move
  throughout the event window.

The `rank(russell_recon)` string stays `dropped` (ALSIG-5). The fix makes the field point in time; it does not queue
a trial.

---

## B. OD-P9-6: three data asks

### Rules common to B1-B3

- **C1. Clock.** Every row carries `available_at`, a UTC instant: the earliest instant at which every column of the
  row was public. A NULL `available_at` means the row is never usable; it is excluded with a reason and counted.
- **C2. Vintages.** Corrections, amendments and restatements are new rows with their own stamp. Nothing is
  overwritten.
- **C3. Seal by partition.** Files are partitioned by the year of `available_at` (`<stage>/<name>/year=YYYY/*.parquet`),
  and each part is listed in the manifest `files` object with its bytes and sha256.
  - Then a TRAIN build never opens a part that holds a row stamped 2024-01-01 or later. The 13F reader already
    applies this rule to holdings parts (`sources/thirteenf.hpp:15-16`).
  - With a single all-years file, the reader has to decode post-seal rows just to drop them.
- **C4. No selection on the future.** The population (which CIKs or filers are landed) is chosen without any fact
  dated 2024-01-01 or later.
  - The legacy text scope selects CIKs linked to a member line in 2019-2026 (`filing_text.py:61-62` on main,
    `MEMBER_YEARS = (2019, 2026)`).
  - Under that scope, whether a TRAIN line has a value can depend on its membership after 2023.
- **C5. Manifest.** The manifest follows the engine `PinnedStage` contract (`sources/stage.hpp:3-7`, `:70-73`):
  - `schema`;
  - a versioned `rule` family name;
  - `clock_rule` and `staleness_days`;
  - `files` as `{rel: {bytes, sha256}}`;
  - complete status.
- **C6. Leavers stay.** Rows of entities that later disappear (delisted issuers, withdrawn advisers) are kept.

### B1. F1, 10-K / 10-Q text for Lazy Prices: priority 1

**Today.**
- `alpha_panel/filing_text.py` (main `:1-30`, `:58-65`) lands only the 10-K family and 20-F, filed from 2018-01-01.
  It extracts the business / risk / MD&A sections and does not keep the full document.
- `alpha_panel/text_features.py` (stage `text`, schema `atx.alpha-panel.text-features/v1`; main `:1-28`, `:55-60`)
  writes per-filing values per section: `{s}_sim_cosine`, `{s}_sim_jaccard` and `{s}_sim_minedit`. The prior filing
  is one filed 180-550 days earlier, and the staleness is 400 days.
- `text_native` (wh `docs/datasets/text_native.md`) covers annual filings only, for 2019-2026. Its measured coverage
  is "unmeasured/FAIL".
- The P9 literature review counted 7% of 10-K / 10-Q landed.

**Forms.**
- Original 10-K-family filings and **10-Q**. The 10-Q is not landed today, and the registered string reads "latest
  10-K or 10-Q".
- 10-K/A and 10-Q/A may be landed but are never the current or the prior filing.

**Columns.** Key `(cik, accession)`, unique.

| column | content |
|---|---|
| `cik` | BIGINT |
| `accession`, `form` | |
| `fiscal_period` | FY / Q1 / Q2 / Q3 |
| `report_date` | fiscal period end |
| `filing_date` | |
| `available_at` | authenticated EDGAR acceptance in UTC; NULL when unproven |
| `prior_accession`, `prior_report_date`, `prior_available_at`, `prior_gap_days` | the comparator filing |
| `doc_sim_cosine` | **new:** term-frequency cosine of the full primary document against the prior. This is the Cohen-Malloy-Nguyen headline measure [verify against the paper] |
| `mdna_sim_cosine`, `risk_sim_cosine` | 10-K Items 7 / 1A; 10-Q Part I Item 2 / Part II Item 1A |
| `doc_words`, `prior_doc_words` | |
| `reason` | missingness |

Use today's tokenizer (`text_features.WORD_RE`: lowercase, no stemming, no stop words).

**Comparator.** The issuer's original filing of the same form family and the same fiscal period one fiscal year
earlier: a 10-K against the prior 10-K, a Q2 10-Q against the prior year's Q2 10-Q. It must be accepted strictly
earlier, within today's 180-550-day gap (the `text_native` "previous fiscal year" rule).

**Availability.** `available_at` = the acceptance instant. Every value is known at its own filing's acceptance,
because the prior filing is always earlier (`text_features.py:25-26`). Check `prior_available_at < available_at` on
every row.

**History depth.**
- **TRAIN** (first session 2020-01-02):
  - with a 400-day freshness, the latest filing a TRAIN session can read was accepted on or after 2018-11-28;
  - its comparator can be up to 550 days older, from 2017-05-27;
  - so TRAIN needs filings accepted **2017-05-01 to 2023-12-31**.
- **OD-3 history block** (2015-2019; first session 2015-01-02):
  - with the same arithmetic, current filings come from 2013-11-28 and comparators from 2012-05-27;
  - so the block needs filings from **2012-05-01**.
- **Ask:** filings accepted 2012-01-01 to 2023-12-31.
  - Priority band 1: 2017-05-01 to 2023-12-31.
  - Priority band 2: 2012-01-01 to 2017-04-30.
  - Rows from 2024 on are yours to land. They sit in parts with year 2024 or later, which a TRAIN build never opens.
- **Population (C4):** every original 10-K / 10-Q filer whose CIK links, at the filing date, to any engine-universe line
  in that year.

**Partition (C3):** `text/features/year=YYYY/features.parquet`, by the year of `available_at`.

**Acceptance on arrival.**
- Root:
  - (i) the manifest follows C5 with a new rule (for example `text-features-v2`);
  - (iv) every part's name matches the years in its manifest entry.
- Owner prints:
  - (ii) coverage per calendar year, 2013-2023: the share of engine-universe CIK-years with at least one usable 10-K
    row and at least three usable 10-Q rows. Gate: at least 90% for 2018-2023 (`text_native`'s own gate is at least
    90% from 2020) and at least 80% for 2013-2017;
  - (iii) on every row: `available_at` is not null, `prior_available_at < available_at`, the comparator gap lies in
    the band, and both filings are originals.
- Engine:
  - (v) the planted-leak gtest of `builder-designs.md` section 1 passes on a two-filing synthetic fixture written by
    your code.

### B2. F2, Form ADV private-fund flag joined to the 13F filer CIK: priority 3 (deferred)

**Why deferred.** The only consumer, `hf_crowd`, fails our bar (expected correlation about .2 > .1; PM ruling
ALSIG-4). It may not be queued without a new PM ruling. We specify the ask so it is ready, and ask you to schedule it
last.

**Source.** SEC Form ADV data:
- Part 1A Item 5.F (regulatory AUM);
- Item 7.B (adviser to a private fund);
- Schedule D Section 7.B.(1), per private fund: fund type ("hedge fund", ...) and gross asset value.

It must be taken **per filing or per dated public snapshot**, including advisers that later withdrew (ADV-W).
- Back-filling today's IAPD snapshot is refused. It adds survivorship and classification look-ahead together.
- [verify] which public archive holds Schedule D 7.B.(1) per filing back to 2012.

**Output.** `adv_filer_type/year=YYYY/adv_filer_type.parquet`, key `(filer_cik, crd, available_at)`.

| column | content |
|---|---|
| `filer_cik` | BIGINT, the 13F filer CIK as in `thirteenf` `filings.filer_cik` |
| `crd`, `sec_file_no` | adviser identifiers |
| `adv_filing_id`, `adv_filing_date` | the ADV filing |
| `available_at` | publication instant (below) |
| `raum_usd` | regulatory AUM |
| `private_fund_adviser` | BOOL |
| `n_private_funds`, `n_hedge_funds` | counts |
| `hedge_fund_gav_usd`, `private_fund_gav_usd` | gross asset values |
| `withdrawn` | BOOL |
| `match_method` | `adv_cik` / `name_address` / `manual` |
| `match_score` | |

**Join.**
- Direct where the ADV lists the adviser's CIK. [verify: the Form ADV as amended in 2016, effective October 2017, asks
  for CIK numbers in Item 1.]
- Before that, an audited match on name and address.
- One CIK may map to several CRDs and the reverse. Keep every pair; the engine aggregates over a CIK's CRDs.

**Availability.**
- `available_at` = the instant the vintage was public: the per-filing timestamp where the archive has one; otherwise
  00:00 UTC on the day after the SEC published the first monthly file that contains it.
- The engine joins 13F quarter P to the latest vintage with `available_at < V(P)`, where V(P) is the 13F visibility
  instant (`sources/thirteenf.hpp:9-11`).
- TRAIN's last usable 13F quarter is 2023q3, filed by 2023-11-14, so TRAIN needs only vintages stamped before
  2024-01-01.

**History depth.**
- TRAIN: vintages from 2018-01-01 to 2023-11-30. ADV annual amendments fall about 90 days after fiscal year end.
- OD-3 block: from 2014-01-01.
- Ask from 2012-01-01. The 13F data is in house from 2013q2.

**Acceptance on arrival.**
- (i) The manifest follows C5.
- (ii) Each quarter, 2013q2-2023q3: at least 95% of 13F reported value has a filer CIK that is either mapped to a CRD
  or marked "not an ADV filer" (banks, insurers and pensions are legitimately unmapped). This gate is our proposal.
- (iii) A hand-checked audit of 100 name/address matches, with the false-match rate printed.
- (iv) Withdrawn advisers are present in 2013-2023.
- (v) No vintage's `available_at` precedes its `adv_filing_date`.

### B3. F10, SG&A / R&D history in the fundamentals export: priority 2 (platform part)

**Why.** Our F10 builder (`bm_int`) fails our bar and will not be queued (`builder-designs.md` section 3). We still
ask for the platform part: every engine fundamentals field needs a pinned, year-partitioned `compustat_std`.

**Today** (wh `docs/datasets/compustat_std.md`):
- `funda` carries `xsga` (fiscal year; `compustat_std.py:178`), `xrd` (`:207`) and `gdwl` (`:199`), with vintage
  rows, `available_at`, `first_reported` and `vintage_kind` (doc `:12-13`, `:26-40`), plus as-of macros (`:45-50`).
- `available_at` runs from 2010-01-05 (doc `:17`).
- There is one `funda.parquet` for all years, so it holds rows stamped up to 2026.

**Columns** (funda, annual; keys unchanged, `(cik, datadate, accession, available_at)`):
- `cik, datadate, fyear, fyr, accession, available_at, vintage_kind, first_reported`;
- `xsga, xrd, gdwl`;
- `seq, ceq, pstk, txditc, at, lt` (for book-equity checks);
- `fin_template, curcd, fx_converted`.

**Partition (C3)** by the year of `available_at`, each part pinned in the manifest.

**History depth.**
- SG&A for every fiscal year ending 2009-01-01 or later (the XBRL era), up to fiscal years whose 10-K was accepted
  before 2024-01-01.
- Optional: fiscal years 2001-2008, as reported, if you can source them. They cut the weight of the
  perpetual-inventory starting stock at the OD-3 block's start from about .33 to about .07 (decay .2: 0.8^5 vs
  0.8^12).

**CPI.** The paper deflates SG&A by the CPI. If CPI-U NSA (BLS CUUR0000SA0) is not already in house, add it monthly,
with its release instant as `available_at`. This series is not revised.

**Availability.**
- The row's own `available_at` (house rule). Restatements are new vintages.
- The engine takes, per `(cik, datadate)`, the latest vintage with `available_at <` its mark (the `funda_asof`
  macro).

**Acceptance on arrival.**
- (i) The manifest follows C5, with parts.
- (ii) `xsga` is finite on at least 85% of first-reported rows of fiscal years 2009-2022 on non-financial templates.
  This gate is our proposal.
- (iii) `xsga >= xrd` on at least 95% of rows where both are present (`xsga` includes `xrd` under standard
  practice).
- (iv) No part holds a row stamped outside its year.

---

## What P9 never needs from you

- No row stamped 2024-01-01 or later.
- No edit made on our behalf to any stage other than those named here.
- No raw data shared with the sprint. Root reads only manifests, receipts and part names before any engine build.
