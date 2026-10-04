# P9 lane briefs (paste one section, plus "Rules for every P9 lane", into an Opus 5.5 implementer's dispatch)

Plan: `docs/plans/2026-10-03-p9-sprint-plan.md` (cited as "plan §n"). Finding ids (F-n, P9-Rn, NV-n, OR-n, FD-n, CM-n,
DS-n) and contracts (K-P9-n) are defined in plan §0 and §2.3. Literature ids (lit §n, [n], F1..F18) refer to
`docs/plans/2026-10-02-p9-literature-review.md`. Root fills `<frozen-sha>` and the pool at dispatch.

## Rules for every P9 lane

1. Read first: `.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding: never build C++, never run real data,
   never dispatch subagents, never push, never touch `atx-db/`, files only with the Write / Edit tools because the
   shell hook breaks heredocs), then plan §0.6 and §2.2-§2.3, then the review files your brief names. For C++:
   `.agents/cpp/agent.md` first; write code that compiles first time under clang-cl 18 `/W4 /permissive- /WX`
   (no unused variables, sign conversions or shadowing; 100-column limit; copy the owning file's idiom).
2. Work only in your leased pool on your branch (`feat/p9-<id>-20261003`, base `<frozen-sha>`). Lease:
   `powershell scripts\lease-worktree.ps1 -Branch feat/p9-<id>-20261003 -Base <frozen-sha> -Agent p9-<id>
   -RunId p9-<id>-20261003 -HeartbeatId p9-<id>-hb -MaxPool 20` (root may have leased it for you; check `-Status`).
3. Blind. Do not open any return, IC, Sharpe, turnover or NAV output of 2020-2023 (`build-equity/` NAV, cards,
   marginal, admission and diagnostics files are closed; manifests, receipts and field lists are open). Nothing dated
   2024-01-01 or later is opened by you or by code you run. The numbers in status 7 and the ledger are public.
4. Identity discipline: every change is behind a flag or provably value-preserving; flag absent = byte-identical; say
   exactly how root verifies it (targets, gtest filters, argv, the expected byte-identical files, any substitution
   list). Never edit an expected hash. A Python copy of a C++ rule is deleted only in a later slice, after root's
   identity run (plan §0.6).
5. PM8-12: numerical and research logic goes in atx-engine C++ (generic) or atx-impl C++ (strategy-specific) with
   gtests; Python is orchestration, specs, receipts and reports. No new versioned copy of any script; no new
   `research_fields_*.py` builder module (plan DEC-5).
6. Stay inside "Files in scope". Touching a file another lane owns is a lane failure unless the brief names it as a
   cross-lane edit; list every such edit in the report. CMake: append one block at the end of the owning list.
7. Implement first, then the tests named in the brief (they are the acceptance contract) plus what pins behaviour.
   Run pytest yourself on synthetic data: `"C:/Program Files/Python312/python.exe" -m pytest -q -p no:cacheprovider
   <files>`. C++ tests are written, not run; name the anchored gtest filters root will run.
8. Commit per task: `git add <your files>`, conventional message, trailer
   `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
9. Report: `.superpowers/sdd/platform-p9-20261003/task-<ID>-report.md`, committed with `git add -f`, in the
   `.agents/harness/TEMPLATES.md` "Lane report" shape: outcome, branch / SHA, files changed, evidence (each pytest
   command with exit code 0 and output tail), how root verifies (build targets, gtest filters, identity runs with
   argv), deviations, cross-lane edits, open risks, 0-3 ledger candidates.
10. Final reply to the PM: at most 15 lines (status DONE / DONE_WITH_CONCERNS / BLOCKED, commit SHAs per task, one test
    line, concerns). An adversarial reviewer reads your exact SHA before merge; fix rounds get a new review.

---

## Lane COV: covariance of record without look-ahead, and the `atx.cov-container` binary file

**Pool / branch:** assigned at wave-2 dispatch (16 if the YARCH tree is released, else 7 or 8 after `-RecoverStale`,
else a fresh lease with `-MaxPool 22`), `feat/p9-cov-20261003`, run id `p9-cov-20261003`, heartbeat `p9-cov-hb`.
**Effort:** L. **Serves:** Sharpe (risk side: the vol-target / risk-target forecasts and the max-Sharpe path),
infrastructure. **Trials:** 0. No TDD (wave2-carry): implement, then the named tests.
**Base:** the P9 integration head after wave-1 merges and P9-B0 (C1 and T1 merged).

**Read:** `.superpowers/sdd/platform-p9-20261003/cov-design.md` in full (it is this lane's specification; section 3.2
is normative, section 4 is the byte layout), plan §0.6, §2.2-§2.3 with the amendment's K-P9-12, `wave2-carry.md`
"All lanes" and "C2" (C1's merge state). Code: `atx-impl/src/strategy_risk_model.{hpp,cpp}` (all),
`strategy_risk_verb.cpp` (all), `strategy_spo.hpp:221-266` and `strategy_spo.cpp:629-777` (the store),
`strategy_risk_target.{hpp,cpp}` as merged by C1 (`BookScaler::estimate`), `strategy_nav_replay.hpp:17-21` (timing),
`atx-engine/include/atx/engine/risk/eigen_adjust.hpp` (use `eigen_adjust_v2`; its default amplification 1.4 must be
overridden), `atx-engine/include/atx/engine/book/risk_target.hpp:55-62` (`FactorRiskView`),
`atx-tsdb/include/atx/tsdb/mapping.hpp` (the mmap RAII to use) and `segment.hpp` (house format idiom: `tag8`, POD
records with `static_assert` sizes, offsets from base), `atx-core/include/atx/core/sha256.hpp`. Tests:
`atx-impl/tests/strategy_risk_model_test.cpp` (synthetic panels), `strategy_spo_fixture.hpp:74`
(`write_risk_model`), C1's `strategy_vol_target_test.cpp` `VolTarget.TruncationInvariant` (the consumer-side pattern).

**Deliver (ordered; one commit per task):**

1. **Container library (engine).** New `atx-engine/include/atx/engine/risk/cov_container.hpp` and
   `atx-engine/src/risk/cov_container.cpp` implementing cov-design §4 exactly: format constants; POD `FileHeader`,
   `SectionEntry`, `DateIndexEntry`, `BlockHeader`, `ArrayEntry`, `Trailer` with `static_assert` on size and field
   offsets; `CovContainerWriter` (create `<path>.partial`; `append(const CovBlockInput&)` enforcing ascending as_of,
   cutoff <= as_of, constant geometry, the priced-first row rule; incremental per-block SHA-256; `finalize()` writes
   section table, META (canonical JSON), FACTOR_NAMES, DATE_INDEX, BLOCK_HASHES, header with root, trailer, flushes,
   renames); `CovModelFile::open(path, std::optional<root pin>, VerifyMode::{Full, Lazy}, max_bytes)` over
   `atx::tsdb::Mapping` (every check of §4.8; Full hashes blocks on a deterministic pool with fixed partition;
   Lazy uses a per-block atomic flag); `view(t)`, `exact(session)`, `at_or_before(session)` (never a block whose
   cutoff exceeds the session); `CovBlockView` (spans into the mapping); `factor_risk_view(view, scratch)` returning
   an `engine::book::FactorRiskView` over rows [0, P) (zero-copy except f32 exposures widened into `scratch`);
   `block_sha256(t)`, `root_sha256()`; `first_difference(a, b, through_session)` for diff tools. No wall clock,
   no RNG, zero padding everywhere. Errors as `atx::core::Result` (`ParseError` for structure, `InvalidArgument` for
   pins and refusals). One line in the `atx-engine` source list (cross-lane, below).
2. **Recipe `atx-cov-v1` (impl estimator).** In `strategy_risk_model.{hpp,cpp}`: `enum class RiskRecipe {RiskV11,
   CovV1}` in `RiskModelConfig` (default RiskV11; every RiskV11 byte unchanged); under CovV1 the S2b stage of
   cov-design §3.1: after the fully observed block's eigenvalue floor and before bordering structural factors, call
   `engine::risk::eigen_adjust_v2(F_keep, n_eff_t, 64, 1.0, seed_t, max_bytes)` with n_eff_t = (sum w_s)^2 / sum
   w_s^2, w_s = 2^(-(t-s)/504) over the sessions s <= t with a market factor return, and seed_t = splitmix64(recipe
   seed 7 XOR the session's ns); the structural border and its PD scaling are applied to the adjusted block as
   today; the VRA bias then reads the adjusted (pre-VRA) prior. `RiskDay` gains the eigen gammas, n_eff and an
   adjusted flag; the manifest `recipe` block gains `eigen_adjustment {method "menchero-wang-orr simulated (USE4
   B7)", amplification 1.0, simulations 64, seed 7, simulated_length "kish effective count of the HL-504 weights",
   seed_rule "per session"}` and `model` reads `atx-cov-v1`, both only under CovV1. A session where the adjustment
   cannot run (fewer than K+1 effective observations, a non-positive eigenvalue) keeps the unadjusted block and
   clears the block's eigen flag; it is counted in the manifest, never silent.
3. **Truncation and eigen tests (impl).** New `atx-impl/tests/strategy_cov_fixture.hpp` (a synthetic panel builder:
   planted factor returns and exposures, two IPOs, two delistings, membership churn, missing descriptors, one thin
   industry; `truncate_after(c)`, `perturb_after(c, seed)`, `add_future_instruments(c, k)`) and
   `strategy_cov_model_test.cpp`. The harness runs `run_risk_model` under each recipe with an in-memory container sink
   and compares block bytes. Extract a shared panel builder from `strategy_risk_model_test.cpp` into the fixture only if
   you reuse it (no assertion there may change).
4. **Writer wiring (impl verb).** In `strategy_risk_verb.cpp`: flags `--recipe atx-risk-v1.1|atx-cov-v1` (default
   `atx-risk-v1.1`), `--emit-container` (no value; absent = none), `--bias-families v1|extended` (default `v1`); a
   `ContainerSink` (`RiskSink`) that writes one block per session (rows = every instrument with an exposure row, a
   finite specific variance or a nonzero style; priced rows first; f32 exposures so the container and
   `style_exposures.f32` hold the same bits; FACTOR_RETURN, FACTOR_FLAGS, ROW_FLAGS, DIAG, EIGEN_GAMMA filled);
   `manifest.json` gains `container: {file "model.atxcov", format "atx.cov-container/1.0", bytes, file_sha256,
   root_sha256}` and a `files` entry only when the flag is given; still written last. In
   `atx-impl/tools/equity_strategy_risk.cpp`, two verbs dispatched to functions declared next to
   `dispatch_risk_model`: `cov-info FILE [--verify full|lazy]` (header, geometry, sessions, flags, root, per-block
   status) and `cov-diff A B [--through SESSION]` (exit 0 when every block hash agrees through the session, else
   prints the first differing session and exits 1) plus `cov-diff --legacy DIR` (every block scattered to the dense
   legacy layout equals `factor_covariance.f64`, `specific_variance.f64`, `style_exposures.f32`, `industry_slot.u8`
   row for row, bit for bit).
5. **Store read path (impl, NAV-free).** In `strategy_spo.{hpp,cpp}`, RiskStore section only: `RiskStore::open` keeps
   today's path for a manifest without `container`; with it, maps the container (Full verification, root equal to the
   manifest's), checks the role pin, geometry 62 / 50 / 11, date count, sessions and forecast flags against
   `diagnostics.csv`, and skips hashing the legacy payloads it will not read; `read(d)` rebuilds the dense
   `RiskSlice` from block d with today's defaults and rules (slot 255, the estimator's quiet NaN specific, style 0;
   NaN covariance -> 0 counted; non-finite style -> 0). `RiskStore` stays copyable-by-shared_ptr as today (it is
   held in `std::shared_ptr<const RiskStore>`); the mapping lives in a `std::shared_ptr<const CovModelFile>` member.
   No NAV, scaler or spo-v3 file is edited.
6. **Extended bias families (impl).** In `BiasHarness` behind `--bias-families extended`: `eigen` (the K
   eigenportfolios of F_{t-1}, z = u_k' f_t / sqrt(lambda_k)), `minvar` (fully invested V_{t-1}^{-1} 1 over the
   eligible names with a complete forecast, by Woodbury), `optimized` (32 seeded random-alpha books V^{-1} alpha,
   dollar-neutral, gross 1), and per family the QLIKE mean(z^2 - ln z^2) and the asset-level MRAD. Same rules as the
   existing families: complete forecasts only, a missing return counts 0 and is reported, never renormalise on
   realised availability. `v1` keeps `bias.json` and the manifest's `bias_harness` block byte-identical.
7. **Bench (engine).** New `atx-engine/bench/risk_cov_container_bench.cpp` (auto-globbed into `atx-engine-bench`):
   the benchmarks and targets of cov-design §4.12 on synthetic containers (K 62, S 11, D 1,000; N 3,000 and 5,627;
   f32 and f64), plus the labelled emulation of the legacy open / read. Prints the SHA-256 backend.
8. **Docs + report.** Two rows in `atx-engine/include/atx/engine/risk/README.md`'s file map (container, recipe
   pointer); `task-COV-report.md` with every item of rule 9, the measured nothing (lanes do not build) and the root
   procedure below verbatim.

**Gtests (the acceptance contract).** Engine, new `atx-engine/tests/risk/risk_cov_container_test.cpp`
(-> `atx-engine-risk-tests`): `RiskCovContainer.RoundTripIsBitExact` (f32 and f64 exposures, NaN payload bits kept),
`.SameInputsSameBytes`, `.HeaderLayoutIsPinned` (every offset of cov-design §4.3-4.6), `.ArraysAre64ByteAlignedAndBlocksPageAligned`,
`.RefusesTruncatedFile`, `.RefusesBadMagicOrMajorAndAcceptsHigherMinor`, `.RefusesIncompleteFlag`,
`.FullVerifyRefusesAFlippedBlockByte`, `.LazyVerifyRefusesAFlippedBlockByteOnFirstTouch`, `.RefusesRootPinMismatch`,
`.RefusesOverlappingOrMisalignedSections`, `.RefusesNonAscendingSessionsAndCutoffAfterAsOf`,
`.IgnoresUnknownOptionalSectionRefusesUnknownRequired`, `.AtOrBeforeNeverReturnsAFutureCutoff`,
`.PrefixRunsShareBlockHashes`, `.FactorRiskViewIsZeroCopyOnPricedRows`, `.WriterRefusesOutOfOrderAppendAndBadRowOrder`,
`.FullVerifyIsWorkerCountInvariant`.
Impl, new `strategy_cov_model_test.cpp` and `strategy_cov_store_test.cpp` (-> `atx-impl-strategy-target-tests`):
`CovTruncation.BlocksBeforeTheCutAreByteIdentical`, `.PerturbedFutureLeavesThePastUnchanged` (with teeth: a block
after the cut differs), `.FutureInstrumentsDoNotMoveThePast`, `.DetectsAPlantedLeak` (each for both recipes);
`CovEigen.RecipeV11IsBitIdenticalToToday`, `.ZeroSimulationsEqualsV11`, `.SmallEigenfactorsInflatedLargeNearOne`,
`.SeededPerSessionAndWorkerInvariant`, `.StructuralBorderAndPdScalingApplied`, `.VraReadsTheAdjustedPrior`,
`.ResultIsPositiveDefinite`, `.LowersTrueRiskOfTheEstimatedMinVariancePortfolio` (planted truth, fixed seeds averaged);
`CovRecipe.LegacyRecipeContainerEqualsDirectoryStore`, `.FlagsAbsentOutputsUnchanged` (shared files byte-equal;
manifest equal except `container` and its `files` entry), `.ManifestNamesTheContainerRoot`, `.CovV1RecipeRecorded`;
`CovStore.RiskSliceFromContainerEqualsDirectory` (every d, every field, `nan_covariance_entries`),
`.VolAndRiskTargetRecordsUnchanged` (C1's replay fixtures, both laws, every record and daily row bit for bit),
`.RefusesRoleGeometryOrRootMismatch`, `.LegacyManifestTakesTheLegacyPath`; `CovBias.EigenFamilyInBandOnAPlantedModel`,
`.MinVarAndOptimizedUseCompleteForecastsOnly`, `.MissingReturnCountsZeroNeverRenormalised`, `.V1FamiliesBytesUnchanged`;
`CovDiff.ThroughSessionReportsTheFirstDifferingBlock`, `.LegacyModeComparesBitForBit`.
Python: none changed. Run T1's guard tests as a sanity line (`scripts/tests/test_no_python_mirror.py`,
`test_no_versioned_scripts.py`) and paste the result.

**Files in scope (COV owns in wave 2):** new `atx-engine/include/atx/engine/risk/cov_container.hpp`,
`atx-engine/src/risk/cov_container.cpp`, `atx-engine/tests/risk/risk_cov_container_test.cpp`,
`atx-engine/bench/risk_cov_container_bench.cpp`, `atx-impl/tests/strategy_cov_fixture.hpp`,
`atx-impl/tests/strategy_cov_model_test.cpp`, `atx-impl/tests/strategy_cov_store_test.cpp`; modified
`atx-impl/src/strategy_risk_model.{hpp,cpp}`, `atx-impl/src/strategy_risk_verb.cpp`,
`atx-impl/tools/equity_strategy_risk.cpp`, the RiskStore section of `atx-impl/src/strategy_spo.{hpp,cpp}`
(`strategy_spo.hpp:221-266`, `strategy_spo.cpp:629-777`), `atx-impl/tests/strategy_risk_model_test.cpp` (fixture
extraction only), `atx-engine/include/atx/engine/risk/README.md` (file map rows), the report.
**Cross-lane edits (list them in the report):** `atx-engine/CMakeLists.txt` one line `src/risk/cov_container.cpp` in
the risk source list (beside `src/risk/specific_risk.cpp`, `:25-29`; the list is not globbed, `:121`);
`atx-impl/tests/CMakeLists.txt` two lines in the `atx-impl-strategy-target-tests` source list (`:114-135`; C2 appends
there too, root resolves the text).
**Forbidden:** `strategy_nav_replay.*`, `strategy_nav_v7.*`, `strategy_risk_target.*`, `strategy_vol_target.*`,
`strategy_spo_v3.*` and the rest of `strategy_spo.cpp` (C1 / C2 / C3 files); `atx-engine/include/atx/engine/book/**`;
`eigen_adjust.{hpp,cpp}` (use it; report a defect, do not fix it here); `atx-impl/CMakeLists.txt` (C2; no new impl
source file is needed: everything lands in existing TUs, which are /O2 in Debug, `atx-impl/CMakeLists.txt:114-129`);
any registered constant (cadence 21, floor 1, annualisation 252, bias 1.15, every atx-risk-v1.1 constant); any
expected hash; any real data.

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

**Out of scope:** any NAV-side change (the store pin is the switch); the `mv-aim-v1` target rule (lane COV-MV / P10);
statistical (hybrid) factors (COV-v2, P10); dense shrinkage, RIE or DCC-NL estimators; running D-COV or any cell;
Python; the Release adoption ruling.

---

## Lane COV-MV (optional, wave 3b; dispatch only if the PM registers P9-MV in P9)

**Pool / branch:** assigned at wave-3b dispatch, `feat/p9-covmv-20261003`. **Effort:** M. **Serves:** Sharpe.
**Trials:** 0. **Hard dependencies:** COV and C3 merged (C3's NAV rule registry with the `target` kind of K-P9-7 and
the typed NavSpec).
**Read:** cov-design §5.2; C3's merged registry and NavSpec; `strategy_spo.hpp:17-28` (the Grinold-Kahn alpha);
`atx-engine/include/atx/engine/risk/README.md` (FactorModel, Woodbury, the fast path's gross-normalisation property);
`cov_container.hpp` (`factor_risk_view`).
**Deliver:** (1) engine kernel `atx-engine/include/atx/engine/book/mv_target.hpp` + `src/book/mv_target.cpp`:
`mv_aim(const FactorRiskView&, std::span<const f64> score, std::span<const f64> sigma, std::span<f64> out)` =
gross-normalised P V^{-1} P alpha over the priced names, alpha_i = sigma_i z_i (any positive scalar such as IC or
1/sqrt(h) washes out under gross normalisation), via `risk::FactorModel` Woodbury, never dense; (2) NAV rule
`mv-aim-v1` of kind `target` in C3's registry: per decision d, from store row d (the pinned `--risk-model`), the
shared desired target's priced members are replaced by `mv_aim`, unpriced members pass through, and the whole vector
is rescaled to the desired target's gross (PM6-6 gross matching unchanged); construction and leverage follow
unchanged; refusal without a store; (3) template `scripts/specs/p9/templates/mv-aim.json` and a K-P9-11 rule file
(`predicted_mechanism`: "V^{-1} tilt lowers ex-ante and realised vol at equal gross alpha").
**Gtests:** `MvTarget.ThreeNamesDiagonalClosedForm` (w proportional to demeaned z / sigma), `.EqualsDenseInverseOnSmallN`
(1e-12), `.GrossNormalised`, `MvAimRule.FlagAbsentIdentity` (Y-F0 NAV byte-identical without the rule),
`.FutureReturnDoesNotChangePast`, `.RefusesWithoutAStore`, `.UnpricedMembersPassThrough`.
**Files in scope:** the new kernel files and test `atx-engine/tests/book/book_mv_target_test.cpp`, one registry row and
one parser entry in C3's files (cross-lane, listed), the template and rule file, tests in C3's NAV test target.
**Root verifies:** builds the NAV exe and target tests; Y-F0 / P9 parent NAV byte-identical with the rule absent.
**Out of scope:** costs inside the transform (spo-v2 / spo-v3 own that), running P9-MV.
