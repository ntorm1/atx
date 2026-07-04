# PF2 Status - Fundamentals Depth + Production Warehouse

Updated: 2026-07-04, America/New_York
Integration branch: `feat/warehouse-parity` / local `main`

PF2-S1 through PF2-S5 are implemented. PF2-S5 lives on branch
`feat/pf2-s5` as implementation commit
`90af61f9b03ed5d1ab7bd09c28f913c3357adb9d`.

## Completed

- PF2-S1: schema-as-contract complete on branch `feat/pf2-s1`, base `636e82b`.
  Migrations consumed: `0097-0099`.
- PF2-S2: migration governance + backup/checkpoint/DR complete on branch
  `feat/pf2-s2` as `ceec446f2db24aee3bc721ebc136692c98a7b86f`. Migrations
  consumed: `0100-0101` (`0102` remains reserved headroom). Local-main DB
  testing-speed improvements were merged into the worktree before verification.
- PF2-S3: standardization engine complete on branch `feat/pf2-s3` as
  `8a78117f5f35d1445a6e8aaf83c1a7c6ebf7a263`. Migrations consumed:
  `0103-0106`.
- PF2-S4: PIT month snapshot + ratio-vintage history complete on branch
  `feat/pf2-s4` as `6a20a31a23bebc660a196c10a25f275b56e98882`. Migrations
  consumed: `0107-0109`.
- PF2-S5: industry-specialized templates complete on branch `feat/pf2-s5` as
  `90af61f9b03ed5d1ab7bd09c28f913c3357adb9d`. Migrations consumed:
  `0110-0113`.

## S5 Implemented Surfaces

- New `db/industry_templates.py` with governed `industry_template`,
  `industry_template_item`, `entity_industry_template`, and
  `industry_template_coverage` refreshes.
- Materialized SIC routing for `ALL`, `BK`, `IS`, `UT`, `BD`, and `RT`; statement
  refresh now reads `entity_industry_template` instead of re-deriving an inline
  CASE.
- Utility and broker-dealer statement-map overlays, item-registry rows, and
  concept-map projections. Vendor-only utility rate base and broker-dealer net
  capital are explicit overlay allowlist entries.
- REIT FFO/AFFO derivation rows in `fundamental_statement_points`, with reported
  `nareit:FundsFromOperations` winning when present.
- Industry-family formula-registry rows for bank, broker-dealer, insurance,
  utility, and REIT formulas without changing formula-engine dispatch.
- Critical quality checks for exactly-one template routing and required-item
  coverage.

## Verification

- `python -m py_compile db\tests\test_industry_templates.py db\industry_templates.py db\fundamental_statements.py db\migrations.py db\quality.py db\__init__.py`
- `python -m pytest db\tests\test_industry_templates.py -q -n0`
- `python -m pytest db\tests\test_fundamental_concept_dictionary.py db\tests\test_concept_coverage.py -q -n0`
- `python -m pytest db\tests\test_item_registry.py db\tests\test_formula_library.py -q -n0`
- `python -m pytest db\tests\test_fundamental_ratios.py -q -n0`
- `python -m pytest db\tests\test_quality_smoke.py db\tests\test_schema_contract_quality_checks.py db\tests\test_schema_contract.py -q -n0`
- `python -m pytest db\tests -q -n0`

## Live DB Smoke

Operator-pending for S1-S5. No live 14 GB shared-DB migration/apply/rebuild was
run from the PF2 worktrees. S5 live proof-slice counts for routed templates,
utility/broker-dealer rows, derived REIT FFO/AFFO rows, and critical quality
checks remain pending until an approved backed-up live run.

## Known Caveats

- S1's PIT-column-presence gate intentionally exposes known pre-existing PIT
  gaps on older fact tables, especially missing `is_latest_revision`; PF2-S10
  owns halt/exemption/backfill policy.
- S4 ratio-vintage history is opt-in and currently lifts TTM accession vintages.
  The default ratio rebuild remains latest-revision mode for compatibility.
- S5 industry routing is SIC-derived. Unclassified entities deterministically
  fall back to `ALL`; richer vendor/security-master profile routing is future
  work.
- S5 live proof slice is not run; docs do not claim live row counts.
- `0102` remains unused reserved headroom after S2.

## Resume Point

1. Merge `feat/pf2-s5` into local `main` / `feat/warehouse-parity`.
2. Continue ROADMAP sequencing with PF2-S6 (calendarization/TTM hardening).
3. Track progress in `.superpowers/sdd/progress.md`; append or update sprint
   closeout rows only when the sprint lands.

## Dirty Worktree Notes

Do not run `git add -A`. Preserve unrelated dirty/untracked files unless the
user explicitly asks to clean them up.
