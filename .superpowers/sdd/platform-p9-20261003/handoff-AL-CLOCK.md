# Handoff AL-CLOCK (lane AL-CLOCK, pool-8, branch `feat/p9-alclock-20261004`)

Resume R8 (`r8-dispatch.md` "AL-CLOCK"). Contract: `resume4-dispatch.md`, `resume6-addendum.md`, `r6-wave3-common.md`,
`resume7-addendum.md`, `resume8-addendum.md`; rulings ALCLOCK-1 / -2 / -3, W3-9, W3-16, C3-5 / C3-13, PRE-7a, DEC-16.
Report: `task-AL-CLOCK-report.md` (same dir). The design text of the earlier handoffs is in git history
(`git show ebaf1e3f:.superpowers/sdd/platform-p9-20261003/handoff-AL-CLOCK.md`, and at `120f6a25`).

Continued in pool-30 on `feat/p9-alclock2-20261007` (TEETH-POOL); pool-8 frozen for the owner. The section "State at
the stop" below is pool-8's, as it stood at STOP 8 (copied by plain `cp`; only the HEAD placeholder filled).

## State at the stop: BLOCKED by a permission refusal (PERM-1)

- HEAD `1cbe5233` holds all lane code, tests, registration and template (committed); root `fd084e78` merged
  (`e40ed78c`, clean).
- The WORKING TREE IS NOT CLEAN: six source files carry the teeth set 1 mutations (uncommitted):
  `atx-impl/src/book/nav_cli.cpp`, `nav_decide.cpp`, `nav_load.cpp`, `nav_lockstep.cpp`, `nav_validate.cpp`,
  `atx-impl/src/strategy_nav_v7.cpp`. The exact diff is `alclock-logs/teeth-set1.diff` (108 lines; made by
  `alclock-logs/tools/teeth.py 1`). The teeth build ran and every mutation was caught (report "Teeth").
- The revert was REFUSED by the permission system, as one command:
  `cd C:/atx-wt/pool-8 && git checkout -- atx-impl/src/book/nav_validate.cpp atx-impl/src/book/nav_lockstep.cpp atx-impl/src/book/nav_load.cpp atx-impl/src/book/nav_decide.cpp atx-impl/src/book/nav_cli.cpp atx-impl/src/strategy_nav_v7.cpp && rtk proxy git status --short && "C:/Program Files/Python312/python.exe" .superpowers/sdd/platform-p9-20261003/alclock-logs/tools/teeth.py 2 && rtk proxy git diff`
  Not retried; no same-effect substitute (git restore / stash / Edit-tool reversal) was used (PERM-1). Everything
  that needs the reverted tree stopped there.
- Build tree `build-equity` (equity-dev, configured, warm) is KEPT for the resume (a fresh agent saves ~25 minutes);
  `bin/atx-impl-strategy-target-tests.exe` is the TEETH exe of `p9-alclock-f` (mutated): never evidence. The canary
  exes in `bin/` (`atx-equity-strategy-targets`, `-ic`, `atx-research-eval`) are `p9-alclock-b`'s (source `c3f3f1f8`,
  clean; SHA-256 in `mega-p9-alclock-b-receipt.json`). `build-equity-rel` does not exist (Release not configured).

## Items

| item | status |
|---|---|
| merge root | done: `c3f3f1f8` (root `1b64ce45`), `e40ed78c` (root `fd084e78`), both clean |
| compile the WIP (16 C++ files) | done: clean under /W4 /WX at `p9-alclock-b` (no slip) |
| C++ test file + CMake tail | done: 13 tests; Debug `p9-alclock-e` (pre-merge tree): `InfoClock*` 13 / 13, named 24 / 24, whole target-tests 403 / 403 |
| `rule_flags` pytest | done: `scripts/tests/test_info_clock_rule.py` 5 / 5 |
| registration + template | done |
| teeth set 1 | done (`p9-alclock-f`): 7 tests fail as intended, 6 pass; NOT reverted (refused) |
| teeth set 2 (ALCLOCK-3 over-loosened) | NOT done (needs the reverted tree) |
| Debug canary | goldens match on `p9-alclock-b`'s exes; one first-run wall miss (16.0 s, limit 15 s; C3-14). R9: `p9-alclock-h` canary 15 / 15 |
| Debug build of the merged head, named + whole on it | NOT done (needs the reverted tree) |
| Release (configure, build, named, whole, canary) | NOT done (needs the reverted tree) |
| `scripts/tests` seeds 0 / 1, stage chain | stage chain 10 / 10 at seeds 0 and 1; `scripts/tests` seed 0 ran on the teeth-mutated tree with 81 ENV-1 hits (void). R9: deferred to the gate (TEST-1) |
| report | R8: committed with placeholders; R9: filled (pool-30) |

## Tools (lane-local, `alclock-logs/tools/`)

- `build.sh TAG PRESET TARGETS` (`RECACHE=1`, `CANARY=1`): waits for no other build on the host and the memory floor
  (90 minutes), then `scripts/research-build.ps1`; copies receipts / logs to `alclock-logs/`, one line in `builds.txt`.
- `gt.sh LABEL EXE LOG FILTER`, `pyt.sh LABEL SEED BASETEMP LOG -- args`: root's wrappers with lane paths.
- `teeth.py 1|2`: the teeth mutations (exact text replacements, CRLF-aware).

## Next step for a fresh agent (after the PM / owner restores the six files to HEAD in an accepted form)

1. Check `git status --short` is empty and `git diff HEAD` is empty (the six files equal HEAD's).
2. Teeth set 2: `teeth.py 2`, `bash alclock-logs/tools/build.sh p9-alclock-g equity-dev atx-impl-strategy-target-tests`,
   `gt.sh g-teeth <exe> <log> 'InfoClockRate.CapacityCurveAcceptsThisRateOnly'` (expect FAILED: per-name-v1 with the
   capacity curve and aim-partial-v6 with this rate both accepted), then restore the file in the accepted form. If
   that restore is not available, skip set 2 and say so (set 1's M10 already shows the refusal line has teeth).
3. Merge `feat/platform-p9-20261003` if root moved.
4. `CANARY=1 bash alclock-logs/tools/build.sh p9-alclock-h equity-dev "atx-impl-strategy-target-tests,atx-equity-strategy-targets,atx-equity-strategy-ic,atx-research-eval"`;
   `gt.sh` named filter `InfoClock*:NavSpec.*:NavRules.*:TradeRateRegistry.*:NavAnnMeanGross.*:CostLawRegistry.*:TargetRegistry.*`
   and `'*'` on `build-equity/bin/atx-impl-strategy-target-tests.exe`.
5. `powershell scripts\atx-build.ps1 configure -Preset equity-rel` (the documented form, from pool-8); then
   `RECACHE=1 CANARY=1 bash alclock-logs/tools/build.sh p9-alclock-i equity-rel "<the four targets>"`; named + whole
   on `build-equity-rel/bin/atx-impl-strategy-target-tests.exe`.
6. If root moved after step 3: `scripts/tests` seeds 0 and 1 and `atx-engine/tools/test_stage_chain.py` again
   (separate invocations, `pyt.sh`).
7. Fill the report's `@@...@@` lines, commit report + handoff + `alclock-logs/` (`git add -f`), then delete
   `build-equity` and `build-equity-rel` object trees by name (keep `mega-*` receipts and logs, already copied to
   `alclock-logs/`).

## R9 (pool-30, `feat/p9-alclock2-20261007`; TEETH-POOL, TEST-1)

State: DONE_WITH_CONCERNS, working tree clean, handed back for the first review (BASE per REV-BASE:
`git merge-base <HEAD> feat/platform-p9-20261003`). No code changed in R9; lane code head `1cbe5233`.

| step | result |
|---|---|
| copy the pool-8 records, DIRTY-1 / TEETH-POOL commit | `3cc8a356` (handoff, report, `alclock-logs/` incl. `tools/`, `teeth-set1.diff`; tools repointed to pool-30) |
| configure `equity-dev` | done twice (before and after the merge, so the baked provenance is the merged head) |
| teeth set 2 | SKIPPED: `teeth.py` does not keep / restore the bytes itself (TEETH-RESTORE) |
| merge root `685e238b` | `adf1c6cb`, docs only, no conflict |
| Debug `p9-alclock-h` (`adf1c6cb`) | exit 0; canary 15 / 15 (built with `-Canary` and the four targets: started before TEST-1 arrived) |
| named filter on `p9-alclock-h` | 24 / 24 (`gt-h-named.log`) |
| `test_info_clock_rule.py` seeds 0 / 1 | 5 / 5, 5 / 5; stage chain 10 / 10 at both seeds (before TEST-1) |
| Debug whole, Release leg, whole `scripts/tests` | deferred to the gate (TEST-1) |
| object trees | pool-30 `build-equity` object tree deleted by name after the records commit (tracked `audits/`, `v8-interim3-pitch-render-run2/` kept) |

Next step for a fresh agent: none in the lane. Owed elsewhere: the gate runs the deferred legs (TEST-1); teeth set 2
only if the PM asks (needs a restore-capable `teeth.py` and one target-tests build). Pool-8 stays frozen for the owner.
