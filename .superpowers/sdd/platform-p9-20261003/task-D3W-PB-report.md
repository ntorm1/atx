# Task D3W-PB report: ew-theme-aim-v2 in the C++ composition fit, the resid reference, the D2-3 deletion; D3W-S

| | |
|---|---|
| Outcome | DONE (see "Scope as delivered" for what D2-3 deletes in P9 and what stays for P10) |
| Pool / branch | `C:/atx-wt/pool-23`, `feat/p9-d3w-20261005`, continued from D3W's APPROVE SHA `f0726acb` (ruling MERGE-SHA) |
| Pre-deletion head (root merges first, runs identity) | the records commit that adds this file (`docs(d3w-pb): ...`); SHA in the PM message |
| Deletion commit (LAST, merged after identity) | its single child, `refactor(d3w-pb): D2-3: ...`; SHA in the PM message |
| Root merged | none. Root's head `fcb0200e` (B2 and T2 merged) does not hold D3W yet; the brief's merge step waits for root's D3W merge, and B2-EVAL-KEEP gives the eval keep-both to root's merge of D3W. `git merge-tree --write-tree fcb0200e f0726acb` and `... fcb0200e 703ac03a` are both clean; this lane's diff since `f0726acb` shares one file with root (`scripts/tests/test_no_python_mirror.py`, deletion commit only: the retired row, away from B2's comment line) |
| Rulings | D3W-PB, D3W-S (S-1 .. S-10), MERGE-SHA, D2-1, D2-3, D2-7, D3-2, ALCOMB-4/-5/-7, P12, P19, PRE-35, EXE-FREEZE, EXE-CUR, TEST-1, HOST-3, TEETH-RESTORE, PERM-1 |
| Builds | `p9-d3wpb-a` Debug (`equity-dev`, clean head `703ac03a`); `p9-d3wpb-b` Debug, the one teeth build (MUTATED, never evidence) |
| Evidence | `d3w-logs/pb-*.log`, `pb-*.json`, tools `d3w-logs/tools/pb-*.py` |

## Commits

| SHA | What |
|---|---|
| `dcddb01d` | S-1: the eight report placeholders and the handoff's two items filled; the report and nine logs / receipts the lane never committed, plus the `mega-p9-d3w-*` receipts and build logs copied from `build-equity/` |
| `6da02a0d` | S-9: `MomVolman.SleeveFilesBeforeAndAfterScaling` writes into its own unique directory |
| `02d3e7a3` | S-2 (one definition of mom-volman-v1's constants), S-4 (`composition_walk_forward` in three helpers), S-5 (`write_mom_volman_sleeves` beside `composition_volman`) |
| `54828cdd` | ew-theme-aim-v2 in the C++ composition fit, its refusals, its `composition_fit.json` keys; gtests `AimV2.*` |
| `bd4e160e` | the fitter's `--engine-fit` for ew-theme-aim-v2; S-8 (no copy of the runner's lifted-candidate refusal); pytests |
| `703ac03a` | S-2's pytest reads the engine header |
| records | this report and `handoff-D3W-PB.md` (the pre-deletion head) |
| deletion | D2-3: composition_resid.py's numpy reference moves into its test; the G-P5 row retires (LAST) |

## First acts (ruling D3W-S, S-1)

- Placeholders: `task-D3W-report.md` OUTCOME, HEAD, ROOT3, LAST, ROOT3B, EVALREAL, TESTS, PERM and `handoff-D3W.md`
  ROOT3 plus its false "object trees deleted" line, from `d3w-logs/` and the receipts. The seed-1 `scripts/tests`
  run is recorded as void under ENV-1 (177 failed / 342 passed / 5 skipped, every failure a child exiting
  3221225794 or a chain downstream of one) and "deferred to the gate (TEST-1)". PERM: none. Commit `dcddb01d`.
- The lane had never committed `task-D3W-report.md` nor nine of its logs / receipts (`git status --ignored`); they
  are in `dcddb01d`, with the `mega-p9-d3w-{a..h}` receipts and build / cache logs copied from `build-equity/`
  first (each verified with `cmp`).
- Object trees: `build-equity-rel/` deleted by name (1.7 GB); in `build-equity/` (2.5 GB) every untracked build
  output deleted by name. `build-equity/` also holds 79 TRACKED files (`audits/`, `v8-interim3-pitch-render-run2/`),
  kept: deleting them would have dirtied the tree.

## Items: file:line -> test -> teeth

### 1. ew-theme-aim-v2 in the C++ fit (the D2-3 precondition)

- `atx-engine/include/atx/engine/research/composition/ew_theme_std.hpp:32-36` `ew_theme_aim_v2_rule`: R-3 on an
  ew-theme-v1 parent = `theme_gain_weights` on scores = the aim gains (Rulings E-27a / E-27b).
- `fit.hpp:14-16` (contract), `:131` `FitSpec::composition`, `:156-157` `CompositionFit::gains`, `theme_weight`,
  `:181-187` refusals, `:224-226` walk-forward refusal.
- `atx-engine/src/research/composition/fit.cpp:184-189` `take_gain_parent` (one parent setter for the three
  theme_gain_weights rules); `:275-287` classification and the modifier refusal; `:335-345` the aim-v2 branch
  (gain refusal naming the member; `theme_weight` summed in member order, the fitter's `aim_theme_weight`);
  `:415-418` `fit_walk_forward` refuses it (its only fitted inputs are the spec's whole-TRAIN gains).
- `atx-engine/src/research/composition/composition_cli.cpp:105-110` parent keys `member_gains`, `theme_weights`
  (only for this composition); `composition_cli.hpp:16`, `:25-27`.
- Tests `atx-engine/tests/research/research_composition_fit_test.cpp:994-1159`: `:1029` `AimV2.ClosedFormOnAPlantedModel`
  (two cap passes, dyadic, bit exact, and equal to `theme_gain_weights` called directly), `:1055`
  `.ReadsNoFactorValue`, `:1070` `.RefusalsBeforeAnyFit`, `:1115` `.CliWritesGainsAndThemeWeightsOnlyForIt` (an ew-theme-std-aim-v1 document has
  neither key; `--walk-forward` refused, nothing published).
- Teeth T1 (modifiers), T2 (gains), T3 (walk-forward): `pb-teeth-cpp.log`.

### 2. The fitter post-step under `--engine-fit` for ew-theme-aim-v2

- `atx-impl/tools/composition_engine_fit.py`: docstring `:35-44`; `AIM_V2`, `SUPPORTED`, `GAIN_PARENTS`
  `:92-95`; `expected_fit` `:439` -> `_expected_aim_v2` `:480-497` (the fitter's own `composition_rules.theme_gain_weights`
  and `ew_theme_aim_v2` on the same members / themes / gains: check 3 under P12 over every key, member_gains and
  theme_weights included); `_kernels` `:634` maps it as a gain parent.
- No new post-step write is needed: the fitter's `_aim_v2_fit` sits inside its producer-fingerprint closure and takes
  no C++ values, so ruling D2-1 is met by the existing `check_written`: the composed weights must equal the C++ final
  weights bit for bit, else the fit is refused and nothing is published (a difference inside P12 included). The C++
  and the Python run the same function in the same member order (`theme_gain_weights`, ascending theme index =
  `sorted(excess)`), so they agree bit for bit (`RealExecutables`, below).
- `--walk-forward` with it: the C++ refuses; the fitter reports the executable's exit (no Python copy of the rule).
- Tests `atx-impl/tools/test_composition_engine_fit.py`: `:49` `CASES["aim-v2"]` and `:130` its stand-in (end to end with the stand-ins:
  flag-present = flag-absent bytes plus `provenance.engine_fit`), `:347` `test_aim_v2_carries_the_cpp_bits_or_refuses`
  (a C++ weight 1e-15 away passes check 3 and is refused by check_written; theme_weights / member_gains beyond P12
  refused by check 3), `:595` `CheckArgs` accepts it; `RealExecutables` runs it on the built exes (the P12 comparison of the
  new mode against the Python side on the fixture world).

### 3. Resid diagnostics and the D2-3 deletion

- The "resid diagnostics" without a fit-side C++ counterpart were composition_resid.py's numpy reference of the
  runner's rule (`centred_tied_ranks`, `residual`, `tie_block_means`, `standardise`, `add_session`, `kernel`,
  `blend`; G-P5 row `composition_resid.py`). Their C++ is atx-engine `composition/theme_resid.hpp`
  `add_theme_residualised` (relocated by D2) and the IC runner's residualise stage, pinned by gtests to the same
  exact rationals; the fit side already writes the residual order in C++ (`theme_resid {order}`). No non-test
  caller uses the reference (searched every `.py`).
- Deletion commit (LAST): the reference moves into `atx-impl/tools/test_composition_resid.py` as the independent
  oracle (`ref.*`; G-P5 scans no test), composition_resid.py loses it and its numpy import, and the
  `composition_resid.py` row of `scripts/tests/test_no_python_mirror.py` is deleted (cross-lane, T1's guard; the
  comment says the remaining rows wait for P10). G-P5 count 34 -> 32 functions, 13 -> 12 rows.
- Reproducible as `d3w-logs/tools/pb-deletion.py apply`: the evidence was taken on a trial apply that the tool
  restored byte for byte (`git status --porcelain` empty, blobs = HEAD); the deletion commit is the same apply.
- Identity-neutral: every fingerprint, `composition_resid`'s included, is the pre-deletion value
  (`pb-fingerprints-deletion.json` = `pb-fingerprints-pre-deletion.json`), and the flag-absent identity is unchanged
  (`pb-identity-deletion.log`).

### Scope as delivered (what D2-3 can delete in P9)

D2-3 as written deletes every `composition_*.py` and replaces them with a document builder over
`composition_fit.json`. That needs every fit path to run the C++: today the flag-absent default path, the era pool
(E-35a fits ew-theme-aim-v2 on pooled decisions; `--engine-fit` refuses `--era`) and the `--engine-fit` Python pass
itself all call the Python rules, and the orchestration passes `--engine-factors` / the exes only under
`--engine-fit`. Deleting `composition_rules.ew_theme_aim_v2` now would remove a registered capability (the era-pooled
fit) and break every flag-absent aim-v2 fit. So the remaining rows (`composition_rules`, `composition_theme_erc`,
`composition_ic_shrink`, the fitter's `screen_v4` / `newey_west_t`) stay, lane column D3, for P10's document builder.
The precondition D2-3 named (a C++ path for ew-theme-aim-v2 and the resid diagnostics) is met.

### REQUIRED and suggested items of D3W-S

| item | disposition | where | test / teeth |
|---|---|---|---|
| S-1 placeholders, 4.2 GB | DONE | `dcddb01d`; trees deleted by name | - |
| S-2 constants defined twice | DONE | new `atx-engine/include/atx/engine/research/composition/mom_volman.hpp` (one definition; `min_decisions = lookback + lag`); `fit.hpp` includes it; `atx-impl/src/strategy_ic_mom_volman.hpp:29-43` re-exports by `using` | `MomVolman.RegisteredConstants`, `MomVolman.*` (IC), `test_constants_match_the_runner_header` (`test_composition_mom_volman.py:28` reads the engine header, `703ac03a`) |
| S-3 | ABS's (not this lane) | - | - |
| S-4 `composition_walk_forward` 92 lines | DONE | `atx-impl/src/strategy_ic_rules.cpp:697` `walk_forward_shape`, `:719` `walk_forward_rows`, `:761` `lift_walk_forward_candidates`, `:796` `composition_walk_forward` (each < 60 lines; messages unchanged) | `WeightsV3.*` (IC) |
| S-5 sleeve writer in the rule-table TU | DONE | `atx-impl/src/strategy_ic_mom_volman.cpp:203` after `add_mom_volman_trace` (declaration stays in `strategy_ic_rules.hpp`); six includes leave `strategy_ic_rules.cpp` | `MomVolman.SleeveFilesBeforeAndAfterScaling` |
| S-6 | PM's PRE 2b-ii list | - | - |
| S-7 | ABS's | - | - |
| S-8 Python copy of the lifted-candidate refusal | DONE (deleted) | `composition_engine_fit.py:725` `weights_v3` keeps the theme mapping only (`:744-749`); the IC runner owns the refusal (`lift_walk_forward_candidates`); the C++ fit cannot produce such a row (members signed +1; every theme of a fit carries mass) | `test_fit_walk_forward.py:579` `WeightsV3.test_the_runner_owns_the_lifted_candidate_refusal`; runner `WeightsV3.RefusalsPrecedeAnyPayloadOrOutput` |
| S-9 fixed temp dir | DONE | `atx-impl/tests/strategy_ic_mom_volman_test.cpp:352` `ScratchDir` (used `:382`) (create_directory settles uniqueness, RAII removal) | `MomVolman.SleeveFilesBeforeAndAfterScaling` |
| S-10 look-ahead mutation | DONE | teeth T4: `cut+wf.lag<=j` -> `<=j+1U` at `fit.cpp:444` `fit_walk_forward` | `WalkForward.NoRowUsesFutureDecisions` (`pb-teeth-cpp.log`) |

## Tests

All logs in `d3w-logs/`. Interpreter `C:/Program Files/Python312/python.exe`, `-p no:cacheprovider`, own
`--basetemp` (parent created first), `PYTHONDONTWRITEBYTECODE=1`.

| run | result | log |
|---|---|---|
| configure `equity-dev` (fresh, after the deletion of the trees) | exit 0 | `pb-configure-equity-dev.log` |
| single-TU `check` of every touched C++ file | exit 0 (one slip: the moved writer's `'\\n'` literals, fixed before any commit) | `pb-check-a.log`, `pb-check-b.log` |
| build `p9-d3wpb-a` Debug: composition tests, `atx-research-composition`, `atx-research-admission`, IC tests; source `703ac03a`, DirtyEntries 0, 137 TUs | exit 0; exe SHA-256 in the receipt | `mega-p9-d3wpb-a-receipt.json`, `-build.log` |
| `atx-engine-research-composition-tests --gtest_filter=AimV2.*:WalkForward.*:MomVolman.*:ResearchComposition.*` | 21 / 21 (4 new) | `pb-gt-a-comp.log` |
| `atx-impl-strategy-ic-tests --gtest_filter=WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*` | 18 / 18 | `pb-gt-a-ic.log` |
| the edited pytest files before the build (no exes) | 45 passed, 3 skipped | `pb-pytest-a.log` |
| the five fitter pytest files (`test_composition_engine_fit`, `_mom_volman`, `_resid`, `test_fit_composition_weights`, `test_fit_walk_forward`) with the exes of `p9-d3wpb-a`, head `703ac03a` | 180 passed, 88 subtests, 0 skipped | `pb-pytest-fitter-real.log` |
| `-k RealExecutables` (ew-theme-aim-v2 on the built exes: check 3 under P12, then check_written bit for bit) | 3 passed | `pb-pytest-real-exes.log` |
| fitter flag-absent identity, base `f0726acb` vs `bd4e160e` | 21 / 24 identical; 3 differ only at `provenance.engine_fit.producer_fingerprint` | `pb-identity-pre-deletion.log` |
| fingerprints | only `composition_engine_fit` moves; `OUTPUT_PRODUCERS` `d28a3089...` | `pb-fingerprints-pre-deletion.json` |
| D2-3 deletion, trial apply on `703ac03a` (blobs `pb-deletion-blobs.txt`): the five fitter pytest files with the exes | 180 passed, 88 subtests | `pb-deletion-pytest-fitter.log` |
| D2-3 deletion trial: `scripts/tests/test_no_python_mirror.py` (own invocation) | 7 passed; G-P5 32 functions in 12 rows | `pb-deletion-pytest-mirror.log` |
| D2-3 deletion trial: fingerprints; identity | unchanged; the same 21 + 3 | `pb-fingerprints-deletion.json`, `pb-identity-deletion.log` |
| the one teeth build `p9-d3wpb-b` (MUTATED; memory admission refused twice, built on the third try) | T1-T4 all red: `AimV2.RefusalsBeforeAnyFit` (the 4 modifier cases, the 4 gain cases, the walk-forward case), `AimV2.CliWritesGainsAndThemeWeightsOnlyForIt`, `WalkForward.NoRowUsesFutureDecisions` and `WalkForward.ExpandingMeanClosedForm` (S-10); 4 others passed; restore verified: `git status --porcelain` empty, `fit.cpp` blob `1c3461a2` = HEAD | `pb-teeth-cpp.log`, `mega-p9-d3wpb-b-MUTATED-receipt.json` |
| whole targets, `scripts/tests` seeds 0 / 1, the stage chain, Release, canaries | deferred to the gate (TEST-1) | - |

Slips fixed in place before the evidence runs: S-2's pytest still parsed the runner header (fixed `703ac03a`); the
first deletion trial left the wrong-tie-step mock on `composition_resid` (the tool now patches the moved global; that
trial was restored by the tool and re-run).

## Flag-absent identity and the TRAIN identity procedure for root

1. Fingerprints (`d3w-logs/tools/d3w-fingerprints.py`, cwd `atx-impl/tools`): versus D3W's final
   (`fingerprints-final.json`) only `composition_engine_fit` moves (`a6dd0da5...` -> `b0052dc5...`); the fitter's
   `OUTPUT_PRODUCERS (fit, fit_prior)` stays `d28a30895ad1393e512d3a20384af025063675e9a2617611ee9cae41e17304a2`
   and every other entry is unchanged (`pb-fingerprints-pre-deletion.json`). The deletion moves nothing
   (`pb-fingerprints-deletion.json`).
2. Fitter outputs: `git archive f0726acb atx-impl/tools atx-engine/tools atx-impl/strategies | tar -x -C BASE`;
   `python d3w-logs/tools/pb-ident.py BASE/atx-impl/tools WORK OUT_BASE`; `python pb-ident.py <merged>/atx-impl/tools
   WORK OUT_HEAD` with the SAME `WORK`; `python d3w-logs/tools/cmp.py OUT_BASE OUT_HEAD`. Cases: plain (no flag)
   std, std+erc+tsmom, shrink+tsmom, std-aim+resid, ew-theme-aim-v2; `--engine-fit` (stand-ins) std+erc+tsmom,
   shrink+tsmom, std-aim+resid. Expected (`pb-identity-pre-deletion.log`): 21 of 24 files byte-identical; the three
   engine-fit weights files differ only at `$.provenance.engine_fit.producer_fingerprint` (the module fingerprint of
   `composition_engine_fit`, written only under `--engine-fit`; D3W precedent). After the deletion commit: the same
   (`pb-identity-deletion.log`). No expected hash or golden was edited.
3. C++: every pre-existing `atx-engine-research-composition-tests` case passes unchanged (`FitEqualsPythonFixture`
   pins the existing modes' values; `CliPublishesTheFit` their documents); a document of any composition but
   ew-theme-aim-v2 gains no key (`AimV2.CliWritesGainsAndThemeWeightsOnlyForIt`). IC: the S-2 / S-4 / S-5 refactors
   keep every message and byte (`WeightsV3.*`, `MomVolman.*`, `ThemeTsmomRunner.*`, `CharCovHedge.*` named filters).
4. Root's targeted checks at merge (ROOT-1a): Debug build of `atx-engine-research-composition-tests`,
   `atx-impl-strategy-ic-tests` (+ `atx-research-composition`, `atx-research-admission` for the real-exe pytest);
   filters `AimV2.*:WalkForward.*:MomVolman.*:ResearchComposition.*` (composition) and
   `WeightsV3.*:CompositionWeightRows.*:MomVolman.*:ThemeTsmomRunner.*:CharCovHedge.*` (IC); the fitter pytest files
   (with `ATX_RESEARCH_ADMISSION_EXE` / `ATX_RESEARCH_COMPOSITION_EXE` set for `RealExecutables`); the identity above.
   Deferred to the gate (TEST-1): whole targets, `scripts/tests` seeds 0 / 1, the stage chain, Release.

## New literals, flags, keys

- Literals: `ew_theme_aim_v2_rule` (C++; the Python id existed). No new `atx.<name>/v<n>`, no new flag.
- Keys: `composition_fit.json` `parent.member_gains`, `parent.theme_weights` (ew-theme-aim-v2 only); spec
  composition `ew-theme-aim-v2` accepted.
- Header: `atx-engine/include/atx/engine/research/composition/mom_volman.hpp` (no CMake change: header only).

## Cross-lane edits

- `atx-impl/src/strategy_ic_mom_volman.{hpp,cpp}` (AL-COMB's): S-2 re-export; S-5 the moved writer.
- `atx-impl/tests/strategy_ic_mom_volman_test.cpp`: S-9.
- `scripts/tests/test_no_python_mirror.py` (T1's guard): the composition_resid row deleted (deletion commit only).

## For PRE

Nothing: no P9 cell reads ew-theme-aim-v2's C++ fit (the C++ fit is used of record only under `--engine-fit`, ALCOMB-7
"fit mode of every P9 cell = its P9-B0 parent's"), and the deletion changes no output.

## Open questions

- The aim rules' gains are whole-TRAIN statistics carried by the spec, so a `--walk-forward` row of
  ew-theme-std-aim-v1 / ic-shrink-aim-v1 uses gains measured after its own window (D3W accepts them; this lane refuses
  only ew-theme-aim-v2, whose rows would equal the full fit). PRE / P10: refuse walk-forward on the aim rules, or
  refit the gains per row?
- D2-3's remaining rows (composition_rules, theme_erc, ic_shrink, the fitter's screen) need the P10 document builder
  (every fit path through the C++, era pool included).

## Permission refusals

None.

## Fix round 1 (D3WPB-R1)

- Commit `90bc1962` on the PARTIAL review commit `a0c7427c` (history not rewritten); one file, `scripts/tests/test_no_python_mirror.py`.
- Four `retired_by` labels `"D3"` -> `"P10"` (ruling D3WPB-Q1): `composition_rules.py` (:60), `composition_theme_erc.py` (:64), `composition_ic_shrink.py` (:66), `fit_composition_weights.py` `newey_west_t` / `screen_v4` (:69); the comments at :56-57 and :61-62 cite D3WPB-Q1 (the D2-5 precedent for `horizon_stats.py`). No row, symbol or check removed or weakened.
- `scripts/tests/test_no_python_mirror.py`, own invocation: 7 passed; "G-P5: 21 mirrored rule function(s) in 8 row(s) left for P9 lanes" (32 / 12 with the D3 labels).
- No build, no root merge, no C++ touched; the review resumes from `task-D3W-PB-review.md` "Next step" (D3WPB-SEQ, REV-FRESH; HEAD = the records commit above `90bc1962`).
