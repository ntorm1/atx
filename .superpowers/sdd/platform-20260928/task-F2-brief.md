# Task F2: structural forecast for short-history risk factors (finding F1-F1)

**Pool:** C:/atx-wt/pool-7. `git status` clean, then `git checkout -B feat/platform-v7-f2-riskstruct-20260928 <BASE>`, BASE =
`git -C C:/atx-wt/pool-2 rev-parse HEAD`. Rules: never build, never run binaries or real data, never spawn subagents, never
touch C:/atx, never read validation/VAL/2023/2024/2025 files, no parameter tuned on returns. Read .agents/cpp/agent.md.
Post-implementation gtests. Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report task-F2-report.md
(<= 25 lines) in the pool-2 sprint dir; reply < 8 lines. You own strategy_risk_model.{cpp,hpp}, strategy_risk_verb.cpp and
their tests only.

**Finding (root, real data, lo1 role):** `risk bias random: refused (series ok 0, refused 64); observations 38,528,
dropped_factor_exposures 15,168 (28%)`; factor family fine (b .995). 11 of 62 factors have empty forecast series (< 63
observations: thin FF49 industries after the linked-operating restriction, possibly some styles early in the window), so
random long-short portfolios that load on them lose that exposure from x'Fx. The same gap makes the optimiser (spo-v1,
Sigma = X F X' + D) treat those exposures as zero variance.

**Fix (R3.1 "structural fallback", USE4 practice):**
1. Factor variance for a factor with < 63 observations: prior = the average variance of its factor class (industries ->
   the cap-weighted mean industry variance that day; styles -> mean style variance), blended by n/63 toward its own EWMA
   as observations accrue (Bayesian shrink; declare the weights). Correlations of such a factor with others: shrink toward
   0 with the same weight. Mark the factor `structural` in the manifest and in factor_covariance's side file per date.
2. Industries with fewer than 10 names are already merged into a residual bucket; verify the merge is applied per date and
   that the bucket is never empty of forecast. Report the per-date count of structural factors in diagnostics.csv.
3. Bias harness: with structural forecasts no observation should be dropped for a missing factor forecast; keep the
   counters and the 5% refusal.
4. The optimiser reads F through the same accessor, so spo-v1 sees the structural values automatically; add a gtest that
   a name loading on a short-history factor gets non-zero factor variance.

**Tests:** planted panel with one factor observed only 20 days: its variance equals the class prior blend; bias harness
random family not refused; factor with >= 63 obs bit-identical to before (identity for fully observed factors);
diagnostics counts.

**Root acceptance:** gtests green; risk verb on lo1 prints random family `ok` (or refused for a documented other reason)
with dropped_factor_exposures 0; factor family b unchanged within 1e-6 for fully observed factors.
