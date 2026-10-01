# Lane MINE-FIX brief: findings of the mining-verb review (review-mine.md)

Read first: `lane-rules.md` (binding; never build C++), `.agents/cpp/agent.md`. Worktree and branch base in the
dispatch. Report `task-MINE-FIX-report.md` (commit `git add -f`). Findings: `review-mine.md` MINE-1..MINE-10 (fix
all; minors where local). Rulings: E-32, E-32a, E-33, E-33a in `progress.md`; pre-registration rule 10.
Order: MINE-1, MINE-5, MINE-4, MINE-3, MINE-2, MINE-7, MINE-8, MINE-6, MINE-9, MINE-10. One commit per finding,
`fix(mine): <what> (review MINE-n)`.
- MINE-1: the C++ verb writes a 64-hex chain head (sha256 of the registry content, as `campaign_line` expects);
  the Python test uses the verb's real output format; a C++ fixture test calls the same validation.
- MINE-5 (E-33a): `registry.count` = records this campaign added; `registry.total` = cumulative; tests with a
  shared registry across two campaigns.
- MINE-4 (E-32a): `--budget N` mandatory; hurdle from N; a fresh registry does not reset the hurdle; the campaign
  manifest and ledger line carry the budget.
- MINE-3: the confirm window (dates, role, fields, library, pool digests) is in the trial identity and the ledger
  line; a second confirm read on the same identity is refused, not skipped.
- MINE-2: the confirm read requires a minimum of `ic_defined` sessions (a registered constant) and `marginal_dates`
  equal to the confirm window; `promote` refuses otherwise.
- MINE-7 (E-32a): `--pool` mandatory for mined-v1; the verb refuses without it.
- MINE-8: `ResearchRole::load` calls `refuse_delisting_returns_signal_role` (FIX-AB B-3); test.
- MINE-6 (E-32a): derive the overlap correction factor by simulation under the null (overlapping 21-session labels,
  iid returns, the verb's own t estimator) in a test-time script committed with its seed; pin the factor as a
  constant; the hurdle applies to t / factor; write the derivation in the report.
- MINE-9: fixture tests that fail on a raw-IC confirm, an unfrozen sign, a wrong N and a seed-dependent stage 1
  (compare review-w1-T.md T-1 standard).
- MINE-10: memory estimate derived from the program depth / slot bound (add the bound if none exists) and the
  per-trial IC series retention (bounded or streamed); write the formula and a worked number at the config bounds.
Files you own: the mining verb (`atx-impl/src/strategy_mine*`), the engine signal-fitness hook files, `ResearchRole`
loader, `research_cycle.py ledger-campaign` and `campaign_line` in `backtest_integrity.py` (keep FIX-C / FIX-3
contracts), their tests. Run pytest for the Python you touch.
