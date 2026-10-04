# Lane S2R review (light task review, M2c-RED-2 / OD5-2 / S2R-1 / S2R-2)

## Verdict

**APPROVE.** 0 Required, 5 Suggested.

## Reviewed SHA

- Head: `048b01c6131fde9c4b9f3fab6e5aadf9c19723f1` (pool-18, branch `feat/p9-s2-20261003`). The tree is clean and the
  reflog is linear: `f686080a` (merge of root `f6444417`), `02eae2ab`, `5c9a5162`, `49029727`, `048b01c6`.
- Scope: `2ed2fc18..048b01c6`, excluding the merge `f686080a` itself.
  - `git diff f6444417 f686080a` touches only `task-S2-rereview-1.md` (S2's own doc), so the merge carries no
    hidden edit.
  - The one code file in scope is `atx-engine/tests/alpha/alpha_lit_golden_test.cpp` (+236 / -6). Everything else is
    sprint-dir docs, logs and scratch tools.
- Read-only. I built nothing, edited no source and switched no branch. This file is the only file I wrote in the repo.

## Evidence (independently run)

| check | command (summary) | result |
|---|---|---|
| hex constants | `grep -o '0x[0-9a-fA-F]+'` on the file at `2ed2fc18` and at `048b01c6`, then `comm` | removed: none. Added: `0x7ff8000000000000ULL` only (canonical NaN in `canonical_bits`, not a pin) |
| removed lines | `grep '^-'` on the diff | 6 lines, all the pinned test's old `if` / `ADD_FAILURE` / `EXPECT_EQ` block, now in the `else` branch unchanged |
| S2R-1 scan reproduces | `PYTHONDONTWRITEBYTECODE=1 python s2r-logs/tools/s2r1_scan.py`, then diff with the committed `s2r1-spec-scan.txt` (CR stripped) | exit 0; diff exit 0 (46 / 46 lines identical) |
| independent book scan | own scan (scratch, specs only): word-bounded calls of ts_zscore / stddev / ts_std / ts_var / slope / rsquare / resid / ts_std_mp / correlation / ... over the 67 inline `dsl` of `fund_industry_ic_v8ysb.json` (sha `41010b0b...` = spec pin) | see E |
| log counts | `RUN` / `OK` / `SKIPPED` / `FAILED` line counts over `s2r-logs/*.log` | see D |

## Checks A-F

### A. No pin moved: PASS

- `kGoldenVmAuditExact` / `kGoldenVmResearchFast` / `kGoldenOracle` are `0x971c3da60ca89aa3` / `0xe9e7128fd5359900` /
  `0x971c3da60ca89aa3`, byte-identical at `2ed2fc18` and `048b01c6` (`alpha_lit_golden_test.cpp:68-70` at the head).
- These are unchanged too: the panel seed `0x6011D3A7ULL`, `golden_panel()`, the 84-row `existing_ops()` battery, the
  FNV fold `fnv_cells` / `run_battery` (basis `1469598103934665603`, prime `1099511628211`) and
  `BatteryCoversEveryPreW2NamedOp`. No diff hunk touches them.
- No other test or fixture file is in the code diff.
- Nothing pins the test file itself: `git grep alpha_lit_golden_test` outside the sprint dir finds no match; CMake
  globs the file.

### B. The skip: PASS

- The condition is `#if defined(_MSC_VER) && defined(_DEBUG) && defined(_DLL)` (`:78`). The pinned test prints the
  three digests in every build. `if constexpr (!kPinnedCrtLinked)` then runs `GTEST_SKIP()` (`:332-337`); the `else`
  branch holds the old comparison verbatim.
- It cannot silently pass:
  - Release reports `[  SKIPPED ]` with the message, in the per-test line and in the summary list
    (`release-g-litgolden.log`, `release-g-alpha-whole.log:808, 2583`).
  - A Debug build on another host or SDK (`ucrtbased.dll` version, non-FMA3 CPU; report section 3) fails loudly.
- The message names "Ruling M2c-RED-2", `ucrtbased.dll` (/MDd) vs `ucrtbase.dll`, "std::pow ... 1 ULP on ~0.2% of
  arguments", the three rows, and the test that carries correctness. It matches report 11.4 item 3 verbatim.
- The condition is right for this repo's presets:
  - Root `CMakeLists.txt` declares `cmake_minimum_required(VERSION 3.20)`, so CMP0091 is NEW.
  - No first-party CMake or preset sets `CMAKE_MSVC_RUNTIME_LIBRARY`, except `equity-asan` (`MultiThreadedDLL`,
    Release). So the default `MultiThreaded$<$<CONFIG:Debug>:Debug>DLL` applies.
  - Debug presets (`dev`, `equity-dev`, `equity-hygiene`, `dev-shared`, ...) therefore get /MDd, and clang-cl defines
    `_DEBUG` + `_DLL`: the pinned branch runs.
  - Release presets (`equity-rel`, `equity-bench`, `rel`, `rel-avx2`, `equity-asan`) get /MD (`_DLL`, no `_DEBUG`):
    the test skips.
  - Confirmed empirically. Debug `debug-e-litgolden.log` takes the pinned branch (3 passed; prints the pins). Release
    `release-g-litgolden.log` skips.
  - /MTd (not used) would also skip, which is conservative.

### C. `EveryPreW2OpcodeVmMatchesOracle`: PASS (with Suggested S-1, S-2)

- **Teeth in audit-exact: yes.**
  - On every one of the 84 rows: `EXPECT_EQ(bit_mismatch(va, vo), "")` (`:385`). That is a bit-for-bit comparison of
    every cell, with NaN payloads canonicalized and +0 / -0 kept distinct.
  - Each row must also have a finite AuditExact cell (`:379-380`).
  - Any VM kernel that disagrees with the oracle in any bit fails.
  - No row is exempt from VM == oracle.
  - Limit (by ruling, not a defect): a common-mode change in VM and oracle is invisible to this test. In Release that
    is now caught by nothing on this battery (see S-5).
- **ResearchFast determinism: asserted, but narrower than the comment says** (S-1).
  - `bit_mismatch(vr2, vr1)` (`:386`) compares two fresh engines, so run-to-run determinism holds on all 84 rows.
  - The 2-worker `DetPool` does not engage the ResearchFast-specific kernels on this 12-instrument panel. Tiles are
    `ceil(12 / kTsInstrumentTile = 64) = 1`, and every reroute requires `tiles > 1` before using the pool
    (`vm.hpp:1853, 1869, 1885, 1924`). The Welford route (`vm.hpp:1931-1935`) never uses a pool.
  - The pool is exercised only on the shared batch column path (`vm.hpp:1960`) and the banded ops (`run_bands`).
- **Exemptions.**
  - The ResearchFast == AuditExact bit check is relaxed to the 1e-9 contract on the 18 rows whose program holds one
    of `kResearchFastToleranceOps` (`:91-95`). Each of the 15 opcodes is justified. The list equals exactly the
    union of the reroute predicates at the head:
    - `TsDecayExp` (`vm.hpp:1838`);
    - `TsSum` / `TsMean` (`:1860`);
    - `TsCorr` / `TsCov` / `TsRegression` (`:1876`);
    - `is_unary_sliding_op` = `TsDecayLinear` / `TsWma` / `TsSlope` / `TsRsquare` / `TsResid` (`:1915`,
      `ts_sliding.hpp:470, 559, 696`);
    - `ts_is_online_variance_op` = `TsVar` / `TsStd` / `TsZscore` / `TsAvDiff` (`:1931`, `ts_ops.hpp:780-797`).
  - No other `mode_` branch changes values: I enumerated every `mode_` use in `vm.hpp`. `TsDelta`'s SIMD subtraction
    (`:2033`) is elementwise and therefore exact. The `SubtreeCache` keys are not value branches.
  - Re-derived from `registry.cpp:74-117`, the 18 rows are: ts_sum, ts_mean, stddev, ts_std, ts_var, correlation,
    covariance, decay_linear, wma, slope, rsquare, resid, ts_zscore, ts_av_diff, ts_corr, ts_regression,
    ts_decay_exp and trade_when(... ts_mean ...). `ema` is `TsEma` and is not rerouted.
  - The count is asserted (`:394-395`) and the list is marked FROZEN.
- **NaN-pattern skip.**
  - It is no wider than the ruling allows. `tolerance_mismatch` is called only on the 18 tolerance rows (`:387-389`);
    the 66 other rows compare NaN patterns bit for bit through `bit_mismatch`.
  - The ruling is named in the function comment (`:224-232`, "P9 Ruling S2R-1 ...") and on the `continue` (`:241`).
  - Ruling S2R-1 sanctions exactly this scope ("skip of the NaN-pattern check on ResearchFast-tolerance rows stands").
  - Within those rows the skip is two-sided and covers every cell. That opens a gap: S-2.

### D. Logs: PASS (Debug provenance inferred, not receipt-proven)

| log | ran | passed | skipped | failed |
|---|---|---|---|---|
| `release-g-alpha-whole.log` | 782 (313 suites) | 780 | 2 | 0 |
| `debug-e-alpha-whole.log` | 783 (314 suites) | 783 | 0 | 0 |
| `release-g-ic-s2filter.log` / `debug-d-ic-s2filter.log` | 28 / 28 | 28 / 28 | 0 | 0 |
| `release-g-ic-whole.log` / `debug-d-ic-whole.log` | 204 / 204 | 204 / 204 | 0 | 0 |

- The 2 Release skips:
  - `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` (M2c-RED-2 message);
  - `AlphaVm_ZeroAlloc.WarmDispatchLoop_BoundedByOutputsAndSizeIndependent` (`alpha_vm_test.cpp(339)`: "allocation
    counting requires the MSVC CRT debug heap"). It was already a skip in round 1's `release-c`.
- Debug-vs-Release gap: the 783 vs 782 tests is the Debug-only `AlphaSlotPoolDeathTest.OverAcquire_Aborts` (the only
  name in the Debug RUN set and not the Release one). The report does not name it (S-4).
- The S2 filter string is the handoff's, in both logs.
- `StrategyIcRunner.*SourcesPinnedToSemanticsVersion` passed in both builds. That re-proves the `dsl_vm` / `ic_result`
  source-digest pins at the lane head.
- Release `p9-s2r-g` is on the head code:
  - receipt `49029727-dirty`, 1 dirty entry;
  - the test file's mtime is 12:35:43, before the build (~13:03-13:08), and unchanged since; the tree is clean at
    `048b01c6`;
  - the skip line `(333)` in the log equals the head's line 333.
- Debug `p9-s2r-e`:
  - The receipt says `02eae2ab-dirty` (1 TU, built 12:01:37); the logs were written 12:01:54-12:03:05 and committed in
    `5c9a5162` (12:04:49).
  - The commits after the Debug run are `5c9a5162` (it contains the e logs), `49029727` (docs only) and `048b01c6`.
    `git diff 5c9a5162 048b01c6 -- <test file>` is comment-only: 4 lines added and 2 removed, all `//`.
  - The receipt holds no hash of the dirty file, so "e built exactly `5c9a5162`'s file" is inferred, not proven. The
    inference rests on the 3-minute window and on the d-to-e line shift (the d log's 358 / 386 vs the wip's
    359 / 387). Root's Debug run at merge closes this. The IC Debug numbers (`p9-s2r-d`) are valid for the head as
    they stand: the lane changed no IC-target source.

### E. S2R-1 list: CONFIRMED (one doc slip, S-3)

- `ts_zscore`: 0 of 67 book candidates, 0 P9 strings.
- `ts_std`, `ts_var`, `slope`, `rsquare`, `resid`: 0 / 0. `qmj_safety` matches "slope" only in `notes.deviation` prose.
- No name or lowering other than `stddev` / `ts_std` reaches `TsStd`; likewise only `ts_var` reaches `TsVar` and only
  `ts_zscore` reaches `TsZscore`. `git grep` shows those opcodes only in the registry, the VM / oracle / streaming /
  op_traits.
- `stddev`: exactly 5 candidates: `res_mom_12_1`, `smax`, `iv_rv_spread`, `qmj_safety`, `smax5`. I agree with the
  per-candidate verdicts by source:
  - **`smax`, `smax5`: can split.**
    - On an all-zero `ret` window, AuditExact gives `std = 0`, hence `0/0` and `0*log(0)`: NaN.
    - ResearchFast gives a Welford residual `s > 0`, because the state resets only when the last finite cell leaves
      (`ts_ops.hpp:864-871`). `emit` has no flat guard (`:884-910`). So the result is `0/tiny + 0*log(tiny)` = 0; if
      `s` clamps to 0, both give NaN.
    - The numerators `ts_max` / `ts_topk_mean` of zeros are exact 0 in both modes.
    - Nuance (no action): by `tsv_is_flat` a constant non-zero return over the window is also flat. AuditExact would
      give NaN and ResearchFast a large finite value, not 0. A cent price grid cannot produce that over 22+ sessions,
      so the list stands.
  - **`iv_rv_spread`, `qmj_safety`: value-only.** AuditExact 0 / -0 vs ResearchFast tiny; there is no NaN on either
    side.
  - **`res_mom_12_1`: masked.**
    - `correlation(ret, mkt_ret, 231)` on a flat `ret` is NaN in both modes.
    - In ResearchFast the drift trigger re-centres the CoMoment on the collapsed window. The `kCancelGuard` gives
      `cxx = 0`, so the relative-flat test and `corr()` give NaN (`ts_sliding.hpp:95-200, 307-312`).
    - The NaN propagates through the subtraction.
  - `bac`'s `correlation` and the W2 ops (`ts_std_mp` in `res_mom_ind` / `iv_vol_of_vol`, `ts_beta_on`,
    `ts_topk_mean`) run the same kernel in both modes, so they cannot split.
- P9 strings at the lane head: 0. `russell_recon` / `lazy_prices` / `hf_crowd` carry only `rank(<field>)`.
  - Report 12.3 says "hf_crowd has no DSL yet". That is wrong: `scripts/specs/p9/blocked/hf_crowd.json:3` carries
    `"dsl": "rank(hf_crowd)"` (no field built). The conclusion is unchanged.
- Root's current tree (`2ee62800`) adds the AL-COMB `rules/`, `templates/` and `diagnostics/` specs. My scan finds 0
  DSL calls of a split op there. The only match is the prose "OLS slope (intercept ..." in
  `rules/theme-cov-hedge-v1.json` `fixed_choices[2]`.
- S2R-2's prereg line (`smax`, `smax5`: "AuditExact NaN may be ResearchFast 0 on a flat window") is consistent with
  this.

### F. Root items and source-digest pins: PASS

- Report 11.4 states the targets, the expected counts per build, the verbatim skip message, the Release prints with
  the `ucrtbase.dll` version / SHA, the gate wording (M2c-RED-2 (c)) and the PCH ccache build trap.
- No S2R edit touches a pinned source file.
  - The only code file is a test. It is not in `dsl_vm_sources` (35 files, `strategy_ic_signal_cache.cpp:42-77`) nor in
    `ic_result_sources` (11 files, `strategy_ic_result_cache.cpp:31-42`).
  - `dsl_vm` `5bcd55c7` and `ic_result` `dd3c6011` therefore do not move on account of S2R, and no re-pin is due.
  - The two `*SourcesPinnedToSemanticsVersion` tests passed in Debug and Release at the head.
- Root has not touched the test file, `vm.hpp`, `ts_ops.hpp`, `ts_sliding.hpp`, `oracle.*` or any alpha test since
  `2ed2fc18` / `f6444417` (`git diff` at root `2ee62800`). The merge of this file should be clean, and the alpha counts
  carry over unchanged.

## Findings

All Suggested; none blocks.

| # | path:line | severity | problem | concrete fix |
|---|---|---|---|---|
| S-1 | `atx-engine/tests/alpha/alpha_lit_golden_test.cpp:351` (and report 11.1) | Suggested | "ResearchFast is deterministic: a serial run and a 2-worker DetPool run give the same bits" overclaims pool coverage. At 12 instruments every ResearchFast reroute runs one tile (`kTsInstrumentTile = 64`, `vm.hpp:1846-1856`), and the Welford route never pools, so the 15 tolerance kernels are only checked run-to-run. Pool invariance is exercised only for the shared batch / banded kernels. | At the next touch of the file (comment only, no pin), say so and point to `alpha_ts_sliding_routing_test.cpp`: its 67-name panels engage the pair and `ts_decay_exp` tiles. No rebuild now. |
| S-2 | `alpha_lit_golden_test.cpp:240-242, 379-380, 396-404` | Suggested | On a tolerance row, a ResearchFast output that is NaN in every cell passes. `tolerance_mismatch` skips any cell where either side is NaN, and the finite-cell check covers only `va`. The teeth self-test exercises only `bit_mismatch`, never `tolerance_mismatch`. | P10, or the next round that touches the file. (1) `EXPECT_TRUE(any_of(vr1, isfinite))` per row. (2) On tolerance rows, count the cells finite in AuditExact and NaN in ResearchFast and expect 0: the d-run showed equal NaN patterns on the 17 non-`ts_zscore` rows. This leaves the S2R-1 direction (AuditExact NaN, ResearchFast finite) unchecked, as ruled. (3) Self-test `tolerance_mismatch`: a 1e-8 relative perturbation and a +inf / -inf swap must be flagged. |
| S-3 | `task-S2R-report.md:402-403` (section 12.3) | Suggested | "`hf_crowd` has no DSL yet" is wrong: `scripts/specs/p9/blocked/hf_crowd.json:3` is `"dsl": "rank(hf_crowd)"`. The conclusion (0 P9 strings) is unchanged. | Reword to "the P9 DSL strings are `rank(russell_recon)`, `rank(lazy_prices)` and `rank(hf_crowd)` (blocked, no field); none uses a time-series op" before the prereg copies it. |
| S-4 | `task-S2R-report.md:255-260, 291-294` (11.3 / 11.4) | Suggested | The expected counts imply that Debug runs one test more than Release (783 vs 782), but the report never names it. Root could misread the gap. | Add: "Debug-only `AlphaSlotPoolDeathTest.OverAcquire_Aborts` (death test), so Debug runs 783 and Release 782." |
| S-5 | report 11.4 (root checks) | Suggested | After the split, Release has no bit-level value check on the battery. VM == oracle cannot see a common-mode change, for example a Release-only codegen difference in a kernel the oracle shares. | Add a diagnostic-only root check at the wave-2 gate (no pin): run `DISABLED_PerRowDigestDump` in both builds and diff. Expect exactly rows 05 / 06 / 83 to differ, in ae / rf / oracle alike, as in `s2r-perrow-diff-debug-vs-release.txt`. Any other moved row is a new red. |

## What root must check at merge

1. **Merge cleanliness.** The lane's only code file is `atx-engine/tests/alpha/alpha_lit_golden_test.cpp`. Root's head
   has not touched it since `2ed2fc18`. Every other lane file sits under the sprint dir.
2. **Build both trees and run.**
   - Targets: `atx-engine-alpha-tests` and `atx-impl-strategy-ic-tests`, in `equity-dev` and `equity-rel`.
   - Debug alpha: 783 passed / 0 skipped / 0 failed (1 disabled).
   - Release alpha: 780 passed / 2 skipped / 0 failed (1 disabled), adjusted only by alpha tests that root's other
     merges added (none since `f6444417`).
   - The 2 Release skips must be exactly `AlphaLitGolden_ExistingOps.EveryPreW2OpcodeDigestIsPinned` and
     `AlphaVm_ZeroAlloc.WarmDispatchLoop_BoundedByOutputsAndSizeIndependent`.
   - The Debug / Release difference of 1 is `AlphaSlotPoolDeathTest.OverAcquire_Aborts`.
   - `EveryPreW2OpcodeVmMatchesOracle` must PASS in both builds.
   - S2 filter: 28 / 28 in both. IC whole: 204 / 204, plus any IC tests root added since.
3. **Read the gtest summary, not the exit code.** A skip exits 0. Check that the Release skip text is verbatim the
   report 11.4 item 3 message. Check that Release prints `0xeeae40c4a6d7d93b` / `0x24c97b5bfeab6518` /
   `0xeeae40c4a6d7d93b`; a different print means the UCRT or a kernel changed (M2c-RED-2 (d)). Check that Debug prints
   the pins.
4. **Source-digest pins.** No recompute is due from S2R. `dsl_vm` `5bcd55c7` and `ic_result` `dd3c6011` must still
   pass `StrategyIcRunner.*SourcesPinnedToSemanticsVersion`. If they move at root, the cause is another merge.
5. **S2R-1 / S2R-2 prereg lines.**
   - Re-run `s2r-logs/tools/s2r1_scan.py`, pointed at the merged tree, over all of `scripts/specs/p9/` (root now
     has `rules/`, `templates/`, `diagnostics/`). Expected: 0 P9 strings and the same 5 `stddev` book candidates. My
     scan of root `2ee62800` agrees.
   - Print `smax` and `smax5` with the S2R-2 line.
   - Correct the `hf_crowd` wording (S-3) wherever the prereg quotes it.
6. **Debug provenance.** The lane's Debug run carries only a `02eae2ab-dirty` receipt. Root's own Debug run of the
   merged tree is the proof that `EveryPreW2OpcodeVmMatchesOracle` passes on this exact code in Debug.
7. **Build trap (report 11.4 item 6).** A re-fetched `deps/<preset>` after ccache stored a PCH fails a changed
   PCH-consumer TU with "has been modified since the precompiled header". Delete the two PCH outputs by name and
   rebuild once with `CCACHE_RECACHE=1`.
8. **Optional (S-5).** Diff the Debug vs Release per-row dump at the wave-2 gate: expect exactly rows 05 / 06 / 83.

## Checked

- [x] `.agents/cpp/agent.md` §10 applied to the diff.
  - The longest line is 99 columns.
  - No UB: `memcpy` bit reads; `snprintf` into a 160-byte buffer whose worst-case line is about 114 characters.
  - Lifetime: the `DetPool` outlives the engines that borrow it.
  - No nested pool.
  - Both builds have 0 warnings (`p9-s2r-g` log, `p9-s2r-e` receipt).
- [x] The diff stays inside the lane's scope: one test file plus sprint-dir docs, logs and tools. No `.mypy_cache` or
  `__pycache__` is committed.
- [x] The report's evidence matches its claims. I re-ran the lane's S2R-1 scan (byte-identical output), re-counted
  every log and re-derived the opcode list and the S2R-1 list from source. The two exceptions are S-3 and S-4.
- Not done (dispatch: build nothing): no build or gtest run. No pytest exists for this lane; the scan script re-run is
  the one re-executed command.
