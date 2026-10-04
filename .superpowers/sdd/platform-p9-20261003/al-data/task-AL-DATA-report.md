# Lane AL-DATA report (doc-only, Ruling W3-18; fix rounds 1, 2 and 3 applied)

## Outcome

DONE_WITH_CONCERNS after fix round 3 (sections "Fix round 2" and "Fix round 3" at the end).

- **What was produced.** The atx-db request and the gated-family designs. Nothing is built or registered, and no data
  has landed (ledger :16; W3-15).
- **Under ALD-4, no family clears the bar.** `lazy_prices` now fails as well.
- **The ALD-3 trace finds a dependence through the identity bridge.**
  - Neither the accepted book (the 67 candidates of `lib-v8ysb-gm`) nor the role `train-2020-2023-lo3` reads the
    indexes stage or the directory snapshot directly.
  - Both read the atx-db identity bridge `identity-bridge-v2-pit`. **A dependence path on 2026-dated inputs exists in
    source; its size needs the owner's diff** (request A0.3).
  - Three paths are in source: (a) the primary flag is computed with the backfill lines; (b) the 2026 ticker list
    enters r4 link acceptance; (c) the name tier is withheld on backfill days. Path (c) was found in round 2 and
    confirmed by re-review 2; it stays in A0 (ALD-9). Within (b), (b1) and (b3) are look-aheads, and (b2) is a
    dependence without a look-ahead.
  - PM rulings ALD-6, ALD-7 and OD5-1 handle it: the exposure is accepted for P9 with a flag, and the A0 diff request
    goes to the owner with the freeze report.

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
   - Section 5: spec amendments. 5.1 and 5.2 root applies at the wave-2 gate (content-only); 5.3, the second
     `russell_recon` aspect, is a P10 note (ALD-11).
3. `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/task-AL-DATA-report.md` (this file).

## Required items (wave3-carry "AL-DATA", as amended by ALD-1..5)

| item | result |
|---|---|
| OD-P9-4..6 not executable: build nothing; a design per family | Done (designs sections 1-4). No code. |
| ALSIG-7 request | Done, now with four channels: presence, `effective_to`, `recon_summary`, and directory eligibility (ALD-3). |
| ALSIG-4: `hf_crowd` never queued | Held. The source is corrected to the house `thirteenf_filer_type`. |
| Amend `blocked/{lazy_prices,hf_crowd}.json`; no duplicate id | Text in designs sections 5.1 and 5.2; root applies at the wave-2 gate (content-only; ALD-2, ALD-12 (c)). `lazy_prices` now carries the ALD-4 never-queued note. |
| `russell_recon` second `non_pit_aspects` entry (ALD-3) | Text and procedure in designs section 5.3. **NOT applied in P9** (ALD-11, ALD-12 (c)): it moves the pinned `formula_sha256` `787a8d87…a699`; P10 note. It would need a build, because it edits the C++ spec literal and the gtest at `:140`; the pytest at `test_p9_al_sig_registrations.py:109` changes too (round 2). |
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
- **Book statistics quoted.** None. The figures quoted are published literature figures (CMN, EKP, CW,
  Johnson-So, McLean-Pontiff).

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
| registry row `russell_recon` | `atx-engine/tools/field_registry.json` (`sources ["indexes"]`, `point_in_time: false`, one aspect) | - | declared non-PIT; the second aspect is a P10 note (designs 5.3, ALD-11): through P9 the row declares the presence aspect only |
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
| identity bridge `identity-bridge-v2-pit` (role universe `linked-operating-v3`; every field with source `identity_bridge` or `sec_identity_bridge`) | `atx-engine/tools/prepare_recent_research.py:29-35`, `:130-140`, `:586`, `:696-699`; `prepare_research_fields.py:328-332`, `:1557-1559`, `:1673-1740`; `scripts/specs/v8/base-lo3.json:43-53` | `links.parquet`: `sr_id, cik, start, end_incl, available_at, primary, basis`, plus `class_status` for the role | **yes, by source** (paths (a)-(c), below); size unmeasured |
| panel `cik` and, through it, `is_operating` and `member_equity` | atx-db `panel.py:468-477`, `:487-489`, `:262-263`; read in our tree only by the gold field `gp_hl_spread_21` (`research_fields_gold.py:120-126`, off by default) and through index eligibility (`russell_recon`) | `identity/link_table.parquet`, every tier; a backfill link used from `evidence_at` = `valid_from` 22:00 UTC (`identity_table.py:22-24`, `:144-145`) | **yes**: the 2026 choice of CIK, backdated (atx-db's own label, `export_impl.py:345-346`). This is channel 4's second input (request A.2) |
| atx-db `delisting` stage (role attributes) | `prepare_recent_research.py:33-41`, `:759`, `:851-864`; `base-lo3.json:4` ("delisting attributes only, --delisting-returns off") | `continued`, `cause`, `dlret`, `available_at` | **yes, but inert for lo3.** It joins `link_table` with no tier filter (`delisting.py:9`, `:55`, `:72-80`). It decides `continued` on up to 30 days after L (`:7-8`, `:74-78`, `:108`) while stamping at L+1 (`:23`, `:132-134`). With returns off, membership and payloads are byte-identical (`prepare_recent_research.py:37`). If `--delisting-returns` is ever turned on, the 30-day continuation is a look-ahead |

**The bridge in detail** (atx-db main `dec03454`, identical at the warehouse tip `8b55bbbe`; full text in request
A0.1):
- `identity-bridge-v2-pit` = the strict and name tiers. The backfill tier is dropped, **but only after** the primary
  flag is computed over all tiers (`alpha_panel/identity_table.py:72-85`, `:111-112`, `:172-176`, `:187-195`).
- Its `class_status` comes from the vendor ticker suffix or the FINRA issue name, both dated (`:16-17`, `:107-116`).
- **Path (a).** On a (cik, session) whose most liquid linked line by prior adv63 is a backfill line, every strict and
  name line of the CIK is `J`. The role drops `J` cells as "secondary".
- **Path (b).** The `reconstruct` step uses the 2026 SEC ticker list (`identity_links.py:1287-1289`). That evidence
  item counts in dominance and the segment bounds, which ignore the clock (`identity_reconstruction.py:464-468`,
  `:1573-1608`). `_pit_clock` guards the threshold only (`:1411-1426`).
  - What reaches the strict tier as a look-ahead: (b1) a rival's 2026 ticker removes a link, and (b3) the segment ends
    of the line's other CIKs move (`:1706-1724`).
  - (b2), a candidate's own ticker winning dominance, also reaches the strict tier, but without a look-ahead. Stage I
    keeps it `high` / `medium` from the candidate's own clock until the rival's evidence is known, then it turns `low`
    (`identity_reconstruction.py:1483-1497`; versions `reconstructed_<tier>`, `identity_links.py:922`; role N,
    `:330-333`) and Stage I drops it (`alpha_panel/identity_links.py:147-151`). Both windows equal the clock-honest
    outcome, so arms B2 and B12 count its cells "in A only" although PIT-b keeps them.
- **Path (c), found in round 2, confirmed by re-review 2.** The name tier is withheld on backfill days
  (`identity_names.py:14-15`, `:148-152`; `identity_backfill.py:118-119`, `:129-130`). The pit export then has no link
  on those line-days.
- **The directory**, corrected from round 1: it reaches rows before 2024 only through the backfill candidates (the
  class of the snapshot links: `identity_links.py:925-967`; `identity_backfill.py:52-58`). The round-1 bullets on
  "class guards and rule-5 history" and "a modelled availability" were too strong. In `build_rehearsal`, that output
  feeds only snapshot-observation links stamped at 2026.

**What settles the size.** The rebuild-against-rebuild diff of request A0.3, with arms A, B1, B2 and B12. Only the
atx-db owner can run it.

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
| 2026-dated inputs, through `identity-bridge-v2-pit` | **yes, by source; size unmeasured** | 35 of the 84 fields name source `identity_bridge` and 17 name `sec_identity_bridge` (2 name both; 50 in all). They publish on `P` lines only (`prepare_research_fields.py:328-332`). See "the bridge in detail". |

**Role `train-2020-2023-lo3`** (universe `linked-operating-v3`):

| source | dependence | evidence |
|---|---|---|
| indexes stage | **no** | `prepare_recent_research.py` reads the vendor projection, the identity bridge, `sic_events` and the `delisting` stage, and no indexes stage. |
| directory snapshot, directly | **no** | No panel type flag is read. The `delisting` stage reads no directory or type flag: a `git grep` of `delisting.py` returns none. |
| 2026-dated inputs, through its pinned bridge `identity-bridge-v2-pit` | **yes, by source; size unmeasured** | The universe keeps only cells with a `P` row for their single CIK (`prepare_recent_research.py:586`; `prepare_research_fields.py:1684-1685`, `:1735-1736`). Paths (a) and (c) remove cells. Path (b) removes strict links and moves their ends. |
| delisting stage | **inert for lo3** | Attributes only, with returns off (`base-lo3.json:4`; `prepare_recent_research.py:37`). |

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
2. **Size the bridge dependence** with the owner's A0.3 diff. Under OD5-1 it annotates the freeze verdict; it does not
   gate P9.
3. **Realism data** (the owner's money decision, ALD-2), then `borrow_fee_implied` as a flagged cost-model protocol,
   if the PM queues it.
4. **B3 platform part**, then B1, both low priority.
5. **Nothing from AL-DATA's signal families** by default (ALD-4).
6. **Designs 5.3**, the second `russell_recon` aspect: a P10 note under ALD-11 (it moves the pinned `formula_sha256`).

## Open risks

- The bridge dependence exists in source, and its size is unmeasured. It touches the role and 50 of the 84 book
  fields. P9 runs with the OD5-1 flag.
- F1's sigma (8-12%), turnover and cost are [est]. The band is shown with its derivation; a lower sigma would raise
  the band but not past .4 unless gross reaches about .95.
- The column names of `thirteenf_filer_type` (`period`, `filer_type_pit`) still need confirming against the stage.
- The `bm_int` "+ xrd" rule depends on the B3 convention report.

## Ledger candidates

- atx-db `indexes`: four non-PIT channels.
  - `intervals()` drop (ALSIG-7);
  - `effective_to` = `last_session` on rank-date rows, which also depends on the data's extent;
  - `recon_summary` counts, with lake `"clock": None`;
  - `snapshot()` eligibility through panel `security_type` / `is_etf` from the 2026-09-18 directory (`panel.py:226-236`),
    and company grouping / `is_operating` through the panel `cik`, backfill tier included (`panel.py:468-477`, `:262`;
    `indexes.py:151`, `:166-174`).
- `identity-bridge-v2-pit` depends on 2026-dated inputs by source (atx-db `dec03454` = `8b55bbbe`):
  - (a) `is_issuer_primary` is computed over all tiers, backfill included, before the pit export drops backfill
    (`identity_table.py:111-112`, `:187-195`);
  - (b) the 2026 SEC ticker list adds a .5 evidence item that dominance and the segment bounds use without a clock
    (`identity_links.py:1287-1289`; `identity_reconstruction.py:1535-1551`, `:1577`, `:1601-1608`, `:1706-1724`).
    In the strict tier it removes links (a rival's ticker, b1) and moves segment ends (b3), both look-aheads. A
    dominance win by the line's own ticker (b2) is kept from its own clock until the rival is known, then dropped as
    tier `low` (`identity_reconstruction.py:1483-1497`; `identity_links.py:922`, `:330-333`;
    `alpha_panel/identity_links.py:147-151`): a dependence without a look-ahead;
  - (c) the name tier is withheld on backfill days (`identity_names.py:148-152`; `identity_backfill.py:129-130`).
  - The size is unmeasured.
- EKP 2022: Jan 1975-Dec 2018; delta .2, g .1; HML_INT minus HML_FF is 2.11% a year at 6.53% sd, SR .32 (.62 since
  2007). House `xsga` is XBRL SG&A with `xrd` a separate element (`fund_items.py:93`, `:104`), unlike Compustat XSGA.

## Questions for the PM

- **Q1.** *Superseded.* The round-1 question ("does 'cannot tell' trigger the ALD-3 stop?") was answered by ALD-6
  (owner-level; no P9 cell before the owner's answer), upgraded by ALD-7 (a dependence by code reading). OD5-1 then
  lifted the gate under the owner's delegation: P9 runs with the flag, and the A0 diff goes with the freeze report.
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
| m12 book statistics sentence | Fixed | Evidence sentence corrected; in round 2 the two lit-review book figures were deleted from both files (re-review Rm3) |
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

## Fix round 2 (stopped by the owner; superseded by "Fix round 2" below)

*Historical record.* The owner stopped the session partway through fix round 2. At that stop, no text in
`atx-db-request.md` or `builder-designs.md` had been rewritten, and the only change to the request was a DRAFT line.
The completed "Fix round 2" below rewrote both files, removed the DRAFT line and replaced the round-1 Outcome,
Consumers and Q1 text; this section's status table is superseded.

**Status per re-review finding** (review `## Re-review`):

| finding | status |
|---|---|
| RM1 (A0 rewrite: the two paths, drop the over-strong bullets; report "cannot tell" and Q1) | **partly**: source for path (a) verified (below); no text written |
| RM2 (a runnable rebuild-vs-rebuild diff; `primary` test; extended rule) | **not started**: no diff design written |
| RM3 (channel 4 misses the panel's backfill-tier `cik`; R7 (d); receipt key; aspect clause) | **not started** |
| Rm1 (assertion 4 "fails today" reason) | not started |
| Rm2 (Consumers: panel `cik`, `delisting` stage) | not started |
| Rm3 (delete the two public book figures from the designs and the report) | not started |
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

## Fix round 2

Completed 2026-10-04 by a fresh doc agent, under rulings ALD-8 and OD5-1 (iv). This section supersedes the status
table of "Fix round 2 (stopped by the owner)" above.

**Method.** Read-only, as in round 1.
- atx-db source was read only through `git -C C:/atx-wt/pool-2 show` at `dec03454` (main), `0e2fa077` (wh),
  `c0eb2f74` and `8b55bbbe` (the warehouse tip). The A0 and channel-4 files have an empty `git diff` from `dec03454` to
  `8b55bbbe`.
- Our tree was read at pool-2 HEAD `20bc5f5f`. The cited files have an empty diff from `f6444417`.
- One `python` call read two of our own spec files from git objects (`scripts/specs/p9/blocked/*.json`) to copy their
  `notes` strings exactly.
- No data was opened. No build, no pytest, no git write. Nothing was touched under `atx-db/`. No permission refusal
  occurred.

**DRAFT mark: removed.** Every re-review finding (RM1-RM3, Rm1-Rm3, Rn1-Rn2) is fixed below.
- Ruling ALD-7 still asks for a re-review of the rewritten A0 before hand-off.
- Path (c) and the A0.3 procedure are new and unreviewed.

### 1. Per finding

| finding | what changed | where |
|---|---|---|
| **RM1** A0 states one mechanism too strongly and misses the real paths | A0.1 rewritten: the build chain, the three 2026 inputs, path (a), path (b), each with citations. **Path (c) added** (found in this round). The two over-strong bullets are replaced by "What the directory does, and does not do", and "Not covered" lists what the check holds fixed. A0.2 now says the universe keeps only `P` cells, the fields publish on `P` lines only, and rows are read from `available_at`. The report says "a dependence path exists in source; its size needs the owner's diff". Q1 is marked superseded (ALD-6, ALD-7, OD5-1) | request A0.1, A0.2; report Outcome, Consumers, the two answer tables, P10 queue item 2, Open risks, Ledger candidates, Q1 |
| **RM2** the diff cannot run and is not sufficient | A0.3 rewritten as rebuild against rebuild:<br>- arms A, B1, B2, B12;<br>- check-only switches S1-S3, with no guard disabled and no empty directory (the guard `:846-848` is never reached);<br>- the whole chain as commands (prepare, six rehearsal phases, `alpha_panel.build --only identity,backfill,names,combine,identity_table`);<br>- sanity checks so a zero is not vacuous;<br>- D0 reproduction against the published files, as a precondition only;<br>- diffs of B against A only: the consumer view per `(sr_id, session_date)`, bridge-eligible cells on panel-member sessions, rows with `start` and `available_at` before 2024 (others reported separately), the panel CIK, and an attribution aid;<br>- an 11-row output table;<br>- how to read a zero and a non-zero result, and what source predicts (B1 non-zero is likely).<br>A0.4 adds rules PIT-a, PIT-b and PIT-c. A0.5 has T1 (the `primary` case the re-review asked for), T2 (path (c)), T3 (path (b1), the rival's ticker, which has teeth today), T4 (a with / without ticker differential over b1-b3), the re-run and the version | request A0.3-A0.5 |
| **RM3** channel 4 misses the panel `cik` | Channel 4 now has two inputs: the directory, and the CIK from the backfill tier (company grouping and `is_operating`). R7 is split into R7.1 (a)-(c), for the directory, and R7.2, for the CIK: (d) restrict to strict / name links, or (e) report. New receipt key `backfill_cik_on_rank`. A.1 gains rows for `p.cik`, `companies_of`, the panel link join, `evidence_at` and atx-db's own non-PIT label. A.6 waits on R7.1 and R7.2. The aspect text gains the CIK clause | request A.1, A.2 (4), A.3 R7 and TRAIN scope, A.4 last paragraph, A.5, A.6; designs 5.3 |
| **Rm1** wrong "fails today" reason for assertion 4 | Restated. X is missing from both runs; Z, Y and V appear only in the full run; R1's `effective_to` is 06-01 / 06-10 / 06-25 against 05-19; R2's summary is (3, 0) against (4, 0); a fixed build gives (2, 1) in both. I re-derived this from the fixture and today's code (`indexes.py:274-279`, `:284-292`); it agrees with the review | request A.4, assertion 4 |
| **Rm2** the trace omits the backfill consumers | Consumers table gains two rows: the panel `cik` (with `is_operating`, `member_equity` and the gold field), and the `delisting` stage. The delisting stage is inert for lo3; its 30-day continuation is a look-ahead if returns are ever turned on | report Consumers; role answer table |
| **Rm3** two public book figures remain | Deleted from designs section 0 and report Evidence. The fix-round-1 m12 row no longer repeats them | designs section 0; report Evidence, Fix round 1 table |
| **Rn1** two publication-date rules | One rule: first public circulation (the working paper where verified, else the journal issue). F1, F2 and F10 restated. No verdict changes. F2's working-paper date is not verified, so its journal issue counts | designs section 0 |
| **Rn2** "stale" vs "NULL" | Both now read "NULL (no FINRA row, or one older than the staleness limit), unknown or common_unverified" (`panel.py:569-570`) | request A.2 (4); designs 5.3 aspect |
| **ALD-8** the disputed span | Both passages quoted verbatim, with the span each limits (§3 below). Path (a) does not depend on it | request A0.1, path (a); §3 |
| **ALD-2 / ALD-3** spec amendments | Made exact (§5). Two round-1 errors are corrected: the history entry is dropped because `test_p9_al_sig_registrations.py:156` pins one entry; and `:109` also pins one aspect, so it is amended too. `lazy_prices.deviation` is replaced, because its old text contradicts ALD-1 | designs section 5; §5 below |
| other | Request header: citations re-pinned to `8b55bbbe`; the `text_native.md` line shift is noted; the wh lines for `p.cik` and `companies_of` were read at `0e2fa077`; Priority and "How it reaches you" follow OD5-1 | request header |

### 2. Verified in source by this round, and what still rests on another reading

**Verified by me** (atx-db at `dec03454`; our tree at `20bc5f5f`):
- **Path (a).**
  - `identity_table.py:72-85`, `:75-77`, `:111-112`, `:119-133`, `:131`, `:172-176`, `:187-195`;
  - `identity_backfill.py:52-58`, `:71-89`, `:96-109`, `:132-152`, `:169`.
- **Path (b)** (confirmed).
  - `identity_links.py:1287-1289` passes the 2026 list to `reconstruct`.
  - `identity_reconstruction.py:1535-1551` adds the item, weight .5 at `:357`.
  - `:464-468`: `Candidate.weight` sums every item with no clock.
  - `:1573` threshold, `:1577` dominance (ratio 3.0 at `:374`), `:1601-1608` segment bounds by rejected rivals.
  - `:1616` with `_pit_clock` at `:1418-1426`, which tests only `_passes` (`:1411-1415`).
  - The residual is documented at `:144-147`.
  - A citation detail: `_pit_clock` itself is `:1418-1426`. The review's `:1411-1426` also covers `_passes`.
  - **Refinement of the re-review's reading: what reaches the strict tier** (corrected in fix round 3 after re-review
    2, R2.3).
    - The re-review says "a line's own ticker can make it dominate a rival, which keeps a link stamped years
      earlier". That is true in the r4 export, and in the strict tier until the rival's evidence is known.
    - A candidate with competitors is tier `low` from the clock at which its rival's known share items reach the floor
      and overlap it (`_tier_history`, `:1483-1497`, called at `:1618`). The rehearsal writes one version per tier
      step, with basis `reconstructed_<tier>` (`identity_links.py:895-924`, `:922`); the `low` step has role N
      (`identity_links.py:330-333`). Stage I keeps tier `high` / `medium` or basis `strict_dated` /
      `current_ticker_verified` (`alpha_panel/identity_links.py:149`), so for a reconstructed version only the tier
      counts.
    - So (b2) reaches our bridge from the candidate's own clock until the rival is known, and is dropped from then on.
      In both windows the outcome equals the clock-honest one, so (b2) is a dependence without a look-ahead. Arms B2
      and B12 count its cells "in A only" (request A0.3).
    - The halves that are look-aheads:
      - (b1) a rival's 2026 ticker removes a link that would sit in the strict tier until the rival's evidence is
        known;
      - (b3) an acceptance that needs the 2026 item moves the segment end of the line's other, early-stamped CIK link
        (`_segments`, `:1706-1711`, `:1715-1719`, `:1722-1724`, read in full).
    - The request's path (b) text and its tests T3 / T4 follow this reading.
- **Path (c).** `identity_names.py:14-15`, `:77`, `:148-152`; `identity_backfill.py:118-119`, `:129-130`; stage order at
  `stagelake/registry.py:133-146` and `build.py:32-33`, `:42-43`.
- **The directory.** `identity_links.py:330-333`, `:517-523`, `:539`, `:673-684`, `:840-852`, `:859-861`, `:925-967`;
  `alpha_panel/identity_links.py:14-16`; `market_owner_bridge.py:41-46`, `:1748`, `:1770-1777`, `:1842-1845`.
  - This confirms the re-review's two "too strong" / "not true" rows: `retrospective` is used only at `:859` and
    `:926`.
- **Guards and switches.**
  - `identity_links.py:725-736`, `:740-741`, `:745`, `:756`, `:846-848`, `:1218-1223`, `:1243-1250`, `:1319-1330`;
  - `common.py:24`, `:28`, `:38-39`;
  - `build.py:76-77`, `:101-116`;
  - `panel.py:6-8`, `:160`: core and membership do not read identity.
- **Primary-only filter** (confirmed).
  - A member cell is kept only when its qualifying rows name one CIK and include a `P` row. A `J`-only cell fails as
    "secondary" (`prepare_recent_research.py:13-15`, `:571-591`, `:586`, `:696-699`, `:728`, `:737`, `:748`).
  - The `P` flag is `kind == "P"` (`prepare_research_fields.py:1684-1685`) and is propagated by `resolve_links`
    (`:1735-1736`).
  - Rows qualify from `available_at <= mark(t)` (`:1714`, `:1722-1723`).
  - The fields publish on `P` lines only (`:328-332`).
  - The spec pins universe `linked-operating-v3` on `identity-bridge-v2-pit` (`base-lo3.json:43-53`).
- **RM3.** `panel.py:262-263`, `:468-477`, `:487-489`, `:569-570`; `indexes.py:151`, `:156-159`, `:166-174` (main),
  `:164`, `:169-172`, `:179-187` (wh); `identity_table.py:22-24`, `:144-145`; `export_impl.py:345-346`.
- **Rm2.** `delisting.py:7-9`, `:23`, `:55`, `:72-80`, `:108`, `:132-134`; `prepare_recent_research.py:33-41`.
- **The amendment targets.** `russell_recon_field.cpp:58-69`;
  `research_fields_al_sig_fixture_test.cpp:112-143`; `test_p9_al_sig_registrations.py:43`, `:58-71`, `:104-111`,
  `:147-163`; `wave_manifest.py:152-164`, `:190-235`; `scripts/specs/p9/blocked/{lazy_prices,hf_crowd}.json`.

**Could not confirm** (no data and no manifest opened):
- that the published bridge (`09aac28f…`) and the role `train-2020-2023-lo3` were built by the code read here. Only
  the pins in `base-lo3.json` tie them; D0 in A0.3 checks the bridge;
- where panel core starts, which is the bridge's floor for backfill rows;
- what the OPS tables `securities`, `exchange_listings` and `security_identifier_history` do in `build_rehearsal`
  (`identity_links.py:824-828`). A0.1 lists them as "not covered";
- the size of any path. That needs data.

**Rests on the reviewer's reading only:** nothing in A0, channel 4 or the trace. The literature numbers are unchanged
from round 1 and were checked by the round-1 reviewer.

### 3. ALD-8: the two passages, verbatim

- `identity_backfill.py:35`:
  `COMBINED_FROM = "2017-10-01"  # combined/strict day expansion starts here (panel warm-up is 2018)`
- `identity_backfill.py:116`:
  `        WHERE c.session_date >= DATE '{COMBINED_FROM}'`
- **`:35` defines the constant, and `:116` is its only use** (`grep` of the file: `:35` and `:116`).
  - `:116` is the WHERE clause of the `strict_days` table (`:111-117`), the strict links of `identity/links.parquet`
    expanded to sessions.
  - So both lines limit the **strict-day** span only.
- **The backfill-day span** (`bf_days`, `:96-104`) is limited by:
  - the final run (`:71-89`);
  - the CIK's first periodic filing (`:101`);
  - the end of any strict link to another CIK (`:102-103`);
  - strict-wins (`:105-109`).
- In the bridge, every tier is also limited by `identity_table.py:72` (years 2017-2026) and by the panel-core sessions
  (`:63-67`, `:79`).
- So the re-review's "backdated over the line's final run from 2017-10" (`:10-14`, `:35`) dates the backfill span by a
  constant that does not limit it. The disagreement stands, and the re-reviewer settles it by reading.
- Path (a) does not depend on it: path (a) needs only that a backfill line and a strict / name line of one CIK share a
  session.

### 4. Open items (one line each)

- **ALD-7 re-review.** A0 (especially path (c) and the A0.3 procedure) needs the re-review before hand-off.
- **OD5-1 flag text.** It names "primary flag computed with backfill lines; r4 link acceptance". Path (c) (name links
  withheld on backfill days) is a third path. PM: add it to the flag line?
- **ALD-DIAG (OD5-1 (iii)).** If it counts delistings from the atx-db `delisting` stage, note that `continued` looks up
  to 30 days past L and keys successors on the any-tier CIK (`delisting.py:74-78`, `:108`).

### 5. The exact spec amendments (ALD-2, ALD-3, ALD-11, ALD-12 (c))

| amendment | status |
|---|---|
| 5.1 `lazy_prices.json` | **root applies at the wave-2 gate (content-only)** |
| 5.2 `hf_crowd.json` | **root applies at the wave-2 gate (content-only)** |
| 5.3 `russell_recon` second aspect | **NOT applied in P9: it moves the pinned `formula_sha256` (`787a8d87…a699`); P10 note** |

The "after" values are the same text as `builder-designs.md` section 5. Root copies them and makes no other change.
The "before" lines and line numbers were read at pool-2 HEAD `5061579d` (`git show`), unchanged since `20bc5f5f`; the
working-tree copies differ only in line endings. Each block below is whole lines, with the file's two-space indent and
trailing comma (every changed line is followed by another key). Replace each "before" line with the "after" line of
the same key.

**5.1 `scripts/specs/p9/blocked/lazy_prices.json`. Root applies at the wave-2 gate (content-only).**
- Replace five lines: 17 `formula`, 18 `domain`, 19 `deviation`, 20 `notes`, 28 `blocked_on`. Leave every other line
  unchanged, including `history` (lines 29-36): no entry is appended, because `test_p9_al_sig_registrations.py:156`
  pins exactly one.

Before (lines 17, 18, 19, 20, 28):

```json
  "formula": "lazy_prices (to be an engine-only C++ kind, DEC-5): the textual similarity of a firm's latest 10-K or 10-Q to its prior filing of the same type (higher = fewer changes: the non-changers CMN buy), held from the session after the filing is visible until the next filing of the type",
  "domain": "NaN without two filings of one type in the text landing; blocked until the landing completes",
  "deviation": "the field is not built: the atx-db text landing holds about 7% of 10-K / 10-Q (lit section 3.1 F1); no field name, kind or constant is chosen beyond the paper's similarity sign until it lands",
  "notes": "horizon class slow, half-life 126 sessions (PM8-5: CMN's 3-18 month drift, the filing_events theme's); data class P; blocked_on_data: the 10-K / 10-Q text landing and its engine reader (lit section 6 row lazy_prices, P9DATA -> P9SIG); rho to the themes .1 or less (lit 3.1 F1)",
  "blocked_on": "the atx-db 10-K / 10-Q text landing (about 7% landed at the literature review) and an engine reader of it",
```

After (lines 17, 18, 19, 20, 28):

```json
  "formula": "lazy_prices (engine-only C++ kind, DEC-5; design .superpowers/sdd/platform-p9-20261003/al-data/builder-designs.md section 1): term-frequency cosine of the full primary document of a firm's latest original 10-K or 10-Q to its original filing of the same form family and fiscal period one fiscal year earlier (higher = fewer changes: the non-changers CMN buy), visible from its EDGAR acceptance under the house stage rule, 400-day staleness; fixed before data (ALD-1)",
  "domain": "[0, 1]; NaN without a same-period prior-year original, past 400 days, or where the text landing does not cover the line",
  "deviation": "the field is not built: PM ruling ALD-4 (it fails the bar on paper); the definition was fixed before any data under PM ruling ALD-1 (full-document cosine of original filings, same-period prior-year comparator, 400-day staleness); the atx-db text landing held about 7% of 10-K / 10-Q at the literature review (lit section 3.1 F1)",
  "notes": "horizon class slow, half-life 126 sessions (PM8-5: CMN's 3-18 month drift, the filing_events theme's); data class P; blocked_on_data: the 10-K / 10-Q text landing and its engine reader (lit section 6 row lazy_prices, P9DATA -> P9SIG); rho to the themes .1 or less (lit 3.1 F1); PM ruling ALD-4 (AL-DATA, 2026-10-04): on post-publication expectations (literature return x (1 - .58)) the a-priori net SR_c is .09-.37 < .4, so it fails the bar; it stays a blocked_on_data registration and is never queued without a new PM ruling; design al-data/builder-designs.md section 1",
  "blocked_on": "atx-db ask al-data/atx-db-request.md B1 (low priority) and a new PM ruling on the bar (ALD-4)",
```

**5.2 `scripts/specs/p9/blocked/hf_crowd.json`. Root applies at the wave-2 gate (content-only).**
- Replace line 20 (`notes`) only. Every other line, including `formula` (17) and `history` (29-36), is unchanged.

Before (line 20):

```json
  "notes": "horizon class slow, half-life 63 sessions (PM8-5: quarterly holdings, the ownership_flow theme's); data class P; blocked_on_data: the Form ADV private-fund flag joined to the 13F filer CIK (lit section 6 row hf_crowd, P9DATA -> P9SIG); rho about .2 to momentum and inst_best_ideas (lit 3.1 F2), above the .1 bar, check against inst_best_ideas before any screen; PM ruling ALSIG-4: it stays a blocked_on_data registration and is never queued without a new PM ruling on the bar",
```

After (line 20):

```json
  "notes": "horizon class slow, half-life 63 sessions (PM8-5: quarterly holdings, the ownership_flow theme's); data class P; blocked_on_data: the Form ADV private-fund flag joined to the 13F filer CIK (lit section 6 row hf_crowd, P9DATA -> P9SIG); rho about .2 to momentum and inst_best_ideas (lit 3.1 F2), above the .1 bar, check against inst_best_ideas before any screen; PM ruling ALSIG-4: it stays a blocked_on_data registration and is never queued without a new PM ruling on the bar; AL-DATA (W3-18, ALD-5, 2026-10-04): design written, not built; the 2004-2016 source sample overlaps the OD-3 block; the house source would be the atx-db thirteenf_filer_type stage (filer_type_pit), which has no point-in-time fund-type input before 2023; the Form ADV join is deferred (al-data/atx-db-request.md, B2 dropped); design al-data/builder-designs.md section 2",
```

**Test lines for 5.1 and 5.2: none change.** The assertions of `scripts/tests/test_p9_al_sig_registrations.py` that
read the changed keys, and why each still holds:
- `:65-68` (regex `:43`) on `notes`: each new `notes` value starts with the old value, so the horizon prefix is kept;
- `:71` `dsl_sha256`: `dsl` is unchanged;
- `:153` (`candidate_problems`) and `:155` (`blocked_on` a non-empty string): every changed value stays a non-empty
  string (`scripts/wave_manifest.py:233-235`);
- `:156` (one `history` entry): `history` is unchanged;
- `:162-163` (`hf_crowd` `notes` contain "ALSIG-4" and "never queued without a new PM ruling"): kept in the prefix.
- No digest pins these files (`classes.json:15`: `pinned_by: git`).
- Check after applying: pytest `scripts/tests/test_p9_al_sig_registrations.py`.

**5.3 `russell_recon`: the second `non_pit_aspects` entry (ALD-3). NOT applied in P9: it moves the pinned
`formula_sha256`; P10 note (ALD-11).** The text below is kept for P10.

(i) `atx-engine/src/research/fields/russell_recon_field.cpp`.
- After line 59, insert:

```cpp
  // Ruling ALD-3 (AL-DATA): the stage's eligibility and company inputs use 2026 snapshots (values).
```

- On line 69, replace `      "present are point in time."};` with `      "present are point in time.",` followed by:

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

(ii) `atx-engine/tools/field_registry.json`, the `russell_recon` row.
- Re-derive it with the AL-SIG round-trip (`task-AL-SIG-report.md:320`):
  - `spec_text.non_pit_aspects` = the two strings of the C++ literal;
  - `formula_sha256` = the C++ `formula_sha256(spec)`;
  - `field_registry.validate`, then `field_registry.dump`, then `--check`.
- The row stays at revision 1. gtest `ResearchFieldsAlSigFixture.CommittedRowsPinTheSpecs` confirms it.

(iii) `atx-engine/tests/research/research_fields_al_sig_fixture_test.cpp`.
- Line 140 becomes `  ASSERT_EQ(russell.non_pit_aspects.size(), 2U);`.
- After line 142, insert:

```cpp
  EXPECT_EQ(russell.non_pit_aspects[1].rfind("values: ", 0), 0U);
  EXPECT_NE(russell.non_pit_aspects[1].find("snapshot-run-backfill-v1"), std::string::npos);
```

- Lines 135-136 become:

```cpp
  // Ruling ALSIG-7: gia_13f is point in time; russell_recon's presence is not (the house stage's
  // row set looks ahead to a delisting), declared as a "presence: ..." aspect; ruling ALD-3 adds a
  // "values: ..." aspect (the stage's 2026 directory and backfill-CIK inputs).
```

(iv) `scripts/tests/test_p9_al_sig_registrations.py`.
- Line 106 becomes
  `    # Rulings ALSIG-7 and ALD-3. The registry row declares the look-ahead exactly ("presence: ..." and "values: ...").`
- Line 109 becomes
  `    assert row["point_in_time"] is False and len(aspects) == 2 and aspects[0].startswith("presence: ")`
- After line 111, insert:

```python
    assert aspects[1].startswith("values: ")                 # ruling ALD-3: the 2026 directory and backfill CIK
    for phrase in ("2026-09-18", "snapshot-run-backfill-v1", "is_operating"):
        assert phrase in aspects[1], phrase
```

(v) **Proof.**
- `scripts/research-build.ps1` builds `atx-engine-research-fields-tests` and `atx-research-fields`.
- Run gtest `ResearchFieldsAlSig*:ResearchFieldsRegistry*`.
- Run pytest `scripts/tests/test_p9_al_sig_registrations.py`, then `atx-engine/tools/test_field_registry.py`, in
  separate invocations.
- The test edits are deliberate rule changes under ALD-3. But 5.3 moves a pinned value: the registry row's
  `formula_sha256` (`787a8d87…a699`, `field_registry.json:3398`; checked by the build, `registry.cpp:288-291`), and
  with it the SHA-256 of the registry file that later research-fields receipts record. Under ALD-11 it is therefore
  not applied in P9; it is a P10 note.

## Fix round 3

Completed 2026-10-04 by a fresh doc agent: text edits only, under re-review 2 (`task-AL-DATA-review.md`,
"## Re-review 2": 0 blocker, 1 major, 4 minor, 4 nit) and rulings ALD-9, ALD-11 and ALD-12.

**Method.** Read-only, as in rounds 1 and 2.
- atx-db source was read only through `git -C C:/atx-wt/pool-2 show dec03454:...`:
  - `alpha_panel/identity_links.py` (`:1-160`, `:280-380`), `alpha_panel/build.py:60-120`,
    `alpha_panel/common.py:125-150`;
  - `alpha_panel/identity_table.py` (`:160-168`, `:198-218`), `alpha_panel/identity_backfill.py` (`:40-46`,
    `:170-182`), `alpha_panel/identity_names.py:152-162`;
  - `identity_links.py` (`:325-336`, `:510-548`, `:722-737`, `:774-784`, `:830-856`, `:890-930`, `:1172-1182`);
  - `identity_reconstruction.py` (`:140-150`, `:352-376`, `:1368-1390`, `:1429-1500`, `:1560-1625`, `:1700-1726`).
- Our tree at pool-2 HEAD `5061579d`: `scripts/specs/p9/blocked/{lazy_prices,hf_crowd}.json` and
  `scripts/tests/test_p9_al_sig_registrations.py` (`git show HEAD:`), `scripts/wave_manifest.py:231-236`,
  `atx-engine/schemas/research_store/classes.json:15`, `russell_recon_field.cpp:20-21`, `registry.cpp:286-292`, and a
  grep of `field_registry.json` for `787a8d87` (`:3398`). The amendment targets have an empty diff from `20bc5f5f`.
- The 5.1 / 5.2 "before" blocks were checked byte for byte against `git show HEAD:` of the two files, and the "after"
  blocks against designs section 5 (both identical, by a scratchpad `diff`).
- No data was opened. No build, no pytest, no git write. Nothing was touched under `atx-db/`. No permission refusal
  occurred.

**Findings fixed: 9 of 9, plus ALD-12 (b) and (c).** Line numbers are those of the files after this round.

| finding | file:line changed | one-line description |
|---|---|---|
| **R2-M1** (major F; ALD-12 (a)) | request `:255-265` | new switch S4: Stage I records its seal-2025 validation (`seal`, `expected`, `got`, `pass`) in `identity/manifest.json` and continues instead of exiting; used only in the diff arms |
| R2-M1 | request `:266-269` | old `:219` reworded: S4 is the only check relaxed, only in this check-only rebuild; no input pin or hash guard is relaxed |
| R2-M1 | request `:246-247` | switch intro: S1 and S2 off in arm A; S3 and S4 on in every arm |
| R2-M1 | request `:216-239` | arms table "measures" column and new "What each comparison measures": A, B1 vs A, B2 vs A (not clean), B12 vs A (headline), B12 vs B1 (clean measure of (b)); (b2) counted "in A only", not a look-ahead; a zero bounds (a)-(c) but not the later-evidence residual |
| R2-M1 | request `:300-301`, `:309-312` | sanity list and D0: arm A's S4 result is a D0 line; the B arms' go to row 11; "report the counts and go on" is now runnable |
| R2-M1 | request `:351`, `:362` | table rows D0 and 11 carry the S4 results (B1 equals A; B2 / B12 differ by construction when (b) moves a pre-2025 link) |
| R2-M1 | request `:365-371` | "Zero": D0 includes S4 `pass`; the zero bounds paths (a)-(c); it does not clear the later-evidence residual |
| **R2-m1** | request `:118-151` | Stage I keep test restated; (b1) and (b3) marked look-aheads; (b2) restated with `identity_reconstruction.py:1483-1497`, `identity_links.py:922`, `:330-333`: kept until the rival is known, a dependence without a look-ahead |
| R2-m1 | request `:203-204` | A0.2 path (b) line: (b1) / (b3) look-aheads; (b2) matches a clock-honest rule |
| R2-m1 | request `:325-326`, `:349` | diffs also compare B12 with B1; "A" / "B" defined for "X vs Y"; new table column "B12 vs B1" |
| R2-m1 | request `:378-385`, `:391` | "How to read": B12 vs B1 is the clean measure of (b); B2 vs A is not; the requested (b2) sentence, verbatim; "B12 vs A is the headline" |
| R2-m1 | report `:115-121`, `:202-205`, `:369-382` | report path (b) text, ledger candidate (b) and the round-2 refinement corrected the same way ("mostly `low`" / "edge cases" removed) |
| **R2-m2** | request `:185-188` | "Not covered" gains the later-evidence residual (`identity_reconstruction.py:144-147`, `:1577`, `:1601-1608`, `:1715-1724`): a zero clears the three 2026 inputs, not this; PIT-b is its fix |
| R2-m2 | request `:369-371` | "Zero": the OD5-1 footnote names the residual |
| **R2-m3** (with ALD-12 (c)) | designs `:311-320` | section 5 status: 5.1 / 5.2 root applies at the wave-2 gate (content-only); 5.3 NOT applied in P9, P10 note, moves `formula_sha256` `787a8d87…a699` (`field_registry.json:3398`) |
| R2-m3 | designs `:332-333`, `:348-349`, `:355-357` | 5.1 / 5.2 headers marked "root applies at the wave-2 gate (content-only)"; 5.3 header marked "P10 note (ALD-11)" |
| R2-m3 | designs `:441-445` | "no expected hash or golden is touched" replaced: 5.3 moves the pinned `formula_sha256` (`russell_recon_field.cpp:20-21`, `registry.cpp:288-291`) and the registry file's SHA-256 |
| R2-m3 | report `:38-39`, `:49`, `:50`, `:86`, `:177` | Files list, required-items rows, consumer row and P10 queue item 6: 5.1 / 5.2 at the wave-2 gate; 5.3 a P10 note; through P9 one aspect |
| R2-m3 | report `:590-593` | 5.3 proof line: "no expected hash" replaced by the moved `formula_sha256` and ALD-11 |
| R2-m3 | request `:683-684` | A.6: "will carry a second aspect (P10); through P9 the row declares the presence aspect only" |
| ALD-12 (c) | report `:455-521` | §5: status table (5.1 / 5.2 "root applies at the wave-2 gate (content-only)"; 5.3 "NOT applied in P9: it moves the pinned `formula_sha256`; P10 note"); 5.1 / 5.2 as whole before / after lines with file paths and line numbers (lazy_prices 17, 18, 19, 20, 28; hf_crowd 20); "Test lines for 5.1 and 5.2: none change", with each assertion that reads the keys (`:43`, `:65-68`, `:71`, `:153`, `:155`, `:156`, `:162-163`) |
| **R2-m4** | request `:315-323` | D0 prints the published manifests' code fields (export `code`, `link_table_manifest` `code`, Stage I `code_version` / `sources`, r4 `code_sha256` / `code_files`) beside the SHA-256 of each file at `dec03454`; backfill and name manifests record no code identity |
| ALD-12 (b) | request `:313-314` | one line: our tree cannot tie `identity-bridge-v2-pit` to `dec03454` (SHA-256 pin and rule string only); the owner is asked to name the commit that built it |
| **R2-n1** | request `:246-247`, `:243`, `:296-298` | S3 is on in every arm; the directory guard "never trips"; sanity reads `W_B2` (no `W_B12`) |
| **R2-n2** | request `:423-425` | T3 gains a class proof that gives `common` (`identity_links.py:540-545`), else role N (`:330-333`) |
| R2-n2 | request `:434-437` | T4: share 2.5 plus symbol .5 (a lifecycle item weighs 1.0: `identity_reconstruction.py:356`, `:1373`, `:1386`); each line carries T3's class proof |
| **R2-n3** | request `:361` | row 10 filled for B2 (and every comparison): path (b) moves the strict CIKs in `link_table` |
| R2-n3 | request `:281-283` | the `export` phase must succeed on its first attempt (`phases/export-001` read by name; `identity_links.py:777-781`) |
| **R2-n4** | report `:258-263`, `:274` | the stopped-round section is marked historical and superseded; the two book figures are removed from its table |
| R2-n4 | request `:119-122` | Stage I keeps tier `high` / `medium` **or** basis `strict_dated` / `current_ticker_verified` (`:149`); reconstructed versions carry `reconstructed_<tier>` (`identity_links.py:922`) |
| consistency | request `:1`, `:154-155`; report `:1`, `:5`, `:15-18`, `:122-124` | titles say fix round 3; path (c) confirmed by re-review 2 and kept in A0 (ALD-9) |

**Not applied as stated:** none.

**Open (from re-review 2, for the PM; not text items):** S4 relaxes a reproduction check inside the check-only rebuild
(ALD-12 (a) asks for it); 5.3 stays a P10 note under ALD-11 unless the PM reads the re-derived fingerprint as a re-pin.
