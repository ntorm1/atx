# Task W4: production path hardening -- holdings writer, order file, reconciliation, corporate actions (C++)

**Pool:** C:/atx-wt/pool-4. `git status` clean, then `git checkout -B feat/platform-v7-w4-orders-20260928 <BASE>`, BASE =
`git -C C:/atx-wt/pool-2 rev-parse HEAD`. Rules: never build, never run binaries or real data, never spawn subagents,
never touch C:/atx, never read validation/VAL/2023/2024/2025 files, never change kSeal. Read .agents/cpp/agent.md first.
Post-implementation gtests. Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report task-W4-report.md
(<= 40 lines) in the pool-2 sprint dir; reply < 12 lines. You own strategy_live.{cpp,hpp}, strategy_nav_replay.{cpp,hpp}
(holdings observer only; the plan_weights dispatch is shared with lane W1, which registers a rule there: keep your edits
away from it), tools/equity_strategy_targets.cpp, tests/strategy_live_test.cpp, new files.

**Read first:** task-L3-report.md (+ fix-up sections), code-review-v7.md B7, B8, B9, C5 and S2 axis B; literature-v7.md
S7. Findings to fix: L3-F1 = `--emit-holdings` made the v6.1 NAV run take 140 s and 374 MB (holdings.csv with ~1.9 M
rows) vs 34 s without; the 180 s cap is near.

**Deliverables**
1. Holdings writer: stream rows through a buffered writer, shard by year (`holdings-<year>.csv`) or write a compact
   binary (`holdings.f64` + `holdings_index.json`) with a small reader in the decide path; keep holdings_days.csv. Target:
   flag-on run <= 1.3x the flag-off wall time. NAV outputs stay byte-identical. `decide --positions` must accept the new
   layout and the old CSV.
2. Order file (B7): `decide` gains `--nav-dollars <NAV> --lot-size 1 --min-notional 500 --price-source close` and writes
   `orders_shares.csv` (name, side, shares rounded to lots, notional, reference price, participation of ADV, tag) plus a
   summary; rounding residuals reported; rows below min notional dropped and counted. Deterministic rounding rule
   documented.
3. Reconciliation (B8): `reconcile --deploy <m> --expected <holdings from the prior decide> --broker <positions csv>`
   compares by security id (and by CIK/ticker via the identity bridge if the broker file has tickers only), prints
   breaks (missing, extra, quantity mismatch beyond a tolerance), applies split/dividend adjustments from a
   `--corporate-actions <csv>` (ratio per name per date) to the expected side, and exits non-zero on any unexplained
   break. Schemas documented in the report.
4. Health (B9): add to decision.json the L3-F2 transfer-coefficient band check (warn if TC < .5 for 5 consecutive
   decisions -- the state comes from a `--prior-decisions <dir>` argument) and a data-freshness check (as-of session
   must be the role's last session unless `--allow-stale`).
5. Tests (gtest, synthetic): writer identity (flag on/off NAV bytes equal; reader round-trip); order rounding and
   min-notional; reconciliation break detection incl. a 2:1 split explained by corporate actions; freshness refusal;
   TC band warn.

**Root acceptance:** v6.1 NAV run with `--emit-holdings` <= 1.3x flag-off wall and identical NAV outputs; decide at the
three L3 dates unchanged (parity 0) and orders_shares.csv produced; reconcile on the decide's own holdings = 0 breaks,
on a mutated copy = the expected breaks. Report: schemas, file:line, the exact root command lines.
