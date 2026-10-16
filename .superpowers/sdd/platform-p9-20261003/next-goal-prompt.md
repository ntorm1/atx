# Goal prompt for the next parent (PM) agent (refreshed at STOP 13, 2026-10-16 13:20)

Paste everything below the line as the `/goal` text. The paragraph "Decisions
already made for me" carries the delegation the owner gave ("do not stop to ask
questions, you are allowed to rule on all open questions"), the owner's
2026-10-07 words (parallel Opus implementers; quality / modular / reusable /
performant C++ centralized in atx-engine and atx-impl; no TDD), the owner's
2026-10-15 words (parallel progress, PAR-1), the owner's 2026-10-16 note (TEST-2:
no large suite runs) and the structural rulings of RESUME 9 .. 13. Strike or
change any you disagree with before pasting. Two items wait on the owner
(status-14 "Open with the owner"): OD3-DB, and the clean-up owner-calls; one is
record-only (who deleted the fields dirs, FIELDS-GONE).

---

Complete the P9 sprint plan
`C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project
manager.

Start by reading, in order, in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:

1. `status-14.md` (state, tracks table with every head, what is open with me,
   next steps; the ledger block from "- RESUME 13" (line 952) to the end records
   every hand-back and ruling of the last session)
2. `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md`,
   `resume10-addendum.md`, `resume11-addendum.md`, `resume12-addendum.md` (the
   standing contracts you give every agent; the last wins where they differ),
   `r12-post-gate.md` (the CELLS runbook and hand-back protocol, OD3-PREP, OD-3
   with the OD3-GO checkpoint, DERIV, FREEZE), `r13-prompts/` (this session's
   dispatch files: `gate-prompt-2.md` .. `gate-prompt-4.md` carry rulings that
   bind the cells; `cells-prompt-1.md` is READY but for `@@DOCS-HEAD@@`),
   `r12-prompts/` (`deriv-prompt.md`, `od3prep-prompt.md`;
   `ledger-cells-rulings-draft.md` = the CELLS-1 .. 6, ADOPT-1, OD3-1 / OD3-GO
   rulings NOT yet in the ledger: append them before the first cell), then
   `progress.md` from "- RESUME 13" to the end (every "Ruling" line is binding,
   in that block and in the earlier ones; REPIN-UW / REPIN-R2 are superseded by
   REPIN-REV)
3. `handoff-root-gate.md` (the gate's FINAL handoff: the re-pin's exe table, the
   six P9-B0 reference dirs, the substitution list, what the cells agent must
   know), `root-gate-report.md`, `gate-report-review.md` (part 1; RETURN R-1
   closed by `d78f5cc2`), `prereg-2bii-review.md` (part 2; may be PARTIAL at
   the stop: its "What remains")
4. `p9-prereg.md` at `a40db317` (round 2b-ii COMPLETE), `rulings-index.md` +
   `rulings-index-check.md` (IDX-FINAL), `p9-od3-runbook.md` (OD-3)

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Gate head
`06dc9e5a`; re-pin of record `2078f877`; code head `f8a85ad4` (EXE-FREEZE from
`2078f877`: no exe of the frozen set is rebuilt). First verify the stop state
yourself (heads, clean trees, trial ledger 133 lines sha `27e40f9f`,
`build-equity/research-store/` absent, `index.sqlite` absent, lease status:
every P9 lease `owner=dead` except pool-32; pools 33 / 34 / 36 are foreign
atx-vol sessions `owner=alive` (HOST-6); all untouched, LEASE-DEAD), read-only.
Pools 8 and 24 are FROZEN (TEETH-POOL). Pool-23's `build-equity/` holds mutated
teeth binaries (TEETH-BIN-23): nobody deletes it, nobody uses its exes.
`C:/atx/atx-db/` is never opened (A4-DB) unless I answer OD3-DB yes (read-only,
the era builds only). The fields dirs v9 / v10 / v13 / v14 / v15a are gone from
disk (FIELDS-GONE): nothing is rebuilt (their builder reads atx-db); the gate
worked around it (FIELDS-SUB-A, FIELDS-X5); FIELDS-RISK may surface at a cell's
preflight (a driver stop: you rule).

How to run it:

- You are PM only: write the ledger, briefs and rulings. Never build, run real
  data or edit code yourself. Use Opus subagents (model: opus) for every root,
  reviewer, PRE, DERIV and cells step (dispatch form of this session: "your full
  dispatch text is the file ...; read it WHOLE; never ask mid-run; no
  subagents"). One root agent at a time in pool-2's working tree for code /
  runs; chain fresh root agents through `handoff-root-cells-<n>.md` when one
  runs low on context; a finished agent may be resumed by message for a small
  follow-up (it keeps its context). Docs agents (PRE, reviewers, DERIV) may work
  beside root in pool-2's sprint dir but commit ONLY by pathspec (PAR-1); every
  commit names explicit paths plus
  `:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache`
  (GATE-MYPY: the editor's type-checker writes into the sprint dir). Hand
  artifacts over as files to protect your context. Reviewers of earlier
  sessions are gone: any re-review is a fresh reviewer that reads the previous
  review first (REV-FRESH).
- My words (2026-10-15): do not get blocked on long-running builds or trials;
  always make progress in parallel where possible (PAR-1 and its family). The
  hard rules are not loosened by it: one C++ build among our pools, one bounded
  real-data run per pool, one root in pool-2's tree.
- My words (2026-10-16, TEST-2): do not waste time on large full-suite testing;
  focus on smaller focused testing only when required. The gate's suites ran
  once (all green) before this note; nothing of the kind runs again. The cells
  run no suite.
- Priority: high quality end-to-end implementations; modular, reusable,
  performant code centralized in the atx-engine and atx-impl C++ code base; keep
  moving from JSON artifacts and repeated Python scripts to SQLite and core C++.
- Decide, don't stall, and do not stop to ask me questions: you may rule on
  every open question. Record every decision in the ledger as "Ruling: decision
  -- why -- cost if wrong" before the measurement it could bias. Stop only for
  irreversible or destructive actions that I have not asked for,
  security-sensitive actions, pushes or merges outside the worktrees, or a plan
  so broken every path is a guess.
- Treat a subagent's report as its own account, not as proof; verify read-only
  (heads, commit scopes, files) at every hand-back.

Hard rules:

- Never build, switch or commit in `C:/atx`. Never touch `atx-db/` (OD3-DB:
  only if I say yes, read-only, the era builds). No push. Never `git worktree
  prune`. Never touch `main` or `origin/main`. Never any lease action (no
  release, no stale recovery).
- Open no data dated 2024-01-01 or later. Never edit an expected hash. Never run
  `atx-impl/tools/holdout_gate.py` (the OD-3 runbook F.1 / `p9-od3-runbook.md`
  orders it never).
- Trial ledger `build-equity/trials.jsonl` is 133 lines (sha `27e40f9f`); it
  changes only through registered P9 cells (a v2 cell's driver at judge time; a
  hand cell after my "DERIV agrees" line written by the PM).
- No build is expected (EXE-FREEZE from `2078f877`; an unavoidable rebuild =
  P9-B0 and the re-pin redone first, no cell between). If one were needed:
  configure only as `powershell scripts\atx-build.ps1 configure -Preset
  <preset>` (`equity-dev` / `equity-rel`); build only through
  `scripts/research-build.ps1`, target-scoped, one at a time among our pools.
  If the permission system refuses a command, do not retry it and do not route
  around it: report it to me (PERM-1); never re-do a refused deletion for a
  subagent (that is permission laundering). Never `git checkout -- <file>`,
  `git restore` or `git stash` on a working file (TEETH-RESTORE; TEETH-CRLF); a
  committed mistake is undone by a new commit (REPIN-REV form).
- Every real-data run through `scripts/run_bounded_research.py` (`--seconds`
  <= 600 per stage, `--max-rss-mib 2048` unless prereg C.14's registered cap
  applies). Pytest, if any, with explicit paths and an outside-tree short TMP
  root (GATE-TMP-2).
- Disk: agents delete their own regenerable scratch by name once their report is
  committed; never wildcard-delete shared scratch; never delete pinned,
  reference or golden artifacts (the D-COV stores under
  `build-equity/p9-m2i-dcov-*` and `p9-m2d-cov-risk-container`; the real cache
  `mega-candidate-cache-v8-lo3` incl. its Release subtree; every P9-B0
  reference dir `p9-b0g-*`, `p9-b0-r2-nav`; the v8 NAVs of record;
  `train-2020-2023-lo3-fields-v15`).
- Subagent commit trailer:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Ledger appends: write the lines to a scratch file and `cat >>` (a Bash heredoc
  with apostrophes fails in the hook layer); never edit earlier lines. Root
  commits the ledger and the PM's dispatch docs with its docs commits
  (DISPATCH-COMMIT); the PM never commits in pool-2.

Decisions already made for me (ledger OD-DELEG, OD5-1 .. OD5-7, and the rulings
of RESUME 6 .. 13; binding unless I say otherwise in this session):

1. Identity bridge: the exposure is accepted. P9 cells and the single OD-3 read
   run on the universe as built; every print carries the flag; a freeze pass is
   worded "passed with the identity-bridge flag".
2. Debug vs Release `pow` / `exp2`: the visible Release skip stays; no re-pin in
   P9. Release is the producer of record; the pinned covariance stores are never
   rebuilt (CRT-2).
3. Local `main` and `origin/main`: untouched; listed for me in the final report.
4. No new-signal cell (P9-S lapsed at 0). No data purchase.
5. G-B2 hurdle 8.0%, labelled "plan placeholder, not owner-stated". P9-X is the
   Y-1 form (vol-target, cap 2.0); it is not a new trial iff P9-F0h is the
   re-based Y-F0 when it is reached (PRE-30 as PRE30-MVX reads it: P9-MV
   accepted or not); its parent is P9-F0h (PRE-MVX, MVX-F0H); P9-V pairs with
   P9-X; PRE-13 stands whole on P9-F0h (MVX-PRE13); a two-store NAV is P10; the
   PRE-34 printed-precision check is the cells root's at the P9-X step
   (PRE34-WHO).
6. GATE-1 DONE: every step holds, 0 failed, named skips only; re-pin of record
   `2078f877`; EXE-FREEZE from it; the P9-B0 references `p9-b0g-yf0-{u,w,nav}`,
   `p9-b0g-x10-nav`, `p9-b0g-y1-nav`, `p9-b0-r2-nav` (R-2 by fields
   substitution on v15, FIELDS-SUB-A, read by path); X-5's identity cut
   (FIELDS-X5); the real cache now carries a Release identity subtree
   (GATE-RELCACHE); the gate review's one Required (the OD5-1 line per
   reference) closed by `d78f5cc2`.
7. Lapse table of record (LAPSE-TABLE): P9-L, P9-H, P9-M, P9-C, P9-MV, P9-V run;
   P9-W LAPSED (0) (D3-Q1a DIFFERENT: the engine fit's stripped weights differ
   from the Python fit's; P9-F0h is the Python fit; a finding for the freeze
   report; PW-LAPSE-PRINT for its prediction and dSR lines); P9-X pending
   PRE-30 / PRE30-MVX. Fit mode of every v2 cell: flag-absent. COMPOSE-1a
   ACCEPT (P9-MV runs). ALCLOCK-1 held (P9-C and its reference name
   `--info-clock-print`).
8. A4: merged at its pre-flip head `6dab6948`; the flips and the fix-1 tests are
   P10 whole (A4-DB, A4-P10-FLIPS, A4-FIX1-P10). Python stays the producer of
   record for the two flipped fields; Y-F0's manifest `26fee5ce` is unchanged.
9. SQL4: in the gate head `06dc9e5a`; checks green (SQL4-CUTOFF-2); SQL-11 (no
   cache index flag on any cell argv).
10. Permission system: PERM-1, TEETH-POOL, TEETH-BIN-23; the refused deletions
    were done by my hand (`index.sqlite`, the mypy unstage).
11. The known-red set is EMPTY.
12. `C:\atx\data` (DATA-3), `atx-db-request.md` (AL-DATA): unchanged, mine.
13. COMPOSE-1 / COMPOSE-1a / COMPOSE-PRE, COVMV-PIN / COVMV-PIN-2 (the
    registration commit's closed diff: `/parent` + `template_sha256` only; the
    stale `mv-aim-v1.json` sentence stays, disclosed), MV-FLAG-P0 (root at the
    OD-3 P0 step), B8A-PROD / B8A-PROD-2 (P9-V's sigma_hat-bias line: the M2g Q5
    chain on the frozen Release exes with fields v15 substituted; "not
    produced" on any refusal), D3Q3-ARGV / D3W-TPL / D3W-V3READ (P9-W: moot),
    F1-FIELDS, SEAL-REF, REPIN-REV, FIELDS-RISK / OD3-FIELDS-RISK: as ruled.
14. OD3-DB: the OD-3 era builds need read-only access to the atx-db stage paths
    listed in `status-13.md`; if I have not said yes by the OD-3 step, OD-3 is
    "not read in P9", G-B8 "not evaluated", the freeze verdict says so.
15. The cells run one at a time with one hand-back each and my explicit
    ACCEPTED / REJECTED line per cell written by the PM (CELLS-1 .. 6 as
    drafted, to be appended at the cells dispatch); hand cells get an
    independent DERIV re-derivation (CELLS-2); the OD-3 read has a pre-read
    checkpoint (OD3-GO).
16. The final report's ruling list = `rulings-index.md` corrected by
    `rulings-index-check.md` plus every ruling from ledger :952 on, compiled at
    the freeze by a fresh docs agent as `rulings-index-final.md` (IDX-FINAL;
    refusals labelled `PERM-R<n>`; DISK-4 cited for :937 / :943).

17. GATE-REV: part 1 PROCEED (`gate-report-review.md` `6c608ff3`); part 2
    CHANGES_REQUIRED with one Required, RQ-1 (the two print-only flags have no
    route onto the driver-built cell NAV argv): Ruling FLAGS-ROUTE (ledger, the
    last RESUME 13 lines) fixes it by one spec commit on the lineage parent's
    `nav.flags` + verify + one bounded `--spec` NAV identity, re-checked by a
    fresh scoped reviewer; the first root agent of the next session does it
    before any cell (status-14 next step 2).

Done means: `p9-prereg.md` (round 2b-ii, COMPLETE) reviewed and ruled
(PRE-2BII-RULED after RQ-1's fix and re-check; the gate review's PROCEED line
is in), P9 cells
run in the registered order with explicit adopt / reject per cell, the adoption
print, OD-3 read once on P9-F0h (or recorded "not read" per OD3-DB), the freeze
gate verdict written (plan 4.4 / prereg G), and a final report with every ruling
made in the sprint (IDX-FINAL) and the owner items. Keep replies to me terse.
