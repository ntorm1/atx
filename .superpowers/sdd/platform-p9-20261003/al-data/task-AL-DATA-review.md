# Review: lane AL-DATA (doc-only, W3-18), fresh read-only reviewer, 2026-10-04

**Verdict: CHANGES_REQUIRED.** Counts: blocker 0, major 6, minor 12, nit 3.

**Safe to hand to the atx-db owner: yes, after these edits.** Make M1-M5 and the request-side minors. Part A is
sound at its core: all three look-ahead channels are real.

Scope: the three files in `al-data/`, the brief, the ledger (ALSIG-2/4/5/7, W3-18, ALD-1/2), and AL-SIG re-review N1.
atx-db source was read only through `git -C C:/atx-wt/pool-2 show` at `dec03454` (main), `0e2fa077` (wh) and
`c0eb2f74` (the current warehouse tip). Engine files were read at `f6444417`; there is no engine or impl diff to HEAD
`f96f9399`. No data was opened, nothing was built, no pytest was run, and no git state changed. The EKP NBER w28056
PDF was fetched and its text was searched.

## 1. The three look-ahead claims: all confirmed from source

| channel | source (main / wh) | true as stated? | concrete timeline (synthetic) |
|---|---|---|---|
| presence (ALSIG-7) | `indexes.py:274-279` / `:287-292`. `last_session` = `max(tradingDate)`, `security_master.py:140` / `:167`. Stamp `:272,283` | yes | R2021 is ranked 2021-05-07 and first readable at session 05-11. Line X (deal pending) last trades 06-01. From 05-11 the snapshot lacks X, so `russell_recon` reads NaN (verified: NaN unless the line is in both R' and R, `russell_recon_field.cpp:111-120`). The missing print first shows at the close of 06-02. |
| `effective_to` | `:274-277,281` / `:287-290,294` | yes | R2023 is ranked 2023-04-28 and readable from 05-02. A line with L = 2024-02-20 gets `effective_to` 2024-02-20 on a row stamped 2023-04-28 22:00, so a 2024 fact sits inside TRAIN. The lead is up to about 13.5 months. |
| `recon_summary` | `:284-292` / `:297-305` | yes, but the wording is loose (m3) | P is in R2022 and is reselected in R2023, with L = 2023-06-01. The R2023 summary, stamped 04-28, counts P as an **addition** (`cur - alive_prev`). A build censored at 05-15 counts P as continuing. |

On the proposed rule: R1, R3 (its stamp equals the house `delisting.py` convention, "first session after L
(22:00 UTC)"), R4 and R6 are correct. R2 and R5 have gaps (m4, m6). The test fails today on assertions 1, 2 and 4
(once the fixture calendar is fixed) and passes on a correct fix. It does not catch channel 3 (M3).

Citations hold: `common.py:35`, `stage.hpp:3-7,44-46,55-59,70-73`, `russell_proxy.hpp:32-33` (the reader reads 5
columns, not `effective_to`: `russell_proxy.cpp:82-83`), `strategy_ic_admission.cpp:583-586`,
`thirteenf.hpp:9-11,15-16`, `filing_text.py:1-30,58-65`, `text_features.py:1-28,55-60`, `compustat_std.py:178,199,207`
and `compustat_std.md:12-13,17,26-50` (wh). Exceptions are m2 and n1.

## 2. Findings (`file:line | severity | problem | required fix`)

**Major**

- **M1.** `atx-db-request.md:92-93` and `:145`
  - **Problem.** "Under R1-R3, no row stamped before 2024-01-01 depends on a fact dated 2024-01-01 or later" is false.
    The rank-date eligibility in `snapshot()` filters on panel `security_type`, `is_adr` and `is_lp`. The panel takes
    these from the 2026-09-18 Nasdaq directory snapshot (`directory_type`) whenever the FINRA type is stale, unknown
    or `common_unverified`. Sources: `panel.py:227-236`, `:574` (main and wh); `security_master.py:9-11`, `:146-148`.
    That snapshot exists only for lines still trading in 2026, so the member set itself depends on survival to 2026.
    The engine follow-up "drop the `russell_recon` `non_pit_aspects` entry" would then label a survivorship-dependent
    field as point in time.
  - **Required fix.**
    - Scope the sentence to `intervals()`.
    - Add a residual item asking the owner to restrict index eligibility to point-in-time `security_type_basis`
      (exclude `directory_snapshot`) or to state that it cannot.
    - Make `:145` conditional: re-derive the aspect so it names the residual, unless the owner confirms the
      restriction.
    - PM ruling needed (see the end).
- **M2.** `atx-db-request.md:139-140`
  - **Problem.** This acceptance check cannot be run, and it would prove nothing. `counts_on_recon` is the summary
    `lines` = `len(r["members"])` (`indexes.py:249,288`), which already counts the dropped lines today. The receipt
    has no per-recon row count, only the total `constituent_rows` (`:233`). Root may not count rows in
    `constituents.parquet`, which is one file holding rows stamped 2024-2026.
  - **Required fix.** Ask for a new receipt key, e.g. `rank_vintage_rows_on_recon` per `(index_id, recon_date)`, and
    compare it with `counts_on_recon`.
- **M3.** `atx-db-request.md:99-130`
  - **Problem.** The fixture is incomplete in three ways.
    - (a) R1 and R3 have no members, so `prev` is empty, R2's additions and deletions are `None` in both runs, and the
      `recon_summary` half of assertion 4 has no teeth. Channel 3 then has no acceptance at all (see M2).
    - (b) "A calendar holding the sessions named below" has no session after 2022-06-24, so
      `cal[bisect_right(cal, recon)]` (`indexes.py:269`) raises IndexError on both today's code and a fixed one. R1's
      `eff` would also be wrong.
    - (c) "Alive" is modelled as `None`, but production `last_session` is never NULL (see m5).
  - **Required fix.**
    - Use a full business-day calendar from 2020-05-01 to 2022-12-30.
    - Give R1 the members X, W (in R1 only, L 2021-06-10) and V (in R1 and R2, alive).
    - Assert R2's `additions` / `deletions` are equal across the full and censored runs at T = 2021-05-20; today they
      differ.
    - Model "alive" as L = the last calendar session.
- **M4.** `atx-db-request.md:254-310` (B2) and `builder-designs.md:119-127`
  - **Problem.** B2 ignores the owner's existing stage `thirteenf_filer_type` (main `filer_type.py:1-27`,
    `adv.py:1-26,62-67,79-92`), which already lands:
    - monthly ADV rosters;
    - bulk Part 1 tables for 2011-11-05..2024-12-31, including `IA_Schedule_D_7B1` and `IA_1D3_CIK`. These answer
      both `[verify]` items at `:267` and `:286`;
    - a dated `filer_type = hedge_fund` and a `filer_type_pit`.

    The B2 stamp rule ("per-filing timestamp where the archive has one") also conflicts with the house clock
    `adv-bulk-publication-v1`: bulk rows are available at the zip's Last-Modified (2025-09-26 / 2026-05-01), with
    `vintage_risk adv_bulk_backfill`. Under that clock, Schedule D GAV is invisible to TRAIN. Rosters carry fund-type
    counts only from 2023 (`adv.py:9-10`). So `hf_crowd`'s GAV-share definition has no point-in-time input before
    2023.
  - **Required fix.** Either drop B2 from the handed file (it is deferred, ALD-2, and keep it only in the designs) or
    rewrite it as a delta on `thirteenf_filer_type` with an explicit stamp choice. In both cases set `hf_crowd`
    `sources` to the house stage.
- **M5.** `atx-db-request.md:350-351` (B3 check iii)
  - **Problem.** The premise is false for house data. House `xsga` is XBRL `SellingGeneralAndAdministrativeExpense`
    (wh `fund_items.py:93`), and R&D is a separate element (`:104`). "xsga includes xrd under standard practice" is
    EKP's statement about **Compustat** XSGA. Verified in the w28056 text: "We subtract xrd from xsga because xrd is
    included in xsga under standard accounting practices." The 95% gate would fail on correct rows of R&D-intensive
    issuers.
  - **Required fix.** Drop (iii), or replace it with a check that reports the convention. See m11 for the matching
    design change.
- **M6.** `builder-designs.md:38`, `:45-48`, `:103-108`; report `:76`, `:119-120`
  - **Problem.** The F1 SR_c band ".4-.65 net" has no derivation.
    - The lane's own inputs (gross .34-.87, cost drag 0 to < .05) give net about .29-.87, centre about .50, so the
      low end fails the bar.
    - The "decays by about a quarter" threshold compares the gross centre .55 with the **net** bar. At the stated .05
      drag the threshold is about 18%.
    - The lit §6 back-solve (.50-.64) is arithmetically right but circular: lit's +.03 to +.06 is itself an estimate
      from the same 34-58 bps.
    - The cost drag "< .05" has no turnover-times-cost arithmetic.
    - Lit §2.5's base rate is not cited (McLean-Pontiff: returns 58% lower after publication, giving .55 to about
      .23), although TRAIN is post-publication.
  - **Required fix.** Print the arithmetic range, the net-basis threshold and the base rate. The honest verdict is:
    "the centre clears, narrowly, only without a post-publication haircut (ALD-1); the low half of the stated inputs
    and the lit base rate fail." PM: ALD-1 was ruled without §2.5 in view.

**Minor**

- **m1.** `atx-db-request.md:48-49`
  - **Problem.** "4-7 weeks early" is imprecise. The lead runs from one session up to about 7 weeks (Russell
    2020-2022) or about 8 weeks (2023+, April rank day). For SP500P (rank at the end of Feb / May / Aug / Nov,
    effective after the third Friday) it is at most about 3 weeks, although `:39` says "applies to all four".
  - **Required fix.** Restate it as "up to about 7-8 weeks (Russell), about 3 weeks (SP500P)".
- **m2.** `atx-db-request.md:37`, `:9-10`, `:188`
  - **Problem.**
    - Main `dec03454` has the same test at `:62-77`, assertion `:72`; the "-" is wrong.
    - `docs/datasets/text_native.md` does not exist at `0e2fa077`. It was added in `9ac8af95` and exists at the tip
      `c0eb2f74`.
    - `feat/tier1-v3-warehouse` has moved to `c0eb2f74`.
  - **Required fix.** Cite main `:62-77`/`:72`, cite text_native at `c0eb2f74`, and say the citations are pinned by
    SHA.
- **m3.** `atx-db-request.md:61-64`, `:81-83`
  - **Problem.** The channel-3 wording is loose. A prior member that delists between the rank date and the recon date
    is dropped from `alive_prev`. It is then not a deletion, and if reselected it is counted as an addition. Also,
    `LAKE_STAGES` registers `recon_summary` with `"clock": None` (`indexes.py:75`), so the lake applies no stamp at
    all.
  - **Required fix.** Reword, and add to R4 a request that `recon_summary` be registered with its clock.
- **m4.** `atx-db-request.md:72-74`
  - **Problem.** R2's "NULL for the last reconstitution" makes a rank-vintage `effective_to` depend on the extent of
    the vendor calendar. A reconstitution is only built when `recon_d < cal[-1]` (`:198,218`), and the next one comes
    from the data (`:270`). So the 2023 vintage's value depends on whether data extends past 2024-06-28. The censoring
    test censors `last_session` but not the calendar.
  - **Required fix.** Take the scheduled end from the schedule constant whatever the data extent, and add a
    calendar-truncation case to assertion 4.
- **m5.** `atx-db-request.md:75-80`
  - **Problem.** R3 keys the exit on `last_session`, which is never NULL (`security_master.py:140`). In the last
    reconstitution, an alive line would get a spurious exit after the vendor's last session.
  - **Required fix.** Define the exit from `delisting_date` (`:163`, NULL while alive) or from
    `last_session < cal[-1]`.
- **m6.** `atx-db-request.md:81-86`
  - **Problem.**
    - R4's "separate columns stamped at the recon-date mark" conflicts with one `available_at` per row (C1).
    - R5 does not say which vintages build the membership row of session s. If it uses the latest exits, row s
      (stamped (s-1) 22:00) drops a line one session before its exit row (stamped (L+1) 22:00) is visible.
  - **Required fix.**
    - R4: put the realised counts in separate vintage **rows**.
    - R5: build row s only from vintages stamped at or before its own stamp.
- **m7.** `atx-db-request.md:235-236`
  - **Problem.** The B1 population ("links ... to any engine-universe line in that year") has three gaps.
    - The prior-year comparator of an entrant's first filing is not landed, so the entrant reads NaN and gate (ii)
      measures the population rule rather than the landing.
    - The house panel and universe start in 2018 (`common.py:30`), so the rule cannot be computed for the 2012-2017
      band.
    - The owner's own `text_native` already uses "the complete original SEC annual cohort, independent of future
      member selection" (`c0eb2f74` `text_native.md:7-8`).
  - **Required fix.** Use all original 10-K / 10-Q filings accepted 2012-2023. This meets C4 by construction. Frame B1
    as extending `text_native` to 10-Q and full-document cosine (its output layout is the owner's choice, provided
    parts are by year).
- **m8.** `atx-db-request.md:245-247`
  - **Problem.** The denominator of the coverage gate includes CIK-years that cannot have a usable row: a first
    filing year, and 20-F / 40-F filers, whose forms B1 does not land.
  - **Required fix.** Exclude them or define them in the gate.
- **m9.** `builder-designs.md:84-96`
  - **Problem.** The `lazy_prices` probe lacks two cases.
    - A row with `prior_available_at >= available_at`: the builder reads the column, but no rule uses it.
    - A row-level seal case: a row stamped 2024-01-01 inside a `year=2023` part.
  - **Required fix.** Add both cases. The builder sets such rows to NaN or refuses them.
- **m10.** `builder-designs.md:242`
  - **Problem.** "NaN maps to `default_fee_bps`" is wrong. `BorrowSchedule::fee()` returns the grid cell
    (`borrow_schedule.hpp:77-79`), and validation rejects a non-finite fee.
  - **Required fix.** The builder writes `default_fee_bps` into cells with no data.
- **m11.** `builder-designs.md:40`, `:169`, `:202-207`
  - **Problem.**
    - "Fails both halves" applies the rho half to the add framing (.7 / .76). For a replace, rho belongs to the
      difference portfolio and is unknown. The SR half alone (.32, pre-cost) suffices.
    - The .62 for 2007-2018 is discarded for "overlapping OD-3", which is a contamination flag, not a bar criterion.
      The consistent reason is the full-sample rule used for F1.
    - `bm_int` uses house `xsga`, which excludes R&D. EKP capitalise 100% of Compustat XSGA, which includes R&D.
    - The EKP numbers are verified: 0.32 / 0.62, 76% (76.2%), δ .2, g .1, January 1975 - December 2018.
  - **Required fix.** Reword. Define SG&A as `xsga + xrd` when R&D is reported separately.
- **m12.** `builder-designs.md:32`, `:207`; report `:67-68`
  - **Problem.** Two book TRAIN statistics are quoted: SR_b 1.77 with the §3.0 gains, and X-4's -.058. Both come from
    the public lit review §3.0 / §0.1, which brief rule 3 permits, and not from any output. But the report's "the only
    Sharpe ratios read are published literature figures (EKP)" is inaccurate. `atx-db-request.md` quotes none.
  - **Required fix.** Correct the report sentence. Dropping the X-4 number is optional.

**Nit**

- **n1.** `atx-db-request.md:164-165`: the 13F reader seals by the part's calendar quarter (`thirteenf.hpp:15-16`).
  Say "an analogous rule".
- **n2.** `builder-designs.md:45-46`: JF 75(3) is June 2020, so January-May 2020 precede the publication. All of TRAIN
  does follow the working paper's circulation.
- **n3.** `builder-designs.md:36`: the rho column is rho to *themes* (lit §3.1 [est]), while the bar is rho to the
  *book* (§3.0). Say that theme rho is used as book rho by the ALSIG-2 / -4 convention.

## 3. Bar arithmetic and literature (task item 3)

**Checked and correct:**
- lit §3.0: +.045 / +.014, and the bar "net SR_c >= .4 and rho <= .1".
- rho for F1 <= .1, F2 .2, F4 .3, F5 .2, F6 .3, F10 .7.
- F1: 34-58 bps/month is 4.1-7.0% a year; gross .34-.87; centre .555.
- The back-solve gives .503 / .639; .55 x .73 = .40.
- BHL 2004-2016; CMN 1995-2014 (`task-AL-SIG-review.md:63-64`).

**Unsourced or misapplied:** the ".4-.65" band, the "< .05" cost drag and the gross/net mix (M6); the decay base rate
is missing (M6).

**The honest reading:** F1 clears only at the centre and without a haircut. F2, F4-F6 and F10 fail. For F2 and F5 the
failure rests on theme rho being taken as book rho (n3).

## 4. Planted-leak probes (task item 4)

- **`lazy_prices` (a).** The probe has teeth in both directions. `<=`, mark(t) and mark(t-2) each break it.
- **`hf_crowd`.** The probe catches "last vintage for all quarters".
- **`bm_int` restatement.** The probe catches a reader that ignores the clock and a reader one session early.
- **borrow.** The probe catches trade-date stamping and mark(t).
- **Gaps.** For the gaps see m9 and, for the atx-db test, M3.

## 5. Book statistics (task item 5)

`atx-db-request.md` contains none. `builder-designs.md` quotes public lit / ledger numbers only (m12). No 2020-2023
output was read.

## Needs a PM ruling

1. **M1.** Is the panel's directory-snapshot `security_type` fallback an accepted house convention for index
   eligibility? If yes, the `russell_recon` aspect is re-worded, not dropped. If no, it becomes a fourth ask.
2. **M6.** Reconfirm ALD-1 (no post-publication haircut) now that lit §2.5's -58% base rate is in view.
3. **M4.** Drop B2 from the handed file, or rewrite it now as a delta on `thirteenf_filer_type`?

---

## Re-review (fix round 1; rulings ALD-3 to ALD-6), 2026-10-04

**Verdict: CHANGES_REQUIRED.** Counts: blocker 0, major 3, minor 3, nit 2.

**Safe to hand to the atx-db owner: yes, after these edits.** A0 and channel 4 must be corrected first. Channels 1-3,
B1 and B3 are ready.

**Method.** Read-only, as in round 1. atx-db source was read through `git show` at `dec03454`, at `0e2fa077` and at
the tips `c0eb2f74` / `75efb231`; the cited files are identical across them, and the branch has since moved to
`f3b0aa75`. Pool-2 engine, tools and specs were read at `f6444417`. The 35 / 17 / 50 bridge-field counts were
recomputed from `lib-v8ysb-gm.json` and `field_registry.json`.

### 1. Round-1 findings

Twenty of the 21 are fixed as required:
- the majors M2, M3, M4, M5 and M6;
- every minor from m1 to m12;
- the nits n1, n2 and n3.

**M1** is fixed for the directory path: channel 4, R7, the TRAIN claim scoped to `intervals()`, the conditional A.6
and the second aspect. But channel 4 is still incomplete (RM3 below).

The M3 fixture now has teeth on all three channels. Its "fails today" explanation for assertion 4 is wrong (Rm1).

### 2. A0 and the Consumers trace, checked against source

| claim | source | holds? |
|---|---|---|
| `build_rehearsal` reads the 2026 directory and the SEC company-ticker list | `identity_links.py:840-852` (the `candidates` phase) | **yes** |
| its output is the strict (r4) tier of `identity-bridge-v2-pit` | `identity_backfill.py:3`, `:43-44` (the r4 export); `identity_table.py:3`, `:172-193` | **yes** |
| `base-lo3.json` and the fields code consume the bridge | `base-lo3.json:49-53`; `prepare_research_fields.py:1557-1559`, `:1673-1685` (reads `sr_id, cik, start, end_incl, available_at, primary, basis`); `prepare_recent_research.py:29-35`, `:130-135`, `:699` | **yes**. 35 + 17 fields, 2 with both sources, 50 in all. The universe keeps **only P lines** (`prepare_recent_research.py:13`, `:586`; `prepare_research_fields.py:1685`, `:1735-1736`). |
| "the directory's classes feed the bridge's class guards and rule-5 history" (`market_owner_bridge.py:1748`, `:1842-1845`) | `identity_links.py:851-861`, `:925-967` | **too strong.** In `build_rehearsal` that output feeds only snapshot-observation links (stamped at the observation, `link_start` = that date) and statistics. Reconstructed candidates take their class from `_class_proof_at`, which excludes current directories (`:517-523`; empty set at `:539`). |
| "current-ticker links carry a modelled `available_at` at their first trade date" (`:1770-1810`) | `identity_links.py:933-953` | **not true of the r4 output.** `build_rehearsal` re-stamps these links at the observation, with `scope: snapshot_observation_onward`. The backdating belongs to the backfill tier. |
| "whether any pre-2024 link depends on them **cannot be told from source**" | see below | **not the honest answer.** Two dependence paths are visible in source; only their size needs data. |

**The two paths A0 misses.**

**(a) `primary` through the backfill tier.** This path is certain, and it reaches the universe.
- `identity_table` computes `is_issuer_primary` per (cik, session) over every linked line of **all** tiers, including
  `snapshot-run-backfill-v1` (`identity_table.py:68-85`, `:107-116`, line `:111`).
- The islands are split on that flag (`:119-132`). Only then does the pit export drop backfill rows (`:187-193`).
- Backfill lines exist only for lines alive at the 2026 snapshot whose current-ticker link the 2026 directory made
  common (P or J):
  - `identity_links.py:954-967`, then `_company_versions` `:330-333`;
  - `identity_backfill.py:7-20`, `:52-58`.
  - They are backdated over the line's final run from 2017-10 (`:10-14`, `:35`).
- So a strict or name line can be marked J only because a 2026-surviving sibling had higher prior adv63 that day.
  The role universe then drops it as "secondary".
- Both snapshots feed this path.

**(b) The SEC ticker snapshot inside r4 reconstruction.**
- Preparation step `reconstruct` passes the ticker list in (`identity_links.py:1287-1289`).
- A ticker match adds an `EV_SYMBOL` item of weight .5, known at the snapshot
  (`identity_reconstruction.py:1535-1551`).
- `_pit_clock` keeps the link clock honest for the threshold only (`:1411-1426`). Acceptance, dominance and segment
  bounds use the final weights, which include that item (`:1564-1581`, `:1601-1608`).
- atx-db documents this residual itself (`:144-147`). A rival's 2026 ticker can remove an early link, and a line's own
  ticker can make it dominate a rival, which keeps a link stamped years earlier.

**Is the empty-snapshot diff valid and sufficient?** Not as specified (RM2).
- With the full chain rebuilt A/B and the visible rows diffed, zero differing rows **would** be sufficient.
  `primary`, tie-breaks and `end_incl` are all materialised in the rows, so any effect shows as a row difference.
- Path (a) makes a non-zero result likely even if no strict link changes.

### 3. ALD-4 arithmetic

Every band is derived and correct:
- F1: 4.08-6.96%; gross .34 / .87 / .55; drag 2·2·.00125/.10 = .05; net .29-.87.
- ALD-4: .42 x .34 - .05 = .09; .42 x .87 = .37; centre .18. To clear, gross must reach (.4 + drag)/.42 = .95-1.07.
- F4: 50 x 52 = 26% a year; x .42 = 10.9%; cost 2 x 26 x .00125 = 6.5%; net 4.4%.
- F5: 17.6% x .42 = 7.4%, less 6.5%, leaves .9%.
- F10: 2.11 / 6.53 = .323, verified in w28056.

The haircut is applied as ruled. The lane haircuts gross, not costs; ALD-4's .12-.37 gives the same verdict. "No
family clears" follows.

### 4. Channel 4 and the aspect text

The directory path is accurate (`panel.py:226-236`, `:259-262`, `:574`; `security_master.py:9-11`, `:144-148`). It is
incomplete for the CIK (RM3).

### 5. Book statistics

`atx-db-request.md` has none. `builder-designs.md:26` and report `:63`, `:215` still name SR_b 1.77 and X-4's -.058
(Rm3).

### Remaining findings (`file:line | severity | problem | required fix`)

**Major**

- **RM1.** `atx-db-request.md:35-55` (A0.1, A0.2); report `:96-119`, `:132`, `:140`, `:186-188`
  - **Problem.** A0 states one mechanism too strongly and misses the two real ones: paths (a) and (b) in section 2,
    and the two "too strong" / "not true" rows of the table. The report says the dependence "cannot be told from
    source". Its Q1 ("P9 cells may proceed") is superseded by ALD-6(d).
  - **Required fix.**
    - Rewrite A0.1 with paths (a) and (b), with citations, and drop or qualify the two over-strong bullets.
    - A0.2: add that the role universe keeps only P lines.
    - Report: "a dependence path exists in source; its size needs the owner's diff". Update Q1 to ALD-6.
- **RM2.** `atx-db-request.md:57-66`, `:73-81` (A0.3, A0.5)
  - **Problem.** The check cannot be run as written, and it is not sufficient.
    - (i) "Rebuild the strict tier" does not name the chain. A rebuild that reuses `links_combined` misses path (a) and
      can return zero while `primary` depends on the snapshot. Path (b) is in `prepare_rehearsal_inputs` step
      `reconstruct`, whose outputs `build_rehearsal` reuses by hash (`identity_links.py:830-834`).
    - (ii) The code refuses an empty directory (`:847-848`) and changed pinned inputs (`:740-741`, `:842-843`,
      `:1248-1250`).
    - (iii) A diff against the *published* bridge mixes in code or input drift.
    - (iv) `start < 2024` is the wrong row set.
    - (v) The acceptance test has no `primary` case.
  - **Required fix.**
    - Specify A/B rebuilds of the whole chain: reconstruct, the `build_rehearsal` phases, `identity/links.parquet`,
      `identity_backfill`, `identity_table` and the pit export. Use new work and build directories, and a check-only
      bypass of the non-empty-directory guard.
    - First confirm that the with-snapshot rebuild reproduces the published rows.
    - Diff every consumed column on rows with `start < 2024-01-01 AND available_at < 2024-01-01`, and report other
      rows separately.
    - Say a non-zero result through `primary` is likely.
    - Add a test: a CIK with a strict line X and a backfill-only line Y with higher adv63. X's pit `primary` in rows
      stamped before 2026 must not depend on Y.
    - Extend the rule: `primary` is computed only among the export's own tiers and links visible at the session.
- **RM3.** `atx-db-request.md:150-166`, `:199-205`, `:209-210`, `:276-278`, `:284-286`; `builder-designs.md:350-357`
  - **Problem.** Channel 4 misses the panel's `cik`. The panel takes `cik` from `link_table`, **backfill tier
    included**, gated by `evidence_at` (= `valid_from` 22:00): the 2026 choice of CIK, backdated (`panel.py:468-477`;
    `identity_table.py:20-24`, `:144-145`; atx-db's own non-PIT label at `export_impl.py:346`).
    - `snapshot()` groups lines into companies by that `cik` (`indexes.py:151`, `:166-174`), and `is_operating` keys on
      it (`panel.py:262`). So company ME, ranks, p1000 and eligibility also depend on the 2026 snapshot through the
      CIK.
    - R7 (a)-(c) and `directory_decided_on_rank` do not cover this. Meeting R7 would not close channel 4, and A.6
      would drop the aspect too early.
  - **Required fix.**
    - Add the CIK path to channel 4.
    - Add R7 (d): index inputs use strict / name links only, or the owner reports the decisions a backfill link made.
    - Add a receipt key for it.
    - Add a clause to the aspect text: "... and its company (CIK) from the snapshot-run-backfill-v1 link tier".

**Minor**

- **Rm1.** `atx-db-request.md:250-253`
  - **Problem.** The reason given for assertion 4 failing today is wrong. On the as-of inputs, today's code gives
    every R2 member `last_session` 2021-05-19 < eff, so X is missing in **both** runs and R2's summary is (4, 0), not
    (2, 1). The assertion still fails today:
    - Z, Y and V appear only in the full run;
    - R1 `effective_to` is 06-01 / 06-10 / 06-25 against 05-19;
    - the summary is (3, 0) against (4, 0).

    (2, 1) is what a fixed build gives in both runs.
  - **Required fix.** Restate it.
- **Rm2.** Report `:88-96`, `:139`
  - **Problem.** The trace omits the consumers of the backfill tier:
    - the panel `cik` (RM3), and through `is_operating` every `member_equity` reader;
    - the `delisting` stage, which joins `link_table` with no tier filter (`delisting.py:9`, `:55`, `:72-80`) and
      decides `continued` on up to 30 days after L while stamping at L+1 (`:7-8`, `:74-78`, `:132-134`).

    The `delisting` path is inert for lo3 (`base-lo3.json:4`: "delisting attributes only, --delisting-returns off").
  - **Required fix.** Add the rows, and note that the 30-day continuation is a look-ahead if returns are ever turned on.
- **Rm3.** `builder-designs.md:26`; report `:63`, `:215`
  - **Problem.** The two public book figures remain, although the designs no longer use them.
  - **Required fix.** Delete them.

**Nit**

- **Rn1.** `builder-designs.md:69` vs `:44-45`: F1 dates publication by the working paper, F2 by the journal. No
  verdict changes. Use one rule.
- **Rn2.** `builder-designs.md:351-352`: the aspect says "stale, unknown or common_unverified"; the request (`:154`)
  also says NULL. Align them.
