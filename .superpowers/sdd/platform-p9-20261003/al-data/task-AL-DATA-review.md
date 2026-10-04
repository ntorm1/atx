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

---

## Re-review 2 (fix round 2; rulings ALD-8 to ALD-11, OD5-1), 2026-10-04

**Verdict: CHANGES_REQUIRED.** Counts: blocker 0, major 1, minor 4, nit 4.

**Safe to hand to the atx-db owner: yes, after R2-M1 and the four minors.** Every fix is a text edit in `al-data/`, so
a delta check of the lines named below is enough. Paths (a), (b1), (b3) and (c) hold in source. As written, the A0.3
procedure stops at Stage I in arms B2 and B12 whenever path (b) has any effect (R2-M1).

**Method.**
- Read-only and doc-only.
- atx-db source was read only through `git -C C:/atx-wt/pool-2 show dec03454:...`. Files: `identity_links.py`,
  `identity_reconstruction.py`, `alpha_panel/{identity_links,identity_backfill,identity_names,identity_table,build,
  common,panel}.py` and `stagelake/registry.py`. Each file was read in full or over every cited range.
- Our tree was read at pool-2 HEAD `2ee62800`.
  - The amendment targets and the universe code have an empty diff from the lane's `20bc5f5f`. Only `.gitattributes`
    moved (`scripts/specs/** text eol=lf`).
  - The working-tree changes to `scripts/specs/p9/blocked/*.json` are EOL-only (`git diff --ignore-cr-at-eol` is
    empty), so no amendment has been applied yet.
- For E, one JSON manifest of our tree was read: `build-equity/train-2020-2023-lo3/manifest.json`. Only its keys
  `universe.id`, `universe.reasons`, `universe.inputs.code`, `universe.inputs.identity_bridge` and
  `universe.link_member_cells` were read. The last holds five cell counts; no figure from it is quoted here.
- No data row was opened. Nothing was built, no pytest was run, no git write was made, and nothing under any `atx-db/`
  working directory was touched. No permission refusal occurred.

### R2.1 The eight re-review-1 findings (A)

| finding | status | evidence |
|---|---|---|
| RM1 | fixed | A0.1 now carries paths (a), (b) and (c), with citations. "What the directory does, and does not do" (request `:152-164`) replaces the two over-strong rows; checked at `identity_links.py:517-539`, `:859-861`, `:925-967`. A0.2 states the primary-only filter (`:176-178`). Report `:13-14`: "a dependence path ... exists in source". Q1 is marked superseded (`:204-206`). |
| RM2 | fixed as asked: the arms, the whole chain, new directories, D0 as a precondition only, the row set, the `primary` case (T1) and PIT-a | the procedure has a new defect that the re-review did not ask about: R2-M1 |
| RM3 | fixed | channel 4, input 2 (request `:453-462`); R7.2 (`:511-515`); `backfill_cik_on_rank` (`:595-599`); the aspect clause (designs `:371-377`) |
| Rm1 | fixed | request `:561-569`. I re-derived (3, 0), (4, 0), (2, 1) and the R1 ends from main `indexes.py:274-292` |
| Rm2 | fixed | report `:100-101` |
| Rm3 | fixed in the designs and in the report Evidence | report `:264` still quotes both figures, inside the superseded status table (R2-n4) |
| Rn1 | fixed | designs `:38-39`, `:45-46`, `:70-72`, `:97-98` |
| Rn2 | fixed | request `:447-449`; designs `:369-370`; `panel.py:569-570` |

### R2.2 Path (a) (B): stated correctly

Every citation holds at `dec03454`:
- `identity_table.py:72-85` has no tier filter, and `:76` keeps the backfill and name tiers.
- The `primary` column of `links_combined` is not selected (`:75-77`).
- The flag is computed over all tiers (`:111-112`).
- The islands split where the flag changes, and an island's `available_at` is the minimum over it (`:119-133`,
  `:131`).
- The pit export keeps strict and name rows only after the flag is set (`:172-176`, `:187-195`), and writes it as `P` /
  `J` (`:190`).
- Backfill lines: `identity_backfill.py:52-58`, `:71-89`, `:96-109`. Their clock is `:20`.

T1 tests this path.

### R2.3 Path (b) and the lane's correction (C)

**(ii) holds.** What reaches the bridge is a rival's 2026 ticker removing a link, and the 2026 list moving the end dates
of the line's other, early-stamped links.

- **(b1)**
  - `identity_links.py:1287-1289` passes the list to `reconstruct`.
  - `identity_reconstruction.py:1535-1551` adds the .5 item (weight at `:357`), clocked at the snapshot.
  - `:464-468` sums every item, with no clock.
  - `:1577` tests dominance on the final weights (ratio 3.0, `:374`).
  - A rejected candidate gets no link version at any clock. The rehearsal keeps it only as an unresolved row at the
    observation (`identity_links.py:881-894`).
  - So a rival's ticker that tips the 3x test removes a link that Stage I would hold from the candidate's own clock
    until the rival's evidence is known. **Real.**
- **(b3)**
  - `_segments` uses `:1706-1711`, `:1715-1719` and `:1722-1724`. A non-rival does not bound a segment (`:1601-1606`).
  - An acceptance that needs the 2026 item is clocked at the snapshot (`:1616`; `_pit_clock`, `:1418-1426`).
  - It still ends the earlier, early-stamped segment at its own start. **Real.**

**(i), the correction for (b2), is half right: the mechanism is right, the reach is wrong.**

- **Why the reach is wrong.**
  - A dominance winner is high or medium from its own clock. It turns `low` only once the rival's known share items
    reach the floor and overlap it (`_tier_history`, `:1483-1497`, called at `:1618`; `_tier_known`, `:1466-1480`).
  - The rehearsal writes one version per tier step (`identity_links.py:895-924`), with basis `reconstructed_<tier>`
    (`:922`).
  - A `low` step is role N (`:330-333`) and fails Stage I's keep test (`alpha_panel/identity_links.py:147-151`).
  - So Stage I drops the link **only from the rival's clock**. Before that, it keeps a high or medium P / J version
    stamped at the candidate's own clock. For that window, the re-review-1 sentence ("keeps a link stamped years
    earlier") was right.
- **What the lane gets right.** In both windows, today's outcome equals the clock-honest one:
  - before the rival is known, a clock-honest rule accepts the link;
  - after it is known, a clock-honest dominance test without the 2026 item rejects it.

  The exception is where the tier test and the rival test (`:1435-1443`: final ratio and spans) disagree.
- **Consequence for A0.3.** (b2) is a dependence without a look-ahead.
  - In arm B2, the line loses its window link through the later-rival rule (`:144-147`, `:1577`).
  - So B2 and B12 count those cells "in A only", although PIT-b keeps them.
  - The request's "this half reaches the strict tier only where ... disagree" (`:131-132`; report `:115-116`, `:196`,
    `:364`) is false as written (R2-m1).

### R2.4 Path (c) (D): confirmed, so under ALD-9 it stays in A0

**The mechanism.**
- `identity_backfill.py:118-119` writes the strict days UNION the backfill days.
- `identity_names.py:77` and `:148-152` delete every name day found in that file (docstring `:14-15`).
- The combine pass anti-joins `bf_days` again (`identity_backfill.py:129-130`).
- The pit export drops the backfill row (`identity_table.py:172-176`).
- So on such a line-day the pit bridge has no row at all.

**It can change pre-2024 rows and universe member-days.**
- Backfill days have no 2017-10 floor (R2.7).
- The withheld name row is dated: `available_at` is the dissemination at 22:00 (`identity_names.py:139`). It would
  qualify in our reader (`prepare_research_fields.py:1714`, `:1722-1723`).
- In arm A the cell fails universe test 1, "unlinked" (`prepare_recent_research.py:586`).
- Under S2 the cell is linked. It is kept when the name row is the CIK's P line and its class passes.

**Direction and coverage.** The path removes lines that survived to 2026, so it works against survivorship. B1 row 1
"in B only" sizes it, and T2 tests it.

### R2.5 The primary-only filter, and provenance (E)

**The filter: confirmed in code at HEAD.**
- Universe test 3 is `(link >= 0) & ~primary`, reason `secondary_line` (`prepare_recent_research.py:586`, `:728`,
  `:737`).
- The flag is `kind == "P"` (`prepare_research_fields.py:1684-1685`). It is set per cell from the P rows of the cell's
  CIK (`:1735-1736`).
- Rows qualify from `available_at <= mark(t)` (`:1714`, `:1722-1724`).
- The fields publish on P lines only (`:328-332`).

**The role: tied to this code, confirmed** (from the role manifest).
- `universe.inputs.code` records `prepare_recent_research` blob `62b762fc`, which is HEAD's blob.
- It records `prepare_research_fields` blob `e8b57af5`, which is the file at `eca04c18`. There, `BRIDGE_KINDS`,
  `BRIDGE_EXCLUDED_BASES`, `load_bridge` and `resolve_links` are byte-identical to HEAD's.
- `universe.id` is `linked-operating-v3`, `universe.reasons` names `secondary_line`, and `universe.link_member_cells`
  carries a non-zero `secondary` tally.

**The bridge: only partly tied.**
- The role records:
  - `identity_bridge.manifest_sha256` `09aac28f...`, equal to `base-lo3.json:52`;
  - the bridge's `rule` `identity-link-table-v1/pit`, `rehearsal_identity` true and `scope_complete` false. These are
    the values that `identity_table.py:41` and `:202-210` write.
- The bridge manifest's own `code` field (the SHA-256 of `identity_table.py` and `common.py`, plus the C:/atx
  `git_head`; `identity_table.py:214`, `common.py:130-147`) sits under `C:/atx/atx-db/data`. Our tree quotes it
  nowhere: `git grep 09aac28f` finds only pins.
- Even that field would not cover `identity_backfill`, `identity_names`, Stage I or the r4 rehearsal.
- **Not confirmed:** that the published bridge was built by the code at `dec03454`. D0 is the check, and R2-m4 makes it
  cheaper.

### R2.6 The A0.3 procedure (F)

| step | runs as written? | what it shows |
|---|---|---|
| S1 (`tickers=()` at `:1287-1289`) | yes. S1 sits in `identity_links.py`, which `:725-735` pins in every arm | removes the EV_SYMBOL item only |
| S2 (empty `cand`) | yes. `pres`, `bf_days` and `other_cik_end` are then empty, and neither pass writes a backfill day | turns off (a), (c), the directory and survival |
| S3 | yes, but `IDENTITY_R4_DIR` is a constant (`common.py:28`). So S3 is a code switch, like S1 and S2, and it is on in every arm, A included (R2-n1) | - |
| the directory guard `:846-848` | runs in every arm and passes, because no arm empties the directory | - |
| the prepare and rehearsal pins (`:740-741`, `:1218-1223`, `:1249-1250`) | pass with new W / R directories | - |
| **Stage I in arms B2 and B12** | **no**, whenever path (b) moves any link visible before 2025-01-01 (see the note below). The same check stops arm A if r4 does not reproduce, so D0's "report the counts and go on" cannot be followed | **R2-M1** |
| `build --only identity,backfill,names,combine,identity_table` | yes. The order is kept (`build.py:108-110`) and the environment is inherited (`:77`). The seeded inputs are the only root reads of these stages (`identity_backfill.py:44-46`; `identity_names.py:74-77`, `:132`, `:144`; `identity_table.py:47-55`). Panel core uses no identity (`panel.py:160`) | - |
| D0 | yes (the owner reads the published files) | reproduction only, never a side of the measured diff |
| consumer view, rows 1-9 | yes. It is limited to sessions up to 2023-12-29 and to rows with `start` and `available_at` before 2024 (row 9 is reported apart) | - |

*Note on Stage I.* `build` re-runs `compute_links` at seal 2025-01-01. It exits on any difference from hard-coded counts
of the published r4 (`alpha_panel/identity_links.py:64-72`, `:330-334`). `build.py:113-115` then stops the chain.

**A zero result.** It proves something only when D0 is clean and the sanity row passes.
- Then a zero B12 proves that the pit bridge, as read in 2017-2023, does not depend on:
  - the 2026 ticker list in reconstruction;
  - the 2026 directory;
  - survival to 2026.
- The directory reaches pit rows only through backfill candidates (confirmed at `identity_links.py:517-539`,
  `:926-967`, `:330-333`).
- A zero does not clear the later-evidence residual, which no arm varies (R2-m2).

**A non-zero result.**
- B1 attributes to (a) plus (c): row 3 against row 1, as the request says.
- B2 against A is not a clean measure of (b), for two reasons:
  - it counts (b2) window cells that carry no look-ahead (R2.3);
  - a snapshot link is written only for a retrospective row without an evidence id (`identity_links.py:926-927`). B2
    changes the evidence passed in at `:835` and `:852`, so it can also move the backfill candidates.
- B12 against B1 (both with the backfill tier off) isolates (b) (R2-m1).
- (b1), (b2) and (b3) can be told apart only through the identifiers and T3 / T4.

### R2.7 ALD-8 (G): settled in the lane's favour

**The two lines.**
- `identity_backfill.py:35`: `COMBINED_FROM = "2017-10-01"  # combined/strict day expansion starts here (panel warm-up
  is 2018)`
- `identity_backfill.py:116`: `        WHERE c.session_date >= DATE '{COMBINED_FROM}'`

**Which span each limits.**
- `:35` defines the constant, and `:116` is its only use, in the WHERE clause of `strict_days` (`:111-117`).
- So both limit the strict-day expansion only.

**What limits the backfill days.**
- The final run (`:71-89`), built from `root/prices` (`:45`, `:76`). The registry documents that stage as bars from 2018
  on (`stagelake/registry.py:119-120`).
- The CIK's first periodic filing (`:101`).
- The end of any strict link to another CIK (`:102-103`).
- Strict-wins, applied against the unfloored `links.parquet` (`:105-109`).
- In the bridge, every tier is further cut by `identity_table.py:72` and by the panel-core sessions.

**Conclusion.** Re-review 1 ("from 2017-10", `:35`) dated the backfill span by a constant that does not bound it. Path
(a) does not depend on this.

### R2.8 ALD-11 and the spec amendments (H)

**The tests, located.**
- `scripts/tests/test_p9_al_sig_registrations.py`:
  - `:156` pins exactly one history entry;
  - `:109` pins `len(aspects) == 1`;
  - `:43` and `:65-68` hold the horizon regex;
  - `:162-163` hold the `hf_crowd` phrases.
- gtest `research_fields_al_sig_fixture_test.cpp`:
  - `:140-142` pin one aspect;
  - `:125-128` compare the row's `formula_sha256` and `spec_text` with the C++ spec. Both values are computed there, not
    written as literals.

**5.1 and 5.2 (the blocked registrations): content only. Root may apply them.**
- `dsl` is unchanged, so `dsl_sha256` is unchanged (test `:71`; `wave_manifest.py:205-207`).
- Both new `notes` keep the required prefix and phrases.
- `blocked_on` stays a non-empty string (`:155`).
- `history` keeps one entry.
- No test edit is needed, and no digest pins these files (`classes.json:15`: `pinned_by: git`).

**5.3 (the second `russell_recon` aspect): under ALD-11 this is the second case. 5.3 is not applied in P9 and becomes a
P10 note.**
- The test edits change only the count and the content of `non_pit_aspects`.
- But the amendment moves the committed `formula_sha256` of the registry row (`field_registry.json:3398`,
  `787a8d87...a699`):
  - the source calls it the "formula fingerprint pinned by the registry check and the AL-SIG gtests"
    (`russell_recon_field.cpp:20-21`);
  - the build refuses a mismatch (`registry.cpp:288-291`).
- It also moves the SHA-256 of the registry file, which every later research-fields receipt records
  (`research_fields_cli.cpp:420`, `manifest.cpp:170`).
- The lane says so itself (designs `:400`), next to "no expected hash or golden is touched" (designs `:436-437`; report
  `:531`).
- No EXE-FREEZE exe is involved. The library links only into `atx-research-fields` and its tests
  (`atx-engine/CMakeLists.txt:326-327`; `tests/CMakeLists.txt:368-376`).

### R2.9 ALD-3, ALD-4 and ALD-5 (I): honoured

- **ALD-3.** Channel 4 has both inputs (request `:442-470`). R7.1 asks for (b) a dated history or (c) a written proof
  (`:504-510`).
- **ALD-4.** B1 (`:637-640`) and B3 (`:687-689`) carry .09-.37 and .32 against the .4 bar. Priority `:34-35` marks them
  low.
- **ALD-5.** B2 is dropped in one item that says why (`:36-38`).
- One consequence of R2.8: A.6 (`:605-607`) says the field "carries two declared aspects", but through P9 the row
  carries one (R2-m3).

### Re-review 2 findings (`file:line | severity | problem | required fix`)

**Major**

- **R2-M1.** request `:211-222` (switches), `:250-257` (D0)
  - **Problem.** Stage I's built-in validation refuses arms B2 and B12, the headline, exactly when they would be
    non-zero.
    - `alpha_panel/identity_links.py build` recomputes the links at seal 2025-01-01 and exits when the counts (rows,
      lines, CIKs, rows by tier, ambiguous line-days) differ from the published r4's hard-coded values (`:64-72`,
      `:330-334`). `build.py:113-115` then stops the chain.
    - Any (b1) or (b2) link change visible before 2025 trips it.
    - Arm A trips it too if D0 is non-zero, which contradicts "report the counts and go on".
    - "No switch disables a guard" (`:219`) misses it.
  - **Required fix.**
    - Add **S4**: in every arm, Stage I records its seal-2025 validation (`expected` and `got`) and continues, instead of
      exiting.
    - Report arm A's result as a D0 line, and the B arms' results in row 11. The B arms differ by construction whenever
      path (b) moves a pre-2025 link.
    - Reword `:219`: S4 is the only check relaxed, and only in this check-only rebuild. No input pin or hash guard is
      relaxed.

**Minor**

- **R2-m1.** request `:127-132`, `:302-311`, the table at `:280-295`; report `:115-116`, `:195-196`, `:359-371`
  - **Problem.** (b2) does reach the strict tier, from its own clock until the rival's evidence is known (R2.3). There
    it equals the clock-honest outcome. So B2 and B12 count those cells "in A only" without any look-ahead.
    - B2 against A can also move the backfill candidates (R2.6).
  - **Required fix.**
    - Restate (b2) with `:1483-1497`, `identity_links.py:922` and `:330-333`.
    - In "How to read", add: "B2 / B12 'in A only' includes (b2) cells that PIT-b also keeps; they appear because the
      no-ticker arm still applies the later-rival rule."
    - Add a column "B12 vs B1" as the attribution of path (b).
- **R2-m2.** request `:166-171` ("Not covered"), `:298-301`
  - **Problem.** A0.3 holds a residual fixed that A0.1 does not list. Acceptance, dominance and segment bounds also use
    evidence items that the archives date 2024-2026: a rival or successor known only after the seal
    (`identity_reconstruction.py:144-147`, `:1577`, `:1601-1608`, `:1715-1724`). No arm varies them.
  - **Required fix.** Add a "Not covered" bullet: "a zero clears the three 2026 inputs, not this residual; PIT-b is its
    fix". The OD5-1 footnote then names it.
- **R2-m3.** designs section 5 (`:311-317`, 5.3, `:436-437`); report `:48`, `:84`, `:441`, `:531`; request A.6
  `:605-607`
  - **Problem.** Under ALD-11, 5.3 moves a pinned digest (R2.8), so it is not applied in P9. The texts still present it
    as a root amendment for now and say no pinned hash moves.
  - **Required fix.**
    - Mark 5.3 "P10 note (ALD-11): moves `formula_sha256` `787a8d87...a699`".
    - Keep 5.1 and 5.2 as root amendments.
    - Request A.6: "will carry a second aspect (P10); through P9 the row declares the presence aspect only".
- **R2-m4.** request `:250-257` (D0)
  - **Problem.** Nothing in our tree ties the published bridge to `dec03454` (R2.5).
  - **Required fix.** Ask D0 also to print the published manifests' code fields beside their blob SHAs at `dec03454`.
    These are JSON reads only:
    - the export `manifest.json` `code` (`identity_table.py:214`);
    - `identity/link_table_manifest.json` `code` (`:165`);
    - `identity/manifest.json` `code_version` and `sources` (`alpha_panel/identity_links.py:326`, `:341`);
    - the r4 `manifest.json` `code_sha256` and `code_files` (`identity_links.py:1177-1178`).

    Also say that `backfill_manifest.json` and `name_manifest.json` record no code identity
    (`identity_backfill.py:178`, `identity_names.py:158`).

**Nit**

- **R2-n1.** request `:211`, `:207-208`, `:246-247`
  - "Off in arm A" is false for S3.
  - The directory guard is "never reached" should read "never trips".
  - B12 has no `W_B12`: the sanity check should read `W_B2`.
- **R2-n2.** request `:346-354` (T3), `:357-358` (T4)
  - T3's line needs a class proof that gives `common` (`identity_links.py:540-545`). Otherwise its role is N and
    Stage I drops it (`:330-333`).
  - T4's "lifecycle .5" cannot occur, because `lifecycle_weight` is 1.0 (`identity_reconstruction.py:356`, `:1373`,
    `:1386`). Use share 2.5 plus symbol .5.
- **R2-n3.** request `:294`, `:231-233`
  - Row 10 should also be filled for B2, because path (b) moves the strict CIKs in `link_table`.
  - The export phase must succeed on its first attempt. Stage I and `identity_backfill` read `phases/export-001` by name
    (`alpha_panel/identity_links.py:53`; `identity_backfill.py:43`; attempts are numbered at `identity_links.py:777-781`).
- **R2-n4.** report `:251-253`, `:264`; request `:119`
  - The stopped-round text still says no text was rewritten, and it quotes the two book figures.
  - Stage I keeps tier high or medium **or** basis `strict_dated` / `current_ticker_verified` (`:149`). Reconstructed
    versions carry `reconstructed_<tier>` (`identity_links.py:922`), so the conclusion holds. Cite `:922`.

### For root and the PM

- **ALD-11.** Apply 5.1 and 5.2. Do not apply 5.3 in P9; it becomes a P10 note.
- **ALD-9.** Path (c) is confirmed and stays in A0.
- **ALD-8.** Settled for the lane (R2.7).
- **Open: R2-M1.** S4 relaxes a reproduction check inside a check-only rebuild. Without it, B2 and B12 cannot run, and
  path (b) goes unmeasured. PM: confirm that this is acceptable.
- **Open: ALD-11.** I read "pinned hash" as covering a committed registry fingerprint that the build checks. If the PM
  treats re-deriving it as a re-pin rather than editing an expected hash, 5.3 may be applied as written.

## Delta check (fix round 3), 2026-10-04

**Verdict: APPROVE.** 9 of 9 re-review-2 findings are fixed as prescribed, and so are ALD-12 (a), (b) and (c). The
request may be handed to the owner, and root may apply 5.1 and 5.2. Two optional nits follow (D-n1, D-n2). Neither
blocks; root may fold them in at hand-off.

**Method.**
- Read-only and doc-only. The only write is this section.
- atx-db source was read only through `git -C C:/atx-wt/pool-2 show dec03454:...`:
  - `alpha_panel/{identity_links,build,common,identity_table,identity_backfill,identity_names}.py`;
  - `identity_links.py` (`:36-40`, `:325-336`, `:515-548`, `:700-790`, `:830-935`, `:1170-1335`);
  - `identity_reconstruction.py` (`:140-150`, `:280-285`, `:352-376`, `:1368-1390`, `:1405-1500`, `:1530-1626`,
    `:1700-1726`).
- Our tree was read at pool-2 HEAD `0f7a84b1`. The amendment targets, the registration test and `wave_manifest.py`
  have an empty diff from `20bc5f5f`.
- Scratchpad check: the 5.1 / 5.2 "before" lines were compared with `git show HEAD:` of the two files, and the
  "after" lines were substituted and parsed as JSON.
- No data was opened. Nothing was built, no pytest was run, no git write was made, and nothing under `atx-db/` was
  touched. No permission refusal occurred.

### A. The nine findings

| finding | status | where (request unless named) |
|---|---|---|
| R2-M1 | fixed | S4 `:255-265`; reworded guard line `:266-269`; switch intro `:246-247`; sanity `:300-301`; D0 `:309-312`; rows D0 and 11 `:351`, `:362`; "Zero" `:365-371` |
| R2-m1 | fixed | (b2) restated with `:1483-1497`, `identity_links.py:922`, `:330-333` (`:130-144`); A0.2 `:203-204`; "B12 vs B1" column `:349`; the prescribed sentence verbatim `:383-384`; report `:115-121`, `:202-205`, `:369-386` |
| R2-m2 | fixed | "Not covered" bullet `:185-188`, with the prescribed wording; OD5-1 footnote `:370-371` |
| R2-m3 | fixed | designs `:313-316`, `:332-333`, `:348-349`, `:355-357`, `:441-445`; report `:38-39`, `:49-50`, `:86`, `:177`, `:457-461`, `:590-593`; A.6 `:683-684` |
| R2-m4 | fixed | D0 provenance `:315-323`: all four manifest fields, and the no-code-identity line for backfill and names |
| R2-n1 | fixed | S3 on in every arm `:246-247`; the guard "never trips" `:242-243`; `W_B2`, no `W_B12` `:296-298` |
| R2-n2 | fixed | T3 class proof `:423-425` (checked at `identity_links.py:529-545`, `:330-333`); T4 share 2.5 plus symbol .5 `:434-437` (`identity_reconstruction.py:356`, `:1373`, `:1386`) |
| R2-n3 | fixed | row 10 open in every comparison column `:361`; export first attempt `:281-283` (`identity_links.py:777-781`) |
| R2-n4 | fixed | report `:258-263` marked historical, figures gone from `:265-276`; request `:119-122` cites `:149` and `:922` |

### B. R2-M1: the diff now runs, and the text says what it must

**Every arm runs to completion with S4, against `dec03454` as read.**
- **Stage I.** The only `SystemExit` in the identity chain is the validation at `alpha_panel/identity_links.py:333-334`.
  S4 replaces it, and `receipt["validation"]` (`:332`, `:351-354`) is still written to `build_root()/identity/
  manifest.json` (`:347`; `common.py:42-45`).
  - The other raises in Stage I are code invariants (`:191`, `:197`, `:199`, `:251`) and the perm-id check (`:318`).
  - S1 does not touch either: it changes only the evidence items, not `security_permanent_ids`.
- **Backfill, names and table.** `identity_backfill`, `identity_names` and `identity_table` contain no `raise`,
  `assert` or validation (grep at `dec03454`).
- **Build.** `build.py:113-115` stops only on a non-zero stage exit.
- **Inputs.** No input is refused.
  - The rehearsal pins compare within the arm's own new W / R directories (`identity_links.py:735-755`, `:1248-1250`).
  - The ticker source is checked against the same receipt in every arm (`:842-843`).
  - S1 lives in `identity_links.py`. So `reconstruction_code_sha256` (`:1244`, `identity_reconstruction.py`) is
    unchanged.
  - The directory guard (`:846-848`) never trips.

**The text states all three required points.**
- **B12 vs B1 is the clean measure of path (b):** `:221`, `:231-232`, `:325`, `:380`.
- **(b2) is not a look-ahead:** `:130`, `:141-144`, `:203-204`, `:233-237`, `:383-384`.
- **A zero does not clear the later-evidence residual:** `:187-188`, `:238-239`, `:365-371`. This satisfies ALD-12
  (a).

### C. ALD-12 (b): present

`:313-314` reads: "Our tree cannot tie the published bridge `identity-bridge-v2-pit` to `dec03454` (it holds only the
bridge's SHA-256 pin and its rule string); please name the atx-db commit that built it." Every alpha-panel citation
behind D0 holds at `dec03454`:
- `identity_table.py:165`, `:214`;
- `common.py:130-147`;
- `alpha_panel/identity_links.py:326`, `:341`;
- `identity_links.py:1177-1178`;
- `identity_backfill.py:178` and `identity_names.py:158`. Grep confirms that no receipt key of either file holds code.

### D. Spec amendments

**5.1 and 5.2 "before" lines.** Each is byte-identical to `git show HEAD:` of its file:
- `lazy_prices.json` lines 17, 18, 19, 20 and 28;
- `hf_crowd.json` line 20.
- Both files are LF with no BOM.

**"After" lines: valid.**
- Substituted, both files parse.
- The key set and order are unchanged, `dsl` and `dsl_sha256` are unchanged, and `history` keeps one entry.
- Each new `notes` starts with the old value.
- The changed keys are exactly `formula`, `domain`, `deviation`, `notes` and `blocked_on` (lazy_prices) and `notes`
  (hf_crowd).
- Every "after" line equals designs section 5 text.

**Readers and pins.**
- `scripts/tests/test_p9_al_sig_registrations.py` is the only reader of these files in the tree.
  - Every assertion that reads them still holds: `:63`, `:65-68`, `:71`, `:153-158`, `:162-163`.
  - `wave_manifest.py:152-164`, `:196-241` hold too.
- `classes.json:15` is `pinned_by: git`.
- The research-store goldens use synthetic trees (`atx-engine/tests/fixtures/research_store`).
- Neither file's SHA-256 nor its blob id occurs anywhere in HEAD.
- So applying 5.1 and 5.2 moves no digest, golden, pinned hash or test expectation.

**5.3 is marked NOT applied in P9 (P10 note).** See designs `:315-316`, `:355-357` and `:441-445`, and report `:461`
and `:520-521`. The supporting citations hold at HEAD:
- `field_registry.json:3398` (`787a8d87…a699`);
- `russell_recon_field.cpp:20-21`;
- `atx-engine/src/research/fields/registry.cpp:288-291`.

### E. New statements against source

Every citation added in this round was spot-checked; all hold except where the two nits below qualify them. Two
statements carried from re-review 2's own prescription are over-strong. Both are optional fixes:

- **D-n1 (nit; the over-strong text came from re-review 2's R2-M1 wording).** request `:263-264` and row 11 `:362`.
  - **Problem.** The seal-2025 check compares only counts (`alpha_panel/identity_links.py:351-354`: rows, lines,
    CIKs, `min_start`, rows by tier, ambiguous line-days).
    - A moved end date (b3), or offsetting changes, can leave the counts equal.
    - A 2024-only change can trip the check, although rows 1-10 stop at 2023-12-29.
    - So "whenever ... exactly when they would be non-zero" and "by construction" are too strong.
  - **Replacement for `:263-264`:** "Without S4, arms B2 and B12 stop at Stage I whenever path (b) changes one of
    those counts before 2025-01-01, as almost any link it moves does; arm A stops whenever r4 does not reproduce them
    (D0)."
  - **Replacement for the row 11 tail in `:362`** (from "B1's equals A's"): "B1's equals A's, and B12's equals B2's
    (same rehearsal output); B2's differs from A's whenever path (b) changes one of the counts before 2025-01-01.
    Equal counts do not show that path (b) moved nothing."
- **D-n2 (nit).** request `:316-317` and `:322-323`.
  - **Problem.**
    - `code_identity` writes one `git_head` per field, not per module (`common.py:144`).
    - Stage I records no code hash. `code_version` is the constant label `identity-links-v1`
      (`alpha_panel/identity_links.py:52`), and `sources` hashes the r4 inputs (`:296-301`).
  - **Replacement for `:316-317`:** "The alpha-panel `code` fields hold `sha256` and `sha256_lf` per module and one
    `git_head` (`alpha_panel/common.py:130-147`)."
  - **Replacement for `:322-323`:** "`identity/backfill_manifest.json` and `identity/name_manifest.json` record no
    code identity (`identity_backfill.py:178`, `identity_names.py:158`), and Stage I records only the label
    `code_version` (`alpha_panel/identity_links.py:52`) and its input hashes (`:296-301`). For these three stages the
    named commit is the only tie."

**Out of scope, for the PM (round-2 text, not edited in this round).** Report `:450-451` still asks whether to add path
(c) to the OD5-1 flag line. ALD-9 already answered it: the flag points at the A0 list.
