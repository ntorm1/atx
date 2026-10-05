# P9 wave-3 lanes dispatched in RESUME 6: common part (PM, 2026-10-04)

Every lane agent of dispatch R6-3 reads this after `resume4-dispatch.md` and `resume6-addendum.md`. Where this file and
an older one differ, this file wins; a ledger "Ruling" line wins over everything.

## Read order

1. `resume4-dispatch.md`, `resume6-addendum.md`, this file.
2. Your brief `briefs/brief-<ID>.md` (if one exists) and `wave3-carry.md`: "Brief text superseded" for your brief,
   "All lanes", your lane's section, "PM answers to Q1-Q18", "Added after drafting". Brief vs carry: the carry wins.
3. The ledger `progress.md`: grep your lane ID and every ruling id your dispatch prompt names; read the block from the
   line starting "- RESUME 6" to the end.
4. Your lane's handoff / report / review files, if your prompt names them.

## How wave 3 runs now (ruling W3-EARLY-2; the owner asked for more lanes in parallel)

- You may start before your dependencies are merged in root, on a dependency head that is APPROVE with 0 Required.
  Never rebase; `git merge` only. All pools share one repository, so `git merge feat/platform-p9-20261003` merges
  root's current head, and `git merge <sha>` merges an approved lane head.
- First step: merge root's head (and the approved heads your prompt names). Last step before your report: merge root's
  then-current head again, re-run your suites on the merged tree, and state in the report which root SHA you merged.
  You resolve conflicts in YOUR hunks; you do not redesign another lane's code. List every conflict and its resolution.
- An item that sits in a file a still-running lane is rewriting is done LAST, after that lane's merge is in root
  (`git -C C:/atx-wt/pool-2 log --format=%s -20` shows the merges). If it is not in root when the rest is done, do not
  do the item: hand it off as pending in your report (a short follow-up round does it).
- Several lanes run at once (C3b, D3, AL-CLOCK, COV-MV, SQL3, OD3P, B2). Stay inside your files. A cross-lane edit is
  the smallest append that works (one function, one call line, one table row at the end) and is listed by file and hunk.

## Build, test, host (ruling HOST-2)

- Presets: `equity-dev` (Debug, tree `build-equity`) and `equity-rel` (Release, tree `build-equity-rel`). Configure only
  as `powershell scripts\atx-build.ps1 configure -Preset <preset>`; build only through `scripts/research-build.ps1`,
  target-scoped, single-use tag `p9-<lane>-<letter>` (W2-BUILD in `resume-rulings.md`; any `mega-p9-*-receipt.json` shows
  the form). One build on the host at a time: when the memory gate refuses, wait and retry; the bound is 90 minutes in
  all per build; a refusal is not a stop. Root's builds have priority. Known trap: a stale-PCH ccache failure on a
  Release build is not a code red (`CCACHE_RECACHE=1`, as earlier lanes did).
- Fresh pools (8, 13, 21): the lease's one-time `dev` configure failed (the `dev` preset names directories this branch
  does not have; a known owner item). Ignore the `build/` directory it left. Use `equity-dev` / `equity-rel`.
- Disk: check free space on C: before each build. Below 12 GB free: one configuration at a time, and delete your own
  previous object tree by name first. Below 6 GB: stop and report.
- No TDD: implement, then the tests your brief, the carry and your prompt name. Every new refusal gets a test with
  teeth (the test fails with the check removed: a working-tree mutation you revert; if the permission system refuses a
  mutation edit, do not retry it; say so).
- Pytest: explicit paths, `-p no:cacheprovider`, own `--basetemp`, `PYTHONDONTWRITEBYTECODE=1`. If you changed Python,
  or anything a Python test executes: the whole `scripts/tests` under `PYTHONHASHSEED` 0 and 1, and
  `atx-engine/tools/test_stage_chain.py`, in SEPARATE invocations. Root's known-red set is EMPTY at `c8ddf736`: every
  red is yours to explain. ENV-1 covers exit code 3221225794.
- Synthetic and fixture data only. No real role, no 2020-2023 statistic, nothing dated 2024-01-01 or later, never
  `atx-impl/tools/holdout_gate.py`. The trial ledger in pool-2 is not opened for writing by any lane.

## Records and hand-back

- `task-<ID>-report.md` and `handoff-<ID>.md` in YOUR pool's sprint dir (`git add -f`), in the "Lane report" form of
  `.agents/harness/TEMPLATES.md`. The report lists: each required item -> change (file:line) -> test -> evidence log;
  optional items done / not done; new `atx.<name>/v<n>` literals, flags, JSON keys, rule ids; cross-lane edits; CMake
  tail appends; the flag-absent identity procedure for root (argv, files expected byte-identical, substitution list);
  what root re-proves at merge; open questions one line each.
- Commit trailer exactly `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Then delete by name your own build
  object trees (keep receipts and logs) and scratch. Leave the tree clean. Do not touch leases.
- If you run low on context: commit what is done, write the handoff (done / not done / next step), report
  DONE_WITH_CONCERNS. A fresh agent continues from the handoff.
- Final message to the PM, under 200 words: STATUS, head SHA, root SHA merged, build tags, tests with pass / fail
  counts, items pending, files written, disk freed, any permission refusal, open questions one line each. No logs.
