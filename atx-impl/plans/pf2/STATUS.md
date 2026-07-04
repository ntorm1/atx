# PF2 Status — Fundamentals Depth + Production Warehouse

Updated: 2026-07-03, America/New_York
Branch: `feat/warehouse-parity`

This module is **designed, not started**. It succeeds pf1 (the fundamentals spine) and assumes
pf1 PF-S1…PF-S8 have landed. Begin with `superpowers:subagent-driven-development`; the controller
should not write implementation code.

## Predecessor state (pf1)

At pf2 design time, pf1 was in flight: PF-S1 (item dictionary), PF-S2 (orchestrator), PF-S3 (concept
coverage), PF-S5 (identifier spine), and PF-S4 (formula registry) had landed; **PF-S7 (XBRL validation)
was in progress, with PF-S6 (valuation multiples) and PF-S8 (restatement lineage) remaining.** pf2 must
not start until pf1's north-star acceptance is met. If a pf1 deliverable name differs from what a pf2
sprint references (e.g. `valuation_multiples.py`), reconcile to the landed name.

## Not started

- PF2-S1…PF2-S10 are all **pending**. No code, migrations, or tranche rows exist yet.
- Reserved migration range for the whole module: `0097–0131` (migration head at design time = `0083`;
  pf1 reserves through `0096`).

## Resume Point

1. Confirm pf1 is complete and green (`python -m pytest atx-impl\db\tests -q`).
2. Read `atx-impl/plans/pf2/ROADMAP.md` (contract, ownership, sequencing, north star).
3. Start PF2-S1 (schema-as-contract) — the platform foundation everything else lands on. Spin its
   worktree with `atx-impl/scripts/new_db_worktree.sh new pf2-s1`; `finish pf2-s1` merges it back into
   the mainline at sprint end.
4. Follow ROADMAP sequencing: S1→S2 → S3→S4 → (S5→S6 ‖ S7 ‖ S8) → S9 → S10. One worktree per sprint;
   never run two sprints sharing `fundamental_ratios.py`/`fundamental_statements.py` in concurrent
   worktrees.
5. Track progress in `.superpowers/sdd/progress.md`; append a `WAREHOUSE_PARITY_TRANCHES.md` row per
   sprint and update `PARITY_GAP.md`.

## Dirty Worktree Notes

Do not run `git add -A`. The tree carries unrelated dirty/untracked files. Stage only explicit paths
owned by the current sprint task. Preserve unrelated dirty files unless the user explicitly asks to
clean them up.
