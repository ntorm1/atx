# Integration 7 brief (root, pool-2; one integrator at a time; Ruling PM5-6)

Read first, binding: `.superpowers/sdd/platform-v8-20260929/integrator-rules.md`. Build tag prefix: `v8-12`
(`v8-12`, `v8-12a`, ...). Build script: `powershell -File scripts/research-build.ps1 -Tag <tag> -Targets "a,b"
-Preset equity-dev`. Merge by commit SHA. Root head at dispatch is in your dispatch message; code head `41ef00fb`
(build `v8-11`). Context: `progress.md` entries PM4-1, PM4-2, PM4-13, PM5-6, E-32, E-32a, E-33; the lane report
`task-MINE-FIX-report.md` (arrives with the merge; main part and "Round 1"); `review-mine.md` (the findings the
lane fixed); the "integration 6 part B" section of `integration-log.md` (argv of identities 1, 4, 7, 8 and the
suite commands).

Why now (PM5-6): MINE-FIX changes an engine header (`ResearchIcRead.ic_dates`, `search_driver.hpp`) and
`strategy_research_role.{hpp,cpp}`, so it moves executables. No spec is locked yet; Wave 0 part 2b writes the
locks right after you. This is the last point before the freeze gate where the executables may change.

## 1. Merge

`git merge --no-ff 20e7bd19 -m "Merge MINE-FIX (MINE-1..12, 17, 18, PM4-13) into the v8 integration branch"`.
Known overlap: the lane lowered FIX-C's E-33 test budget 2048 -> 1000 (cross-lane edit, ruled PM4-13); other
conflicts are resolved by reading both sides and the report, keeping both behaviours.

## 2. Build (one tag for every research executable)

The engine header change is a wide rebuild. Build, under ONE tag where memory allows (else consecutive tags, and
say so): every research executable (IC runner, NAV, targets, risk, mine and any other `atx-equity-*` tool the
cells use), `atx-impl-strategy-mine-tests`, `atx-engine-factory-tests`, and every test target whose sources or
headers changed (`atx-impl-strategy-ic-tests`, `atx-impl-strategy-target-tests`, `atx-impl-tests`, the engine
book / combine tests if they include the changed header). After the build every research executable must carry
this integration's tag: list each executable with its receipt digest in the log (Wave 0 part 2b pins them).
The lane's C++ has never been compiled: fix compile errors in place (smallest change, lane design kept,
`/W4 /WX`). A design error is reported, not patched over.

## 3. Tests

- The mining determinism golden: the registry chain head `0x889874a3b9b29c55` on the fixture must hold at 1
  worker and at 4 workers. Never edit the golden. If it does not hold: find the cause. A slip (a test premise,
  an include, an obvious one-line error that leaves the lane's design intact) is fixed in place. Anything else:
  STOP, revert the merge (`git revert -m 1 <merge>`), rebuild the executables under a new tag so root is back
  on part-C behaviour, record the first differing registry entry, and report (PM4-2's timing then returns).
- The gtest filters the MINE-FIX report names, then every C++ suite. Baselines at `41ef00fb`: target 259, book
  155, strategy 46, ic 145, mine 18, factory 387, combine 233, impl 1012 / 5 skipped / 1 known failure
  (`ConfigJsonNotInDiscoverDigest`). Counts may only grow by the lane's tests.
- Every Python suite. Baselines: strategies 163 + 9, engine tools 253, impl tools 567 / 1 skipped, scripts 182 /
  3 skipped.
- `tiny_world`: no golden moves. The spo-v1 / spo-v2 pins hold. Never edit a pin or an expected hash.

## 4. Identities (bounded runner; clean tree; 3-year role and existing caches only)

Every executable was rebuilt, so re-run identities 1, 4, 7 and 8 with the argv of "integration 6 part B":
1 NAV with every new flag absent against `build-equity/v7-w4-holdings`; 4 composition v8 re-rank and cap off
against the v7 composition (expected as in part C: data files identical, timing fields only); 7 spo-v2 side files
against the pinned v7 side files; 8 `--label-role` equal to `--role` against flag absent. A mismatch is a
finding: stop, record the first differing file, do not continue. Hidden-data rule absolute. Do not run Wave 0
steps and do not run any cell. Do not run a mining campaign on real data (fixture only).

## 5. Report

Append the section "integration 7" to `integration-log.md`: the merge (SHA, merge commit, conflicts), build tags
and compile fixes (file, reason), the executable list with receipt digests, the golden at 1 and 4 workers, every
suite count, identities (digests), open items. Commit with `git add -f`; tree clean at the end. Final reply, at
most 12 lines: head SHA, code head SHA, last build tag, golden at 1 and 4 workers, suite counts, number of
compile / test fixes, identity results, whether every research executable is on one tag, any design error.
