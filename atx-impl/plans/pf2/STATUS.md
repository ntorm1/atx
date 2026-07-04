# PF2 Status - Fundamentals Depth + Production Warehouse

Updated: 2026-07-04, America/New_York
Branch: `feat/warehouse-parity`

PF2 is in progress. PF2-S1 (schema-as-contract) is **complete and merged** into
`feat/warehouse-parity` as merge commit `1776fb0`. PF2-S2 is implemented and
committed in `C:/atx-wt/pf2-s2` as `eba1df9ffa00620b9bab7f16baf5e14bb42bdf41`,
awaiting merge.

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
  as `eba1df9ffa00620b9bab7f16baf5e14bb42bdf41`. Migrations consumed: `0100-0101`
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

## Known S1 Caveats

- PIT-column-presence intentionally exposes known live PIT gaps on pre-existing fact tables, mostly
  missing `is_latest_revision`; ratchet/backfill/exemption belongs to S10 quality gating or future cleanup.
- PF2-S2 owns checksum enforcement, migration apply-lock, backup/checkpoint/restore governance.
  The code path is implemented; operator live-DB proof remains pending.
- PF2-S10 still owns orchestrator halt-on-critical behavior.

## Resume Point

1. Merge `feat/pf2-s2` into local `main`/`feat/warehouse-parity`, and remove or retire the
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
