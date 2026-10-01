# Lane FIX-2 brief: Wave 1 review part 2 fixes (P-2, P-3, T-1, T-2, T-4, N-1, N-2, N-3)

Read first: `C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding; never build C++),
then `.agents/cpp/agent.md`.
Worktree: `C:/atx-wt/pool-7`. First command: `git checkout -b feat/platform-v8-fix2-20260930 b44774d6`.
Report: `.superpowers/sdd/platform-v8-20260929/task-FIX-2-report.md` in your worktree (commit with `git add -f`).
Findings: `review-w1-P.md`, `review-w1-T.md`, `review-w1-N.md` in the sprint directory. Rulings that bind the
fixes: E-14, E-29, E-31, E-36, E-37 in `progress.md`. One commit per finding, `fix(<area>): <what> (review X-n)`.

Order and binding rules:
- N-2 (with Ruling E-37): spo-v3 accepts `--capacity-curve` as a report-only pass (E-29): the primary series and
  scored files are unchanged, the capacity rows are computed on the spo-v3 book at each multiple with the ADV cap
  of E-15. Theta stays without effect on spo-v3 and the verb says so in `--help`; a test.
- P-2: the pitch config and scorecard template carry every registered criterion: R-6 the E-14 check (traded-book
  correlation with the aim >= .9, the value FIX-AB writes), "tripwire clear" and the E-31 `limits_unmet == 0` on
  scored decisions; R-5 "S3 not lower"; R-7 "turnover not higher" (Ruling E-36: marginal IC is not an acceptance
  criterion; the card shows it report-only). Tests pin the criteria per cell.
- P-3: `ladder_checks` compares each cell's recorded verdict with the computed rule (rule 5 of `v8-prereg.md`:
  paired S2 net dSR > 0 against the parent AND mechanics AND the cell's mechanical criterion) and refuses a recorded
  accept that the rule rejects; it also checks that every cell's parent is the last accepted cell before it (a
  rejected cell is never a parent). Tests for both refusals.
- N-3 (card binding, Ruling E-36): `alpha_report_card.py` binds the K6 marginal IC it loads to role, window, pool
  and library (their digests are in the K6 receipt or manifest; refuse when they differ from the card's) and the
  K6 pin is mandatory; the card labels marginal IC "report-only (rule 8)". Test.
- N-1: the reuse inputs of `ftd_shares_ratio21`, `regsho_threshold_days63` and `sv_offexchange_share126` (and any
  other holdings field whose payload depends on the seal) include the seal value from `research_window`, so a
  `--reuse` after a seal move recomputes them; test: two seals give two fingerprints; the same seal reuses.
- T-1: strengthen the ew-theme-std-v1 C++ tests so a wrong rule 1 fails: distinct ranks whose sum is not any single
  member's rank, mixed signs, unequal weights inside a theme; the expected values are computed in the test from
  the five registered rules written out (not from the code under test). No production code change.
- T-2: strengthen the gscore7_lowbm tests so a look-ahead read of me_company fails: give session t and t+1 different
  me_company for one instrument and assert the value at t. No production code change unless the test exposes a
  defect; if it does, STOP, report it under "defect found", do not fix it (it is a registration matter).
- T-4: add an independent reference for the spo-v3 solver: a two-name factor-covariance problem with a closed-form
  optimum computed in the test, a nonzero external_gap case, and a pinned gamma annualisation (sqrt(252)) value.
  No production code change unless a test exposes a defect; same STOP rule as T-2.

Files you own: report tooling (pitch config, scorecard template, ladder checks, `alpha_report_card.py`),
`atx-impl/src/strategy_nav_v7.cpp` and the spo-v3 verb files for N-2 only, `research_fields_holdings.py`, the gtest
and pytest files of T-1, T-2, T-4. Do not edit `scripts/research_cycle.py`, `backtest_integrity.py` or the era
pooling modules (lanes FIX-C, merged, and ERA own them). Run pytest for every Python test you touch. Write C++ that
compiles first time under `/W4 /WX`.
