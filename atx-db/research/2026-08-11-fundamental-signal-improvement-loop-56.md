# Fundamental signal improvement loop 56: comprehensive external financing

Status: production feature retained; **rejected upstream from mega-alpha**.

## Research hypothesis

Bradshaw, Richardson, and Sloan construct a comprehensive statement-of-cash-
flows measure of net corporate financing and report a negative relation with
future profitability and returns; their long-short strategy averaged 15.5% per
year in the original sample
(https://doi.org/10.1016/j.jacceco.2006.03.004). Cohen and Lys subsequently show
that the return relation attenuates and becomes insignificant after controlling
for total accruals, a relevant warning against assuming the anomaly is distinct
(https://doi.org/10.1016/j.jacceco.2006.04.006).

The frozen factor is:

`-annual_financing_cash_flow / prior_total_assets`.

Positive financing cash flow represents net capital raising and receives a
negative score; distributions receive a positive score. The annual flow must be
an exact-accession 330-400-day duration, beginning assets must be positive and
visible, and no missing item is imputed. No return-fitted parameter is allowed.

The staged gate requires positive IC at 21/63/126/252 days, HAC t-statistics of
at least 2.0 at two horizons including 126 or 252 days, and positive coherent
long-horizon Q10-Q1 tails. Only then may the signal reach a 2021+ stability test;
only a stable signal may reach the $50 million Polars costed test.

## Production implementation

Added `atx_db.external_financing`, build command
`scripts/build_external_financing.py`, two focused calculation/governance tests,
and applied append-only migration `0243`.

For performance, the feature reuses the governed
`investment_conservative_asset_growth` rows for formation dates, exact annual
accessions, and prior assets, then exact-accession joins
`financing_cash_flow`. This preserves point-in-time lineage while avoiding a
second universe and price reconstruction. The live build completed in 26
seconds and produced 99,589 rows across 923 securities and 175 monthly dates
from 2012-04-30 through 2026-06-15.

## Full-history evidence

Run IDs: `loop56-external-financing-screen` and
`loop56-external-financing-full`.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0143 | 2.12 | -0.214% | 52.6% | -0.236 |
| 63d | 0.0238 | 2.80 | -0.062% | 50.3% | 0.164 |
| 126d | 0.0354 | 3.31 | -0.106% | 50.0% | 0.176 |
| 252d | 0.0435 | 2.68 | -1.123% | 47.5% | 0.079 |

Average monthly breadth is 553-573 names. Top/bottom-decile turnover is
14.7%/18.6%, with mean rank autocorrelation 0.954 across 174 rebalance pairs.

The average monthly rank association is statistically strong, but the
investable tails contradict it: Q10 underperforms Q1 at every horizon, every
spread is negative, hit rates are approximately random, and the pooled decile
ladders are non-monotonic. This indicates the IC is carried by non-tail or
time-weighting structure and does not support the frozen long-short portfolio.

## Decision

**Reject `financing_low_external_financing` from mega-alpha.**

The long-horizon tail gate fails before modern-regime or transaction-cost
testing. In accordance with the staged contract, no 2021+ screen and no $50
million `atx-factor` run were performed, so there is intentionally no costed
decision artifact. Router v6 and the mega-alpha registry remain unchanged.

The governed feature partition is retained for downstream research and for
future conditional/accrual-controlled studies; retaining a reproducible failed
experiment prevents rediscovery of the favorable IC alone while omitting its
negative tails.

## Verification and process improvement

- Two focused calculation/governance tests pass.
- Ruff passes on the feature, builder, migration, registry, and tests.
- Migration checksums validate at schema version 0243 with 217 numeric
  migrations.
- No full suite was run.

Loop 56 is the first loop after the Loop 55 staging defect to stop at the first
failed upstream gate. It avoided both the modern-regime query and all Polars
portfolio passes, preserving the requested fast research cadence.
