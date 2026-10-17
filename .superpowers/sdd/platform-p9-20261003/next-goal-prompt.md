# Goal prompt for the next parent (PM) agent (refreshed at STOP 14, 2026-10-16 16:00)

Paste everything below the line as the `/goal` text. The paragraph "Decisions
already made for me" carries the delegation the owner gave ("do not stop to ask
questions, you are allowed to rule on all open questions"), the owner's
2026-10-07 words (parallel Opus implementers; quality / modular / reusable /
performant C++ centralized in atx-engine and atx-impl; no TDD), the owner's
2026-10-15 words (parallel progress, PAR-1), the owner's 2026-10-16 note (TEST-2:
no large suite runs) and the structural rulings of RESUME 9 .. 14. Strike or
change any you disagree with before pasting. Two items wait on the owner
(status-15 "Open with the owner"): OD3-DB, and the clean-up owner-calls; one is
record-only (who deleted the fields dirs, FIELDS-GONE).

---

Complete the P9 sprint plan
`C:/atx-wt/pool-2/docs/plans/2026-10-03-p9-sprint-plan.md` end to end as project
manager.

Start by reading, in order, in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/`:

1. `status-15.md` (state, tracks table with every head, what is open with me,
   next steps; the ledger block from "- RESUME 14" (line 1020) to the end
   records every hand-back and ruling of the last session)
2. `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
   `resume7-addendum.md`, `resume8-addendum.md`, `resume9-addendum.md`,
   `resume10-addendum.md`, `resume11-addendum.md`, `resume12-addendum.md` (the
   standing contracts you give every agent; the last wins where they differ),
   `r12-post-gate.md` (the CELLS runbook steps 0-5 and hand-back protocol, DERIV,
   OD-3 with the OD3-GO checkpoint, FREEZE), `r14-prompts/` (this session's
   dispatch files: `cells-prompt-1.md` is the cells root's dispatch of record
   and binds every cells agent; `deriv-p9l-prompt.md` is READY but for
   `@@OUTPUT-DIRS@@`; `deriv-p9v-prompt.md` / `deriv-p9x-prompt.md` /
   `idx-final-prompt.md` carry tokens; the FLAGS / PRE / reviewer prompts are
   history of the RQ-1 close), then `progress.md` from "- RESUME 14" to the end
   (every "Ruling" line is binding, in that block and in the earlier ones;
   REPIN-UW / REPIN-R2 are superseded by REPIN-REV; the CELLS-1 .. 6, ADOPT-1,
   OD3-1 / OD3-GO rulings are IN the ledger at :1035-1042; P9L-WBUDGET :1047 and
   SEAL-HAND :1048 govern the P9-L resume)
3. `handoff-root-cells-1.md` (the cells root's stop handoff: P9-L's Done / Not
   done / Exact next step), `cells/p9l-label-terminal-plan.md`,
   `cells/p9l-label-terminal-record.md` (STOPPED at w, no verdict),
   `handoff-root-gate.md` (the gate's FINAL handoff + its "FLAGS-ROUTE
   amendment"), `root-gate-report.md` ("Status by step ... FINAL"; "FLAGS-ROUTE
   (RQ-1 close)"), `gate-report-review.md` (PROCEED), `prereg-2bii-review.md`
   (part 2; "RQ-1: CLOSED (APPROVE)")
4. `p9-prereg.md` (round 2b-ii COMPLETE, RULED: PRE-2BII-RULED), `rulings-index.md`
   + `rulings-index-check.md` (IDX-FINAL), `p9-od3-runbook.md` (OD-3; "F.1
   governs")

Root tree: `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Re-pin of
record `2078f877` (EXE-FREEZE from it: no exe of the frozen set is rebuilt);
the FLAGS-ROUTE spec commit `f8d852f3` (the lineage parent `lib-v8ysb-gm.json`
now `a4ef6c04...c06a`); P9-L's registration specs `0bba6c89` / `6ea79e5a` (code
head); docs head `31036825`. First verify the stop state yourself (heads, clean
tree but the five PNGs and the PM's uncommitted `progress.md` /
`next-goal-prompt.md` / `status-15.md`, trial ledger 133 lines sha `27e40f9f`,
`build-equity/research-store/` absent, `index.sqlite` absent, lease status:
every P9 lease `owner=dead` except pool-32; foreign atx-vol sessions
`owner=alive` on pools 1 / 3 / 4 / 33 / 34 (HOST-6 / HOST-6a); all untouched,
LEASE-DEAD), read-only. Pools 8 and 24 are FROZEN (TEETH-POOL). Pool-23's
`build-equity/` holds mutated teeth binaries (TEETH-BIN-23): nobody deletes it,
nobody uses its exes. `C:/atx/atx-db/` is never opened (A4-DB) unless I answer
OD3-DB yes (read-only, the era builds only). The fields dirs v9 / v10 / v13 /
v14 / v15a are gone from disk (FIELDS-GONE): nothing is rebuilt; FIELDS-RISK may
surface at a cell's preflight (a driver stop: you rule).

How to run it:

- You are PM only: write the ledger, briefs and rulings. Never build, run real
  data or edit code yourself. Use Opus subagents (model: opus) for every root,
  reviewer, DERIV and cells step (dispatch form: "your full dispatch text is
  the file ...; read it WHOLE; never ask mid-run; no subagents"). One root
  agent at a time in pool-2's working tree for code / runs; chain fresh root
  agents through `handoff-root-cells-<n>.md` when one runs low on context; a
  finished agent may be resumed by message for a small follow-up (it keeps its
  context). Docs agents (DERIV, reviewers, IDX-FINAL) may work beside root in
  pool-2's sprint dir but commit ONLY by pathspec (PAR-1); every commit names
  explicit paths plus
  `:(exclude).superpowers/sdd/platform-p9-20261003/gate-logs/tools/.mypy_cache`
  (GATE-MYPY). Hand artifacts over as files to protect your context. Reviewers
  of earlier sessions are gone: any re-review is a fresh reviewer that reads the
  previous review first (REV-FRESH).
- My words (2026-10-15): do not get blocked on long-running builds or trials;
  always make progress in parallel where possible (PAR-1 and its family). The
  hard rules are not loosened by it: one C++ build among our pools, one bounded
  real-data run per pool, one root in pool-2's tree.
- My words (2026-10-16, TEST-2): do not waste time on large full-suite testing;
  focus on smaller focused testing only when required. The cells run no suite.
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
  `atx-impl/tools/holdout_gate.py` (the OD-3 runbook F.1 orders it never).
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
  <= 600 per stage, `--max-rss-mib 2048` unless prereg C.14's registered cap or
  P9L-WBUDGET's w cap applies; free RAM stated first). Pytest, if any, with
  explicit paths and an outside-tree short TMP root (GATE-TMP-2). C: must hold
  at least 15 GB free before a run that writes (HOST-6a).
- Disk: agents delete their own regenerable scratch by name once their report is
  committed; never wildcard-delete shared scratch; never delete pinned,
  reference or golden artifacts (the D-COV stores under
  `build-equity/p9-m2i-dcov-*` and `p9-m2d-cov-risk-container`; the real cache
  `mega-candidate-cache-v8-lo3` incl. its Release subtree; every P9-B0
  reference dir `p9-b0g-*`, `p9-b0-r2-nav`; the v8 NAVs of record;
  `train-2020-2023-lo3-fields-v15`; a cell of record's output dirs; P9-L's
  completed u / fit / card outputs).
- Subagent commit trailer:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Ledger appends: write the lines to a scratch file and `cat >>` (a Bash heredoc
  with apostrophes fails in the hook layer); never edit earlier lines. Root
  commits the ledger and the PM's dispatch docs with its docs commits
  (DISPATCH-COMMIT); the PM never commits in pool-2.

Decisions already made for me (ledger OD-DELEG, OD5-1 .. OD5-7, and the rulings
of RESUME 6 .. 14; binding unless I say otherwise in this session):

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
6. GATE-1 DONE and reviewed PROCEED: re-pin of record `2078f877`; EXE-FREEZE
   from it; the P9-B0 references `p9-b0g-yf0-{u,w,nav}`, `p9-b0g-x10-nav`,
   `p9-b0g-y1-nav`, `p9-b0-r2-nav` (FIELDS-SUB-A, read by path); X-5's identity
   cut (FIELDS-X5); the real cache carries a Release identity subtree
   (GATE-RELCACHE); R-1 closed by `d78f5cc2`.
7. Lapse table of record (LAPSE-TABLE): P9-L, P9-H, P9-M, P9-C, P9-MV, P9-V run;
   P9-W LAPSED (0) (D3-Q1a DIFFERENT; PW-LAPSE-PRINT); P9-X pending PRE-30 /
   PRE30-MVX. Fit mode of every v2 cell: flag-absent. COMPOSE-1a ACCEPT.
   ALCLOCK-1 held.
8. A4: merged at its pre-flip head `6dab6948`; the flips and the fix-1 tests are
   P10 whole (A4-DB, A4-P10-FLIPS, A4-FIX1-P10). Python stays the producer of
   record for the two flipped fields; Y-F0's manifest `26fee5ce` is unchanged.
9. SQL4: in the gate head `06dc9e5a`; checks green (SQL4-CUTOFF-2); SQL-11 (no
   cache index flag on any cell argv).
10. Permission system: PERM-1, TEETH-POOL, TEETH-BIN-23; the refused deletions
    of earlier sessions were done by my hand.
11. The known-red set is EMPTY.
12. `C:\atx\data` (DATA-3), `atx-db-request.md` (AL-DATA): unchanged, mine.
13. COMPOSE-1 / COMPOSE-1a / COMPOSE-PRE, COVMV-PIN / COVMV-PIN-2, MV-FLAG-P0,
    B8A-PROD / B8A-PROD-2, D3Q3-ARGV / D3W-TPL / D3W-V3READ (P9-W: moot),
    F1-FIELDS, SEAL-REF / SEAL-HAND, REPIN-REV, FIELDS-RISK / OD3-FIELDS-RISK:
    as ruled.
14. OD3-DB: if I have not said yes by the OD-3 step, OD-3 is "not read in P9",
    G-B8 "not evaluated", the freeze verdict says so.
15. The cells run one at a time with one hand-back each and my explicit
    ACCEPTED / REJECTED line per cell written by the PM (CELLS-1 .. 6, in the
    ledger); hand cells get an independent DERIV re-derivation (CELLS-2); the
    OD-3 read has a pre-read checkpoint (OD3-GO).
16. The final report's ruling list = `rulings-index.md` corrected by
    `rulings-index-check.md` plus every ruling from ledger :952 on, compiled at
    the freeze by a fresh docs agent as `rulings-index-final.md` (IDX-FINAL;
    `r14-prompts/idx-final-prompt.md`; refusals labelled `PERM-R<n>`).
17. GATE-REV part 2's one Required RQ-1 is CLOSED: FLAGS-ROUTE (`f8d852f3`: the
    two print-only flags live in the lineage parent's `nav.flags`; every spec
    over it inherits them, FLAGS-ROUTE-2; the spec route reproduced the Y-F0
    reference byte for byte; re-check `a32b6f4c`); argv token order is not an
    input (FLAGS-ORDER); `p9-prereg.md` is RULED (PRE-2BII-RULED). No re-pin.
18. P9-L (`p9l-label-terminal`) is registered (`0bba6c89`, `6ea79e5a`,
    `0950cb57`) and STOPPED at its w pass: the pinned IC exe's memory admission
    refuses `--label-terminal` under the registered w budget 3,072 MiB (needs
    >= 3,289) before any payload load; no read, 0 trials, not a void, not
    UNDEFINED (P9L-WBUDGET): the next cells root agent redoes CELLS step 1 for
    that one edit (w budget 3,584 MiB, runner cap alike, free RAM stated) and
    resumes step 2 with `--attempt w=2`, never re-running the completed u / fit /
    card; a second refusal = STOP, you rule (UNDEFINED (0) is B.1's fallback; a
    rebuild is never the answer). Then DERIV, my ruling line, P9-H.

Done means: P9 cells run in the registered order with explicit adopt / reject
per cell, the adoption print, OD-3 read once on P9-F0h (or recorded "not read"
per OD3-DB), the freeze gate verdict written (plan 4.4 / prereg G), and a final
report with every ruling made in the sprint (IDX-FINAL) and the owner items.
Keep replies to me terse.
