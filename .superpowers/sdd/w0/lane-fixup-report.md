# Lane W0-FIXUP report — integration regressions

## Outcome

**DONE.** Two of the four failures that I0a found in the whole `atx-impl-tests` executable are
fixed on this branch:

- `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` (CRLF checkout).
- `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` (fixture data that could not produce a
  finite value once the A-09 guard landed).

The other two fail on integration as reported. They sit in files owned by lane I0b, which is still
in flight, so they are left to I0b:

- `StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput`
- `EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials`

No other regression was found. All eight engine test executables pass whole. After the fixes,
`atx-impl-tests` has 526 passed, 5 skipped and 2 failed; both failures are owned by I0b.

## Branch / SHA / base / pool

- Branch `feat/w0-fixup`, pool `C:\atx-wt\pool-2`, run id `aes-w0-fixup` (the lease is held by
  the orchestrator; this lane never ran `lease-worktree.ps1`).
- Base: `feat/w0-integration` @ `16a2ae35b0e1b18b37217a5bee8c16bf9c8c0214`.
- Code commits: `9f1d2003` (.gitattributes) and `c79c2ede` (fixture). The commit that adds this
  report is the tip of the branch. The tree is clean after it.

## Files changed

| File | Change |
|---|---|
| `.gitattributes` | `*.jsonl text eol=lf`, plus `.superpowers/sdd/tier1-parity/*-headroom-observations.jsonl -text` so the two jsonl files that were committed with CRLF blobs keep their exact bytes (see Deviations). |
| `atx-impl/tests/fundamental_zoo_test.cpp` | `synthetic_records`: operating cash flow is now `(12 + q) * s` instead of `12 * s`, and a comment explains why. No assertion changed and no zoo expression changed. |
| `.superpowers/sdd/w0/lane-fixup-report.md` | This report. |

No production source changed. `ts_ops.hpp` is unchanged, because the A-09 guard turned out to be
correct (see below).

## Per-failure table

| # | Test | Fails on integration? | Root cause | Owner | Fix / disposition |
|---|---|---|---|---|---|
| 1 | `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` | Yes, in any checkout with `core.autocrlf=true`. It is pre-existing and not caused by W0 code, because no W0 commit touches the ledger, its test or `trial_ledger.*`. | With autocrlf, `atx-engine/reviews/trial-ledger.jsonl` (an LF blob) is checked out as CRLF. `verify_trial_ledger` hashes the raw line bytes and rejects CR: `ParseError: trial ledger: CR in line 0 (LF endings only)`. | fixup | **FIXED.** `.gitattributes` `*.jsonl text eol=lf`. Every LF jsonl blob is unchanged; the ledger blob stays `4c813122ea7735145cf621182e2bf63936ff2f57` at both the base and HEAD. The working copies were re-checked-out as LF. `CMakePresets.json` still reads `attr/` unspecified and `w/crlf`. |
| 2 | `StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput` | Yes | E0a changed the eval defaults to `execution_delay 1` + `TwoHorizonV2`, so the last h=63 label needs one more session. `stage_equity_ic.cpp:1171` still computes `common_sample_dates = dates - kHorizons.back()` without subtracting `eval::label_embargo(maxH, delay)`. The last common-sample date has no label, which gives `dates_emitted` 6 instead of 7, `common_prefix_gaps` 1 instead of 0, `summary_reportable` false, `ic_mean` null, and ic.csv/coverage.csv 31361 lines instead of 32001. | **I0b** (`stage_equity_ic.*`) | Not touched. This is the existing W0b integration note to I0b ("`common_sample_dates` must subtract `eval::label_embargo(maxH, delay)`"). |
| 3 | `EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials` | Yes (`n_raw` 9, expected 12, `stage_equity_mine_cli_test.cpp:221`) | A-01 average rank ties (A0). In the fixture, volume is constant, so `rank(volume)` is an all-tie cross-section. It becomes a constant 0.5, and it and its two decayed forms are degenerate, so 9 of 12 candidates register as trials. | **I0b** | Not touched. I0b already carries the re-pin commit `a28a6c1e` ("re-pin mine smooth-window trial count after A0 average rank ties (A-01)"). |
| 4 | `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` ("acc_ts produced no finite cell") | Yes | Degenerate fixture data. The A0 guard is not over-broad. `acc_ts: rank(-1 * ts_zscore(accruals, 252))`. `accruals = (NI - CFO) / mean(assets, assets_lag1y)` is scale-free, and the synthetic filings scale every raw field by one factor `s` (NI = 10·s·sign, CFO = 12·s, assets 100·s / 90·s). Accruals is therefore the constant (10·sign − 12)/95 per instrument every quarter, to within 1–2 ulp (measured relative spread ≤ 3.3e-15). Every 252-session window is flat. The A-09 guard (`tsv_is_flat`, relative tolerance 1e-10) makes `ts_zscore` NaN (0/0), and `rank` of an all-NaN row is NaN. | fixup (test data) | **FIXED in the fixture (option b):** CFO = `(12 + q)·s`, so accruals genuinely changes each quarter. The expectation is unchanged, and so is the pre-registered expression. |

## Acceptance table (RULES §4; added in fix pass 1)

Measured figures are from the fix-pass-1 runs on the merged tree (`dc821303` = this branch +
`feat/w0-integration` @ `2170a259`); see "Fix pass 1" for commands and output.

| # | Brief acceptance item | Test(s) | Measured result | Status |
|---|---|---|---|---|
| 1 | Ledger test passes from repo root with `core.autocrlf=true`; `*.jsonl` LF; no blob changes; `CMakePresets.json` keeps CRLF | `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` (exe from repo root); `git ls-files --eol` | `[ OK ]` (26 ms). `trial-ledger.jsonl` `i/lf w/lf attr/text eol=lf`; `CMakePresets.json` `i/lf w/crlf`; `git diff feat/w0-integration HEAD --stat` lists no `*.jsonl` | MET |
| 2 | `FundamentalZoo.FixtureParsesTypechecksAndEvaluates` passes, root cause proven, fixture (not expectation) fixed with justification | `FundamentalZoo.FixtureParsesTypechecksAndEvaluates`; `FundamentalZoo.FixtureAccrualsVaryOverTime` (new, fix pass 1) | both `[ OK ]`; ctest `^FundamentalZoo` 4/4 (1 opt-in skip). Mutation check: with CFO = 12·s restored, both fail (`acc_ts produced no finite cell`; `positive_sd` 0, `max_rel` 0 vs 1e-3) | MET |
| 3 | `StageEquityIc.*` / `EquityMineCli.*` failures confirmed and attributed to I0b, not touched | whole `atx-impl-tests` | both still fail with the root causes in the per-failure table (rows 2, 3); files untouched | MET |
| 4 | Every other touched executable green, or each failure attributed with root cause | 8 engine exes + `atx-impl-tests`, whole | 8 engine exes exit 0 (alpha 704, factory 299, learn 193, data 238+14 skip, eval 251, combine 183, risk 470+1 skip, book 122). `atx-impl-tests` 536 ran: 517 passed, 5 skipped, 14 failed = 2 I0b (rows 2, 3) + 12 arriving with the B0 merge (B-02 ×8, B-04 ×4; I0b-owned call sites, attributed in "Fix pass 1") | MET (all failures attributed) |
| 5 | No test weakened; report at `.superpowers/sdd/w0/lane-fixup-report.md` | diff review | No assertion, tolerance or skip changed; one assertion-only test added | MET |

## Defect table (RULES §4; added in fix pass 1)

| ID | Disposition |
|---|---|
| A-09 | NOT REOPENED: the guard is correct. The `acc_ts` NaN is the true 0/0 on a degenerate fixture (item 4 below). The fixture now varies, and `FundamentalZoo.FixtureAccrualsVaryOverTime` pins that. The ResearchFast Welford path, which has no guard, is DEFERRED to W1-A1 (integration notes). |
| (none cited) | The CRLF ledger failure is an environment defect with no register ID. CLOSED by `.gitattributes` (`9f1d2003`). |

## Item 4: A-09 decision, based on the evidence

The question was whether the A0 guard is over-broad, returning NaN for operators that are
well-defined on a flat window (option a), or whether the NaN is mathematically correct (option b).

**The answer is (b).** Evidence:

1. **Which operators the guard touches** (read at `ts_ops.hpp:151-153, 216-232, 274-327,
   922-1175, 1186-1244`). The guard runs only in `tsv_var` (var/std → exactly 0, the true
   population value), `tsv_lin_fit` (slope 0, resid 0, rsquare NaN), `TsZscore`/`TsSkew`/`TsKurt`
   (NaN, which is 0/0), `TsCorr` (NaN) and `TsRegression` on a flat predictor (NaN). `delay`,
   `delta`, `ts_mean`, `ts_sum`, `ts_rank`, `ts_min`/`max`, the median, decay, `ts_scale` and every
   other operator never consult it. The NaN cases are exactly the 0/0 statistics, and the
   well-defined ones (var 0, slope 0, resid 0) return their true value. That is not over-broad.
2. **Measured on the failing fixture** with a temporary diagnostic test, run under three
   policies: default, `FlatGuard::NoneV1` only, and `legacy_v1()`. The test was built, run and
   then deleted, and was never committed. Verbatim lines:

```
[diag] fixture=old policy=default    ts_std(accruals, 252) / abs(ts_mean(accruals, 252))  finite=336/3600 zeros=336 max|v|=0.000e+00
[diag] fixture=old policy=flatNoneV1 ts_std(accruals, 252) / abs(ts_mean(accruals, 252))  finite=336/3600 zeros=  0 max|v|=3.291e-15
[diag] fixture=old policy=default    ts_zscore(accruals, 252)                             finite=  0/3600 zeros=  0 max|v|=0.000e+00
[diag] fixture=old policy=flatNoneV1 ts_zscore(accruals, 252)                             finite=336/3600 zeros=  0 max|v|=1.289e+00
[diag] fixture=old policy=legacy_v1  ts_zscore(accruals, 252)                             finite=336/3600 zeros=  0 max|v|=1.289e+00
[diag] fixture=old policy=default    rank(-1 * ts_zscore(accruals, 252))                  finite=  0/3600 zeros=  0 max|v|=0.000e+00
[diag] fixture=old policy=flatNoneV1 rank(-1 * ts_zscore(accruals, 252))                  finite=336/3600 zeros= 28 max|v|=1.000e+00
[diag] fixture=old policy=default    ts_mean(accruals, 252)                               finite=336/3600 zeros=  0 max|v|=2.316e-01
[diag] fixture=old policy=flatNoneV1 ts_mean(accruals, 252)                               finite=336/3600 zeros=  0 max|v|=2.316e-01
[diag] fixture=old policy=default    ts_rank(accruals, 252)                               finite=336/3600 zeros=  2 max|v|=8.765e-01
[diag] fixture=old policy=flatNoneV1 ts_rank(accruals, 252)                               finite=336/3600 zeros=  2 max|v|=8.765e-01
[diag] fixture=old policy=default    delta(accruals, 63)                                  finite=2592/3600 zeros=891 max|v|=3.123e-17
[diag] fixture=old policy=flatNoneV1 delta(accruals, 63)                                  finite=2592/3600 zeros=891 max|v|=3.123e-17
[diag] fixture=new policy=default    ts_std(accruals, 252) / abs(ts_mean(accruals, 252))  finite=336/3600 zeros=  0 max|v|=3.223e-01
[diag] fixture=new policy=default    ts_zscore(accruals, 252)                             finite=336/3600 zeros=  0 max|v|=2.204e+00
[diag] fixture=new policy=flatNoneV1 ts_zscore(accruals, 252)                             finite=336/3600 zeros=  0 max|v|=2.204e+00
[diag] fixture=new policy=default    rank(-1 * ts_zscore(accruals, 252))                  finite=336/3600 zeros=  9 max|v|=1.000e+00
```

   All 336 valid 252-windows (28 dates × 12 names) of the old fixture have a relative dispersion
   of at most 3.3e-15, which is pure rounding. Without the guard, `ts_zscore` turns that rounding
   into a "signal" of up to |z| = 1.289: the pre-W0 "finite cells" were noise, which is the exact
   A-09 defect ("Flat windows give ts_zscore ≈ ±0.97 … This hits every forward-filled
   fundamental"). `ts_mean`, `ts_rank` and `delta` are bit-identical under the default and NoneV1
   policies, confirming that the guard does not touch well-defined operators. On the corrected
   fixture, the default policy and NoneV1 agree exactly (`ts_zscore` 336/336 finite, max |z|
   2.204), so the guard costs nothing when there is real variation.
3. **An independent IEEE-double restatement** (Python, scratch) of the fixture arithmetic gives
   accruals per instrument over the 5 quarters: 1 distinct value for i = 0, 4 and 8, and 2–5
   ulp-apart values otherwise. The exact value is (10·sign − 12)/95, and the maximum relative
   spread is 1.8e-15.
4. **Tradeability.** For real forward-filled fundamentals, a window that lies inside one filing
   period is bit-exactly constant for every ratio that does not use `market_cap`, because those
   ratios are computed cell-by-cell from identical inputs. A z-score against zero dispersion is undefined, so NaN is the only defensible output: a
   finite value would be an untradeable artefact. A 252-session window normally spans about 4
   filings and stays finite. The zoo expression itself is left as pre-registered.
   `FlatGuard::NoneV1` / `KernelPolicy::legacy_v1()` still give the unguarded values. In the
   diagnostic they agree with each other on the old fixture (336 finite, max |z| 1.289). The
   alpha exe's `AlphaFlatWindow_Digest`, which checks that the V1 enums reproduce the pre-W0
   digest, is green.

The fixture change is not a weakening. The assertion ("every zoo line yields at least one finite
cell") is unchanged and is now met for a real reason. Before W0, `acc_ts` passed only because
it ranked rounding noise.

## Evidence

All commands were run from `C:\atx-wt\pool-2` with `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'`, and
free RAM was checked before each build (2.69 / 2.66 / 2.34 GB).

Configure. The tree had `ATX_TEST_GROUPS=alpha;factory`, so it was reconfigured with the eight
groups under test:

```
atx-build.ps1 configure -Preset equity-dev -Groups "alpha;factory;learn;data;eval;combine;risk;book"
-- atx-impl engine_git_sha: 16a2ae35b0e1b18b37217a5bee8c16bf9c8c0214
-- Build files have been written to: C:/atx-wt/pool-2/build-equity
exit=0
```

Build on the integration code (/W4 /WX):

```
atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker atx-engine-alpha-tests atx-engine-factory-tests atx-engine-learn-tests atx-engine-data-tests atx-engine-eval-tests atx-engine-combine-tests atx-engine-risk-tests atx-engine-book-tests
[125/461] Linking CXX executable bin\atx-shm-worker.exe
[131/461] Linking CXX executable bin\atx-engine-alpha-tests.exe
[452/461] ... factory  [453] learn  [455] data  [456] eval  [457] combine  [458] risk  [459] book
[460/461] Linking CXX executable bin\atx-impl-tests.exe
exit=0
```

Whole executables on integration code (`<exe> --gtest_brief=1`, cwd = repo root):

| Executable | Exit | Result |
|---|---|---|
| atx-engine-alpha-tests | 0 | `704 tests from 273 test suites ran. [  PASSED  ] 704 tests.` |
| atx-engine-factory-tests | 0 | `299 tests from 57 test suites ran. [  PASSED  ] 299 tests.` |
| atx-engine-learn-tests | 0 | `193 tests from 31 test suites ran. [  PASSED  ] 193 tests.` |
| atx-engine-data-tests | 0 | `252 tests from 38 test suites ran. [  PASSED  ] 238 tests. [  SKIPPED ] 14 tests.` |
| atx-engine-eval-tests | 0 | `251 tests from 43 test suites ran. [  PASSED  ] 251 tests.` |
| atx-engine-combine-tests | 0 | `183 tests from 34 test suites ran. [  PASSED  ] 183 tests.` |
| atx-engine-risk-tests | 0 | `471 tests from 62 test suites ran. [  PASSED  ] 470 tests. [  SKIPPED ] 1 test.` |
| atx-engine-book-tests | 0 | `90 tests from 14 test suites ran. [  PASSED  ] 90 tests.` |
| atx-impl-tests | 1 | `533 tests from 101 test suites ran. [  PASSED  ] 525 tests. [  SKIPPED ] 5 tests.` FAILED: `FundamentalZoo.FixtureParsesTypechecksAndEvaluates`, `StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput`, `EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials` |

This integration-code `atx-impl-tests` run happened after the `.gitattributes` fix had already
re-checked-out the ledger as LF, so it shows 3 failures rather than I0a's 4. Failure 1 was
reproduced separately against the same exe, first with a CRLF copy of the ledger and then with an
LF copy (each copy with the manifest sidecar, in a scratch cwd):

```
[crlf] trial_ledger_test.cpp(634): error: Value of: head.has_value()
       ParseError: trial ledger: CR in line 0 (LF endings only)
       [  FAILED  ] TrialLedgerRepository.ExistingCp14Ledger_StillVerifies      exit=1
[lf]   [==========] 1 test from 1 test suite ran.  [  PASSED  ] 1 test.         exit=0
```

Skips are all environmental and self-skipping:

- data: ATX_DATA_DIR / ORATS smoke data absent.
- risk: the Nightly dense-oracle battery (`ATX_NIGHTLY`).
- impl: `ATX_ALPHA101_PANEL` real panel (3), APCA window on the synthetic panel (1), and
  `ATX_L10_FUNDZOO_OUT` opt-in (1).

`.gitattributes` / renormalize check. The blob hash computed through the new attributes
(`git hash-object --path=`) equals the index blob for all 13 tracked `*.jsonl`, with 0 DIFF
(before the `-text` exception the two tier1-parity files showed DIFF). `git add --renormalize --
'*.jsonl'` staged nothing. After the re-checkout, `git ls-files --eol`:

```
i/lf    w/lf    attr/text eol=lf  atx-engine/reviews/trial-ledger.jsonl   (and the other 10 LF blobs)
i/crlf  w/crlf  attr/-text        .superpowers/sdd/tier1-parity/archive1{5,6}-headroom-observations.jsonl
i/lf    w/crlf  attr/             CMakePresets.json
```

After the fixes (rebuild `atx-impl-tests atx-shm-worker`, exit 0, one TU recompiled):

```
atx-build.ps1 -Ctest -Preset equity-dev -R '^FundamentalZoo'         100% tests passed, 0 tests failed out of 3 (RealDataIcReport opt-in skip)   exit=0
atx-build.ps1 -Ctest -Preset equity-dev -R '^TrialLedgerRepository'  100% tests passed, 0 tests failed out of 1   exit=0
  (ctest runs from build-equity\, where this test self-skips "not run from the repository root", so
   the real proof is the exe from the repo root:)
atx-impl-tests.exe --gtest_brief=1 --gtest_filter=TrialLedgerRepository.*   1 test ran, [  PASSED  ] 1 test.   exit=0

build-equity\bin\atx-impl-tests.exe --gtest_brief=1        exit=1
[  FAILED  ] StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput (27128 ms)
[  FAILED  ] EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials (2507 ms)
[==========] 533 tests from 101 test suites ran. (295724 ms total)
[  PASSED  ] 526 tests.
[  SKIPPED ] 5 tests.
```

Neither the ledger test nor FundamentalZoo is among the 5 skips (the skip list is unchanged:
alpha101 ×2 sites, discover, single_alpha_capacity, the zoo real-data opt-in). The engine
executables were not rebuilt: nothing they compile changed, so the integration-code results above
still apply.

## Golden-digest old → new

None.

## Existing test expectations changed

None. The only test-file edit changes synthetic input data (see Item 4 for why that is not a
weakening).

## Deviations

1. **The `.gitattributes` exception line.** The brief asked for `*.jsonl text eol=lf`. On its own,
   that rule would change the blobs of `.superpowers/sdd/tier1-parity/archive15/16-headroom-
   observations.jsonl`, which were committed with CRLF (`i/crlf`). They would show as modified
   in every tree, and a renormalize would rewrite another sprint's archived data. They are marked
   `-text`, so their committed bytes check out unchanged.
2. **Temporary diagnostic.** A `FundamentalZooDiag.AccTsRootCause` test was added, built, run
   (output quoted above) and removed before the commit. It is not in any commit.

## Integration notes

- **Every other tree must re-checkout the jsonl files after merging this.** Git does not rewrite
  an unchanged file when only its attributes change. `feat/w0-integration` (pool-1), I0a, I0b and
  B0 keep a CRLF `trial-ledger.jsonl` until they do this, and the ledger test keeps failing
  there. The fix, from the tree root:
  `Remove-Item atx-engine\reviews\*.jsonl; git -C <tree> checkout -- atx-engine/reviews`.
  This is safe because the blobs are identical.
- **I0b:** failures 2 and 3 above. `stage_equity_ic.cpp:1171` needs the label embargo in
  `common_sample_dates` (the existing W0b note), and the mine re-pin `a28a6c1e` covers
  `EquityMineCli`. After both, `atx-impl-tests` should be fully green whole (526 + 2).
- **W1-A1 (next owner of `ts_ops.hpp`), for tradeable fundamental alphas.** A-09 is guarded on the
  AuditExact batch path only. Under `EvalMode::ResearchFast`, `ts_var`/`ts_std`/`ts_zscore`/
  `ts_av_diff` run the online Welford sweep (`tsv_welford_var_family`, `ts_is_online_variance_op`),
  which has no flat guard. A search in ResearchFast can therefore still score rounding-noise
  z-scores on flat forward-filled windows, and those alphas become NaN when re-evaluated in
  AuditExact. This is reasoned from the code and not measured here. It adds to A0's existing note
  about `ou_ar1_fit` and `ts_cov`.
- **The fixture lesson (for anyone writing fundamental fixtures):** scaling every raw field by one
  factor makes every ratio-derived field (roa, roe, leverage, accruals, gross_profitability)
  time-invariant, so any time-series statistic on it is flat. `ts_rank` on such ulp-noise windows
  ranks noise (observed max 0.8765 on the old fixture). The operator is correct; the data is
  degenerate.

## Ledger candidates

- `core.autocrlf=true` checks hash-chained `*.jsonl` ledgers out as CRLF, and
  `verify_trial_ledger` rejects CR. The fix is `*.jsonl text eol=lf`, and existing trees need a
  delete + `git checkout` of the files (attributes alone do not rewrite them).
- W0-FIXUP: the fundamental-zoo `acc_ts` failure was fixture degeneracy, not an over-broad A-09
  guard. Synthetic accruals had a relative spread of 3.3e-15 over 252 windows. Legacy z was noise
  up to 1.289; the guard gives NaN (0/0). With varying CFO, default and NoneV1 agree bit-exactly.
- `TrialLedgerRepository.ExistingCp14Ledger_StillVerifies` self-skips under ctest (cwd
  `build-equity\`). Only the exe run from the repo root exercises it.

## Fix pass 1

This pass answers the three minor findings in `lane-fixup-review.md` (0 blocker, 0 major). All
commands were run from `C:\atx-wt\pool-2` with `$env:CMAKE_BUILD_PARALLEL_LEVEL='2'`. Free RAM
before each build was 2.55, 3.26 and 2.36 GB. `ATX_TEST_GROUPS` already listed the eight groups,
so no reconfigure was needed.

### Finding 1 (minor): no acceptance or defect table

**Changed.** Added the "Acceptance table" (items 1 to 5, each with test, measured result and
MET/UNMET) and the "Defect table" (A-09 not reopened because the guard is correct; the ResearchFast
path is deferred to W1-A1; the CRLF defect is closed) above the "Item 4" section. All five items
are MET.

### Finding 2 (minor): no pre-merge of the current `feat/w0-integration`

**Changed.** `git -C C:\atx-wt\pool-2 merge --no-ff feat/w0-integration` merged `2170a259` (B0)
as `dc821303`. There were no conflicts, and 20 B0 files came in, none of them owned by this lane.
Rebuilt all ten targets on the merged tree:

```
atx-build.ps1 build -Preset equity-dev atx-impl-tests atx-shm-worker atx-engine-{alpha,factory,learn,data,eval,combine,risk,book}-tests
[17/83] Linking CXX static library lib\atx-engine.lib ... [79/83] ...book-tests.exe  [80/83] ...atx-impl-tests.exe
exit=0
```

Then every executable was run whole with `build-equity\bin\<exe>.exe --gtest_brief=1` (cwd = repo
root), after the final rebuild with the new test:

| Executable | Exit | Result |
|---|---|---|
| atx-engine-alpha-tests | 0 | `704 tests from 273 test suites ran. [  PASSED  ] 704 tests.` |
| atx-engine-factory-tests | 0 | `299 tests from 57 test suites ran. [  PASSED  ] 299 tests.` |
| atx-engine-learn-tests | 0 | `193 tests from 31 test suites ran. [  PASSED  ] 193 tests.` |
| atx-engine-data-tests | 0 | `252 tests from 38 test suites ran. [  PASSED  ] 238 tests. [  SKIPPED ] 14 tests.` |
| atx-engine-eval-tests | 0 | `251 tests from 43 test suites ran. [  PASSED  ] 251 tests.` |
| atx-engine-combine-tests | 0 | `183 tests from 34 test suites ran. [  PASSED  ] 183 tests.` |
| atx-engine-risk-tests | 0 | `471 tests from 62 test suites ran. [  PASSED  ] 470 tests. [  SKIPPED ] 1 test.` |
| atx-engine-book-tests | 0 | `122 tests from 18 test suites ran. [  PASSED  ] 122 tests.` (B0 added 32) |
| atx-impl-tests | 1 | `536 tests from 102 test suites ran. [  PASSED  ] 517 tests. [  SKIPPED ] 5 tests.` 14 FAILED (below) |

**The merge brings in 12 `atx-impl-tests` failures that are new to this branch.** B0's own report
(`lane-b0-report.md` lines 6-10, 75, 171-183 and 227ff) already records them, and the
orchestrator merged B0 knowing about them. Their root cause is B0's plan-required engine defaults
reaching atx-impl call sites that I0b owns:

| Failing tests | Count | Root cause (verbatim evidence) | Owner |
|---|---|---|---|
| `ReplayPolicyStage.BoundedIdentifiedWrapperRetainsScheduleAndExplicitFeeValidation`, `.CoherentReplacementGraphMustMatchEveryAdmittedArtifactIdentity`; `ReplayReport.WeeklyCoverageDriftExactAxesAndDeterministicManifest`, `.DollarTradeFeesAndActualCalendarBorrowReconcileCashAndAssets`, `.PolicyAllocationsBindActualDollarRowsAndModelProvenance`, `.ExplicitIntentReportBindsHoldCloseAndActualCashFlows`, `.LaterPolicyFailurePublishesNoAcceptedAllocationOrCompleteManifest`; `StageEquityBaseline.ExplicitZeroCostsDelayAndGlobalDefaultAumArePreserved` | 8 | B-02. `replay.cpp` `validate_inputs` now refuses delay 0 without the opt-in: `"replay: execution_delay_periods=0 fills at the decision close; set allow_same_close to opt in"` (`replay_report_test.cpp:559`). The other tests fail at `accepted/result.has_value()` → false. `replay_report.cpp:394` has no way to pass `allow_same_close`, and `config.hpp` has no `--allow-same-close` flag yet. | I0b (`config.{hpp,cpp}`, `replay_report.cpp` I-11 site, `stage_equity_baseline.cpp`) |
| `ReplayReport.MissingHeldPriceMapsExactSecurityAndDateWithoutCompletePublication`; `StageEquityBaseline.MissingHeldMarkPreservesBoundFailureAndNoCompleteManifest`, `.ConstrainedBookPreservesMissingHeldMarkFailureOnOriginalWindow`, `.ObservedCloseEntryConstraintBindsAvailabilityWithoutShrinkingUnion` | 4 | B-04. By default a missing held close is now liquidated (`DelistingPolicy::TerminalReturn`) instead of aborting, so these tests get `has_value()` true where they expect false. | I0b |
| `StageEquityIc.TwoRunsProduceByteIdenticalStatisticsAndPublishEveryOutput`, `EquityMineCli.SmoothWindowsAddDecayedVariantsAsTrials` | 2 | Rows 2 and 3 of the per-failure table (unchanged) | I0b |

This lane does not touch them. The fix sites are I0b-owned files, and I0b is in flight, so this
lane may not edit them. The fix is B0's integration note 1 for I0b: add `allow_same_close` to
`RunConfig`, add the CLI flag, and set the delisting policy at the I0b call sites. Nothing else
regressed on the merged tree. The fixture and ledger fixes still hold.

### Finding 3 (minor, optional): nothing pinned the fixture's non-degeneracy

**Changed.** Added the test `FundamentalZoo.FixtureAccrualsVaryOverTime`
(`atx-impl/tests/fundamental_zoo_test.cpp`). It is assertion-only and uses the same synthetic
panel and records as the zoo test. It requires:

- `ts_std(accruals, 252)` has at least one finite cell and at least one cell greater than 0;
- the maximum `ts_std / |ts_mean|` is above 1e-3. That is about 11 orders of magnitude above the
  degenerate fixture's ulp noise (≤ 3.3e-15), 7 above the guard tolerance (1e-10), and well below
  the corrected fixture's 0.322;
- `ts_zscore(accruals, 252)` has a finite cell.

No existing assertion changed.

Evidence:

```
atx-build.ps1 -Ctest -Preset equity-dev -R '^FundamentalZoo'
2/4 Test #2684: FundamentalZoo.FixtureAccrualsVaryOverTime ...........   Passed    0.14 sec
100% tests passed, 0 tests failed out of 4        (RealDataIcReport: opt-in skip)   exit=0

atx-impl-tests.exe --gtest_filter=FundamentalZoo.*:TrialLedgerRepository.*   (repo root)
[       OK ] FundamentalZoo.FixtureParsesTypechecksAndEvaluates (269 ms)
[       OK ] FundamentalZoo.FixtureAccrualsVaryOverTime (23 ms)
[       OK ] TrialLedgerRepository.ExistingCp14Ledger_StillVerifies (26 ms)
[  PASSED  ] 4 tests.  [  SKIPPED ] 1 test (RealDataIcReport)   exit=0
```

Mutation check. The degenerate fixture was temporarily restored (CFO = `(12 + 0·q)·s`), rebuilt
and run, then reverted before any commit (`git diff --stat` afterwards showed only the new test):

```
fundamental_zoo_test.cpp(383): error: ... acc_ts produced no finite cell
[  FAILED  ] FundamentalZoo.FixtureParsesTypechecksAndEvaluates
fundamental_zoo_test.cpp(417): error: Expected: (positive_sd) > (0U), actual: 0 vs 0
  every accruals window is flat (fixture degenerate)
fundamental_zoo_test.cpp(418): error: Expected: (max_rel) > (1.0e-3), actual: 0 vs 0.001
fundamental_zoo_test.cpp(422): error: ... ts_zscore(accruals, 252) produced no finite cell
[  FAILED  ] FundamentalZoo.FixtureAccrualsVaryOverTime        exit=1
```

The new test therefore fails exactly when the fixture turns degenerate. It gets there on its own
path, not through the zoo expression.
