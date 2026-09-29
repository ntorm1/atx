# Task W2: DSL ops for the literature alpha families (atx-engine alpha VM)

**Pool:** C:/atx-wt/pool-10. `git status` clean, then `git checkout -B feat/platform-v7-w2-dslops-20260928 <BASE>`, BASE =
`git -C C:/atx-wt/pool-2 rev-parse HEAD`. Rules: never build, never run binaries or real data, never spawn subagents,
never touch C:/atx, never read validation/VAL/2023/2024/2025 files. Read .agents/cpp/agent.md first. Post-implementation
gtests (oracle <-> VM <-> streaming differential tests are the house pattern: read atx-engine/tests/alpha/* first).
Trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Report task-W2-report.md (<= 40 lines) in the pool-2
sprint dir; reply < 12 lines.

**Read first:** code-review-v7.md finding A7 and S4 Lane 4 (files: atx-engine/include/atx/engine/alpha/{cs_ops,ts_ops,
ts_order_stat,registry,typecheck,streaming_engine}.hpp; atx-engine/src/alpha/{registry,typecheck,oracle}.cpp; registry.cpp:
107-109; cs_ops.hpp:425-433; ts_ops.hpp:25-26); literature-v7.md S6 (wave-1 families: q5 expected growth, nincr,
QMJ-safety) and v6-literature.md S4 (missing families) for what the ops must express; the static library checker
atx-impl/strategies/check_fund_ic_v6.py (allowed-ops list; extend it).

**Ops to add (semantics version stays 1 for existing opcodes; new opcodes get new ids; NaN semantics documented per op):**
1. `ts_topk_mean(x, w, k)`: mean of the k largest values in the trailing window w (MAX5-style signals); NaN below
   min-periods.
2. `bucket(x, n)` -> Group: cross-sectional n-quantile bucket ids (rank-based, ties deterministic), NaN -> no group.
3. `group_cross(g1, g2)` -> Group: the product group (e.g. FF12 x size tercile).
4. `ts_resid_on(y, x1[, x2[, x3]], w)`: residual of a trailing-window regression of y on up to 3 regressors + intercept
   (FF3-style residual momentum); `ts_beta_on(y, x, w)` the slope.
5. `cs_residualize(x, c1..c4)`: cross-sectional residual on up to 4 covariates + intercept (today: 1 covariate).
6. Min-periods variants of the existing full-window ts ops (`ts_mean_mp(x, w, m)` etc. or a `min_periods` argument on
   the existing ops if the parser allows a clean optional argument; choose the smaller change and document it).
7. `ts_count_increases(x, w)`: number of consecutive trailing increases (nincr).

**Tests:** oracle vs VM vs streaming differential tests per op on random panels with NaN holes; bit-unchanged results for
every existing opcode (a golden-hash test over the existing op set on a fixed seed); typecheck refusals (Group-typed
arguments where a Vector is required and vice versa); the library checker accepts a synthetic library using each new
op and refuses unknown ops. Show, in the report, DSL strings for MAX5-SMAX, a BAC-quintile signal, an FF3 residual
momentum, nincr and q5 expected growth expressed with the new ops (do not add them to any library: that is a
pre-registered library revision for root).

**Root acceptance:** atx-engine alpha/eval test targets green; the golden hash unchanged; the v6.1 IC pass byte-identical
(identity on real data). Report: op table (name, signature, NaN rule, opcode id), file:line, test names.
