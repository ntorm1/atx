# Task T5 — DSL alpha library v2 (96 candidates: frozen v1 48 + 48 new diversifying price/volume)

Owner worktree: `C:/atx-wt/pool-7`, branch `feat/mega-alpha-library-v2-20260926` (base = root HEAD at
dispatch). New files only under `atx-impl/strategies/`:
`generate_price_volume_ic96_v2.py`, `price_volume_ic96_v2.json`, `price_volume_ic96_v2.recipe.json`.
Do not edit v1 files or any C++.

## Why
The fixed 48-candidate v1 blend (`slow_price_volume_ic48_v1.json`, generator
`generate_slow_ic_48.py` — read both first and mirror their structure/determinism/`--check`) is
dominated by correlated "winner + low-risk" tilts (momentum, trend quality, price location, low vol).
Measured on TRAIN 2020-2022 it gives only ~0.36-0.55 annualized gross ratio. The objective (net Sharpe
>= 1 at <= 30%/month turnover, ~3000 names) needs DIVERSIFYING slow sources. Families must be chosen
from economic/literature priors BEFORE any measurement; TRAIN later fits signs; validation is not used
for selection.

## Requirements
1. Library v2 = the 48 v1 candidates copied EXACTLY (same ids, family names, DSL text byte-for-byte, so
   their `dsl_sha256` values are identical and cached signals are reused) + 48 new candidates:
   8 new families x 3 templates x 2 slow variants, using the same wrapper as v1
   (`decay_linear(rank(base), s)` with s in {21, 63}) unless a family is already smooth (say why).
2. Hard constraints: only fields `close` (adjusted), `raw_close`, `volume`; total prior-bar lookback of
   every expression <= 314 (the generator must compute and assert it, as v1 does; the runner verifies
   natively too); no future references; ids unique; deterministic bytes; `--check` mode.
3. The cross-sectional market return is `vec_avg(ret)` (member-masked cross-section mean), where daily
   return ret = close/delay(close,1) - 1. Useful ops available (see `atx-engine/src/alpha/registry.cpp`):
   `ts_regression(y,x,d)` (rolling OLS slope), `ts_corr(x,y,d)`, `ts_std`, `ts_mean`, `ts_sum`,
   `ts_max`, `ts_min`, `ts_skew`, `abs`, `log`, `delay`, `delta`, `rank`, `decay_linear`, `vec_avg`,
   `signedpower`, `winsorize`. Confirm exact signatures/arity in the registry and parser before use.
4. New families (economic priors; pick 3 concrete templates each, prefer distinct horizons):
   - residual_momentum: cumulative beta-adjusted residual return (ret - beta*mkt, beta from a trailing
     rolling regression), e.g. over ~126/189 sessions with a ~21-session skip; residual "Sharpe".
   - market_beta: betting-against-beta style trailing beta (e.g. 126/189) and downside beta.
   - idiosyncratic_risk: trailing std of residual returns; idio share of total variance.
   - lottery_demand: max daily return over 21/63 sessions; return skewness over 63/126.
   - abnormal_volume: short-window vs long-window volume/dollar-volume shocks that are NOT the v1
     `dollar_expansion_21_126` / `dollar_instability_63` definitions (check v1 to avoid duplicates).
   - illiquidity: Amihud |ret| / (raw_close*volume) averaged over 21/63/126 sessions.
   - comovement: trailing correlation / R-squared with the market return.
   - volatility_dynamics: volatility-of-volatility or short/long realized-volatility term structure
     not duplicating v1 `vol_expansion_21_126`.
   If any template's DSL is not expressible, substitute the closest expressible one and record why.
5. Recipe: mirror v1's recipe schema; composition rule for v2 = fixed family-equal weights
   (1/16 per family, 1/6 within family = 1/96 per candidate) with TRAIN 21-session sign fitting;
   record `validation_selection: false`, the per-candidate lineage (family, template, smoothing,
   prior_bars, dsl_sha256), and a statement that families were fixed before measurement.
6. Validate syntax without the C++ runtime as far as possible (e.g. a small Python parenthesis/arity
   check against the registry arities); root will run the native `--plan-only` compile check and send
   back any parse/typecheck error.

## Constraints
- No compile/run of C++; no market data reads. No subagents. Commit in pool-7 (messages end with
  `Co-Authored-By: Claude Opus 5.5 (1M context) <noreply@anthropic.com>`). No push.

## Report
Full report to `C:/atx-wt/pool-2/.superpowers/sdd/mega-alpha-20260926/task-T5-report.md` (family
rationale with one-line literature prior each, DSL of each new template, lookbacks, file SHA256s,
commit SHA). Return only: status, commit SHA, one-line summary, concerns.
