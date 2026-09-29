# Task W1: cost-aware single-period optimiser around the GP aim (construction rule `spo-v1`)

**Pool:** C:/atx-wt/pool-3. `git status` clean, then `git checkout -B feat/platform-v7-w1-spo-20260928 <BASE>`, BASE =
`git -C C:/atx-wt/pool-2 rev-parse HEAD` (contains L1-L4: atx-risk-v1 in strategy_risk_model.{cpp,hpp}, cost v2 in
strategy_cost_v2.{cpp,hpp}, aim-partial-v6 in strategy_nav_v7.{cpp,hpp}, the L3 plan_weights dispatch in
strategy_nav_replay.cpp). Rules: never build, never run binaries or real data, never spawn subagents, never touch C:/atx,
never read validation/VAL/2023/2024/2025 files, never pick parameters from TRAIN returns (defaults from the literature
below; root pre-registers before any run). Read .agents/cpp/agent.md first. Post-implementation gtests. Trailer
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report task-W1-report.md (<= 40 lines) in the pool-2 sprint
dir; reply < 12 lines.

**Read first:** literature-v7.md S2 R2.1 (objective, constraints, solver notes) and S3 R3.4; code-review-v7.md B5;
plan-v7.md; task-L4-report.md (what exists: risk model outputs = factor exposures X (N x K), factor covariance F, specific
variance D per date; cost v2 = per-name half-spread + impact eta_i; transfer coefficient); atx-engine/include/atx/engine/
risk/optimizer.hpp and risk/factor_model.hpp (existing QP / GP aim code, reuse if sound); strategy_nav_v7.cpp (how a v7
rule plugs into plan_weights and how the flag-off identity is preserved).

**Deliverable: rule `spo-v1`** (new files atx-impl/src/strategy_spo.{cpp,hpp} + tests/strategy_spo_test.cpp; the only
touch to strategy_nav_replay.cpp / strategy_nav_v7.cpp is registering the rule and its flags).
Per decision date, given alpha vector a (the pinned combined signal, scaled to expected return per GK: a_i = IC_book *
sigma_i * z_i with IC_book a declared constant, default .02), current weights w0, risk model (X, F, D) from a
`--risk-model <dir>` produced by the risk verb (load exposures_last/factor_covariance/specific_variance for the as-of date;
refuse if the date is missing), costs c_i (linear s_i and impact eta_i from cost v2), solve
  max_w  a'w - (gamma/2) w'(X F X' + D)w - sum_i [ s_i |dw_i| + eta_i |dw_i|^{3/2} ] - sum_i b_i max(-w_i, 0)
  s.t. 1'w = 0 (dollar neutral), |beta'w| <= .02, sum |w_i| <= L, |w_i| <= min(w_max, q ADV_i/NAV), |dw_i| <= p ADV_i/NAV
       (p = .01), w_i >= 0 where no locate.
Solver: no external dependency is available; implement a projected/proximal gradient (FISTA) or ADMM on this convex
problem with the factor structure exploited (never form the N x N covariance; use X (F (X' w))). Deterministic iteration
count and tolerance (declared flags, defaults 500 iterations / 1e-8); the |dw|^{3/2} term via its prox or a smooth
approximation with a declared epsilon. Warm-start from w0. Output the same planned-weights interface as aim-partial-v5 so
the replay's fills, costs, scenarios and holdings emit are unchanged. Flags: `--rule spo-v1 --risk-model <dir> --gamma <g>
--ic-book .02 --w-max .01 --adv-cap-q .05 --adv-trade-p .01 --spo-iters 500 --spo-tol 1e-8`. gamma default: calibrated
to a declared ex-ante vol target (`--target-vol .05` annualised) by a 1-D bisection on the first decision date only
(documented), not tuned on returns.
Diagnostics per date (CSV): objective terms, ex-ante vol, TC = corr(a_i/sigma_i^2, w_i), iterations, constraint
activity counts, gross/net, |beta'w|.

**Tests (synthetic, gtest):** KKT residual < tol on a 50-name problem; identity with the closed-form Markowitz solution
when costs = 0 and no bounds; dollar neutrality and box constraints hold to 1e-10; warm start reproduces the same
solution as cold start (determinism); flag-off identity for aim-partial-v5 (byte-identical planned weights); refusal when
the risk model lacks the as-of date.

**Root acceptance:** tests green; a TRAIN cell `mega-nav-v61u-spo-v1-L1.247` runs under 180 s / 1536 MiB (state your
estimate; the replay makes ~750 decisions, so the per-date solve must be <= ~150 ms at N ~ 1,750 members); results are
compared to the v6.1 cell only after root pre-registers the cell. Report: formulas as implemented, defaults and sources,
complexity per iteration, expected wall time, the exact root command lines.
