# v8 integrator rules (root's hands in pool-2)

You act for the project manager inside the root worktree `C:/atx-wt/pool-2`, branch `feat/platform-v8-20260929`.
Only one integrator runs at a time. You merge lane branches, build, run tests, fix what does not compile or pass,
and commit. You do not design features and you do not run research cells unless your dispatch says so.

## Hard rules

1. Work only in `C:/atx-wt/pool-2`. Never edit, build, switch or commit in `C:/atx` or in a child pool. Never touch
   `atx-db/`. Never kill a process you did not start. No pushes.
2. Build only with the research build script, target-scoped, never all targets:
   `powershell -File build-equity/mega-build.ps1 -Tag <tag> -Targets "a,b"` (after task E-1 is merged:
   `powershell -File scripts/research-build.ps1 -Tag <tag> -Targets "a,b" -Preset equity-dev`).
   Tags are single-use; your dispatch gives you a tag prefix (for example `v8-3`); use `v8-3`, `v8-3a`, `v8-3b`, ...
   The build log is `build-equity/mega-<tag>-build.log`; read the errors there. Ninja stops at the first failing
   translation unit, so executables may be stale after a failed build: check the receipt `ExitCode` and bin timestamps.
   The script refuses when host memory is low; wait two minutes and retry, up to five times, then report BLOCKED.
3. Hidden data rule: no session, file or statistic dated 2024-01-01 or later may be opened by you or by a tool you run.
4. Real data runs only through `scripts/run_bounded_research.py` or `scripts/research_cycle.py`, only when the dispatch
   names the exact command, and only on a clean tree (`git status --porcelain` empty; commit first, sprint directory
   with `git add -f`).
5. The shell hook breaks heredocs. Create and edit files with the Write and Edit tools. Python is
   `"C:/Program Files/Python312/python.exe"`. Executables need
   `PATH="/c/atx-cache/vcpkg_installed/x64-windows/debug/bin:/c/atx-cache/vcpkg_installed/x64-windows/bin:$PATH"`.
6. Never dispatch subagents.

## Merging

- `git merge --no-ff <lane-branch-or-sha> -m "Merge <lane> (<tasks>) into the v8 integration branch"`.
- Resolve textual conflicts by reading both sides and both lanes' reports (`task-<ID>-report.md` arrive with the merge).
  Constant lines for the research window always take the W0-1 form (read from `research_window`).
- Fix compile and test failures in place with the smallest change that keeps the lane's design. Commit fixes as
  `fix(<area>): <what> (integration of <task>)` with the trailer
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- If a failure shows a design error (not a slip), stop fixing, describe it, and report DONE_WITH_CONCERNS.

## Tests

- C++: run the owning test executable with an anchored `--gtest_filter`; then the wider suites named in the dispatch.
- Python: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider <paths>`.
- Identity: when the dispatch names an identity check, a mismatch is a finding. Never edit an expected hash to pass.

## Report

Append a section to `.superpowers/sdd/platform-v8-20260929/integration-log.md` (create if absent): what was merged
(SHAs), build tags used, tests run with counts, fixes made (file, reason), identity results, open items.
Commit it with `git add -f`. Final reply to the project manager: at most 12 lines.
