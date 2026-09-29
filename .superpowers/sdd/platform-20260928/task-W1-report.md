# Task W1 report: spo-v1, a cost-aware single-period optimiser around the GP aim
Branch feat/platform-v7-w1-spo-20260928 @ 8326d660 (pool-3, base 5b958dd2, 3 commits). Nothing was compiled or run (hard rule), so expect one /W4 /WX fix-up pass. The TU is on the /O2 list in the Debug tree.

**Files.** New: src/strategy_spo.{hpp,cpp} (solver, RiskStore, Engine) and tests/strategy_spo_test.cpp. Registration: strategy_nav_v7.{hpp,cpp} (`--rule spo-v1` rewritten to aim-partial-v5 as v6 is, the flags, State::plan route, relabel to spo-v1, recipe/summary/extras blocks, `--spo-books`) and CMake (lib + /O2 list + target tests). strategy_nav_replay.cpp has one seam argument: plan_weights passes `p.tiers.tier, p.no_locate` to v7::plan. Those defaults are empty, so the no-extension path is still update_weights, and the decide path uses the same seam.

**Formulas as implemented** (per rebalance decision and book; non-rebalance decisions are v5, exits only)
- Objective: max a'w - (gamma/2) w'(XFX'+D)w - (1/H) sum[s|dw| + eta|dw|^1.5] - sum[b max(-w,0) + l max(w,0)].
- Terms:
  - a_i = IC_book sqrt(D_i) z_i, with z = desired / SD over members (GK with atx-risk-v1 specific vol).
  - s = (5+1) bps and eta_i = .6 sigma_i sqrt(NAV/ADV_i). This is the primary S2 law on the decision window, for every book, as in v6.
  - b and l are the book's own financing per session: annual bps x (365/252)/day_count, with the tier fee at d.
- Constraints:
  - Book net 0 and book gross <= --aim-leverage.
  - |beta'w| <= .02, with beta = Sigma m / m'Sigma m and m the equal-weight portfolio of the optimized names.
  - |w_i| <= min(w_max, q ADV/NAV) and |dw_i| <= p ADV/NAV.
  - w_i >= min(w0_i, 0) where the book's locate rule guards the name. The replay's locate block is then a no-op.
- Names: members with a risk row are optimized. Nonmembers follow v5's exit_rate/dust rule. Members without a forecast keep their weight. Both groups enter net, gross, beta and risk as fixed positions.
- Risk inputs: X/F/D are the risk verb's row d, read by seek. NaN factor entries are set to 0, the same convention as the L4 bias harness.
- Refusals: a date without a forecast refuses (Unavailable). So does a role, session axis or manifest SHA that differs.

**Decisions to pre-register (none from returns)**
- H = --spo-horizon, default 1/theta = 20. Under GP quadratic costs, gamma/lambda = theta^2/(1-theta), so a single-period problem amortizing costs over H trades at rate theta exactly when H = 1/theta. This is Boyd et al. 2017 gamma_trade = theta. At H = 1 the book barely deploys: alpha is about 4e-4 z per day against 6 bps one-way.
- gamma = max(gamma_vol, gamma_bind), set once on the first rebalance decision's cost-free aim (net 0, |beta| <= .02, |w| <= w_max, no ADV caps):
  - gamma_vol sets the aim's annualised vol to .05.
  - gamma_bind sets the aim's gross to L.
  - Each is a bracketed monotone root in ln gamma.
  - Why max: with L = 1.247, a 5% aim needs gross of about 5, so a vol-only gamma turns the budget into a lasso on roughly 35% of names. The diagonal estimate is gamma_bind about 1,100 against gamma_vol about 270.
  - Result: gamma_bind binds, and ex-ante vol is about 1-2%. `--gamma` overrides.
- Defaults: IC .02, w_max .01, q .05, p .01, 500 iterations, tolerance 1e-8, beta .02 (R2.1).
- Not reused: engine optimizer.hpp. Its fast path is, by its own header, a gross-normalized projection heuristic, not a solver of this objective.

**Solver and complexity**
- Method: FISTA (Beck-Teboulle) with adaptive restart (O'Donoghue-Candes) in the metric sigma gamma D. sigma = 1 + 1.1 lambda_max(D^-1/2 XFX' D^-1/2 on 1'd = 0), estimated by 40 power steps once per date and shared by the books; backtracking on the exact quadratic covers an underestimate.
- Exact prox:
  - For fixed dollar/gross multipliers, a name's positive part depends only on alpha+ = mu + nu and its negative part only on alpha- = mu - nu. That gives two independent monotone 1-D roots (safeguarded Newton), with a gross-slack case alpha- = -alpha+.
  - Beta is an outer monotone root.
  - The linear + |.|^1.5 cost has a closed-form 1-D minimizer (cancellation-free quadratic in sqrt|dw|).
  - Every iterate is feasible.
- Stop: when the prox-gradient residual is <= tol (a KKT certificate), or at the iteration cap. Warm start from w0 and the book's previous multipliers. Deterministic.
- Cost per iteration: the gradient is X(F(X'y)) over sparse rows (market, industry, 11 styles): about 2 x 13N + K^2, about 50k flops at N = 1,750. The prox is about 5 passes of N 1-D minimizers, and 3-4 times that while beta binds. That is about 0.1-0.3 ms per iteration.
- Expected per-decision solve time: 50-150 iterations, so about 10-30 ms per book, and about 50-150 ms per decision with the 5 books.

**Outputs**
- <output>/spo_diagnostics.csv: one row per (decision, book). It carries a'w, risk, trade cost (unamortized and /H), financing, objective, ex-ante vol (new and current), TC = corr(a/sigma^2, w), gross/net/long/short, |beta'w|, turnover, iterations/restarts/backtracks/passes, converged, KKT residual, coupling met, no-trade/at-cap/at-trade-limit/at-locate-floor counts, gross/beta binding and mu/nu/rho.
- A calibration and summary block in v7_extras.json and summary.json.
- The L4 TC file as before.
- A console line with solve count, total, mean and max ms, and gamma.

**Gtests** (atx-impl-strategy-target-tests)
- SpoSolver.{KktResidualBelowToleranceOnFiftyNames, CostFreeUnboundedSolutionIsTheClosedFormMarkowitz, NetGrossBetaAndBoxesHoldTo1e10, WarmAndColdStartsAgreeAndRepeatBitForBit, TwoNamesMatchAOneDimensionalSearchWithTheThreeHalvesCost}
- SpoRisk.RefusesADateWithoutForecastAnotherRoleAndAnotherPin
- SpoHook.{FlagOffKeepsAimPartialV5BitForBit, ParseRoutesTheRuleAndRefusesBadCombinations, ReplayPlansNeutralBudgetedBooksAndRelabelsTheRule, ReplayRefusesWhenTheRiskModelLacksADecisionDate}

**Root commands** (pool-2, after merge)
1. `powershell -File build-equity\mega-build.ps1 -Tag v7-w1 -Targets atx-impl-strategy-target-tests,atx-equity-strategy-targets,atx-equity-strategy-risk`
2. `build-equity\bin\atx-impl-strategy-target-tests.exe --gtest_filter=Spo*:NavV7Hook*:AimV6*:TransferCoefficient*:StrategyNavReplay*:NavV5*:TargetReplayV5*:StrategyLive*`
3. Risk model with per-date exposures: `python scripts/run_bounded_research.py --output build-equity/v7-w1-risk-all-run --seconds 180 --max-rss-mib 1536 --min-free-mib 512 -- build-equity\bin\atx-equity-strategy-risk.exe risk --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7/manifest.json --fields-sha256 1d1fa87a00d519bcf08fbec83f3fd17029e23650a98a9af26e99ddb1f1a73ee1 --emit-exposures all --output build-equity/v7-w1-risk-all`. Expect about 35 s and about 520 MiB; style_exposures.f32 is 286 MB. Then set RISK_SHA = sha256 of build-equity/v7-w1-risk-all/manifest.json.
4. Cell, after pre-registration: `python scripts/run_bounded_research.py --output build-equity/mega-nav-v61u-spo-v1-L1.247-run --seconds 180 --max-rss-mib 1536 --min-free-mib 512 -- build-equity\bin\atx-equity-strategy-targets.exe nav --combined build-equity/mega-v61w-train-ew-1/train_combined.json --combined-sha256 62bc30a3bf1ee047c200e35064f8cfe18c089a77adb3f651e4615b3c05d1c6c0 --role build-equity/recent-fast-train-2020-2022-v2-lo1/manifest.json --role-sha256 3e79978a858cbf6b723ff7a896d56814f7505dde11b805c30c5b909783ebb809 --fields build-equity/recent-fast-train-2020-2022-v2-lo1-fields-v7/manifest.json --fields-sha256 1d1fa87a00d519bcf08fbec83f3fd17029e23650a98a9af26e99ddb1f1a73ee1 --cadence 1 --trade-fraction .05 --dust-multiple .1 --aim-leverage 1.247 --daily-turnover-mean-max .20 --daily-turnover-p95-max .30 --neutralize price-risk-v1 --max-bytes 1073741824 --order-basis delta --exit-rate .05 --locate-in-aim --liquidity-cache --rule spo-v1 --risk-model build-equity/v7-w1-risk-all --risk-model-sha256 <RISK_SHA> --ic-book .02 --w-max .01 --adv-cap-q .05 --adv-trade-p .01 --spo-iters 500 --spo-tol 1e-8 --target-vol .05 --spo-books primary --output build-equity/mega-nav-v61u-spo-v1-L1.247`

**Expected wall time and memory**
- `--spo-books primary` (S1 + S2, 1,500 solves): about 50-80 s, about 420 MiB. The S2 files are bit-identical to `all`: the books are independent, and gamma and the date data are shared inputs.
- `all` (5 books, 3,750 solves): about 90-170 s. Use it only if step 4's console line shows a mean below about 25 ms per solve.

**Untested (no build):** compile and /WX; every gtest; the real-data iteration counts, timing and RSS; whether the risk model forecasts every TRAIN decision date (a gap refuses the run with its session). The gtests compare to the closed-form Markowitz solution and a 1-D brute force, but the prox has not been cross-checked on a real problem with more than two names against an external solver.
