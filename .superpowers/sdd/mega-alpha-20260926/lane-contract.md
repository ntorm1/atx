# Lane contract — mega-alpha sprint (read before any work)

You are a child agent in the mega-alpha sprint. Authoritative objective: handoff
`C:/atx-wt/pool-2/docs/plans/2026-09-26-mega-alpha-parent-handoff-2.md` §2 / §2a / §2b (read only the
sections your brief points to; do not read the whole ledger).

## Deliverable (what every task is judged against)

A real out-of-sample NAV backtest of a mega-alpha: many atx-engine alpha-DSL subalphas admitted, signed
and weighted on TRAIN (2020-2022) only, combined into one market-neutral book, **rebalanced daily
(`--cadence 1`)**, simulated at $1bn NAV with S2 costs and `swap-fin-v1` financing, frozen, then run once
on validation (2023-2024). 2025+ reserved.

Declared limits (do not relax/tighten after seeing results):
- NET Sharpe >= 1 (S2 costs + `swap-fin-v1`, excess returns, 252 sessions/yr) on validation.
- Combined book daily one-way turnover `tau_t = sum_i |fill$_{i,t}| / GMV_t` (GMV = long$+short$
  pre-trade): mean <= 0.20, p95 <= 0.30, deployment session excluded, forced exits count.
- Individual alpha standalone `tau_k` <= 0.70/day (neutralized gross-1 book, daily full rebalance, TRAIN).
- Netting ratio `NR = tau_book / sum_k w_k tau_k` reported on TRAIN.
- 30%/month turnover target is RETIRED (report monthly for continuity only; never a flag/constraint).

## Rules for implementers

- Work ONLY in the worktree named in your dispatch. Inspect `git status` / `git log -3` first.
  Never touch `C:/atx` (read-only inspection okay) or any other worktree. No pushes.
- **Never build** (no cmake/ninja/atx-build.ps1/mega-build.ps1) and never run real data. The root
  controller builds, runs and tests after cherry-picking your commits. You may run pure-Python
  synthetic fixtures (pytest / `python -m`) if they need no build and no real data.
- Not TDD. Implement, then write focused postimplementation fixtures (GoogleTest for C++, pytest for
  Python). Because you cannot build, desk-check C++ carefully: includes, signatures, const-correctness,
  every call site of anything you change. List in your report exactly which test targets/suites the
  root must build and run, and any new source/test files that need CMake registration (say whether you
  already registered them in CMakeLists.txt).
- C++: read `C:/atx-wt/pool-2/.agents/cpp/agent.md` (house style, safety rules) before editing C++.
- Never dispatch subagents (no helpers, no reviewers). Review is scheduled by the controller.
- Commit your work in your worktree (conventional commit subject, ending line
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`). Several small commits are fine.
- Write the full report to the report file named in your dispatch (append a new `## Fix round N`
  section on fix rounds; never delete earlier sections). Then reply with ONLY (<15 lines):
  Status (DONE | DONE_WITH_CONCERNS | BLOCKED | NEEDS_CONTEXT), commits (short SHA + subject),
  one-line test summary, concerns, report path, and the exact root build targets + test filter.
- If blocked or the brief is ambiguous in a way that changes the result, say so specifically rather
  than guessing silently. Small ambiguities: decide, and record the decision in the report.
- Selection hygiene: anything that chooses signs/weights/admission/construction uses TRAIN only.
  Never read or compute on validation (2023-2024) or 2025+ data.

## Rules for reviewers

Read `reviewer-contract.md` (task review) or `re-review-contract.md` (scoped re-review) in this directory
for method and output format. Inputs are given as file paths in your dispatch. Read-only: never edit,
commit, build or run real data. Write your full review to the review file named in your dispatch and
reply with only the verdict lines (spec ✅/❌, quality verdict, counts of Critical/Important/Minor, and
the review path).
