# S3 T8 implementation report

## Result and retirement gate

Retired all **18 planned per-metric modules**, **14 build wrappers**, and **18 superseded module tests** only after genuine baseline parity passed. The replacement publishes **23 retirement factor IDs**. The twenty-fourth seed row, retained `investment_conservative_asset_growth`, remains available through explicit `factor_ids` opt-in; the default refresh excludes it and the retained annual asset-growth refresher remains its production publisher and parent. The brief's claim that all 24 IDs came from deleted modules was incorrect.

The final post-deletion harness compares all **46,338 frozen legacy rows**, **23 factors**, **28 securities**, and **89 distinct dates** using a full outer join on factor/security/date. No factor, raw-value case, missing key, or extra key is skipped. Each factor covers all 28 securities; every emitted baseline cohort has at least 24 names. Maximum scaled raw error is **3.445315762960032e-16**, maximum absolute z-score error **1.4547807403175739e-12**, below the required 1e-9 tolerances. Exact raw values match for 22 factors; the remaining Beneish difference is floating-point evaluation order.

Conventional derived definitions and their seeds are unchanged, including S3 T4's corrected RSST definition. The separate `legacy_rsst_accruals` expression preserves the published old NOA-only change definition without cancelling investment/debt differences in the data.

## Implementation and owned paths

- `derived_compatibility.py` defines reusable annual/quarter/instant/TTM input relations, accession pivots, current/nearest-year/consecutive fiscal selection, input-age and denominator eligibility, share/split transformations, and raw-input cap/EV compatibility adapters. A single selection pipeline uses the existing restricted `derived_dsl.compile_expression` arithmetic compiler.
- `derived_compatibility_catalog.py` declares 23 distinctly named `legacy_*` formulas and their selectors. It does not import retired implementations or duplicate their loader/refresher classes. Conventional metric codes retain their meanings.
- `derived_factor_projection.py` keeps its public refresh signature and owns the existing monthly projection, winsorization and z-score path. Raw orientation and standardized orientation are separate: quarterly WCA retains positive raw accruals and negative standardized orientation. The seed records the actual legacy net-payout 2.5% winsorization, other selected factors' 1% limits, and exact legacy source ownership.
- `derived_compatibility_parents.py` exposes typed `refresh_compatibility_parents(store, options)`. It refreshes only selected retained parents: asset growth, fundamental signals, cash-flow profitability, quarterly operating/cash profitability and quarterly revenue growth. QOP starts 600 calendar days before a scoped start to satisfy prior-year lineage; final projections and other parents retain the requested scope. Results report row counts per parent, selected retirement-factor count, history start, and `empty_parents`. Zero counts are explicit diagnostics; coverage policy belongs to the caller. No cohort is fabricated.
- The activation/jobs owner must build the legacy liquid cohort separately, call the parent helper, then projection. An explicit RSST selection also refreshes its projected NOA prerequisite in the same requested date scope; the parent helper's selected-factor count describes the requested retirement mappings. This task edits no jobs, activation, CLI, API, publication, measurement, universe, delisting, industry-template or LEI/FIGI behavior.
- Neither compatibility SQL nor its parent helper reads or refreshes deprecated `market_cap`, `enterprise_value` or `valuation_multiples` tables. Cap/EV compatibility denominators use current raw bars/shares and statement/fact-revision inputs with original selection conventions. The final fixture leaves old cap and EV tables empty and asserts they stay empty.
- Migration **0309**, registered under the controller's exclusive lock together with its body, repoints existing declarations without changing other metadata, and declares the four previously missing annual asset-turnover/margin factors using their original meanings. Tests cover all 23 declarations, idempotence, and preservation of existing/unrelated metadata.

## Frozen evidence and adversarial domain

Independent fixture/evidence commit: `a4fcc0559140fe42a1172937426e4fb5fab863f6`. Genuine baseline source: `5b11a272c90e9d51cf6fcad3cb3ac0ed6493f9c3`. The generator runs the unchanged legacy source export, verifies its named Git blobs, records 314 executed source/seed hashes and actual options, and invokes genuine retained parents and legacy cap/EV refreshers. Annual margin uses its genuine loader/computer because it never had a refresher. Ordinary tests load frozen outputs and do not need removed modules.

- Base-source input SHA256: `ee60a447941ebfbb28f2b04c2b40497300eddbeb9becd0d0123831b4924d2bc9`.
- Frozen output SHA256: `b65f4511dfb96e30b1c4aedcfaedacb2f9556a3ec96c3f5194e44f61d718bea2`.
- `tests/data/derived_retirement_inputs.json.gz`: 5,163,840 bytes, including genuine legacy denominators for audit.
- `tests/data/derived_retirement_expected.json.gz`: 657,470 bytes, complete unique emitted keys, raw/z values and original timestamps.

The shared inputs comprise 56,024 facts in each identical standardized/statement layer, 14,496 genuine four-quarter TTM records, 880 share observations, 2,688 bars and 28 membership windows. Independently varying quarterly flows and balances span 2016-2023; annual records are actual four-quarter sums, not equalized quarter/annual data. Cases include six splits, old-period amendments with revised Q4/annual/TTM vintages, missing components, reporting stops/staleness, membership entry/exit, zero assets/revenue, negative income/zero tax, low cap/ADV and heterogeneous price availability. Both historical tax rates and both binary R&D outcomes are asserted. No expected-output values were adjusted to create agreement.

## Availability and consumer continuity

Legacy z-scores contain 44,612 rows timestamped before their latest eligible peer; those original timestamps remain frozen verbatim. Per the controller's explicit T7-review alternative, compatibility outputs publish conservatively at the common **22:00 decision cutoff**. Own selected input times, selected IDs/vintages and source parent lineage remain in `input_lineage_json`, separate from publication time. The output must be at least as late as every selected own/parent/peer input and no later than the decision cutoff. A focused heterogeneous parent/child cohort case verifies this, and a loader test rejects inputs arriving after 22:00. Generic quarterly/daily projections retain their existing actual-cohort-maximum timing.

The reverse-consumer audit found a real Piotroski filter tied to the old net-issuance source. With controller-approved scope, `piotroski.py` now accepts official projection and legacy issuance, filters visibility **before** ranking, then selects newest visible availability with deterministic projection priority for a tie. Its regression verifies a 23:00 projection cannot hide an eligible 22:00 legacy observation, and a same-time projection wins on the next date. The book-side decision input uses the same 22:00 cutoff. Piotroski consumes raw values, so no old lineage shape is assumed.

`factor_panel.py` filters latest revisions and deduplicates by availability/run; conditional router reads governed values and metadata without a legacy-source or old-lineage requirement. `signal_eval.load_panel_for_eval` reads all latest sources and performs a membership ranking before factor ranking, so leaving official legacy and projection rows simultaneously latest could yield a stale choice. The canonical projection refresh therefore marks **only exact known legacy source/factor/date-scope rows** non-latest, in the same transaction as replacement publication. Physical historical rows remain. Empty refreshes also supersede newly ineligible legacy rows; outside-scope dates and unrelated sources remain unchanged. Experimental `options.source` refreshes do **not** supersede the official legacy publisher. Three focused regressions prove these contracts. No blanket source deletion or out-of-scope consumer rewrite was introduced.

The pre-deletion AST scan found 36 imports, all within the 50 removal paths or the mutually deleted RSST/NOA pair. The post-deletion scan covered 737 Python files and found **zero** imports of the 18 removed modules. Factor/source literal review found no additional retained legacy-lineage dependency among these 23 factors. Historical migration string literals remain valid and unchanged.

## Verification and failure-driven corrections

All pytest commands used `.venv/Scripts/python.exe`, `-n 0`, temporary isolated schema copies and the controller's serial DB slot. No full suite, live database, network/LLM API, secrets/authentication or user contact transmission was used.

1. Initial bounded compatibility package (`tests/test_derived_parity.py tests/test_derived_compatibility.py`): 16 passed, 1 failed. All complete keys and raw values already matched. The only mismatch was net-payout standardized values; the original module's actual 2.5% winsorization exposed the prior projection seed's incorrect 1%. Corrected the seed from legacy evidence, then reran only the failed complete parity test: **1 passed**.
2. SQL binding for all 23 compatibility expressions, retained mapping explicit-opt-in/default exclusion, scoped parent history/empty diagnostics, generic quarterly/daily cohort timing, after-cutoff rejection, and migration metadata preservation passed in the focused package. Frozen-source provenance passed separately.
3. Consumer handoff package: Piotroski case passed; three supersession test fixtures initially referenced a nonexistent `updated_at` field. Fixed the fixture to explicit real columns, reran only those failures: **3 passed**. No production change was needed for that test-fixture error.
4. After all 50 removals, `tests/test_derived_parity.py tests/test_module_boundaries.py tests/test_import.py` filtered to complete parity/PIT, 18 module guards, wrapper/fresh-registry guards and all import/boundary checks: **32 passed**, one transient public-snapshot mismatch due concurrently introduced AR6 `ticker_history_quality`. Its owner added that single entry; reran only the failed snapshot test: **1 passed**. The controller permits this isolated snapshot-line overlap; AR6 owns its source implementation.
5. Ruff passed all 11 touched Python paths (compiler/catalog/parents/projection/Piotroski/body/registration/tests). Strict mypy passed all five new/extended compiler/catalog/parent/projection/migration source files. `git diff --check` passed.

Local JUnit evidence: `.pytest_cache/s3-t8-compatibility.xml`, `s3-t8-final-parity.xml`, `s3-t8-consumer-handoff.xml`, `s3-t8-supersession.xml`, `s3-t8-post-retirement.xml`, `s3-t8-boundary-final.xml`. The final post-retirement XML records the following full per-factor evidence. Independent review remains a controller integration gate after this implementation commit.

| Factor ID | Rows | Dates | Max scaled raw error | Max absolute z error |
| --- | ---: | ---: | ---: | ---: |
| `distress_altman_z_score` | 2,131 | 80 | 0 | 0 |
| `earnings_tax_expense_momentum` | 1,896 | 71 | 0 | 8.882e-16 |
| `earnings_tax_to_book_income` | 1,845 | 71 | 0 | 1.688e-14 |
| `efficiency_annual_asset_turnover_change` | 1,586 | 59 | 0 | 0 |
| `financing_low_external_financing` | 1,911 | 71 | 0 | 3.553e-15 |
| `financing_low_net_debt_financing` | 1,911 | 71 | 0 | 2.54e-13 |
| `financing_low_net_share_issuance` | 2,193 | 80 | 0 | 0 |
| `financing_net_payout_yield` | 2,223 | 83 | 0 | 0 |
| `intangibles_large_rd_increase` | 1,586 | 59 | 0 | 2.22e-16 |
| `profitability_annual_gross_margin_change` | 1,911 | 71 | 0 | 0 |
| `profitability_annual_net_margin_change` | 1,911 | 71 | 0 | 0 |
| `profitability_annual_operating_margin_change` | 1,911 | 71 | 0 | 0 |
| `profitability_quarterly_gross_margin_change_yoy` | 2,039 | 77 | 0 | 0 |
| `profitability_quarterly_operating_profitability_change_yoy` | 2,039 | 77 | 0 | 0 |
| `quality_low_beneish_m_score` | 1,911 | 71 | 3.445e-16 | 1.455e-12 |
| `quality_low_quarterly_operating_working_capital_accruals` | 2,382 | 89 | 0 | 0 |
| `quality_low_rsst_accruals` | 1,911 | 71 | 0 | 4.441e-15 |
| `quality_net_operating_assets` | 1,911 | 71 | 0 | 0 |
| `valuation_enterprise_yield_ebit` | 2,235 | 83 | 0 | 0 |
| `valuation_enterprise_yield_sales` | 2,235 | 83 | 0 | 0 |
| `valuation_gross_profit_enterprise_yield` | 2,277 | 83 | 0 | 3.775e-15 |
| `valuation_operating_cash_flow_enterprise_yield` | 2,148 | 80 | 0 | 2.665e-15 |
| `valuation_rd_to_market_equity` | 2,235 | 83 | 0 | 8.882e-16 |
