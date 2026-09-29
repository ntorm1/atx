# v8 lane rules (every child lane reads this first)

You are one implementer lane of the atx platform v8 sprint. The project manager (root) works in `C:/atx-wt/pool-2`.
You work only in the pool worktree and branch named in your dispatch. Your brief is your requirements.

## Hard rules

1. Work only inside your own worktree. Never edit, build, switch or commit in `C:/atx` or in another pool.
   Never touch `atx-db/`. Never kill a process you did not start.
2. Never build C++ (no cmake, ninja, `atx-build.ps1`, `mega-build.ps1`). The host has 3.5 GB of free memory and root
   serialises every build. You write C++ that compiles first time: read the owning files fully, copy the local idiom,
   respect `/W4 /WX` (no unused variables, no sign conversions, no shadowing), and read `.agents/cpp/agent.md` first.
3. Never run real data. `C:/atx-wt/pool-2/build-equity/` may be read for file formats, manifests and receipts only.
   Hidden data rule: no session, file or statistic dated 2024-01-01 or later may be opened, by you or by code you run.
4. Never dispatch subagents. Never push. No warehouse writes, no broker actions.
5. Python: you may and should run pytest on synthetic data for the tests you write:
   `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider <test files>`.
6. The shell hook rewrites commands and breaks heredocs. Create and edit files with the Write and Edit tools.

## Way of working (owner directive, 2026-09-29)

- No test-driven development. Implement first. Then write the tests named in the brief (they are the acceptance
  contract) plus what is needed to pin behaviour. Do not run a red-green cycle.
- No review agent follows each task. Your own self-review is the gate until the wave review. Be strict with yourself.
- Speed matters, but the code must be modular and reusable: small focused files, clear interfaces, no copy-paste of
  logic blocks, generic machinery in `atx-engine`, strategy-specific code in `atx-impl`.
- Identity discipline: every platform change is behind a flag or is provably value-preserving. Accepted outputs must
  stay byte-identical when the new flag is off. State in the report how root can verify it.
- The research window comes from one source (task W0-1): `atx-engine/tools/research_window.py` (Python) and
  `atx/engine/data/research_window.hpp` (C++). TRAIN is [2020-01-01, 2024-01-01); seal at 2024-01-01. Never hard-code
  a TRAIN end or seal date. If W0-1 has not reached your branch yet, import it as specified in the W0-1 brief
  (`task-W0-1-brief.md`) and root will merge.
- Stay inside the files your brief names. If you must touch another lane's file, make the smallest edit and list it
  under "Cross-lane edits" in the report.
- Do the tasks of your lane in the order given. Commit after each task: `git add <your files>` then
  `git commit` with a conventional message ending with the trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Report

Write `.superpowers/sdd/platform-v8-20260929/task-<ID>-report.md` in YOUR worktree for each task and commit it with
`git add -f`. Content: what was built (files, interfaces as coded), how root verifies (exact build targets, gtest
filters, identity runs with argv), deviations from the brief with reasons, cross-lane edits, open risks.

Your final reply to the project manager is short: status (DONE, DONE_WITH_CONCERNS, BLOCKED), commit SHAs per task,
one line of test results, concerns. No long prose.
