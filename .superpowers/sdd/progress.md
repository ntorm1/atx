# PF3 SDD Progress

Controller: Codex
Worktree: `C:\atx-wt\pf3-s7`
Branch: `feat/pf3-s7`
Updated: 2026-07-05 America/New_York

## Landed Before This Worktree

- PF3-S1 backfill + incremental-maintenance DAG: merged to `main`.
- PF3-S2 schema-contract v2: merged to `main`.
- PF3-S3 architecture decomposition: merged to `main`.
- PF3-S4 PIT universe + historical price backfill client: merged to `main`.
- PF3-S5 valuation input completeness: merged to `main` at `6a33e86`.
- PF3-S6 ratio & metric engine v2: completed on `feat/pf3-s6` through `29d2cc1`. This S7 worktree is stacked on `feat/pf3-s6` because the primary tree is currently on an unrelated dirty branch and the helper finish merge could not be run safely from it.

## PF3-S6 - Ratio & Metric Engine V2

- S6-0 - Complete the ratio catalog: done. Worker `019f3425-5b30-7823-8a37-0319a15b32cf` expanded the registry CSV and ratio tests; worker `019f342d-ff60-7941-a193-0b3eb7621f5b` added migration 0148 and seed-loader durability; reviewer `019f3449-2f39-7fa1-b8a9-b46254ecdf35` found the valuation routing gap; worker `019f3451-505e-7c31-8b03-9645f344e90a` wired the new valuation formulas through the valuation-multiples path.
- S6-1 - Growth / CAGR engine: done. Explorer `019f3462-6dfe-7821-892d-1050235fff61` mapped the least-invasive input and migration path; controller implemented the `fundamental_growth` metric engine, migration 0149, growth formula specs, job registration, and focused test coverage.
- S6-2 - Metric engine + queryable lineage: done. Controller added migration 0150 with `v_metric_lineage`, spanning `fundamental_ratios`, `fundamental_growth`, and `valuation_multiples`, plus `metric_lineage_asof` gating on metric availability and formula definition validity.
- S6-3 - Reconciliation gate + catalog: done. Controller added a pure metric-frame reconciliation helper, a `metric_lineage_completeness` gate over `v_metric_lineage`, migration 0151 with `v_metric_catalog`, metric lookup indexes, and `quality_check_registry` rows for lineage completeness and ratio reconciliation.

## PF3-S7 - Factor Framework + PIT Engine

- S7-0 - Factor catalog: done. Controller added the `db.factors` package with a pure legacy-catalog reconciler, migration 0152 with `factor_definition`, and focused tests for legacy feature/alpha attribution plus undeclared-input validation.
- S7-1 - Factor dependency DAG + PIT compute engine: not started.
- S7-2 - Cross-sectional operators: not started.
- S7-3 - Neutralization + leakage/lookahead gate: not started.

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

## Live Smoke

Not run from the sprint worktree. Per PF3 clause F, any live shared-DB apply/smoke must be operator-run from the primary tree with a backup first.
