# Fundamental signal improvement loop 31: change in quarterly operating profitability

Date: 2026-08-10

## Research hypothesis

The trajectory of profitability can contain information beyond its current level. Akbas, Jiang,
and Koch find that a recent profit trend predicts future profitability and returns and is not
subsumed by profitability level or earnings momentum. Lim et al. similarly connect profitability
growth to future returns. Loop 31 tested several point-in-time quarterly constructions before
governing the broadest viable operating-profitability change feature.

Primary references:

- [Akbas, Jiang, and Koch, *The Trend in Firm Profitability and the Cross Section of Stock Returns*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2538867)
- [Lim et al., *The Value of Growth: Changes in Profitability and Future Stock Returns*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2819183)
- [Novy-Marx, *The Other Side of Value: Good Growth and the Gross Profitability Premium*](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1598056)

## Pre-registration funnel

Three constructions were evaluated in memory before code was added:

1. Same-quarter gross-profit growth retained 32,419 rows and had positive IC, but its
   63/126/252-day Q10-Q1 spreads were -0.61%/-1.93%/-8.73%. It was rejected.
2. The published eight-quarter seasonal trend regression was kept exact rather than weakened for
   coverage. ATX's current continuous quarterly history produced only 607 monthly rows across 31
   securities. IC was negative at every horizon, including -0.141/-0.139 at 126/252 days. It was
   rejected.
3. Continuous year-over-year gross-margin change retained 32,960 rows, but full-history HAC
   t-stats were only 1.10/0.58/0.64/0.13 and its long-horizon tails reversed. It was rejected.

The selected construction is the same-quarter year-over-year change in governed quarterly
operating profitability. It retained all 33,088 exact revenue-growth pairs and produced positive
rank IC at every horizon. The feature is useful as a low-correlation diagnostic despite weak
full-history tails; those tails explicitly prevent portfolio promotion.

## Point-in-time implementation

Added `atx_db.quarterly_profitability_change`, migration `0234`, a standalone CLI, and three
focused tests. The governed factor is
`profitability_quarterly_operating_profitability_change_yoy`, sourced as
`atx-db PIT quarterly operating profitability change v1`:

`zscore(winsorize_1pct(qop_t - qop_t_4))`.

The governed quarterly revenue-growth feature supplies the exact same-quarter pair and 330-400 day
period contract. The loader resolves both recorded quarterly-operating-profitability factor-value
IDs and ranks their raw ratio difference. It rejects non-finite changes and absolute changes above
10, winsorizes 1% per monthly cross-section, and sample-zscores the result. `available_at` is the
maximum of the pair record and both QOP rows; no return-fitted parameter is used.

The live build exactly matched pre-registration: 33,088 rows across 439 securities and 161 dates
from 2013-04-30 through 2026-06-15. It completed in 33 seconds.

## Standalone analysis

Run id: `loop31-quarterly-profitability-change-production-evaluation`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0129 | 1.49 | -0.157% | 54.1% | 0.042 |
| 63d | 0.0142 | 1.16 | 0.100% | 55.4% | -0.164 |
| 126d | 0.0182 | 0.98 | -2.550% | 48.7% | -0.285 |
| 252d | 0.0137 | 0.55 | -6.913% | 54.1% | -0.224 |

Top/bottom turnover is 36.2%/37.2%, mean rank autocorrelation is 0.883, and there are 160
rebalance pairs. The factor is positive in mean IC but not a standalone long-short portfolio: the
extreme tails and monotonicity contradict the average rank relationship.

### Modern-regime evidence

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2021-2026 | 0.0215 | 0.0215 | 0.0244 | 0.0365 |
| 2023-2026 | 0.0329 | 0.0351 | 0.0526 | 0.0772 |

The 2023+ HAC t-stats are 2.24/2.56/3.10/4.93. That strong recent slice is useful for monitoring,
but it does not override weak full-history inference. Mean correlations are 0.393 with SUE, 0.426
with revenue/margin confirmation, 0.371 with direct revenue growth, 0.189 with quarterly operating
profitability level, 0.139 with quarterly gross profitability, and only 0.024 with production router
v6.

## Router and mega-alpha decision

The candidate changed only ordering inside production primary deciles and supplied a secondary key
on 32,915 of the router's unchanged 114,684 rows.

| Secondary key | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|---:|
| Cash profitability (production v6) | 0.02281 | 0.03653 | 0.05306 | 0.07247 |
| Profitability change | 0.02244 | 0.03566 | 0.05230 | 0.07164 |

The candidate trails v6 at every full-history horizon. Its 21/63/126/252-day router spreads are
0.349%, 1.250%, 2.978%, and 8.275%, also below v6. The 2023+ comparison is mixed rather than
dominant. No router migration was made.

The separate `atx-factor` Polars engine subsequently ran the required costed admission test at $100
million AUM. Across eight disjoint 12-month folds (96 OOS observations), the candidate produced a
-0.429 Sharpe, -2.202% annual return, -27.888% maximum drawdown, and 0.000224 deflated-Sharpe
probability. Router v6 produced a 0.629 Sharpe. An 80/20 router/candidate blend fell to 0.449 Sharpe,
a marginal change of -0.180; doubled costs reduced it further to 0.357. The blend also exceeded the
10% participation ceiling and reached only 83.8% minimum gross deployment.

Decision: **do not accept into the mega-alpha portfolio**. It fails standalone Sharpe, DSR,
marginal improvement, participation, and deployment gates. No registry mutation was made. Keep the
factor in the governed feature catalog for monitoring, conditional research, and future nonlinear
combinations. The immutable evidence artifact is
`C:\atx\atx-factor\research\loop31-profitability-change-mega-alpha-decision.json`, digest
`9bf020c09af3b432b92c3ec6e9d79bedfc079f85390412905dde365cc7c0631e`.

## Verification

- Two pure tests and the single migration-governance test pass; the full suite was intentionally
  not run.
- The live partition has 33,088 unique IDs and natural keys, no null/non-finite/future-knowledge
  rows, valid JSON lineage, exact monthly sample z-score normalization, breadth of 23-372, and
  maximum lineage of 1,083 bytes.
- The live schema is `0234` with 208 checksummed migrations. Migration checksums, schema drift,
  schema-contract v2 pin, and checkpoint pass.
- Ruff, compilation, and changed-tree whitespace checks pass for the implementation slice.

## Next loop

The separate Polars engine is now live and loop 31 has passed through it with an explicit rejection.
Begin the next primary-research signal loop; every subsequent candidate must pass both governed
point-in-time factor analysis and the costed `atx-factor` mega-alpha gate before portfolio admission.
