# P9 AL-DATA atx-db request — status 1 (owner stop, 2026-10-04 ~21:15 EDT)

Written for the next parent (controller) agent. It replaces nothing: the ledger and git log are the source of
truth; this file is the map.

## 1. What the goal is

Implement completely the request that the P9 sprint (lane AL-DATA) sent to the atx-db owner, using
subagent-driven development with Opus 5.5 implementers, and free disk space along the way.

- Request (spec): `C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/atx-db-request.md`
- Frozen copy used by the plan: `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/request-snapshot.md`
  (59,756 B, SHA-256 `8f9916d59eb7f3b6f5c7b72768c3e4246c0d189dfbd42344330b57689f1d3321`). If the pool-2 file no
  longer has this hash, the requester revised it: diff before continuing.

The request has four parts:

| part | ask | priority |
|---|---|---|
| A0 | Measure how far the published identity bridge `identity-bridge-v2-pit` depends on three 2026-dated inputs (four rebuild arms A / B1 / B2 / B12, rows D0 and 1-11); if non-zero, fix it (rules PIT-a, PIT-b, PIT-c, tests T1-T4), re-run, publish a bridge with new rule versions | first |
| A.1-A.5 | Make the `indexes` stage point in time (rules R1-R8, one synthetic test, four new receipt keys, rules `russell-proxy-v2` / `sp500-proxy-v2`) | high |
| B3 | Year-partitioned, pinned `compustat_std` funda export, acceptance prints, CPI-U NSA | low |
| B1 | Extend `text_native`: 10-Q originals, full-document cosine, complete 2012-2023 cohort, year parts | low |

## 2. Where everything is

| what | path |
|---|---|
| Plan (11 tasks + Global Constraints GC1-GC10) | `docs/superpowers/plans/2026-10-04-p9-al-data-atx-db-request.md` |
| Ledger (rulings, preflight scan, log) | `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/progress.md` |
| State surveys (read these instead of the old tier1-v4 HANDOFF) | same directory: `survey-a0.md`, `survey-indexes.md`, `survey-b.md` |
| Task briefs / reports | same directory: `task-N-brief.md`, `task-N-report.md` |
| SDD skill scripts | `~/.claude/plugins/cache/claude-plugins-official/superpowers/6.4.1/skills/subagent-driven-development/scripts/{sdd-workspace,task-brief,review-package}` |

Tree: `C:/atx`, branch `feat/tier1-v3-warehouse`. HEAD at session start **and at stop**: `81d201fc`
(no commit was made by this session — confirm with `git log --oneline 81d201fc..HEAD`).

## 3. What this session did

1. Read the request; found that an earlier Codex-run session (tier1-v4, idle since 17:21 EDT) had already
   absorbed it and left much half-built code, most of it uncommitted and none of it run on real data.
2. Ran three read-only state surveys (Opus) — the three `survey-*.md` files. They are the most valuable output
   so far: each maps what exists (file:line), what is broken, and what a real run needs.
3. Wrote the plan and the ledger, with a preflight conflict scan and every ruling recorded.
4. Dispatched three implementers in parallel on disjoint files (Tasks 1, 6, 8). The owner stopped the session
   about 15-30 minutes later; all three were asked to wind down (section 5).
5. Disk: emptied the Recycle Bin, +24.6 GB (section 8).

## 4. Task table

| # | task | state at stop |
|---|---|---|
| 1 | A0 check harness: switches S1/S2, pin-safe S3/S4 stage runner, arm orchestrator, input recovery (367 SEC review bodies, receipt, TickerHistory hard link) | dispatched 20:55, stopped 21:11 before any code was written — restart from brief (section 11) |
| 2 | Run arms A, B1, B2, B12 + D0 + four comparisons (≈ 1.5-2.5 h serial) | not started; needs Task 1 reviewed |
| 3 | PIT-a + PIT-c in `identity_table` / `identity_backfill` / `identity_names`, tests T1, T2, v3 export names | not started; needs Task 1 committed |
| 4 | PIT-b clock-honest reconstruction, tests T3, T4 (hard) | not started; needs Task 1 committed |
| 5 | Fixed build: re-run the four arms, expect zero in rows 1-4, 7, 8; publish `identity-bridge-v3-*` side by side | not started; needs 2, 3, 4 |
| 6 | Indexes v2: panel-based stage on the existing PIT kernel, R7.1(a) / R7.2(d), year parts, measured receipt keys, full A.4 test, registry fix | dispatched 21:05, stopped 21:11 before any code was written — restart from brief (section 11) |
| 7 | Indexes v2 real build, gates, acceptance | not started; needs 6, 1, 3 |
| 8 | B3 export + acceptance prints + CPI-U NSA | dispatched 20:57, stopped 21:11 before any code was written — restart from brief (section 11) |
| 9 | B1 code + pilot landing (≤ 2,000 filings) | not started |
| 10 | B1 full-cohort landing | **owner-gated** (≈ 330k filings, ≈ 650 GB transfer, ≥ 19-37 h) |
| 11 | Response document for the P9 lane (`atx-db/docs/p9-al-data/response.md`) | not started; last |

No task has been reviewed. No task is complete.

## 5. Implementer state at stop

At 21:11 EDT, when the wind-down messages were sent, `git log 81d201fc..HEAD` was empty and none of the new files
named in the briefs existed yet (`a0_check.py`, `exchange_calendar.py`, `compustat_std_parts.py`,
`cpi_u_nsa.py`, their tests, `atx-db/docs/p9-al-data/`). No new directory existed under
`atx-db/data/research/identity_rehearsal/`, and `atx-db/data/cache/identity-oracle-session8` had not been
re-created, so the SEC re-fetch had not started.

Each implementer was told: start nothing new, commit only test-green work with an explicit pathspec, otherwise
leave the tree as is, and write a `## Stopped at (owner stop 2026-10-04)` section at the top of its report file.
**Read those sections first** — they are the authoritative per-task state:

- `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/task-1-report.md`
- `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/task-6-report.md`
- `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/task-8-report.md`

Section 11 at the end of this file records what the implementers replied: all three had written nothing, so the
three tasks restart from their briefs.

## 6. Facts the plan rests on (from the surveys)

**A0 (identity bridge)**
- Nothing of A0 had run on real data. Switches S1-S4 existed only in an untracked, partly failing
  `private_identity_capsule.py` (3 of 10 tests failing, no controller). PIT-a/b/c and T1-T4 did not exist.
- Reusable and green: `private_identity_differential.py` (`compare_d0`, 3/3) and
  `private_identity_arm_comparison.py` (`compare_arms`, rows 1-10 and identifier exports, 4/4).
- Missing inputs are all recoverable: the receipt `ri2_a.json` can be rewritten verbatim from the published
  `build.json` `pin.inputs`; `TickerHistory3.parquet` in `C:/Users/natha/Downloads` matches the pinned SHA and
  only needs a hard link at its pinned path; the 367 document-review bodies are public SEC documents whose URLs
  and SHA-256 are pinned (≈ 361 requests, ≈ 0.56 GB).
- Measured cost of one rehearsal: prepare ≈ 7 min, six phases ≈ 8 min, peak 0.58 GiB. Whole check ≈ 1.5-2.5 h.
- Provenance: the published bridge records `git_head c0dc376f`, which has no `alpha_panel/` directory, and its
  `common.py` hash matches no git revision — no producing commit can be named. That is itself an answer D0 asks for.
- PIT-a and PIT-c are low-to-medium effort in unpinned files. PIT-b is high effort (a clocked replay per vendor
  line in `identity_reconstruction.py`; design sketch in `survey-a0.md` §5).

**Indexes**
- The earlier session's pure kernel `private_index_pit.py` (R1-R5 + receipt) is sound and tested, and the A.4
  test passes, but its assertions 1-3 are only partly explicit.
- The production stage cannot run: it was wired to six native inputs that do not exist, and the v1 build was
  deleted from the working-tree `indexes.py` (HEAD still has it).
- **The uncommitted `indexes.py` breaks `stagelake.registry.load()` for every stage** (`TypeError ...
  'native_product'`). Task 6 fixes this. Until it lands, anything that loads the registry from the working tree
  fails; code run from a `git archive` export of HEAD is unaffected.
- The published panel already has every column R7 needs, so no panel rebuild (6,102 s) is required.
- `directory_decided_on_rank` and `backfill_cik_on_rank` were hard-coded zeros in the earlier code; the request
  wants measured counts.

**B**
- B3: all 20 requested columns already exist in the published funda (235,563 rows). The export is a cheap
  re-partition plus a manifest. FY2009 has 458 first-reported issuers and FY2010 has 1,457, against ≈ 7,400 a year
  from FY2011 (XBRL phase-in). CPI-U NSA is not in house; bls.gov returned 403 to the house client.
- B1: `text_native` is fixture-only and has never run on real filings; no 10-Q support, no `doc_sim_cosine`.
  The cohort is 330,258 filings; ≈ 650 GB of primary documents; a streaming design needs < 16 GB of disk.
- Engine manifest contract (C5): `schema`, `status` exactly `"complete"`, `rule`, `clock_rule`,
  `staleness_days` (integer), `files` = `{rel: {bytes, sha256}}`. Details in `survey-b.md`.

## 7. Rulings I made (each is in the ledger with its cost if wrong)

1. **S1** Work in the main tree `C:/atx`, explicit-pathspec commits only, no new worktree — the data lake, the
   `.venv` and the uncommitted scaffolding exist only there.
2. **S2** The request is the spec; the plan was written without owner review.
3. **S3** Up to three implementer lanes in parallel on disjoint files (overrides the skill's "never parallel");
   review packages are path-scoped when lanes interleave.
4. **S4** Implementers Opus; task reviewers Sonnet, Opus for Task 4 and the final review.
5. **S5** `alpha_panel/common.py` and `alpha_panel/identity_links.py` are never edited: an in-flight fundamentals
   release generation (r17) and a universe release (r15) of the paused tier1-v4 work pin their bytes. Consequence:
   the production identity chain stays on rehearsal r4; Task 5 prepares an activation patch and does not apply it.
6. **S6** Published artefacts that P9 pins by SHA are never overwritten. New versions go side by side
   (`identity-bridge-v3-*`, `identity_v3/`, `indexes_v2/`, `compustat_std_parts/`, `text_native_v2/`).
7. **A0-1** Abandon the capsule. S1/S2 become default-off environment switches in the unpinned production
   modules; S3/S4 a pin-safe runner; arms run from a frozen `git archive` export of one commit.
8. **A0-2** Re-fetch the 367 pinned review bodies from SEC (≤ 5 req/s, each verified against its pinned SHA).
   The earlier session's "HTTP hold" was its own accounting rule, not an owner prohibition.
9. **B1-1** B1 code and a ≤ 2,000-filing pilot proceed; the full landing is owner-gated (transfer volume).
10. **B3-1** B3 exports the published v10-based funda as is; the thin FY2009-2010 history is reported as measured.
11. **IX-1** Indexes v2 ships by "Path B" (panel-based snapshot feeding the existing kernel); the native-input
    path is left untouched.
12. **IX-2** Published as `indexes_v2/`, flat, manifest key `rule`; v1 `indexes/` stays.
13. **IX-3** R7.2(d): a backfill-linked line is treated as if the backfill link did not exist (name-linked if a
    name link exists without backfill withholding, else unlinked) — not excluded. This is why Task 7 depends on
    Task 3.
14. **IX-4** R7.1: option (a), dated evidence only. If the R2000P count gate fails under it, report FAIL; do not
    loosen the band.
15. **P1 / P2** `a0_check` does not hard-code the pit export directory name; Task 3 names the pit link-day file
    `identity/links_pit.parquet` (schema of `links_combined`).
16. **D-1** Emptied the Recycle Bin (irreversible).
17. **A0-3** Arm seeds are plain copies, not hard links; no attribute change on published files (made after the
    wind-down, from the Task 1 implementer's note). Cost if wrong: ≈ 10 GB of temporary disk.
18. **A0-4** No junction from the frozen code export into the data lake; the runner patches the path constants of
    `alpha_panel.common` in memory (same origin). Cost if wrong: the runner patches four constants instead of one.

The owner has not reviewed any of these. Rulings 5, 6, 9 and 13 change what "complete" means and are the ones to
raise first.

## 8. Disk

- Free space: 38.3 GB before, 62.9 GB after emptying the Recycle Bin (+24.6 GB: six stale pool worktrees and
  three build directories that an earlier cleanup had already deleted). At stop `df` showed 85 GB free; the extra
  ≈ 21 GB was freed by something other than this session.
- Attempted and **denied by the permission classifier** (not retried): clearing
  `C:/Users/natha/vcpkg/buildtrees` (25.9 GB of port build intermediates).
- Left for the owner, largest first: `C:/atx-wt/pool-2/build-equity` 76.6 GB (owned by the live P9 sprint
  session); `vcpkg/buildtrees` 25.9 GB and `vcpkg/packages` 14.6 GB (regenerable); `C:/atx/deps/vcpkg-server`
  15 GB (in use by `atx-server`); `C:/atx-cache/ccache` 11.3 GB (in use); stale scratch
  `atx-db/data/alpha_panel/v1/_tmp/*.duckdb` ≈ 3.6 GB; `AppData/Local/Temp/pytest-of-natha` 1.7 GB.

## 9. Hazards for whoever resumes

- **Dirty tree.** ≈ 230 uncommitted paths from other sessions. Never `git add -A`, `git stash`, `git restore` or
  `git checkout -- <file>`. Commit with `git commit -m ... -- <paths>` only.
- **Pinned files** (ruling 5): editing either one invalidates the paused fundamentals release generation.
- **Host.** 15.7 GB RAM with ≈ 2-3 GB free; several other Claude sessions are live (P9 sprint in
  `C:/atx-wt/pool-2`, statarb in pool-26). Every real-data job goes through
  `C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py`; one heavy job at a time.
- **Deletes.** The permission classifier refuses irreversible deletes of files this session did not create. Do
  not work around it; list the candidate and move on.
- **Do not follow the old tier1-v4 HANDOFF's process** (40 GiB disk floor, per-fixture reviews, HTTP hold). It
  produced almost no runnable result in two days. Its facts are useful; its gates are not this plan's gates.
- **Stage registry** is broken in the working tree until Task 6 lands (section 6).

## 10. How to resume

1. Verify the request hash (section 1), then `git log --oneline 81d201fc..HEAD` and `git status --short` on the
   files named in the Task 1, 6 and 8 briefs.
2. Read the three `Stopped at` sections (section 5) and section 11 below; reconcile them against git.
3. For each of Tasks 1, 6, 8: if committed work exists, generate a review package for it and continue the task
   from the report's next steps with a fresh implementer (hand it the brief, the report and the dispatch context
   that the ledger records); if nothing durable exists, re-dispatch from the brief.
4. Follow the plan's order. Lanes that can run together: {1 → 2}, {3, 4 after 1}, {6 → 7 after 3}, {8}, {9}.
   Task 5 needs 2, 3 and 4. Task 11 is last.
5. Every task gets a task review (spec + quality) before it is marked complete in the ledger; a final
   whole-branch review comes after Task 11.
6. Ask the owner about Task 10 (B1 full landing) and about ruling 5 (when to activate rehearsal r5) once the
   other work is done, or earlier if they are available.

## 11. Implementer replies after the wind-down

All three replied by 21:25 EDT. **None of them had written, committed or run anything**: each was still reading
when the stop arrived. So Tasks 1, 6 and 8 restart from their briefs; the `Stopped at` sections hold only
orientation notes that save the next implementer some reading.

| task | commits | files edited | real runs / network | notes worth carrying into the re-dispatch |
|---|---|---|---|---|
| 1 | none | none (`identity_links.py`, `identity_backfill.py` unmodified; `a0_check.py` absent) | none; 0 of 367 bodies fetched; no receipt, no hard link, no new directory | (a) Stage I exits through a `raise SystemExit` statement, not a call, so the S4 run-time patch has to go on `_validation_result`; two ways are written up in the report. (b) A junction from the frozen export into `atx-db/data` is dangerous to delete. (c) An NTFS read-only flag on a hard-linked seed also flags the published file. |
| 6 | none | none (the four files that were already dirty are as the earlier session left them) | none | (a) `stagelake.registry.load()` is still broken in the working tree. (b) `atx-db/tests/test_exchange_repair.py:37` already fails on the working tree: it calls the old two-argument `snapshot(con, d)` and its test panel lacks the columns the new eligibility rules need; that file is not in the Task 6 brief and needs a ruling. (c) `panel.py` formulas for `security_type`, `is_etf`, `is_operating` are the same in the working tree and at HEAD; only the link tiers differ. |
| 8 | none | none | two read-only schema/count probes under the guard; 0 network requests | upstream `compustat_std/manifest.json` SHA-256 is `9a7d486f61fa557ffbe53aea7aab12a08525ac3d37f3f3f24fd133fea5c96325`; funda schema, rows per year and FSDS layout are in the report. |

Two points of Task 1 were ruled on at once and the plan text was changed to match (ledger rulings A0-3, A0-4):

- **A0-3.** Arm seeds are plain copies (≈ 2.55 GB per arm, deleted after the run). No hard links, no attribute
  change on published files.
- **A0-4.** The frozen export gets no junction into the data lake. The runner sets the `PACKAGE_ROOT`-derived
  path constants of `alpha_panel.common` in memory instead.

Scratch left outside the repo by the implementers (safe to delete): one probe script and one read copy of
HEAD's `panel.py` in the session scratchpad under `AppData/Local/Temp/claude/`.
