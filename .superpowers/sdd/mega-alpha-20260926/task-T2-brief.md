# Task T2 — NAV replay of the saved blend ($1bn, costs, causal missing-price policy)

Owner worktree: `C:/atx-wt/pool-5`, branch `feat/mega-alpha-nav-20260926` (base `5c9cbaed`).
Requirements: `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/nav-backtest-design.md` — read it
first; it is the approved design with exact file:line reuse points, equations, scenarios, CLI, output
schema and fixture list. Implement it as written, with the root rulings below.

Files you own: `atx-impl/src/strategy_target_replay.cpp` (append-only `detail::` wrappers + optional
volume load; no arithmetic change to existing replay), new `atx-impl/src/strategy_target_replay_detail.hpp`,
new `atx-impl/src/strategy_nav_replay.hpp/.cpp`, `atx-impl/tools/equity_strategy_targets.cpp`, new
`atx-impl/tests/strategy_nav_replay_test.cpp`. Root owns CMake: list the CMake lines needed in your
report (new CPP into `atx-impl-core` + the Debug `/O2 /Ob2 /clang:-finline` + `SKIP_PRECOMPILE_HEADERS`
list; new test TU into `atx-impl-strategy-target-tests`), do not edit CMake yourself.

## Root rulings on the design
- Scenarios S1/S2/S3 exactly as designed; S2 `modeled-1bn-stale5-v1` is primary. Borrow stays 300 bps.
- K = 5 stale-carry write-off primary; S3 K=1 adverse. No lookahead anywhere.
- Rules: baseline-v1 and monthly-budget-v2 only (no new turnover levers in this task).
- Keep the `replay_nav` core pure (no I/O) so fixtures call it directly.
- Also emit in summary, per scenario, `turnover_definition` text and both `mean_monthly_one_way_turnover`
  (execution month, all months) and `mean_monthly_one_way_turnover_ex_deployment_month`.
- Add a stable extension point for a later task: `desired_target` is called through one function in
  the NAV path (e.g. `plan_decision`) so a later neutralization step can post-process the desired
  target vector in one place. Do not implement neutralization.
- The existing 7 target-replay fixtures must remain valid and unchanged.

## Constraints
- House style: read `C:/atx-wt/pool-5/.agents/cpp/agent.md` first. Private CPPs, light headers.
- Do NOT compile or run: root alone builds and runs real data. Write careful code; root compiles and
  returns errors to you.
- Not TDD: production first, then the postimplementation fixtures in design section 10.
- Memory: O(N) per-name state + one loaded blend/role; admission accounting as designed.
- No subagents. Commit in pool-5 (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
Full report to `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T2-report.md` (files,
file:line of key logic, CMake lines root must add, fixtures, open questions, commit SHAs). Return only:
status, commit SHAs, one-line summary, concerns.
