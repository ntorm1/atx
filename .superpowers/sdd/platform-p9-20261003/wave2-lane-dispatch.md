# Wave-2 lane dispatch contract (PM, W2-EARLY)

You implement ONE lane of the P9 sprint. Sprint dir (read-only for you, in the root tree):
`C:/atx-wt/pool-2/.superpowers/sdd/platform-p9-20261003/` (called ROOT-SDD below).

Read first, in this order:
1. ROOT-SDD/`briefs/brief-<LANE>.md` -- your requirements, exact values verbatim.
2. ROOT-SDD/`wave2-carry.md` -- sections "Lanes, pools", "Brief text superseded", "All lanes" and your lane's section.
   Where it and the brief disagree, the carry wins. Items marked required are required.
3. `C:/atx/CLAUDE.md` and `.agents/cpp/agent.md` before any C++.
4. Wave-1 reports your brief or carry section names (ROOT-SDD/`task-<ID>-report.md`, `root-wave1-merge-report.md`).
Never read the whole plan file or the whole ledger; grep them for an id if you must.

Workspace:
- Base = `1239a5ff` (W2-EARLY: wave-1 slots 1-7 merged; C1 / slot 8 is NOT in your base and its files are not yours).
- Reused pool (12, 13, 14, 17, 18, 19, 20): the tree must be clean (`git status --porcelain` empty, else stop and
  report); then `git switch -c feat/p9-<lane-lowercase>-20261003 1239a5ff` inside that pool. No lease call.
- Fresh pool (T2 only): from `C:/atx`, `powershell scripts\lease-worktree.ps1 -Branch feat/p9-t2-20261003 -Base
  1239a5ff -Agent p9-t2 -RunId p9-t2-20261003 -HeartbeatId p9-t2-hb -MaxPool 24`; an exit 1 "configure failed; lease
  remains held" is expected and fine (see ROOT-SDD/`pools.md`). Use the pool it names.
- Work only in your pool. Never build / switch / commit in `C:/atx` or `C:/atx-wt/pool-2`; never touch `atx-db/`;
  never push; never `git worktree add` / `prune`; open no data dated >= 2024-01-01; never edit an expected hash or
  an expected value to make a test pass.

Method:
- NO test-driven development: implement first, then write the tests the brief names.
- Lanes do NOT build C++ and do NOT run real data (root does both after merge; machine has ~1.5 GB free RAM).
  Python: run the pytest suites you touch, explicit paths only (PY-HYG), and kill your own leftover children.
  Because you cannot compile, re-read every C++ file you wrote for includes, signatures against the headers you
  call, `/W4 /WX` traps (unused, sign, shadow, narrowing), and CMake list entries.
- Edit only the files your brief / carry gives you. Any other file is a cross-lane edit: keep it to the minimum,
  append at list tails, and list it in the report.
- You dispatch no subagents and request no review. Ambiguity: choose the reading closest to existing code and the
  rulings, record it under "Decisions". Need a rule decision the carry says to ask about: stop and report.
- Commits on your lane branch, small and themed; trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

Report: `<your pool>/.superpowers/sdd/platform-p9-20261003/task-<LANE>-report.md`, committed with `git add -f`:
files changed (owned / cross-lane), what each required carry item became, contracts (K-P9-*) implemented, tests
written (names) and pytest counts run, build targets + anchored gtest filters root must run, identity checks root
must run (flag-absent byte identity), decisions, concerns, carried minors fixed / not fixed and why.

Final message to the PM (short): status DONE / DONE_WITH_CONCERNS / BLOCKED, pool, branch, base, head SHA, one-line
test summary, concerns. Nothing else.
