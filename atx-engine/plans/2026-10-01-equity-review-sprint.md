# Equity long/short review and remediation sprint

Date: 2026-10-01. Scope: `C:/atx/atx-engine`, directly in the shared checkout.
Preserve unrelated database and platform work. Coordinator commits each bounded
change. Three sub-agents own execution/accounting, risk, and alpha evaluation;
the coordinator owns data alignment, integration, and this record.

## Sprint 1: repair research and trading correctness

| Item | Problem and deliverable | Acceptance | Status |
| --- | --- | --- | --- |
| S1.1 | Dispatch standalone trade-participation and liquidation constraints; validate optimizer inputs | Public single/multi-horizon solves enforce or explicitly reject constraints; malformed inputs return errors | Complete |
| S1.2 | Validate execution prices, fees, permanent impact, and latency arithmetic before side effects | Invalid modeled fills cannot mutate portfolio, liquidity, or marks; valid later fills remain possible | Complete |
| S1.3 | Support finite negative net short rebates | Flat-price long/short replay books the expected financing debit without duplicate borrow charge | Complete |
| S1.4 | Repair CPCV return/turnover aggregation and consistent deflated-Sharpe sample statistics | Public fitness path preserves real fold returns and chronological turnover; unique-sample deflation | Complete |
| S1.5 | Reject ambiguous instrument IDs and malformed masks; remove redundant alignment work | Dataset-to-panel path preserves instrument mapping and missingness; existing PIT semantics remain intact | Complete |
| S1.6 | Integrate, review diffs, run narrow critical regression selections, commit results | Native build and selected workflow checks, exact commands and limits recorded | Complete |

Implementation precedes targeted regression additions. No broad test campaign,
real-data alpha search, or production trading is part of this repair sprint.

## Sprint 2: consistent alpha admission and scalable research

1. Refresh all search elite DSR scores when the trial count changes; version the
   cache and distinguish raw metrics from trial-dependent admission scores.
2. Profile real top-3000 workloads for the capped optimizer, alignment, and CPCV.
   Preserve certified constraints and numerical contracts when replacing repeated
   whole-book projections or copying. Record wall time, peak memory, and input hash.
3. Bind simulation settings (decision delay, neutralization, universe, costs,
   decay, and truncation) to candidate provenance and admission artifacts across
   both legacy and current evaluation paths. Audit existing coverage before adding
   a second implementation.
4. Require independent holdout qualification for production admission; the legacy
   `oos_fraction=0` default remains research-only. Integrate the currently isolated
   residual IC, delayed net-PnL, and alpha-pool diversification paths with explicit
   pool/calendar identity.

Acceptance: changing trial count refreshes all ranked candidates, selection never
uses lockbox observations, and performance changes preserve economic outputs.

## Sprint 3: executable equity long/short book

1. Integrate security transitions, delisting proceeds, and cash-claim settlement
   with admitted point-in-time evidence and the consuming pipeline.
2. Make financing-rate semantics explicit (fee versus net rebate, including zero),
   and connect available stock-loan quantity, recalls, and forced buy-ins through
   optimizer and execution. Audit existing borrow schedule/cap support first.
3. Calibrate chronological impact and participation against execution evidence;
   publish capacity only for an identified AUM, cost model, and liquidity universe.
4. Add broker order lifecycle/reconciliation and operational controls before live
   deployment. Research simulation primitives alone are insufficient acceptance.

Acceptance: one reproducible multi-year book handles changing membership,
corporate actions, short financing, unavailable borrow, and execution restrictions
with reconciled cash/positions and explicitly reported unavailable evidence.

## Validation and completion record

Sprint 1 is complete. Sprints 2-3 remain planned; the remaining findings are
explicitly open in the [review](../reviews/2026-10-01-equity-long-short-review.md).
The combiner lane also repaired nonfinite fitted-weight adoption and verified
recovery to valid weights.

Native Debug build succeeded with Windows clang-cl and the existing `equity-dev`
configuration. The initial multi-group build was deliberately stopped before
compiling unrelated test groups. The dedicated `EXCLUDE_FROM_ALL` review target
builds 11 existing/modified test translation units against the production library.

```powershell
# From C:/atx; reuse the configured equity-dev dependency installation.
& C:/atx/scripts/atx-build.ps1 build atx-engine-equity-review-tests -Preset equity-dev -Jobs 6
$reviewFilter = 'RiskConstraintDispatch.StandaloneLiquidity*:RiskConstraintDispatch.TrueMpc*:RiskConstraintDispatch.FastPortfolio*:RiskConstraintDispatch.PortfolioTrackingRetainsTurnoverCostAndPreviousBook:RiskConstraintDispatch.HorizonTrackingRetainsCapacityBoundAndHonorsOptOut:ExecSim.Invalid*:ExecSim.Latency*:ExecSim.VolumeCap_SellRemainder*:ExecSim.ReplacePending_SameBar*:ExecSim.Limit*:BacktestAccounting.*:BacktestIntegration.CostHonesty*:BacktestIntegration.NoLookAhead*:BookBorrowSchedule.*:BookBorrowSingleCount.FeeQuoted*:BookBorrowSingleCount.RebateQuoted*:BookBorrowSingleCount.FeeAndRebate*:FactoryFitness.FitnessIsOosOnly:FactoryFitness.Cpcv*:FactoryFitness.Deflation*:FactoryFitness.Turnover*:CombineWalkForward.Invalid*:CombineWalkForward.PointInTime*:CombineWalkForward.Hysteresis*:DataAlign.*:DataAdaptFeature.Instrument*:DataAdaptFeature.Missing*:DataAdaptFeature.Aligned*:CpcvCache.PoolAware*'
& C:/atx/build-equity/bin/atx-engine-equity-review-tests.exe "--gtest_filter=$reviewFilter"
```

Selected 55 checks across 11 suites, including all 11 newly added regressions.
First run: 54 passed, one test-fixture expectation failed, in 423 ms total.
The new feature-ingestion test incorrectly assumed terminal-date feature rows
were absent; FeatureMatrix retains them even when the forward label is unavailable.
Corrected its dated expected values, rebuilt only that test object and executable,
and reran the one failed check successfully (0 ms reported). No production change
was required after the initial run. Thus 55 distinct selected checks have passing
evidence; the other 54 were not unnecessarily repeated after a test-only edit.
Local XML evidence is in `build-equity/equity-review-results.xml` and
`build-equity/equity-review-data-recheck.xml` (not committed machine artifacts).

`git diff --check` passed. No full suite, real-data investment run, isolated
performance benchmark, or live broker validation was performed. Structural
performance improvements are documented without an unmeasured speedup claim.

| Commit | Delivered change |
| --- | --- |
| `5324ac8a` | Initial sprint plan |
| `63bdfdd7` | Dataset identity/membership validation and alignment performance |
| `88e511a3` | Liquidity-constraint dispatch, optimizer validation, finite combiner fits |
| `a2607f98` | Safe fill economics and negative short rebates |
| `622982b1` | Chronological CPCV scoring and consistent DSR sample |

The completion commit adds the bounded test target, corrected fixture expectation,
review, and this validation record. All changes are confined to `atx-engine`;
unrelated shared-checkout work was preserved.
