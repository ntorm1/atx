# Request to the atx-db owner (P9 lane AL-DATA, 2026-10-04; fix round 1)

**DRAFT. Do not hand this over.** Fix round 2 was stopped by the owner before any major re-review finding (RM1-RM3) was fixed; A0 below is the superseded round-1 text (see `task-AL-DATA-report.md`, "Fix round 2 (stopped by the owner)").

- **From:** the atx P9 sprint, lane AL-DATA. This lane writes documents only (Ruling W3-18).
- **To:** the atx-db owner. Part A goes to stage owner MKT, panel U3 and security_master. Parts B1 / B3 go to the TXT
  and fundamentals owners.
- **Ready to hand over.** P9 changes nothing under `atx-db/` and opens no atx-db data.
- **Citations.** Every citation was read from source with `git -C C:/atx-wt/pool-2 show <sha>:<path>` and is pinned
  by SHA:
  - `main` = `dec03454`;
  - "wh" = `0e2fa077`.
  - Every cited source file is byte-identical at the later warehouse tips `c0eb2f74` and `75efb231`
    (`git diff --stat` is empty).
  - `docs/datasets/text_native.md` does not exist at `0e2fa077`. It is cited at `c0eb2f74`.
  - Your working tree in `C:/atx` was not read.
- **The rule behind every item.** A row stamped `available_at = A` holds only facts that were public at A.
  - The engine reads a stage row at role session t only if `available_at < date(t-1) 22:00 UTC`
    (`atx-engine/include/atx/engine/research/fields/sources/stage.hpp:44-46`).
  - TRAIN is `[2020-01-01, 2024-01-01)`.

**Priority.**
1. **Part A** is the request. **A0 (the identity bridge check) comes first**, because only you can run it (PM ruling
   ALD-6). Then come the four look-ahead channels in the indexes stage and its inputs.
2. **B1 and B3** are low priority. Our own bar does not admit the fields that would use them (the numbers sit with
   each part).
3. **B2** (the Form ADV join) is deferred and dropped from this file. Its only consumer, `hf_crowd`, fails our bar
   (expected correlation .2 > .1; PM rulings ALSIG-4, ALD-5). If it is ever needed, it will be written as a delta on
   your existing `thirteenf_filer_type` stage (main `alpha_panel/filer_type.py:1-25`, `adv.py:1-26`).

---

## A. Look-ahead: the identity bridge (A0) and the indexes stage (ALSIG-7 and three more channels)

### A0. Identity bridge `identity-bridge-v2-pit`, strict (r4) tier (first item; PM ruling ALD-6)

**1. What the source shows** (main `dec03454`):
- `export/identity-bridge-v2-pit` is the strict and name tiers. The 2026-snapshot backfill tier is excluded
  (`alpha_panel/identity_table.py:25-29`, `:172-176`). The strict tier is the r4 links of
  `identity/links_combined.parquet` (`:3`).
- The r4 rehearsal builder `identity_links.build_rehearsal` reads two 2026 snapshots
  (`atx-db/src/atx_db/identity_links.py:840-852`):
  - the Nasdaq Trader symbol directory (`bridge._read_directory`);
  - the SEC company-ticker list (`ir.read_sec_ticker_snapshot`).
- The directory's classes feed the bridge's class guards and rule-5 history (`market_owner_bridge.py:1748`,
  `:1842-1845`). Lines linked through the current SEC ticker carry a modelled `available_at` at their first trade
  date (`:1770-1810`).
- Columns of `links.parquet` that can be affected (`identity_table.py:188-192`): `sr_id, cik, start, end_incl,
  available_at, primary, class_status`.
  - `primary` is chosen by prior 63-session dollar volume among an issuer's linked lines (`:111-112`).
  - So adding or removing one link can also flip another line's `primary`.

**2. Why it matters to us.**
- The accepted book's role universe `linked-operating-v3` reads this bridge, and so do 50 of its 84 fields (35 with
  source `identity_bridge`, 17 with `sec_identity_bridge`).
- A link that exists, or has its dates, only because a name appears in a 2026 list would be survivorship in a
  2020-2023 backtest.

**3. The check we ask for.**
- Rebuild the strict tier with **both** 2026 snapshots empty (or absent).
- Diff the result's `links.parquet` against the published `identity-bridge-v2-pit`, restricted to rows with
  `start < 2024-01-01`.
- Report:
  - the count of added, removed and changed rows by year of `start`;
  - for changed rows, the count by column changed.
- **Zero differing rows = no dependence.**
- The check opens nothing dated 2024-01-01 or later on our side. You run it entirely inside atx-db, and we receive only
  the counts.

**4. If the diff is non-zero.**
- Send the list of affected `sr_id` / `cik`, as identifiers only.
- The point-in-time rule we ask for: **a link row may use only sources dated at or before its `available_at`.** A
  2026 snapshot can be evidence only for a row stamped at or after the snapshot's own date.

**5. Acceptance test for the fix** (atx-db pytest, synthetic):
- **Planted line.** Its last symbol appears in the 2026 company-ticker list and in the 2026 directory, but no dated
  source links it before T. The strict tier then has no row for it with `available_at < T`.
- **Planted class.** A line whose class differs between the 2026 directory and its dated FINRA name evidence takes its
  dated class in every row stamped before 2026.
- **Re-run of check 3** on the fixed build, comparing it with and without the two snapshots, gives zero differing
  rows with `start < 2024-01-01`.
- **Version.** The bridge's manifest carries a new rule version, and P9 re-pins the role and the fields to the new
  manifest SHA-256.

### A.1 Where

Stage `indexes`, file `atx-db/src/atx_db/alpha_panel/indexes.py`:

| what | main `dec03454` | wh `0e2fa077` |
|---|---|---|
| docstring: "`available_at` of a membership = the rank date's 22:00 UTC mark (every input is known by then)" | `:33-34` | `:33-34` |
| `recon_summary` registered in `LAKE_STAGES` with `"clock": None` | `:75` | `:77` |
| `snapshot()` eligibility filter on the rank-date panel: `security_type`, `is_adr`, `is_adr_likely`, `is_fpi`, `is_lp`, `is_royalty_trust`, `is_operating`, `is_index` | `:156-159` | `:169-172` |
| `last_session` per line, from `security_master/lines.parquet` | `:185-187` | `:198-200` |
| a reconstitution is built only when `recon_d < cal[-1]` (the vendor calendar) | `:198`, `:218` | `:211`, `:231` |
| receipt `constituent_rows` (a total only) and `counts_on_recon` (= summary `lines`) | `:233`, `:249` | `:246`, `:262` |
| `eff = cal[bisect_right(cal, recon_date)]`; `nxt` = the next recon_date from the data | `:269-270` | `:282-283` |
| `mark` = rank date at `C.MARK_HOUR_UTC` (22, `common.py:35`) | `:272` | `:285` |
| `end` = `nxt`, cut to `last_session` when earlier | `:274-277` | `:287-290` |
| **the line is dropped** when `end < eff` | `:278-279` | `:291-292` |
| kept row: `effective_to = end`, `available_at = mark` | `:280-283` | `:293-296` |
| `recon_summary`: `alive_prev` filters prior members on `last_session > recon_date`; `lines = len(members)`; stamped `mark` | `:284-294` | `:297-307` |
| test pinning `effective_to = last_session` on a row stamped at the rank date (`atx-db/tests/test_alpha_panel_indexes.py`) | `:62-77`, assertion `:72` | `:62-77`, assertion `:72` |

Other files:

| what | main | wh |
|---|---|---|
| `alpha_panel/security_master.py` docstring: the directory snapshot (2026-09-18) is "current lines only. Not point in time" | `:9-11`, `:19` | `:9-11`, `:19` |
| directory read only for lines whose `last_session` is within 14 days of the vendor file's last date | `:144-148` | `:171-175` |
| `last_session = max(tradingDate)` over all history | `:140` | `:167` |
| `delisting_date` NULL while alive (`DEAD_BEFORE` 2026-09-01) | `:163`, `:32` | `:190`, `:32` |
| `alpha_panel/panel.py`: `security_type` falls back to `directory_type`; `security_type_basis` names it | `:226-235` | `:226-235` |
| `alpha_panel/panel.py`: `is_etf` ORs in `directory_etf` on every row | `:236` | `:236` |
| `alpha_panel/panel.py`: `is_adr`, `is_lp`, `is_common` | `:237`, `:240`, `:250-251` | same |
| `alpha_panel/panel.py`: `is_operating` (needs `NOT is_etf`) | `:259-262` | same |
| `alpha_panel/panel.py`: `directory_type`, `directory_etf` joined as static line columns | `:574` | same |

`intervals()` serves `R1000P`, `R2000P`, `R3000P` and `SP500P`, and `snapshot()` serves both families.

### A.2 What goes wrong: four channels

**1. Presence (ALSIG-7; high).**
- A line selected on rank date R whose `last_session` falls before R's `effective_from` is not written
  (`:278-279` / wh `:291-292`). Every other line of R is stamped at the rank date's 22:00 UTC.
- From the first session that can read R onward, the missing line reveals a delisting that has not happened yet.
- The lead runs from one session up to about 7 weeks for the Russell proxies ranked in May 2018-2022, about 8 weeks
  from 2023 (April rank day), and up to about 3 weeks for SP500P (rank at the end of February / May / August /
  November, effective after the third Friday of the next month; `indexes.py:26-27`).
- The engine field `russell_recon` reads the gap as NaN. Its registry row declares this, and the IC runner refuses
  the field (`atx-impl/src/strategy_ic_admission.cpp:583-586` at pool-2 `f6444417`).

**2. Values: `effective_to` (medium).**
- A kept line whose `last_session` falls before the next recon_date gets `effective_to = last_session`, on a row
  stamped at the rank date. That is the exact future delisting date, up to about 13.5 months early.
- For the 2023 reconstitution (rank date 2023-04-28), `effective_to` is computed from the 2024 recon_date and from
  any `last_session` in 2024 or later. A row stamped inside TRAIN therefore carries a fact dated 2024 or later.
- The value also depends on how far the vendor calendar extends: `nxt` comes from the data (`:270`), and a
  reconstitution is built only when `recon_d < cal[-1]` (`:198`, `:218`).
- `russell_recon` does not read `effective_to` (`russell_proxy.cpp:82-83` reads five columns). Any consumer that reads
  it as of its stamp is exposed.

**3. Aggregates: `recon_summary` (low).**
- `alive_prev` keeps only prior members whose `last_session` is after recon_date, and the row is stamped at the rank
  date. A prior member that delists between the rank date and recon_date is therefore:
  - not counted as a deletion;
  - counted as an **addition** if it is reselected (`cur - alive_prev`, `:286`).
- A build censored before the delisting counts it as continuing. The same holds for `turnover_oneway` and
  `turnover_cap`.
- `LAKE_STAGES` registers the file with `"clock": None` (`:75`), so the lake view applies no stamp to it at all.

**4. Eligibility: the 2026 directory snapshot (high; PM ruling ALD-3).**
- `snapshot()` selects members on the rank-date panel through `security_type`, `is_adr`, `is_adr_likely`, `is_lp` and
  `is_operating` (`:156-159`).
- On every panel row:
  - `security_type` falls back to the Nasdaq Trader directory type whenever the dated FINRA type is stale, NULL,
    `unknown` or `common_unverified` (`panel.py:226-235`);
  - `directory_etf` is ORed into `is_etf` (`:236`), which gates `is_operating` (`:259-262`).
- Those `directory_*` columns come from one 2026-09-18 snapshot that covers only lines still trading in September 2026
  (`security_master.py:9-11`, `:144-148`). The module's own docstring calls them "Not point in time".
- So whether a line was eligible at a 2018-2023 rank date can depend on whether it survived to 2026 and on how it was
  classified in 2026. Both are post-seal facts.
- Through company ME ranks and the p1000 breakpoint, eligibility also moves other lines' R1000 / R2000 assignment.
- Channels 1-3 are in `intervals()`. Channel 4 is in its inputs, so fixing `intervals()` alone does not make the
  stage point in time.
- Related, not used by `snapshot()`: the panel also carries `delisting_date` (`panel.py:576`) as a static line column
  on every session row, which is the future last session. Any panel consumer that reads it as of a session sees the
  future.

### A.3 The point-in-time rule we ask for

- **R1. Selection vintage.** The constituents rows of reconstitution R stamped at the rank-date mark hold the complete
  member set selected on the rank-date snapshot. No line is dropped, and no column of these rows depends on
  `last_session`, `delisting_date` or any session after the rank date.
- **R2. Scheduled end.** In that vintage, `effective_to` = the next reconstitution's recon_date from the schedule
  constant (`RUSSELL`, `sp_schedule`). `effective_from` = the first session after recon_date on the exchange
  calendar.
  - Both are taken whatever the extent of the vendor data, including for the last reconstitution built.
  - Equally acceptable: `effective_to` NULL (open) in the rank-date vintage, closed by a vintage stamped at the next
    reconstitution's rank-date mark.
  - Not acceptable: any value that changes when the vendor calendar is extended.
- **R3. Exit vintage.** A line's early exit is a separate fact.
  - Form: a new vintage row of `(index_id, security_id, recon_date)` with `effective_to` = the line's last session,
    or an `exits` file with `(index_id, security_id, recon_date, last_session, available_at)`.
  - Trigger: `delisting_date` (`security_master.py:163`, NULL while alive), or `last_session < cal[-1]`. Never
    `last_session` alone, which is never NULL (`:140`).
  - Stamp: 22:00 UTC of the first session after the last session. This is the house `delisting` convention
    (`alpha_panel/delisting.py:23`, `:134`).
  - Consumers take the latest vintage with `available_at <` their mark.
- **R4. Summary.**
  - `recon_summary` rows stamped at the rank date count only rank-date member sets and exits stamped before the rank
    date.
  - Realised counts that need later exits are separate vintage **rows** with their own `available_at` (one stamp per
    row, C1).
  - Register `recon_summary` in `LAKE_STAGES` with its clock (`available_at`), not `None`.
- **R5. Daily membership.** The membership row of session s carries `available_at` = max(rank-date mark, 22:00 UTC of
  the session before s). It is built only from vintages (selection rows and exits) stamped at or before its own stamp.
- **R6. Version.** Bump the rules to `russell-proxy-v2` and `sp500-proxy-v2`.
  - The engine reader pins `russell-proxy-v1` (`russell_proxy.hpp:32-33`).
  - It refuses a rule list that names two versions of one family (`stage.hpp:55-59`).
- **R7. Eligibility (channel 4).** Choose one of these three, and tell us which:
  - (a) restrict index eligibility to dated evidence: `security_type_basis = 'finra_name'` (or another dated basis),
    and `is_etf` without `directory_etf`;
  - (b) land a dated directory history (daily Nasdaq Trader files, each stamped at its own date) and read it as of the
    rank date;
  - (c) give a written proof that the directory fields used are time-invariant for the lines they decide, with the
    count of (rank date, line) eligibility decisions per reconstitution, 2018-2023, that the directory decided.
- **R8. Seal by partition (recommended).** Write `constituents` (and the exit vintages) in parts by the year of
  `available_at`, each listed in the manifest. Today the engine reader decodes rows stamped 2024-2026 in
  `constituents.parquet` and only then drops them.
- **Scope of the TRAIN claim.** Under R1-R5, no output of `intervals()` stamped before 2024-01-01 depends on a fact
  dated 2024-01-01 or later. That claim does **not** cover the eligibility inputs: channel 4 remains until R7 is met.

### A.4 A test that catches channels 1-3 (atx-db pytest, synthetic, no data)

Add `test_intervals_are_point_in_time_at_their_stamp` to `atx-db/tests/test_alpha_panel_indexes.py`. The signature of
the fixed `intervals()` is yours; the test passes it the schedule constant, an exchange calendar and the per-line
`(last_session, delisting_date)`.

**Calendar.** Every business day (Monday to Friday) from 2020-05-01 to 2022-12-30.

**Reconstitutions (R2000P)** with their members. "Alive" means `last_session` = the last session of the data and
`delisting_date` NULL; it is never `None`, matching production.

| reconstitution | rank date | recon date | effective from | members (`last_session`) |
|---|---|---|---|---|
| R1 | 2020-05-08 | 2020-06-26 | 2020-06-29 | X (sid 7, 2021-06-01), W (sid 6, 2021-06-10; in R1 only), V (sid 5, alive) |
| R2 | 2021-05-07 | 2021-06-25 | 2021-06-28 | X (2021-06-01: the planted line), Z (sid 8, 2021-09-15), Y (sid 9, alive), V (alive) |
| R3 | 2022-05-06 | 2022-06-24 | 2022-06-27 | V, Y (alive) |

The schedule constant gives the next recon after R3 as 2023-06-23.

**Assertions.**
1. **Presence.** A row `(R2000P, 7, recon 2021-06-25)` exists with `available_at` 2021-05-07 22:00 and
   `effective_from` 2021-06-28. *Fails today* (`:278-279`).
2. **Scheduled ends.**
   - R1 rows have `effective_to` 2021-06-25, R2 rows 2022-06-24, R3 rows 2023-06-23 (or NULL by the R2 alternative).
   - None of them equals a `last_session`.
   - *Fails today:* X 2021-06-01, W 2021-06-10 and Z 2021-09-15 in their rank-date rows; R3's V and Y take the data's
     last session.
3. **Exit stamps.** Exits are stamped 2021-06-02 22:00 (X), 2021-06-11 22:00 (W) and 2021-09-16 22:00 (Z). V and Y
   have no exit row.
4. **As-of replay.**
   - Cuts: T in {2021-05-10 22:00, 2021-05-20 22:00, 2021-06-15 22:00, 2021-10-01 22:00, 2022-05-20 22:00}.
   - As-of inputs at T:
     - the reconstitutions whose rank-date mark is before T;
     - for a line whose exit stamp is before T, its true `last_session` and `delisting_date`;
     - for every other line, `last_session` = the last session before date(T) and `delisting_date` NULL;
     - the schedule constant and the exchange calendar in full.
   - The stage's rows (selection vintages, exits, `recon_summary`, membership) with `available_at < T` must be equal
     between the full run and the as-of run.
   - *Fails today at T = 2021-05-20, twice:*
     - X is missing in the full run and present in the as-of run;
     - R2's `(additions, deletions)` is `(3, 0)` in the full run (X spuriously added, W not deleted) and `(2, 1)` in
       the as-of run.
5. **Data-extent truncation.** Run on full inputs with the data ending 2022-06-30 and again with it ending 2022-12-30.
   The rank-date vintage rows of R1-R3 must be identical. *Fails today:* the R3 rows of V and Y take
   `effective_to` = the data's last session.
6. **S&P-like proxy.** Checks 1-5 also hold for one `SP500P` quarterly pair.

The existing assertion `test_alpha_panel_indexes.py:72` pins channel 2. Rewriting it is part of the v2 meaning: a
deliberate rule change under a new rule version, not an edit of a pinned value to make a test pass.

**Channel 4 has no fixture test here,** because it lives in the panel inputs. R7 (a) and (b) are checked by the
receipt key in A.5. R7 (c) is the written proof.

### A.5 Acceptance on arrival

Root reads the manifest and receipt JSON only. It counts no rows and reads nothing stamped 2024 or later.

- The manifest `rule` lists `russell-proxy-v2` and `sp500-proxy-v2`, with no v1.
- A **new** receipt key, `rank_vintage_rows_on_recon`, gives the rank-date-vintage row count per
  `"<index_id> <recon_date>"`. It must equal `counts_on_recon` (= `len(members)`, `:249`, `:288`) for every
  reconstitution.
  - Today's `constituent_rows` is a total, not per reconstitution (`:233`).
  - Today's `counts_on_recon` already counts the lines that are later dropped.
- A **new** receipt key, `exit_rows`, counts exit vintages per index.
- A **new** receipt key, `directory_decided_on_rank`, counts per rank date the selected lines whose eligibility used a
  directory-derived input (`security_type_basis = 'directory_snapshot'` or `directory_etf` true). It must be 0 under
  R7 (a) or (b). Under R7 (c) it is reported beside the proof.
- The new atx-db test is green in your suite.

### A.6 What the engine does afterwards (our job, not yours)

- Re-pin the stage, and move `kIndexesStage` to v2.
- `russell_recon` carries two declared aspects (`builder-designs.md` section 5):
  - the ALSIG-7 presence aspect is dropped only once R1-R5 hold;
  - the ALD-3 eligibility aspect is dropped only once R7 is met.
- Until both hold, the field stays non-point-in-time and barred from trials.
- The `rank(russell_recon)` string stays `dropped` (ALSIG-5) either way.

---

## B. OD-P9-6 data asks (low priority)

### Rules common to B1 and B3

- **C1. Clock.** Every row carries `available_at`, a UTC instant: the earliest instant at which every column of the
  row was public. A NULL `available_at` means the row is never usable; it is excluded with a reason and counted.
- **C2. Vintages.** Corrections, amendments and restatements are new rows with their own stamp. Nothing is
  overwritten.
- **C3. Seal by partition.** Files are partitioned by the year of `available_at`, and each part is listed in the
  manifest `files` object with its bytes and sha256.
  - A TRAIN build then never opens a part that holds a row stamped 2024-01-01 or later.
  - This is analogous to the engine's 13F reader, which seals holdings parts by their calendar quarter
    (`sources/thirteenf.hpp:15-16`).
- **C4. No selection on the future.** The population is chosen without any fact dated 2024-01-01 or later.
- **C5. Manifest.** The manifest follows the engine `PinnedStage` contract (`sources/stage.hpp:3-7`, `:70-73`):
  - `schema`;
  - a versioned `rule` family;
  - `clock_rule` and `staleness_days`;
  - `files` as `{rel: {bytes, sha256}}`;
  - complete status.
- **C6. Leavers stay.** Rows of entities that later disappear are kept.

### B1. F1, 10-K / 10-Q text for Lazy Prices (low priority)

**Why low.**
- Under ruling ALD-4, `lazy_prices`' a-priori net SR_c after the literature's post-publication base rate is about
  **.09-.37** against a bar of .4 (`builder-designs.md` section 0).
- It is not queued without a new PM ruling. B1 matters only if the PM or the owner overrides that.

**Ask: extend your `text_native` stage.**
- Today (`c0eb2f74` `docs/datasets/text_native.md`):
  - it covers original annual filings and already uses "the complete original SEC annual cohort, independent of future
    member selection" (`:7-10`);
  - its coverage years are 2019..2026 (`:46-49`);
  - its measured coverage is "unmeasured/FAIL" (`:74`).
- Extensions:
  - **10-Q** originals (Part I Item 2 MD&A, Part II Item 1A risk factors).
  - **A full-document similarity:** `doc_sim_cosine`, the term-frequency cosine of the full primary document against
    the comparator, with your existing tokenizer.
  - **Years:** every original 10-K-family and 10-Q filing accepted 2012-01-01 to 2023-12-31. This is the complete
    cohort, so it meets C4 by construction.
- The output layout is yours, provided its parts are by the year of `available_at` (C3) and pinned (C5).
- Amendments may be landed but are never the current or the prior filing.

**Columns.** Key `(cik, accession)`.
- `cik, accession, form, fiscal_period, report_date, filing_date`;
- `available_at` (authenticated acceptance in UTC; NULL when unproven);
- `prior_accession, prior_report_date, prior_available_at, prior_gap_days`;
- `doc_sim_cosine`, plus the existing section columns;
- `reason`.

**Comparator.** The issuer's original filing of the same form family and the same fiscal period one fiscal year
earlier, accepted strictly earlier, within your existing 180-550-day gap.

**Availability.**
- `available_at` = the acceptance instant of the current filing. Its prior is earlier, so every value is known then.
- `prior_available_at < available_at` holds on every row.

**History arithmetic.** With a 400-day freshness (`text_native.md:46-47`) and a comparator up to 550 days older:
- TRAIN (first session 2020-01-02) needs filings accepted from 2018-11-28 and comparators from 2017-05-27.
- The OD-3 history block (first session 2015-01-02) needs filings from 2013-11-28 and comparators from 2012-05-27.
- Hence the 2012-2023 range. No filing accepted in 2024 or later is needed.

**Acceptance.**
- (i) Root: the manifest follows C5 with a new rule, and every part's year matches its name.
- (ii) Owner prints, per acceptance year 2013-2023: landed originals over the originals listed in the SEC quarterly
  master indexes for that year (your cohort-closure proof).
- (iii) Owner prints, per acceptance year: the share of landed originals that have a landed same-period prior-year
  original and a finite `doc_sim_cosine`. Gate: at least 90%. The denominator **excludes** an issuer's first filing of
  a period (it has no comparator) and the forms B1 does not land (20-F, 40-F).
- (iv) Owner: every row has `prior_available_at < available_at`, and both filings are originals.

### B3. F10, SG&A / R&D history in the fundamentals export (low priority)

**Why low.** The F10 field (`bm_int`) is excluded by our bar. Its difference portfolio has a Sharpe of .32 over
1975-2018 in the source paper, before costs, against a bar of .4 (`builder-designs.md` section 0). What remains is a
platform item: a pinned, year-partitioned `compustat_std`, which every engine fundamentals field would use.

**Today** (wh `docs/datasets/compustat_std.md`):
- `funda` carries:
  - `xsga` (`compustat_std.py:178`), the XBRL `SellingGeneralAndAdministrativeExpense` (`fund_items.py:93`);
  - `xrd` (`:207`), a separate XBRL element (`fund_items.py:104`);
  - `gdwl` (`:199`).
- Rows are vintages with `available_at`, `first_reported` and `vintage_kind` (doc `:12-13`, `:26-40`), and there are
  as-of macros (`:45-50`).
- `available_at` runs from 2010-01-05 (`:17`).
- There is one file for all years.

**Ask.**
- Columns (funda; keys unchanged):
  - `cik, datadate, fyear, fyr, accession, available_at, vintage_kind, first_reported`;
  - `xsga, xrd, gdwl`;
  - `seq, ceq, pstk, txditc, at, lt`;
  - `fin_template, curcd, fx_converted`.
- Parts by the year of `available_at` (C3), pinned (C5).
- History: every fiscal year ending 2009-01-01 or later whose 10-K was accepted before 2024-01-01. Fiscal years
  2001-2008, as reported, are optional.
- If CPI-U NSA (BLS CUUR0000SA0) is not already in house, add it monthly, stamped at its release instant.

**Availability.** The row's own `available_at`. Restatements are new vintages.

**Acceptance.**
- (i) The manifest follows C5, with parts.
- (ii) Owner prints `xsga` coverage of first-reported rows, fiscal years 2009-2022, on non-financial templates.
- (iii) Owner **reports** the SG&A convention. It is not a gate.
  - Report the share of rows with both `xsga` and `xrd` present.
  - For a sample of 50 R&D-reporting issuers, report whether the filed SG&A element includes R&D.
  - The house `xsga` is the XBRL SG&A element, and `xrd` is a separate element (`fund_items.py:93`, `:104`). EKP's
    "xrd is included in xsga" describes Compustat XSGA, not the house item.
- (iv) No part holds a row stamped outside its year.

---

## What P9 never needs from you

- No row stamped 2024-01-01 or later.
- No edit to any stage other than those named here.
- No raw data shared with the sprint. Root reads manifests and receipts only.
