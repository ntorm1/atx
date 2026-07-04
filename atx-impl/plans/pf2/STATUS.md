# PF2 Status - Fundamentals Depth + Production Warehouse

Updated: 2026-07-04, America/New_York
Branch: `feat/warehouse-parity`

PF2 is in progress. PF2-S1 and PF2-S2 are complete and merged into local `main` /
`feat/warehouse-parity`. PF2-S3 is implemented in `C:/atx-wt/pf2-s3` on branch
`feat/pf2-s3` as `8a78117f5f35d1445a6e8aaf83c1a7c6ebf7a263`, awaiting merge.

## Completed

- PF2-S1: schema-as-contract complete on branch `feat/pf2-s1`, base `636e82b`.
  Sprint worktree `C:/atx-wt/pf2-s1` was removed after merge.
- S1 task commits: `f7a83a2`, `a10c443`, `e17972c`, `a276aba`, `5df7d59`,
  `a01b045`, `f38d598`; closeout docs commit `1878f0a`.
- Migrations consumed: `0097-0099`.
- Offline verification after merge from `C:/atx`: `python -m pytest atx-impl/db/tests -q`
  passed to 100% in about 225s.
- Live DB smoke: OPERATOR-PENDING. No live migration/apply/smoke was run from the worktree.
- PF2-S2: migration governance + backup/checkpoint/DR implemented on branch `feat/pf2-s2`
  as `ceec446f2db24aee3bc721ebc136692c98a7b86f`. Migrations consumed: `0100-0101`
  (`0102` remains reserved headroom). The local-main DB testing-speed improvements were
  merged into the worktree before S2 verification.
- S2 implemented surfaces: migration source checksums + checksum verification, persistent
  `migration_apply_lock`, `migration_backup_registry`, new `db/migration_admin.py`
  checkpoint/backup/restore/recovery/retention helpers, and governed
  `scripts/warehouse_migrate.py`.
- S2 focused verification so far: `python -m py_compile db/migration_admin.py db/migrations.py
  scripts/warehouse_migrate.py`; `python -m pytest db/tests/test_migration_governance.py -q -n0`;
  `python -m pytest db/tests/test_migrations.py -q -n0`; temp `warehouse_migrate.py` smoke to
  schema version `0101`; full `python -m pytest db/tests -q -n0` passed.
- S2 live DB smoke: OPERATOR-PENDING. No live 14 GB migration/apply/restore was run from the
  worktree.
- PF2-S3: standardization engine implemented on branch `feat/pf2-s3` as
  `8a78117f5f35d1445a6e8aaf83c1a7c6ebf7a263`. Migrations consumed: `0103-0106`
  (`0102` remains reserved headroom).
- S3 implemented surfaces: `db/standardization.py`, generated
  `db/seeds/standardization_rules.csv` with 300 rules (126 annual, 126 ttm, 48 instant),
  `fundamental_standardized`, `fundamental_standardization_exception`,
  `v_fundamental_standardization_coverage`, standardized-first ratio input pivots with raw
  fallback, and critical quality gates for exception-rate and template coverage.
- S3 focused verification: `python -m py_compile db/standardization.py
  db/fundamental_ratios.py db/quality.py db/migrations.py db/__init__.py`;
  `python -m pytest db/tests/test_standardization.py -q -n0`;
  `python -m pytest db/tests/test_fundamental_ratios.py -q -n0`;
  `python -m pytest db/tests/test_migrations.py -q -n0`;
  `python -m pytest db/tests/test_migration_governance.py -q -n0`;
  `python -m pytest db/tests/test_schema_contract.py db/tests/test_schema_contract_quality_checks.py -q -n0`;
  `python -m pytest db/tests/test_quality_smoke.py -q -n0`.
- S3 full verification: `python -m pytest db/tests -q -n0` passed in `C:/atx-wt/pf2-s3`.
- S3 live DB smoke: OPERATOR-PENDING. No live standardization rebuild/proof-slice was run from the
  worktree.

## Known S1 Caveats

- PIT-column-presence intentionally exposes known live PIT gaps on pre-existing fact tables, mostly
  missing `is_latest_revision`; ratchet/backfill/exemption belongs to S10 quality gating or future cleanup.
- PF2-S2 owns checksum enforcement, migration apply-lock, backup/checkpoint/restore governance.
  The code path is implemented; operator live-DB proof remains pending.
- PF2-S10 still owns orchestrator halt-on-critical behavior.
- PF2-S4 still owns ratio vintage math (`as_first_reported` vs restated); S3 only changes the
  input surface and preserves the existing ratio compute path.
- S3 proof-slice row counts, coverage %, exception count, and run_id remain OPERATOR-PENDING
  until a live standardized rebuild is approved.

## Resume Point

1. Merge `feat/pf2-s3` into local `main`/`feat/warehouse-parity`, and remove or retire the
   worktree after merge.
2. Follow ROADMAP sequencing: S1 -> S2 -> S3 -> S4 -> (S5 -> S6 || S7 || S8) -> S9 -> S10.
   One worktree per sprint; never run two sprints sharing `fundamental_ratios.py` /
   `fundamental_statements.py` in concurrent worktrees.
3. Track progress in `.superpowers/sdd/progress.md`; append/update sprint closeout rows only when the
   sprint actually lands.

## Dirty Worktree Notes

Do not run `git add -A`. The tree carries unrelated dirty/untracked files. Stage only explicit paths
owned by the current sprint task. Preserve unrelated dirty files unless the user explicitly asks to
clean them up.
