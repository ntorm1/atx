# W0 lane rules (binding for every W0 implementer, reviewer and fixer)

Issued by the orchestrator for the alpha-engine swarm, wave W0 (plan:
`docs/plans/2026-09-24-alpha-engine-production-swarm.md` §5, §7 W0; defect register:
`docs/plans/2026-09-24-alpha-engine-review-findings.md`). Owner rulings and overrides:
`docs/plans/2026-09-24-alpha-engine-swarm-goal-prompt.md` §2-§3.

## 0. Hard safety rules (a breach is a lane failure)

- `C:\atx` is **another session's live checkout** (branch `feat/tier1-parity`, running atx-db
  migrations). Never run git in it, never build in it, never Edit/Write any path under `C:\atx\`.
  Read the plan docs from your own pool (`docs/plans/...`), not from `C:\atx`.
- **The shell cwd resets to `C:\atx` after every tool call.** Therefore:
  - every git command is `git -C C:\atx-wt\pool-N ...` (your pool, always explicit);
  - every build/test command starts with `Set-Location C:\atx-wt\pool-N;` in the **same**
    PowerShell call (the wrapper also has a wrong-tree guard);
  - every Read/Edit/Write path starts with `C:\atx-wt\pool-N\`.
- Never touch another pool's files. Never run `lease-worktree.ps1`, `git worktree ...`,
  `git push`, `git stash`, `git rebase`, `git reset --hard`, `git branch -D`, or anything that moves
  `main` or `feat/w0-integration`. Only commit to your own lane branch `feat/w0-<lane>`.
- Never kill a process you did not start. Never open `atx-db/data/warehouse.duckdb`.
- **Data discipline:** W0 lanes are RAM:light and use synthetic fixtures only. Do not read real
  data artifacts under `C:\atx\data\`, and never anything dated >= 2020-01-01. (G0 real-data
  re-runs are the orchestrator's job.)

## 1. Build and test loop

- Preset **`equity-dev`** (binary dir `build-equity\`; test exes in `build-equity\bin\`). The `dev`
  preset is broken on this base (atx-vol was removed) — never use it.
- RAM: the host is 16 GB, shared with ~6 lanes and another session. In **every** build call set
  `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'`. Before a `build`, check free RAM
  (`(Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1MB` GB); if < 2.0 GB, `Start-Sleep 60`
  and re-check (at most 20 times), then report BLOCKED-host rather than build.
- Commands (run from your pool, one PowerShell call each):
  ```powershell
  Set-Location C:\atx-wt\pool-N; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 configure -Preset equity-dev -Groups "<g1;g2>"   # ONLY if your groups differ from build-equity\CMakeCache.txt ATX_TEST_GROUPS
  Set-Location C:\atx-wt\pool-N; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 check -Preset equity-dev atx-engine\src\<file>.cpp
  Set-Location C:\atx-wt\pool-N; $env:CMAKE_BUILD_PARALLEL_LEVEL='2'; powershell -NoProfile -File scripts\atx-build.ps1 build -Preset equity-dev <owning-test-target>
  Set-Location C:\atx-wt\pool-N; powershell -NoProfile -File scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'
  ```
- Never build all targets, never `ctest -L <label>`. You may run your owning target's whole gtest
  executable directly (`build-equity\bin\<target>.exe --gtest_brief=1`) to prove "must stay green";
  add `atx-shm-worker` to the build line for targets that spawn it (atx-impl-tests, parallel).
- Warnings are errors (`/W4 /WX`). 100-column limit, match surrounding style. Do not run
  clang-format or clang-tidy.

## 2. Code rules (house style `.agents/cpp/agent.md`, with the owner's overrides)

- **No TDD (owner override).** Implement first, then add tests that prove **every** acceptance
  item. Tests remain mandatory. Never weaken, skip, disable or delete an acceptance test or an
  existing test to go green. Changing an existing test's expectation is allowed only where that
  test pins a cited defect — list each such change in the report with its defect ID.
- **Owned files only** (listed in your brief). Anything needed elsewhere goes into the report's
  "Integration notes" section. New test files are always allowed in the right folder:
  `atx-engine/tests/<group>/<group>_w0<lane>_<topic>_test.cpp` (prefix must match the folder) or
  `atx-impl/tests/w0<lane>_<topic>_test.cpp`.
- **No `CMakeLists.txt` edits and no new `.cpp` under `src/`** (engine sources are listed
  explicitly in CMake and W0 has no Lane-0 stubs). New headers the plan names (e.g.
  `eval/hac.hpp`, `eval/trial_clusters.hpp`) are header-only or implemented in an owned `.cpp`.
  (Exception: lane O1 is W0's scaffold lane and may edit CMake/preset files for its own items.)
- Test helpers live in `namespace atx_test_w0_<lane>_<file>`. Suite names use the prefixes your
  brief lists (lane-unique).
- Any changed numeric default keeps the old behaviour reproducible behind a **versioned enum**
  (e.g. `RankTies::OrdinalV1`, `BlockLenRule::V1`); defaults move to the corrected behaviour.
- Golden-digest re-baselines: record an old→new table in the report, each row tied to a cited
  defect ID.
- No UB, no narrowing, no uninitialised variables, bounded loops, `[[nodiscard]]`/`const`/
  `noexcept` where they hold, validated inputs, contracts documented (agent.md §10 checklist).

## 3. Git

- Commit early and often on `feat/w0-<lane>`. Message: `w0-<lane>: <what>` and end the body with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`.
- `.superpowers/` is gitignored but tracked: add sdd files with `git -C <pool> add -f <path>`.
- Leave the tree clean (everything committed) at the end of every stage.
- **Pre-merge before review:** `git -C C:\atx-wt\pool-N merge --no-ff feat/w0-integration`,
  resolve conflicts only inside owned files (anything else: `git merge --abort` and report),
  rebuild, re-run your suites.

## 4. Report — `.superpowers/sdd/w0/lane-<lane>-report.md` (committed on the lane branch)

Sections (TEMPLATES.md "Lane report" plus):
- Outcome (DONE | BLOCKED), branch + final SHA, base SHA, pool.
- Files changed.
- **Acceptance table:** each plan acceptance item → test name(s) → measured result → MET/UNMET.
- **Defect table:** each cited ID → CLOSED (how, where) | DEFERRED (why, to which lane).
- Evidence: exact commands, exit codes and verbatim output tails (test counts) for every claim.
- Golden-digest old→new table (or "none").
- Deviations from brief; Integration notes; Ledger candidates (≤ 3 one-line facts).
Report honestly: an UNMET item stays UNMET (only the owner can waive it).
