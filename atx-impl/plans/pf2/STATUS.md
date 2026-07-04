# PF2 Status - Fundamentals Depth + Production Warehouse

Updated: 2026-07-04, America/New_York
Branch: `feat/warehouse-parity`

PF2 is in progress. PF2-S1 (schema-as-contract) is **complete and merged** into
`feat/warehouse-parity` as merge commit `1776fb0`.

## Completed

- PF2-S1: schema-as-contract complete on branch `feat/pf2-s1`, base `636e82b`.
  Sprint worktree `C:/atx-wt/pf2-s1` was removed after merge.
- S1 task commits: `f7a83a2`, `a10c443`, `e17972c`, `a276aba`, `5df7d59`,
  `a01b045`, `f38d598`; closeout docs commit `1878f0a`.
- Migrations consumed: `0097-0099`.
- Offline verification after merge from `C:/atx`: `python -m pytest atx-impl/db/tests -q`
  passed to 100% in about 225s.
- Live DB smoke: OPERATOR-PENDING. No live migration/apply/smoke was run from the worktree.

## Known S1 Caveats

- PIT-column-presence intentionally exposes known live PIT gaps on pre-existing fact tables, mostly
  missing `is_latest_revision`; ratchet/backfill/exemption belongs to S10 quality gating or future cleanup.
- PF2-S2 still owns checksum enforcement, migration apply-lock, backup/checkpoint/restore governance.
  PF2-S1 did not implement those.
- PF2-S10 still owns orchestrator halt-on-critical behavior.

## Resume Point

1. Start PF2-S2 (migration governance + backup/checkpoint/DR) by creating a `pf2-s2` worktree off
   `feat/warehouse-parity`.
2. Read `atx-impl/plans/pf2/ROADMAP.md` and the PF2-S2 sprint plan.
3. Follow ROADMAP sequencing: S1 -> S2 -> S3 -> S4 -> (S5 -> S6 || S7 || S8) -> S9 -> S10.
   One worktree per sprint; never run two sprints sharing `fundamental_ratios.py` /
   `fundamental_statements.py` in concurrent worktrees.
4. Track progress in `.superpowers/sdd/progress.md`; append/update sprint closeout rows only when the
   sprint actually lands.

## Dirty Worktree Notes

Do not run `git add -A`. The tree carries unrelated dirty/untracked files. Stage only explicit paths
owned by the current sprint task. Preserve unrelated dirty files unless the user explicitly asks to
clean them up.
