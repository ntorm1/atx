# atx-vol r6 known issues (Q0 release qualification of `e5438aa5`)

Frozen candidate: `feat/vol-resume5-20261022 @ e5438aa5d406f5ae28b21f59ee1fcc15cc4ae782` (the W2c-m docs commit;
final code root `546201d8`). Qualified by lane Q0 (run `vol-sota-r6-20261023`). Evidence:
`C:\atx-cache\quote-benchmark\Q0-20261023-qualification\` (`sha256.txt`). Receipts: `q0-receipts-r6.json` (this
folder). Every line below is an open item of THIS candidate: it is listed, not fixed. A number is a measured value
with its source; "Q0" marks a value re-measured on the frozen root by this lane.

Nothing here is promoted. Promotion (Q2) and the holdout (Q1) are separate lanes.

## A. Found or re-measured by Q0 on the frozen root

1. **48 full-label failures outside the ignore list (Q0-F1..Q0-F48): pre-existing, not regressions of this sprint.**
   The ONE full-label run (`-L atx_vol_fast` 4,785 tests + `-L atx_vol_slow` 259, vol-rel, the monolith built once)
   failed 40 fast + 14 slow tests. Six are the ignore list (`AmericanIv.PricePolishDefaultOffIsBitForBitControl`,
   `SimdNanSafety.BaryEvalPd_ExactNodeHit_ReturnsNodeValue`, the umbrella suite: ctest `atx-vol-umbrella` + the three
   `VolUmbrella.*` cases in the monolith). The other 48 all reproduce on slice executables built 2026-10-16..19, before
   the plan base `7344eb37` (Q0 evidence `oldbin\`), and their test files are unchanged in `7344eb37..e5438aa5`. They
   sit in the older PricerFitter / curve-fit / SurfaceV2 / mark stack behind `VolaSession` / `PricedSurface` and in
   storage / backtest / dispersion pins; none is in the r6 product suites (quote-slice, serve-record, served-analytics,
   tick-path, chain-batch, discrete-div all green). Clusters by first-failure message: A = a fit / mark refused (30);
   seven outputs carry `fit_slice_curve: linear-variance slice has uncertified density or a negative density atom`, a
   refusal added by `524bf6fd` (2026-10-15), the candidate cause to route (curve-fit / pricer-fitter owner).
   B = an exact-equality expectation (last-bit numbers, enum statuses, counts) (15). C = other (3). Q0 made no fix.
   - Q0-F1: `ArbButterflyScheme.CurveGateAndCountEntryMatchThePreUnificationStencil` (fast, B; `arb_test.cpp:1280`)
   - Q0-F2: `BacktestExec.L2SettlementMarkMemoDropsExpirySolve` (fast, C; `backtest_exec_test.cpp:1872`)
   - Q0-F3: `Breakeven.GoldenPathPnlIsPinned` (fast, C; `breakeven_test.cpp:223`)
   - Q0-F4: `CatalogTest.RecordTrialAgainstUnknownTrackFails` (fast, B; `catalog_test.cpp:933`)
   - Q0-F5: `ChainExport.PinnedSplineVolIsTheFamilyActuallyServed` (fast, C; `chain_export_test.cpp:1034`)
   - Q0-F6: `CurveFitDiagnostics.UnderdeterminedChiSquareIsFlaggedAndBandEvidenceSurvives` (fast, A; `curve_fit_coverage_test.cpp:392`)
   - Q0-F7: `CurveFitSliceFallback.LinearFallbackRecoversForcedSplineFailures` (fast, A; `curve_fit_parallel_test.cpp:534`)
   - Q0-F8: `CurveSelector.RelaxedCoverageFloorsAdmitAPartiallyCoveredSoleCandidate` (fast, A; `curve_selector_test.cpp:807`)
   - Q0-F9: `CurveSelector.SplineCandidateFlagAddsCandidate` (fast, B; `curve_selector_test.cpp:729`)
   - Q0-F10: `DeAmFitCache.CachedAndColdLinearVarianceFitsAreEconomicallyEquivalent` (fast, A; `session_test.cpp:1208`)
   - Q0-F11: `EssviFitSlice.FixedFixtureFit_IsBitIdentical` (fast, B; `essvi_calib_test.cpp:202`)
   - Q0-F12: `LinearVarianceCurve.CalendarFloorUsesPriorBreakpoints` (fast, A; `pricer_fitter_test.cpp:2916`)
   - Q0-F13: `ListedDispersionStrategy.ForcedColdValidationPublishesExactEntrySeedsOnFastSnapshot` (fast, B; `listed_dispersion_strategy_test.cpp:243`)
   - Q0-F14: `MarkButterflyHonesty.MarkDigestCarriesItsOwnButterflyTally` (fast, A; `surface_v2_qualification_test.cpp:917`)
   - Q0-F15: `MarkButterflyHonesty.PolicyCanDemoteAButterflyCarryingMarkToDegraded` (fast, A; `surface_v2_qualification_test.cpp:975`)
   - Q0-F16: `MarkRescue.MarkRescueIsBitIdenticalAcrossFitWorkerCounts` (fast, B; `mark_rescue_test.cpp:233`)
   - Q0-F17: `MarkRescue.RiskRefusedBoardStillYieldsAMarkSurface` (fast, B; `mark_rescue_test.cpp:180`)
   - Q0-F18: `PreparedFitting.ConfiguredPopulationIsDeterministicAndFamilyAgnostic` (fast, A; `prepared_fitting_test.cpp:165`)
   - Q0-F19: `PreparedFitting.CurveFitParityOptOutReportsZeroScoreInversions` (fast, A; `prepared_fitting_test.cpp:897`)
   - Q0-F20: `PricerFitterPolicy.AdmissionFailureRetainsPublishedSurfaceAndDecisionTransactionally` (fast, B; `pricer_fitter_test.cpp:1892`)
   - Q0-F21: `PricerFitterPolicy.CertificationAndAdmissionBitIdenticalAcrossFitWorkerCounts` (fast, A; `pricer_fitter_test.cpp:2108`)
   - Q0-F22: `PricerFitterPolicy.RiskAutoFitFallsBackWhenPrimaryFailsIndependentOracle` (fast, A; `pricer_fitter_test.cpp:1782`)
   - Q0-F23: `PricerFitterPolicy.SessionOverlayCurveIsThePublishedDecisionAndReportCurve` (fast, B; `pricer_fitter_test.cpp:2013`)
   - Q0-F24: `PricerFitterValueChain.ColdPriceOnlyChunksAreDeterministicAcrossWorkers` (fast, A; `pricer_fitter_test.cpp:1389`)
   - Q0-F25: `PricerFitterValueChain.SameExpiryChunksRemainBitIdenticalAcrossWorkers` (fast, A; `pricer_fitter_test.cpp:1329`)
   - Q0-F26: `StrategyPipelineRisk.AggregatesRiskAndSupportsClampOrReject` (fast, B; `strategy_pipeline_test.cpp:152`)
   - Q0-F27: `SurfaceDbPopulate.PropagatesStoredSurfacePolicyAndPersistsServedProvenance` (fast, B; `surface_db_populate_test.cpp:1366`)
   - Q0-F28: `SurfaceDbPopulate.SymbolConfigOverlayReachesFit` (fast, B; `surface_db_populate_test.cpp:1780`)
   - Q0-F29: `SurfaceV2FailClosed.MarkOnlyConfigServesTheMarkByDefault` (fast, A; `surface_v2_qualification_test.cpp:410`)
   - Q0-F30: `SurfaceV2FailClosed.UnservedRiskIsNeverAnsweredByTheMarketMark` (fast, A; `surface_v2_qualification_test.cpp:378`)
   - Q0-F31: `SurfaceV2LegacyCompat.HftWithPinnedLinearVarianceIsAMarkOnlyRequest` (fast, A; `surface_v2_qualification_test.cpp:432`)
   - Q0-F32: `SurfaceV2LegacyCompat.InvalidRiskPolicyStillPublishesTheRequestedMark` (fast, A; `surface_v2_qualification_test.cpp:463`)
   - Q0-F33: `SviMmCalib.FixedFixtureFit_IsBitIdentical` (fast, B; `svi_calib_test.cpp:362`)
   - Q0-F34: `VarSwapMemo.RowsAreBitIdenticalToTheUnmemoizedPerRowPath` (fast, B; `deriv_book_test.cpp:1255`)
   - Q0-F35: `AllQualityModes/SurfaceV2Qualification.KnownTruthBoardPublishesSeparatedAdmittedDeterministicBundle/Accuracy` (slow, A; `surface_v2_qualification_test.cpp:127`)
   - Q0-F36: `AllQualityModes/SurfaceV2Qualification.KnownTruthBoardPublishesSeparatedAdmittedDeterministicBundle/Balanced` (slow, A; `surface_v2_qualification_test.cpp:127`)
   - Q0-F37: `AllQualityModes/SurfaceV2Qualification.KnownTruthBoardPublishesSeparatedAdmittedDeterministicBundle/Latency` (slow, A; `surface_v2_qualification_test.cpp:127`)
   - Q0-F38: `PricerFitterTest.AutoRoutedLinearVarianceMarkAlwaysScoresParity` (slow, B; `pricer_fitter_test.cpp:2679`)
   - Q0-F39: `PricerFitterTest.GenericParityOptOutCannotPublishRiskButCanPublishMark` (slow, A; `pricer_fitter_test.cpp:1065`)
   - Q0-F40: `PricerFitterTest.HftCarryBankQueriesExplicitlyEnableBankedCacheServing` (slow, A; `pricer_fitter_test.cpp:1692`)
   - Q0-F41: `PricerFitterTest.HftQueryPricingTiersKeepColdAndRepresentativeDistinct` (slow, A; `pricer_fitter_test.cpp:1667`)
   - Q0-F42: `PricerFitterTest.HftUsesDirectLinearVarianceCurve` (slow, A; `pricer_fitter_test.cpp:1653`)
   - Q0-F43: `PricerFitterTest.PinnedCurveKeepsTheFloorFreeMarkParityOptOut` (slow, A; `pricer_fitter_test.cpp:2726`)
   - Q0-F44: `PricerFitterTest.SelectorServedBreadthFloorBindsOnANonEssviFamily` (slow, A; `pricer_fitter_test.cpp:2809`)
   - Q0-F45: `PricerFitterTest.UnsupportedFamilyRefitIsExplicitAndTransactional` (slow, A; `pricer_fitter_test.cpp:849`)
   - Q0-F46: `PricerFitterTest.V2MarkBidAskFloorRetainsParityScoringByDefault` (slow, A; `pricer_fitter_test.cpp:1168`)
   - Q0-F47: `QualifiedCorpus.QuarantinedFitStaysReportedAndCannotLeakIntoADateArchive` (slow, A; `corpus_test.cpp:1419`)
   - Q0-F48: `SurfaceV2Fallback.InvalidRefreshKeepsLastAdmittedRiskGeneration` (slow, A; `surface_v2_qualification_test.cpp:248`)
2. **Quiet-host timing rows on the frozen root (no other r6 lease live; load lines in the evidence).**
   - G3: SPY serial cold, 10 fresh trials, K4b batch policy: min 2.433 / median 2.473 / max 2.766 s vs the 1 s bar
     (MISS); inverses 1,000 ms (K3 enclosure 605 ms), final grid 512 ms, endpoints 340 ms, reference 299 ms.
   - F3: SPY all-core (16 workers): median 0.777 s vs 150 ms (MISS); the longest chain 0.447 s bounds any width.
   - H1: record call p50 / p99 83.7-110.6 / 99.4-146.2 ns (MET); through the book (steady) 122.6-149.4 ns p50 (MET,
     SPY tick at 149.4 ns on the margin); under concurrent publication 253-298 ns p50 (1.7-2.0x over 150 ns).
   - H2: record IV 194-214 ns p50 (MET); tier-0 300-chain 13.7-18.7 us (MET); mixed 300-contract served chain through
     the book 141-267 us p50 vs 100 us (MISS); the default 12 x 6 plane answers ladder IV `Unconfirmed`.
   - H3: miss 0, all-query p99 6.3-8.9 us, p99.9 8.7-11.9 us, max 35-350 us; quote burst all fresh in 20-92 ms,
     `budget_exhausted` 0.
   - A1: chain (334) Taylor 1,003.8 / 1,203.9 us and exact 2,893.1 / 3,424.0 us per contract (p50 / p99); the
     remainder estimate misses 9 / 516 vol-only and 1 / 5,444 mixed cells (the exercise-edge kink).
3. **G2 kernel speed bars on the quiet G3 row (K4b A1-A4):** family side 662 ms (bar 300), final grid 512 ms (bar
   200), endpoints 340 ms (bar 120) MISS; confirm 41.2 us amortised (bar 50) met.
4. **R1-R3 still reproduce on the frozen root**: 15 / 15 reproducers refuse the same chain with the same error
   (5 non-finite node price, 6 invalid grid volatility, 4 Legendre condition).
5. **The FINAL review and Q1 are separate.** Q0 did not read the holdout; any FINAL-review finding is the PM's to add.

## B. Carried from the PM's pre-dispatch list (through W2c-l)

1. **L3g in-band theta (A0 / L3g owner).** The L3g sigma-family bundle serves the in-band theta with status `Ok`:
   a wrong number with an `Ok` status. The (sigma, q) bundle masks it (A0 re-review; review-L3g m5 in-band part OPEN).
2. **Wheel DLL bundling (W2c-g).** `atxvol-1.1.0` (sha256 `0ad53ac8...`) bundles neither `arrow.dll` nor
   `parquet.dll`; `test_chain.py` cannot be collected (pyarrow 18 vs vcpkg Arrow 24 DLL collision). Binding run: 24
   failed / 217 passed / 5 skipped: 20 by design (`test_run_sp100_strangle_backtest.py` refuses an installed atxvol),
   4 unexplained (2 `test_report_from_archive.py` "no friction_regime", 2 `test_surface_db_build.py` "no expiry
   produced a usable eSSVI slice"). Not rebuilt at Q0.
3. **D1 misses (G5).** Per-event drift misses D0's u on 3 of 4 chains (Refined) and 4 of 4 (Standard); ILp Refined
   +4.49 bp is an UNRESOLVED row. Every P1 / P2 / P3 cash-dividend latency budget is a MISS (cold 24.1, price 23.8,
   IV 114.2, bundle 191.3 us p50; inverses 99.94 % of the cold clock). The bounded Chebyshev challenger was not run.
4. **A0 300-chain stretch.** The 300-chain <= 1 ms stretch is missed (1.7-2.4 ms p50, loaded host).
5. **Inert family certification grid (K3 B(i), routed to K4).** K4's disposition stands; no Q0 change.
6. **Archive withheld-row mask (C2 -> H0 / D1).** The fit archive does not carry the withheld-nodes mask.
7. **K4 audit exercise-class column.** Typed outcomes are not row fields of the audit output (K4 minor -> H1 / Q0).
8. **K3 put envelope.** SPY puts verify all 48 premium quadrature nodes (K5); the H0-A fixture's 34 unverified put
   nodes are explained by elimination only (review-K5 m4): re-measure the put per-point bound through
   `deam_family_certify` before re-pinning ladders.
9. **K5 m6.** The inverse sub-clock behind "99 % of the saving on the confirm sub-clock" was a temporary diagnostic;
   K4b's committed clocks supersede it (confirm 276 ms = the diagnostic's 276 ms).
10. **K4b m-1.** 43 BABO rows move by up to 0.506 vol (chain `1143835804049873679`) through the A17 retyping without a
    per-root typed reason; attribute each chain to a typed root before promoting the typing.
11. **K4b m-2.** The lane-vega hook fires 0 times in production on every cohort (unit-tested only).
12. **K4b m-3.** Three code lines exceed 100 columns (`same_opts`, two `t0 = timed ? ...` in `iv_native_prepare_*`).
13. **K4b m-4.** `rung_memo.hpp` and `american_iv_premium.hpp` were edited outside K4b's listed scope.
14. **K4b m-6.** Puts are not re-hulled after the opt-in TIGR raise (convexity is caught only by `create`): a Q2
    precondition for `calendar_parity_clamp`.
15. **K4b `reference_budget.wall_us`.** Measurement-only: the served set is host-dependent (dev200 lost 317 / gained
    1,931 rows when it binds). Never promoted.
16. **K4b speed bars A1-A4 / A6 MISS** (family side 1,095 ms, confirm 53.0 us, grid 587 ms, endpoints 406 ms, cold
    4.17 s at K4b's tip).
17. **H1 book tier-0 p50 and the mixed 300-chain** (lane rows, loaded host: book p50 197-246 ns steady, 427-459 ns
    under publication vs 150 ns; mixed 300-chain 183-513 us vs 100 us). Q0's quiet re-measure is in section A.
18. **H1 default-plane ladder IV `Unconfirmed`.** Only tick-like planes (`serve_record_policy_tick()`) give `Ok`.
19. **H1 m10.** No test pins an escrowed-PV record against its fit.
20. **H1 m15.** A duplicate file-local Clenshaw evaluation is declared, not de-duplicated.
21. **H1 no TSan on clang-cl.** Seven stress runs (torn 0) stand in; there is no sanitizer build in this repo.
22. **H1 `ServeAnswer` price-only reason / IV interval.** Carried since fix round 1; A1 consumes them.
23. **H1 reader-side mutex.** Miss / Stale / Invalidated refusal paths lock `queue_mutex` on the reader thread
    (documented, not wait-free).
24. **P0 F3 / F4 / F5 development MISSes.** P0 lane rows (loaded host, before K4b / K5): F3 1.182 s all-16 (bar
    150 ms), F4 dev200 per-expiry median 13.0 ms / p99 236.9 ms (bars 30 / 100 ms), F5 16.48 s (bar 10 s). Q0's
    merged-root F3 row is in section A.
25. **P0 surface-level parallelism absent** (F5 floor 12.98 s; serial calendar / market passes ~10.4 % on dev200).
26. **P0 in-process width test is synthetic.** Only the pinned P-vs-E child test is RED on revert; the 4-expiry
    in-process test would not flip on the old root.
27. **P0 / K4b two-executable tests.** `fit_native_route_test.cpp` and `fit_fanout_determinism_test.cpp` compile into
    two executables each (quote-slice tests + the monolith; `fit_native_route` also into the misc slice).
28. **C3 repair arms reprice 111-141 linked rows** (C3 D6: dev200 graded-v1-serve 74 / 17 + dev200b
    h0-repair-serve 67 / 17 pooled).
29. **C3 SPY 17 / 32 on the combined arm** (-> C4: 26 / 33 with the in-box projection rung, section A).
30. **W2c-k cohort5 repair-arm first-run flips at workers 2.** Explained and fixed by P0's Eigen pin; Q0's 9/9 at
    workers 1 and 2 re-checks the defaults (section A).
31. **Eigen CPUID trap.** The determinism contract needs `EIGEN_NO_CPUID` + pinned cache sizes on every Eigen
    consumer (LEDGER, P0 c1); on this root 854 / 854 vol-rel TUs carry the define.

## C. W2c-m deferred minors (P0, C4, A1, U0 reviews; integration)

1. **P0.** `fit_scheduler.hpp`'s "pinning changes only WHERE a task runs" comment is true only with the Eigen pin;
   B0's runner is single-CPU (multi-worker trials need `f3_trials.py`); the default-policy widths 8 / 16 and dev200
   w2 / w4 cells were skipped by ruling.
2. **C4.** `tests/inbox_projection_test.cpp:275` is 112 columns; NonConverged rollback is tested on the pure function,
   not at slice level; the speed lever has no target number (Q0 names one in section A); dev200b loses 44 rows on 2
   chains to `calendar_arbitrage` (typed; containment -0.01 pp); 2 dev200 `infeasible_data`; keys-ON cost 17.36 ->
   63.50 s cold (final_grid_validation 10.46 -> 42.58 s): Q2 may not promote the rung until moved-node
   re-implication is cheap.
3. **A1.** The legacy `session.cpp` / `PricedSurface` / `chain_tick` path is not routed through the book (declared
   deviation); the Taylor remainder estimate is Sampled, not a bound, and misses at the exercise-edge kink in sigma
   (five of the six worst vol misses); A1's CHANGELOG entry still says "34" tests (38 after fix round 1); the
   null-book guard is defensive.
4. **U0.** The query tool's stale check is near-vacuous by construction (one reader; `current` stored before
   `published`): quote "stale 0" with that caveat; universe queue ages are not instrumented (SPY re-fits only);
   response tails are attributed to preemption by inference; the census is of `989e7e96`'s binary (Q0 re-ran the
   sample, section A).
5. **U0 R1-R3 (routed reproducers, `U0-20261023-universe\routing\`).**
   - R1: a fitted node price is non-finite or negative (`AmericanPriceSlice: node N: strike/prices must be finite,
     strike positive, prices nonnegative`), typed as `arbitrage_failure`: 5 chains (GOVT, SPSB, VGSH, OPCH, UTZ).
   - R2: `American price certification: invalid grid volatility`: 6 chains (SKYD x2, STRC x2, TRV, UUP).
   - R3: `nondividend American/European Legendre condition violated`: 4 chains (BTQ, EYPT, IEI, ZNTL; node 0 in 3).
   Lower priority input pathologies: `invalid derived carry or forward` (8 chains), `conflicting contract yields at
   the same strike` (6 jobs), `at least two nodes required` (1 chain).
6. **W2c-m integration.** No semantic fix. Not run under the light ruling (W2c-m): the 31-row keys-ON table, the
   cohort5 / dev200 arms, a second 10-trial row, the A1 bench row, a census re-run, ForceScalar, the umbrella suite.

## D. Earlier deferred minors (pointers; text in the sprint log and the ledger)

- `atx-vol/sprints/2026-10-16-common-model-resume4-sprint.md`: "Deferred minors" sections for W2c-e (B0, K1, D0),
  W2c-f (C1, K2, C0), W2c-g (K3), W2c-h (D1, A0), W2c-i (C2, H0-A), W2c-j (K4), W2c-k (C3), W2c-l (K5, H1, K4b),
  W2c-m (P0, C4, A1, U0).
- `atx-vol/docs/LEDGER.md` lines that record a deferred minor: 36, 515, 519, 523, 529, 533, 538, 544, 545.
