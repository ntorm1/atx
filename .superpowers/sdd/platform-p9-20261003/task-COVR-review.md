# Lane COVR review (light; ruling COV-12 (b))

## Verdict

**APPROVE.** Required: 0. Suggested: 6 (S-1 .. S-6). None of them blocks the merge. S-1 matters for root's TRAIN
step: see "What root must check", item 4.

- (a) The attribution holds. On the lane fixture, the `exp2` swap explains every differing byte, in both directions and
  for both recipes. I found no other candidate that the swap leaves open on the fixture.
- (b) No production source changed. The only code is a `DISABLED_` gtest plus one source-list line in an
  `EXCLUDE_FROM_ALL` test executable.
- (c) No pin, golden, expected value or fixture expectation was edited. No existing test was skipped or weakened.
- (d) The logs support Release = Release on the fixture, and they name the exe and output hashes.
- (e) The counts match the logs, and the skips are the named ones.
- (f) Release does have the correctly rounded value. I checked this myself to 150 digits. There are downstream notes
  for root (item 6).

## Reviewed

- **Worktree / head:** `C:/atx-wt/pool-24`, `feat/p9-cov-20261003`, head `494ec9b2`.
- **Lane commits:**
  - `b54986a0`: the diagnostic test and the CMake line;
  - `d0647e8c`: the include-order slip (`windows.h` before `tlhelp32.h`);
  - `16325e23`: the report and evidence;
  - `494ec9b2`: the handoff.
- **Root merge `9a438f5e` (parents `75428364`, `d7f762cf`) is plain.**
  - `git merge-tree --write-tree 75428364 d7f762cf` gives `ca61c2f9`. That equals the merge commit's tree, and it
    also equals `d7f762cf`'s tree.
  - The lane side's only change since the merge base `c8ff4f1c` is `task-COV-rereview-2.md`, which root already had.
  - So the merge adds nothing and resolves nothing.
- **What `git diff --name-only d7f762cf 494ec9b2` lists (47 files):**
  - `atx-impl/tests/strategy_cov_stage_diag.cpp` (new, 572 lines);
  - `atx-impl/tests/CMakeLists.txt` (+1 source, +2 comment lines);
  - `task-COVR-report.md` and `handoff-COVR.md`;
  - 43 evidence files under `cov-evidence/covr/`.
- **Read in full:**
  - the diagnostic source;
  - the CMake hunk;
  - `covr_cmp.py`, `covr_root_match.py`, `crt_exp2.py` and `crt_scan.py`;
  - the report and the handoff;
  - every evidence `.txt` file;
  - the comparison outputs;
  - the receipts;
  - the gtest log summaries.
- **Production code read at the head:**
  - `strategy_risk_model.cpp`: every libm call site, plus `:486-526`, `:690-700` and `:812-858`;
  - `strategy_risk_model.hpp:102-109` (defaults);
  - `strategy_risk_verb.cpp:841-866`;
  - the resolved `equity-dev` / `equity-rel` presets;
  - `atx-impl/CMakeLists.txt:108-130`.

## Evidence (my own runs, all read-only, synthetic, `PYTHONDONTWRITEBYTECODE=1`)

The lane has no pytest. Instead I re-ran its comparison tools on the committed dumps, plus one CRT probe.

1. **Swap stage comparisons.**
   - Commands: `python tools/covr_cmp.py stages debug-swap-exp2.dump release.dump`, then the same with
     `debug.dump release-swap-exp2.dump`.
   - In both, the only `DIFF` keys are `verb/v11/model.atxcov` and `verb/covv1/model.atxcov`. These are whole-file
     SHA-256 values, and META carries the test exe's SHA, so the exit code is 1 by construction.
   - All 48 per-session stage rows (24 stages x 2 recipes) show "differ 0 / 300".
   - Every input, `const/*` and `iso/*` key is SAME.
2. **Plain Debug vs Release comparison.**
   - Command: `python tools/covr_cmp.py stages debug.dump release.dump`.
   - The first differing key is `const/exp2/vol_hl84` (`...865c` / `...865d`).
   - The per-stage counts and first sessions are the report's §3 table (for example, `ewma.fast_var` 298 / first 2,
     `F.final` 237 / first 63, `D.specific_variance` 48 / first 252).
3. **Stage table regenerated.**
   - Command: `python tools/covr_table.py debug.dump release.dump debug-swap-exp2.dump release-swap-exp2.dump`.
   - The output equals the committed `stage-table.md`: 79 lines, equal True.
4. **CRT probe re-run.**
   - Command: `python tools/crt_exp2.py`, on this host now.
   - Exit 0. The output is identical to the committed `crt-exp2.txt`.
   - At H = 84 and H = 504 the two DLLs differ and the Release value is the correctly rounded one. The H = 1..2000
     scan gives: differ 1032; `ucrtbase` correctly rounded 1999 / 2000; `ucrtbased` 967 / 2000.
5. **DLLs now (both match the report §8.1):**
   - `ucrtbase.dll` 10.0.26100.7623, SHA-256 `2C08E77F...65EF`;
   - `ucrtbased.dll` 10.0.22000.832, SHA-256 `19E79E91...F48D`.
6. **Independent rounding check (150-digit `Decimal`), distance from the true 2^(-1/H) in ULPs:**
   - H = 84: Release 0.429, Debug 0.571.
   - H = 504: Release 0.289, Debug 0.711.
   - So Release is correctly rounded at both. Debug is faithful (within 1 ULP) but not correctly rounded. H = 84 is not
     a near-tie, so the call is unambiguous.
7. **`git status` after all runs:** clean. No `__pycache__` was written.

## Findings by question

### (a) Attribution

**Verified by my own reading and runs.**
- Both swap directions reproduce the other build on every per-session digest of both recipes (Evidence 1).
- On the verb containers, the two committed comparisons (`cmp-containers-debugswap-vs-release.txt` and
  `cmp-containers-debug-vs-releaseswap.txt`) show the same thing for v11 and for covv1:
  - 124 differing bytes: `header.root` 32, `META` 60 (only `/producer/executable_sha256`), trailer 32;
  - all 11 block arrays differ in 0 / 300 sessions;
  - 0 date-index entries differ;
  - the 10 other output files are equal.
- So `exp2` is the whole Debug-vs-Release difference on the fixture.
- The replica check (`strategy_cov_stage_diag.cpp:529`, an `EXPECT`) records `replica_mismatches 0` for both recipes
  in all four dumps. That shows the replica stages are the production values.

**Sites.**
- `exp2` has three call sites in `strategy_risk_model.cpp`:
  - `:490`: the `LaggedEwma` constructor, used with HL 84 for vol and specific, 504 for corr and 252 for nw;
  - `:815`: the VRA decay, HL 42;
  - `:853`: the Kish decay, HL 504.
- The half-lives are compile-time defaults (`strategy_risk_model.hpp:102-109`). The verb only records them
  (`strategy_risk_verb.cpp:841-866`), and no recipe overrides them. So on the verb path, the set of sites that differ
  is fixed: `:490` (at HL 84 and 504) and `:853`.
- `:815` is on the path but gives equal bits at H = 42.
- `pow` at `:523` only propagates the difference. It gives equal bits in both DLLs for both builds' `d` values:
  `crt_exp2.py` prints a `pow` line on any mismatch, and it printed none.
- The report's "two sites" claim is therefore correct. It just does not name `:815` (S-2).

**Other candidates, and whether the swap excludes them:**
- **FMA contraction or ISA.** The resolved presets differ only in `CMAKE_BUILD_TYPE` and the FetchContent dir. Neither
  sets `/arch`, so the default is SSE2, which has no FMA instructions to contract into. Neither sets `/fp:fast`.
  `strategy_risk_model.cpp` and `strategy_risk_verb.cpp` are `/O2 /Ob2` in Debug as well
  (`atx-impl/CMakeLists.txt:114-122`). The build log of `p9-covr-a` shows the flags (`/W4 /WX`, no `/arch`). A
  code-path difference like this would not depend on the data, so it would show on the fixture. The swap excludes it.
- **Reduction order or threads.** I grepped `atx-impl/src`, `atx-engine/src/risk`, `atx-engine/include/.../risk`,
  `atx-core/src` and `atx-tsdb/src` for `std::thread`, `jthread`, `async`, `execution::par` and thread pools.
  - On the risk path, the only hit is `cov_container.cpp:798-816`: the reader's verification pool, which writes no
    bytes.
  - The other hits are `stage_discover`, `stage_equity_mine` and `stage_sweep`, which are not on the risk path.
  - There is no OpenMP.
  So this is excluded by the structure of the code and by the swap.
- **Debug-only branches.** None: there is no `NDEBUG` or `_DEBUG` in `strategy_risk_model.cpp`, its `.hpp`,
  `strategy_risk_verb.cpp` or `cov_container.cpp`.
- **`exp` / `log` / `sin` / `cos` / `sqrt` / `pow`.**
  - On the fixture, the swap excludes them, because it leaves each of them on its own build's CRT.
  - On other inputs, such as TRAIN, `exp` and `log` (`:31-32`, `:358`, `:370`, `:397`, `:624`, `:890-913`, `:1106`)
    and Box-Muller's `log` / `cos` (`:1206`) take data-dependent arguments. The swap does not exclude them there. The
    200,000-argument scan per function (`crt-scan.txt`: 0 differ, in the estimator's ranges) makes a difference
    unlikely, but it does not prove there is none.
  - This does not affect the gate: COV-12 asks for the attribution on the fixture, and it replaces Debug = Release by
    Release = Release on TRAIN. The report should say it, though (S-3).
  - `sqrt` is IEEE correctly rounded.
- **Caveat on the evidence chain.**
  - The swap ran inside the test exe, through its in-process `risk` verb, not in `atx-equity-strategy-risk.exe`.
  - Both executables link the same static `atx-impl-core` (`atx-impl/CMakeLists.txt:4`, `:49`, `:193`;
    `ATX_SHARED_LIBS=OFF`).
  - The in-process Debug-vs-Release block differences (v1.1: Blocks 45,796 + BlockHashes 9,335 bytes) equal root's
    risk-exe figures exactly. The lane's risk exes reproduce root's M2g blocks and payload files
    (`{debug,release}-root-match.txt`).
  - What is missing is a direct block comparison between the in-process run and the exe run (S-4, optional).

### (b) No production change: verified by my own reading

- The diff touches no file under `src/`, `include/` or `tools/`, and no top-level or module CMake.
- The CMake hunk (`atx-impl/tests/CMakeLists.txt:137-140`) adds `strategy_cov_stage_diag.cpp` to
  `atx-impl-strategy-target-tests`, which is `EXCLUDE_FROM_ALL` (`:114`). It changes no flag and no other target.
- The file is not a `*_test.cpp`, so the `atx-impl-tests` glob (`:40-41`) does not take it.
- The test is `DISABLED_` and also skips unless `ATX_COVR_DUMP` is set (`:405-409`). It enters no suite total.
- The C++ stays within 100 columns. `git diff --check` is clean.

### (c) No pin or test edited: verified by my own reading

- No existing test source and no expected value appears in the diff.
- The extra Release skip `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read` predates this lane: the file's
  last commit is `370f7af4`, and it is not in the diff.

### (d) Release = Release on the fixture: the logs support it

Taken from `release-det.txt` and `debug-det.txt`; I checked the tool's logic by reading it.
- The Release exe's full SHA-256 is named: `9d9fc4fa...71a1ae`. It equals the `p9-covr-c` receipt's risk exe.
- For each recipe:
  - three runs (run1 and run2 at affinity `0xffff`; run3 under mask 1, confirmed `0x1` by a child probe);
  - 12 files each, compared by full SHA-256 inside the tool: `run1 = run2 True`, `run1 = cpu1 True`;
  - the `model.atxcov` SHA-256 is printed (16-hex prefix): v11 `03227531e9a95471` x3, covv1 `6358e9a6ecf5f2ea` x3.
- Debug shows the same with exe `813c9efd...` (= the `p9-covr-b` receipt).
- The runs used different `--output` directories and all 12 files were still equal, so no output path enters any
  file.
- "1 vs N workers" is "1 CPU vs all". This is acceptable here because nothing on the write path starts a thread (see
  (a)). Affinity tests scheduling, not a pool size.
- Gap: the other 11 file hashes are printed only as booleans (S-5).

### (e) Counts: they match the logs

Taken from the logs; I grepped every summary line.

| suite | Debug | Release |
|---|---|---|
| `RiskCovContainer.*` | 21 passed | 21 passed |
| `CovTruncation.*:CovEigen.*:CovRecipe.*:CovStore.*:CovBias.*:CovDiff.*` | 29 passed | 29 passed |
| `RiskWls.*:RiskEwma.*:...:Spo*:NavV7Hook.*` | 106 passed | 106 passed |
| engine `Risk*` minus the 31-minute oracle | 432 run: 431 passed, 1 skipped | 432 run: 430 passed, 2 skipped |

- The Debug skip is `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`.
- Release also skips `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read`, with its message "checked indexing is
  a Debug-build property".
- No log has a `FAILED` line or a `0xC0000142` / `3221225794` exit.
- The logs carry no exe hash. Their build attribution rests on the receipts (`p9-covr-b` / `-c`, both at `d0647e8c`,
  `DirtyEntries 0`) and on the timings. Root re-proves the counts at merge.

### (f) Correctly rounded value; downstream notes

- **Correctly rounded: supported.**
  - `crt_exp2.py` uses an 80-digit `Decimal` reference, rounded to nearest through `float(str())`.
  - My 150-digit distance check agrees (Evidence 6).
- **Downstream:** root needs notes on three points (item 6 below):
  - the Debug-built stores of record;
  - which store and exe make up the two sides of D-COV;
  - COV step 4, which is a mixed-build comparison.

## Suggested (none blocks the merge)

- **S-1:** `task-COVR-report.md:230`, `:233`, `handoff-COVR.md:75`, and `covr_cmp.py:224-230`.
  - **Problem:** the report sends root to "`covr_cmp.py det`" for the TRAIN Release = Release check and also says
    "keep the bounded runner's 600 s cap". But `det` calls `run()`, which starts the exe directly through
    `subprocess.run` with no `--seconds` / `--max-rss-mib`. That is outside `run_bounded_research.py` (RUN-1 /
    RUN-2).
  - **Fix:** a root slip at merge. Root must not run `det` on TRAIN as written. Each run goes through the bounded
    runner, and root compares the output trees by SHA-256 itself (the logic of `tree()`). Alternatively, the tool
    could take a runner prefix.
- **S-2:** `task-COVR-report.md:15-23` (section 1, site list).
  - **Fix:** name `:815` (the VRA decay at HL 42, an `exp2` site on the path whose bits agree at the default). Say
    that `:523` `pow` only propagates. Say that the set of differing sites is fixed because the half-lives are
    compile-time defaults, not verb flags (`crt-exp2.txt` shows H = 21 also differs).
- **S-3:** `task-COVR-report.md:33` and `:81`.
  - **Fix:** say that the swap excludes the data-dependent `exp` / `log` / `sin` / `cos` only on the fixture. On
    TRAIN, they are excluded only by the 200k-argument scan, not by the swap.
- **S-4:** `strategy_cov_stage_diag.cpp:423-448` and `:565-568`.
  - **Problem:** the IAT slots are restored only on the normal path. Any `ASSERT_*` after the rebind (`:436`, `:441`,
    `:524`) returns with `exp2` still rebound for any later test in the same process. The same happens to slots
    already rebound when one `write_slot` fails.
  - **Fix:** use an RAII guard that restores in its destructor.
  - **Impact today:** none, because the test is `DISABLED_`, gated on an environment variable, and run alone.
  - **Optional:** log every module whose slot is rebound. Debug rebinds 2 slots; only the first module is named.
- **S-5:** `covr_cmp.py:271-277` (`det` output).
  - **Fix:** print the full SHA-256 of every file for every run, not only a 16-hex `model.atxcov` prefix, so that the
    TRAIN record names all output hashes.
  - **Optional:** add one in-process-vs-exe block comparison on the fixture (the caveat in (a)).
- **S-6:** `task-COVR-report.md:192`.
  - **Fix:** the skip is at `risk_w0r0_sector_columns_test.cpp:171`; `:170` is the `#if`. A trivial slip.

## What root must check

1. **Merge.**
   - The merge is test-only: `git diff --name-only` against root's head lists only
     `atx-impl/tests/strategy_cov_stage_diag.cpp`, `atx-impl/tests/CMakeLists.txt` and sprint-dir files.
   - The lane's own merge of `d7f762cf` is plain (Reviewed).
2. **Rebuild and counts.**
   - Rebuild `atx-impl-strategy-target-tests` and `atx-engine-risk-tests` in Debug and in Release.
   - Reproduce the counts in (e). `CovStageDigest.DISABLED_DumpPerStageOnTheLaneFixture` must show only as 1
     disabled test.
   - Optional: run the diagnostic plain and with `ATX_COVR_SWAP=exp2` in both builds, then run
     `covr_cmp.py stages` / `containers`.
3. **CRT guard (M2c-RED-2 (d)).**
   - Record the `ucrtbase.dll` version and SHA-256 before the TRAIN runs. Today it is 10.0.26100.7623,
     `2C08E77F...65EF` (re-read on 2026-10-04).
   - The `atx-cov-v1` store's bits now depend on `ucrtbase!exp2`. I recommend that the guard names that store
     (report open question 2).
4. **Release = Release on TRAIN (COV-12 (b), cause (i)).**
   - **Setup:** one Release `atx-equity-strategy-risk`, with its SHA-256 recorded. Use the D-COV argv (role
     `train-2020-2023-lo3` and fields `-v10` pins, `--emit-exposures all --emit-container --bias-families
     extended`), once per recipe (`atx-risk-v1.1`, `atx-cov-v1`).
   - **Runs:** run 1 and run 2 at full affinity, and run 3 under process affinity mask 1. Mask 1 is the "1 worker"
     leg. The estimator, S2b and the writer have no worker knob and start no thread; the only pool is the reader
     verification at `cov_container.cpp:798`, which writes nothing.
   - **Bounds:** every run goes through `run_bounded_research.py` (`--seconds` <= 600, `--max-rss-mib 2048`). Do not
     use `covr_cmp.py det` as written (S-1). A covv1 run that hits the cap is a timing stop, not an identity result.
   - **Compare:** every output file by full SHA-256, including the whole `model.atxcov` and `manifest.json`. With the
     same exe, META is identical, so the root SHA-256 must be equal too. Print hashes and equal / differ only; read
     no statistic.
   - **Outcome:** any difference is an identity miss, and D-COV waits.
5. **D-COV preconditions (COV-12 (a) / (c), M2c-RED-2 (e), gate report 4 (c)).**
   - COVR is merged after this review, and item 4 holds.
   - S9 (`returns_applied == false`) is checked before each COV-reading run.
   - Release only.
   - `--seconds 600`, not the list's 1800, plus `--max-rss-mib 2048`.
   - Both D-COV sides come from the same Release exe in the same step. The v1.1 side is the Release run, never the
     Debug-built `v8-risk-lo3-v10` or `p9-m2d-cov-risk-container`; pairing those with a Release `atx-cov-v1` store
     would mix the CRT difference into the recipe difference.
   - The `atx-cov-v1` store is kept and its manifest pins are recorded (W3-10).
   - The ucrtbase record from item 3 is in place.
6. **Downstream of the Debug-built stores (f).**
   - **Expected mismatches, not reds.** `v8-risk-lo3-v10` and `p9-m2d-cov-risk-container` hold Debug-CRT decay bits,
     and no Release build reproduces them. A Release-built atx-risk-v1.1 store will not equal them. `cov-diff --legacy`
     of a Release-built container against v10 will report a difference (block 2). Both are expected under this
     attribution, not a red, and never a reason to re-pin.
   - **P9-X and Y-1** keep reading the pinned Debug-built store (report section 8.3).
   - **COV step 4** compares a Release `atx-equity-strategy-targets` reading the Debug-built container against the
     Debug P9-B0 reference (`407c34ad`).
     - The store side is consistent: the container's bytes are v10's, and both were built in Debug at M2d.
     - The consumer side is Release vs Debug. It relies on G-P3 (Release = Debug for NAV), which was shown on X-5 at
       M1c, not on Y-1's `--vol-target vol-target-v1` path.
     - The engine has run-time `exp2` sites that COVR did not check: `cov_ewma.cpp:20`, `vol_regime.cpp:65`,
       `horizon.hpp:72` and `two_speed.hpp:49`, plus `pow` (M2c-RED-2). If Y-1's path reaches one of them at an H
       where the CRTs differ, step 4 can miss for a reason unrelated to the container.
     - The pre-run list already has the fallback (re-run with the Debug exe).
     - I recommend that root, before the run, either checks by reading that the vol-target-v1 / NAV path does not
       reach those sites at a differing H, or adds a like-for-like leg to the pre-run list: the Release exe reading
       `v8-risk-lo3-v10` vs the Release exe reading the container. That leg isolates the container path from the
       build. Any amendment goes in before the run.
7. **P10:** `exp2` joins the OD5-2 deterministic-libm item (report section 9), and the `v8-risk-lo3-v10` re-pin
   question is decided there.

## Checked

- [x] `.agents/cpp/agent.md` section 10 items that apply to a test-only diff:
  - includes: the slip is fixed, `windows.h` comes before `tlhelp32.h`;
  - `/W4 /WX`: the builds have 0 warnings per the report, and the receipts exit 0;
  - 100 columns;
  - no UB on the normal path;
  - the restore path (S-4).
- [x] The diff stays inside COV-12 (b) scope: a diagnostic plus docs. No kernel and no pin.
- [x] The evidence matches the claims. I re-ran the stage comparisons, the stage table and the CRT probe.
- [x] The merge of root `d7f762cf` is plain.

## Blind rule and disclosures

- No 2020-2023 return, IC, Sharpe, turnover or NAV statistic was read. No `build-equity` NAV directory was opened. No
  root commit body was read. Nothing dated 2024 or later was opened.
- Incidental: the instructed ledger grep for `COV-7` returned line 510 (root M2g), which carries the M2g rho and
  ALD-DIAG figures. The instructed `git log --format='%h %s'` showed the subject of root commit `29adf2fa`, which
  carries the Q5 q values. I used neither.
- No build was run, no real data was touched, and no file was written except this review (plus a scratch copy of the
  regenerated stage table in the session scratchpad).
- Permission refusals: none.
