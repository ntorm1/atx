# Lane ERA brief: Ruling E-35 (pooled fit supports v8 compositions) and review finding P-1

Read first: `C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding).
Worktree: `C:/atx-wt/pool-3`. First command: `git checkout -b feat/platform-v8-era-20260930 b44774d6`.
Report: `.superpowers/sdd/platform-v8-20260929/task-ERA-report.md` in your worktree (commit with `git add -f`).
Context: `task-H-1-report.md` (era pooling: era_pool, pooled fitter, nav_summ, ledger, roles loop, era_data_audit),
`task-A2-report.md` section "R-3 on the R-1 rule" (ew-theme-std-aim-v1), `review-w1-P.md` finding P-1, ledger
rulings E-17, E-27, E-35 in `progress.md`.

## Requirements

1. Ruling E-35: the pooled (era) fit supports `ew-theme-std-v1` and `ew-theme-std-aim-v1`. Today it refuses the
   aim variant and ignores the std variant. The pooled fit of a v8 composition must apply the same five registered
   rules of ew-theme-std-v1 (and E-27's gains inside each theme for the aim variant) that the single-window fitter
   applies, on the pooled data, with the same Python implementation (call the existing function; never copy the
   rule). Any composition id the pooled fitter does not implement is refused with a message naming the id (no
   silent fall-back to another rule). Tests on synthetic data: (a) pooled fit of ew-theme-std-v1 over one era equals
   the single-window fit on that era byte for byte; (b) the aim variant likewise; (c) an unknown id is refused.
2. P-1: a history read on one role writes a ledger line with label `ERA`, an era block and no `era_of`, and
   `dsr_variance`, `research_ledger.cells` and `ledger_net_series` treat it as a TRAIN cell. Fix: every history-read
   line carries `era_of` (the TRAIN cell it re-reads) and the three readers skip lines that carry an era block
   whether or not `era_of` is present; a test for each reader with a fixture ledger that holds one such line.
   Note lane FIX-C (merged) changed `backtest_integrity.py` (hash chain, rerun_of, mining-campaign kind); read the
   merged code before editing and keep its contracts.

No era read, no real data. Run pytest for every test file you touch and the era / ledger test files. Files you
own: the era pooling modules of H1, `nav_summ.py`, the research ledger module, `fit_composition_weights.py` only
where the pooled path calls it, their tests. Do not edit `scripts/research_cycle.py` or report tooling (lane
FIX-2 owns them).
