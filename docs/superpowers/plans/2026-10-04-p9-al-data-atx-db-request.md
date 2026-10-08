# P9 AL-DATA request to the atx-db owner — implementation plan (2026-10-04)

**Spec (binding authority):** `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/request-snapshot.md`
(byte copy of `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/atx-db-request.md`, 59,756 B,
SHA-256 `8f9916d59eb7f3b6f5c7b72768c3e4246c0d189dfbd42344330b57689f1d3321`). Line numbers below refer to it.

**State surveys (read these, not the old tier1-v4 HANDOFF):** in the same workspace directory:
`survey-a0.md` (identity bridge), `survey-indexes.md` (indexes stage), `survey-b.md` (B1 text, B3 fundamentals).

**Tree:** `C:/atx`, branch `feat/tier1-v3-warehouse`. Python project `C:/atx/atx-db`, interpreter
`C:/atx/atx-db/.venv/Scripts/python.exe`. Data lake `C:/atx/atx-db/data/` (git-ignored).

## Global Constraints

- **GC1 PIT rule (the request's rule behind every item).** A row stamped `available_at = A` holds only facts that
  were public at A. The engine reads a stage row at role session t only if `available_at < date(t-1) 22:00 UTC`.
  TRAIN is `[2020-01-01, 2024-01-01)`. The mark hour is 22 UTC (`alpha_panel/common.py` `MARK_HOUR_UTC`).
- **GC2 Commits.** The tree is dirty with another session's uncommitted work (≈230 paths). Commit only the files you
  changed, with an explicit pathspec: `git add <paths> && git commit -m "<msg>" -- <paths>`. Never `git add -A`,
  `git add .`, `git commit -a`, `git stash`, `git checkout -- <file>`, `git restore`, `git reset`, or any push. Never
  commit anything under `atx-db/data/`. Commit messages end with
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. If `git` reports `index.lock`, wait 5 s and retry
  (another lane may be committing).
- **GC3 Hash-pinned files.** `atx-db/src/atx_db/alpha_panel/common.py` and
  `atx-db/src/atx_db/alpha_panel/identity_links.py` are byte-pinned by an in-flight fundamentals release generation
  (`fund_batch_release.code_paths`) and a universe release. Do NOT modify either file in any task of this plan.
- **GC4 Published artefacts are immutable.** Never overwrite, rename or delete anything that already exists under
  `atx-db/data/alpha_panel/v1/` (in particular `identity/`, `export/identity-bridge-v2-*`, `indexes/`, `panel/`,
  `text/`, `compustat_std/`) or `atx-db/data/research/identity_rehearsal/session8-phased-r4/`. The P9 sprint pins
  these by SHA-256. New outputs go to NEW directories named in the task.
- **GC5 Host.** 15.7 GB RAM, about 2-3 GB free. Every data job (anything that opens real lake data) runs under
  `python C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb <cap ≤ 1.0> --wait-minutes 90 -- <cmd>`
  (it queues behind other guarded jobs — that is expected, wait for it). DuckDB only through
  `atx_db.alpha_panel.common.connect` (file-backed spill, threads ≤ 2) or an explicit `memory_limit` ≤ 600MB. One
  heavy job at a time per task. Pytest: synthetic offline fixtures, one file at a time,
  `-q -p no:cacheprovider`, each file < 60 s.
- **GC6 Disk.** Keep `C:` free space ≥ 25 GB at all times; check before any landing > 1 GB. Delete your own scratch
  (spill dirs, scratch DuckDB files, seed copies) when the task ends and say so in the report. Never delete files
  you did not create.
- **GC7 Network.** No network access except the fetches a task names explicitly. SEC only through the committed
  `atx_db.sec_http` client / committed fetchers, ≤ 5 requests/s, existing user agent.
- **GC8 Evidence.** Every count, gate and acceptance print is reported with the exact command and the measured
  number. A FAIL that cannot be closed in the task is reported as FAIL, never hidden or re-labelled.
- **GC9 Scope.** No change to any stage other than those a task names. No row stamped 2024-01-01 or later is ever
  needed by the requester. Follow the surrounding code's style, naming and comment density; no new dependencies.
- **GC10 Older scaffolding.** `private_identity_capsule.py`, `private_index_*.py` and other untracked `private_*`
  modules are a previous session's uncommitted work. Use them only where a task says so; otherwise leave them
  untouched (do not delete, do not commit).

---

## Task 1: A0 check harness — switches S1/S2, pin-safe S3/S4 stage runner, arm orchestrator, input recovery

**Spec:** request lines 207-301 (A0.3 principle, arms, check-only switches S1-S4, procedure, sanity checks) and
303-365 (D0, provenance, the diffs, rows D0 and 1-11). Background: lines 44-205. Survey: `survey-a0.md` §1, §2, §4,
§6.

**Goal.** Everything needed to run the rebuild-against-rebuild check for arms A, B1, B2, B12 and to print the
requested rows — as code, plus the recovered inputs. The long run itself is Task 2.

**Files**
- Modify `atx-db/src/atx_db/identity_links.py` (S1 only).
- Modify `atx-db/src/atx_db/alpha_panel/identity_backfill.py` (S2 only).
- Create `atx-db/src/atx_db/alpha_panel/a0_check.py` (CLI: `recover-inputs`, `stage`, `run-arms`, `report`).
- Create `atx-db/tests/test_a0_check.py`.
- Reuse unchanged: `alpha_panel/private_identity_differential.py` (`compare_d0`),
  `alpha_panel/private_identity_arm_comparison.py` (`compare_arms`, rows 1-10 + identifier exports).
- Do not touch: the GC3 files; `private_identity_capsule.py` (superseded by this task — leave as is).

**Requirements**
1. **S1** (request lines 248-249). Environment variable `ATX_A0_S1=1` makes the `prepare --step reconstruct` step pass
   `tickers=()` to `ir.reconstruct_in_batches` (today `identity_links.py:1287-1289`). Nothing else changes. The
   switch value is recorded in the work directory's preparation pin so a work dir prepared with S1 on cannot be
   continued with S1 off (and vice versa). Unset or `0` = today's behaviour exactly.
2. **S2** (lines 250-251). `ATX_A0_S2=1` makes `identity_backfill` build an empty candidate set `cand` (same
   schema; today `identity_backfill.py:52-58`), so neither pass writes a backfill day.
   `identity/backfill_manifest.json` records the switch. Unset or `0` = today's behaviour exactly.
3. **S3 + S4, pin-safe** (lines 252-265), in `a0_check.py stage --root P --r4 R --stage <identity|backfill|names|
   combine|identity_table>`. One process per stage. It sets `ATX_ALPHA_PANEL_ROOT=P`, sets
   `atx_db.alpha_panel.common.IDENTITY_R4_DIR = R` in memory BEFORE the stage module is imported (Stage I computes
   `R4_EXPORT` at import), then runs the stage's own entry point. For stage `identity` it must record Stage I's
   seal-2025 validation as manifest key `validation` (`seal`, `expected`, `got`, `pass`) in
   `P/identity/manifest.json` and continue instead of raising `SystemExit` on a mismatch. Reuse the stage module's
   own functions — no copy of its SQL. Every arm runs this same runner code.
4. **`run-arms`** implements request procedure steps 1-3 (lines 271-301) exactly:
   - rehearsal for arms A and B2 only: `prepare --step vendor`, `facts`, `reconstruct` (S1 on for B2), then
     `rehearsal --phase` `symbols`, `candidates`, `uncertainty`, `links`, `names`, `export`, one process each, with
     the published OPS manifest and `--document-review` file that the published `build.json` pins; the `export`
     phase must be attempt `001` (if it fails, the arm's six phases re-run into a new `R_<arm>`);
   - alpha stages `identity, backfill, names, combine, identity_table` for A and B1 (on `R_A`) and B2 and B12 (on
     `R_B2`), S2 on for B1 and B12, through the `stage` runner;
   - each arm root `P_<arm>` is seeded with `prices/`, `calendar.parquet`, `short_interest/si.parquet`,
     `_tmp/panel_core/`, `_tmp/panel_member/` from the published root as plain COPIES (≈ 2.55 GB per arm, deleted
     when the run ends). No hard links and no attribute change on any published file: an NTFS read-only flag is
     per file and would also mark the published file (GC4). FINRA raw dir and FSDS/companyfacts staging stay as
     they are;
   - strictly serial, every step under the GC5 guard, resumable: a per-step record (command, exit code, wall
     seconds, peak bytes from the guard receipt) so a re-invocation skips completed steps;
   - all directories are new, under `atx-db/data/research/identity_rehearsal/a0-check-20261004/`
     (`W_A`, `R_A`, `W_B2`, `R_B2`, `P_A`, `P_B1`, `P_B2`, `P_B12`, `code/`, `report/`).
5. **Frozen code.** `run-arms --commit <sha>` runs every step from a `git archive` export of that commit placed in
   `.../a0-check-20261004/code/<sha7>/` (at least `atx-db/src`, plus any non-`src` files the modules read at run
   time), with `PYTHONPATH` at the export's `atx-db/src`. Do NOT create a junction or symlink into the data lake
   (a recursive delete of the export could then reach the lake): instead the runner sets the
   `PACKAGE_ROOT`-derived path constants of `alpha_panel.common` (`DEFAULT_ROOT`, `COMPANYFACTS_DIR`, `FSDS_DIR`,
   `IDENTITY_R4_DIR`) in memory to the real `C:/atx/atx-db/data` locations before any stage module is imported,
   and passes explicit absolute paths to `prepare` / `rehearsal`. Every step first asserts that `atx_db.__file__` lies
   inside the export. `arms.json` records the commit and the SHA-256 of every identity module used. Purpose: later
   tasks edit the identity modules in the main tree while Task 2's arms are still running.
6. **`report`** writes `report/a0-results.json` and `report/a0-results.md`:
   - **D0** (lines 303-325): `compare_d0` of `R_A/phases/export-001/security_company_links.part-0000.parquet` vs the
     published file (all columns except `created_at`, key `TABLE_KEYS`), `P_A/export/identity-bridge-v2-pit/
     links.parquet` and `P_A/identity/link_table.parquet` vs the published files as full rows; arm A's Stage I
     validation (`pass`, and `got` beside `expected` when false);
   - **provenance prints** (lines 313-325): the listed `code` / `code_version` / `sources` / `code_sha256` /
     `code_files` fields of the published manifests, each code hash beside the SHA-256 (raw and LF-normalised) of
     the same file at `dec03454` (`git show dec03454:<path>`); state plainly whether a producing commit can be named;
   - **rows 1-10** via `compare_arms` for `B1 vs A`, `B2 vs A`, `B12 vs A`, `B12 vs B1`;
   - **row 11** per B arm: `backfill_rows`, count of `symbol_snapshot_match` items in `W_<arm>/evidence/*.parquet`
     (B12 uses `W_B2`), `name_tier`, Stage I validation `got`;
   - **sanity checks** of procedure step 3 (lines 294-301) as PASS/FAIL lines;
   - **B2 attribution aid** (lines 345-347): r4 `security_company_links` with `link_start < 2024-01-01` keyed on
     `TABLE_KEYS`, and Stage I `identity/links.parquet`, each as in-A-only / in-B2-only counts;
   - **identifier exports** for request A0.4 (lines 397-398): `sr_id` / `cik` lists behind rows 1-4 and the
     `(sr_id, session_date)` cells behind row 7 in-A-only and in-B-only, as parquet under `report/identifiers/`;
   - the markdown table uses the request's row numbering and column order (lines 351-364).
7. **`recover-inputs`** (survey `survey-a0.md` §4.1 X2-X4):
   - write the recovered receipt `a0-check-20261004/ri2_a.recovered.json` whose `inputs` equal the published
     `build.json` `pin.inputs` verbatim (plus a `recovery` note);
   - create an NTFS hard link at the pinned TickerHistory3 path pointing at
     `C:/Users/natha/Downloads/TickerHistory3.parquet` and verify the pinned SHA-256;
   - re-fetch the 367 document-review bodies with the committed fetcher into the pinned cache layout
     (`data/cache/identity-oracle-session8`), ≤ 5 req/s, and verify every `document_sha256`; the six bodies recorded
     as 0 bytes are recreated as the fetcher would store them. **This fetch is the only network access of this
     task.** On any hash mismatch: stop, list the mismatching URLs in your report, do not substitute.
8. **Tests** (`tests/test_a0_check.py`, synthetic, offline, < 60 s): S1 off passes the snapshot tickers and S1 on
   passes `()` (capture the call); S2 on gives zero backfill rows and records the switch, S2 off is unchanged, on a
   tiny fixture; the `stage` runner redirects the r4 directory and records a failing validation without exiting;
   the report assembles row 11 and the sanity lines from tiny fixture manifests; the frozen-export assertion
   rejects an `atx_db` imported from outside the export.
9. **Run for real in this task:** `recover-inputs` (under the guard), and `run-arms --dry-run` printing the full
   step plan. Do NOT start the arms.

**Report:** include the recover-inputs outcome (367/367 hash matches or the mismatch list), the dry-run plan, test
output, and the exact Task 2 command lines.

---

## Task 2: Run the A0 check — arms A, B1, B2, B12, D0, the four comparisons

**Spec:** request lines 207-393. Depends on Task 1 (reviewed). Survey `survey-a0.md` §4.2-4.3 for expected cost
(≈ 1.5-2.5 h serial wall, peaks ≤ 0.6 GiB, ≈ 5-8 GB disk with hard-linked seeds).

**Goal.** The measured answer to A0.3: the D0 row and rows 1-11 for the four comparisons, with identifiers.

**Steps**
1. `a0_check run-arms --commit <Task 1 head>` to completion (it is resumable; if a step fails, diagnose, and if the
   harness is at fault fix it in `a0_check.py` with a test, commit, and re-run from a new frozen export — every arm
   must have run on ONE commit, so re-run all arms if the fix changes arm-visible behaviour).
2. `a0_check report`.
3. Verify the sanity checks (request lines 294-301). A vacuous zero is a FAIL.
4. Write `atx-db/docs/p9-al-data/a0-results.md` (committed): the filled request table (rows D0, 1-11; one column
   per comparison), arm commit SHA, per-step wall time and peak memory, the provenance prints, the reading of the
   result per request lines 366-393 (zero / non-zero, row 7 shares per year, attribution), and the path of the
   identifier files (which stay under `atx-db/data/...`, not committed).
5. Disk: remove seed copies (if any were copied) and scratch DuckDB files of the arms; keep `R_*`, `W_*/evidence`,
   `P_*/identity`, `P_*/export`, `report/`.

**Constraints.** No edit to identity modules in this task except harness fixes in `a0_check.py`. D0 non-zero is
reported and the run continues (request lines 311-312).

---

## Task 3: PIT-a and PIT-c — pit/strict bridge exports computed from their own tiers (tests T1, T2)

**Spec:** request lines 73-101 (path (a)), 154-163 (path (c)), 395-409 (PIT-a, PIT-c), 413-421 (T1, T2), 444-445
(version). Design sketch: `survey-a0.md` §5 "PIT-a" and "PIT-c".

**Files**
- Modify `atx-db/src/atx_db/alpha_panel/identity_table.py`, `identity_backfill.py`, `identity_names.py`;
  `atx-db/src/atx_db/stagelake/registry.py` only if a stage's declared outputs change.
- Create `atx-db/tests/test_alpha_panel_identity_pit.py`.
- Update `atx-db/docs/` text that describes the bridge rule, if such a doc exists for these stages.

**Requirements**
1. **PIT-a.** In the `strict` and `pit` exports, the primary flag of (cik, session d) is computed only among that
   export's own tiers, and only among the links with `available_at <= d 22:00 UTC`. Islands (`start`, `end_incl`,
   `available_at`) of those exports are built on that export's own flag. A backfill line can never turn a strict or
   name line into `J`, and never splits its islands.
2. **PIT-c.** In the `pit` export a name-tier day is withheld only by a link of the export's own tiers (strict),
   never by a backfill day.
3. **Unchanged on purpose:** `identity/link_table.parquet`, `identity/links_combined.parquet` and the `all` export
   keep today's precedence and today's all-tier flag (the panel reads them; request lines 402-403, 408-409).
4. **Versions.** The fixed exports are written under NEW export directory names and carry new rule versions in
   their manifests and in the link-table manifest (bump every rule string whose meaning changed; follow the
   existing naming pattern, e.g. `identity-bridge-v3-pit`). A `v2` export directory is never written by the new
   code.
5. The Task 1 switches S2/S3/S4 keep working with the new code (the fixed build is re-checked with them in Task 5).
6. **T1** (lines 413-417) and **T2** (lines 418-421) exactly as specified, on synthetic build roots (monkeypatched
   `ATX_ALPHA_PANEL_ROOT`, FINRA dirs through `C`), plus one test that `link_table` / the `all` export keep the
   all-tier precedence on the T1 fixture. TDD: show T1 and T2 failing on today's code first.

**Out of scope:** `identity_reconstruction.py`, top-level `identity_links.py` (Task 4); any real-data rebuild
(Task 5).

---

## Task 4: PIT-b — clock-honest link versions in the reconstruction (tests T3, T4)

**Spec:** request lines 103-152 (path (b): b1, b2, b3), 185-188 (later-evidence residual), 404-407 (PIT-b), 422-442
(T3, T4). Design sketch: `survey-a0.md` §5 "PIT-b" (clocked replay per line).

**Files**
- Modify `atx-db/src/atx_db/identity_reconstruction.py` and `atx-db/src/atx_db/identity_links.py` (candidate /
  evidence serialisation and `_company_versions`).
- Create `atx-db/tests/test_identity_reconstruction_pit.py`.
- Do NOT modify `alpha_panel/identity_links.py` (GC3). Stage I must consume the new rehearsal output through its
  existing interface (versions with `available_at` / `valid_until`).

**Requirements**
1. **PIT-b.** A reconstructed link version visible at clock c is the outcome of the acceptance, dominance and
   segment rules applied to the evidence items known at c. An item known later (the 2026 ticker snapshot, or a
   rival's item) can only produce a NEW version stamped at its own clock, with the earlier version closed by
   `valid_until`. It never changes or removes a version stamped earlier.
2. The method identifier changes (new `METHOD` / rule version strings) so old preparation and rehearsal
   directories cannot be mixed with the new code.
3. S1 (`ATX_A0_S1`) keeps working.
4. **Cost bound.** The `prepare --step reconstruct` step on the real inputs must stay ≤ 60 min wall and ≤ 1.0 GiB.
   Measure it once on the real inputs in a scratch work dir under
   `atx-db/data/research/identity_rehearsal/a0-pitb-scratch/` (under the GC5 guard; needs Task 1's recovered
   inputs) and report wall time and peak memory. Delete the scratch afterwards.
5. **T3** (lines 422-433) and **T4** (lines 434-442) exactly as specified, including the class-proof set-up; T4's
   fixture holds the three lines named there. TDD: show T3 and T4 failing on today's code first.
6. Existing reconstruction tests stay green; where a test pins hindsight behaviour that PIT-b deliberately removes,
   rewrite it and say so in the report (a rule change under a new method version, not a pinned value edited to
   pass).

**Escalate (BLOCKED) rather than guess** if the existing Stage I interface cannot express a withdrawn version
without editing the GC3 file — describe the minimal interface change needed.

---

## Task 5: Fixed build — re-run A0.3 on the fixed code and publish the v3 bridge

**Spec:** request lines 443-445 ("Re-run of A0.3 on the fixed build. B12 against A gives zero in rows 1-4, 7 and
8." / new rule versions). Depends on Tasks 2, 3, 4.

**Steps**
1. `a0_check run-arms --commit <head with Tasks 3+4>` into a NEW directory
   `atx-db/data/research/identity_rehearsal/a0-fixed-20261004/` (arms A, B1, B2, B12 on the fixed code; extend
   `a0_check.py` minimally if the fixed code's new export names / new files need it, with tests).
2. `a0_check report` for the fixed arms: rows 1-11 for the four comparisons. **Acceptance: rows 1-4, 7 and 8 are
   zero for B12 vs A, and the sanity checks pass.** If not zero: investigate, fix the responsible module with a
   regression test (this is the point of the task), re-run. Report any residual honestly with identifiers.
3. Also report fixed arm A vs the published v2 bridge (rows 1-10 with "A" = published, "B" = fixed A): this is the
   size of the fix on the consumer view.
4. **Publish** the fixed arm A outputs side by side with v2 (GC4): the new export directories
   (`export/identity-bridge-v3-*`, with manifests) are copied into `atx-db/data/alpha_panel/v1/export/`, and the
   fixed identity stage (links, name days, combined, pit links, link table, manifests) plus its rehearsal into
   `atx-db/data/alpha_panel/v1/identity_v3/` and
   `atx-db/data/research/identity_rehearsal/session9-pit-r5/`. Verify every published file's SHA-256 against its
   manifest after the copy. Print the SHA-256 of the v3 pit export `manifest.json` (the requester re-pins to it).
5. Record the fixed rehearsal's Stage I seal-2025 counts (`got`) as the new expectation in
   `atx-db/docs/p9-al-data/a0-fixed-results.md`, together with: the filled table, the v3 manifest SHA-256, the rule
   versions, and an **activation note** — the exact two-file patch (`alpha_panel/common.py` `IDENTITY_R4_DIR`,
   `alpha_panel/identity_links.py` `VALIDATION_EXPECTED`) that would make the production chain use r5, saved as
   `atx-db/docs/p9-al-data/activate-identity-r5.patch` but NOT applied (GC3).
6. Disk: remove arm seeds and scratch; keep reports and identifiers.

---

## Task 8: B3 — year-partitioned, pinned `compustat_std` funda export, acceptance prints, CPI-U NSA

**Spec:** request lines 696-714 (rules C1-C6) and 766-803 (B3). Survey: `survey-b.md` (C5 contract section, B3
section, CPI section).

**Files**
- Create `atx-db/src/atx_db/alpha_panel/compustat_std_parts.py` (export + acceptance printers; CLI `build`,
  `accept`), `atx-db/src/atx_db/alpha_panel/cpi_u_nsa.py` (CPI stage), tests
  `atx-db/tests/test_compustat_std_parts.py`, `atx-db/tests/test_cpi_u_nsa.py`, docs
  `atx-db/docs/datasets/compustat_std_parts.md`, `atx-db/docs/datasets/cpi_u_nsa.md`, registry stubs
  `atx-db/docs/datasets/<stage>.registry.json` following the existing stub pattern.
- Read-only input: the published `atx-db/data/alpha_panel/v1/compustat_std/` funda file (GC4). Do not modify
  `compustat_std.py`.

**Requirements**
1. **Export.** New stage directory `atx-db/data/alpha_panel/v1/compustat_std_parts/` with
   `parts/year=YYYY/funda.parquet`, YYYY = the UTC year of `available_at`, holding exactly these columns (request
   lines 783-787): `cik, datadate, fyear, fyr, accession, available_at, vintage_kind, first_reported, xsga, xrd,
   gdwl, seq, ceq, pstk, txditc, at, lt, fin_template, curcd, fx_converted`. Every vintage row of the published
   funda is exported (restatements are separate rows; nothing overwritten; leavers stay — C2, C6). A row with NULL
   `available_at` is excluded with a reason and counted in the manifest (C1). All years present in the source are
   written; the manifest lists every part.
2. **Manifest (C5).** `manifest.json` follows the engine `PinnedStage` contract exactly as `survey-b.md` states
   it: `schema`, `status: "complete"`, `rule` as ONE versioned string (new family, e.g. `compustat-std-funda-v1`),
   `clock_rule` (string), `staleness_days` (JSON integer), `files` = `{rel: {bytes, sha256}}` for every part,
   written last and atomically. It also records the upstream stage manifest SHA-256 and code identity, and the
   per-year row counts. Use `common.write_json_atomic` / `common.sha256_file`; do not edit `common.py`.
3. **Acceptance printers** (`accept`), each printing the command and the number:
   (i) manifest contract self-check (every listed file re-verified by bytes and SHA-256; all rows of each part
   fall inside its year — this is also (iv));
   (ii) `xsga` coverage of first-reported rows, fiscal years 2009-2022, on non-financial templates, per fiscal year;
   (iii) the SG&A convention report: share of rows with both `xsga` and `xrd` present; and, for a deterministic
   sample of 50 R&D-reporting issuers, whether the filed SG&A element includes R&D (from local FSDS `pre`/`num`,
   offline) — a report, not a gate;
   (iv) no part holds a row stamped outside its year.
   Also print, per fiscal year 2009-2023, the count of issuers with a first-reported row (the history the request
   asks for) — the thin FY2009-2010 XBRL phase-in is reported as measured, not hidden.
4. **CPI-U NSA** (request line 791). New stage `atx-db/data/alpha_panel/v1/cpi_u_nsa/`: monthly `CUUR0000SA0`
   values, one row per (reference month, vintage), `available_at` = the release instant (08:30 America/New_York on
   the release date, converted to UTC). **Network allowed for this requirement only:** at most 30 requests, ≤ 1
   req/s, to `api.bls.gov`, `www.bls.gov`, `fred.stlouisfed.org` or `alfred.stlouisfed.org`, to obtain the values
   and an authoritative dated source for the release dates (BLS release schedule archive or ALFRED vintage dates).
   Raw responses are landed under `atx-db/data/raw/cpi_u_nsa/` with a `receipts.jsonl`. If no authoritative
   release-date source can be obtained, stamp each month with a documented conservative floor that is provably not
   earlier than the true release, set a `clock_basis` column to say so, and report requirement 4 as PARTIAL. The
   manifest follows C5 with year-of-`available_at` parts.
5. **Tests** (synthetic, offline): partitioning by year of `available_at` including a row at
   `2023-12-31T23:59:59Z` vs `2024-01-01T00:00:00Z`; NULL clock excluded and counted; vintages preserved; manifest
   matches the C5 shape and its hashes verify; a tampered part fails the self-check; CPI release-instant
   conversion across a DST boundary; CPI vintage rows never overwritten.
6. **Run for real** (under the GC5 guard): `build`, then `accept`; put the printed acceptance output in
   `atx-db/docs/p9-al-data/b3-acceptance.md` (committed) with the manifest SHA-256 of both new stages.

---

## Task 9: B1 — `text_native` v2: 10-Q originals, full-document cosine, 2012-2023 cohort, year parts (code + pilot)

**Spec:** request lines 696-714 (C1-C6) and 716-764 (B1). Survey: `survey-b.md` (C5 contract, B1 as-is, sizing,
gap list). The existing `text_native` code is fixture-only and has never run on real filings.

**Files** (follow what exists; the survey lists the current modules)
- Modify the `text_native` stage module(s) and their tests/docs, or add `text_native_v2` modules beside them if
  that keeps the published v1 behaviour byte-stable; new docs section in `atx-db/docs/datasets/text_native.md`.

**Requirements**
1. **Cohort (C4).** Every original 10-K-family (10-K, 10-KT, 10-K405, 10-KSB where they occur in the range) and
   10-Q / 10-QT filing accepted 2012-01-01 to 2023-12-31, taken from the SEC quarterly master indexes for
   2012Q1-2023Q4 (48 files, landed under `atx-db/data/raw/sec_master_index/` with receipts). Amendments may be
   landed but are never the current or the prior filing. No selection on any later fact.
2. **Columns** (key `(cik, accession)`; request lines 738-743): `cik, accession, form, fiscal_period, report_date,
   filing_date, available_at, prior_accession, prior_report_date, prior_available_at, prior_gap_days,
   doc_sim_cosine`, the existing section columns (10-K sections as today; 10-Q: Part I Item 2 MD&A and Part II
   Item 1A risk factors), and `reason`.
3. **Clock (C1).** `available_at` = the authenticated acceptance instant of the filing in UTC; NULL when unproven
   (row kept, excluded from use, counted with a reason).
4. **Comparator** (lines 745-746). The original filing of the same issuer, same form family and same fiscal period
   one fiscal year earlier, accepted strictly earlier, within the existing 180-550-day gap.
   `prior_available_at < available_at` on every row with a comparator.
5. **`doc_sim_cosine`.** Term-frequency cosine of the full primary document against that of the comparator, with
   the existing tokenizer.
6. **Streaming, disk-bounded.** Issuer-ordered worker: fetch → parse → term-frequency profile → similarity against
   the prior profile → discard the body. Persistent disk for the whole 2012-2023 cohort must stay < 16 GB;
   resumable per issuer with receipts; SEC via the committed client at ≤ 5 req/s. Bodies are NOT retained; a
   per-filing receipt (URL, bytes, SHA-256, fetch time) is.
7. **Parts and manifest (C3, C5).** Output parts by the year of `available_at`; `manifest.json` per the
   `PinnedStage` contract (`status: "complete"`, one `rule` string of a new version, `clock_rule`,
   `staleness_days`, `files` with bytes and SHA-256), written under a NEW stage directory
   `atx-db/data/alpha_panel/v1/text_native_v2/` (GC4).
8. **Acceptance printers** (lines 757-764): (i) manifest self-check and the year of each part matches its name;
   (ii) per acceptance year 2013-2023, landed originals over originals listed in the master indexes; (iii) per
   acceptance year, share of landed originals with a landed same-period prior-year original and a finite
   `doc_sim_cosine`, denominator excluding the first filing of a period by an issuer and excluding 20-F/40-F
   (gate ≥ 90%); (iv) every row has `prior_available_at < available_at` and both filings are originals.
9. **Tests** (synthetic, offline): 10-Q section extraction (both sections, and a filing lacking Item 1A); fiscal
   period matching incl. a fiscal-year change and a 10-KT; comparator gap bounds 179/180/550/551 days; amendments
   never chosen; cosine on known vectors (identical = 1, disjoint = 0); NULL clock handling; year-part routing at
   the year boundary; resume after an interrupted issuer does not duplicate or drop rows.
10. **Network allowed in this task:** the 48 master index files, and a **pilot** landing of at most 2,000 filings
    (a deterministic sample of whole issuers, both form families, spanning 2012-2023; ≤ 6 GB transferred). Run
    the pilot under the GC5 guard, then run the four acceptance printers on the pilot and put their output in
    `atx-db/docs/p9-al-data/b1-pilot.md` (committed) together with measured throughput (filings/s, bytes/filing,
    requests/filing) and the extrapolated cost of the full cohort.
11. **Do NOT start the full-cohort landing.** Leave the exact resumable command in the report (Task 10).

---

## Task 10: B1 — full-cohort landing (owner-gated)

The full 2012-2023 cohort is ≈ 330,000 filings, ≈ 650 GB transferred and ≥ 19-37 h at the house rate limit
(`survey-b.md`). It starts only after the owner confirms the transfer volume. When confirmed: run the resumable
command from Task 9 under the guard to completion, run acceptance (i)-(iv) on the full output, and write
`atx-db/docs/p9-al-data/b1-acceptance.md`.

---

## Task 11: Response to the P9 lane

**Goal.** One committed document, `atx-db/docs/p9-al-data/response.md`, answering the request item by item, written
for the P9 sprint (lane AL-DATA) — they read manifests and receipts only.

Contents: (1) A0: the filled D0 + rows 1-11 table for the published code and for the fixed build, the identifier
file paths, the provenance answer (which commit built `identity-bridge-v2-pit`, or the plain statement that none
can be named and why), the PIT-a/b/c rules as implemented with rule versions, T1-T4 status, the v3 pit manifest
SHA-256 to re-pin; (2) A.1-A.5: which R7.1 / R7.2 options were chosen, the v2 rule versions, the new receipt keys
and their values, the status of the new test, the v2 stage manifest SHA-256; (3) B1 and B3: what was delivered,
the acceptance prints, what is owner-gated; (4) what was held fixed / not covered. Every number is copied from a
committed results file or a manifest, with its path. No number is restated from memory.

---

## Task 6: Indexes v2 — panel-based production stage on the point-in-time kernel (R1-R8, A.4 test, A.5 receipt keys)

**Spec:** request lines 447-690 (A.1 where, A.2 four channels, A.3 rules R1-R8, A.4 the test, A.5 acceptance keys).
Survey: `survey-indexes.md` (whole file: status table, architecture, Path B, remaining-work list).

**Situation.** An earlier session rewrote `indexes.py` (uncommitted) into a facade over three untracked modules.
The pure kernel `private_index_pit.py` (R1-R5 + receipt) is sound and tested. The rest
(`private_index_sources.py`, `private_index_release.py`) targets native inputs that do not exist, so the stage
cannot run, the v1 build was deleted from the working tree, and the new `LAKE_STAGES` entry makes
`stagelake.registry.load()` raise `TypeError` for every stage. This task makes the stage real again by **Path B**:
a panel-based snapshot (as v1 read, see `git show HEAD:atx-db/src/atx_db/alpha_panel/indexes.py`) with the R7
filters, feeding the kernel, with a plain year-partitioned writer.

**Files**
- Modify `atx-db/src/atx_db/alpha_panel/indexes.py`, `atx-db/tests/test_alpha_panel_indexes.py`,
  `atx-db/docs/datasets/indexes.md`, `atx-db/docs/datasets/indexes.registry.json`.
- Commit (and amend where a requirement needs it) `atx-db/src/atx_db/alpha_panel/private_index_pit.py` and
  `atx-db/tests/test_private_index_pit.py`.
- Create `atx-db/src/atx_db/alpha_panel/exchange_calendar.py` + `atx-db/tests/test_exchange_calendar.py`.
- Leave untouched and uncommitted: `private_index_sources.py`, `private_index_release.py` and their tests
  (`indexes.py` must no longer import them; the functions of `indexes.py` they import — the pure selection rules —
  keep their names and signatures so those tests still import). Do not edit `panel.py`, `security_master.py`,
  `universe.py`, `release_inputs.py`, `stagelake/catalog.py` (GC9; downstream readers keep reading published v1).

**Requirements**
1. **Stage identity.** New stage `indexes_v2`, written to `atx-db/data/alpha_panel/v1/indexes_v2/` (flat: no
   generations, no pointer file). The published v1 `indexes/` is never written (GC4). `LAKE_STAGES` declares
   `indexes_v2` with keys the registry accepts, `recon_summary` registered with clock `available_at` (R4), and
   keeps a declaration of the legacy v1 `indexes` stage so existing readers and `universe.registry.json` still
   resolve. `stagelake.registry.load()` must succeed (add a test that calls it).
2. **R1-R5** through the kernel (`private_index_pit.derive` / `iter_membership`): complete rank-date vintages, no
   column depending on `last_session` / `delisting_date` / any later session; scheduled `effective_to` from the
   schedule constants (`RUSSELL`, `sp_schedule`) and `effective_from` = first session after `recon_date` on the
   exchange calendar, both independent of the vendor data extent, including for the last reconstitution built;
   exit vintages (trigger `delisting_date` not NULL or `last_session < cal[-1]`, never `last_session` alone; stamp
   = 22:00 UTC of the first session after the last session); `recon_summary` rows stamped at the rank mark count
   only rank-date member sets and exits stamped before it; daily membership row of session s stamped
   max(rank mark, 22:00 UTC of the session before s), built only from vintages stamped at or before its own stamp.
   Line states come from `security_master/lines.parquet` (`last_session`, `delisting_date`), `data_end = cal[-1]`.
   A reconstitution is built whenever its rank date has panel data.
3. **Exchange calendar.** `exchange_calendar.py`: NYSE sessions generated from rules (the installed `holidays`
   package's NYSE calendar, plus an explicit, commented list of any special closures it lacks). Test: it equals
   the vendor `calendar.parquet` sessions exactly on their overlap (fixture = the session list checked into the
   test as data or computed from a committed small file; the real-data equality is asserted again at build time
   and a mismatch aborts the build).
4. **R6.** Rules `russell-proxy-v2` and `sp500-proxy-v2`; the manifest key is `rule` and lists exactly these two.
5. **R7.1 (a) — dated evidence only.** Index eligibility uses the panel's security type only where
   `security_type_basis = 'finra_name'`, and an `is_etf` that does not OR in `directory_etf`; `is_operating` is
   recomputed inside the stage from those (same formula as `panel.py`, minus the directory inputs). A line whose
   type is known only from the directory snapshot is not eligible. No `directory_*` column influences selection.
6. **R7.2 (d) — strict and name links only.** The company key and `is_operating` use a CIK only from
   `link_tier IN ('strict','name')`. A line whose panel link on the rank date is `backfill` is NOT excluded: its
   backfill CIK and every issuer-derived field that came through it (`filer_regime` → `is_fpi` / `is_adr_likely`;
   `sec_last_periodic_date`; `sic` → `is_spac` / `is_royalty_trust`; `ni_*`) are ignored, and the line is then
   (i) name-linked if the optional input `--pit-links <parquet>` (link days computed without backfill
   withholding; schema of `identity/links_combined.parquet`; produced in Task 7) holds a strict/name link for
   that (line, rank date) — issuer facts are then taken from the rank-date panel row when its CIK equals the name
   CIK, else from another strict/name-linked panel row of that CIK on that date, else the line is treated as
   unlinked and counted; or (ii) unlinked (its own company; `is_operating` by the unlinked rule). Without
   `--pit-links`, every backfill line is unlinked and the manifest says `pit_links: null`.
7. **R8.** `constituents`, `exits`, `membership` and `recon_summary` are written in parts by the year of
   `available_at` (`<product>/available_year=YYYY/<product>.parquet`, normal row-group sizes via
   `common.copy_to_parquet`), each listed in the manifest `files` with bytes and SHA-256. Manifest: `schema`,
   `status: "complete"`, `rule`, `clock_rule`, `staleness_days`, `files`, `code`, input manifest hashes,
   `gate_status`, written last and atomically.
8. **A.5 receipt keys**, all MEASURED: `counts_on_recon`, `rank_vintage_rows_on_recon` (per
   `"<index_id> <recon_date>"`, must equal `counts_on_recon`, build aborts otherwise), `exit_rows` per index,
   `directory_decided_on_rank` (per rank date: selected lines whose eligibility used a directory-derived input —
   must be 0, measured on the selected set), `backfill_cik_on_rank` (per rank date: selected lines whose company
   key came from a backfill-tier link; lines excluded only because a backfill CIK failed `is_operating` — both
   must be 0, measured), plus diagnostics per rank date: v1-eligible lines dropped by (a); backfill lines
   re-linked by name / left unlinked.
9. **A.4 test** `test_intervals_are_point_in_time_at_their_stamp` in `tests/test_alpha_panel_indexes.py`, exactly
   as request lines 603-656: the calendar, the three R2000P reconstitutions and members, the production schedule
   constant giving 2023-06-23, and assertions 1-6 each made explicit (presence with stamp and `effective_from`;
   every scheduled end and "none equals a `last_session`"; all three exit stamps and no exit for V and Y; the
   as-of replay at the five cuts over all four products with `(2, 1)`; data-extent truncation; the SP500P
   quarterly pair). Add: the `delisting_date` trigger; the R7 snapshot filters and the four measured counts on a
   small synthetic panel parquet (a directory-typed line, a directory-ETF line, a backfill line re-linked by name,
   a backfill line left unlinked, a backfill CIK that would have failed `is_operating`).
10. CLI: `python -m atx_db.alpha_panel.indexes build [--pit-links <parquet>]` and a `measure` subcommand that
    prints, per rank date 2018-2023, how many v1-selected lines had a directory-decided type or a backfill link
    (run on the published v1 `indexes/` + panel; this predicts the R2000 gate). Do not run `build` on real data in
    this task (Task 7); `measure` may be run under the GC5 guard and its output put in the report.

---

## Task 7: Indexes v2 — real build, gates, acceptance

**Spec:** request lines 661-680 (A.5). Depends on Task 6 (reviewed), Task 1 (`a0_check stage` runner) and Task 3
(PIT-c link days).

**Steps**
1. **Link days without backfill withholding.** With the Task 3 code, run the alpha identity stages `identity`,
   `backfill`, `names`, `combine` through `a0_check stage` against the PUBLISHED rehearsal
   (`data/research/identity_rehearsal/session8-phased-r4`) into a new root
   `atx-db/data/research/identity_rehearsal/ix-pit-links-20261004/` (seeded like an arm root). Its pit link-day
   file is the `--pit-links` input.
2. `indexes measure`, then `indexes build --pit-links <file>` under the GC5 guard.
3. **Gates, each reported with its number:** the build's own assertions (`rank_vintage_rows_on_recon` =
   `counts_on_recon`; `directory_decided_on_rank` = 0; `backfill_cik_on_rank` = 0/0; calendar equality); the
   R2000P company-count gate per reconstitution (the existing `russell_gate` band) — if (a)+(d) push a
   reconstitution outside the band, report it as FAIL with the per-rank measure table and set `gate_status`
   accordingly (the stage still publishes; do not loosen the band); every part's rows fall inside its year; manifest
   file hashes verify; row counts vs the published v1 per reconstitution (a diff table, not a gate).
4. **As-of spot check on real data:** for two reconstitutions (one Russell, one SP), re-run the stage with the
   line states cut as of a date between rank date and `effective_from` (as in A.4 assertion 4) and show the rows
   stamped before the cut are identical.
5. Write `atx-db/docs/p9-al-data/indexes-v2-acceptance.md` (committed): options chosen (R7.1 (a), R7.2 (d) with
   name re-link), the manifest SHA-256, `rule`, every A.5 receipt key with its values, gate results, wall time and
   peak memory; refresh the measured tables in `atx-db/docs/datasets/indexes.md`.
6. Disk: delete the scratch root's seeds and scratch DuckDB files; keep its link-day file and manifests.

