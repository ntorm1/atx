# Lane COV report -- covariance of record without look-ahead, and the `atx.cov-container` file

## Outcome

**DONE_WITH_CONCERNS.** Every deliverable of brief COV items 1-8 is implemented; the C++ builds clean under the
equity-dev tree (`/W4 /WX`, four target-scoped builds `p9-cov-b..e`, 0 warnings, 0 errors) and every named gtest
passes (engine 18 `RiskCovContainer.*`; impl 26 `Cov*`), as do the unchanged regression filters the brief names.
Concerns (below): the COV-4 trace finds no look-ahead in the risk model's code path, but flags two role-side items
the reviewer must check (`--delisting-returns` roles; the vendor factor's last-bit dependence); the bench is written
but not compiled (bench is OFF in the equity-dev tree).

- Pool `C:/atx-wt/pool-24`, branch `feat/p9-cov-20261003`, base `ad406718` (all eight wave-1 lanes merged, C1 in).
- Commits (oldest first): `6fa702b2` WIP container lib + engine tests + recipe/S2b + partial bias families;
  `16233715` hand-off; `c1d495df` bias families, container rows, scatter, refusal-fixture fix; `98e0f31b` verb flags,
  cov-info / cov-diff, RiskStore container path; `56e61f63` impl tests + CMake tail; `ba2e2de6` bench + README rows;
  then the bench cast tweak and this report (see `git log`).

## Files changed

Owned (brief "Files in scope"):
- new `atx-engine/include/atx/engine/risk/cov_container.hpp`, `atx-engine/src/risk/cov_container.cpp`
- new `atx-engine/tests/risk/risk_cov_container_test.cpp` (auto-globbed into `atx-engine-risk-tests`)
- new `atx-engine/bench/risk_cov_container_bench.cpp` (auto-globbed into `atx-engine-bench`)
- new `atx-impl/tests/strategy_cov_fixture.hpp`, `strategy_cov_model_test.cpp`, `strategy_cov_store_test.cpp`
- `atx-impl/src/strategy_risk_model.{hpp,cpp}`, `atx-impl/src/strategy_risk_verb.cpp`,
  `atx-impl/tools/equity_strategy_risk.cpp`
- RiskStore section of `atx-impl/src/strategy_spo.hpp` (class `RiskStore`: doc lines, `container()`, a private
  `struct Container;` and a `std::shared_ptr<const Container>` member) and `strategy_spo.cpp` (store section:
  `struct RiskStore::Container`, `open` container branch, `read` container branch); plus **two include lines at the
  top of `strategy_spo.cpp`** (`atx/engine/risk/cov_container.hpp`, `strategy_risk_model.hpp`) -- outside the
  section line range but needed by it; no other line of `strategy_spo.*` changed.
- `atx-engine/include/atx/engine/risk/README.md` (two file-map rows).
- `strategy_risk_model_test.cpp`: **not touched** (the fixture is new code, no extraction).

Cross-lane (list tails):
- `atx-engine/CMakeLists.txt`: one line `src/risk/cov_container.cpp` beside `src/risk/specific_risk.cpp`.
- `atx-impl/tests/CMakeLists.txt`: two lines at the tail of the `atx-impl-strategy-target-tests` source list
  (`strategy_cov_model_test.cpp`, `strategy_cov_store_test.cpp`); C2 appends there too -- keep both sides.

Not touched: `strategy_nav_replay.*`, `strategy_nav_v7.*`, `strategy_risk_target.*`, `strategy_vol_target.*`,
`strategy_spo_v3.*`, `atx-engine/include/atx/engine/book/**`, `eigen_adjust.{hpp,cpp}`, `atx-impl/CMakeLists.txt`,
`strategy_spo_fixture.hpp`, any registered constant, any expected hash, any data, `atx-db/`, `C:/atx`.

## Contracts implemented

- **K-P9-12** (`.atxcov`): cov-design section 4 byte layout (header, section table, META / FACTOR_NAMES / DATE_INDEX /
  BLOCK_HASHES, 4096-aligned blocks of 64-aligned arrays, trailer, two-level Merkle root); writer
  (`<path>.partial`, publish-by-rename), reader (`CovModelFile::open` Full / Lazy, every check of 4.8),
  `view` / `exact` / `at_or_before` (never past a cutoff), `factor_risk_view`, `first_difference`.
- **I-1..I-4** (cov-design 3.2): stamps (`as_of = data_cutoff = session`, `history_first` = first session whose return
  entered an estimator), recipe / role / inputs SHA-256 in the header, reader refusals (cutoff, role pin, geometry,
  root, truncation, unknown required section, major > 1).
- Recipe **`atx-cov-v1`** (cov-design 3.1 S2b) and the extended bias families (S7).

## What each brief item became

1. **Container library (engine)** -- as specified; `CovBlockInput` / `encode_cov_block` / `parse_cov_block` are public
   so the impl tests' in-memory sink produces exactly the bytes the writer stores. No JSON dependency in the engine:
   META, recipe and inputs are opaque canonical texts from the caller (recipe_sha256 / inputs_sha256 = SHA-256 of them).
2. **Recipe `atx-cov-v1`** -- `RiskModelConfig::recipe` (default `RiskV11`); S2b in `fully_observed_forecast` after the
   eigenvalue floor and before the prior / covariance writes, so the structural border (and its PD scaling), the class
   priors and the VRA all read the adjusted pre-VRA block. `eigen_adjust_block` (public, testable) calls
   `engine::risk::eigen_adjust_v2(F_keep, n_eff_t, 64, 1.0, seed_t, max_bytes)`; n_eff_t = Kish count of
   w_s = 2^(-(t-s)/504) over sessions s <= t with a finite market factor return (tracked recursively in
   `Model::update_vra`); seed_t = splitmix64(7 XOR session ns). A session that cannot be adjusted keeps the block and
   is counted (`EigenOutcome` ShortHistory = n_eff < K+1; Refused = any kernel error, e.g. a non-positive eigenvalue).
   `RiskDay` gains `eigen`, `eigen_effective`, `eigen_gamma`, `vra_factor_applied`, `returns_entered`,
   `specific_flags`. Manifest: `recipe.model` = `atx-cov-v1` and `recipe.eigen_adjustment` with the brief's verbatim
   values; top-level `model` = `atx-cov-v1`; a top-level `eigen_adjustment` block (CovV1 only) with the per-session
   outcome counts, the kernel workspace bound and its source.
3. **Truncation and eigen tests** -- `strategy_cov_fixture.hpp` (own panel builder, see Decisions) + the CovTruncation /
   CovEigen suites.
4. **Writer wiring (verb)** -- `--recipe atx-risk-v1.1|atx-cov-v1`, `--emit-container` (valueless, parsed before the
   generic value path), `--bias-families v1|extended`; `ContainerSink` (one block per session: rows by the lossless
   row rule, priced first, f32 exposures with the same bits as `style_exposures.f32`, FACTOR_RETURN, FACTOR_FLAGS,
   ROW_FLAGS, DIAG (32, names in META), EIGEN_GAMMA); the writer is created before `run_risk_model` and finalised
   after it; `manifest.json` gains `container {file, format, bytes, file_sha256, root_sha256}` and
   `files["model.atxcov"]` only with the flag, still written last. `cov-info FILE [--verify full|lazy]` and
   `cov-diff A B [--through SESSION]` / `cov-diff --legacy DIR` in `atx-equity-strategy-risk`.
5. **Store read path** -- `RiskStore::open`: no `container` key -> today's code, unchanged. With it: Full verification
   at the manifest's root, role pin == `--risk-model` role pin, geometry 62 / 50 / 11, date count, every session and
   forecast flag equal to `diagnostics.csv`; the four legacy payloads are neither required nor hashed (diagnostics.csv
   still is). `read(d)` = `view(d)` (as_of checked == session d) -> `scatter_cov_block` -> today's rules (NaN covariance
   -> 0 counted, f32 -> f64, non-finite style -> 0). Copyable by `shared_ptr` as before.
6. **Extended bias families** -- `eigen` (eigenportfolios of F_{t-1} incl. VRA, z = u_k'f_t / sqrt(lambda_k), missing
   factor return counts 0 and is reported), `minvar` and 32 `optimized` books (Woodbury over the eligible names with a
   complete forecast; optimized = V^-1 alpha made dollar-neutral, gross 1; alpha = `random_score(seed XOR
   "optimize", q, id)`), QLIKE (pooled over a family's ok series) and MRAD (per series over its rolling b, then the
   family mean; windows 63 and 252) under `bias_summary.json` families and the manifest's `bias_harness` block
   (`family_set: extended`, `optimized_portfolios`) only with `--bias-families extended`.
7. **Bench** -- `risk_cov_container_bench.cpp`: `BM_CovContainerWrite`, `OpenLazy`, `OpenFull` (default pool and 1
   worker), `FirstTouch`, `View`, and the labelled emulations `LegacyStoreOpen` / `LegacyStoreRead`, on K 62 / S 11 /
   D 1,000, N 3,000 and 5,627, f32 and f64; labels name the SHA-256 backend. (Names carry `CovContainer` so root's
   `--benchmark_filter=CovContainer` selects them; cov-design 4.12's `BM_CovOpenLazy` etc. map one to one.)
   **Not compiled**: `ATX_BUILD_BENCH` is OFF in the equity-dev tree and a lane does not reconfigure.
8. **Docs + report** -- README rows; this report.

Rulings: COV-1 (order: recipe + kernel with gtests, then verb + link) followed; COV-2 `--max-bytes` reaches the kernel
under `atx-cov-v1` when given (`eigen_max_bytes`), kernel default 268,435,456 when absent (recorded in the manifest's
`eigen_adjustment.max_working_bytes[_source]`; never read under atx-risk-v1.1, so a V11 run with `--max-bytes` is
byte-identical); COV-3 MRAD per asset series as above; COV-4 trace below.

## COV-4 / G-P11: as-of trace (mandatory; reviewer must re-check)

Question: does block d, read by a decision at session d, hold anything from after the point that decision is formed?

1. **Consumer timing.** NAV replay (`strategy_nav_replay.hpp:21-23`): decide at session d after its mark, fill at d+1's
   close, first return row d+2; session loop `strategy_nav_replay.cpp:1370` (`s.decision = t + 2 < end`), phase 2
   `plan_decision(..., s.t, ...)` (`:1286`) -> `p.leverage->plan(...)` (`:966`) -> `BookLeverage::plan` ->
   `scaler_.leverage(x, d, ...)` (`strategy_risk_target.cpp:192`) -> `estimate(d, ...)` (`:114`) ->
   `risk_->read(d, slice_)` (`:141`). The container path reads exactly block d (`view(d)`, as_of checked equal to
   session d; it never uses `at_or_before`). Role clock (`prepare_recent_research.py:82`,
   `modeled-session+22h-mark+23h-decision-v1`): row d's close is the 22:00 UTC mark of d, the decision is 23:00 UTC.
2. **Producer.** Block t is emitted at the end of session t's pass of `run_risk_model`: `update_prices(t)` (returns
   over (t-1, t], rolling beta sums of returns <= t, ADV over rows t-62..t), `regress(t)` (returns at t on X_{t-1},
   caps t-1), `update_vra(t)` (prior forecasts of t-1 vs f_t; Kish moments through t), `exposures(t)` (rows <= t:
   momentum rows t-252 / t-21, descriptors and caps of row t), `factor_forecast` (EWMA states through t; S2b on F_t
   with n_eff_t and session t's own seed -- no draw depends on another session), `specific_forecast` (residuals <= t,
   caps of row t). The `Model` vectors sized by `dates` are indexed <= t only. **Mechanical proof:** `CovTruncation.*`
   (both recipes): truncating the panel after c, replacing every value after c (prices, volume, membership, caps,
   descriptors, industries), or adding instruments that first appear after c leaves every block <= c byte-identical
   (by id after projection for added instruments), with teeth (a later block differs; the role indexes shift) and a
   planted leak (one style shifted by its full-sample mean) caught at the first exposed session.
3. **Inputs at row d** (code reading only, no data opened):
   - Fields: issuer items and industry groups read the latest row with clock < the mark of session d-1
     (`prepare_research_fields.py:28-30`, `--fund-lag-sessions 1`); FINRA SI strict `available_at < date(session)`
     (`:9-11`); `shares_out` the last row dated <= date(session) - 90 days restated by a cumulReturnFactor ratio of rows
     <= d (`:14-18`); `me_company` = shares_out x raw_close (row d). All visible by d's 22:00 mark.
   - Role close: `close = f64(raw f32 close) x f64 cumulReturnFactor` per row (`prepare_recent_research.py:305`,
     `close_basis` `:555`). A vendor cumulative factor is anchored at a date after d (back-adjusted; the vendor
     re-anchors at 2021-01-04, `prepare_research_fields.py:16-17`), so the **level** close[d] does embed corporate
     actions after d. The risk model never reads a level: every use of `close` is a ratio of two rows <= t of the same
     line (`interval_return` close[t]/close[t-1]; momentum close[t-21]/close[t-252]), in which the common anchor
     cancels and only events with ex-dates in (s, t] remain; caps use `me_company` / raw close, ADV raw close x volume.
     A factor jump without a raw move (a re-anchoring break) fails the guard (`|log adj| > |log raw| + .10`) and the
     return is missing, not leaked. **Caveat A (last bits):** the cancellation is exact in real arithmetic only; in f64
     the product raw x f rounds differently under another anchor, so a role rebuilt from a later vendor snapshot can
     change past returns in their last bits. The truncation property on real data therefore holds byte-for-byte only
     for a prefix of the same role payload (root's optional `cov-diff --through S` must truncate the role's arrays, not
     rebuild it). **Caveat B (vintage):** `historical_vintage_verified: False` in the role cache manifest -- restated
     factors in the snapshot cannot be excluded from code.
   - **Finding (reviewer must check the registered role):** with the role builder's `--delisting-returns` option
     (default off), the termination session T = L+1 of a delisted line gets close[T] = close[L] x (1 + dlret) with an
     imputed dlret whose cause "may be classified after T" (up to 30 days; `prepare_recent_research.py:155-163`, whose
     own text says it "must not feed a T-dated signal"). The line is a member at L with cap, so its return over
     (L, T] enters the factor WLS at T and F_T: block T would then embed information available up to T+30 days. COV
     changes nothing here (atx-risk-v1.1 behaves identically). If the R-8 / Y-1 role was built with
     `--delisting-returns`, root should record it under G-P11 and decide (e.g. a risk-verb rule that drops a
     member-at-L line whose row T is a patched termination row (present, volume 0, member 0) from the regression).
4. **Verdict:** under the replay's timing a decision at d reads block d, built from closes through d's 22:00 mark and
   fields visible by d-1's mark, first traded at d+1's close; no statistic over the role's whole axis, no future
   instrument and no future draw enters it (tests). The d-2 lag is not needed for the covariance. Open: the
   `--delisting-returns` question and the two caveats above, which are role-builder properties outside COV's code.

## Evidence

Builds (research-build, equity-dev tree of pool-24, memory-gated by `scratchpad/cov-gated-build.ps1`, which also waits
until no other compiler / linker / ninja runs):

| tag | targets | exit | wall s | TUs |
|---|---|---|---|---|
| p9-cov-a | atx-engine-risk-tests (previous session) | 1 (unused helper, fixed in `16233715`) | 460 | 222 |
| p9-cov-b | atx-engine-risk-tests | 0 | 312 | 52 |
| p9-cov-c | atx-equity-strategy-risk | 0 | 334 | 85 |
| p9-cov-d | atx-impl-strategy-target-tests, atx-engine-risk-tests, atx-equity-strategy-risk | 0 | 197 | 37 |
| p9-cov-e | atx-impl-strategy-target-tests | 0 | 56 | 8 |

0 compiler warnings in every build (only clang-cl's `/MP unused` driver note). Receipts:
`pool-24/build-equity/mega-p9-cov-{b,c,d,e}-receipt.json`; logs copied to `cov-evidence/` beside this report.

Tests (Debug, equity-dev):
- `atx-engine-risk-tests --gtest_filter=RiskCovContainer.*` -> **18 passed** (1.0 s).
- `atx-engine-risk-tests --gtest_filter=Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` -> **428 passed,
  1 skipped** (`RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`, a nightly gate), 0 failed (170 s).
- `atx-impl-strategy-target-tests --gtest_filter=CovTruncation.*:CovEigen.*:CovRecipe.*:CovStore.*:CovBias.*:CovDiff.*`
  -> **26 passed**, 0 failed (232 s; the atx-cov-v1 verb runs dominate).
- `atx-impl-strategy-target-tests --gtest_filter=RiskWls.*:RiskEwma.*:RiskBias.*:RiskModel.*:RiskVerb.*:RiskRobust.*:RiskTarget.*:VolTarget.*:NavBookRule.*:Spo*:NavV7Hook.*`
  -> **104 passed**, 0 failed (59 s; unchanged suites, run after every COV change was in).
- Python sanity (T1 guards): `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
  scripts/tests/test_no_python_mirror.py scripts/tests/test_no_versioned_scripts.py` -> exit 0, `11 passed in 3.88s`.
- Bench: not built (see item 7).

Tests written (names): engine `RiskCovContainer.{RoundTripIsBitExact, SameInputsSameBytes, HeaderLayoutIsPinned,
ArraysAre64ByteAlignedAndBlocksPageAligned, RefusesTruncatedFile, RefusesBadMagicOrMajorAndAcceptsHigherMinor,
RefusesIncompleteFlag, FullVerifyRefusesAFlippedBlockByte, LazyVerifyRefusesAFlippedBlockByteOnFirstTouch,
RefusesRootPinMismatch, RefusesOverlappingOrMisalignedSections, RefusesNonAscendingSessionsAndCutoffAfterAsOf,
IgnoresUnknownOptionalSectionRefusesUnknownRequired, AtOrBeforeNeverReturnsAFutureCutoff, PrefixRunsShareBlockHashes,
FactorRiskViewIsZeroCopyOnPricedRows, WriterRefusesOutOfOrderAppendAndBadRowOrder, FullVerifyIsWorkerCountInvariant}`;
impl `CovTruncation.{BlocksBeforeTheCutAreByteIdentical, PerturbedFutureLeavesThePastUnchanged,
FutureInstrumentsDoNotMoveThePast, DetectsAPlantedLeak}` (each loops over both recipes),
`CovEigen.{RecipeV11IsBitIdenticalToToday, ZeroSimulationsEqualsV11, SmallEigenfactorsInflatedLargeNearOne,
SeededPerSessionAndWorkerInvariant, StructuralBorderAndPdScalingApplied, VraReadsTheAdjustedPrior,
ResultIsPositiveDefinite, LowersTrueRiskOfTheEstimatedMinVariancePortfolio}`,
`CovRecipe.{LegacyRecipeContainerEqualsDirectoryStore, FlagsAbsentOutputsUnchanged, ManifestNamesTheContainerRoot,
CovV1RecipeRecorded}`, `CovStore.{RiskSliceFromContainerEqualsDirectory, VolAndRiskTargetRecordsUnchanged,
RefusesRoleGeometryOrRootMismatch, LegacyManifestTakesTheLegacyPath}`, `CovBias.{EigenFamilyInBandOnAPlantedModel,
MinVarAndOptimizedUseCompleteForecastsOnly, MissingReturnCountsZeroNeverRenormalised, V1FamiliesBytesUnchanged}`,
`CovDiff.{ThroughSessionReportsTheFirstDifferingBlock, LegacyModeComparesBitForBit}`.

## How root verifies (brief "Root verifies", verbatim, plus notes)

**Root verifies (after merge, build tag `p9-2<letter>`):**
1. Builds `atx-engine`, `atx-engine-risk-tests`, `atx-impl-strategy-target-tests`, `atx-equity-strategy-risk`,
   `atx-equity-strategy-targets` with 0 warnings; filters: `atx-engine-risk-tests --gtest_filter='RiskCovContainer.*'`
   and the existing `Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery`; `atx-impl-strategy-target-tests
   --gtest_filter='CovTruncation.*:CovEigen.*:CovRecipe.*:CovStore.*:CovBias.*:CovDiff.*'` and, unchanged,
   `'RiskWls.*:RiskEwma.*:RiskBias.*:RiskModel.*:RiskVerb.*:RiskRobust.*:RiskTarget.*:VolTarget.*:NavBookRule.*:Spo*:NavV7Hook.*'`.
2. **Flag-absent identity, real data, 0 trials** (<= 600 s, <= 8,192 MiB, through `run_bounded_research.py`): re-run
   the R-8 store build (role and fields pins and argv from `build-equity/v8-risk-lo3-v10`'s manifest / receipt) with
   no new flag into a new dir: every `files` entry SHA-256 equal to `v8-risk-lo3-v10`'s; `manifest.json` equal except
   the substitution list `producer.executable_sha256`, `producer.engine_git_sha` (written before the run).
3. Same argv plus `--emit-container` into a new dir: every legacy file byte-equal to step 2; manifest = step 2's plus
   `container` and its `files` entry; `atx-equity-strategy-risk cov-diff --legacy <dir>` exits 0.
4. Y-1's registered NAV (`y-vol-target-y-1.json`, as re-based at P9-B0) with `--risk-model <step-3 dir>
   --risk-model-sha256 <its manifest SHA>`: `vol_target.csv` and every NAV payload byte-identical to the P9-B0 Y-1
   reference except the substitution list written before the run (the recorded risk-model directory and manifest
   pin, and argv-derived receipt digests). 0 trials (identity re-run, SHA comparison only, no statistic read).
5. Once, `--recipe atx-cov-v1 --emit-container --bias-families extended` on the same role: exit 0, wall and peak
   memory logged (if wall > 600 s on the tree's build: stop and take the Release ruling of cov-design §6 Q4); no
   statistic is read (the bias tables are D-COV's, after wave 2).
6. `rel` preset with `ATX_BUILD_BENCH=ON`: `atx-engine-bench --benchmark_filter=CovContainer
   --benchmark_repetitions=5`; medians against cov-design §4.12 logged in the integration log.
7. T1's tiny-world canary goldens unchanged.

Notes for root:
- Step 3's `cov-diff --legacy` compares all four legacy files only when the run wrote `style_exposures.f32` /
  `industry_slot.u8` (`--emit-exposures all`, which the R-8 store has since RiskStore requires it); it prints the files
  compared.
- Step 4 reads through the container path (`RiskStore::open` takes it whenever the manifest has `container`), so it
  is the real-data proof of `CovStore.VolAndRiskTargetRecordsUnchanged`.
- Step 5 timing: the Debug tree compiles `eigen_adjust.cpp` at /Od (engine) while the impl estimator is /O2. On the
  synthetic 300-session x 200-name role, `--recipe atx-cov-v1` took ~29 s vs ~1 s for atx-risk-v1.1 (S2b runs 64
  simulations of length n_eff per session); TRAIN (~1,000 sessions, K <= 62, n_eff up to ~700) will be much slower
  per session. If step 5 exceeds 600 s, that is the Release ruling of cov-design 6 Q4.
- Step 6: the bench TU has never been compiled (see item 7); a compile error there is mine to fix.
- New schema literals for `classes.json` (ruling SQL2-CLS): none -- the container's format name is
  `atx.cov-container/1.0` (a file format, not an `atx.<name>/v<n>` schema); the risk manifest stays
  `atx.risk-model/v1` and `bias_summary.json` stays `atx.risk-bias/v2` (extended families only add keys).

Identity checks root must run: step 2 (flag absent, real data) and step 3 (container present, legacy bytes equal),
plus the in-tree proofs `CovRecipe.FlagsAbsentOutputsUnchanged` (every file of a flag-absent run byte-equal to a run
spelling `--recipe atx-risk-v1.1 --bias-families v1`, manifest included, and to a `--emit-container` run except the
manifest's `container` / `files["model.atxcov"]`), `CovEigen.RecipeV11IsBitIdenticalToToday` and
`CovBias.V1FamiliesBytesUnchanged`.

## Decisions

- **Fixture not extracted** from `strategy_risk_model_test.cpp`: the COV panel needs IPOs, delistings, churn, missing
  descriptors and a thin industry the existing `Planted` panel lacks, so the fixture is new code in its own namespace
  and no line of the existing test changed (brief: "extract only if you reuse it").
- **Small windows in the model suites** (`small_config`: vol HL 21, NW HL 42, VRA HL 10 / 5 dates, min factor history
  20, structural history 30, beta / momentum windows 40, min regression names 60; correlation HL 504 kept): the
  estimator code is the recipe's; only the windows differ, so every stage including S2b runs within 150 sessions in
  Debug. The verb-level suites use the default config on a 300-session role.
- `eigen_adjust_v2` does not return gammas (and is forbidden to edit): gammas are diagnostics computed as
  sqrt(u_k'F_adj u_k / d_k) on the input's eigenbasis; `CovEigen.SmallEigenfactorsInflatedLargeNearOne` checks that
  F_adj u_k = gamma_k^2 d_k u_k (an eigen-rotation adjustment).
- `RiskStore::Container` is a private nested struct holding the `CovModelFile`, so the header needs no forward
  declaration outside the RiskStore section.
- The container's META and inputs record carry no paths (fields / book pins, fields used, descriptors, cap rule,
  producer, seal, DIAG names, row rule, stamp rule), so the root does not depend on where inputs sit; the producer
  block makes the root build-specific, exactly as the manifest's `producer` is.
- `--emit-container` under a role reaching the seal with `--unseal` sets the header's `unsealed` flag (and META's seal
  record); refusal without `--unseal` is unchanged.
- `CovBias.EigenFamilyInBandOnAPlantedModel`: the planted industries obey the cap constraint, so the factor block has
  near-singular directions and USE4's band cannot hold; the test asserts z = realised / vol bit for bit, a pooled b in
  (0.5, 3) under atx-cov-v1 and that S2b lowers the family's b relative to atx-risk-v1.1 (whose b > 1). First version
  (b < 2.0) failed at 2.056 and was restated this way -- recorded here, not hidden.
- `CovBias.MinVarAndOptimizedUseCompleteForecastsOnly` runs with structural factor forecasts off, so names loading on a
  short-history style are genuinely incomplete and the test has teeth (with them on, every exposed name of the planted
  panel has a complete forecast and the "left out" count was 0).
- Engine fixture fix: `risk_cov_container_test.cpp`'s `rehash_root` took a `subspan` of a section moved past the end of
  the file and aborted (MSVC debug assertion) before the reader was asked; it now leaves such a section out of the
  recomputed root (the reader still refuses the file: test passes).

## Concerns

1. G-P11: the `--delisting-returns` finding and caveats A / B in the trace (role-builder properties; not COV code).
2. Bench uncompiled (item 7).
3. Debug cost of S2b on TRAIN (step 5 note).
4. `CovStore.VolAndRiskTargetRecordsUnchanged` runs C1's spo fixture (70 sessions x 12 names) under both laws on a
   legacy store and the same store with a container; the real-data equivalent is root's step 4.

## Carried minors

- C1 minor "config check skips a null risk store" stays C2's (carry). COV guarantees `RiskStore::open` refuses a
  container whose root is not the manifest's (`CovStore.RefusesRoleGeometryOrRootMismatch`).

## Ledger candidates

- `.atxcov` S2b cost: atx-cov-v1 on a 300 x 200 synthetic role in the Debug tree ~29 s vs ~1 s for atx-risk-v1.1
  (eigen_adjust.cpp /Od).
- The role's adjusted close is a back-adjusted level (vendor anchor after d); risk-model reads are same-line ratios of
  rows <= t, exact up to f64 rounding -- real-data truncation proofs must truncate the role payload, not rebuild it.

## Fix round 1 (review `task-COV-review.md` at `535fb057`: CHANGES_REQUIRED, R1 + R2; ruling COV-R1)

Status: **R1 and R2 fixed; band met on a well-posed planted panel (no kernel defect).** No test renamed, no band
loosened, no expected hash edited.

### R1 -- the band is asserted (ruling COV-R1)

New planted panel `BandPanel` (`atx-impl/tests/strategy_cov_fixture.hpp`) with `band_config`:
- Well-conditioned factor block: every name in the residual industry slot (no cap-constraint null direction among
  several industries), equal caps (size degenerate, so off), six descriptor styles with fixed N(0, 1) exposures;
  factor returns i.i.d. N(0, Sigma), Sigma = Q diag(lambda) Q', lambda geometric 2e-6 .. 2e-4 (two decades), Q a seeded
  rotation; specific sd 1%; every member present every session. 500 sessions x 300 names, seed 29.
- Variance half-life consistent with the simulated effective count: `vol_halflife = correlation_halflife = 42`,
  variance / correlation Newey-West lags 0, so S2b's simulated length (the Kish count of those weights) is the
  effective sample of every entry of F. VRA off (`vra_min_dates` 1,000,000: S2c is not under test); price styles out of
  reach (windows 4096). The recipe code is unchanged; only the fixture's windows differ.

`CovBias.EigenFamilyInBandOnAPlantedModel` now asserts on that panel, with the design's band (cov-design 2.8 / 5.3,
b in 1 +- sqrt(2/T)): under atx-cov-v1 ranks at the eigenvalue floor (< 1e-6 x max) excluded and counted (<= 1),
>= 6 ranks kept, pooled N > 2000, **|pooled b - 1| <= sqrt(2/N)**, **share of ranks in their band >= 0.95**; teeth:
atx-risk-v1.1 on the same panel has pooled b > 1 + sqrt(2/N), and cov's in-band share >= v1.1's. The churn
(cap-constrained, short-HL) panel keeps the relative checks (0.5 < b < 3, b_v11 > 1, b < b_v11, QLIKE / MRAD) and
prints per-rank b as a diagnostic.

Planted-panel result (`cov-evidence/cov-gt-cov-all-f1.log`, T = 469 per rank):

| panel / recipe | eigen-00 .. eigen-06 | pooled b +- band (N) | ranks in band | at floor |
|---|---|---|---|---|
| band / atx-cov-v1 | 1.014 0.994 1.011 1.009 0.990 1.000 1.005 | **1.003 +- 0.025** (3283) | **7/7** | 0 |
| band / atx-risk-v1.1 | 1.111 1.060 1.060 1.049 1.013 1.015 0.993 | 1.043 +- 0.025 (3283), outside | 6/7 | 0 |

Churn-panel diagnostic (T = 119; eigen-12 T = 109):

| recipe | eigen-00 .. eigen-12 | pooled b +- band (N) | in band |
|---|---|---|---|
| atx-cov-v1 | 6.074 2.152 1.497 1.255 1.124 1.287 0.969 1.178 1.101 0.925 0.925 0.861 0.769 | 2.056 +- 0.036 (1537) | 5/13 |
| atx-risk-v1.1 | 7.123 2.431 1.689 1.323 1.112 1.226 1.017 1.253 1.033 0.949 0.815 0.821 0.673 | 2.327 +- 0.036 (1537) | 4/13 |

Reading: on the churn panel the excess sits in the smallest ranks (eigen-00 at 6-7, the cap-constraint direction that
moves at every membership change) plus the short variance HL (21) against the Kish length of HL 504 (review S8);
S2b lowers every small rank. On the well-posed panel the same kernel puts every rank in band. History kept: the first
version's `b < 2.0` failed at 2.056 (above, Decisions), then the relative restatement, now replaced by this band.

### R2 -- overflow-safe bounds (`atx-engine/src/risk/cov_container.cpp`)

- `parse_cov_block` array table: `size == 0 || offset_in_block > bytes.size() || count > (bytes.size() -
  offset_in_block) / size` (no underflow; a refused array never forms a span). `previous_end = offset + count * size`
  is computed only after that check, so it is <= the block size and cannot wrap.
- Section extents: `offset > data_end || bytes > data_end - offset`.
- Section table: `section_table_offset > data_end || section_count * 64 > data_end - section_table_offset`.
- Section sizes: `elem == 0 || bytes % elem != 0 || count != bytes / elem` (no `count * elem` product).
- Block walk: `block_bytes > blocks_end - next_block` before `next_block += block_bytes`; the final check is
  `next_block == blocks_end` (`blocks_end` computed once from the already-bounded section).

New tests (engine): `RiskCovContainer.ParseBlockRefusesArrayBoundsThatWrapOrLeaveTheBlock` (direct `parse_cov_block`:
offset past the block, offset `SIZE_MAX - 63`, count `SIZE_MAX / 8` and `SIZE_MAX`, plus a tight buffer accepted and
refused one byte short) and `RiskCovContainer.RefusesSectionTableAndBlockSizesThatWrap` (section-table offset near
2^64; an index `block_bytes` that wraps, at t = 0 and t = 2, under Full and Lazy with the root re-hashed so only the
bounds check can refuse; META bytes near 2^64). All refuse with a ParseError, none reaches UB.

### Suggested findings

Done:
- **S2** input-side leak: `CovPanel::with_future_mean_return_value()` replaces each name's value descriptor by its
  own full-axis mean return; it flows through `run_risk_model`, survives standardisation, and
  `CovTruncation.DetectsAPlantedLeak` now requires the comparator to find it (both recipes), beside the sink-side leak.
- **S3** budget refusal counted apart: `EigenOutcome::BudgetRefused` (manifest / summary name `refused_budget`),
  checked before the kernel from the same workspace rule (K^2 128 + T K 64 <= budget, overflow-safe); the risk verb
  refuses a run with any such session ("risk: the atx-cov-v1 eigen adjustment's workspace exceeds the budget on N
  sessions; raise --max-bytes"). Test: 51,199 bytes refused / 51,200 adjusted at the boundary.
- **S4** seal on the read path: `RiskStore::open` refuses a container with `last_session_ns >= kSealBeginNs` and no
  unseal record (`CovStore.RefusesAContainerReachingTheSealWithoutUnseal`). Cross-section edit: one more include in
  `strategy_spo.cpp` (`atx/engine/data/research_window.hpp`, outside the RiskStore section; the check itself is
  inside it).
- **S6** reflow: the new engine files (`cov_container.{hpp,cpp}`, `risk_cov_container_test.cpp`, the bench) are
  clang-formatted with the repo `.clang-format` and have 0 lines over 100 columns; the new impl tests 0; the owning
  impl sources are back to their base counts (`strategy_risk_model.cpp` 13, `.hpp` 1, `strategy_risk_verb.cpp` 16 --
  all base lines).
- **S7** books: `CovBias.MinVarAndOptimizedUseCompleteForecastsOnly` asserts held == complete set (0 mismatches);
  new `CovBias.MinVarAndOptimizedEqualTheDenseInverseOnSmallN` (6 names, Woodbury minvar / optimized weights against
  a dense V^-1 to 1e-10); `BiasHarness::extended_unformed()` counts sessions whose extended books could not be formed
  (non-finite block entry, failed LLT, non-positive 1'V^-1 1) and `bias_summary.json` records it as
  `extended_families.unformed_sessions`.

Left, with reason:
- **S1** (rename `RecipeV11IsBitIdenticalToToday` / `VolAndRiskTargetRecordsUnchanged`): the names are the brief's
  acceptance contract and ruling COV-R1 says no rename; the base-anchored proof is root step 2 (real data, flag
  absent) -- unchanged.
- **S5** (Debug S2b cost on TRAIN): root note, not lane code; plan the cov-design 6 Q4 Release ruling for step 5 and
  check Release-vs-Debug byte identity of `eigen_adjust_v2` on the synthetic fixture first.
- **S8** (simulated length = Kish of HL 504 vs variance HL 84): by design in the brief; changing it is a new recipe id
  (COV-5). The churn-panel table above is the evidence for D-COV's reading and the P10 recipe candidate.
- **S9** (`universe.delisting.returns_applied`): copying it into the manifest changes V11 manifest bytes (breaks root
  step 2's identity) and refusing is a new policy on the registered verb; R-8 / Y-1 roles have it false. Left to
  root / PM.

Manifest / summary additions, under atx-cov-v1 / extended only (flag-absent bytes unchanged,
`CovRecipe.FlagsAbsentOutputsUnchanged` and `CovBias.V1FamiliesBytesUnchanged` pass):
`eigen_adjustment.sessions.refused_budget`, `extended_families.unformed_sessions`.

### Bench

`ATX_BUILD_BENCH` is OFF in equity-dev, so the bench target does not exist in the lane tree. Done instead: a
syntax-only compile (`clang-cl /Zs`) of `atx-engine/bench/risk_cov_container_bench.cpp` with the exact flags of its
engine neighbour `cov_container.cpp` from `compile_commands.json` (`/W4 /WX /permissive- -std:c++20`) plus
google-benchmark's headers -> exit 0 (the checker was validated on a deliberately broken probe, exit 1). Not linked or
run: root step 6 (`rel`, `ATX_BUILD_BENCH=ON`) still owns that.

### Builds and tests

| tag | targets | exit | wall s | TUs |
|---|---|---|---|---|
| p9-cov-f | atx-engine-risk-tests, atx-impl-strategy-target-tests, atx-equity-strategy-risk | 1 ("could not load cache": the object tree had been deleted under DISK-2; tag spent) | 5 | 0 |
| p9-cov-g | same, after `atx-build.ps1 configure -Preset equity-dev` | 0 | 721 | 369 (fresh tree) |

Only warnings in p9-cov-g: clang-cl's `/MP unused` driver note on dependency TUs and two pre-existing third-party
`getenv` deprecations in `databento-cpp` (fresh tree recompiles them); none in lane code. Receipts and UTF-8 logs:
`cov-evidence/mega-p9-cov-{f,g}-{receipt.json,build.log}`, `cov-f.log`, `cov-g.log`, `cov-configure-f1.log`.

Tests (Debug, equity-dev, p9-cov-g executables):
- `atx-engine-risk-tests --gtest_filter=RiskCovContainer.*` -> **20 passed**.
- `atx-engine-risk-tests --gtest_filter=Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` -> **430 passed, 1
  skipped** (`RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`), 0 failed (`cov-gt-engine-risk-f1.log`).
- `atx-impl-strategy-target-tests --gtest_filter=CovTruncation.*:CovEigen.*:CovRecipe.*:CovStore.*:CovBias.*:CovDiff.*`
  -> **28 passed**, 0 failed (180 s; `cov-gt-cov-all-f1.log`).
- Unchanged filter `RiskWls.*:RiskEwma.*:RiskBias.*:RiskModel.*:RiskVerb.*:RiskRobust.*:RiskTarget.*:VolTarget.*:NavBookRule.*:Spo*:NavV7Hook.*`
  -> **104 passed**, 0 failed (`cov-gt-regress-f1.log`).

New test names this round: `RiskCovContainer.ParseBlockRefusesArrayBoundsThatWrapOrLeaveTheBlock`,
`RiskCovContainer.RefusesSectionTableAndBlockSizesThatWrap`, `CovBias.MinVarAndOptimizedEqualTheDenseInverseOnSmallN`,
`CovStore.RefusesAContainerReachingTheSealWithoutUnseal`.

Root's verification list above is unchanged; root step 1's counts become 20 (`RiskCovContainer.*`) and 28 (`Cov*`).

## Fix round 2 (re-review `task-COV-rereview-1.md` at `2639de2e`: CHANGES_REQUIRED, R1b + R2b; rulings COV-R1, COV-9)

Status: **R1b and R2b fixed. On the well-posed planted panel at half-lives 21, atx-cov-v1 is in the band; the a = 0.5
negative control and atx-risk-v1.1 are out of it. No kernel defect.** No test was renamed, no band was loosened and no
expected hash was edited. Commits: `fb6201b8` (R2b), `e9fc3668` (R1b and the S7 nit), then this report.

### R1b: band panel at half-lives 21, with a negative control

- `band_config` (`atx-impl/tests/strategy_cov_fixture.hpp`): `vol_halflife = correlation_halflife = 21` (was 42).
  NW lags stay 0, the VRA stays off and the panel is unchanged (`BandPanel` 500 x 300, seed 29, K = 7). Kish(21) =
  60.6 >= K + 1, so the panel is still well-posed. The band is unchanged: per rank 1 +- sqrt(2/T) with a share >= 0.95;
  pooled 1 +- sqrt(2/N).
- Negative control (`CovBias.EigenFamilyInBandOnAPlantedModel`): same panel and config with `eigen_amplification =
  0.5`, i.e. gamma = 1 + 0.5 (v - 1), about half the variance correction. It must miss the band (pooled out, **or**
  < 95% of ranks in) on the same N.

Per-rank b, C++ (`p9-cov-h`, `cov-evidence/cov-gt-cov-all-f2.log`). T = 469 per rank, N = 3,283; the rank band is
+-0.065 and the pooled band +-0.025:

| band panel, HL 21 | eigen-00 | -01 | -02 | -03 | -04 | -05 | -06 | pooled b | ranks in band | band |
|---|---|---|---|---|---|---|---|---|---|---|
| atx-cov-v1 (a = 1) | 1.013 | 0.998 | 1.013 | 1.020 | 0.993 | 1.021 | 1.009 | **1.009** | **7/7** | **pass** |
| a = 0.5 (negative control) | 1.085 | 1.051 | 1.058 | 1.054 | 1.015 | 1.024 | 1.007 | 1.042 | 6/7 | **fail** (both clauses) |
| atx-risk-v1.1 | 1.181 | 1.108 | 1.104 | 1.084 | 1.039 | 1.036 | 0.996 | 1.080 | 3/7 | fail |

How to read it:
- Margins from 1, in pooled band widths:
  - atx-cov-v1 is 0.36 widths inside the band.
  - The a = 0.5 control is 1.7 widths out, and fails the rank share too (6/7 = 0.86).
  - atx-risk-v1.1 is 3.2 widths out. At HL 42 (round 1) it was 1.7.
- Worst atx-cov-v1 rank: eigen-05 at 1.021, which is 0.32 of its rank band.
- Agreement with the reviewer's numpy mirror at HL 21 (6 seeds):

  | | mirror | C++, seed 29 |
  |---|---|---|
  | atx-cov-v1 pooled | 1.0056 +- 0.0017 | 1.009 |
  | atx-cov-v1 ranks | 0.993 .. 1.011 | 0.993 .. 1.021 |
  | v1.1 pooled | 1.075 | 1.080 |
  | half strength (C++: a = 0.5) | 1.039 | 1.042 |

  C++ ran one seed. The rates across seeds are the mirror's.
- Test time: 25.6 s -> 38.6 s (one more atx-cov-v1 run).
- The churn panel (`small_config`, untouched) prints the same per-rank b as in round 1.

### R2b: K = 1 + G + S without a u32 wrap

- New in `cov_container.hpp`: `cov::geometry_ok(K, G, S)` with `cov::max_groups` and `cov::max_dense` = 4096. It
  requires G <= 4096, S <= 4096 and u64 K == 1 + u64 G + u64 S. Used in four places:
  - the header check in `check_header_and_trailer` (`cov_container.cpp:637`, was `:631`);
  - `parse_cov_block` (`:213-215`, was `:208-209`);
  - `encode_cov_block`'s `check_block`, which refuses G or S > 4096 before `spec.factors()` is formed
    (InvalidArgument);
  - `CovContainerWriter::create` (same constants, same rule).
- The reader therefore also refuses a geometry the writer cannot produce (G or S > 4096), even without a wrap.
- New test `RiskCovContainer.RefusesGeometryThatWrapsK`:
  - **Direct `parse_cov_block`.** The block header and the caller agree on (K 0, G 2^32-1, S 0), (K 0, G 0, S 2^32-1)
    and (K 4098, G 4097, S 0). Each gives ParseError "block geometry". The edge G = 4096 passes the geometry check;
    that K = 8 block is then refused by the array check, with no "geometry" in the message.
  - **Encoder and writer.** `encode_cov_block` with G = 2^32-1 gives InvalidArgument "geometry", and
    `CovContainerWriter::create` refuses it.
  - **Hash-consistent file.** For each of the three header geometries, with the root re-hashed: ParseError "header
    geometry" under Full and under Lazy.
- **Teeth.** Each refusal is asserted by its message. On the old code (read, not built) the same inputs are refused
  later, by other checks with other messages: the file by "factor names: K NUL-terminated names expected", the block by
  "array dtype or count", the encoder by "exposures are not rows x dense in the file's dtype" (`check_block` checks
  the exposures before the covariance). So the test fails on the old code. [Corrected by root at merge, M2d,
  re-review 2 S12: this line first named "factor covariance is not K x K".]
- **Consequence.** K is now bounded at 8,193 on every container path, so `factor_risk_view` cannot report
  2^32 - 1 groups. Through a container, `book::check_model`'s usize `k * k` (`atx-engine/src/book/risk_target.cpp:35`)
  can no longer wrap. That line is pre-existing and outside COV's files: a note for the engine owner (root item 5).

### Re-review nits and the earlier Suggested items

- **Fixed: section-table count clause.** Re-review (3) noted it was not exercised.
  `RiskCovContainer.RefusesSectionTableAndBlockSizesThatWrap` now keeps the writer's in-range table offset and sets
  `section_count` to 64. It asserts that what remains is < 64 x 64 bytes, and gets "section table out of bounds".
- **Fixed: S7 counter only ever checked at 0.** New test `CovBias.UnformedExtendedBooksAreCounted`, with 4 names on
  market + one industry:
  - a non-finite off-diagonal entry counts 1 and forms no book;
  - a non-positive-definite block (the LLT fails) counts 1;
  - the positive-definite control counts 0, and its minvar book holds all 4 names.
- **Not fixed: S3 verb-level refusal test.** The refusal is unreachable through the verb:
  - `load_role` refuses any `--max-bytes` <= 64 MiB + cells x 96 (`strategy_risk_verb.cpp:124`).
  - The kernel's worst workspace is at K 62 with Kish(504) <= 1,454: 62^2 x 128 + 1,454 x 62 x 64 = 6,261,504 B
    (6.0 MiB).
  - So no verb run can reach the budget refusal. The refusal stays as a free guard against a future default change.
    Its rule is tested at the kernel boundary (51,199 B refused / 51,200 B adjusted).
- **Not fixed: S7, an optimized book with gross 0 is skipped silently** (`strategy_risk_model.cpp:1490`). w = V^-1
  (alpha - c 1) is 0 only when alpha is constant over the held names. That has probability zero for continuous random
  scores, and counting it would need a new summary key.
- **S1 not fixed:** the names are the brief's acceptance contract; the base-anchored proof is root step 2.
- **S5:** superseded by COV-7.
- **S8:** by design. It is the D-COV reading below and a P10 recipe candidate.
- **S9:** a root check (COV-9), root item 1.

### Builds and tests

| tag | targets | exit | wall s | TUs |
|---|---|---|---|---|
| p9-cov-h | atx-engine-risk-tests, atx-impl-strategy-target-tests, atx-equity-strategy-risk | 0 | 606 | 369 (fresh tree) |

- **Receipt:** Source `e9fc3668`, DirtyEntries 0, Jobs 4.
- **Warnings:** only clang-cl's `/MP unused` driver note and the two third-party `getenv` deprecations in
  `databento-cpp`. 0 in lane code.
- **Configure.** The tree had been deleted after round 1, so it was configured first. The first attempt of my gate
  wrapper died in PowerShell: cmake's stderr became a NativeCommandError under `ErrorActionPreference = Stop`. That
  happened before research-build ran, so there is no receipt and the tag was not spent. I fixed the wrapper and re-ran
  it.
- **Gate:** admitted at free 3,760 MiB / commit 5,360 MiB, with no other compiler, linker, ninja or cmake running.
- **Evidence** (`cov-evidence/`): `cov-h.log` (gate + configure), `mega-p9-cov-h-{receipt.json,build.log}` (UTF-8).

Tests (Debug, `p9-cov-h` executables):
- `atx-engine-risk-tests --gtest_filter=RiskCovContainer.*` -> **21 passed** (`cov-gt-container-f2.log`).
- `atx-engine-risk-tests --gtest_filter=Risk*:-RiskQpAugment.MatchesDenseOracleAcrossBattery` -> **431 passed, 1
  skipped** (`RiskQpAugmentNightly.MatchesDenseOracleAcrossLargeBattery`, nightly), 0 failed, 138 s
  (`cov-gt-engine-risk-f2.log`).
- `atx-impl-strategy-target-tests --gtest_filter=CovTruncation.*:CovEigen.*:CovRecipe.*:CovStore.*:CovBias.*:CovDiff.*`
  -> **29 passed**, 0 failed, 236 s (`cov-gt-cov-all-f2.log`).
- Unchanged filter `RiskWls.*:RiskEwma.*:RiskBias.*:RiskModel.*:RiskVerb.*:RiskRobust.*:RiskTarget.*:VolTarget.*:NavBookRule.*:Spo*:NavV7Hook.*`
  -> **104 passed**, 0 failed, 70 s (`cov-gt-regress-f2.log`).

New tests this round: `RiskCovContainer.RefusesGeometryThatWrapsK` and `CovBias.UnformedExtendedBooksAreCounted`.
Root step 1's counts become 21 (`RiskCovContainer.*`) and 29 (`Cov*`).

### What the churn-panel small-rank bias means for root's D-COV read

1. **Headline per rank, not pooled.**
   - On the churn panel, eigen-00 (6.07 / 7.12) carries about 2/3 of the pooled sum of z^2. Without it, pooled b is
     about 1.22 instead of 2.06.
   - eigen-00 is most likely the near-null direction the cap-weighted industry constraint leaves. That is plausible
     but not shown: there is no eigenvector diagnostic. It sits above the floor, so the floor rule does not remove it.
   - D-COV should print per-rank b, plus the pooled family with and without the lowest rank (or the rank aligned with
     the industry cap-share vector).
2. **The absolute band is a kernel test, not a TRAIN adoption criterion.**
   - The band holds only when S2b's simulated length equals the estimator's effective sample. Here that means HL 21 /
     21 and NW 0. The a = 0.5 control shows what the band then separates.
   - The recipe's settings are different: variance HL 84, correlation HL 504, NW 5 / 2. S2b simulates Kish(504)
     ~ 1,454 observations, while F's variances carry Kish(84) ~ 242, and the NW noise is not simulated (S8).
   - So atx-cov-v1 under-corrects the small ranks by design. "Above band" in small ranks on TRAIN is expected and is
     not a kernel defect.
   - Read atx-cov-v1 against atx-risk-v1.1 on identical observations: per-rank b reduction, minvar / optimized b, QLIKE
     and MRAD. The size of the remaining excess is the P10 recipe input (S8: simulated length = Kish of the variance
     HL, or simulate the EWMA estimator).
3. **Read rises at the large-rank end together with lambda^2_factor before calling them harm.** Ranks 04 / 05 / 08 rise
   on the churn panel. That can be a VRA level effect (the VRA reads the adjusted prior) rather than an eigen effect.
4. **COV-5:** every atx-cov-v1 parameter stays as fixed in the brief. A change after D-COV is a new recipe id.

### Root items (merge and after)

1. **S9 (COV-9).** Before every COV run (steps 3-5 and D-COV), check that the role manifest has
   `universe.delisting.returns_applied == false`. The first review found it false for `train-2020-2023-lo3`. The verb
   guard is P10's.
2. **Bench link and run (root step 6).**
   - Run the `rel` preset with `ATX_BUILD_BENCH=ON`, then `atx-engine-bench --benchmark_filter=CovContainer
     --benchmark_repetitions=5`, and log the medians against cov-design 4.12.
   - The bench TU has had only a syntax-only compile (`clang-cl /Zs`, round 1). It has never been linked or run. A
     compile or link error there is the lane's to fix.
   - This round added `cov::geometry_ok` / `max_groups` / `max_dense` to the header. The bench uses none of them.
3. **COV-7.** Step 5 (atx-cov-v1 on TRAIN) runs on the Release tree, after Debug = Release byte identity of the
   container on the lane fixture.
4. **Counts for root step 1:** `RiskCovContainer.*` 21, `Cov*` 29, unchanged filter 104, `Risk*` 431 + 1 skipped. The
   base-anchored flag-absent proof is still root step 2 (S1). In `atx-impl/tests/CMakeLists.txt`, keep both C2's and
   COV's tail lines.
5. **Engine owner note.** `atx-engine/src/book/risk_target.cpp:35` computes `k * k` in usize (pre-existing). It can no
   longer wrap through a container now that K <= 8,193. Record it for the engine owner.

### Ledger candidates

- The eigen-family band is a kernel test only with equal variance / correlation half-lives and no NW lags. At HL 21 /
  T 469 / K 7, the C++ pooled b is: atx-cov-v1 1.009 (7/7 ranks in band), a = 0.5 1.042 (out), v1.1 1.080 (out), with a
  band of +-0.025. At HL 42 the teeth were too thin (mirror: half-strength variants passed in 50-75% of seeds).
- The atx-cov-v1 budget refusal cannot be reached through the risk verb. `load_role` needs `--max-bytes` > 64 MiB, and
  the kernel's worst case at K 62 / Kish(504) is 6.0 MiB.
