# Lane AL-DATA report (doc-only, Ruling W3-18; fix round 1 applied)

## Outcome

DONE_WITH_CONCERNS after fix round 1.

- **What was produced.** The atx-db request and the gated-family designs. Nothing is built or registered, and no data
  has landed (ledger :16; W3-15).
- **Under ALD-4, no family clears the bar.** `lazy_prices` now fails as well.
- **The ALD-3 trace does not close.**
  - Neither the accepted book (the 67 candidates of `lib-v8ysb-gm`) nor the role `train-2020-2023-lo3` reads the
    indexes stage or the directory snapshot directly.
  - Both read the atx-db identity bridge `identity-bridge-v2-pit`. Its strict tier is built by a step that reads the
    2026 directory, so whether they depend on it **cannot be told from source**.
  - Ruling ALD-3 says the PM stops if a dependence is found; this answer is "cannot tell", with a stated way to settle
    it (Consumers section).

## Branch / SHA / pool

- No pool, no branch, no commit (W3-18). The files are untracked.
- Read at pool-2 HEAD `f6444417`.
- atx-db source was read only through `git -C C:/atx-wt/pool-2 show` at `dec03454` (main), `0e2fa077`, `c0eb2f74` and
  `75efb231` (the warehouse branch over time; the cited files are identical across them).

## Files

1. `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/atx-db-request.md`
   - Part A: four look-ahead channels, rules R1-R8, the fixture test, receipt-based acceptance.
   - B1 / B3: low priority, corrected.
   - B2: dropped, with one deferral line (ALD-5).
2. `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/builder-designs.md`
   - Section 0: the bar under ALD-4, with every band derived.
   - Sections 1-4: the designs.
   - Section 5: spec amendments for root, including the second `russell_recon` aspect.
3. `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/task-AL-DATA-report.md` (this file).

## Required items (wave3-carry "AL-DATA", as amended by ALD-1..5)

| item | result |
|---|---|
| OD-P9-4..6 not executable: build nothing; a design per family | Done (designs sections 1-4). No code. |
| ALSIG-7 request | Done, now with four channels: presence, `effective_to`, `recon_summary`, and directory eligibility (ALD-3). |
| ALSIG-4: `hf_crowd` never queued | Held. The source is corrected to the house `thirteenf_filer_type`. |
| Amend `blocked/{lazy_prices,hf_crowd}.json`; no duplicate id | Text in designs section 5, applied by root (ALD-2). `lazy_prices` now carries the ALD-4 never-queued note. |
| `russell_recon` second `non_pit_aspects` entry (ALD-3) | Text and procedure in designs section 5.3, applied by root. It needs a build, because it edits the C++ spec literal and the gtest at `:140`. |
| K-P9-11 keys; `source_sample_end` from the paper; ids at most 24 characters | `lazy_prices` 2014; `hf_crowd` 2016; `bm_int` 2018 (verified); `pcp_spread` left out (unverified). All ids are 6-18 characters. |
| ALD-3 consumer trace | Done (below). |
| ALD-4 bar arithmetic | Done (designs section 0). |
| ALD-5: B2 dropped; B1 / B3 low priority with the numbers | Done. |

## Evidence

- **Read only.**
  - Ledger grep; review; plan; literature review.
  - Specs: `scripts/specs/v8/lib-v8ysb-gm.json`, `base-lo3.json`, and the library and recipe JSON they pin.
  - `atx-engine/tools/field_registry.json` and engine / tools source.
  - atx-db source through `git show` / `git grep`.
  - The EKP NBER PDF (saved by WebFetch in round 0).
- **Not done:**
  - no build, no pytest, no git write;
  - no atx-db data, no `build-equity/` file (including manifests), nothing dated 2024 or later;
  - no 2020-2023 return / IC / Sharpe / NAV output.
- **Book statistics quoted.** Two public figures from the literature review: SR_b 1.77 (lit §3.0) and X-4 -.058 (lit
  §0.1). The rest are published literature figures (CMN, EKP, CW, Johnson-So).

## Consumers (ALD-3 trace; pool-2 tree at `f6444417`; specs, registry and source only)

Method:
- grep over `atx-engine`, `atx-impl`, `scripts`, `atx-core` and `atx-tsdb` for:
  - `indexes/`, `constituents`, `membership.parquet`, `recon_summary`;
  - `R1000P`-`SP500P`, `r1000`-`sp500`, `univ_r3000`;
  - `directory_*`, `security_type(_basis)`, `is_etf`, `is_operating`, `member_equity`, `is_common`, `is_adr`, `is_lp`,
    `delisting_date`, `line_types`;
- then every hit was read, and every registry row of the parent's field list was checked for its `sources`.

### The indexes stage

| consumer | file:line | reads | can a value differ from what was knowable at its stamp? |
|---|---|---|---|
| engine kind `russell_recon` (reader + kernel) | `atx-engine/include/atx/engine/research/fields/sources/russell_proxy.hpp:32-36`; `src/research/fields/sources/russell_proxy.cpp:82-89`; `russell_recon_field.cpp:102-120` | constituents `index_id, security_id, rank_date, recon_date, available_at` | **yes**, through channel 1 (presence) and channel 4 (eligibility). It reads neither `effective_to` nor `recon_summary` |
| registry row `russell_recon` | `atx-engine/tools/field_registry.json` (`sources ["indexes"]`, `point_in_time: false`, one aspect) | - | declared non-PIT; the second aspect is pending (designs 5.3) |
| string `rank(russell_recon)` | `scripts/specs/p9/candidates/russell_recon.json` (`dropped`, final) | - | no trial |
| IC admission guard | `atx-impl/src/strategy_ic_admission.cpp:583-586` | the manifest's `non_pit_aspects` | refuses the field |
| fixtures / tests / class list | `atx-engine/tests/fixtures/research_fields_al_sig/indexes/`; `research_fields_al_sig_{russell,fixture}_test.cpp`; `atx-engine/schemas/research_store/classes.json:54` | synthetic / schema id | n/a |

No role builder, universe builder, Python field builder or other registry row reads the indexes stage.

### The directory snapshot (directly, or through the panel flags derived from it)

| consumer | file:line | reads | differs from knowable? |
|---|---|---|---|
| `russell_recon`, through index eligibility | atx-db `indexes.py:156-159` (main); `panel.py:226-236`, `:259-262` | panel `security_type`, `is_adr`, `is_lp`, `is_operating` | **yes** (channel 4) |
| field `is_common` (Python, `lake`) | `atx-engine/tools/prepare_research_fields.py:271-277`, `:1450-1469`; registry row `is_common` (`point_in_time: false`, aspect `values`) | research lake `line_types.security_type`, built by atx-db `research/spine.py:815-852` from the 2026-09-18 directory | **yes**; already declared static and non-PIT, and refused by the IC guard |
| gold field `gp_hl_spread_21` | `atx-engine/tools/research_fields_gold.py:120-126` ("member_equity rows only" at `:125`); off by default, registered only through `prepare_research_fields_xdata.py` (`research_fields_gold.py:1-6`) | atx-db gold stage; `member_equity` = member AND `is_operating` (`panel.py:259-263`), and `is_operating` is gated by `is_etf`, which includes `directory_etf` (`:236`) | **yes**, through presence |
| `regsho_threshold_days63`, `exch_up_365d` (the `security_master` readers) | `research_fields_holdings.py:69-70`, `:937`, `:1136` | `finra_names.parquet` only: `security_id, available_at, dissemination_date, market_class` | **no**: dated FINRA rows. The directory columns are in `lines.parquet`, which they do not read |
| identity bridge `identity-bridge-v2-pit` (role universe `linked-operating-v3`; every field with source `identity_bridge`) | `atx-engine/tools/prepare_recent_research.py:29-35`, `:130-135`; `prepare_research_fields.py:1554-1558`, `:1675-1681`; `scripts/specs/v8/base-lo3.json` `inputs.identity_bridge` | `links.parquet`: `sr_id, cik, start, end_incl, available_at, primary, class_status` | **cannot tell** (below) |

**The bridge in detail** (atx-db main `dec03454`):
- `identity-bridge-v2-pit` = the strict and name tiers. The 2026-snapshot backfill tier is excluded
  (`alpha_panel/identity_table.py:25-29`, `:172-176`).
- Its `class_status` comes from the vendor ticker suffix or the FINRA issue name, both dated (`:16-17`, `:107-116`).
- Panel `is_operating` is loaded into `core` (`:65`) but not used in the link days (`:68-85`).
- **But** the strict tier is the r4 links (`:3`), and the r4 rehearsal builder `identity_links.build_rehearsal` reads
  two 2026 snapshots: the Nasdaq directory and the SEC company-ticker list (`identity_links.py:840-852`).
  - The directory's classes feed the bridge's class guards and rule-5 history (`market_owner_bridge.py:1748`,
    `:1842-1845`).
  - Current-ticker links carry a modelled availability (`:1770-1810`).
- This bounded read cannot settle whether any strict-tier link the role or the fields read (start before 2024-01-01)
  exists, or is absent, because of the 2026 directory.

**What would tell.** Any one of these:
- (a) The atx-db owner confirms which build produced the r4 rows in `identity/links_combined.parquet`, and whether its
  class guards can create or remove a link.
- (b) A rebuild-diff in atx-db: rebuild the strict tier with an empty directory and diff the `links.parquet` rows with
  `start < 2024-01-01`. Zero differing rows means no dependence. The same diff with a dated ticker history in place of
  the 2026 company-ticker file settles the second snapshot.
- (c) A follow-up full read of `_with_class_guards`, `_rule5_classifier` and `_with_reconstructed_history` in
  `market_owner_bridge.py`.

### The answer

**Accepted book.** Parent `scripts/specs/v8/lib-v8ysb-gm.json`:
- 84-field `fields.list`;
- library `atx-impl/strategies/fund_industry_ic_v8ysb.json`, with **67 candidates** over 58 fields (all in the list,
  plus the role payload's `close`, `raw_close` and `volume`);
- recipe `fund_industry_ic_v8ysb.recipe.v2.json`.

| source | dependence | evidence |
|---|---|---|
| indexes stage | **no** | No listed field has `indexes` among its registry `sources`; only `russell_recon` does, and it is not listed. The library and recipe text contain none of the index or directory terms. |
| directory snapshot, directly | **no** | `is_common` and the gold fields are not listed. The two listed `security_master` fields read `finra_names` only. |
| directory snapshot, through `identity-bridge-v2-pit` | **cannot tell from source** | 35 of the 84 fields name source `identity_bridge` and 17 name `sec_identity_bridge` (50 in all). See "the bridge in detail". |

**Role `train-2020-2023-lo3`** (universe `linked-operating-v3`):

| source | dependence | evidence |
|---|---|---|
| indexes stage | **no** | `prepare_recent_research.py` reads the vendor projection, the identity bridge, `sic_events` and the `delisting` stage, and no indexes stage. |
| directory snapshot, directly | **no** | No panel type flag is read. The `delisting` stage reads no directory or type flag: a `git grep` of `delisting.py` returns none. |
| directory snapshot, through its pinned bridge `identity-bridge-v2-pit` | **cannot tell from source** | Same as the book. |

## Bar verdicts (designs section 0)

| family | net SR_c | verdict |
|---|---|---|
| F1 `lazy_prices` | .09-.37 after the post-publication base rate (.29-.87 before) | **fails**; never queued without a new PM ruling |
| F2 `hf_crowd` | not derivable | fails on rho .2 |
| F4 / F6 `pcp_spread` | - | fails on rho .3 |
| F5 `os_ratio` | - | fails on rho .2 |
| F10 `bm_int` | difference-portfolio SR .32 (full sample, pre-cost) | fails |
| `borrow_fee_implied` | - | outside the bar (realism) |

## Recommended P10 queue

1. **Part A of the request:** the four indexes channels, including R7 for the directory. If R7 (a) is chosen, the same
   restriction is worth applying to every panel consumer of `security_type` / `is_etf`.
2. **Settle the bridge question** with one of (a) / (b) / (c) above, before the OD-3 read.
3. **Realism data** (the owner's money decision, ALD-2), then `borrow_fee_implied` as a flagged cost-model protocol,
   if the PM queues it.
4. **B3 platform part**, then B1, both low priority.
5. **Nothing from AL-DATA's signal families** by default (ALD-4).

## Open risks

- The bridge dependence is unresolved, and it touches the role and 50 of the 84 book fields.
- F1's sigma (8-12%), turnover and cost are [est]. The band is shown with its derivation; a lower sigma would raise
  the band but not past .4 unless gross reaches about .95.
- The column names of `thirteenf_filer_type` (`period`, `filer_type_pit`) still need confirming against the stage.
- The `bm_int` "+ xrd" rule depends on the B3 convention report.

## Ledger candidates

- atx-db `indexes`: four non-PIT channels.
  - `intervals()` drop (ALSIG-7);
  - `effective_to` = `last_session` on rank-date rows, which also depends on the data's extent;
  - `recon_summary` counts, with lake `"clock": None`;
  - `snapshot()` eligibility through panel `security_type` / `is_etf` from the 2026-09-18 directory (`panel.py:226-236`).
- `identity-bridge-v2-pit`'s strict (r4) tier is built by `identity_links.build_rehearsal`, which reads the 2026
  Nasdaq directory and the SEC company-ticker snapshot (`identity_links.py:840-852`). Whether its links depend on them
  is open.
- EKP 2022: Jan 1975-Dec 2018; delta .2, g .1; HML_INT minus HML_FF is 2.11% a year at 6.53% sd, SR .32 (.62 since
  2007). House `xsga` is XBRL SG&A with `xrd` a separate element (`fund_items.py:93`, `:104`), unlike Compustat XSGA.

## Questions for the PM

- **Q1.** Does the "cannot tell" answer on the identity bridge trigger the ALD-3 stop? Recommend: treat it as a
  finding for the owner. Ask for check (b), the rebuild-diff, before the OD-3 read. The P9 cells may proceed, because
  their parents read the same bridge and the comparison is like with like; the OD-3 read waits for (b).
- **Q2.** Panel `delisting_date` is a static future fact on every panel row, and no traced consumer reads it. Add it to
  the request as a note only? Recommend: yes, as written. No engine action.
- **Q3.** Which R7 option should the request prefer? Recommend (a): restrict to dated FINRA evidence. It is cheap, and
  it makes channel 4 provably closed through the receipt key `directory_decided_on_rank` = 0.

## Fix round 1 (review `al-data/task-AL-DATA-review.md`: 6 major, 12 minor, 3 nit; rulings ALD-3, ALD-4, ALD-5)

| finding | status | where / what |
|---|---|---|
| **M1** "no 2024+ fact under R1-R3" was false (directory eligibility) | Fixed | Request A.2 channel 4, R7, and the TRAIN claim scoped to `intervals()`; A.6 conditional; designs 5.3 second aspect; Consumers trace (ALD-3) |
| **M2** the `counts_on_recon` check could not be run and proved nothing | Fixed | A.5: new receipt keys `rank_vintage_rows_on_recon` (compared with `counts_on_recon`), `exit_rows`, `directory_decided_on_rank` |
| **M3** incomplete fixture | Fixed | A.4: business-day calendar 2020-05-01..2022-12-30; R1 members X / W / V; R2's `(additions, deletions)` asserted `(3, 0)` vs `(2, 1)`; "alive" = the last data session |
| **M4** B2 ignored `thirteenf_filer_type` | Fixed | B2 dropped (ALD-5), one line naming the stage; designs section 2 now reads `thirteenf_filer_type.filer_type_pit` and states the pre-2023 gap |
| **M5** house `xsga` does not include R&D | Fixed | B3 (iii) is now a convention report, not a gate; designs section 3: SGA = `xsga + xrd` when R&D is a separate element |
| **M6** F1's band had no derivation | Fixed | Designs section 0: every band derived; under ALD-4, F1 is .09-.37 and fails |
| m1 lead wording | Fixed | A.2 channel 1: one session to about 7 / 8 weeks (Russell), about 3 weeks (SP500P) |
| m2 citations | Fixed | Main test `:62-77` / `:72`; `text_native` at `c0eb2f74`; SHAs pinned |
| m3 channel-3 wording; lake clock | Fixed | A.2 channel 3; R4 asks for the `recon_summary` clock |
| m4 R2 depends on the data extent | Fixed | R2 takes the schedule constant whatever the data; A.4 assertion 5 truncates the data extent |
| m5 R3 keyed on `last_session` | Fixed | R3 is triggered by `delisting_date` or `last_session < cal[-1]` |
| m6 R4 / R5 | Fixed | R4 uses separate vintage rows; R5 builds only from vintages stamped at or before its own stamp |
| m7 B1 population | Fixed | B1 extends `text_native`: the complete cohort of original 10-K / 10-Q filings 2012-2023; layout is the owner's choice, partitioned by year |
| m8 coverage denominator | Fixed | B1 (iii) excludes first filings and 20-F / 40-F; (ii) measures landing completeness |
| m9 `lazy_prices` probe gaps | Fixed | Designs section 1, cases (f) inverted prior and (g) row-level seal |
| m10 NaN fee | Fixed | Designs 4a: the builder writes `default_fee_bps`; cites `borrow_schedule.hpp:77-79`, `:85-86`, `:102` |
| m11 `bm_int` wording and R&D | Fixed | Designs sections 0 and 3: SR half only; .62 set aside by the full-sample rule; R&D added |
| m12 book statistics sentence | Fixed | Evidence names SR_b 1.77 and X-4 -.058 as lit-review figures |
| n1 13F seal analogy | Fixed | C3 says "analogous" and gives the quarter rule |
| n2 JF issue date | Fixed | Designs section 0, F1: June 2020; the working paper circulated before TRAIN (review n2) |
| n3 theme rho vs book rho | Fixed | Designs section 0 bar note |
| ALD-3 trace | Done, with one open answer | The Consumers section. The bridge's strict tier is "cannot tell", with three ways to settle it |
| ALD-4 | Done | F1 fails; `lazy_prices` gets the never-queued note (designs 5.1) |
| ALD-5 | Done | B2 is one line; B1 / B3 marked low priority with the numbers |
| ALD-6 | Done | Request item **A0** added as the first item of part A: the identity-bridge strict (r4) tier. It covers the source evidence, why it matters, the rebuild-diff check (both 2026 snapshots empty; rows with `start < 2024-01-01`; counts by year and column), the identifiers and point-in-time rule if non-zero, and the acceptance test. Nothing else renumbered. |

Not fixed: nothing in the review is left undone.
- The bridge question is a new open item from the trace, not a review finding.
- Claims that could not be supported were removed rather than softened:
  - "usually excludes R&D" became the cited element split;
  - the circular back-solve is no longer used as an argument;
  - the CW sample end stays unverified.

## Fix round 2 (stopped by the owner)

The owner stopped the session partway through fix round 2. **No text in `atx-db-request.md` or `builder-designs.md` was
rewritten.** The only change to the request is a DRAFT line at its top. The Outcome, Consumers and Q1 text above is
still the round-1 text: ALD-7 and the re-review supersede its "cannot be told from source".

**Status per re-review finding** (review `## Re-review`):

| finding | status |
|---|---|
| RM1 (A0 rewrite: the two paths, drop the over-strong bullets; report "cannot tell" and Q1) | **partly**: source for path (a) verified (below); no text written |
| RM2 (a runnable rebuild-vs-rebuild diff; `primary` test; extended rule) | **not started**: no diff design written |
| RM3 (channel 4 misses the panel's backfill-tier `cik`; R7 (d); receipt key; aspect clause) | **not started** |
| Rm1 (assertion 4 "fails today" reason) | not started |
| Rm2 (Consumers: panel `cik`, `delisting` stage) | not started |
| Rm3 (delete SR_b 1.77 and X-4 -.058 at designs `:26`, report `:63`, `:215`) | not started |
| Rn1 (one publication-date rule, F1 vs F2) | not started |
| Rn2 (aspect text vs request: NULL) | not started |

**A0: what was verified in source by this round** (atx-db at main `dec03454`, read with `git show`; the bridge files
`identity_links.py`, `alpha_panel/identity_table.py`, `alpha_panel/identity_backfill.py`, `identity_reconstruction.py`
and `market_owner_bridge.py` have an empty `git diff` from `dec03454` and from `0e2fa077` to `f3b0aa75`):

- **Path (a), the primary flag computed with the 2026 backfill lines: verified within `identity_table` and
  `identity_backfill`.**
  - `identity_table.py:72-85`: the per-session `days` table joins every row of `identity/links_combined.parquet`,
    with no tier filter (`:76` keeps `backfill` and `name` as tiers). `links_combined` holds strict, backfill and name
    rows (`:3-4`; `identity_backfill.py:132-152`, `:169`).
  - `identity_table.py:111-112`: `is_issuer_primary` = first row per `(cik, session_date)` by `adv63 DESC NULLS LAST,
    security_id`, over all tiers. `links_combined`'s own `primary` column is not read (`:75-78` select `cik, tier,
    basis, available_at` only).
  - `:119-133`: the intervals are split where the flag changes; `:131`: an interval's `available_at` is the minimum
    over it.
  - `:172-176`, `:187-195`: the `pit` export keeps the strict and name tiers only **after** the flag is set; `:190`
    writes it as `P` / `J`.
  - So on any `(cik, session)` whose highest-`adv63` linked line is a backfill line, the `pit` export carries every
    line of that CIK as `J` that session. Splitting can also change `start`, `end_incl`, `available_at` and row
    presence of the strict / name rows (derived from `:119-133`, `:131`; not yet written into A0).
  - Backfill lines: `identity_backfill.py:52-58` (r4 export rows with `link_basis = 'current_ticker_verified'`,
    `link_primary` P / J, `valid_until` NULL: open at the 2026 snapshot); `:71-89` (the line's final run, no gap over 21
    sessions, ending on or after 2026-09-01); `:96-109` (days after the CIK's first periodic filing, after any strict
    link to another CIK, and not already strict-linked). `available_at` is the snapshot time (`:20`;
    `identity_table.py:20-21`).
- **Still unverified by this round:**
  - that the role universe `linked-operating-v3` keeps only `P` lines (`prepare_recent_research.py:13`, `:586`;
    `prepare_research_fields.py:1685`, `:1735-1736`, as the re-review cites): not read;
  - how `build_rehearsal` creates the `current_ticker_verified` rows and whether the 2026 directory decides them
    (`identity_links.py:930-967`, `_company_versions` `:274-338`, `_class_proof_at` `:517-549`): not read;
  - **path (b)**, the 2026 SEC ticker list changing r4 link acceptance (`identity_links.py:1287-1289`;
    `identity_reconstruction.py:144-147`, `:1411-1426`, `:1535-1551`, `:1564-1581`, `:1601-1608`): **not verified**;
  - what the Nasdaq directory snapshot does in `build_rehearsal` (`identity_links.py:840-861`, `:925-967`): not read;
  - the empty-directory guard and pinned-input checks (`:740-741`, `:842-848`, `:1248-1250`): not read;
  - the panel `cik` path of RM3 (`panel.py:468-477`; `indexes.py:151`, `:166-174`): not read.
- **Runnable rebuild-vs-rebuild diff: not written.**

**Disagreement with the re-review (one, minor).** The re-review says backfill lines are backdated "over the line's
final run from 2017-10" (`identity_backfill.py:10-14`, `:35`). In source, `COMBINED_FROM = 2017-10-01` (`:35`) bounds
only the strict-day expansion (`:116`); the backfill days (`:96-104`) are bounded by the final run and the first
periodic filing, and `identity_table` reads link days from 2017-01-01 (`identity_table.py:72`). Where an A0 rewrite
dates the backfill span, it should cite those lines, not `:35`. This does not change path (a).
