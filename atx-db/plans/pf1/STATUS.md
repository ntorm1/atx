# PF1 Status - Fundamentals Parity

Updated: 2026-07-02, America/New_York
Branch: `feat/warehouse-parity`
Current HEAD: `43c0a8d7f3086eaacc8b0a5ef50596785c5b3d1b`

This handoff is for the paused `superpowers:subagent-driven-development` run for
`atx-impl/db` fundamentals parity. Continue with fresh implementer/reviewer subagents; the
controller should not write implementation code.

## Completed And Reviewed

- PF-S1 complete: `b39a2ee..b8fb113`
  - Canonical item registry schema/seed/resolver.
  - Ratio inputs governed through registry with byte-identical rebuild gate.
  - Additive fact and ratio item linkage.
  - Docs/ledger SHA placeholders fixed.

- PF-S2 complete: `85f1203..6338b02`
  - `DatasetOrchestrator` DAG, manifest/audit tables, incremental skips, retry/backoff, resume.
  - `run-all`/`refresh_quant_warehouse` wired through orchestrator.
  - Important review fix: same-`dataset_id` job variants now merge params, so `sec_submissions`
    preserves generic `10-K/10-Q/8-K`, ownership `3/4/5`, and blockholder `SC 13D/G` coverage.
  - No live network smoke was claimed.

- PF-S3 complete: `b14d2de..4cc46fb`
  - Full canonical companyfacts concept defaults and `db/seeds/concept_map.csv`.
  - Statement-map overlay reconciliation with explicit exception report.
  - Companyfacts-derived `fundamental_xbrl_metric` widened; inline XBRL cache remains operator gated.
  - Coverage/unmapped/universe-gap views and quality gates in migration `0069`.
  - PF-S3 docs updated; no live companyfacts refetch/backfill was run.

## In Progress At Pause

- PF-S5 S5-0 is implemented but not yet accepted.
  - Implementation commit: `a4c165a1038cdc5faf602a05d3564614bcdad496`
  - Review found two blockers:
    - Legacy pre-`0079` DB upgrade failed because `v_security_master_current` referenced
      `securities.entity_id` before migration `0079` could add it.
    - `ENTITY_ID` rows in `security_identifier_history` tripped the existing
      `identifier_multi_security_overlaps` quality check, even though shared entity IDs are valid.
  - Fix commit: `43c0a8d7f3086eaacc8b0a5ef50596785c5b3d1b`
    - View/bootstrap now tolerates missing `entity_id` pre-migration and refreshes after `0079`.
    - Quality check allows shared `ENTITY_ID` windows but still fails security-level identifier overlaps.
    - Added offline regressions for both issues.
  - Active/pending review agent at pause: `019f2491-468c-74c3-bb76-4107773f919a` (`Cicero`).
    - Last controller wait timed out, so do not mark S5-0 complete until this review returns clean
      or a fresh reviewer re-checks `a4c165a..43c0a8d`.

## Resume Point

1. First, check/wait for Cicero's S5-0 fix review.
2. If approved, append S5-0 completion to `.superpowers/sdd/progress.md`.
3. Continue PF-S5 in order:
   - S5-1 offline FIGI loader (`identifiers_figi.py`)
   - S5-2 offline LEI loader (`identifiers_lei.py`)
   - S5-3 link fundamentals to stable security/entity spine
   - S5-4 CUSIP export scan and identifier overlap repair, then PF-S5 docs/ledger
4. Then follow roadmap order: PF-S4 -> PF-S7 -> PF-S6 -> PF-S8.

## Verification State

- Last reported full suite pass was from S5-0 fix implementer:
  `python -m pytest atx-impl\db\tests -q`
- S5-0 fix review has not yet reported final verdict in this controller thread.
- No SEC/FRED/FINRA/OpenFIGI/GLEIF network tests were added or run.
- Live/operator smokes remain pending where sprint docs explicitly say they were not run.

## Dirty Worktree Notes

Do not run `git add -A`. The tree contains unrelated dirty/untracked files. Known relevant dirt at
pause includes:

- `atx-impl/db/fundamentals.py` - unrelated zip-loader/companyfacts work predating PF-S3/PF-S5.
- `atx-impl/db/tests/conftest.py` - unrelated dirty test harness work noted by S5 agents.
- Various CMake/editor/script files and untracked research/plans/backups unrelated to pf1.

Stage only explicit paths owned by the current sprint task. Preserve unrelated dirty files unless the
user explicitly asks to clean them up.
