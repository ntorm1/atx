# Lane W0-A0 review

Fresh adversarial review of lane W0-A0 ("Alpha kernel correctness"), pool-2, branch `feat/w0-a0`.
The reviewer was read-only on code: no source or test file was edited. The only file written is
this review.

## Verdict

**BLOCK.** There is one major finding. The new A-18 `CanonSet::insert(h, form)` default-inserts
an empty form list for a hash-only (resume) key. After that, `contains(h, form)` flips from
"seen" to "not seen". Everything else holds up. The kernels are correct. Every acceptance item
is proven by a test that would fail on the old code, all six suites and both owning executables
are green at /W4 /WX, and every `*V1` enum reproduces its pre-W0 digest bit-exactly.

## Reviewed SHA

`9cf203a3b4f50f045070467a90efba6c0336e459` (lane head). W0 base / `feat/w0-integration` =
`458d0bef480a624e258070c9d45174a9984466bf` (ancestor of HEAD, verified:
`git merge-base --is-ancestor` exit 0). The tree was clean before this review was written.

## Evidence

All commands were run by the reviewer from `C:\atx-wt\pool-2`. Free RAM was checked first
(5.58 GB), and `CMAKE_BUILD_PARALLEL_LEVEL=2`. `build-equity\CMakeCache.txt` has
`ATX_TEST_GROUPS:STRING=alpha;factory`, so no reconfigure was needed.

```
atx-build.ps1 build -Preset equity-dev atx-engine-alpha-tests atx-engine-factory-tests atx-shm-worker
  [9/12] Linking CXX executable bin\atx-shm-worker.exe
  [10/12] Linking CXX executable bin\atx-engine-alpha-tests.exe
  [11/12] Linking CXX executable bin\atx-engine-factory-tests.exe
  exit=0
atx-build.ps1 check -Preset equity-dev <alpha_w0a0_kernels_test.cpp | canonical.cpp | oracle.cpp | crossover.cpp>
  ninja: no work to do.   exit=0 (x4; objects current at HEAD; build.ninja flags for the TU carry /W4 /WX)

build-equity\bin\atx-engine-alpha-tests.exe --gtest_brief=1          alpha exit=0
  [w0a0-digest] rank (A-01)        old=0xa50ec3743580856b new=0xdd31545d3a5ad696
  [w0a0-digest] hump (A-02)        old=0x0a9ce0c6fb27f23e new=0x3159e020352402f8
  [w0a0-digest] flat (A-09)        old=0x1c75499cdd019337 new=0xc87fd2b6cc8ceb63
  [w0a0-digest] sum AuditExact     old=0xfbd765ddbfee7d5c new=0xda7bd655e7f69444
  [w0a0-digest] sum ResearchFast   old=0xfbd765ddbfee7d5c new=0xca905bf2dfb89bb4
  [w0a0] hump(ts_mean(x,5),0.1): finite cells new=48 old=0 of 60
  [w0a0] legacy ts_zscore noise: 372 finite cells, max |z| = 0.974679
  [w0a0] AuditExact ts_sum/ts_mean: new mismatches=0, legacy online mismatches=27306 of 43200 cells
  [w0a0] panel-start dependence over 1192 cells: new=0, legacy=474
  [w0a0] ResearchFast max rel err vs compensated reference: neumaier=0.000e+00 uncompensated=2.771e-12
  [==========] 703 tests from 273 test suites ran. (53550 ms total)
  [  PASSED  ] 703 tests.

build-equity\bin\atx-engine-factory-tests.exe --gtest_brief=1        factory exit=0
  [w0a0] crossover stress: children=10000 attempts=12045 scalar_slots=13591 non_literal=0 scalar_rejects=0
  [==========] 299 tests from 57 test suites ran. (62820 ms total)
  [  PASSED  ] 299 tests.

atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'   (each exit=0)
  ^AlphaCsRankTies_              100% tests passed, 0 tests failed out of 7
  ^AlphaHumpWarmup_              100% tests passed, 0 tests failed out of 4
  ^AlphaTypecheckScalarLiteral_  100% tests passed, 0 tests failed out of 5
  ^AlphaFlatWindow_              100% tests passed, 0 tests failed out of 5
  ^AlphaAuditExactParity_        100% tests passed, 0 tests failed out of 5
  ^FactoryCanonCollision_        100% tests passed, 0 tests failed out of 4
```

These numbers match the lane report line for line.

### Acceptance items (re-run and test code read)

| # | Item | Test read | Proves it? |
|---|---|---|---|
| 1 | Tied row → 0.5 | `AlphaCsRankTies_Row.AllTiedRowRanksHalfForEveryName` (VM + oracle, n = 2/5/6/7), `_Radix` (200 names, radix path), `AlphaCs_Rank.AllEqual_DeterministicOrdinal` | Yes. `EXPECT_EQ(...,0.5)` is exact. The old code gives 0..1, so the test fails on it. |
| 2 | `hump(ts_mean(x,5),0.1)` finite from t=4 | `AlphaHumpWarmup_TsMean.FiniteFromT4` | Yes. NaN for t<4, all 48 cells finite for t≥4, VM == oracle == stream. `StickyV1` gives 0 finite cells, which is the old behaviour. |
| 3 | `winsorize(x, close)` → Err | `AlphaTypecheckScalarLiteral_Analyze.WinsorizeWithPanelScalarIsErr` | Yes. The test uses `winsorize(open, close)`, which is the same slot, and checks for `InvalidArgument`. Siblings cover all 4 ops, folded literals and defaults. |
| 4 | 10k crossover, no non-literal slot | `AlphaTypecheckScalarLiteral_CrossoverStress.TenThousandChildrenHaveOnlyLiteralScalarSlots` | Yes, and it is not vacuous. `shape_broadcastable` is always true (crossover.hpp:70-74), so the old crossover offered panel donors to these slots. Slots audited 13591 > 1000; non_literal 0; analyze never needed to reject. |
| 5 | constant 0.1 `ts_zscore` NaN in VM and oracle | `AlphaFlatWindow_Zscore.ConstantPointOneWindowIsNaNInVmAndOracle` | Yes, in AuditExact, which is the Engine default and the mode the finding names. Legacy gives 372 finite cells with max \|z\| 0.975, so the test fails on the old code. |
| 6 | AuditExact ts_sum/ts_mean bit-exact vs oracle | `AlphaAuditExactParity_Sum.TsSumTsMeanBitExactVsOracle`, `.OutputIndependentOfPanelStart` | Yes. 0/43200 mismatches, with 1e8 magnitudes and NaN/±inf holes. The legacy online path has 27306 mismatches. |
| 7 | Full VM↔oracle differential green | whole alpha exe 703/703, plus `AlphaAuditExactParity_Differential.W0SurfaceBitExact` | Yes. |
| 8 | Goldens re-baselined, old→new table | `alpha_w0a0_digest_test.cpp`; factory pins | Yes. I read the diff: the `*V1` paths are the pre-W0 code verbatim. OrdinalV1 rank is `(2r)*0.5 == r` exactly, `hump_step_v1` is the renamed old body, the NoneV1 guard is skipped, and OnlineV1 uses `compensated=false` (the old slide). So the kOld digests reproduced under V1 are the base digests. |

### Defect IDs

| ID | Reviewer status |
|---|---|
| A-01 | Closed. `cs_ops.hpp:264-281` (`cs_for_each_rank`) is used by rank, quantile and group_rank (both the reference and the CSR paths). The oracle has an independent `average_rank_order` (`oracle.cpp:17-43`). Equal runs are contiguous under both stable_sort and radix, because -0.0 and +0.0 tie. |
| A-02 | Closed. `state_ops.hpp:110-124`, `vm.hpp:1717-1735`, the oracle restatement at `oracle.cpp:1085-1110`, and the stream at `streaming_engine.hpp:706`. The cap semantics are identical in VM and oracle: the prior is dropped on the 6th consecutive NaN. |
| A-03 | Closed. `typecheck.cpp:327-345` runs in `analyze_call`. Every Program goes through `compile()`, which calls `analyze()` (`bytecode.cpp:158`), and genomes go through `analyze_into`. Crossover filters donors at `crossover.cpp:98-114`. The VM and oracle read sites (`vm.hpp:1329-1333,1714`; `oracle.cpp:315,1084`) are the only scalar-slot reads, and all four ops are covered. |
| A-09 | Closed for the cited batch surface: `ts_ops.hpp` tsv_var / lin_fit / zscore / skew / kurt / corr / regression, restated in the oracle. The OU family and ts_cov are not guarded; see minor finding 3. |
| A-13 | Closed. `vm.hpp:1522-1541` routes AuditExact to `ts_value_at`, and ResearchFast uses the Neumaier `TsvRunSum` (`ts_ops.hpp:497-523`). |
| A-18 | Partial. The verified API exists, but it has the major defect below. The production driver (`search_driver.cpp:165,576,587,908`) still uses the hash-only API, so collision reuse is still live in production. The lane reports this honestly as an integration item; see waiver_needed. |

## Findings

| path:line | severity | problem | required fix |
|---|---|---|---|
| `atx-engine/src/factory/canonical.cpp:273` (with `:264-268`) | major | `insert(h, form)` evaluates `forms[h]` before it decides anything. For a key known only as a bare hash (legacy `insert(h)` or a resume snapshot, which is the case `search_driver.cpp:165` restores), this default-inserts an empty vector and then returns false ("seen"). From then on, `contains(h, form)` finds that empty entry and returns **false**, while `insert(h, form)` keeps returning false. This breaks the documented contract ("a hash known WITHOUT a form cannot be disproven and counts as seen", canonical.hpp:171-174). Once the driver is wired as the lane's own integration note asks, every resumed hash would be treated as unseen by `contains`, re-scored every generation, and never admitted, which distorts all_scored and the trial counts. Test gap: `FactoryCanonCollision_Set.LegacyHashOnlyEntryIsNotDisproven` only checks `contains` before the verified insert. | Use `forms.find(h)` and return false before creating any entry for a hash-only key, or treat an empty form list as hash-only in `contains`. Extend the test to assert `set.contains(42, "anything")` is still true after `set.insert(42, "anything")`, and that `forms` gained no entry. |
| `.superpowers/sdd/w0/lane-a0-report.md:101` | minor | A-18 is marked "CLOSED (lane scope)", but the collision-blind path is still the only one production uses. `search_driver.cpp` uses hash-only `contains`/`insert`, and `fitness_cache` is keyed by hash. | Relabel it as DEFERRED (driver + fitness_cache wiring, factory track), with the integration note as the handoff. |
| `atx-engine/include/atx/engine/alpha/ts_ops.hpp:1288-1293` (`ou_ar1_fit`), `ts_ops.hpp:1223-1230` (ts_cov) | minor | This is the same class as A-09, but outside the cited lines. The OU fit uses the raw-moment denominator `sxx - sx*sx/n`. On a flat forward-filled window that denominator is rounding noise rather than 0, so `b`, and therefore ou_theta / ou_mean / ou_zscore / ou_halflife, can come out finite and meaningless (this was reasoned from the code, not run). ts_cov on a flat window gives noise of about 1e-18 instead of 0. | Not required for this lane. Add it to the Integration notes for the owner of the next ts_ops pass (W1-A1), so the flat guard is extended to the OU fit and cov with an oracle restatement. |
| `atx-engine/include/atx/engine/alpha/cs_ops.hpp:537` | minor | This is pre-existing and was disclosed by the lane. `static_cast<int>(n_real)` is UB for a finite literal outside the int range, such as `quantile(x, 1e20)`. After A-03, finite literals are the only accepted input for this slot, and `typecheck.cpp` is owned by this lane. | Optional in-lane: add a range check (for example 2 ≤ n ≤ 2^31-1, or clamp) to `validate_scalar_literal_operand` for CsQuantile. Otherwise keep the integration note. |
| `atx-engine/tests/factory/factory_nsga_search_test.cpp:91`, `factory_oos_test.cpp:891,1587` | minor | `kGoldenMultiObjectiveOffPath`, R3b and HoldoutEngineReuse are re-baselined with the A-03 share attributed only by elimination. No versioned switch restores the pre-W0 typecheck/crossover rule, so these three old digests can no longer be re-derived. The reviewer re-checked the elimination: the factory path has no oracle or StreamingEngine use, and CanonSet `size()` is unchanged for hash-only inserts. The residual really can only come from A-03 or the four enums. | None blocking. Carry this "not reproducible, A-03 by elimination" caveat into the G0 truth-delta report. |
| `atx-engine/include/atx/engine/alpha/streaming_engine.hpp:116-120,205-212` | minor | The StreamingEngine has no `KernelPolicy`. Stream == batch holds only for the default policy, and `legacy_v1()` cannot be reproduced on the streaming path. | Document the limitation in the StreamingEngine header or the report, or thread `KernelPolicy` through `StreamingEngine::create` in a later lane. |

No blocker was found. On UB and bounds: `cs_for_each_rank` indices stay below n, and `hi+1<n` is guarded. On lifetimes: `form_visit` returns a reference into a fixed-size memo that is never resized during the recursion. Error paths: typecheck returns Err, and crossover returns NotFound when no donors remain. There is no narrowing (`u32` stale counter, `usize` ranks cast to f64 exactly), and the switches in `form_visit` cover every `Expr::Kind`.

## Checked

- [x] `.agents/cpp/agent.md` §10 was applied to the diff: UB, bounds, narrowing, uninitialised state (`HumpState`/`TsvRunSum` have member initialisers), lifetimes, error paths, exhaustive switches, `[[nodiscard]]`/`noexcept`, `// SAFETY:` notes at the VM scalar reads. /W4 /WX is clean: the objects are current at HEAD with /W4 /WX in build.ninja, and the build exits 0. The hygiene preset was not run.
- [x] Every plan acceptance item was re-run through anchored ctest and the whole executables, and the test code was read. Each test is non-vacuous and would fail on the old code (see the table above).
- [x] Every cited defect ID was checked at the code. A-01, A-02, A-03, A-09 and A-13 are closed. A-18 is partial, with the major finding above plus the driver wiring that is out of scope.
- [~] File ownership. Everything is in scope except `alpha/streaming_engine.hpp`, which no W0 lane owns, and the `vm.hpp` policy plumbing beyond the A-02/03/13 sites (`KernelPolicy`, the `CsRowsCtx`/`TsBatchCtx` fields, the `cs_one_date` `ties` parameter, the flat guard at the `eval_ts_column` call sites, the SubtreeCache bypass). Both are disclosed as Deviations and both are forced. The stream==batch contract in the owning alpha target breaks without the streaming edits, and RULES §2 requires Engine-level V1 digest reproduction. Both go to waiver_needed, and W1-A1 must be told about the vm.hpp hunks. New test files follow the RULES naming and namespaces.
- [x] No test was weakened. The pre-existing edits in alpha_cs, alpha_oracle, alpha_eval_perf, cs_radix_rank and alpha_trade_when each re-pin A-01 or A-02 and keep the legacy expectations under the V1 enum where one exists. The factory goldens keep their old values in comments. There are no DISABLED_ or GTEST_SKIP additions and no assertions were deleted.
- [x] Changed numeric behaviour has a versioned enum (`RankTies::OrdinalV1`, `HumpNaN::StickyV1`, `FlatGuard::NoneV1`, `TsSumPath::OnlineV1`, `KernelPolicy::legacy_v1()`), and each is proven bit-exact against the pre-W0 digests. The old→new tables are tied to defect IDs. Exceptions are the oracle (pins the default only), the StreamingEngine, and the A-03 factory goldens; see the minor findings.
- [x] Both owning executables pass whole: alpha 703/703, factory 299/299, `--gtest_brief=1`, exit 0.

## Re-review 1

**Verdict: APPROVE.** The one major finding (CanonSet hash-only key) is fixed, and so are all
five minors (one in code, four in the report). The fix commits add assertions and one new test
and weaken nothing. Both owning executables are green whole.

Reviewed SHA: `9cb64501f5d98087cae18ee2f47c2899f623ae61` (lane head). Fix commits since the
review commit `a2611f70`: `b74e27d3` (code + tests) and `9cb64501` (report). The tree was clean
before this section was written. `9cf203a3` (the originally reviewed SHA) is an ancestor of HEAD,
and the only commit between it and `a2611f70` is the review itself.

### Per finding

| # | Sev | Finding | Status | Evidence |
|---|---|---|---|---|
| 1 | major | `CanonSet::insert(h, form)` default-inserted an empty `forms[h]` for a hash-only key, flipping `contains(h, form)` to false | **FIXED** | `canonical.cpp:271-290`: `insert` now uses `forms.find(h)` and returns false for `hash_known && (it==end \|\| it->second.empty())` before it touches `forms`. `forms[h]` is reached only on the admit path, after `seen`/dup checks. `contains` (`:265`) also treats an empty list as hash-only. The logic was traced: a verified key with a different form still counts as a collision and is admitted, a true duplicate returns false, and legacy `insert(h)` never touches `forms`. The test `FactoryCanonCollision_Set.LegacyHashOnlyEntryIsNotDisproven` now asserts `contains(42, ...)` stays true after `insert(42, "anything")`, `forms.find(42)==end`, `forms.empty()`, repeated inserts are idempotent with `collisions()==0`, a planted empty list is still treated as seen, and a verified key still disproves another form. On the old code, `forms.find(42)==end` and the post-insert `contains` fail, so the test is not vacuous. |
| 2 | minor | A-18 labelled CLOSED although production stays collision-blind | **FIXED** | In `lane-a0-report.md`, the defect table row now reads "DEFERRED (driver + `fitness_cache` wiring, factory track)" and cites `search_driver.cpp:165,576,587,908`, with the Integration note as the handoff. The Outcome paragraph was updated to match. |
| 3 | minor | OU fit / `ts_cov` unguarded on flat windows | **FIXED (as required: note)** | The report's Integration notes now carry a W1-A1 item: extend `tsv_is_flat` to `ou_ar1_fit` and `ts_cov`, add the oracle restatement, and keep `FlatGuard::NoneV1` reproducing the old digests. |
| 4 | minor | `static_cast<int>(n_real)` UB for an out-of-range quantile literal | **FIXED (in lane)** | `typecheck.cpp:337-347` rejects a CsQuantile literal unless `INT_MIN-1 < n < INT_MAX+1`. Both bounds are exactly representable in f64. The kernel (`cs_ops.hpp:537`) and oracle (`oracle.cpp:258`) truncate toward zero, so every accepted value truncates into `int`. `p*nb` with p in [0,1] cannot exceed `nb`, and `nb-1` runs only when nb>=2. The new test `AlphaTypecheckScalarLiteral_Analyze.QuantileBucketCountMustFitInInt` checks ±1e20, 2^31, -2^31-1 and 1e308 → InvalidArgument, and the boundary values (2147483647.9, -2147483648.9, 0, 1) plus scale/winsorize/hump at 1e20 → Ok. The contract comment was updated in `typecheck.hpp`. |
| 5 | minor | Three factory re-baselines not reproducible (A-03 by elimination) | **FIXED (as required: caveat carried)** | A G0 truth-delta caveat in the report Integration notes names `kGoldenMultiObjectiveOffPath`, R3b and HoldoutEngineReuse. |
| 6 | minor | StreamingEngine has no `KernelPolicy` | **FIXED (as required: documented)** | The limitation is in the report Integration notes (stream == batch holds only under the default policy, and `legacy_v1()` cannot be reproduced when streaming). The file stays unedited, which is correct because no W0 lane owns it. |

### No regression / no weakened test

- The diff `a2611f70..HEAD` touches only `canonical.cpp`, `typecheck.{hpp,cpp}`, the two lane test
  files and the report. The test edits only add: 18 assertions in the CanonSet test and 1 new
  typecheck test. Nothing was deleted, and there is no `DISABLED_`/`GTEST_SKIP`.
- The quantile rail rejects only literals that were UB before, so no accepted genome changes
  behaviour. The factory goldens are unchanged (299/299 with the same pins), and the 10k crossover
  stress line is identical (`scalar_rejects=0`).

### Evidence (run by the re-reviewer from `C:\atx-wt\pool-2`)

Free RAM was 3.55 GB before the build, with `CMAKE_BUILD_PARALLEL_LEVEL=2` and the equity-dev
preset.

```
atx-build.ps1 build -Preset equity-dev atx-engine-alpha-tests atx-engine-factory-tests atx-shm-worker
  [9/12] Linking CXX executable bin\atx-shm-worker.exe
  [10/12] Linking CXX executable bin\atx-engine-alpha-tests.exe
  [11/12] Linking CXX executable bin\atx-engine-factory-tests.exe      exit=0
atx-build.ps1 check -Preset equity-dev <canonical.cpp | typecheck.cpp | alpha_w0a0_kernels_test.cpp | factory_w0a0_canon_test.cpp>
  ninja: no work to do.   exit=0 (x4; build.ninja carries /WX for canonical.cpp)

build-equity\bin\atx-engine-alpha-tests.exe --gtest_brief=1
  [==========] 704 tests from 273 test suites ran. (65938 ms total)
  [  PASSED  ] 704 tests.                                         alpha exit=0
build-equity\bin\atx-engine-factory-tests.exe --gtest_brief=1
  [w0a0] crossover stress: children=10000 attempts=12045 scalar_slots=13591 non_literal=0 scalar_rejects=0
  [==========] 299 tests from 57 test suites ran. (85043 ms total)
  [  PASSED  ] 299 tests.                                         factory exit=0

atx-build.ps1 -Ctest -Preset equity-dev -R '^<Suite>'   (each exit=0)
  ^AlphaCsRankTies_              100% tests passed, 0 tests failed out of 7
  ^AlphaHumpWarmup_              100% tests passed, 0 tests failed out of 4
  ^AlphaTypecheckScalarLiteral_  100% tests passed, 0 tests failed out of 6
  ^AlphaFlatWindow_              100% tests passed, 0 tests failed out of 5
  ^AlphaAuditExactParity_        100% tests passed, 0 tests failed out of 5
  ^FactoryCanonCollision_        100% tests passed, 0 tests failed out of 4
```

No new findings were introduced by the fix. The waivers from the first review still stand for the
orchestrator: the `streaming_engine.hpp` and `vm.hpp` policy-plumbing ownership deviations, and
A-18 production wiring deferred to the factory track.
