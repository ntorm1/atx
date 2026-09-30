# Lane FIX-AB brief: Wave 1 review fixes, areas A (book construction) and B (IC runner, signals, fields)

Read first: `C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/lane-rules.md` (binding; you never build C++).
Then `.agents/cpp/agent.md` in your worktree.
Worktree: `C:/atx-wt/pool-7`. First command: `git checkout -b feat/platform-v8-fixab-20260930 81abfa12`.
Report: `.superpowers/sdd/platform-v8-20260929/task-FIX-AB-report.md` in your worktree (commit with `git add -f`).

## Requirements

Fix every finding of severity I (Important) and M (Medium) in
`C:/atx-wt/pool-2/.superpowers/sdd/platform-v8-20260929/review-w1-A.md` and `review-w1-B.md`, in this order:
B-2, B-3, B-4, A-2, A-3, A-4, A-1, B-1. One commit per finding, message `fix(<area>): <what> (review X-n)`.

Before fixing any finding the review marks "unverified" or "partly unverified" (B-4, B-2, A-2), verify it against
the code and record in the report: CONFIRMED (file:line, the exact threshold or path) or NOT A DEFECT (reason).
Do not fix a NOT A DEFECT. For B-4 state the exact byte size of a 73-row fields manifest as the current builder
writes it (compute it from the writer's format, or from `build-equity/` manifests you may read) against the limit.

Binding rules for the fixes:
- B-2: the marginal IC verb reads themes from the same `theme_standardise` source as the composition step, so a
  screen after R-1 uses the R-1 themes; a test with two themes shows the marginal statistic is computed inside the
  theme.
- B-3: the signal role loader refuses (clear message naming the manifest and the offending flag / field) a role
  built with delisting returns when used as the signal role; the label role path (E-25, lane R45, unmerged) is not
  yours; keep the refusal in a function R45 can call.
- B-4: raise the fields-manifest read limit so a 73-row manifest with the widest plausible row is accepted with
  margin; the limit stays a named constant with the reason in a comment; a test reads a 100-row synthetic manifest.
- A-2: under a warm start gamma is calibrated on the first scored decision, not the first warm-up decision, or the
  warm-up decisions are calibrated from the risk store rows that exist; spo-v3 must not stop on a warm-start parent
  whose risk store has no earlier rows. State which you chose and why in one paragraph of the report.
- A-3: a warm start that builds no book is refused with a message that names the number of warm-up sessions and the
  first scored session; the NAV manifest records `warm_start_sessions` and the row index of `score_begin`, and the
  gross exposure at that row is written to the summary.
- A-4: the E-14 criterion (mean correlation of the traded book with the aim >= .9) is computed on the traded
  (post-solve, post-limits) weights, not the planned weights; both values are written; the criterion reads the
  traded one.
- A-1: the parallel grid with an ADV cap uses each variant's own leverage.
- B-1: the price-field reuse fingerprint includes the session calendar (its digest), so a role with a different
  calendar never reuses a price field; a test.

Identity: every change is behind the existing flag or is provably value-preserving with the flags off. Say in the
report, per finding, how root verifies it (gtest filter, identity run argv).

Files you own: `atx-impl/src/strategy_*.cpp` and headers, `atx-engine` files the findings name, their gtest files,
`atx-engine/tools/research_fields*.py` for B-1 and its tests. Do not touch `scripts/research_cycle.py`,
`scripts/cycle_verdict.py`, `scripts/compare_window_overlap.py`, `scripts/backtest_integrity.py` (lane FIX-C owns
them). Write C++ that compiles first time under `/W4 /WX`; you cannot build. Run pytest for the Python you touch.

Minor findings (m) in reviews A and B: fix only if inside a line you already edit; otherwise list them untouched.
