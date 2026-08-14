# Fundamental signal improvement loop 66: profitability and conservative investment

Status: production feature added; **rejected upstream from mega-alpha**.

## Research and frozen construction

Fama and French link expected returns jointly to expected profitability and
investment: controlling for investment and value, more profitable firms have higher
expected returns; the investment dimension supplies separate information
(https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/acrobat/Profitability%20Growth%20and%20Average%20Returns_2005_06.pdf).
Hou, Xue, and Zhang's investment-CAPM implementation likewise uses profitability
and investment factors to absorb a broad anomaly set
(https://theinvestmentcapm.com/uploads/1/2/2/6/122679606/houxuezhang2015rfs.pdf).
Later evidence finds that expected-profitability measures based on Ball-style cash
profitability have the strongest pricing power among tested profit definitions
(https://doi.org/10.1016/j.econlet.2019.108547).

The local audit found no factor that directly combines cash operating profitability
with conservative asset growth. Existing q5 expected-growth features use those
inputs to forecast future investment growth, which is a different estimand. The two
production inputs intersect on 66,048 security-months across 740 securities and 175
dates. Their pooled value correlation is only -0.13, confirming that the investment
leg is not a duplicate of profitability.

The candidate was frozen before return inspection as an untuned equal-weight blend:

```text
raw score = 0.5 * z(cash operating profitability)
          + 0.5 * z(negative annual asset growth)
```

Both parent values must be governed PIT observations for the same security and
formation date. Weights are not fitted to returns. The blend is winsorized 1% per
tail and re-standardized cross-sectionally. Higher profitability and more
conservative investment are the preregistered preferred directions.

The first gate requires positive IC at all 21/63/126/252-day horizons and HAC
t-statistics of at least 2.0 at two horizons including 126 or 252 days. Failure stops
all tail, stability, incremental, and capacity work.

## Production implementation

Added `atx_db.profitability_investment`,
`scripts/build_profitability_investment.py`, focused formula/orientation/governance
tests, and append-only migration `0253`. Lineage preserves each parent factor value
ID, raw and standardized value, source, availability timestamp, fixed weight, and
the composite decision timestamp.

The production build completed with 66,048 observations across 740 securities and
175 rebalance dates from 2012-04-30 through 2026-06-15.

## Evidence and attribution

Run ID: `loop66-profitability-investment-screen`.

| Horizon | Rank IC | HAC t-stat | Dates | Mean names |
|---:|---:|---:|---:|---:|
| 21d | -0.0011 | -0.17 | 171 | 378.7 |
| 63d | -0.0090 | -1.28 | 169 | 375.5 |
| 126d | -0.0120 | -1.31 | 166 | 370.6 |
| 252d | -0.0150 | -1.47 | 160 | 360.0 |

The composite is slightly negative at one month and becomes increasingly negative
through one year. It fails the sign condition at every horizon and has no positive
HAC support.

A read-only attribution check against the latest stored parent screens explains the
economic failure. Cash profitability remains positive at all horizons (IC 0.0204,
0.0249, 0.0258, and 0.0254), while conservative asset growth is negative at all
horizons (-0.0088, -0.0148, -0.0166, and -0.0185). The published low-investment
direction does not hold in the ATX period, and adding that leg destroys rather than
diversifies the cash-profitability signal. The weights and investment direction
cannot be changed after seeing the candidate's returns.

Decision: **reject `composite_cash_profitability_conservative_investment` from
mega-alpha**. No full tail run, 2021+ stability run, incremental router challenge,
or $50 million Polars capacity test was performed. There is intentionally no costed
decision artifact. Router v6 and the mega-alpha registry remain unchanged.

## Verification

- Three focused blend, orientation, lineage, fixed-weight, and governance tests pass
  in 1.98 seconds.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate through schema version 0253 with 227 applied numeric
  migrations.
- `git diff --check` is clean for the loop files.
- No full suite was run.
