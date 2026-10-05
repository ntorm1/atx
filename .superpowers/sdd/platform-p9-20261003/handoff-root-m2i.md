# Handoff: root after M2i (D2R, COVR, item 4 re-run in Release), 2026-10-04

Root `C:/atx-wt/pool-2`, branch `feat/platform-p9-20261003`. Rows: `integration-log.md` "M2i"; detail and every pre-run
list: `root-wave2-gate-report.md` item 4 (d) (pre-run list committed in `f1d2a102` before the first run); evidence
`m2i-logs/` (tools `m2i-logs/tools/`).

- **Code head: `c8ddf736`** = `e8e424b9` (docs; code `59893e9f`) + `5afb5447` (merge of D2R `6d3eb19a`) + `c8ddf736`
  (merge of COVR `0145a834`). No slip commit to code; no SQL2-CLS (neither lane adds a literal).
- **Docs head:** the commit that adds this file (earlier M2i docs commit: `f1d2a102`, the pre-run list).
- **Status: DONE.** Parts A, B and C ran to the end; no identity miss, no red, no stop, no permission refusal. Nothing of
  mine is running; nothing half-merged.
- **Trial ledger** `build-equity/trials.jsonl`: 133 lines, sha256 prefix `27e40f9f` at start and end (0 trials).
- **Next free build tag: `p9-2p`.**
- **UCRT** `ucrtbase.dll` 10.0.26100.7623 `2c08e77f...65ef`, `ucrtbased.dll` 10.0.22000.832 `19e79e91...f48d`
  (before and after; `m2i-logs/ucrt-before.txt`).

## Builds (all through `scripts/research-build.ps1`, target-scoped, one at a time; receipts copied to `m2i-logs/`)

| tag | preset | source | targets | TUs / wall | warnings | exes (SHA-256) |
|---|---|---|---|---|---|---|
| p9-2m | equity-dev | `5afb5447` | atx-research-admission, atx-research-composition, atx-equity-strategy-targets | 1 / 9.9 s | 0 | admission `411af33a0def5a6c...` and composition `b0175bd1eb503d67...` (no work: current); targets **`c02717ef29380a56...`** (replaces the stale p9-2d `90cb34a7`) |
| p9-2n | equity-dev | `c8ddf736` | atx-impl-strategy-target-tests, atx-engine-risk-tests, atx-equity-strategy-risk | 7 / 49 s | 0 | target-tests `9f289a74...`, risk-tests `cbb24a35...`, risk `9f2c6934b5c0074d...` |
| p9-2o | equity-rel | `c8ddf736` | same three | 63 / 293 s | 0 | target-tests `1fd9a34a...`, risk-tests `cd5915cf...`, risk **`ccc73cd10ddc0c4454de50a51f2624e8f38f440e1ea5b47b984cbab06444d811`** |

Exes of record used in M2i (EXE-CUR): Release risk p9-2o `ccc73cd1...d811` (source `c8ddf736`; all four COV runs);
Release NAV `atx-equity-strategy-targets` p9-2l `53ca5dd4...` (source `3fd0bf98`; `git diff 3fd0bf98..HEAD` over C++ /
CMake = only COVR's two test files of the EXCLUDE_FROM_ALL test exe: current); Debug admission / composition / targets
p9-2m (the exe-gated pytest). The p9-2n / p9-2o configure re-ran (COVR's CMake line): both trees' provenance is now
`c8ddf736-dirty`.

## Results

| part / item | result | log |
|---|---|---|
| start state | head `e8e424b9`, tracked change only `progress.md`; ledger 133 / `27e40f9f` | `start-state.txt` |
| A merge D2R | `5afb5447`, 0 conflicts; diff outside the sprint dir = the lane's 3 files; lane vs merge differs only by root's E2 hunks | `d2r-merge.txt` |
| A `test_research_spec.py` | **87 passed, 0 failed**; both e27b tests among the 87 (collect list) | `a-research-spec-s0.log` |
| A fitter / composition (15 files) with real exes | **286 passed, 0 skipped**, 39 subtests: `RealExecutables` and `test_executables_reproduce_the_fitter` ran green (review saw 284 + 2 skips) | `a-fitter-composition-s0.log` |
| A ROOT-1 | `scripts/tests` seed 0 **512 passed / 4 skipped / 0 failed**; stage chain 9 passed | `a-root1-*.log` |
| B merge COVR | `c8ddf736`, 0 conflicts; diff = `strategy_cov_stage_diag.cpp` (new) + one CMake source line; no pin | `covr-merge.txt` |
| B gtests Debug (p9-2n) | 21 / 29 / 106 / 431 + 1 skip (nightly), 0 failed = the lane's account | `b-dbg-gt-*.log`, `gt-exits.txt` |
| B gtests Release (p9-2o) | 21 / 29 / 106 / 430 + 2 skips (nightly + `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read`), 0 failed = the lane's account | `b-rel-gt-*.log` |
| B diagnostic | `CovStageDigest.*`: 0 tests ran, "1 DISABLED TEST", both builds | `b-*-gt-diag-default.log` |
| B ROOT-1 | `scripts/tests` seed 0 **512 / 4 / 0 failed**; stage chain 9 passed | `b-root1-*.log` |
| B review Suggested | S-2, S-3, S-6 applied as one-line doc slips in root's `task-COVR-report.md` (marked); S-1 ruled by COVR-1; S-4, S-5 P10 notes | - |
| C1 S9 | holds before each of the 4 COV runs | `cov-s9.txt` |
| C2 D-COV | v1.1 47.1 s / 633 MiB; atx-cov-v1 **211.2 s / 601 MiB** | `dcov-runs.txt`, receipts |
| C3 Release = Release (TRAIN) | **holds**, both recipes: repeats under affinity mask 1 (probe `[0]`), 12 / 12 files byte-identical, container roots equal | `c3-rel-eq-*.txt`, `aff-probe-*.txt` |
| C4 D-COV read | printed in gate report item 4 (d), "C4 D-COV" (families table + per-rank eigen table); raw `m2i-logs/dcov-read.txt` | `dcov-read.txt` |
| C5 step 4 | **holds**: COVR-1 reading found no `exp2` / `pow` / `exp` on the `--risk-model` read path (only `sqrt`); Release NAV on the Debug-built container 30.5 s / 1,079 MiB, argv = P9-B0's but the 3 tokens; 27 / 28 files identical, `summary.json` differs only at `/producer/build_type`, `/producer/engine_git_sha`; no Debug attribution run needed | `step4-*.txt` |
| C6 step 5 | = C2's atx-cov-v1 run; `recipe.eigen_adjustment` printed (a = 1, 64 sims, seed 7, per-session seed, Kish of HL 504); sessions adjusted 1,279 / not_run 126 / refused 0 | gate report 4 (d) |
| C7 pins | below; deleted by name: `p9-m2i-dcov-v11-r2`, `p9-m2i-dcov-covv1-r2` (980 MB each), `p9-m2g-cov7/` (163 MB) | `store-pins.txt` |

## Store pins (W3-10, PRE-29; both kept, never deleted)

Both written by Release `atx-equity-strategy-risk` p9-2o `ccc73cd1...d811` (source `c8ddf736`) on role
`train-2020-2023-lo3` (`e1c67101...95f4`), fields `train-2020-2023-lo3-fields-v10` (`a4a060ae...0809`),
`--emit-exposures all --emit-container --bias-families extended`.

| store | recipe | read by | `manifest.json` SHA-256 | container root SHA-256 | `model.atxcov` SHA-256 |
|---|---|---|---|---|---|
| `build-equity/p9-m2i-dcov-v11` | atx-risk-v1.1 | P9-X (Y-1 form), wave-3 P9-B0 re-based Y-1 (PRE-29) | `397366473b3a3b0985ed4fb78868f4d894e16ae6c6dec05afda8d0c5126823c7` | `b39a1b776639e7d0b88c70e8a43bb9f6c5e6dacb92a336a327234dfc5d312e1d` | `cd48d5749dd7484ca69ae6b0b9df142b73e2beb64c814d3c143982ec59bb2ce3` |
| `build-equity/p9-m2i-dcov-covv1` | atx-cov-v1 | P9-MV, P9-V (W3-10, PRE-29) | `ea58691fc40745049a2eaf51e48136cd84bfc3cd56f397eb4630af8ec3a0f0f3` | `20919f7b338dfbd786994cbbcb506bdaabd9c3af3266d4394d4d2e07d90c9f8e` | `0f90bf7c78203ca8c93d6003d58c8b2894747045485137756fbc13a32e3b642a` |

Every other payload's SHA-256: `m2i-logs/store-pins.txt` (each equals its `files` pin). Neither store is
byte-comparable with the Debug-built stores of record (`v8-risk-lo3-v10`, `p9-m2d-cov-risk-container`): COV-7 / COVR.

## Known-red set

- **Empty.** The two e27b tests are green (D2R). Release `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned`
  stays a named skip (OD5-2), not a red.

## Kept outputs / deleted

- Kept (new): the two D-COV stores above (980 MB each); `build-equity/p9-m2i-cov-y1-nav` (step-4 NAV, 12 MB,
  regenerable; 27 / 28 files equal to `p9-b0-y1-nav`); run dirs `p9-m2i-*-run` incl. `-r2-run` (receipts).
  `p9-m2d-cov-risk-container` untouched.
- Deleted by name: `p9-m2i-dcov-v11-r2`, `p9-m2i-dcov-covv1-r2`, `p9-m2g-cov7/` (2.1 GB); my pytest basetemps under my
  session scratchpad `m2i/` after the docs commit. Debug / Release object trees in pool-2 kept (root's standing trees).
  C: 36 GB free after the deletions.

## Uncommitted state at the end

- None of mine. `progress.md` and `p9-prereg-flags.md` committed as they stood (LEDGER-1). Four untracked
  `docs/plans/*.png`: not mine, left. Pools 20 and 24: leases untouched.

## Open questions for the PM (one line each)

1. D-COV eigen ranks 00-02 have b above 1,000 in BOTH recipes on TRAIN (three near-null directions; the COV report expected one): an eigenvector diagnostic or a ruled exclusion before P9-MV / P9-V lean on the pooled eigen figures?
2. COVR open question 2, now wider: should the M2c-RED-2 (d) `ucrtbase.dll` guard name both Release-built stores of record (`p9-m2i-dcov-v11`, `p9-m2i-dcov-covv1`)?
3. P10 notes carried from the COVR review: S-4 (RAII restore of the rebound `exp2` import in the diagnostic), S-5 (`covr_cmp.py det` full per-file hashes; root's `tree_cmp.py` already prints them), plus COVR's `exp2` deterministic-libm item (OD5-2).
4. Keep `build-equity/p9-m2i-cov-y1-nav` (12 MB, regenerable) or delete it at the next step group?
