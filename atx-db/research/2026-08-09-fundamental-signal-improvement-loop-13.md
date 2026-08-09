# Fundamental signal improvement loop 13: cash-profitability tail and corrected distress

Date: 2026-08-09

## Research question

Is the non-linear low-cash-flow tail a value or distress exposure, and can a point-in-time control
separate that exposure from monotonic cash profitability?

## Primary-source basis

- Fama and French treat value and profitability as distinct return dimensions and evaluate them
  through dependent and independent portfolio sorts:
  <https://www.sciencedirect.com/science/article/pii/S0304405X14002323>.
- Piotroski applies financial strength specifically within high book-to-market firms, motivating a
  profitability-within-value test rather than a naïve unconditional blend:
  <https://papers.ssrn.com/sol3/papers.cfm?abstract_id=249455>.
- Campbell, Hilscher, and Szilagyi find that financially distressed stocks have anomalously low,
  not high, returns despite large risk exposures. Distress therefore needs to be measured rather
  than inferred from book-to-market alone:
  <https://onlinelibrary.wiley.com/doi/10.1111/j.1540-6261.2008.01416.x>.

## Control audit

Among the intended controls, only `value_book_to_market` had live production breadth: 99,300 rows,
956 securities, and 176 dates. Leverage, Piotroski, and Altman existed only as empty governed
definitions.

The declared Altman definition was not materializable:

- it depended on `total_debt`, which has zero canonical statement rows;
- its implementation used market equity divided by total debt;
- the classic public-company Altman leg uses market equity divided by total liabilities.

The corrected components have sufficient coverage: assets cover 1,484 securities; current assets
and liabilities about 1,184; retained earnings 1,414; total liabilities 1,192; and operating income
TTM 1,143.

## Value-controlled cash profitability

Cash-flow profitability and book-to-market have mean cross-sectional rank correlation -0.472 on
74,413 common keys. Cash profitability retains positive IC after controlling for value:

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Cash, common cohort | 0.0225 / 2.96 | 0.0325 / 2.68 | 0.0397 / 2.46 | 0.0562 / 3.20 |
| Cash residualized on B/M rank | 0.0183 / 2.82 | 0.0212 / 2.20 | 0.0248 / 1.93 | 0.0397 / 2.54 |
| Cash ranked within B/M quintile | 0.0175 / 2.66 | 0.0190 / 1.96 | 0.0230 / 1.81 | 0.0362 / 2.33 |

Value explains part of the rank effect but does not restore consistently positive long-short
spreads. Neither controlled transform is productionized.

## Corrected Altman build

Added `atx_db.altman_distress`, migration `0209`, a standalone CLI, and live values for the existing
`distress_altman_z_score` identifier. Migration `0209` replaces the broken definition and dependency
graph.

The formula is:

```text
1.2 * (current assets - current liabilities) / assets
+ 1.4 * retained earnings / assets
+ 3.3 * TTM operating income / assets
+ 0.6 * market capitalization / total liabilities
+ TTM revenue / assets
```

The production loader:

- uses the independently governed cash-flow-profitability month/asset scaffold;
- uses the exact-date PIT book-to-market row only for its already audited market-cap lineage;
- selects USD statement and TTM revisions visible at the later upstream decision timestamp;
- requires all classic components and positive assets, liabilities, and market capitalization;
- caps absolute raw Z-scores at 100 before 1% winsorization and cross-sectional z-scoring;
- records both upstream factor IDs and every statement/TTM input ID and availability time.

Live output: 30,875 rows, 441 securities, 173 dates, 2012-04-30 through 2026-06-15.

## Analysis

Run id: `loop13-altman-distress-production`.

| Horizon | Mean rank IC | HAC t-stat | Spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.02503 | 2.30 | 0.320% | 52.9% | 0.067 |
| 63d | 0.04274 | 2.44 | 0.611% | 51.8% | 0.018 |
| 126d | 0.04962 | 2.14 | 0.797% | 59.4% | 0.006 |
| 252d | 0.06890 | 2.39 | 2.616% | 56.0% | 0.030 |

Altman ranks are extremely persistent: top/bottom turnover is 16.9%/21.7% and mean rank
autocorrelation is 0.990. All rows are finite, unique, and available no later than their decision
date.

### Subperiod stability

| Period | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0148 / 0.86 | 0.0088 / 0.35 | 0.0026 / 0.12 | 0.0221 / 0.71 |
| 2016-2020 | 0.0392 / 2.01 | 0.0733 / 2.14 | 0.0865 / 1.75 | 0.1303 / 2.56 |
| 2021-2026 | 0.0188 / 1.04 | 0.0373 / 1.59 | 0.0473 / 1.57 | 0.0380 / 1.10 |

Every subperiod/horizon sign is positive. The effect is strongest in 2016-2020 but remains
directionally stable on both sides.

## Cash-tail interpretation

Altman and cash-flow profitability are 0.600 rank-correlated on their complete-case cohort. Cash
ranked within Altman quintiles or residualized on Altman remains directionally positive but loses
inference and does not repair decile monotonicity. The low-cash tail is therefore not cleanly
explained by either value or corrected Altman distress in the available complete-case sample.

The appropriate product response is to expose signed cash profitability and Altman separately,
not encode an ex-post nonlinear transform.

## Router allocation

Altman has only 0.087 mean rank correlation with the broad router, but sparse overlays do not
improve the full panel:

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Router | 0.02045 / 3.32 | 0.03316 / 4.40 | 0.04800 / 5.05 | 0.06791 / 6.44 |
| Router + 10% Altman where available | 0.01993 / 3.31 | 0.03285 / 4.20 | 0.04742 / 4.61 | 0.06706 / 6.09 |
| Router + 25% Altman where available | 0.02004 / 3.28 | 0.03338 / 4.10 | 0.04802 / 4.48 | 0.06782 / 6.06 |

The narrow complete-case cohort already has stronger router IC than the broad panel. Overlaying
Altman changes ranks within that selected cohort but does not improve the unconditional router.

## Decision

Promote the corrected `distress_altman_z_score` as a production-quality standalone distress and
financial-strength feature. Do not use the legacy total-debt formula, do not materialize a
value- or Altman-neutralized cash transform, and do not alter the broad router.

The cash tail remains explicitly non-linear and unresolved; clients can jointly query signed cash
profitability, book-to-market, and corrected Altman without hidden imputation or thresholding.

## Verification

- Corrected-Altman targeted tests: 2 passed serially.
- New files: Ruff and Python compilation clean.
- Schema `0209`, eight dependencies, migration checksums, checkpoint, duplicate, availability, and
  finiteness checks: passed.
- The scoped `atx-db` diff check passes. The repository-wide check is independently blocked by
  pre-existing conflict markers in the sibling `atx-vol` worktree, which this loop did not modify.
- Full-suite execution was intentionally avoided.

## Next loop

Materialize a point-in-time Piotroski financial-strength score and test it only within high
book-to-market names, matching the paper's conditional design. This also closes another governed
definition with no live values and provides a discrete complement to continuous Altman strength.
