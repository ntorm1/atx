# Task L4: fundamental risk model + bias harness, name-level cost model v2, and construction rule aim-partial-v6

**Pool:** C:/atx-wt/pool-3. `git status` must be clean, then `git checkout -B feat/platform-v7-l4-risk-20260928 <BASE>`
where BASE = `git -C C:/atx-wt/pool-2 rev-parse HEAD`. Work only in pool-3. Never build C++ (root builds), never run the
pipeline or any real data, never spawn subagents, never touch C:/atx. You may read C:/atx-wt/pool-2/build-equity/** by
absolute path (TRAIN outputs only; never anything named validation/VAL/2023/2024/2025). Commit trailer:
`Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report: C:/atx-wt/pool-2/.superpowers/sdd/platform-20260928/
task-L4-report.md (<= 40 lines). Reply in chat < 15 lines. Read .agents/cpp/agent.md first. No TDD ceremony: implement,
then post-implementation gtests. Do NOT choose or tune any parameter by looking at TRAIN returns: parameters come from
the literature values in the brief; root pre-registers them before any run.

**Read first:** literature-v7.md S2 (R2.2, R2.3, R2.5), S3 (R3.1, R3.3, R3.4), S4 (R4.1, R4.2); code-review-v7.md B5, C1,
S4; plan-v7.md S2-S3 (lane L3 owns strategy_nav_replay.cpp: you may add ONE dispatch hook there, <= 30 lines, for the new
rule and the new scenarios, and you rebase on L3 at merge; everything else goes in new files). Code: atx-engine
risk/factor_model.hpp, risk/optimizer.hpp (existing, unused), atx-impl/src/strategy_price_exposures.cpp (price-risk-v1),
strategy_target_replay.cpp:222-282 (aim-partial-v5), research_cost_sim.hpp and the cost code in strategy_nav_replay.cpp
(linear 6 bps, sqrt impact, 1% ADV cap, swap financing tiers), diag_risk.hpp.

**Deliverables (new files unless stated)**
1. `atx-impl/src/strategy_risk_model.{cpp,hpp}` = `atx-risk-v1`: daily sqrt(mcap)-weighted WLS of returns on market +
   FF49 industries (merge < 10 names into a residual bucket) + styles available from the fields set (size, beta252,
   residual vol, 12-1 momentum, value, profitability, asset growth, leverage, ladv63, SI/DTC -- use what exists; list
   what does not). Factor covariance EWMA half-lives 84 (variance) / 504 (correlation), Newey-West lags 5/2, volatility
   regime adjustment 42 d; specific risk EWMA-84 shrunk to the size-decile mean with a structural fallback under 252 d.
   Output: per-date factor exposures, factor covariance, specific variances; a `risk` verb (or flag) that writes them
   for a role/fields set. Reuse atx-engine/risk where it fits; do not rewrite what works.
2. Bias harness: rolling 63/252-day bias statistic (realised/forecast) on random long-short portfolios, on the
   factor-mimicking portfolios and on a supplied book weights file; band 1 +- sqrt(2/T) (report the kurtosis-widened band
   too). Output CSV + JSON.
3. `atx-impl/src/strategy_cost_v2.{cpp,hpp}`: name-level impact model with Kyle-Obizhaeva invariance form
   impact_i(x) = (sigma_i/.02) (W_i/W*)^(-1/3) [k0 + kI sqrt(x (W_i/W*)^(2/3) / .01)] with KO constants as the prior,
   and a FIM form a + b x + c sqrt(x) at the FIM medians; exposed as stress scenarios `S2-KO` and `S2-FIM` next to
   S1/S2/S3 (S2 stays primary; existing scenario outputs byte-identical). Capacity curve: given the replay's planned
   trades, cost per dollar and net SR at NAV multiples {.5, 1, 2, 4, 8}x (replayed, not extrapolated: the participation
   cap binds), written as a CSV.
4. Construction rule `aim-partial-v6` (R2.2 + R2.3), selectable by `--rule aim-partial-v6` with params `--cost-shrink-kappa`,
   `--band-b`, `--rate-clip lo,hi`: target_i = aim_i / (1 + kappa c_i/c_bar); band_i = (b/N)(c_i/c_bar)^(1/3) replaces the
   uniform dust multiple; theta_t = theta (c_bar_ref/c_bar_t)^(1/2) clipped to [lo, hi] theta, where c_i is the modelled
   marginal cost per dollar of name i on that date and c_bar the cross-sectional median. Defaults kappa 1, b = dust-equivalent
   (b such that the median band equals the current dust .1), clip [.5, 1.5]. Report the transfer coefficient
   corr(alpha_i/sigma_i^2, w_i) per date (R2.5). aim-partial-v5 output must stay byte-identical.
5. Tests (gtest, synthetic): WLS recovers planted exposures; covariance EWMA/NW on planted series; bias statistic ~ 1
   on correctly specified data; KO formula values; scenario identity for S1/S2/S3; aim-partial-v5 identity; v6 rule
   with kappa 0 and clip [1,1] and a band equal to dust reproduces v5 bit-for-bit.

**Root acceptance:** all tests green under the v7 build; risk model + bias harness run on the v6.1 role within 180 s /
1536 MiB (state your expected cost); S2-KO/S2-FIM on the v6.1 cell; aim-partial-v6 cells only after root writes
v7-prereg.md with the declared parameters (you do not run anything).

Report: changes with file:line, formulas as implemented, defaults and their literature source, expected RSS/time, the
exact root command lines, and what you could not test without a build.
