# Fundamental signal improvement loop 07: conditional profitability / financing router

Date: 2026-08-09

## Research question

Can the new net-share-issuance factor improve the operating-profitability leader without the
signal dilution observed in Loop 06's equal-weight blend?

The production candidate uses a hard availability route:

1. use operating profitability for a security/date when a finite point-in-time score exists;
2. otherwise use low net share issuance;
3. standardize the selected scores across the expanded date cohort.

This is a coverage router, not an average. It never replaces or dilutes operating profitability on
their common cohort.

## Primary-source research

Loop 07 began by researching total and net payout yield as the next financing signal.

- Boudoukh, Michaely, Richardson, and Roberts find that payout yield (dividends plus repurchases)
  and net payout yield (dividends plus repurchases less equity issuance) contain cross-sectional
  expected-return information beyond dividend yield. They also report a priced high-minus-low
  payout-yield factor:
  <https://onlinelibrary.wiley.com/doi/10.1111/j.1540-6261.2007.01226.x>.
- The corresponding NBER paper reports economically and statistically significant predictability
  at short and long horizons and robustness to payout-measure choice:
  <https://www.nber.org/papers/w10651>.
- The SEC's share-repurchase taxonomy guide defines the structured disclosure surface available
  for issuer repurchase activity:
  <https://xbrl.sec.gov/shr/2023/shr-taxonomy-guide-2023-09-18.pdf>.

The research supports adding repurchases and issuances rather than treating dividend yield as the
complete payout channel. The live audit, however, showed that a no-new-input router could first
capture a larger and cleaner improvement.

## Live payout and coverage audit

| Surface | Rows | Coverage |
|---|---:|---:|
| `shares_outstanding_history` | 186,251 | 1,531 securities |
| TTM share repurchases | 87,151 | 1,154 securities |
| TTM dividends paid | 37,840 | 463 securities |
| Raw `ProceedsFromIssuanceOfCommonStock` facts | 51,467 | 917 filers |
| Normalized TTM stock issuance | 0 | not yet activated |
| Corporate-action dividend metrics | 140 | 36 securities, 2012-2013 only |

The raw cash-flow inputs are promising, but total net payout cannot yet be built honestly from the
current TTM layer: stock-issuance facts are mapped but not normalized, and the event-level dividend
surface is a small historical slice. Missing values must not be interpreted as zero cash flow.

The existing factor overlap offered a higher-confidence opportunity:

| Availability by security/date | Keys | Securities |
|---|---:|---:|
| Operating profitability and issuance | 68,590 | 898 |
| Operating profitability only | 2,005 | 115 |
| Issuance only | 44,089 | 668 |

## Prototype gate

The issuance-only cohort is predictive rather than merely additional coverage:

| Horizon | Mean rank IC | HAC t-stat | Mean names |
|---:|---:|---:|---:|
| 21d | 0.03083 | 3.16 | 256 |
| 63d | 0.04772 | 4.15 | 257 |
| 126d | 0.06742 | 4.85 | 259 |
| 252d | 0.09226 | 4.89 | 263 |

The conditional prototype therefore passed the build gate. This differs fundamentally from the
rejected Loop 06 composite: the rejected model averaged two scores on the common cohort and reduced
IC at every horizon; the router adds issuance only where operating profitability has no score.

## Production build

Added `composite_operating_profitability_or_net_issuance` with:

- deterministic primary-else-fallback routing at `(security_id, as_of_date)`;
- finite/latest-revision filtering and deterministic duplicate resolution;
- selected-input `available_at` propagation;
- cross-sectional z-scoring without a second winsorization that could alter prototype ranks;
- nested lineage identifying the selected upstream factor value and route;
- date-scoped idempotent refresh semantics;
- explicit factor dependency edges for the primary and fallback inputs;
- migration `0200` and a standalone build CLI.

Live build `loop7-conditional-router-build` produced:

| Metric | Result |
|---|---:|
| Stored rows | 132,233 |
| Securities | 1,296 |
| Rebalance dates | 176 |
| Date range | 2012-03-30 to 2026-06-15 |
| Primary-routed rows | 70,961 |
| Fallback-routed rows | 61,272 |
| Duplicate security/date keys | 0 |
| Non-finite values | 0 |

## Production evaluation

Run id: `loop7-conditional-router-production`.

| Horizon | Router IC | Router HAC | OP IC | OP HAC | Router mean names | OP mean names |
|---:|---:|---:|---:|---:|---:|---:|
| 21d | 0.02219 | 3.57 | 0.02061 | 2.83 | 660 | 404 |
| 63d | 0.03509 | 4.50 | 0.03054 | 3.00 | 657 | 400 |
| 126d | 0.05171 | 5.37 | 0.04350 | 3.45 | 653 | 394 |
| 252d | 0.07088 | 6.93 | 0.05603 | 3.46 | 643 | 380 |

The router beats the prior leader at every horizon while increasing average evaluable breadth by
63% to 69%.

| Horizon | Top-minus-bottom spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|
| 21d | 0.357% | 51.5% | 0.576 |
| 63d | 1.259% | 61.5% | 0.624 |
| 126d | 2.981% | 63.3% | 0.624 |
| 252d | 8.262% | 74.4% | 0.697 |

Top- and bottom-decile turnover are 16.2% and 19.2%; mean rank autocorrelation is 0.971.

## Subperiod stability

| Period | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0138 / 1.30 | 0.0279 / 2.63 | 0.0467 / 6.02 | 0.0737 / 19.60 |
| 2016-2020 | 0.0218 / 2.05 | 0.0291 / 2.48 | 0.0386 / 2.79 | 0.0481 / 3.90 |
| 2021-2026 | 0.0285 / 2.76 | 0.0463 / 2.92 | 0.0693 / 3.28 | 0.0952 / 4.67 |

The IC sign is positive at all four horizons in every period. The early 252-day HAC statistic is
inflated by the short, overlapping sample and should not be interpreted literally, but the result
does not depend on that period: the middle and recent samples remain independently positive.

## Decision

Promote `composite_operating_profitability_or_net_issuance` to the leading broad production
fundamental factor. Retain the two inputs as separately queryable factors and preserve operating
profitability as the primary route.

This promotion is provisional with respect to the unresolved Loop 05 controls. It uses adjusted
price returns because the warehouse still lacks observed historical delisting returns, and current
SEC classifications cannot be leaked backward for historical FF12 neutralization.

## Verification

- Targeted conditional-router tests: 2 passed.
- New router files: Ruff and Python compilation clean.
- Migration checksum verification: passed through schema `0200`.
- Live key, finiteness, dependency, route, and idempotence checks: passed.
- Full-suite execution was intentionally avoided.

## Next loop

Activate `stock_issuance` in `fundamental_statement_points` and `fundamental_ttm_points`, then build
cash-flow net payout yield as `(dividends + repurchases - issuance) / market_cap`. Missing payout
components require an explicit observation/policy model; they must not be silently coalesced to
zero. Compare net payout against the current router on equal and incremental cohorts before any
blend or replacement.
