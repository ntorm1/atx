# Integration 6 part C brief (root, pool-2; one integrator at a time)

Read first, binding: `.superpowers/sdd/platform-v8-20260929/integrator-rules.md`. Build tag prefix: `v8-11`
(`v8-11`, `v8-11a`, ...). Build script: `powershell -File scripts/research-build.ps1 -Tag <tag> -Targets "a,b"
-Preset equity-dev`. Merge by commit SHA, never by branch name. Root code head at dispatch: `9c5cfa0c` (build
`v8-10`); the two lane SHAs are in your dispatch message. Context if you need it: `progress.md` from "re-review at
integration 6 part B and fix round FIX-4" down (rulings PM4-7..PM4-12), `task-FIX-4-brief.md`, and the
"integration 6 part B" section of `integration-log.md` (argv of the identities, suite commands).

## 1. Merges (in this order, each `--no-ff` by SHA)

| lane | report (arrives with the merge) |
|---|---|
| FIX-4a | `task-FIX-4a-report.md` (O-1..O-7, C-1, C-5) |
| FIX-4b | `task-FIX-4b-report.md` (C-2, C-4, S-1, S-2) |

Known overlaps (resolve by reading both sides and both reports; keep every lane's behaviour):
- `scripts/research_cycle.py`: FIX-4a (O-5, parent weights passed to the theme-resid fit) and FIX-4b (C-4, the
  E-27b guard on parsed argv in `validate_v8_keys`). Union.
- `scripts/tests/test_research_spec.py`: union of both lanes' tests.
- FIX-4a's history contains a WIP commit `b8b68f4e` (saved by the PM at the owner stop); the lane finished the
  finding in a later commit. Merge the lane head; do not cherry-pick around the WIP commit.

## 2. Build

Target-scoped, every target whose sources changed: the IC runner executable (`strategy_ic_theme_resid`,
`strategy_ic_admission.cpp`), the NAV executable only if a NAV / spo source (not a test) changed, and the owning
test targets the two reports name (expected: `atx-impl-strategy-ic-tests`, the target that owns
`strategy_spo_v3_test.cpp`, `atx-impl-tests`). The lane C++ has never been compiled: fix compile errors in place
(smallest change, lane design kept, `/W4 /WX`). A design error is reported, not patched over.

## 3. Tests

- The gtest filters each report names under "what root builds / verifies".
- Every C++ suite. Baselines at `9c5cfa0c`: target 256, book 155, strategy 46, ic 139, mine 18, factory 387,
  combine 233, impl 1003 / 5 skipped / 1 known failure (`ConfigJsonNotInDiscoverDigest`). Counts may only grow by
  the lanes' tests. Suites whose target was not rebuilt need not be re-run; say which.
- Every Python suite. Baselines: strategies 163 + 9, engine tools 253, impl tools 529 / 1 skipped, scripts 178 /
  3 skipped.
- `tiny_world`: no golden moves. The spo-v2 pin (`SpoPin.*`, `3bfd293e`) must hold; never edit a pin or an
  expected hash.
- A test that fails because the lane's premise was wrong is fixed in the test only when the registered rule is
  still asserted exactly; otherwise report it.
- R6B-S-1 closes the open item "E-31a void exit path untested end to end": state explicitly whether the CLI void
  test (exit 3, `status: void`, `voided: limits_unmet`, no NAV or returns file) ran and passed on the built
  executable.

## 4. Identities (bounded runner; clean tree; 3-year role and existing caches only)

- Identity 4 (composition v8 re-rank and cap off against the v7 composition), argv as in "integration 6 part B".
  Expected: data files identical; the weights file may differ only in `module_sha256` / `script_sha256` if a
  hashed Python module changed. Anything else is a finding.
- Identity 1 (NAV with every new flag absent against `build-equity/v7-w4-holdings`) ONLY if a NAV source
  (`strategy_nav_v7*`, `strategy_spo*`, target or book sources; not tests) changed in either lane. Say which
  case applied and why.
- A mismatch is a finding: stop, record the first differing file, do not continue. Hidden-data rule absolute.
  Do not run Wave 0 steps and do not run any cell.

## 5. Report

Append the section "integration 6 part C" to `integration-log.md`: merges (SHAs, merge commits, conflicts and how
resolved), build tags and compile fixes (file, reason), every suite count, gtest filters and results, identities
(digests), the diff range for the scoped review (`<part-B head>..<part-C head>` restricted to code), open items.
Commit with `git add -f`; tree clean at the end. Final reply, at most 12 lines: head SHA, code head SHA, last
build tag, suite counts, number of compile / test fixes, identity results, anything that looks like a design
error.
