# Next parent goal prompt — P9 AL-DATA atx-db request, resume after status 1

Paste the block below as the goal for the next parent session, started in `C:/atx` on branch
`feat/tier1-v3-warehouse`.

---

Resume and finish the P9 AL-DATA request to the atx-db owner. This prompt is my explicit instruction to continue
after the 2026-10-04 owner stop. The request is the spec:
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/al-data/atx-db-request.md` (frozen copy with SHA-256
`8f9916d5…3321` at `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/request-snapshot.md`). A plan, a ledger
and three state surveys already exist. Resume them; do not re-plan, re-survey, or restart.

**Read first, in this order**

1. `docs/superpowers/handoffs/2026-10-04-p9-al-data-status-1.md` — in full. It is the map: task table, facts,
   the rulings already made (section 7), hazards, resume steps.
2. `docs/superpowers/plans/2026-10-04-p9-al-data-atx-db-request.md` — Global Constraints GC1-GC10 and the eleven
   tasks.
3. `.superpowers/sdd/2026-10-04-p9-al-data-atx-db-request/progress.md` — the ledger. The ledger plus `git log`
   are the source of truth; trust them over any summary.
4. The `## Stopped at` section at the top of `task-1-report.md`, `task-6-report.md` and `task-8-report.md` in
   that directory.
5. Only when dispatching a task: its `task-N-brief.md` and the matching `survey-a0.md` / `survey-indexes.md` /
   `survey-b.md`. Do not read `.superpowers/sdd/tier1-v4/HANDOFF.md` or its `progress.md`; they are huge and the
   surveys already extracted what matters.

**How to work**

- Use the `superpowers:subagent-driven-development` skill as the controller. Implementers are Opus 5.5
  (`model: opus`), one fresh implementer per task, each given its brief path, the Global Constraints and the
  context the ledger records for that task. Task reviewers are Sonnet, except Task 4 (PIT-b) and the final
  whole-branch review, which are Opus. The controller dispatches, reviews, rules and keeps the ledger; it does
  not write source code.
- Up to three implementer lanes in parallel when their files are disjoint (ledger ruling S3). Lanes:
  {Task 1 → Task 2}, {Tasks 3 and 4 after Task 1 is committed}, {Task 6 → Task 7 after Task 3}, {Task 8},
  {Task 9}. Task 5 needs Tasks 2, 3 and 4. Task 11 is last.
- Every task gets a task review (spec compliance and code quality) before its `Task N: complete` line goes into
  the ledger. Run the fix loop the skill describes. Record every decision you take as a `Ruling:` line with its
  cost if wrong.
- Do not pause to ask me between tasks. Stop only for: an irreversible or destructive operation; a
  security-sensitive action; a side effect outside the worktree; or a plan defect that leaves every path a guess.

**First reconcile (before any dispatch)**

1. `sha256sum` the pool-2 request file. If it differs from the frozen copy, diff the two and tell me what changed
   before going on.
2. `git log --oneline 81d201fc..HEAD` and `git status --short` on the files named in the Task 1, 6 and 8 briefs.
   Compare with the three `Stopped at` sections. All three implementers reported that they had written,
   committed and run nothing when stopped; confirm that against git.
3. Check for live jobs by command line and start time (not by old PIDs): no arm run, fetch or pytest of this plan
   should be alive.
4. Regenerate the Task 1 brief (`task-brief PLAN 1`): the plan text of Task 1 changed after the first dispatch
   (seeds are copies; no junction into the data lake — ledger rulings A0-3, A0-4). Then re-dispatch Tasks 1, 6
   and 8 from their briefs, carrying the dispatch context recorded in the ledger and the notes in status section
   11. One addition for Task 6: `atx-db/tests/test_exchange_repair.py:37` already fails on the working tree (it
   calls the old two-argument `snapshot(con, d)`); decide whether that test belongs to Task 6 and rule on it.

**Binding constraints (details in the plan)**

- The tree holds about 230 uncommitted paths from other sessions. Commit only with an explicit pathspec. Never
  `git add -A`, `git stash`, `git restore`, `git checkout -- <file>`, or push.
- Never edit `atx-db/src/atx_db/alpha_panel/common.py` or `atx-db/src/atx_db/alpha_panel/identity_links.py`
  (byte-pinned by a paused fundamentals release).
- Never overwrite a published artefact under `atx-db/data/alpha_panel/v1/` or the published rehearsal
  `session8-phased-r4`; the P9 sprint pins them by SHA. Publish new versions side by side.
- The host has about 2-3 GB of free RAM and other Claude sessions are live. Every real-data job runs under
  `C:/atx/.superpowers/sdd/tier1-parity/run_memory_guarded.py`, one heavy job at a time.
- Network only where a task brief names it (SEC through the committed client at ≤ 5 requests/s).
- Keep at least 25 GB free on `C:`. Clean up what the plan's own jobs create. Do not delete files other sessions
  created; if the permission system refuses a delete, list the candidate for me and move on.

**Owner-gated — ask me, do not start**

- Task 10, the B1 full-cohort landing: about 330,000 filings, about 650 GB transferred, 19-37 hours or more.
- Applying the activation patch that moves the production identity chain to the fixed rehearsal (it edits the two
  pinned files and implies a downstream rebuild).

**Done means**

- Tasks 1-9 and 11 are each `complete` in the ledger with a clean review or parked findings with rulings.
- `atx-db/docs/p9-al-data/response.md` answers the request item by item: the A0 table (rows D0 and 1-11) for the
  published code and for the fixed build, with identifier file paths and the provenance answer; T1-T4 green and
  the v3 pit bridge manifest SHA-256; the indexes v2 manifest with rules `russell-proxy-v2` / `sp500-proxy-v2`,
  the four new receipt keys with measured values, the R7 options chosen, and the new test green; the B3
  acceptance prints; the B1 pilot results.
- The final whole-branch review has run and its findings are fixed or parked with rulings.
- Your last message lists every `Ruling:` line from the ledger in order, each with what it costs if wrong, and
  states plainly what is still open (Task 10, activation, any FAIL gate).

---

Short form, if a one-line goal condition is wanted:

> Finish the P9 AL-DATA atx-db request per `docs/superpowers/handoffs/2026-10-04-p9-al-data-status-1.md` and the
> plan it names: Tasks 1-9 and 11 complete and reviewed in the ledger, `atx-db/docs/p9-al-data/response.md`
> written, final review done. Subagent-driven, Opus 5.5 implementers. Task 10 and identity activation stay
> owner-gated.
