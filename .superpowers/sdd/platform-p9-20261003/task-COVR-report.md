# Task COVR report: Debug vs Release bytes of the COV container (ruling COV-12 (b))

Lane COVR, pool-24, branch `feat/p9-cov-20261003`. Base `75428364` (COV re-review 2) plus a merge of root
`d7f762cf` (code head `30b9c9d2`): merge commit `9a438f5e`. 2026-10-04. Synthetic data only (the lane fixture). No
real data, nothing dated 2024 or later, no 2020-2023 statistic read. Trial ledger untouched.

**STATUS: DONE.** The cause is found. It is class (i), the C runtime, so ruling COV-12 (b) / OD5-2 applies and no
kernel was changed. No test pins container bytes captured under one CRT, so no M2c-RED-2 split was needed. No
production source, pin, golden or expected value changed. The only code added is a DISABLED_ diagnostic test.

## 1. Cause (one line)

`std::exp2(-1.0 / H)` returns different bits in the two builds. Debug (`/MDd`) resolves it to `ucrtbased.dll`
10.0.22000.832 and Release (`/MD`) to `ucrtbase.dll` 10.0.26100.7623. Two of the risk model's default half-lives are
affected: H = 84 (vol and specific) and H = 504 (correlation and the Kish count). H = 42 (VRA) and H = 252 (Newey-West)
give equal bits in both builds. The call sites:
- `atx-impl/src/strategy_risk_model.cpp:490`: the `LaggedEwma` constructor's `decay_`, reached from the `Model`
  constructor at `:694` (factor_fast, HL 84), `:696` (factor_corr, HL 504) and `:697` (spec_fast, HL 84).
- `atx-impl/src/strategy_risk_model.cpp:853`: `update_vra`'s Kish decay, HL 504. This feeds DIAG `eigen_effective`
  and atx-cov-v1's S2b length.

| H | argument `-1.0/H` (both builds) | Debug `ucrtbased` | Release `ucrtbase` | correctly rounded |
|---|---|---|---|---|
| 84 | `bf88618618618618` | `3fefbcae0646865c` | `3fefbcae0646865d` | `...865d` (Release) |
| 504 | `bf60410410410410` | `3feff4bdc935c3a0` | `3feff4bdc935c39f` | `...c39f` (Release) |
| 42 | `bf98618618618618` | `3fef79e9aca4c53e` | same | same |
| 252 | `bf70410410410410` | `3fefe97f887a34a8` | same | same |

Release has the correctly rounded value. Debug is 1 ULP off.
- Over H = 1..2000, `exp2(-1/H)` differs between the two DLLs on 1,032 values. `ucrtbase` is correctly rounded on
  1,999 of them and `ucrtbased` on 967 (`crt-exp2.txt`).
- On 200,000 random arguments in [-1, 0], 76,499 differ (`crt-scan.txt`).
- The other libm calls on the estimator path showed no difference on 200,000 random arguments each in the
  estimator's ranges: `log`, `exp`, `sin`, `cos`, `sqrt`.
- `pow(d, l)` (`:523`, the Newey-West lag weights) gives equal bits in both DLLs for both builds' d values. So it is
  not a second cause on this fixture. (It is a known 0.2% `pow` difference elsewhere, M2c-RED-2.)

Classification:
- **(i) C runtime call. Yes**, `exp2`.
- **(ii) Contractible or foldable form in our code: no.**
  - `strategy_risk_model.cpp` / `strategy_risk_verb.cpp` are compiled `/O2 /Ob2` in both builds
    (`atx-impl/CMakeLists.txt:115-122`, the Debug /O2 list).
  - No `/arch`, no fast-math. x64 SSE2 has no FMA.
  - The diagnostic's replica of the factor block runs at /Od in the Debug test TU. It equals the /O2 production
    output bit for bit in every session of both builds.
- **(iii) Build- or worker-dependent reduction order: no.** The estimator, S2b and the writer start no thread (see 4).
- **(iv) Other: no.** Every stage is deterministic run to run, and the swap below explains every differing byte.

## 2. Method

**Diagnostic.** `atx-impl/tests/strategy_cov_stage_diag.cpp`, test `CovStageDigest.DISABLED_DumpPerStageOnTheLaneFixture`.
- Fixture: `CovPanel(300, 200, 23)`, root's COV-7 fixture. Its role / fields manifests are `c93324e7...` /
  `7e0cc52b...` in both builds, equal to root's copy.
- It writes one FNV-1a digest per stage and session to `ATX_COVR_DUMP`, for both recipes, in pipeline order:
  - inputs (11 panel arrays);
  - the CRT constants `exp2(-1/H)` with their arguments, and `pow(d, l)`;
  - exposures (slot, eligible, styles) and the WLS factor returns;
  - the recursive EWMA moments (fast variance, NW zero-lag and HAC, correlation);
  - the Kish n_eff, bias and lambda^2;
  - the fully observed block: serial variances, raw block, after the eigenvalue floor, after S2b, and the gammas;
  - the final F and its structural border;
  - specific risk: bias, lambda^2, structural bounds, variance, flags;
  - the encoded container block (the verb's ContainerSink encoding).
- The factor side is a replica built from the production `LaggedEwma` / `serial_adjusted_variance` /
  `eigen_adjust_block`. It is checked equal to the production F, gammas and S2b outcome bit for bit in every session.
  Replica mismatches: 0 in all four runs.
- Two isolated probes use inputs that are identical in both builds: the eigen kernel on a planted block (m = 10, 30)
  and `LaggedEwma` at HL 21 / 42 / 84 / 252 / 504.
- With `ATX_COVR_OUT` it also writes the fixture and runs the `risk` verb in-process with root's argv for both
  recipes.

**Counterfactual (`ATX_COVR_SWAP=exp2`).**
- The test rebinds the process's `exp2` import (one IAT slot, in the test exe) to the other CRT's export, then
  restores it.
- Debug + `ucrtbase!exp2` reproduces **every** Release digest: all 24 per-session stages × 300 sessions × 2 recipes,
  all constants and probes.
- Release + `ucrtbased!exp2` reproduces every Debug digest.
- The verb containers then differ only in META `/producer/executable_sha256` and the two root fields (124 bytes).
  All BLOCKS, BLOCK_HASHES, the DATE_INDEX and all 10 other output files, including `bias.csv`, are equal
  (`cmp-containers-debugswap-vs-release.txt`, `cmp-containers-debug-vs-releaseswap.txt`).
- So `exp2` is the whole build-to-build difference on this fixture, for both recipes. This covers the eigen kernel
  (engine /Od vs /O2, Box-Muller `log` / `sin` / `cos`), Eigen, the structural specific model (`exp` / `log`) and the
  bias harness.

## 3. Stage table (Debug / Release / swaps)

Fold = SHA-256 (12 hex) of the stage's 300 per-session digests. "differ" / "first" compare Debug with Release. The
full table, including every constant and probe, is `cov-evidence/covr/stage-table.md`. Per-session detail is in the
four `*.dump` files and `cmp-stages-*.txt`.

| key | Debug | Release | Debug + ucrtbase exp2 | Release + ucrtbased exp2 |
|---|---|---|---|---|
| `exp2_arg` HL 84 / 504 | `bf886186...` / `bf604104...` | same | same | same |
| `exp2` HL 84 | `3fefbcae0646865c` | `3fefbcae0646865d` | `...865d` | `...865c` |
| `exp2` HL 504 | `3feff4bdc935c3a0` | `3feff4bdc935c39f` | `...c39f` | `...c3a0` |
| `exp2` HL 42 / 252 | `3fef79e9aca4c53e` / `3fefe97f887a34a8` | same | same | same |
| iso eigen m10 / m30 out | `c940b51c...` / `c936300a...` | same | same | same |
| iso EWMA HL 42 / 252 | `542652bd...` / `57294d46...` | same | same | same |
| iso EWMA HL 21 / 84 / 504 | `c6c5...` / `da59...` / `9147...` | `b095...` / `4fd4...` / `0241...` | = Release | = Debug |

atx-risk-v1.1 (atx-cov-v1 is the same picture; its S2b stages `F.eigen`, `F.eigen_gamma` differ 237 / 300 from 63):

| stage | Debug | Release | Dbg + rel exp2 | Rel + dbg exp2 | differ | first |
|---|---|---|---|---|---|---|
| x.slot / x.eligible / x.styles | `d050025d0d3c` / `40bb503d9c8b` / `449d8bad2bbd` | same | same | same | 0 | - |
| f.return (WLS) | `b62e566e96bc` | same | same | same | 0 | - |
| ewma.fast_var (HL 84) | `242217d46ae3` | `1e8fe10b226a` | `1e8fe10b226a` | `242217d46ae3` | 298 | 2 |
| ewma.nw_zero_lag / nw_hac (HL 252) | `e2c95a759868` / `2e2d0291ade7` | same | same | same | 0 | - |
| ewma.corr (HL 504) | `b38277ddcd43` | `c17acf1a671c` | `c17acf1a671c` | `b38277ddcd43` | 298 | 2 |
| vra.kish_n_eff (HL 504, `:853`) | `7a513d6765f3` | `6cbba7c7a9cf` | `6cbba7c7a9cf` | `7a513d6765f3` | 280 | 2 |
| vra.bias_factor | `2830d829e322` | `5e9d9253d6d8` | `5e9d9253d6d8` | `2830d829e322` | 201 | 65 |
| vra.lambda2_factor | `aa94cec95f2a` | `249b6d2c2fad` | `249b6d2c2fad` | `aa94cec95f2a` | 204 | 84 |
| F.serial_var | `af5e9d6f0e9e` | `b24a6cc8853b` | `b24a6cc8853b` | `af5e9d6f0e9e` | 237 | 63 |
| F.raw | `36ec4f741693` | `73ada72f6792` | `73ada72f6792` | `36ec4f741693` | 237 | 63 |
| F.floor (= F.eigen under v1.1) | `0ba0dbea7a9c` | `ce72acf520ec` | `ce72acf520ec` | `0ba0dbea7a9c` | 237 | 63 |
| F.final (border + VRA) | `7db2a323c3c6` | `f0844a5aaf3d` | `f0844a5aaf3d` | `7db2a323c3c6` | 237 | 63 |
| F.structural_border | `adc051a417e0` | `67ecd10ce4f2` | `67ecd10ce4f2` | `adc051a417e0` | 173 | 63 |
| D.structural_bounds | `1fb54b5735d3` | `9b8f24ea1c4d` | `9b8f24ea1c4d` | `1fb54b5735d3` | 47 | 252 |
| D.specific_variance | `3fdc7d6b0145` | `c98f5f825ee6` | `c98f5f825ee6` | `3fdc7d6b0145` | 48 | 252 |
| D.bias_specific / lambda2_specific | `4a06cb758834` / `11a746030c7a` | `3f46f6cf6469` / `154b4855ac69` | = Release | = Debug | 28 / 8 | 254 / 273 |
| D.flags | `d5dda953736e` | same | same | same | 0 | - |
| encoded block | `63af8bcc63cf` | `76347f2af0a7` | `76347f2af0a7` | `63af8bcc63cf` | 293 | 2 |

How it reads:
- The **first differing value** is `LaggedEwma::decay_` for factor_fast (HL 84), computed at `:490` when `Model` is
  constructed, before session 0. Its input `-1.0/84` is bit-identical in both builds.
- In per-session values, the first differences are the fast variance, the correlation moments and the Kish count, at
  session 2. That is the first session where a decay multiplies a non-zero moment.
- Nothing upstream of these moves: exposures, factor returns, and the HL-252 Newey-West moments are all equal.
- In the container, the first differing block is session 2, DIAG slot `eigen_effective` (`:853`). That matches
  root's `cov-diff` "first difference at block 2".
- FACTOR_COV first differs at session 63, the first forecast (`min_factor_history`). SPECIFIC first differs at 252,
  the first specific forecast (`structural_history`).
- The verb container numbers equal root's M2g figures exactly (`cmp-containers-debug-vs-release.txt`):
  - v1.1: 31,552 / 32,556 finite F cells differ, median 17 ULP, max 7,976,655 ULP; specific variance 48 sessions;
    Blocks 45,796 + BlockHashes 9,335 bytes.
  - atx-cov-v1: 31,908 cells, median 24 ULP.

## 4. Determinism inside one build (fixture, both recipes)

- **Release = Release.** The Release risk exe (`9d9fc4fa`) wrote all 12 output files byte-identical, including the
  whole `model.atxcov` and its root, over two runs and a third run under CPU affinity mask 1 (a child probe reported
  `0x1`; the default is `0xffff`) (`release-det.txt`).
- **Debug = Debug** in the same three runs (`debug-det.txt`).
- **Workers.** There is no worker setting for the estimator or the writer. `run_risk_model`, S2b
  (`eigen_adjust.hpp:49`: "no parallelism") and `CovContainerWriter` start no thread. The only threads on the COV path
  are the readers' full verification (`cov_container.cpp:798`, `min(8, hardware)` workers, a fixed partition). It
  writes no bytes and reports the same first failure for any pool size. "1 vs N" is therefore 1 CPU vs all 16.
- Both builds reproduce root's M2g outputs on the same fixture. BLOCKS, BLOCK_HASHES, DATE_INDEX and the 10 payload
  files are equal to `p9-m2g-cov7/{dbg,rel}-{v11,covv1}` (`debug-root-match.txt`, `release-root-match.txt`). The lane
  build is root's estimator byte for byte.

## 5. What changed, what did not, and why

- **Changed: test-only.**
  - `atx-impl/tests/strategy_cov_stage_diag.cpp` (new, DISABLED_; it is not a `*_test.cpp`, so the `atx-impl-tests`
    glob does not take it).
  - One source-list entry in `atx-impl/tests/CMakeLists.txt` (`atx-impl-strategy-target-tests`).
  - Commits `b54986a0`, then `d0647e8c` (slip: `tlhelp32.h` before `windows.h`, build p9-covr-a failed on it).
- **Not changed:** any production source, header, kernel, pin, golden, fixture expectation, or test expectation.
  `git diff d7f762cf HEAD` touches only those two test files.
- **Why no kernel fix.**
  - The cause is class (i). COV-12 (b) and OD5-2 say no kernel change: a deterministic in-repo libm is P10.
  - A fix in our code would also not meet COV-12's condition. A correctly rounded `exp2` (or binding Debug to
    `ucrtbase`) leaves every Release byte here unchanged. But it moves every Debug-built byte: the Debug store of
    record `v8-risk-lo3-v10` (root found that the Debug build reproduces it), and `cov-diff --legacy` identity of
    Debug-built stores.
- **Why no M2c-RED-2 split.**
  - No test pins container (or estimator) bytes captured under one CRT.
  - Every COV and risk test compares within one process or one build: truncation, legacy-vs-container,
    recipe-vs-recipe, round trips, the band.
  - `grep` finds no pinned digest in the COV / risk test files. The only pinned digests in the suites (SpoPin,
    SpoV3Pin) do not use the estimator and pass in Release.
  - Nothing reads `atx-equity-strategy-risk` or `v8-risk-lo3-v10` in `scripts/tests`, `atx-impl/tests` or
    `atx-engine/tests`.
  - So no test is skipped and none is edited.

## 6. Tests (same filters as root's M2d)

| suite (exe) | Debug p9-covr-b | Release p9-covr-c |
|---|---|---|
| `RiskCovContainer.*` (atx-engine-risk-tests) | 21 pass | 21 pass |
| `CovTruncation.*:CovEigen.*:CovRecipe.*:CovStore.*:CovBias.*:CovDiff.*` (impl target) | 29 pass | 29 pass |
| `RiskWls.*:RiskEwma.*:RiskBias.*:RiskModel.*:RiskVerb.*:RiskRobust.*:RiskTarget.*:VolTarget.*:NavBookRule.*:Spo*:NavV7Hook.*` | 106 pass | 106 pass |
| `Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` (engine) | 431 pass, 1 skip | 430 pass, 2 skip |
| `CovStageDigest.*` (DISABLED_, run on demand) | 1 pass (x2: plain, swap) | 1 pass (x2) |

- 0 failed in either build.
- Skips:
  - `RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery` (nightly, both builds).
  - Release only: `RiskSectorColumnsById.CheckedIndexingCatchesThePreW0Read`, a pre-existing NDEBUG skip ("checked
    indexing is a Debug-build property", `risk_w0r0_sector_columns_test.cpp:170`). It is not COV and not CRT.
- The regress filter has 106 tests where root's M2d had 104: later merges added tests.
- **Before vs after:** the existing tests' sources and every production source equal root's `30b9c9d2`. So "Release
  today" is the Release column, before and after this change. This lane adds one disabled test and moves no count.

Builds (all through `research-build.ps1`, target-scoped: `atx-impl-strategy-target-tests,atx-engine-risk-tests,
atx-equity-strategy-risk`, 4 jobs, 0 warnings in our code):

| tag | preset | source | result | wall / TUs | exes (sha256 prefix: target-tests / risk-tests / risk exe) |
|---|---|---|---|---|---|
| p9-covr-a | equity-dev | `b54986a0` | exit 1 (diagnostic include order) | 682 s / 392 | - |
| p9-covr-b | equity-dev | `d0647e8c` | exit 0 | 22 s / 9 | `055a075d` / `cd5dfe4b` / `813c9efd` |
| p9-covr-c | equity-rel | `d0647e8c` | exit 0 | 922 s / 392 | `b042b4e9` / `f2213423` / `9d9fc4fa` |

- The Release warnings are the pre-existing third-party `databento-cpp` `getenv` deprecation. Debug has only the
  spdlog `/MP` driver notes.
- Each tree was configured at its build's source SHA. The Debug configure was re-run after each commit.
- Receipts: `build-equity/mega-p9-covr-{a,b,c}-*` (copies of the receipts in `cov-evidence/covr/`).
- The Release tree is new in pool-24 (deps fetched into `deps/equity-rel`).

## 7. What root must re-prove at merge

1. The merge is test-only. Confirm `git diff` is limited to the two files above.
2. Rebuild `atx-impl-strategy-target-tests` in Debug and in Release, and reproduce the counts in section 6. The new
   test shows as 1 disabled test and enters no total.
3. Optional, recommended once: run the diagnostic in both builds, plain and with `ATX_COVR_SWAP=exp2`, then
   `covr_cmp.py stages` / `containers`. Expect Debug + ucrtbase exp2 = Release on every key except the verb's
   whole-file SHA (META exe hash).
4. No `dsl_vm` / `ic_result` / store / fixture re-pin is involved. The risk exe is unchanged, so `cov-diff --legacy`,
   S9, the bench link and the fixture chain need no re-run beyond root's normal merge checks.

## 8. What root must check on real data before D-COV (COV-12 (b), cause (i))

The Debug = Release precondition of COV-7 is replaced by Release = Release.
1. Record `ucrtbase.dll` version and SHA-256 before the runs (M2c-RED-2 (d)). Today: 10.0.26100.7623, `2c08e77f...65ef`.
   `ucrtbased.dll` 10.0.22000.832 is `19e79e91...f48d`. The atx-cov-v1 store's bits depend on `ucrtbase!exp2` from
   now on.
2. On TRAIN (`train-2020-2023-lo3`, the D-COV argv), for each recipe, with the same Release risk exe:
   - Run twice at full affinity and once under affinity mask 1 (`covr_cmp.py det` does this).
   - Every output file must be byte-identical, including the whole `model.atxcov` and its root.
   - Compare SHA-256 only; read no statistic.
   - Keep the bounded runner's 600 s cap. A covv1 TRAIN run that exceeds it is a root timing issue, not identity.
3. Do not rebuild `v8-risk-lo3-v10` with Release and expect its pins to hold: they are Debug-CRT bytes. P9-X / Y-1
   keep reading that pinned Debug-built store, and the atx-cov-v1 store of record is Release-built (COV-12 (a)).

## 9. P10 note (the CRT is the cause)

- `exp2` belongs in the OD5-2 deterministic-libm item next to `pow` / `tanh`. In the Debug SDK CRT it is not
  correctly rounded on about half of `-1/H` arguments. In the OS CRT it is correctly rounded on 1,999 of 2,000.
- An in-repo correctly rounded `exp2` (for example CORE-MATH `cr_exp2`, MIT, contraction-free) would make the half-life
  decays a function of source alone.
  - On this fixture it leaves every Release byte unchanged (`ucrtbase` is already correctly rounded at H = 42 / 84 /
    252 / 504).
  - It moves Debug bytes: the Debug-built `v8-risk-lo3-v10` would need a one-time ruled re-pin, or stay frozen as an
    artifact no build reproduces.
  - A cheaper interim is to compute `2^(-1/H)` once per half-life with a correctly rounded routine; only the integer
    half-lives are ever used.
- The same class, not checked here, applies to the engine's run-time `exp2` sites:
  - `atx-engine/src/risk/cov_ewma.cpp:20`;
  - `atx-engine/src/risk/vol_regime.cpp:65`;
  - `atx-engine/include/atx/engine/risk/horizon.hpp:72`;
  - `atx-engine/include/atx/engine/book/two_speed.hpp:49` (two-speed-v1 fast-sleeve weight).
- `admm_schedule.hpp:78` is `exp2` of an integer: exact.

## 10. Files

- Report: `.superpowers/sdd/platform-p9-20261003/task-COVR-report.md`.
- Evidence, `.superpowers/sdd/platform-p9-20261003/cov-evidence/covr/`:
  - dumps `{debug,release}{,-swap-exp2}.dump`;
  - `stage-table.md`, `cmp-stages-*.txt`, `cmp-containers-*.txt`, `cmp-debug-vs-debugswap.txt`;
  - `{debug,release}-det.txt`, `{debug,release}-root-match.txt`;
  - `crt-exp2.txt`, `crt-scan.txt`, `risk-exe-crt-imports.txt` (Debug exe imports `exp2` from `ucrtbased.dll`,
    Release from `api-ms-win-crt-math-l1-1-0.dll`);
  - gtest logs `{debug,release}-gt-*.log`, diagnostic logs, configure logs, receipts, `p9-covr-a-failure.txt`;
  - tools `tools/covr_cmp.py` (stages / containers / det), `covr_table.py`, `covr_root_match.py`, `crt_exp2.py`,
    `crt_scan.py`.
- Code: `atx-impl/tests/strategy_cov_stage_diag.cpp`, `atx-impl/tests/CMakeLists.txt`.

## 11. Open questions (one line each)

1. COV-12 (b) cause (i) confirmed. Does root run D-COV on Release now, with section 8 step 2 (Release = Release on
   TRAIN) as its gate?
2. Should the M2c-RED-2 (d) `ucrtbase.dll` guard name the atx-cov-v1 store explicitly (its bits depend on
   `ucrtbase!exp2`)?
3. P10: should `exp2` join the OD5-2 deterministic-libm item, with the `v8-risk-lo3-v10` re-pin question decided there?
