# PF2 Status - Fundamentals Depth + Production Warehouse

Updated: 2026-07-04, America/New_York
Integration branch: `feat/warehouse-parity` / local `main`

PF2 is in progress. PF2-S1, PF2-S2, and PF2-S3 are complete and merged into
local `main` / `feat/warehouse-parity`. PF2-S4 is implemented in
`C:/atx-wt/pf2-s4` on branch `feat/pf2-s4` as
`6a20a31a23bebc660a196c10a25f275b56e98882`, awaiting merge.

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

## S4 Implemented Surfaces

- New `db/pit_snapshot.py` pure transform, materializer, dataset wrapper, and
  as-of-month reader for `fundamental_pit_snapshot`.
- New `fundamental_ratios.vintage_class` metadata and opt-in
  `FundamentalRatiosOptions(vintage_mode="history")` path that materializes
  one ratio row per TTM accession vintage.
- New month-end ratio reader `fundamental_ratios_asof_month(...)` plus
  `query_asof.py --view fundamental-ratios-asof-month`.
- New `query_asof.py --view pit-snapshot`.
- Migration `0107` adds `fundamental_pit_snapshot` and the ratio vintage
  column, `0108` adds lookup indexes, and `0109` catalogs the PIT snapshot
  surface and refreshes schema-contract metadata.

## Verification

- `python -m py_compile db\pit_snapshot.py db\fundamental_ratios.py db\migrations.py db\asof.py db\__init__.py scripts\query_asof.py db\tests\test_pit_snapshot.py db\tests\test_fundamental_ratios.py`
- `python -m pytest db\tests\test_pit_snapshot.py -q -n0`
- `python -m pytest db\tests\test_fundamental_ratios.py -q -n0`
- `python -m pytest db\tests\test_migrations.py db\tests\test_migration_governance.py -q -n0`
- `python -m pytest db\tests\test_schema_contract.py db\tests\test_schema_contract_quality_checks.py -q -n0`
- `python -m pytest db\tests\test_quality_smoke.py -q -n0`
- `python -m pytest db\tests -q -n0`

## Live DB Smoke

Operator-pending for S1-S4. No live 14 GB shared-DB migration/apply/rebuild was
run from the worktrees. S4 proof-slice counts for `fundamental_pit_snapshot`,
ratio vintage rows, and as-of-month examples remain pending until an approved
backed-up live run.

## Known Caveats

- S1's PIT-column-presence gate intentionally exposes known pre-existing PIT
  gaps on older fact tables, especially missing `is_latest_revision`; PF2-S10
  owns halt/exemption/backfill policy.
- S4 ratio-vintage history is opt-in and currently lifts TTM accession vintages.
  The default ratio rebuild remains latest-revision mode for compatibility.
- S4 live proof slice is not run; docs do not claim live row counts.
- `0102` remains unused reserved headroom after S2.

## Resume Point

1. Merge `feat/pf2-s4` into local `main` / `feat/warehouse-parity`.
2. Continue ROADMAP sequencing with PF2-S5 (`sprint-5-industry-templates.md`).
3. Track progress in `.superpowers/sdd/progress.md`; append or update sprint
   closeout rows only when the sprint lands.

## Dirty Worktree Notes

Do not run `git add -A`. Preserve unrelated dirty/untracked files unless the
user explicitly asks to clean them up.
