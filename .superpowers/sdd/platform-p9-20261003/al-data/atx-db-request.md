# Request to the atx-db owner (P9 lane AL-DATA, 2026-10-04; fix round 3)

- **From:** the atx P9 sprint, lane AL-DATA. This lane writes documents only (Ruling W3-18).
- **To:** the atx-db owner. A0 goes to the identity owner (stages `identity`, `identity_backfill`, `identity_names`,
  `identity_table` and the r4 rehearsal). The rest of part A goes to stage owner MKT, panel U3 and security_master.
  Parts B1 / B3 go to the TXT and fundamentals owners.
- **How it reaches you.** With our P9 freeze report (PM ruling OD5-1 (iv)). P9 changes nothing under `atx-db/` and
  opens no atx-db data.
- **Citations.** Every citation was read from source with `git -C C:/atx-wt/pool-2 show <sha>:<path>` and is pinned
  by SHA:
  - `main` = `dec03454`; "wh" = `0e2fa077` (the warehouse branch; `indexes.py` and `security_master.py` differ from
    main, so both line numbers are given).
  - The files cited in A0 and in channel 4 (`identity_links.py`, `identity_reconstruction.py`,
    `market_owner_bridge.py`, `alpha_panel/{identity_links,identity_backfill,identity_names,identity_table,panel,`
    `delisting,export_impl,common,build}.py`) are byte-identical at main and at the current warehouse tip
    `8b55bbbe`. `stagelake/registry.py` is cited at main only.
  - Every file cited in A.1-A.5 and B3 is byte-identical at `0e2fa077` and `8b55bbbe`.
  - `docs/datasets/text_native.md` does not exist at `0e2fa077` and is cited at `c0eb2f74`. At `8b55bbbe`, 22 lines
    were inserted after its line 27, so the cited `:46-49` and `:74` read `:68-71` and `:96` there.
  - Our own tree (`atx-engine`, `scripts`) is cited at pool-2 `20bc5f5f`; the cited files are unchanged since
    `f6444417`.
  - Your working tree in `C:/atx` was not read.
- **The rule behind every item.** A row stamped `available_at = A` holds only facts that were public at A.
  - The engine reads a stage row at role session t only if `available_at < date(t-1) 22:00 UTC`
    (`atx-engine/include/atx/engine/research/fields/sources/stage.hpp:44-46`).
  - TRAIN is `[2020-01-01, 2024-01-01)`.

**Priority.**
1. **Part A** is the request.
   - **A0 (the identity bridge check) comes first**, because only you can run it (PM rulings ALD-6, ALD-7).
   - Under PM ruling OD5-1 it no longer gates our P9 work: P9 runs on the bridge as published, with a flag, and your
     result annotates our freeze verdict. Nothing is re-run.
   - Then come the four look-ahead channels in the indexes stage and its inputs.
2. **B1 and B3** are low priority. Our own bar does not admit the fields that would use them (the numbers sit with
   each part).
3. **B2** (the Form ADV join) is deferred and dropped from this file. Its only consumer, `hf_crowd`, fails our bar
   (expected correlation .2 > .1; PM rulings ALSIG-4, ALD-5). If it is ever needed, it will be written as a delta on
   your existing `thirteenf_filer_type` stage (main `alpha_panel/filer_type.py:1-25`, `adv.py:1-26`).

---

## A. Look-ahead: the identity bridge (A0) and the indexes stage (ALSIG-7 and three more channels)

### A0. Identity bridge `identity-bridge-v2-pit`: dependence on 2026-dated inputs (PM rulings ALD-6, ALD-7, OD5-1)

**The published artefact.** `export/identity-bridge-v2-pit` under your build root, manifest SHA-256
`09aac28f757fa959b0ed4cd9296b2267940e70af98e0d2c67cc45b1df4f7fa01`. Our spec pins it
(`scripts/specs/v8/base-lo3.json:49-53`).

#### A0.1 How 2026-dated inputs reach rows stamped before 2024 (main `dec03454`)

**The build chain** (`alpha_panel/build.py:32-33`, `:42-43`, `:58`; `stagelake/registry.py:129-150`):
1. **r4 rehearsal.**
   - `python -m atx_db.identity_links prepare` runs steps `vendor`, `facts`, `reconstruct`
     (`identity_links.py:1231-1309`).
   - `python -m atx_db.identity_links rehearsal` runs phases `symbols` .. `export` (`:665`, `:702-1170`).
   - Output: `session8-phased-r4/phases/export-001/security_company_links` (`alpha_panel/common.py:28`).
2. **Stage I.** `alpha_panel.identity_links build` writes `identity/links.parquet`, the strict tier. It drops role `N`
   rows and basis `current_ticker_verified` (`alpha_panel/identity_links.py:14-16`).
3. **Tiers.**
   - `identity_backfill`, first pass, writes `_tmp/identity_days_strict_backfill.parquet` (`identity_backfill.py:118-119`).
   - `identity_names` writes `identity/links_name_days.parquet`.
   - `identity_backfill`, second pass, writes `identity/links_combined.parquet`
     (`stagelake/registry.py:138`, `:146`).
4. **Table and exports.** `identity_table` writes `identity/link_table.parquet` and the exports
   (`identity_table.py:172-195`).

**The three 2026-dated inputs:**
- the SEC company-ticker list (receipt input `company_tickers`, observed in 2026);
- the Nasdaq Trader directory (OPS export `nasdaq_symbol_directory`);
- survival to 2026 in the vendor price file (`identity_backfill.py:36`, `:88`).

**Path (a): the pit export's primary flag is computed with the backfill lines.** Certain from source.
- `identity_table.py:72-85` joins every row of `identity/links_combined.parquet` to the panel-core sessions, with no
  tier filter. Backfill and name rows keep their tier (`:76`). `links_combined` holds strict, backfill and name rows
  (`:3-4`; `identity_backfill.py:132-152`, `:169`).
- `identity_table.py:111-112` sets the flag over all tiers:
  `row_number() OVER (PARTITION BY d.cik, d.session_date ORDER BY d.adv63 DESC NULLS LAST, d.security_id) = 1 AS is_issuer_primary`.
  The `primary` column of `links_combined` is not read (`:75-77`).
- `:119-133` split the islands wherever the flag changes. An island's `available_at` is the minimum over it (`:131`).
- **Only after that** does the pit export drop the backfill rows (`:172-176`, `:187-195`). `:190` writes the flag as
  `P` / `J`.
- **Which lines are backfill lines:**
  - r4 `current_ticker_verified` P / J links still open at the 2026 snapshot (`identity_backfill.py:52-58`);
  - on the line's final run (no gap over 21 sessions) ending on or after 2026-09-01 (`:71-89`);
  - on days after the CIK's first periodic filing, after any strict link to a different CIK, and not strict-linked
    (`:96-109`).
  - Their `available_at` is the snapshot (`:20`; `identity_table.py:20-21`).
- **The span of the backfill days** (PM ruling ALD-8; the two passages, verbatim):
  - `identity_backfill.py:35`: `COMBINED_FROM = "2017-10-01"  # combined/strict day expansion starts here (panel warm-up is 2018)`
  - `identity_backfill.py:116`: `        WHERE c.session_date >= DATE '{COMBINED_FROM}'`
  - `:116` is the only use of `:35`. It is the WHERE clause of `strict_days` (`:111-117`, the strict links expanded to
    sessions), so it limits the **strict** days only.
  - The **backfill** days (`bf_days`, `:96-104`) are limited by the final run (`:71-89`), the first periodic filing
    (`:101`), the other-CIK end (`:102-103`) and strict-wins (`:105-109`). They have no 2017-10 floor.
  - In the bridge, every tier is further limited to `identity_table.py:72` (`for year in range(2017, 2027)`) and to the
    panel-core sessions (`:63-67`, `:79`). We did not read where panel core starts; the house warm-up start is
    `common.py:30` (2018-01-02).
- **Effect.** On a (cik, session) whose most liquid linked line by prior adv63 is a backfill line, every strict and
  name line of that CIK is `J` in the pit export that session. The islands, and with them `start`, `end_incl` and
  `available_at`, split there.

**Path (b): the 2026 ticker list inside r4 link acceptance.**
- `identity_links.py:1287-1289`: the `reconstruct` step passes
  `tickers=ir.read_sec_ticker_snapshot(inputs.company_tickers.path)` and
  `ticker_observed_at=inputs.company_tickers.received_at`.
- `identity_reconstruction.py:1535-1551`: when a symbol the line traded under is a current SEC ticker of the candidate
  CIK, the candidate gets `EvidenceItem(EV_SYMBOL, snapshot_at, None, params.symbol_weight, ...)`. The weight is .5
  (`:357`) and the item is known at the snapshot.
- `:464-468`: `Candidate.weight` sums every item, that one included, with no clock.
- These rules all use the final weights:
  - the threshold (`:1573`);
  - dominance, `candidate.weight < params.dominance_ratio * weight` against the rivals' final weights (`:1577`;
    ratio 3.0, `:374`);
  - the segment bounds set by rejected rivals (`:1601-1608`).
- The link's clock is `_pit_clock` (`:1616`, `:1418-1426`). It tests only the threshold (`_passes`, `:1411-1415`) on
  the items known by then. A link that needs the .5 to reach the threshold therefore gets the snapshot clock.
- **What reaches the strict tier.**
  - Stage I keeps a version whose tier is `high` or `medium`, or whose basis is `strict_dated` or
    `current_ticker_verified` (`alpha_panel/identity_links.py:60-61`, `:149`). It then drops role `N` and basis
    `current_ticker_verified` (`alpha_panel/identity_links.py:151`). A reconstructed version's basis is
    `reconstructed_<tier>` (`identity_links.py:922`), so for it only the tier counts: `high` or `medium`.
  - An accepted candidate with competitors is tier `low` from the clock at which its rival's known share items reach
    the floor and overlap it (`identity_reconstruction.py:1466-1497`, called at `:1618`).
  - **(b1) A rival's 2026 ticker removes a link. A look-ahead; it reaches the strict tier.**
    - Without the ticker, the candidate would be accepted, and it would sit in the strict tier at `medium` / `high`
      until the rival's own evidence is known.
    - With it, the candidate fails dominance, and no version exists at any clock.
    - Your module documents this residual (`identity_reconstruction.py:144-147`).
  - **(b2) A candidate's own 2026 ticker lifts it to 3x a rival. A dependence, not a look-ahead; it reaches the strict
    tier.**
    - With the ticker, the candidate is accepted with its own early clock. Its tier steps (`_tier_history`,
      `identity_reconstruction.py:1483-1497`) are `high` / `medium` until the rival's known share items reach the floor
      and overlap it, and `low` from then on.
    - The rehearsal writes one version per tier step, with basis `reconstructed_<tier>` (`identity_links.py:895-924`,
      `:922`). The `low` step has role `N` (`identity_links.py:330-333`) and fails Stage I's keep test
      (`alpha_panel/identity_links.py:147-151`).
    - So Stage I keeps a `high` / `medium` P / J link from the candidate's own clock until the rival's evidence is
      known, and drops it from then on. Without the ticker, the candidate fails dominance
      (`identity_reconstruction.py:1577`) and has no version at any clock.
    - Both windows match a clock-honest rule: before the rival is known, it accepts the link; after, a dominance test
      without the 2026 item rejects it. The exception is where the tier test (known items) and the rival test (final
      ratio and spans, `identity_reconstruction.py:1435-1443`) disagree.
    - So arms B2 and B12 of A0.3 count these window cells "in A only", although PIT-b keeps them.
  - **(b3) The segment ends of the line's other CIKs move. A look-ahead; it reaches the strict tier.** An acceptance
    that needs the 2026 item is stamped at the snapshot, but its place in the accepted set moves the other CIKs'
    segment ends, and those links are stamped early:
    - an accepted later candidate ends the earlier segment at its own start (`:1722-1724`), dated by a listing filing
      when one exists (`:1706-1711`);
    - a rejected rival bounds it at the rival's first evidence (`:1715-1719`);
    - a non-rival does not bound it at all (`:1601-1606`).
- Stage I passes the surviving links into the strict tier (`alpha_panel/identity_links.py:9-18`).

**Path (c): the name tier is withheld on backfill days.** Found in fix round 2 and confirmed in source by our second
re-review; it stays in this list (PM ruling ALD-9).
- `identity_names.py:14-15` says the link applies "only on days without a strict or backfill link".
  - `:77` and `:148-152` delete every (line, session) of `_tmp/identity_days_strict_backfill.parquet`, which holds the
    strict days UNION the backfill days (`identity_backfill.py:118-119`).
  - The second pass anti-joins the backfill days again (`:129-130`).
- The pit export then drops the backfill rows (`identity_table.py:172-176`). On those line-days the line has **no**
  pit link, even where a dated FINRA name link existed.
- **Which lines.** Those alive at the 2026 snapshot that hold a P / J current-ticker link, over their backfill span
  (path (a)). Their pit coverage before 2024 therefore depends on survival to 2026 and on their 2026 directory class.

**What the directory does, and does not do** (this corrects round 1, which overstated it).
- In `build_rehearsal`, the directory and the ticker list are read at `identity_links.py:840-852` and feed
  `classify_reconstructed_with_history`.
  - Its output is used only for snapshot-observation links and for statistics (`:859-861`, `:925-967`).
  - Reconstructed candidates take their class from `_class_proof_at`, which excludes current directories (`:517-523`;
    an empty set at `:539`).
- Snapshot links are stamped at the observation, with `link_start` = that date and scope `snapshot_observation_onward`
  (`:933-953`). The bridge module's modelled first-trade-date clock (`market_owner_bridge.py:41-46`, `:1770-1777`) is
  not used here.
- The directory decides whether a snapshot link is common (`:954-967`; `_snapshot_class_status`, `:673-684`), and only
  common lines become P or J (`_company_versions`, `:330-333`).
- Stage I drops these links (`alpha_panel/identity_links.py:14-16`). They reach rows before 2024 only as backfill
  candidates (`identity_backfill.py:52-58`): through paths (a) and (c), and through the panel CIK (channel 4 below).

**Not covered by this item.** The check below holds these fixed in every arm, so it does not measure them:
- the companyfacts and submissions archives are 2026 retrievals with modelled clocks
  (`identity_reconstruction.py:147-149`);
- the price file is the 2026-09-20 TickerHistory3 snapshot (`stagelake/registry.py:123`);
- the OPS tables `securities`, `exchange_listings` and `security_identifier_history`, loaded at
  `identity_links.py:824-828`, were not traced;
- **the later-evidence residual.** Acceptance, dominance and segment bounds also use evidence items that the archives
  date 2024-2026, such as a rival or successor known only after the seal (`identity_reconstruction.py:144-147`,
  `:1577`, `:1601-1608`, `:1715-1724`). No arm varies them. A zero clears the three 2026 inputs, not this residual;
  PIT-b (A0.4) is its fix.

#### A0.2 Why it matters to us

- Our accepted book's role universe `linked-operating-v3` reads this bridge (`base-lo3.json:43-53`).
  - It keeps a cell only when the cell's qualifying rows name one CIK and include a `P` row. A `J` cell fails as
    "secondary" (`prepare_recent_research.py:13-15`, `:586`, `:728`, `:748`; `prepare_research_fields.py:1684-1685`,
    `:1735-1736`).
  - A row is used from `available_at <= ` the session mark (`prepare_research_fields.py:1714`, `:1722-1723`). So a
    `J` set on a row stamped years early is read years early.
- 50 of our book's 84 fields read the bridge (35 with source `identity_bridge`, 17 with `sec_identity_bridge`, 2 with
  both). They publish values on `P` lines only (`prepare_research_fields.py:328-332`).
- What each path does to our universe:
  - **(a)** drops an issuer's strict / name line on the sessions when a 2026-surviving sibling was more liquid;
  - **(c)** drops lines that survived to 2026 over their backfill span, wherever a dated name link existed;
  - **(b)** removes early strict links (b1) and moves their segment ends (b3), both look-aheads; (b2) keeps a link
    only until the rival is known, as a clock-honest rule would.
- The size and the net direction are unmeasured.

#### A0.3 The check: rebuild against rebuild

**Principle.**
- Every arm is a fresh rebuild from the same pinned inputs with the same code. Arms differ only in the inputs varied
  below.
- The published bridge is never a side of the measured diff. Step D0 compares it with arm A as a precondition only.

**Arms.**

| arm | r4 `reconstruct` step | backfill tier | measures |
|---|---|---|---|
| A | as published (2026 ticker list in) | as published | baseline of every comparison; D0 compares it with the published files |
| B1 | as A (reuses A's rehearsal output) | empty (S2) | vs A: paths (a) and (c), and every directory and survival effect |
| B2 | without the 2026 ticker list (S1) | as published | vs A: path (b), not cleanly (below) |
| B12 | as B2 (reuses B2's rehearsal output) | empty (S2) | vs A: all three together, **the headline**; vs B1: path (b), **the clean measure** |

What each comparison measures:
- **A** is the baseline. It differs from the published bridge only as D0 reports.
- **B1 vs A**: paths (a) and (c), with every directory and survival effect. B1 covers the directory fully, because
  the directory reaches rows before 2024 only through backfill candidates (A0.1).
- **B2 vs A**: path (b) with the backfill tier on. It is **not** a clean measure of (b). A snapshot link is written
  only for a retrospective row without an evidence id (`identity_links.py:926-927`), and S1 changes the evidence
  passed in at `:835` and `:852`, so B2 can also move the backfill candidates, and with them paths (a) and (c).
- **B12 vs A**: all three paths together, interactions included. **The headline.**
- **B12 vs B1**: **the clean measure of path (b).** Both arms have the backfill tier off (S2), so they differ only in
  the 2026 ticker list.
- **(b2) is counted "in A only", and it is not a look-ahead.** In B2 and B12, a (b2) line loses its window link,
  because without its own ticker the candidate fails dominance against the later rival
  (`identity_reconstruction.py:1577`; the residual at `:144-147`). PIT-b keeps those cells (A0.1, path (b2)). So
  "in A only" in B2 vs A, B12 vs A and B12 vs B1 includes (b2) cells. (b1), (b2) and (b3) are told apart only through
  the identifiers (A0.4) and T3 / T4.
- **A zero bounds paths (a)-(c). It does not clear the later-evidence residual** (A0.1, "Not covered"), which no arm
  varies.

Directory and ticker list:
- No arm empties the directory. The guard `identity_links.py:846-848` ("nonempty pinned directory was not read by
  bridge adapter") therefore never trips.
- The `candidates` phase of every arm reads the same directory and the same ticker list (`:840-852`).

**Check-only switches.** They sit in the code that every arm runs. S1 and S2 are off in arm A; S3 and S4 are on in
every arm, A included.
- **S1.** The `reconstruct` step passes `tickers=()` to `ir.reconstruct_in_batches` (`identity_links.py:1287-1289`).
  Nothing else changes.
- **S2.** `identity_backfill` builds an empty candidate set `cand` (`identity_backfill.py:52-58`), so neither pass
  writes a backfill day.
- **S3.** `IDENTITY_R4_DIR` (`common.py:28`, read by `alpha_panel/identity_links.py:53-56` and
  `identity_backfill.py:43`) points at the arm's rehearsal output. The build root already follows
  `ATX_ALPHA_PANEL_ROOT` (`common.py:38-39`).
- **S4.** Stage I (`alpha_panel/identity_links.py build`) records its seal-2025 validation and continues, instead of
  exiting.
  - Today `build` recomputes the links at seal 2025-01-01 (`alpha_panel/identity_links.py:330-332`) and raises
    `SystemExit` when they differ from hard-coded counts of the published r4 (`:333-334`; `VALIDATION_EXPECTED`,
    `:64-72`: rows, lines, CIKs, `min_start`, rows by tier, ambiguous line-days). `build.py:113-115` then stops the
    chain.
  - With S4, `build` keeps the receipt entry `validation` (`seal`, `expected`, `got`, `pass`; `:351-354`) and goes on.
    The entry is written to `P_<arm>/identity/manifest.json` (`:347`).
  - Without S4, arms B2 and B12 stop at Stage I whenever path (b) moves a link visible before 2025-01-01, which is
    exactly when they would be non-zero; arm A stops whenever r4 does not reproduce (D0).
  - S4 is used only in the diff arms of this check-only rebuild.
- **S4 is the only check relaxed, and only in this check-only rebuild. No input pin or hash guard is relaxed.**
  - New work, out and root directories satisfy the pin guards (`identity_links.py:740-741`, `:1249-1250`).
  - S1 lives in `identity_links.py`, whose hash the rehearsal pins (`:725-735`). So arm A must run the same file,
    with S1 off.

**Procedure** (from `C:/atx/atx-db`, under your memory guard; every directory is new and named per arm):
1. **Rehearsal, arms A and B2 only.** Use the inputs the published rehearsal pinned:
   - the receipt whose inputs match the published `preparation.json` `pin.inputs` (`identity_links.py:1243-1245`;
     the receipt is hash-checked at `:1218-1223`);
   - the OPS manifest and the document review that the published `build.json` pins (`:729-734`).
   - `python -m atx_db.identity_links prepare --work W_<arm> --receipt <receipt> --step vendor`, then `--step facts`,
     then `--step reconstruct` (S1 on for B2).
   - `python -m atx_db.identity_links rehearsal --work W_<arm> --ops-manifest <ops> --out R_<arm> --phase <p>
     [--document-review <file>]`, for p = `symbols`, `candidates`, `uncertainty`, `links`, `names`, `export`, one
     process each (`:1319-1330`).
   - The `export` phase must succeed on its first attempt. Stage I and `identity_backfill` read `phases/export-001` by
     name (`alpha_panel/identity_links.py:53`; `identity_backfill.py:43`), and a retry writes `export-002` (attempts
     are numbered at `identity_links.py:777-781`). If it fails, re-run the arm's six phases into a new `R_<arm>`.
2. **Alpha-panel identity stages, every arm,** in root `P_<arm>` (`ATX_ALPHA_PANEL_ROOT`).
   - S3 points at `R_A` (arms A, B1) or `R_B2` (arms B2, B12). S2 is on for B1 and B12.
   - Seed `P_<arm>` with the published inputs these stages read that do not depend on identity: `prices/`, the
     calendar, `short_interest/si.parquet`, `_tmp/panel_core/` and `_tmp/panel_member/`. Core and membership are
     vendor-derived (`panel.py:6-8`, `:160`).
   - Leave the FINRA raw directory (`ATX_FINRA_SI_DIR`) and the FSDS / companyfacts staging (`common.py:24`, `:26-27`)
     as they are.
   - `python -m atx_db.alpha_panel.build --only identity,backfill,names,combine,identity_table` (`build.py:3-6`,
     `:32-33`, `:42-43`, `:58`). `--only` keeps the `STAGES` order (`:101-116`), and every stage inherits the
     environment (`:76-77`), so `ATX_ALPHA_PANEL_ROOT` and the switches reach it.
3. **Sanity checks**, so that a zero is not vacuous:
   - arms B1 and B12: `identity/backfill_manifest.json` has `backfill_rows` = 0; arms A and B2: > 0;
   - arm B2 (whose rehearsal output arm B12 reuses; there is no `W_B12`): no `symbol_snapshot_match` item
     (`EV_SYMBOL`, `identity_reconstruction.py:283`) in `W_B2/evidence/*.parquet`; arm A: some, in
     `W_A/evidence/*.parquet`;
   - every arm: `identity/backfill_manifest.json` has `name_tier` true;
   - every arm: `identity/manifest.json` carries the Stage I `validation` entry (S4). Arm A's goes to D0; the B arms'
     go to row 11.

**D0, reproduction and provenance** (before any B result is read).
- Compare `R_A/phases/export-001/security_company_links.part-0000.parquet` with the published file.
  - Use every column except `created_at`, which is the build's wall clock (`identity_links.py:745`, `:756`).
  - Key on `(perm_security_id, perm_company_id, link_start, available_at)` (`TABLE_KEYS`, `:39`).
- Compare `P_A/export/identity-bridge-v2-pit/links.parquet` and `P_A/identity/link_table.parquet` with the published
  files, as full rows.
- Report arm A's Stage I validation (S4; `P_A/identity/manifest.json`, key `validation`): `pass`, and `got` beside
  `expected` when `pass` is false. `pass` false means r4 did not reproduce the published counts.
- Expect zero differences and `pass` true. If not, report the counts and go on; S4 lets the chain continue. The A/B
  result then measures today's code and inputs, and D0 says how far the published bridge is from them.
- **Provenance.** Our tree cannot tie the published bridge `identity-bridge-v2-pit` to `dec03454` (it holds only the
  bridge's SHA-256 pin and its rule string); please name the atx-db commit that built it.
- Also print these fields of the published manifests (JSON reads only), each code hash beside the SHA-256 of the same
  file at `dec03454`. The alpha-panel `code` fields hold `sha256`, `sha256_lf` and `git_head` per module
  (`alpha_panel/common.py:130-147`).
  - the export `manifest.json` `code` (`identity_table.py:214`);
  - `identity/link_table_manifest.json` `code` (`identity_table.py:165`);
  - `identity/manifest.json` `code_version` and `sources` (`alpha_panel/identity_links.py:326`, `:341`);
  - the r4 `manifest.json` `code_sha256` and `code_files` (`identity_links.py:1177-1178`).
  - `identity/backfill_manifest.json` and `identity/name_manifest.json` record no code identity
    (`identity_backfill.py:178`, `identity_names.py:158`). For those two stages the named commit is the only tie.

**The diffs.** Each B arm is compared with arm A, and B12 also with B1 (the clean measure of path (b)). In a
comparison "X vs Y", "A" in the rows below means Y and "B" means X.
- **Consumer view, key `(sr_id, session_date)`.** Take every pit `sr_id` and every calendar session d from the
  bridge's first session to 2023-12-29. The state is what our reader resolves at d 22:00 UTC.
  - Qualifying rows: `start <= d <= end_incl`, `available_at <= d 22:00 UTC`, `primary` in (P, J) and
    `basis <> 'current_ticker_verified'` (`prepare_research_fields.py:1686-1693`, `:1714`, `:1722-1723`).
  - State = (the number of distinct CIKs among them: 0, 1, or more; that CIK; whether any of its rows is `P`; the set
    of `class_status`).
- **Bridge-eligible cell.** The cell has exactly one CIK and a `P` row, and every qualifying row's `class_status`
  passes our class test: `common`, or a share-class letter other than U, W or R (`prepare_recent_research.py:29-33`).
  Count such cells on the sessions where your `_tmp/panel_member` has `member` true. That flag stands in for our base
  membership.
- **Rows.** Full-row equality of pit `links.parquet` rows with `start < 2024-01-01 AND available_at < 2024-01-01`.
  Rows with `start < 2024-01-01 AND available_at >= 2024-01-01` are reported separately: they are never read inside
  TRAIN.
- **Panel CIK (channel 4), key `(security_id, session_date)`.** The CIK from `identity/link_table.parquet`, any tier,
  as the panel admits it (`panel.py:468-477`: a backfill row by `evidence_at`, the others by `available_at`), on
  panel-member sessions 2018-2023.
- **Attribution aid, B2 only.**
  - r4 `security_company_links` with `link_start < 2024-01-01`, keyed on `TABLE_KEYS`;
  - `identity/links.parquet` (stage I).

**What we ask you to send** (counts only; one column per comparison; row 11 gives each B arm's own values):

| # | count (sessions from the bridge's first session to 2023-12-29, consumer view at d 22:00 UTC) | B1 vs A | B2 vs A | B12 vs A | B12 vs B1 |
|---|---|---|---|---|---|
| D0 | reproduction differences, A vs published: r4 rows, pit rows, link_table rows; arm A's Stage I seal-2025 validation (S4): `pass`, and `got` vs `expected` if false | (once) | | | |
| 1 | line-sessions linked in one arm only: in A only / in B only | | | | |
| 2 | line-sessions linked in both arms, to different CIKs | | | | |
| 3 | line-sessions with the same CIK and a different primary flag: P in A and J in B / J in A and P in B | | | | |
| 4 | line-sessions with the same CIK and flag, but a different `class_status` | | | | |
| 5 | distinct `sr_id` with any of rows 1-4 | | | | |
| 6 | distinct CIKs with any of rows 1-4 | | | | |
| 7 | bridge-eligible panel-member cells, per year 2020, 2021, 2022 and 2023, and one figure for 2017-2019: eligible in A only / in B only / total eligible in A | | | | |
| 8 | pit rows with `start` and `available_at` before 2024-01-01: in A only / in B only | | | | |
| 9 | pit rows with `start` before 2024-01-01 and `available_at` on or after it: in A only / in B only | | | | |
| 10 | panel-member line-sessions 2018-2023 whose panel CIK differs, per year (path (b) also moves the strict CIKs in `link_table`) | | | | |
| 11 | sanity, per B arm: `backfill_rows`; `symbol_snapshot_match` items; `name_tier`; Stage I seal-2025 validation `got` (S4). B1's equals A's (same rehearsal output); B2's and B12's differ from `expected` by construction whenever path (b) moves a link visible before 2025-01-01 | B1 | B2 | B12 | n/a |

**How to read the result.**
- **Zero.** Rows 1-4, 7 and 8 are zero for B12 vs A, D0 is clean (arm A's S4 `pass` included), and the sanity checks
  pass.
  - Then the pit bridge, as read in 2017-2023, does not depend on the 2026 ticker list, the 2026 directory or survival
    to 2026: the zero bounds paths (a)-(c).
  - It does not clear the later-evidence residual (A0.1, "Not covered"), which no arm varies. PIT-b is its fix.
  - Our flag (OD5-1) becomes a footnote naming what was held fixed (A0.1, "Not covered"), the later-evidence residual
    included.
- **Non-zero.**
  - **Row 7 is the number that reaches our universe.**
    - Cells "in A only" are in the published universe only because of 2026 inputs.
    - Cells "in B only" are missing from it because of them.
    - Divided by the year's total, they give a share.
  - **Attribution.**
    - B1 vs A measures paths (a) and (c): "J in A and P in B" in row 3 is mostly path (a), and "in B only" in row 1 is
      mostly path (c).
    - B12 vs B1 measures path (b) cleanly: both arms have the backfill tier off.
    - B2 vs A is not a clean measure of (b): it can also move the backfill candidates (see "What each comparison
      measures").
    - B2 / B12 "in A only" includes (b2) cells that PIT-b also keeps; they appear because the no-ticker arm still
      applies the later-rival rule.
    - The arms interact, so B1 plus B2 need not equal B12. **B12 vs A is the headline.**
  - Then send the identifiers (A0.4).
  - Under OD5-1, a non-zero result annotates our freeze verdict. Nothing is re-run.
- **What source predicts.**
  - B1 is non-zero wherever a backfill line was the most liquid line of its CIK (path (a)), or a backfill day met a
    name match (path (c)).
  - B2 vs A and B12 vs B1 cannot be predicted from source.

#### A0.4 If the diff is non-zero

- Send the `sr_id` / `cik` lists behind rows 1-4, and the `(sr_id, session_date)` cells behind row 7 in A only and in
  B only, as identifiers only. We map them onto our role ourselves.
- The point-in-time rules we ask for:
  - **General.** A link row uses only sources dated at or before its `available_at`. A 2026 snapshot can be evidence
    only for a row stamped at or after that snapshot's own date.
  - **PIT-a.** In the `strict` and `pit` exports, the primary flag of (cik, session d) is computed only among that
    export's own tiers, and only among the links with `available_at <= d 22:00 UTC`.
  - **PIT-b.** A reconstructed link version visible at clock c is the outcome of the acceptance, dominance and segment
    rules applied to the evidence items known at c. An item known later (the 2026 ticker snapshot, or a rival's item)
    can only produce a new version stamped at its own clock, with the earlier version closed by `valid_until`. It
    never changes a version stamped earlier.
  - **PIT-c.** In the `pit` export, a name-tier day is withheld only by a link of the export's own tiers (strict),
    never by a backfill day. The `all` export may keep today's precedence.

#### A0.5 Acceptance tests for the fix (atx-db pytest, synthetic)

- **T1, path (a).**
  - Set-up: CIK K has a strict line X and a backfill-only line Y (alive at the snapshot). Y's prior adv63 exceeds X's
    on 2021 sessions d1..d2.
  - Assertion: X is `P` on d1..d2 in every pit row stamped before 2026, and X's pit rows do not split at d1 or d2.
  - Today X is `J` there (`identity_table.py:111-112`).
- **T2, path (c).**
  - Set-up: line Z has a FINRA name match and a backfill day on the same 2021 sessions.
  - Assertion: the pit export carries Z's name row on those sessions.
  - Today it does not (`identity_names.py:148-152`; `identity_backfill.py:129-130`).
- **T3, path (b1), the rival's ticker.**
  - Set-up, CIK A: share items of weight 2.0 and a lifecycle item of 1.0, all known in 2019, with an acceptable match
    ratio (`identity_reconstruction.py:1429-1432`).
  - Set-up, class proof: A's share items use a concept of `ir.SHARE_CONCEPTS`, and the line has a dated vendor symbol
    from before A's 2019 clock that `_line_symbol_class` does not mark non-common, so `_class_proof_at` gives `common`
    (`identity_links.py:540-545`). Without it the line's role is N and Stage I drops it (`identity_links.py:330-333`).
  - Set-up, rival CIK B: share weight 1.0, its share items known only from 2022, a span overlapping A's, and a 2026
    symbol item of .5. B is a rival under `_is_rival` (`identity_reconstruction.py:1435-1443`).
  - Today: A's 3.0 < 3 x 1.5 (B's final weight), so A is rejected and has no version at any clock.
  - Assertion: Stage I's `identity/links.parquet` links the line to A, at tier `medium` or `high`, on the sessions
    from A's 2019 clock until B's evidence is known in 2022. Without B's ticker, 3.0 < 3 x 1.0 is false, and A is
    `low` only from 2022 (`identity_reconstruction.py:1466-1497`).
- **T4, path (b), differential.** Run the reconstruction and Stage I on one fixture twice, with and without the 2026
  ticker list.
  - The fixture holds T3's line; a line whose candidate passes the threshold only with its own 2026 ticker (share 2.5,
    symbol .5) and that has an earlier, sequential CIK on the same line (b3); and a line whose candidate dominates a
    rival only with its own ticker (b2). Each line carries T3's class proof. A lifecycle item cannot supply .5: it
    weighs 1.0 (`identity_reconstruction.py:356`, `:1373`, `:1386`).
  - Assertion: every Stage I row visible at a calendar date before the ticker snapshot's observation is identical in
    the two runs: presence, CIK, start, end, tier and primary.
  - Today it fails on T3's line at least.
- **Re-run of A0.3 on the fixed build.** B12 against A gives zero in rows 1-4, 7 and 8.
- **Version.** The bridge and link-table manifests carry new rule versions. We re-pin the role and the fields to the
  new manifest SHA-256 in a later sprint.

### A.1 Where

Stage `indexes`, file `atx-db/src/atx_db/alpha_panel/indexes.py`:

| what | main `dec03454` | wh `0e2fa077` |
|---|---|---|
| docstring: "`available_at` of a membership = the rank date's 22:00 UTC mark (every input is known by then)" | `:33-34` | `:33-34` |
| `recon_summary` registered in `LAKE_STAGES` with `"clock": None` | `:75` | `:77` |
| `snapshot()` selects `p.cik` and the eligibility filter on the rank-date panel: `security_type`, `is_adr`, `is_adr_likely`, `is_fpi`, `is_lp`, `is_royalty_trust`, `is_operating`, `is_index` | `:151`, `:156-159` | `:164`, `:169-172` |
| `companies_of()` groups lines into companies by `cik` | `:166-174` | `:179-187` |
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

The wh rows for `p.cik` and `companies_of` were read at `0e2fa077` (wh `snapshot()` reads the panel through
`listed_panel_sql`, `:166`; the `cik` column is the same).

Other files:

| what | main | wh |
|---|---|---|
| `alpha_panel/security_master.py` docstring: the directory snapshot (2026-09-18) is "current lines only. Not point in time" | `:9-11`, `:19` | `:9-11`, `:19` |
| directory read only for lines whose `last_session` is within 14 days of the vendor file's last date | `:144-148` | `:171-175` |
| `last_session = max(tradingDate)` over all history | `:140` | `:167` |
| `delisting_date` NULL while alive (`DEAD_BEFORE` 2026-09-01) | `:163`, `:32` | `:190`, `:32` |
| `alpha_panel/panel.py`: `security_type` falls back to `directory_type`; `security_type_basis` names it | `:226-235` | `:226-235` |
| `alpha_panel/panel.py`: `finra_type` is NULL when the FINRA row is older than the staleness limit | `:569-570` | same |
| `alpha_panel/panel.py`: `is_etf` ORs in `directory_etf` on every row | `:236` | `:236` |
| `alpha_panel/panel.py`: `is_adr`, `is_lp`, `is_common` | `:237`, `:240`, `:250-251` | same |
| `alpha_panel/panel.py`: `is_operating` (needs `NOT is_etf`; keys on `cik`) | `:259-262` | same |
| `alpha_panel/panel.py`: `cik` and `link_tier` from `identity/link_table.parquet`, every tier, backfill gated by `evidence_at` | `:468-477`, `:487-489` | same |
| `alpha_panel/panel.py`: `directory_type`, `directory_etf` joined as static line columns | `:574` | same |
| `alpha_panel/identity_table.py`: backfill `evidence_at` = `valid_from` 22:00 UTC, "the choice of CIK still comes from the snapshot" | `:22-24`, `:144-145` | same |
| `alpha_panel/export_impl.py`: your own non-PIT label "issuer link via the snapshot-run-backfill-v1 tier (2026 snapshot choice of CIK)" | `:345-346` | same |

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

**4. Eligibility and company grouping: two 2026 snapshots in the panel inputs (high; PM ruling ALD-3).**
- `snapshot()` selects members on the rank-date panel through `security_type`, `is_adr`, `is_adr_likely`, `is_lp` and
  `is_operating` (`:156-159`). It groups the selected lines into companies by the panel `cik` (`:151`;
  `companies_of`, `:166-174`).
- **Input 1: the directory snapshot.** On every panel row:
  - `security_type` falls back to the Nasdaq Trader directory type whenever the dated FINRA type is NULL (no FINRA
    row, or one older than the staleness limit: `panel.py:569-570`), `unknown` or `common_unverified`
    (`:226-235`);
  - `directory_etf` is ORed into `is_etf` (`:236`), which gates `is_operating` (`:259-262`).
  - Those `directory_*` columns come from one 2026-09-18 snapshot that covers only lines still trading in September
    2026 (`security_master.py:9-11`, `:144-148`). The module's own docstring calls them "Not point in time".
- **Input 2: the CIK from the backfill tier.**
  - The panel takes `cik` from `identity/link_table.parquet`, every tier including `snapshot-run-backfill-v1`. A
    backfill link is used from its `evidence_at` = `valid_from` 22:00 UTC (`panel.py:468-477`, `:487-489`;
    `identity_table.py:22-24`, `:144-145`).
  - The backfill tier's choice of CIK is the 2026-09-20 SEC ticker snapshot, and the tier exists only for lines alive
    at that snapshot (`identity_backfill.py:7-20`). Your export labels such fields non-PIT itself
    (`export_impl.py:345-346`).
  - The CIK groups lines into companies, and so sets company ME, the ranks and the p1000 breakpoint. It also keys
    `is_operating`: a linked line needs a periodic filing within 400 days, while an unlinked line passes
    (`panel.py:262`).
- So whether a line was eligible at a 2018-2023 rank date, and which company it counted in, can depend on whether it
  survived to 2026 and on its 2026 classification and CIK. All three are post-seal facts.
- Through company ME ranks and the p1000 breakpoint, this also moves other lines' R1000 / R2000 assignment.
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
- **R7. Eligibility and company inputs (channel 4).** Meet R7.1 and R7.2, and tell us which options you chose.
  - **R7.1, the directory.** Choose one:
    - (a) restrict index eligibility to dated evidence: `security_type_basis = 'finra_name'` (or another dated
      basis), and `is_etf` without `directory_etf`;
    - (b) land a dated directory history (daily Nasdaq Trader files, each stamped at its own date) and read it as of
      the rank date;
    - (c) give a written proof that the directory fields used are time-invariant for the lines they decide, with the
      count of (rank date, line) eligibility decisions per reconstitution, 2018-2023, that the directory decided.
  - **R7.2, the CIK.** Choose one:
    - (d) index inputs (company grouping and `is_operating`) use strict and name links only
      (`link_tier IN ('strict', 'name')`);
    - (e) report, per rank date 2018-2023, the selected lines whose company key came from a backfill link, and the
      lines excluded only because a backfill CIK failed `is_operating`.
- **R8. Seal by partition (recommended).** Write `constituents` (and the exit vintages) in parts by the year of
  `available_at`, each listed in the manifest. Today the engine reader decodes rows stamped 2024-2026 in
  `constituents.parquet` and only then drops them.
- **Scope of the TRAIN claim.** Under R1-R5, no output of `intervals()` stamped before 2024-01-01 depends on a fact
  dated 2024-01-01 or later. That claim does **not** cover the eligibility and company inputs: channel 4 remains
  until R7.1 and R7.2 are met.

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
   - *Fails today at T = 2021-05-20.* On the as-of inputs, every R2 member has `last_session` 2021-05-19, before
     `eff` 2021-06-28. So today's code drops all four R2 members in the as-of run, and drops only X in the full run:
     - X is missing from **both** runs;
     - Z, Y and V appear only in the full run;
     - R1's `effective_to` is 2021-06-01 / 2021-06-10 / 2021-06-25 (X / W / V) in the full run, against 2021-05-19 in
       the as-of run;
     - R2's `(additions, deletions)` is `(3, 0)` in the full run (X spuriously added, W not deleted) and `(4, 0)` in
       the as-of run (no prior member counts as alive).
   - A fixed build gives `(2, 1)` (Z and Y added, W deleted) in both runs.
5. **Data-extent truncation.** Run on full inputs with the data ending 2022-06-30 and again with it ending 2022-12-30.
   The rank-date vintage rows of R1-R3 must be identical. *Fails today:* the R3 rows of V and Y take
   `effective_to` = the data's last session.
6. **S&P-like proxy.** Checks 1-5 also hold for one `SP500P` quarterly pair.

The existing assertion `test_alpha_panel_indexes.py:72` pins channel 2. Rewriting it is part of the v2 meaning: a
deliberate rule change under a new rule version, not an edit of a pinned value to make a test pass.

**Channel 4 has no fixture test here,** because it lives in the panel inputs. R7 (a), (b) and (d) are checked by the
receipt keys in A.5. R7 (c) is the written proof, and R7 (e) is the report.

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
- A **new** receipt key, `backfill_cik_on_rank`, gives two counts per rank date:
  - the selected lines whose company key came from a backfill-tier link (`link_tier = 'backfill'` on the rank-date
    panel row);
  - the lines excluded only because a backfill CIK failed `is_operating`.
  - Both must be 0 under R7 (d). Under R7 (e) they are the report.
- The new atx-db test is green in your suite.

### A.6 What the engine does afterwards (our job, not yours)

- Re-pin the stage, and move `kIndexesStage` to v2.
- `russell_recon` will carry a second declared aspect (P10; `builder-designs.md` section 5.3). Through P9 the row
  declares the presence aspect only (PM ruling ALD-11). Then:
  - the ALSIG-7 presence aspect is dropped only once R1-R5 hold;
  - the ALD-3 eligibility aspect is dropped only once R7.1 and R7.2 are met.
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
- No raw data shared with the sprint. Root reads manifests and receipts only, and A0 sends counts and identifiers.
