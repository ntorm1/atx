# PF3 SDD Progress

Controller: Codex
Worktree: `C:\atx-wt\pf3-s10`
Branch: `feat/pf3-s10`
Updated: 2026-07-05 America/New_York

## Landed Before This Worktree

- PF3-S1 backfill + incremental-maintenance DAG: merged to `main`.
- PF3-S2 schema-contract v2: merged to `main`.
- PF3-S3 architecture decomposition: merged to `main`.
- PF3-S4 PIT universe + historical price backfill client: merged to `main`.
- PF3-S5 valuation input completeness: merged to `main` at `6a33e86`.
- PF3-S6 ratio & metric engine v2: completed on `feat/pf3-s6` through `29d2cc1`. This S7 worktree is stacked on `feat/pf3-s6` because the primary tree is currently on an unrelated dirty branch and the helper finish merge could not be run safely from it.
- PF3-S7 factor framework + PIT engine: completed on `feat/pf3-s7` through `f8aa3fa`. The S8 worktree was stacked on `feat/pf3-s7` for the same primary-tree isolation reason.
- PF3-S8 fundamental factor families: completed on `feat/pf3-s8` through `1bcbe22`.
- PF3-S9 cross-domain factors + unified namespace: completed on `feat/pf3-s9` through `840b4a8`. This S10 worktree is stacked on `feat/pf3-s9` while the primary tree remains on an unrelated dirty branch.

## PF3-S6 - Ratio & Metric Engine V2

- S6-0 - Complete the ratio catalog: done. Worker `019f3425-5b30-7823-8a37-0319a15b32cf` expanded the registry CSV and ratio tests; worker `019f342d-ff60-7941-a193-0b3eb7621f5b` added migration 0148 and seed-loader durability; reviewer `019f3449-2f39-7fa1-b8a9-b46254ecdf35` found the valuation routing gap; worker `019f3451-505e-7c31-8b03-9645f344e90a` wired the new valuation formulas through the valuation-multiples path.
- S6-1 - Growth / CAGR engine: done. Explorer `019f3462-6dfe-7821-892d-1050235fff61` mapped the least-invasive input and migration path; controller implemented the `fundamental_growth` metric engine, migration 0149, growth formula specs, job registration, and focused test coverage.
- S6-2 - Metric engine + queryable lineage: done. Controller added migration 0150 with `v_metric_lineage`, spanning `fundamental_ratios`, `fundamental_growth`, and `valuation_multiples`, plus `metric_lineage_asof` gating on metric availability and formula definition validity.
- S6-3 - Reconciliation gate + catalog: done. Controller added a pure metric-frame reconciliation helper, a `metric_lineage_completeness` gate over `v_metric_lineage`, migration 0151 with `v_metric_catalog`, metric lookup indexes, and `quality_check_registry` rows for lineage completeness and ratio reconciliation.

## PF3-S7 - Factor Framework + PIT Engine

- S7-0 - Factor catalog: done. Controller added the `db.factors` package with a pure legacy-catalog reconciler, migration 0152 with `factor_definition`, and focused tests for legacy feature/alpha attribution plus undeclared-input validation.
- S7-1 - Factor dependency DAG + PIT compute engine: done. Controller added `db.factors.engine` with typed dependency-edge generation, topological ordering/cycle rejection, a minimal PIT-safe expression materializer with `available_at = max(inputs)`, migration 0153 for `factor_dependency_edges`/`factor_build_manifests`, and tests covering seeded legacy edges plus fixture compute.
- S7-2 - Cross-sectional operators: done. Controller added pure `rank`, `zscore`, and `winsorize` operators in `db.factors.cross_section`, migration 0154 with `factor_operator` metadata, and tests proving full-frame output matches per-date isolated computation.
- S7-3 - Neutralization + leakage/lookahead gate: done. Controller added pure neutralization residuals, `pit_safety_report` for future-input and cross-date-pooling detection, migration 0155 indexes plus `v_factor_engine_catalog`, and a critical `factor_operator_pit_safety` registry row.

## PF3-S8 - Fundamental Factor Families

- S8-0 - Core academic families: done. Controller added `db/seeds/factor_definitions.csv`, `db.factors.fundamental_families`, migration 0156 seeding value/quality/profitability rows into `factor_definition`, and tests covering Novy-Marx gross profitability hand reconciliation, PIT availability, lineage, missing-leg withholding, and catalog round-trip.
- S8-1 - Growth / investment / leverage + named composites: done. Controller extended the staged seed with growth, investment, leverage, Piotroski F-score, Altman Z-score, and Sloan accrual rows; migration 0157 seeds those definitions/edges; compute tests reconcile the named composites and verify missing-leg withholding.
- S8-2 - Signal-native factors: done. Controller added signal-native seed rows and pure compute for PIT revisions momentum, standardization-delta anomaly, segment revenue concentration, and footnote disclosure change, including a planted future-restatement fixture proving revisions momentum ignores unavailable future vintages.
- S8-3 - Family panels + lineage + gated checks: done. Controller added the `fundamental_factor_values` panel table, `v_fundamental_factor_family_catalog`, lineage-completeness and family-coverage gate helpers, and critical `quality_check_registry` rows with green/red planted fixture tests.

## PF3-S9 - Cross-Domain Factors + Unified Namespace

- S9-0 - Price/liquidity factor integration: done. Controller added `db.factors.cross_domain` with pure `equity_price_metrics` mappers for momentum, volatility, liquidity, beta, idiosyncratic vol, distance-from-high, and drawdown factors; native price-surface ranks are carried as diagnostics and cross-checked against S7 percent-rank output. Migration 0160 seeds price/liquidity definitions and source dependency edges into the S7 catalog plus `v_cross_domain_price_liquidity_factor_catalog`.
- S9-1 - Estimate-revision + 13F-flow factors: done. Controller extended `db.factors.cross_domain` with pure mappers for `est_surprise` SUE/surprise percent, `est_consensus` revision mean/breadth, and `thirteenf_concentration_metrics` crowding/flow factors. Migration 0161 seeds the estimate and 13F definitions/edges plus `v_cross_domain_estimate_13f_factor_catalog`; tests cover consensus-revision lineage and a 13F filing-lag fixture that emits no row before `available_at`.
- S9-2 - Short-interest + insider factors: done. Controller added mappers for `short_interest_metrics` crowding/flow/pressure factors and `insider_transaction_metrics` net-buy, cluster-buy, and plan-sale factors. Migration 0162 seeds the short-interest/insider definitions/edges plus `v_cross_domain_short_insider_factor_catalog`; tests cover FINRA publication-lag filtering, bearish short-pressure rank direction, insider symbol propagation, and plan-sale direction.
- S9-3 - Unified namespace assembly + consistency gates: done. Controller added `compute_cross_domain_factor_rows`, `cross_domain_namespace_consistency`, the `cross_domain_factor_values` table, `v_cross_domain_factor_catalog`, and the critical `cross_domain_factor_namespace_consistency` quality-check registration. Tests cover five-domain assembly, planted duplicate/missing/collision failures, and catalog/gate registry rows.

## PF3-S10 - Backtest Export Panel

- S10-0 - PIT factor-panel views: done. Controller added `db.factor_panel` with pure long-panel assembly and wide pivot helpers, plus migration 0164 with `v_factor_panel` and `v_factor_panel_wide`. The panel uses decision dates (`max(source as_of_date, available_at date)`), applies PF3-S4 universe membership as-of, and catalogs both long and wide surfaces. Focused tests cover long/wide consistency, non-member exclusion, and future-availability rows moving to their first safe decision date.
- S10-1 - Parquet lake export registration: done. Controller added migration 0165 registering `v_factor_panel` as a date-partitioned lake object with `available_at` watermark metadata and a persisted expected schema hash, extended `LakehouseExporter` to enforce optional schema contracts before writing, and taught `validate_lake_export` to validate partitioned export directories from their manifests. `db.factor_panel.export_factor_panel` is a thin governed export entry point over the existing lake exporter.

## Verification

- Baseline `python -m pytest db\tests\test_formula_library.py -q -n0` passed before S6-0 edits.
- `python -m pytest db\tests\test_formula_library.py -q -n0` passed after S6-0.
- `python -m pytest db\tests\test_fundamental_ratios.py -q -n0` passed.
- `python -m pytest db\tests\test_valuation_multiples.py -q -n0` passed.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed.
- `python -m pytest db\tests\test_metric_engine.py -q -n0` passed.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed.
- `python -m pytest db\tests\test_jobs_dag.py -q -n0` passed.
- `python -m pytest db\tests\test_metric_engine.py db\tests\test_module_boundaries.py -q -n0` passed.
- `python -m pytest db\tests\test_formula_library.py db\tests\test_fundamental_ratios.py -q -n0` passed.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed.
- `python -m pytest db\tests\test_metric_engine.py -q -n0` passed after S6-2 lineage-view additions.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S6-2.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S6-2.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S6-2.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S6-2.
- `git diff --check` passed with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\metric_engine.py db\migrations\bodies_0148_0151.py` passed after S6-3.
- `python -m pytest db\tests\test_metric_engine.py -q -n0` passed after S6-3.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S6-3.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S6-3.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S6-3.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S6-3.
- `git diff --check` passed after S6-3 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\__init__.py db\factors\catalog.py db\migrations\bodies_0152_0155.py db\migrations\registry.py db\migrations\__init__.py` passed after S7-0.
- `python -m pytest db\tests\test_factor_engine.py -q -n0` passed after S7-0.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S7-0.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S7-0.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S7-0.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S7-0.
- `git diff --check` passed after S7-0 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\engine.py db\migrations\bodies_0152_0155.py db\tests\test_factor_engine.py` passed after S7-1.
- `python -m pytest db\tests\test_factor_engine.py -q -n0` passed after S7-1.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S7-1.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S7-1.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S7-1.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S7-1.
- `git diff --check` passed after S7-1 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\cross_section.py db\migrations\bodies_0152_0155.py db\tests\test_factor_engine.py` passed after S7-2.
- `python -m pytest db\tests\test_factor_engine.py -q -n0` passed after S7-2.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S7-2.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S7-2.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S7-2.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S7-2.
- `git diff --check` passed after S7-2 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\cross_section.py db\migrations\bodies_0152_0155.py db\tests\test_factor_engine.py` passed after S7-3.
- `python -m pytest db\tests\test_factor_engine.py -q -n0` passed after S7-3 (rerun with a larger timeout after the first run hit the local schema-template/import-time cap).
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S7-3.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S7-3.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S7-3.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S7-3.
- `git diff --check` passed after S7-3 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\fundamental_families.py db\migrations\bodies_0156_0159.py db\tests\test_fundamental_factors.py` passed after S8-0.
- `python -m pytest db\tests\test_fundamental_factors.py -q -n0` passed after S8-0.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S8-0.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S8-0.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S8-0.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S8-0.
- `git diff --check` passed after S8-0 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\fundamental_families.py db\migrations\bodies_0156_0159.py db\tests\test_fundamental_factors.py` passed after S8-1.
- `python -m pytest db\tests\test_fundamental_factors.py -q -n0` passed after S8-1.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S8-1.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S8-1.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S8-1.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S8-1.
- `git diff --check` passed after S8-1 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\fundamental_families.py db\migrations\bodies_0156_0159.py db\tests\test_fundamental_factors.py` passed after S8-2.
- `python -m pytest db\tests\test_fundamental_factors.py -q -n0` passed after S8-2.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S8-2.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S8-2.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S8-2.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S8-2.
- `git diff --check` passed after S8-2 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\fundamental_families.py db\migrations\bodies_0156_0159.py db\tests\test_fundamental_factors.py` passed after S8-3.
- `python -m pytest db\tests\test_fundamental_factors.py -q -n0` passed after S8-3.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S8-3.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S8-3.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S8-3.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S8-3.
- `git diff --check` passed after S8-3 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\cross_domain.py db\migrations\bodies_0160_0163.py db\tests\test_cross_domain_factors.py` passed after S9-0.
- `python -m pytest db\tests\test_cross_domain_factors.py -q -n0` passed after S9-0.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S9-0.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S9-0.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S9-0.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S9-0.
- `git diff --check` passed after S9-0 with Git LF/CRLF normalization warnings only.
- `python -m py_compile db\factors\cross_domain.py db\migrations\bodies_0160_0163.py db\tests\test_cross_domain_factors.py` passed after S9-1.
- `python -m pytest db\tests\test_cross_domain_factors.py -q -n0` passed after S9-1.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S9-1.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S9-1.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S9-1.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S9-1.
- `python -m py_compile db\factors\cross_domain.py db\migrations\bodies_0160_0163.py db\tests\test_cross_domain_factors.py` passed after S9-2.
- `python -m pytest db\tests\test_cross_domain_factors.py -q -n0` passed after S9-2.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S9-2.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S9-2.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S9-2.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S9-2.
- `python -m py_compile db\factors\cross_domain.py db\migrations\bodies_0160_0163.py db\tests\test_cross_domain_factors.py` passed after S9-3.
- `python -m pytest db\tests\test_cross_domain_factors.py -q -n0` passed after S9-3.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S9-3.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S9-3.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S9-3.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S9-3.
- `python -m py_compile db\factor_panel.py db\migrations\bodies_0164_0167.py db\tests\test_factor_panel.py` passed after S10-0.
- `python -m pytest db\tests\test_factor_panel.py -q -n0` passed after S10-0.
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S10-0.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S10-0.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S10-0.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S10-0.
- `python -m py_compile db\factor_panel.py db\lake.py db\migrations\bodies_0164_0167.py db\tests\test_factor_panel.py` passed after S10-1.
- `python -m pytest db\tests\test_factor_panel.py -q -n0` passed after S10-1.
- `python -m pytest db\tests\test_storage_admin.py -q -n0 -k "partitioned_incremental_lake_export_skips_unchanged_partitions or undeclared_lake_object_keeps_single_file_layout"` passed after S10-1 (rerun with larger timeout after the first run hit the local cap without failure output).
- `python -m pytest db\tests\test_migrations.py -q -n0 -k "migrations_ordered_ascending or migrations_unique_versions"` passed after S10-1.
- `python -m pytest db\tests\test_schema_contract_v2.py -q -n0 -k "version_table_pins"` passed after S10-1.
- `python -m pytest db\tests\test_schema_contract.py -q -n0 -k "complete_manifest or non_contract_fact_table_marks_present_pit_columns or every_pit_marked_table"` passed after S10-1.
- `python -m pytest db\tests\test_module_boundaries.py -q -n0 -k public_api_snapshot_matches_pinned_fixture` passed after S10-1.

## Live Smoke

Not run from the sprint worktree. Per PF3 clause F, any live shared-DB apply/smoke must be operator-run from the primary tree with a backup first.
