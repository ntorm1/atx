# Lane FIX-C brief: Wave 1 review fixes, area C (research tooling, Python) plus Ruling E-33

Read first: `C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding).
Worktree: `C:/atx-wt/pool-3`. First command: `git checkout -b feat/platform-v8-fixc-20260930 81abfa12`.
Report: `.superpowers/sdd/platform-v8-20260929/task-FIX-C-report.md` in your worktree (commit with `git add -f`).

## Requirements

Fix every finding of severity I (Important) and M (Medium) in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/review-w1-C.md`, in this order:
C-1, C-2, C-9, C-10, C-3, C-4, C-5, C-6, C-7, C-13, C-11, C-12. Skip C-8 (ruled, E-34: the freeze-gate p is
one-sided; both p values are printed). One commit per finding (or per tightly coupled pair), message
`fix(research): <what> (review C-n)`.

Before fixing any finding the review marks "unverified" or "partly unverified", verify it against the code and
record in the report: CONFIRMED (with file:line) or NOT A DEFECT (with the reason). Do not fix a NOT A DEFECT.

Binding rules for the fixes:
- C-1: the cycle verdict and the headline DSR use the pre-registered cross-trial variance: `research_cycle.py` passes
  `--dsr-ledger` (the sprint ledger of record) to the summ / verdict step and `cycle_verdict.py` refuses to compute a
  DSR without it. The ledger N and the variance come from the ledger, never from a cell count. Add a test that
  reproduces the review's example (Sharpe 1.0, N 41: DSR must be the ledger-variance value, not .42).
- C-2: every summ step under a v8 spec carries `--protocol v8`; add-alpha specs derived from a v8 parent inherit the
  v8 bootstrap seed and draw count; a test pins the argv.
- C-9 / C-10: `compare_window_overlap.py` reports `bit_identical: false` and a reason when zero cells were compared;
  NaN-against-value counts as a mismatch; NaN-against-NaN as a match; tests for all three.
- C-3, C-4, C-5: `backtest_integrity.py` refuses (non-zero exit, clear message) a defect flag or rerun flag on a
  cell that is already ledgered unless the rerun line names a `rerun_of` that exists and is itself a ledgered cell
  with a `defect:` line; a rerun never lowers N (the replaced cell stays counted; the rerun adds 0).
- C-6 / C-7: the hash chain covers the 37 legacy lines (chain from the first line of the ledger, legacy lines hashed
  in their stored form); Appendix A prints the admission-trial count from the ledger's admission lines.
- C-13: resume checks that the NAV artefact's recorded spec digest (or its argv digest if no spec digest exists)
  matches the current spec before scoring it; a mismatch refuses with the two digests in the message.
- C-11: the `holdout_gate` ruling file carries a sha256 of its content in the ledger line that cites it, and the
  gate refuses when the file's hash differs.
- C-12: `cache gc --under` normalises paths (case, separators, trailing slash, `..`) before comparing with the
  referenced stores; a test shows a referenced store spelled differently is kept.
- Ruling E-33: `mining-campaign` joins `LEDGER_KINDS` in `backtest_integrity.py`; a `mining-campaign` line adds 0
  to the construction N and carries its own registry count field; test.

Identity: none of these changes may alter any number produced when the same inputs are given and the flags are
consistent; where output changes (C-1 DSR, C-7 admission count) say so in the report with the old and new values on
the synthetic fixture.

Files you own: `scripts/cycle_verdict.py`, `scripts/research_cycle.py`, `scripts/compare_window_overlap.py`,
`scripts/backtest_integrity.py`, `scripts/holdout_gate.py` (or wherever the gate lives), the `cache gc`
implementation (find it; likely `scripts/research_gc.py`), their tests under `scripts/tests/` or the tool tests
directory. If a fix needs a file of area A or B (C++ or field builders), stop and list it in the report as
"cross-lane" instead of editing it.

Run the tests you touch with pytest (lane rules 5). Report the exact command and counts.

Minor findings (m) in review C: fix only if the fix is inside a line you already edit; otherwise list them
untouched in the report.
