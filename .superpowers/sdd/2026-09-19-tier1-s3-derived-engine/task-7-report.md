# S3 T7 reconciliation report (2026-09-20)

Status: implementation reconciled with explicitly incomplete parity coverage. **S3 T8's all-18 retirement gate is not satisfied.** Work used Codex only. No registry, jobs, activation, catalog, or legacy-module implementation was changed.

## Delivered

- Generic monthly, universe-gated factor projection and the 24-row seed covering 18 proposed retirement modules plus the retained `asset_growth` module.
- Quarterly values selected by the newest fiscal period known at each cutoff, preserving then-visible revisions and preventing late revisions of older periods from displacing newer periods. Daily values and prices respect the 22:00 cutoff; daily revisions resolve to one row per security/date.
- Projection refresh deletes only within the requested date range. Empty scoped refreshes remove stale rows inside the range while retaining rows outside it. Reversed date ranges raise before writes.
- Harness checks both source layers contain identical facts, requires every mapped factor and at least 20 securities in raw comparisons, and requires equal security cohorts on both sides for z-score comparisons. Asset-growth inputs have actual cross-sectional variation rather than proportional assets whose variance is floating-point noise.
- Known gaps are pytest collection-time skips with per-module reasons, avoiding expensive fixture builds for cases that cannot establish parity. The availability assertion checks the recorded metric availability as well as the rebalance date.
- `derived_factor_projection` was already in the committed public API snapshot from allowed prior snapshot bleed. This task does not need to add it again.

## Verification

Interpreter: `C:/atx/atx-db/.venv/Scripts/python.exe`; commands run from `C:/atx/atx-db`.

1. Required inherited-state check: `python -m pytest tests/test_derived_parity.py -n 0 -q -ra` completed with exit 0. Of 45 collected cases, 17 passed and 28 explicitly skipped (14 module cases, each skipped for both raw and z-score tests). No unexpected empty-fixture skip occurred.
2. Final changed behavior: all 11 affected parity/availability cases passed, followed by all 4 new projection regression cases passing. Exact commands, transient failures, and measured errors are recorded below.
3. `python -m ruff check src/atx_db/derived_factor_projection.py tests/test_derived_parity.py`: clean.
4. `python -m mypy --strict src/atx_db/derived_factor_projection.py`: clean, one source file.

No full suite was run. Controller owns the common module-boundary/schema gate. The first new-regression invocation overlapped S4 T7's `0307` bootstrap fix; its setup errors are distinguished from the final focused verification below.

## Actual parity evidence and its limits

The five modules below executed real legacy-loader/refresher versus engine/projection comparisons in the inherited-state run. They are not skips. `annual_margin_change` includes all three mapped factor IDs; the other modules have one each. Thus **7 of 24 factor mappings across 5 of 19 module cases have raw-value evidence**, and only **4 of the 18 retirement candidates** are represented (the fifth module, `asset_growth`, is intentionally retained).

| Module | Compared factors | Rows / dates / securities | Max scaled raw error | Max z-score error |
| --- | --- | --- | --- | --- |
| `net_operating_assets` | `quality_net_operating_assets` | 725 / 29 / 25 | 1.11e-16 | 4.15e-13 |
| `annual_margin_change` | annual net-, operating-, and gross-margin change | 2,175 / 29 / 25 | 0 | 0 |
| `beneish_m_score` | `quality_low_beneish_m_score` | 725 / 29 / 25 | 3.52e-16 | 6.27e-12 |
| `rd_intensity` | `valuation_rd_to_market_equity` | 1,025 / 41 / 25 | 0 | 4.67e-15 |
| `asset_growth` (KEPT) | `investment_conservative_asset_growth` | 725 / 29 / 25 | 0 | 0 |

Counts and rounded-up error bounds above come from the strengthened post-fix harness, recorded with pytest `record_property` in `.pytest_cache/t7-reconciliation-focused.xml`. Every listed row also has a matched-cohort z-score comparison. Beneish runs the legacy asset-growth prerequisite; R&D intensity has comparable legacy market-cap inputs.

The fixture has 25 securities, 16 quarters over 2019-2022, and monthly price rows through June 2023. It deliberately puts full-year flows in Q4 and zero in Q1-Q3, while holding balances at the previous annual level until Q4. This makes trailing sums and quarterly balance observations match legacy annual-only inputs. Those are actual numeric comparisons on a constrained input domain, **not evidence that annual-only publication semantics equal normal quarterly/TTM updates**. For example, `rd_intensity` is annual R&D in the legacy module versus TTM R&D in the engine. Similar annual/quarterly update differences affect the other passing cases. Legacy reporting-age, input-completeness, and magnitude filters are also not reproduced by the generic seed.

The raw comparator follows the brief's supplied expression: `abs(engine - module) / max(1, abs(module)) <= 1e-9`. That is an absolute tolerance below unit magnitude and a relative tolerance above it, rather than pure relative tolerance. Z-score absolute tolerance is `1e-9`. Comparisons cover overlapping rows, with all mapped factors required; they do not assert complete equality of emitted row sets.

## Explicit skip list and retirement blockers

Each row below accounts for two explicit skips: raw and z-score. No skipped case is eligible retirement evidence. "Definition" records an observed formula difference; except for the separately ruled RSST correction, it does **not** mean permission was granted to redefine a published factor.

| Module | Classification | Specific blocker / next closure work |
| --- | --- | --- |
| `altman_distress` | Fixture/harness gap | Populate `fundamental_ttm_points` for legacy EBIT/revenue, required balance components, and cash-flow-profitability lineage. Then compare the matching Altman formulas and market-cap inputs. |
| `quarterly_working_capital_accruals` | Fixture gap + definition mismatch + projection limitation | Missing quarterly-cash-profitability lineage. Legacy uses quarterly changes in receivables/inventory/deferred revenue/AP divided by lagged assets; engine uses year-over-year change of broad operating working capital divided by average assets. Legacy keeps un-oriented `raw_value` and negates only the z-score input; the projection's single orientation cannot preserve both columns. Requires a compatible metric and separate raw/value orientation support, not only wider fixtures. |
| `asset_turnover_change` | Definition mismatch | Legacy annual revenue divided by lagged annual assets, then differenced; engine revenue TTM divided by average current/year-ago assets, then differenced. Requires a compatibility metric or an explicit published-definition change. |
| `quarterly_gross_margin_change` | Fixture gap + definition mismatch | Missing quarterly revenue/margin parent chain. Legacy compares same-quarter margins; engine compares TTM gross margins. Populate parent chain and use a quarterly compatibility metric. |
| `quarterly_profitability_change` | Fixture gap + definition mismatch | Missing quarterly-operating-profitability parent rows. Legacy numerator is quarterly revenue minus COGS minus SG&A plus R&D, divided by one-quarter-lagged assets; engine uses TTM operating income over average current/year-ago assets. Requires compatible numerator, horizon, and denominator. |
| `net_issuance` | Definition mismatch | Legacy is negative log change in split-adjusted shares; engine is negative arithmetic year-over-year share growth. Requires log/split-compatible metric. |
| `net_payout` | Fixture/harness gap | Populate TTM common dividends, repurchases, and issuance for the same accession plus comparable market-cap inputs. Sign and missing-input conventions then need raw comparisons. |
| `enterprise_yield` | Fixture/harness gap (4 factor IDs) | Populate legacy `enterprise_value`, TTM inputs, and gross-/cash-flow-profitability parent lineage. All four EBIT/gross-profit/CFO/sales yield variants need separate nonempty evidence. |
| `rsst_accruals` | Intentional engine-definition difference from S3 T4 ruling | Legacy is `-delta(NOA)/average_assets`; corrected engine also includes changes in long-term investments minus debt. Preserve the existing factor through a compatibility metric or obtain an explicit factor-definition migration ruling. Equalizing investment/debt changes in a fixture would conceal the difference. |
| `external_financing` | Definition mismatch | Legacy aggregate financing cash flow includes dividends and divides by prior annual assets; engine nets equity/debt issuance without dividends and divides by average assets. Requires compatible cash-flow aggregate and denominator. |
| `net_debt_financing` | Definition mismatch | Legacy signed issuance/repayment divides by lagged annual assets; engine divides by average assets. Match denominator and repayment sign convention. |
| `rd_increase` | Definition mismatch | Legacy binary large-R&D-increase event versus engine continuous R&D growth ratio. Requires legacy event thresholds/indicator logic. |
| `tax_expense_momentum` | Definition mismatch | Legacy same-quarter tax-expense change scaled by lagged assets versus engine TTM tax growth scaled by prior tax. Requires matching horizon and denominator. |
| `tax_to_book_income` | Definition mismatch | Legacy current-tax/statutory-rate estimate of after-tax taxable income over book income versus engine total tax expense/net income. Requires current-tax components and the historical statutory-rate transform. |

No engine/catalog definition files were changed to force these comparisons to pass. General compatibility of the three fixture-only families has not yet been measured; the absence of an identified formula mismatch is not proof of parity.

## Retirement decision

**Do not retire all 18 modules from this task's results.** Four candidate modules have measured raw evidence only on the annual-aligned fixture; fourteen have no raw parity evidence. For the four passing candidates, ordinary quarterly updates, row eligibility/completeness, reporting-age filters, and full output coverage still require review before deletion. The generic projection also is not wired into a scheduled/activation replacement path by this task; T8 must preserve actual publication of each retired factor.

The handoff explicitly permits landing T7 with per-module skips. That supersedes the original brief's instruction to widen fixtures whenever more than three raw comparisons skip, but it does not relax T8's evidence gate. No legacy module was deleted, renamed, or modified.

## Recommended T8 task boundary

Keep conventional core definitions (`asset_turnover`, `rsst_accruals`, TTM margins, share growth, etc.) unchanged. Split compatibility work into these bounded responsibilities:

1. **Shared engine/projection compatibility support:** explicitly select annual, quarter, or TTM inputs and matching fiscal lags; preserve legacy reporting-age/completeness/magnitude eligibility; support separate raw-value and standardized-value orientation; retain the legacy factor's monthly grid and market-value input conventions. These are reusable timing/output rules, not changes to conventional financial metrics. Add only input provenance/period-selection support needed by the 18 named modules; industry templates remain deferred.
2. **Factor-specific legacy expressions:** add distinctly named compatibility definitions (for example, `legacy_asset_turnover_change`) or equivalent dedicated projection expressions, preserving the published factor IDs while leaving conventional metric codes intact. This layer owns lagged-versus-average denominators, log/split-adjusted issuance, the financing cash-flow aggregate, R&D event thresholds, current-tax/statutory-rate treatment, quarterly profit components, and the legacy RSST formula. First audit which required current-tax, financing, or corporate-action inputs already exist; missing inputs are explicit closure tasks, not zero imputation. The three fixture-only families still need all dependencies populated before deciding that a new expression is unnecessary.
3. **Evidence and deletion:** build a shared realistic fixture with independently varying quarterly flows/balances, amendments and out-of-order filings, sparse/missing/stale inputs, share splits, and changing universe cohorts. Populate legacy TTM/market-cap/EV/parent-factor dependencies from those same underlying facts. Compare every one of the 23 retirement factor IDs for raw value, z-score, complete output keys/eligibility, and availability. Freeze legacy-produced expected outputs plus their input fixtures before deletion so the retirement tests remain independent of the engine under test and no longer import deleted modules. Wire projection publication, inspect inbound imports/callers, then delete only modules whose full compatibility evidence passes.

The generic support, compatibility expressions, and evidence/retirement integration can be separate ownership units. None requires an industry-template expansion or approval to silently change the meaning of an existing factor ID. This report recommends that boundary only; T7 does not implement it.

## Final focused verification details

- First four-case regression invocation: 4 setup errors in the then-uncommitted S4 T7 `bodies_0307.py`, `DependencyException` on `api_schema_coverage_slo` during schema initialization. No T7 regression body executed. S4 T7 fixed its migration independently; no migration file was changed here.
- After the controller's stable-source signal: `python -m pytest tests/test_derived_parity.py -n 0 -q -k 'net_operating_assets or annual_margin_change or asset_growth or beneish_m_score or rd_intensity or selects_latest_known_period or daily_rows_respect_cutoff or scoped_refresh_preserves_other_dates or available_at' --junitxml=.pytest_cache/t7-reconciliation-focused.xml -o junit_family=legacy`: **11 passed, 4 setup errors**, 34 deselected, 163.264 seconds. All five raw and five z-score cases and the stronger availability check passed. The four new tests exposed missing required `reason`/`rules_json` fields in their small universe fixture; those fields were then added.
- Reran only those four failed/new concerns: `python -m pytest tests/test_derived_parity.py -n 0 -q -k 'selects_latest_known_period or daily_rows_respect_cutoff or scoped_refresh_preserves_other_dates' --junitxml=.pytest_cache/t7-projection-regressions.xml`: **4 passed**, 45 deselected, exit 0. These cover historical/superseded quarterly revisions, late old-period amendments, daily cutoff/revision selection including a late price, and nonempty/empty date-scoped refresh preservation.
- Ruff remained clean on both touched Python files; strict mypy remained clean on the new source module. The raw/z-score cases were not rerun after the isolated required-column fixture fix, because that fixture is only used by the four regression tests.

The final file has 49 tests: the original 45 plus four regressions. Verification is intentionally the initial full focused file plus targeted reruns, per the speed ruling; no second all-file run was performed. The 28 known skips remain explicit and uncounted as parity success.
