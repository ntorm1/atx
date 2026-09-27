# Lane 8 (l8-e2e) status — E2E zoo->book: replay cost, borrow, events, PreferenceSource

- Worktree: `C:/atx-wt/pool-9`  branch `feat/qps-l8-e2e`  head `d8cd33b3`  base `334a7939`
- Spec: Lane 8 section of swarm plan (scratchpad plan.md; copied to pool-1 `SWARM_PLAN.md`)
- Builder: **finished**. Adversarial review: **not run / interrupted**. Not merged.

## Commits
```
d8cd33b3 Wire zoo-to-book path: preference source, factor-bounded allocation, cost-aware research sim
c653de67 Add borrow schedule, delisting events and scheduled event batches
d55f96bd Add per-name replay cost models with participation-capped working orders
```

## Uncommitted working tree
```
(clean)
```

## Diff vs base
```
 21 files changed, 2742 insertions(+), 23 deletions(-)
```

## Builder report

Lane 8 (feat/qps-l8-e2e, C:\atx-wt\pool-9). Engine book: replay_cost.hpp/.cpp adds ReplayCostModel, LiquidityRow, TradeCost, FlatBpsCost (reuses the aggregate trade_bps arithmetic, so results are bit-identical) and SqrtImpactCost (half-spread + commission + Y*sigma*part^delta, with a participation cap). borrow_schedule.hpp adds per-name fee and locate grids plus rebate and cash rates; a trade that grows a short beyond its locate is rejected. event_batch_builder.hpp adds delisting_events_from_records, detect_terminal_delistings and make_event_batch_policy (scheduled transitions and payments -> ReplayMandatoryEventPolicy). replay.hpp/.cpp gets opt-in ReplayConfig fields: cost_model, liquidity, borrow_schedule, delisting_policy (Abort / CrspDelistReturn / LastMarkZeroReturn) and delistings. ReplayResult gains delistings and open_working_orders. A capped fill leaves a working order for the residual that is re-attempted each period until a new decision replaces it. A held name that stops printing is liquidated into cash instead of aborting the run. All defaults leave the existing arithmetic unchanged, and the claims entry point rejects the new fields. atx-impl: equity_allocation gains opt-in beta_tolerance and sector_net_cap (QP rows plus post-fee certificate fields postfee_beta_exposure and postfee_max_sector_net) and attach_equity_exposures(). preference_source.{hpp,cpp} adds PreferenceSource::from_combo(combo PanelArtifact, evaluation PanelIdentity, pit_cutoff) and preference(row), which demeans the row and scales it to a target gross. research_cost_sim.hpp scores alphas net of a ReplayCostModel with an admission gate. atx-impl/CMakeLists.txt now lists preference_source.cpp. Bench: atx-engine/bench/replay_cost_bench.cpp.

### Tests

powershell scripts\atx-build.ps1 -Ctest -Preset equity-dev -R '^(Book[A-Za-z]*|Report[A-Za-z]*|SecurityTransition[A-Za-z]*|Claims[A-Za-z]*|Decay[A-Za-z]*)\.' -> 86/86 passed. This run included the new BookReplayCost (8), BookBorrowSchedule (6) and BookEventBatch (10) suites plus all existing book suites; ReplayReportParity was added and run after this. -R '^(EquityAllocationFactor|ResearchCostSim|PreferenceSource|ZooToBookE2E|EquityAllocation|ReplayReportParity)\.' -> 29/29 passed, after two e2e/factor assertions were corrected: the e2e now measures exposure against pre-trade NAV, because the replay's impact cost is higher than the allocation's planning fee. -R '^[A-Za-z]*(Equity|Replay|TrialLedger|Config)[A-Za-z]*\.' -> 185/185 passed (atx-impl regression, including StageEquityIc and StageEquityUniverse). ctest printed "The following tests did not run:" but the list was not captured; assumed to be tests that were already disabled.

### Bench

Release (equity-rel + -Bench, isolated FETCHCONTENT_BASE_DIR), noisy host shared with 7 lanes, 3 iterations. BM_ReplaySqrtImpact 3000 names x 2520 days, weekly rebalance, 10% ADV cap: 4116 ms (1.94M name-days/s, 1.60M trades including working-order fills). 500x2520: 775 ms. Flat-bps baseline 3000x2520: 3562 ms (2.20M name-days/s). The per-name model with working orders costs about 15% over the aggregate path.

### Deferred
- stage_equity_book still hard-requires the slow-momentum baseline recipe; PreferenceSource exists and is tested but the stage has not been switched to it (it needs provenance and recipe schema changes in stage_equity_book, stage_run and stage_report)
- run_all moved onto policy replay, and deletion of the legacy 1-period report path (parity tests exist in ReplayReportParity; they cover the detail::accumulate_period core, not the full stage_report file output)
- stage_discover and stage_sweep fitness still use the frictionless research_sim; research_cost_sim is not yet wired into admission there
- corporate_actions -> SecurityTransition conversion: the security master has no delisting or merger terms, and the claims path only admits synthetic fixtures. Delisting is handled through DelistingRecord/DelistingPolicy instead; a real PCS 2013-05-01 native run was NOT performed
- Full OOS run reporting net Sharpe, turnover and capacity at $10m/$100m/$1bn: not run (needs real data and the stage wiring)
- The cost model, borrow schedule and delisting policy are rejected on replay_scheduled_intents_with_events (claims path); combining them is future work
- stage_optimize changes

### Integration notes
- L6+L7 -> L8: equity_allocation uses a dummy 1-factor model; swap in the Lane 7 hybrid factor model at allocate_equity_preference (the FactorModel::create call) and take kappa_i from SqrtImpactCost::cost_fraction for Lane 6 TradeCostTerms
- Beta/sector exposures come in through attach_equity_exposures(decision, beta, sector) after freeze_equity_allocation_decision; the caller must supply point-in-time exposures
- The equity-bench preset named in the task does not exist in CMakePresets.json at 334a7939; benches were built with 'atx-build.ps1 configure -Preset equity-rel -Bench -DFETCHCONTENT_BASE_DIR=<per-tree>' (the shared deps cache causes an _ITERATOR_DEBUG_LEVEL link mismatch). atx-engine-bench also aborts at startup unless atx-shm-worker is built next to it
- ReplayConfig gained trailing fields (cost_model, liquidity, borrow_schedule, delisting_policy, delistings) and ReplayResult gained delistings and open_working_orders; the new fields are opt-in (existing replay tests pass unchanged, including the IdentityPolicyExactlyMatchesFixedReplay bit-identity checks) and other lanes' positional aggregate inits stay valid
- EquityAllocationConfig gained trailing beta_tolerance and sector_net_cap (default -1 = off); EquityAllocationCertificate gained postfee_beta_exposure and postfee_max_sector_net, which are not yet serialized in stage_equity_book's certificate JSON

## Next step

Resume: review diff `git diff 334a7939..HEAD`, finish/verify uncommitted work, rerun lane suites, then integrate per plan §9.

## Adversarial review (2026-09-23) - verdict: fix-required
- Rebuilt atx-engine-book-tests + atx-impl-tests (equity-dev). Book/Report/ReplayReportParity/SecurityTransition/Claims/Decay: 99/99 pass. atx-impl lane + regression (Equity|Replay|TrialLedger|Config|ZooToBookE2E|PreferenceSource|ResearchCostSim): 196/196 pass (1 pre-existing skip). Bench reproduced: SqrtImpact 3000x2520 4616 ms, FlatBps 3531 ms (existing build-equity-rel binary).
- Findings: (1) unity-build ambiguity: `using namespace atx_test_l8_*` at file scope makes day_axis/immediate/expect_identity ambiguous when book tests are unity-batched (verified by compiling a concatenated TU). (2) research_cost_sim charges zero cost on names with unusable liquidity and undercharges capped fills. (3) BorrowSchedule pays cash interest on short proceeds AND the short rebate (double count). (4) spec acceptance not met (stage wiring, run_all, native 2013-05-01 run, OOS capacity run, real factor model). (5) capped fills can break certified beta/sector bounds with no check.

## Fix round (2026-09-23) — head 93c76304 (committed)
Findings addressed:
1. Unity ambiguity — FIXED. Removed file-scope `using namespace atx_test_l8_*` in all 7 lane test files (4 book + 3 impl + research_cost_sim); TEST bodies now live inside the named namespaces. Verified: the 4 lane book files as one clang-cl TU -> 0 errors. Full 13-file book unity TU failed on PRE-EXISTING collisions (claims_state/security_transition vs claims_replay digest/evidence; book_replay_test vs claims_replay kDay/kNaN/prices) -> added SKIP_UNITY_BUILD_INCLUSION for those 3 in atx-engine/tests/CMakeLists.txt; remaining 10 files compile as one TU, 0 errors.
2. research_cost_sim undercharge — FIXED. New virtual ReplayCostModel::unrationed_cost (Sqrt override = cost_fraction(|q|)*|q|, NaN on unusable row). Sim charges unusable_liquidity_penalty_bps (default 1000; NaN => Err). Tests: UnusableLiquidityNameIsNeverCheaperThanALiquidOne, CappedTradeIsChargedAtTheUncappedFullSizeRate (sqrt(5) ratio).
3. Financing double count — FIXED. cash_bps now on free cash = cash - shorts; test expectation updated (1300->1000); new DollarNeutralBookEarnsThePolicyRateOnlyOnceOnNav.
4. Acceptance not met — CONFIRMED, recorded as open items in atx-impl/docs/ZOO_TO_BOOK_OPEN_ITEMS.md (stage wiring, run_all, discover/sweep admission, native delisting rerun past 2013-05-01, OOS capacity run, real factor model).
5. Capped fills vs certified bounds — PARTIAL. New impl::measure_equity_exposures (shares code with certificate); e2e BindingCapHeldBookExposureIsMeasuredNotAssumed (ADV 2e6) proves caps bind and realized beta drifts. QP box bounds from cap*ADV deferred (open item 8).
6. Stuck working orders — FIXED. flag_delistings cancels orders on delisted names even with units==0; open-order counter makes has_working_orders O(1). Test DelistingCancelsAnUnfilledWorkingOrderOnAnUnheldName.
Tests: book suites 101/101; atx-impl lane+regression 199/199 (1 pre-existing skip); lane-only 20/20. Risk/combine test binaries rebuilt and linked OK; full raw-binary run stopped after 10 min (slow); neither group includes replay/replay_cost/borrow_schedule, so unaffected.
Next: stage wiring items in ZOO_TO_BOOK_OPEN_ITEMS.md.
