# Fundamental signal improvement loop 55: net operating assets

Status: production feature retained; **rejected from mega-alpha**.

## Research hypothesis

Hirshleifer, Hou, Teoh, and Zhang report that high net operating assets (NOA),
the cumulative gap between accounting and cash value added, negatively predict
future returns. Their construction scales NOA by beginning assets and interprets
the effect as weak sustainability of accounting profitability
(https://doi.org/10.1016/j.jacceco.2004.10.002). Follow-up evidence decomposes
the effect into working and investing components and finds that the negative
relation is concentrated in asset-side NOA components
(https://doi.org/10.1016/j.irfa.2011.06.001).

The frozen candidate is the higher-is-better negative financing-side identity:

`-((total_assets - cash_st_inv - total_liabilities + st_debt + lt_debt) /
prior_total_assets)`.

Every current component must come from the same accession; the denominator must
be the consecutive 300-430-day prior annual period; every value must be visible
at the governed month end; and no missing component is imputed. The warehouse's
canonical `st_debt` is a prioritized current-debt concept rather than a sum of
every possible disclosed instrument, and this standardization limitation is
carried in every row's lineage.

The candidate advances only with positive IC at all 21/63/126/252-day horizons,
HAC t-statistics of at least 2.0 at two horizons including 126 or 252 days,
positive coherent long-horizon tails, and no material 2021+ long-horizon sign
reversal. Any costed test remains subject to every existing mega-alpha gate.

## Production implementation

Added `atx_db.net_operating_assets`, build command
`scripts/build_net_operating_assets.py`, focused calculation/governance tests,
and applied append-only migration `0242`. The factor definition records all
five current metrics, the prior-asset denominator, the point-in-time policy,
the debt standardization limitation, and the no-imputation/no-return-fitting
contract.

The live partition contains 30,562 rows over 372 securities and 173 monthly
formation dates from 2012-04-30 through 2026-06-15. Full feature construction
completed atomically, although the command returned after the 45-second shell
cap; partition inspection confirmed the all-or-nothing write.

## Full-history evidence

Run IDs: `loop55-net-operating-assets-screen` and
`loop55-net-operating-assets-full`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0165 | 2.98 | 1.168% | 57.1% | 0.588 |
| 63d | 0.0198 | 2.51 | 2.805% | 62.5% | 0.648 |
| 126d | 0.0230 | 2.16 | 7.941% | 59.4% | 0.673 |
| 252d | 0.0391 | 2.49 | 23.313% | 66.0% | 0.576 |

The full-history signal is strong and directionally coherent. Mean breadth is
about 175 names. Top/bottom-decile turnover is 12.4%/16.8%, and mean monthly
rank autocorrelation is 0.981 across 172 rebalance pairs.

## Modern stability failure

The 2021+ screen (`loop55-net-operating-assets-modern`) is materially weaker:

| Horizon | Rank IC | HAC t-stat |
|---:|---:|---:|
| 21d | 0.0131 | 1.32 |
| 63d | 0.0018 | 0.13 |
| 126d | -0.0065 | -0.39 |
| 252d | -0.0080 | -0.41 |

Both long horizons reverse sign. This independently fails the frozen modern
stability rule and shows that much of the attractive full-history decay is not
currently persistent.

## Costed mega-alpha decision

The costed Polars run used $50 million AUM, 21-day holding periods, nine
expanding 60/12/12 folds with one-period embargo, 32 declared trials, 20%
candidate allocation, and the existing commission, spread, impact, borrow,
participation, gross, and name-cap settings.

- candidate Sharpe 0.5939, annualized return 2.532%, turnover 0.0983, and
  drawdown -7.33%;
- router-v6 Sharpe 0.4418;
- 80/20 blend Sharpe 0.6528, improvement +0.2111, doubled-cost Sharpe 0.5491;
- candidate/router return correlation -0.163;
- blend minimum gross deployment 1.000 and maximum participation 0.0820;
- candidate deflated-Sharpe probability 0.3740 versus the required 0.95.

The standalone candidate's maximum participation is 0.355, while the actual
20% blend remains inside the 0.10 execution ceiling. The immutable artifact is
`C:\atx\atx-factor\research\loop55-net-operating-assets-decision.json`, evidence
digest `7e5635a962866173306a7c791bde59ba0ccfeeb7cd363a34ead7d9cd9a8bbf61`.

Decision: **reject `quality_net_operating_assets` from mega-alpha**. The
deflated-Sharpe gate fails, and the 2021+ long-horizon reversal is an independent
research rejection. Router v6 and the registry remain unchanged. The feature
partition is retained as governed research data because its full-history
predictive evidence and complete lineage are useful to downstream users.

## Self-improvement and verification

Loop 54's replacement evaluator now short-circuits and serializes primary-gate
rejections before computing incumbent comparison or doubled-cost stress; four
focused tests pass. Three focused NOA calculation/governance tests pass. Ruff is
clean on every touched source, script, migration, and test file. Migration
checksums validate at schema version 0242 (216 numeric migrations). No full test
suite was run.

This loop exposed a staging defect: the modern-regime check ran after the
costed evaluation. Future loops must run full-history inference, long-horizon
tails, and modern stability before invoking `atx-factor`.
