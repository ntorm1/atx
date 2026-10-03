# P9 integration log

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. One section per integration step, appended in order.
Detail for wave 1 lives in `root-wave1-merge-report.md` (same directory); this log keeps the summary, the gates and
the open items.

## P9 wave-1 merges

### M1a: slots 1-4 (E1, T1, A1, A2), 2026-10-03

Base `6a68d7f9` (R0-14 cut). Head after M1a: the report commit on top of `96c0cfee`.

| slot | lane head | merge commit | conflicts | build tag (targets) | warnings | gtests | pytest | identity |
|---|---|---|---|---|---|---|---|---|
| 1 E1 | `fdd2bda3` | `28cd7d8c` | none | none (Python only) | - | - | scripts/tests x2 seeds 352p/4s/1f each (1f pre-existing); engine/tools 349p; impl/tools 625p/2s | `wave plan y-s` identical pre/post (`990cad41`); `wave status` 4 waves 9/9 unchanged |
| 2 T1 | `0a61b706` | `769ca3d3` | none | none of its own (CMake only; configured in p9-1a/b) | - | - | scripts/tests x2 seeds 373p/4s/1f each (same 1f); strategies 14p; impl/tools 625p/2s; eval_tie 7p | slice-2 `--check` before and after: v71 `787c802e`, recipe.v2 `69e95298`, 19 frozen, exit 0 |
| 3 A1 | `f8edca96` | `de925dca` | none | none (Python only) | - | - | engine/tools 399p; fixtures 9p/1s; focused 50p | registry `ok 92`, `6c56b739` |
| 4 A2 | `2b6f8e6f` | `b50c0a1f` | `atx-engine/tests/CMakeLists.txt` tail (T1 + A2 appends), both kept | p9-1a FAILED (slip); **p9-1b** `atx-engine-research-fields, atx-engine-research-fields-tests, atx-research-fields`, 19.2 s | 0 | Registry 7/7, Manifest 8/8, Fixture+Cli 6/6; `ResearchFields*` 44p/2f (2f pre-existing) | engine-path real exe 11p/0s; engine/tools 399p; fixtures 12p/1s | v15 `--registry`: 84/84 payloads, 82 reused + 2 recomputed as predicted, **2 metadata path groups outside A1's list** -> STOP; A2 TRAIN 3/3 = v15 |

Extra commits: `a97d0483` (registry flip of si_shares, si_dtc, vol_126 to kind engine through A1's `regenerate()`,
sha256 `6c56b739` -> `835b93ea`, `check` passes, round-trip byte-identical) and `96c0cfee` (slip: `-Werror,
-Wfor-loop-analysis` at `research_fields_cli.cpp:243`, loop now steps by two).

CTest (listing only): `-N -L atx_research` = 90 (fields-tests 46 = 31 + A2's 15, mine-tests 44; T1 predicted 75 before
A2's cases); `-N -L atx_equity_strategy` = 537.

Trial ledger: 133 lines, `27e40f9f` (0 trials). Real runs (bounded runner, memory gate free >= cap + 1,536 MiB):
v15 identity (cap 1,536, 38.9 s, peak 315 MiB), A2 TRAIN identity (cap 1,536, attempt 1 refused at spec parse in
0.3 s -- my missing empty output dir -- attempt 2 27.5 s, peak 192 MiB). Nothing dated 2024-01-01 or later opened;
outputs compared by SHA and JSON path only.

### Open items (PM)

1. **v15 identity metadata outside A1's predicted list** (payloads all identical): `source_checks.v9.earn_season_rank`
   moved into `reuse.prior_source_checks_of_partial_groups.v9` (content equal modulo the key rename; the A1 review
   had flagged this as a deferred minor), and `source_checks.holdings.code.*` = the new file hash of
   `research_fields_holdings.py` (A1 rewrote it). Needs a ruling before these count as the accepted identity (and
   for the DEC-20 substitution list).
2. **Pre-existing at the base, not from wave 1:** `scripts/tests/test_research_mine.py::
   test_fields_are_the_rule_applied_to_the_registry` (v8 `a5914373` registry rows vs the test's `EXCLUDED_BY_CLASS`)
   and gtests `ResearchFieldsWriter.QuantilesPartitionLikeNumpy`, `ResearchFieldsVolumeMean.SumOrderIsNumpys`
   (unchanged v8 code; the exe was never in CTest before T1). They block the wave gates "scripts/tests 0 failed" and
   `ctest -L atx_research` until ruled.

### Platform gates (G-P) checkable after M1a

- G-P4 (CTest registration): partial -- `atx_research` lists 90 tests across both registered research exes; final
  count after B1 (admission target) is M1d's.
- G-P5 (mirror guard): `test_no_python_mirror.py` green under both seeds, printing 37 mirrored rule functions in 14
  rows; A1/A2's seal-rule pair is not yet a `FROZEN_PAIRS` row (A2 open risk; root adds it under a ruling).
- G-P6 (no versioned script): the 11 class-C generators (+ 9 tests) are deleted after root's slice-2 `--check`
  passed (before and after the merge); `test_no_versioned_scripts.py` green under both seeds. Checkable now; tick at
  M1d with the full suite.
- G-P7 first half (eval_tie): `generate_eval_tie.py --check` tied; fixture test 7 passed.
- G-P8 (canary goldens): not yet -- recorded after C1 (M1c/M1d).
