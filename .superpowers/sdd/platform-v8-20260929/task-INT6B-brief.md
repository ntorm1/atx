# Integration 6 part B brief (root, pool-2; one integrator at a time)

Read first, binding: `.superpowers/sdd/platform-v8-20260929/integrator-rules.md`. Build tag prefix: `v8-10`
(`v8-10`, `v8-10a`, ...). Build script: `powershell -File scripts/research-build.ps1 -Tag <tag> -Targets "a,b"
-Preset equity-dev`. Merge by commit SHA, never by branch name. Root head at dispatch: the commit that added this
brief (parent `272cc897`; code head `48c625fc`, build `v8-9a`). Context if you need it: `progress.md` from
"integration 6 part A closed" down (rulings E-27a/b, E-31a, E-14a, E-44, E-45, PM4-3, PM4-4), and the
"integration 6 part A" and "integration 5 part C" sections of `integration-log.md`.

## 1. Merges (in this order, each `--no-ff` by SHA)

| lane | SHA | report (arrives with the merge) |
|---|---|---|
| FIX-2 | `de32b9ad` | `task-FIX-2-report.md` (main part and "Round 1") |
| FIX-3 | `75acb091` | `task-FIX-3-report.md` (main part and "Round 1") |
| COMB2 | `1e8af5b8` | `task-R-10-report.md` (main part and "Round 1") |
| ORTH | `c1cc57ce` | `task-R-11-report.md` |

Known overlaps (resolve by reading both sides and both reports; keep every lane's behaviour):
- `scripts/specs/v8/r6*.json` description line: FIX-2 (E-31a pre-return check, E-14a criterion) and FIX-3 F-14
  (`--capacity-curve` kept, E-37). Keep both.
- `atx-impl/tools/fit_composition_weights.py`: FIX-3, COMB2 and ORTH each add hunks (ERA is already in root).
- `scripts/tests/test_research_spec.py`: union of the lanes' tests.
- `atx-impl/src/strategy_ic_admission.cpp`: COMB2 creates the C++ rule table, ORTH adds its row.
- `atx-impl/tests/strategy_ic_runner_test.cpp`, `atx-impl/CMakeLists.txt`: union.
- FIX-2's N-2 commit `28051c4c` is already in root (it came with RISK); expect it to merge as a no-op.

## 2. Part-B edits (ruled; commit each as `fix(<area>): ... (integration 6 part B, Ruling <id>)`)

1. ERA pooled fit (E-27b, E-35a): delete `pooled_aim_weights` and its `elif` so the pooled (era) path runs
   FIX-3's shared `theme_gain_weights`; the pooled list implements `ew-theme-aim-v2` (not `ew-theme-aim-v1`);
   the pooled path refuses `ew-theme-aim-v1` and the message names `ew-theme-aim-v2`. Un-skip the one-era
   equality test (pooled fit on one era equals the single-window fit); it must pass for `ew-theme-std-v1`,
   `ew-theme-std-aim-v1` and `ew-theme-aim-v2`.
2. `scripts/specs/v8/r10.json` parent map (E-45): delete the `ew-theme-v1` and `ew-theme-aim-v1` entries, so R-10
   is defined only on a standardised (rerank-true) parent: `ew-theme-std-v1 -> ic-shrink-v1`,
   `ew-theme-std-aim-v1 -> ic-shrink-aim-v1`; every other parent composition refuses at load. Update
   `test_r10_derives_its_rule_from_the_parent_and_runs_the_w_pass_at_3072` (B0c and R-3-on-B0c parents are now
   refusals). No v8 spec or template may map or name `ew-theme-aim-v1` as a rule to run (E-27b).
3. `scripts/specs/v8/r11.json` (E-45, E-44, PM4-4): confirm it refuses a parent whose weights file has no
   rerank-true `theme_standardise`, and that its recorded acceptance text is rule 5 plus R-1's mechanical
   criterion (planned turnover per unit gross not higher than the parent), as r10 records it. Align text and
   test if not.
4. Anything a merge leaves inconsistent between `PRIOR_COMPOSITIONS` / `COMPOSITIONS` / `AIM_RULES` and the
   pooled list: one list of record, the others derive from or are tested against it.

## 3. Build

Target-scoped, every target whose sources changed: the strategy executables (IC runner, NAV / target, mine if its
sources changed) and the owning test targets: `atx-impl-strategy-ic-tests`, `atx-impl-strategy-target-tests`,
`atx-engine` book / combine / factory tests as touched, `atx-impl-tests`, plus whatever the four reports name.
About 6,000 lines of lane C++ have never been compiled: fix compile errors in place (smallest change, lane design
kept, `/W4 /WX`). A design error is reported, not patched over.

## 4. Tests

- The gtest filters each report names under "How root verifies" and in its Round 1 section (FIX-2:
  `SpoV3.*:SpoPin.*:SpoHook.*:SpoTripwire.*:NavV7Hook.*` and its T-1 / T-4 targets; COMB2:
  `IcShrinkV1.*:IcShrinkAimV1.*:GroupShrink.*:GroupCap.*:CompositionV8.*:StrategyIcComposition.*:StrategyIcRunner.*:MarginalIc.*`;
  ORTH: per `task-R-11-report.md`).
- Every C++ suite: target, book, strategy, ic, mine, factory, impl (baseline at 48c625fc: 253, 154, 46, 105, 18,
  387, 979 / 5 skipped / 1 known failure `ConfigJsonNotInDiscoverDigest`; counts may only grow by the lanes' tests).
- Every Python suite: strategies, engine tools, impl tools, scripts (baseline 163 + 9, 252, 482 / 2 skipped,
  168 / 3 skipped).
- `tiny_world`: no golden moves. The spo-v2 pin test (`SpoPin.*`) must pass against the captured pin `3bfd293e`
  values; never edit a pin or an expected hash.
- A test that fails because the lane's premise was wrong (FIX-2 round-1 "fixture premise") is fixed in the test
  only when the registered rule is still asserted exactly; otherwise report it.

## 5. Identities (Ruling PM4-3; bounded runner; clean tree; 3-year role and existing caches only)

Re-run identities 1, 4, 7 and 8 exactly as in the log section "integration 5 part C" (same argv, new executables):
1 NAV with every new flag absent against `build-equity/v7-w4-holdings`; 4 composition v8 re-rank and cap off
against the v7 composition; 7 spo-v2 side files against the pinned v7 side files; 8 `--label-role` equal to
`--role` against flag absent. A mismatch is a finding: stop, record the first differing file, do not continue to
any data build. Hidden-data rule absolute. Do not run Wave 0 steps and do not run any cell.

## 6. Report

Append the section "integration 6 part B" to `integration-log.md`: merges (SHAs, merge commits, conflicts and
how resolved), part-B edits (commits), build tags and compile fixes (file, reason), every suite count, gtest
filters and results, identities (digests), open items. Commit with `git add -f`; tree clean at the end.
Final reply, at most 12 lines: head SHA, last build tag, suite counts, number of compile / test fixes, identity
results, anything that looks like a design error.
