# Fundamental signal improvement loop 06: net share issuance

Date: 2026-08-09

## Research question

Does one-year, split-adjusted change in shares outstanding provide an independent, production-
usable financing signal for the governed US common-equity universe?

The tested factor is oriented so that a high value means low issuance or net repurchase activity:

`raw_value = -log(split_adjusted_shares_t / split_adjusted_shares_t_minus_1y)`

## Primary-source basis

- Pontiff and Woodgate report that post-1970 share issuance is a strong cross-sectional return
  predictor, with greater statistical significance than size, book-to-market, or momentum when
  those predictors are considered separately:
  <https://onlinelibrary.wiley.com/doi/10.1111/j.1540-6261.2008.01335.x>.
- Fama and French include net stock issues among the anomalies they dissect across size and
  book-to-market portfolios:
  <https://onlinelibrary.wiley.com/doi/10.1111/j.1540-6261.2008.01371.x>.
- The Federal Reserve defines net stock issues as the log change in split-adjusted shares and
  summarizes the empirical result that low net stock issues are followed by high returns:
  <https://www.federalreserve.gov/pubs/ifdp/2012/1070/ifdp1070.pdf>.
- The NBER comparison of repurchase and issuance measures distinguishes one-year net share
  issuance from five-year composite share issuance and notes that splits and stock dividends do
  not change the properly adjusted measure:
  <https://www.nber.org/system/files/working_papers/w24163/w24163.pdf>.

These sources support the signal direction and make split invariance a correctness requirement,
not an optional data-cleaning step.

## Input audit

The live warehouse already had enough point-in-time observations to build the factor:

| Input | Rows | Securities |
|---|---:|---:|
| `shares_outstanding_history` | 186,251 | 1,531 |
| `CommonStockSharesOutstanding` SEC facts | 112,956 | 1,202 filers |
| `EntityCommonStockSharesOutstanding` SEC facts | 79,228 | 1,499 filers |

The share history is primarily sourced from 10-Q and 10-K filings. All observation selection is
bounded by both fact `available_at` and the rebalance timestamp. A candidate prior observation
must use the same taxonomy and concept and be 300 to 430 calendar days old.

The price feed's `split_factor` also contains small dividend-like adjustments. To avoid treating
ordinary distributions as share-count changes, the cumulative share adjustment uses only material
factors at or below 0.80 or at or above 1.25. Market capitalization carries a stale reported share
count from its observation split basis to the decision-date split basis.

## Build

Added a vectorized production feature pipeline with:

- one SQL input scan and point-in-time observation pairing;
- month-end rebalances, with the last available session used for an incomplete current month;
- split-invariant share growth;
- cross-sectional 1%/99% winsorization and z-scoring;
- deterministic IDs, full input lineage, idempotent replacement, and build manifests;
- governed factor definition and dependency edges in migration `0199`;
- a CLI entry point at `scripts/build_net_issuance.py`.

The key correctness fixture proves that a pure 2-for-1 split has exactly zero net issuance while a
genuine 10% increase produces `log(1.1)` before the factor-direction sign is applied.

Live build `loop6-net-issuance-build` produced:

| Metric | Result |
|---|---:|
| Factor observations | 130,195 |
| Securities | 1,278 |
| Rebalance dates | 176 |
| Date range | 2012-03-30 to 2026-06-15 |
| Non-finite normalized values | 0 |
| Duplicate factor/security/date keys | 0 |
| Governed panel observations | 112,679 |
| Governed panel securities | 936 |

## Analysis

### Full governed universe

Run id: `loop6-net-issuance`.

| Horizon | Mean rank IC | HAC t-stat | Top-minus-bottom spread |
|---:|---:|---:|---:|
| 21d | 0.01637 | 2.25 | -0.048% |
| 63d | 0.02819 | 3.04 | 0.041% |
| 126d | 0.04473 | 3.78 | 1.214% |
| 252d | 0.06078 | 3.52 | 6.129% |

The factor is persistent rather than excessively reactive: average top- and bottom-decile turnover
are 22.1% and 20.8%, and mean rank autocorrelation is 0.961. The 252-day deciles are U-shaped, so
the positive long-horizon rank IC and endpoint spread do not establish a globally monotonic
portfolio response.

### Common cohort against operating profitability

The equal-sample comparison uses 68,590 factor/date/security keys, 898 securities, and 175 dates.

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Operating profitability | 0.0220 / 2.97 | 0.0323 / 3.07 | 0.0465 / 3.58 | 0.0603 / 3.64 |
| Low net issuance | 0.0071 / 0.89 | 0.0141 / 1.24 | 0.0249 / 1.68 | 0.0309 / 1.45 |
| Equal-weight composite | 0.0166 / 2.30 | 0.0263 / 2.74 | 0.0392 / 3.12 | 0.0470 / 2.48 |

Operating profitability wins at every horizon on the common cohort. The equal-weight composite
also loses to operating profitability at every horizon and has negative endpoint spreads, so it is
rejected. Net issuance is nevertheless genuinely distinct: its mean cross-sectional correlation
with operating profitability is 0.055 and mean absolute correlation is 0.071.

### Subperiod stability

| Factor / period | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Net issuance, 2012-2015 | 0.0179 / 1.44 | 0.0346 / 2.92 | 0.0485 / 4.09 | 0.0687 / 8.86 |
| Net issuance, 2016-2020 | -0.0014 / -0.12 | 0.0110 / 0.65 | 0.0338 / 1.75 | 0.0481 / 2.61 |
| Net issuance, 2021-2026 | 0.0325 / 2.61 | 0.0407 / 2.60 | 0.0533 / 2.28 | 0.0689 / 1.59 |
| Operating profitability, 2012-2015 | — | — | — | 0.0247 / 0.71 |
| Operating profitability, 2016-2020 | 0.0313 / 2.43 | — | — | 0.0547 / 2.76 |
| Operating profitability, 2021-2026 | 0.0235 / 2.01 | — | — | 0.0841 / 4.29 |

Net issuance keeps a positive 126- and 252-day sign in every period. Its short-horizon edge is
absent in 2016-2020, while operating profitability is weak in the early sample. That regime
difference is consistent with complementarity, but does not justify an unconditional linear blend.

## Decision

Accept `financing_low_net_share_issuance` as a production factor and as the first broad financing
sleeve. Do not displace operating profitability on names where both are available, and do not
promote the equal-weight composite.

The correct next ensemble experiment is conditional: preserve operating profitability where it is
available and measure whether net issuance improves breadth or outcomes in the missing-profitability
cohort. Any blend must beat operating profitability on an equal sample and pass turnover, subperiod,
and monotonicity gates before promotion.

## Verification

- Targeted tests: 6 passed.
- New feature files: Ruff clean.
- Migration checksum verification: passed through schema `0199`.
- Full-suite execution was intentionally avoided in favor of the affected feature and evaluator
  tests.

## Next loop

Test a conditional financing/profitability router before adding another correlated accounting
ratio. Separately research a total-payout or shareholder-yield feature that can add cash dividends
and repurchases to the issuance channel without double counting share-count changes.
