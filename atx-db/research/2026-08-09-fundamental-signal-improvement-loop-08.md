# Fundamental signal improvement loop 08: cash-flow net payout yield

Date: 2026-08-09

## Research question

Does strict cash-flow net payout yield add predictive power beyond the conditional operating-
profitability / net-issuance router?

The factor is:

`(-common_dividends_ttm - share_repurchases_ttm - stock_issuance_ttm) / market_cap`

The leading minus signs convert the warehouse's signed dividend and repurchase cash outflows to
positive payouts. Stock-issuance proceeds remain positive and are subtracted.

## Primary-source basis

- Boudoukh, Michaely, Richardson, and Roberts define net payout yield as common dividends plus
  common-share repurchases less common-share issuance, divided by market capitalization. Their
  sample requires data for the payout components rather than treating absent data as zero:
  <https://www.nber.org/papers/w10651.pdf>.
- The published study finds that total and net payout measures contain expected-return information
  beyond dividend yield:
  <https://onlinelibrary.wiley.com/doi/10.1111/j.1540-6261.2007.01226.x>.
- The SEC's structured share-repurchase disclosures provide an additional prospective source for
  validating issuer repurchase activity:
  <https://xbrl.sec.gov/shr/2023/shr-taxonomy-guide-2023-09-18.pdf>.

The production implementation therefore uses a strict complete-case policy. Dividends,
repurchases, and issuance must be present in the same filing for the same TTM end date. An absent
XBRL tag is not silently treated as zero.

## Data activation

Two mapped SEC concepts existed in raw companyfacts but had not been propagated through the
revision, statement, and TTM layers.

| Concept / metric | Revisions | Statement points | TTM points | TTM securities |
|---|---:|---:|---:|---:|
| `ProceedsFromIssuanceOfCommonStock` / `stock_issuance` | 51,467 | 50,959 | 31,982 | 601 |
| `PaymentsOfDividendsCommonStock` / `common_div_paid` | 73,446 | 72,068 | 61,083 | 597 |
| `PaymentsForRepurchaseOfCommonStock` / `share_repurchases` | — | — | 87,151 | 1,154 |

There are 6,118 strict same-filing TTM triplets across 148 securities. The complete-case policy is
correct but selects a narrow subset because firms with a true zero component often omit that tag.
Inferring zero requires a governed statement-completeness model and is deferred.

## Production performance incident and fix

The first stock-issuance activation exposed an operational flaw: `refresh_fundamental_ttm_points`
could scope neither its delete nor its source scan. A one-concept update attempted to rebuild all
1.5 million TTM rows, exceeded eight minutes, and held about 7 GB.

The worker was terminated after preserving the database and WAL. DuckDB then hit an internal WAL-
replay assertion. Recovery was performed only on a full byte-for-byte copy; quarantining the failed
WAL recovered schema `0200`, all 7,380,653 bars, 788,477 factor rows, and all 1,496,175 prior TTM
rows. The verified copy was moved into the live path and checkpointed before work resumed.

The TTM refresher now accepts `canonical_metrics`. Its delete and base statement scan are both
scoped to the requested metric. With real stock-issuance data, the scoped TTM stage completed in
12.9 seconds while preserving unrelated metrics; the full three-stage activation completed in
41.6 seconds. A targeted fixture proves that refreshing one metric removes/rebuilds only that
metric and leaves an unrelated TTM row intact.

## Build

Added `financing_net_payout_yield` with:

- same-security, same-accession, same-TTM-end complete-case alignment;
- point-in-time availability predicates for every component;
- split-aware decision-date share count and market capitalization;
- monthly liquidity, size, age, and raw-outlier gates;
- the study's 2.5% two-sided net-payout winsorization followed by z-scoring;
- deterministic IDs, full component and market-cap lineage, and idempotent refresh;
- governed factor definition and dependencies in migration `0201`;
- a standalone build CLI.

Live build `loop8-net-payout-build` produced:

| Metric | Result |
|---|---:|
| Factor rows | 3,845 |
| Securities | 92 |
| Rebalance dates | 141 |
| Date range | 2012-03-30 to 2025-06-30 |
| Duplicate keys | 0 |
| Non-finite values | 0 |

## Analysis

Run id: `loop8-net-payout-production`.

| Horizon | Mean rank IC | HAC t-stat | Mean names | Spread | Hit rate |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.02234 | 0.98 | 23.9 | 0.179% | 53.2% |
| 63d | 0.02900 | 0.89 | 23.6 | 0.293% | 51.8% |
| 126d | 0.05996 | 1.17 | 23.3 | 1.217% | 57.6% |
| 252d | 0.07585 | 1.18 | 22.6 | 7.584% | 63.0% |

The sign is positive and strengthens with horizon, but no HAC statistic reaches 1.2. The low name
count also makes ten-quantile portfolios fragile. Top/bottom turnover is 20.0%/19.1%, and rank
autocorrelation is 0.970.

### Equal-cohort comparison

The common cohort contains 3,346 security/date keys, 74 securities, and 140 dates. Mean
cross-sectional correlation between net payout and the router is 0.204.

| Factor | 21d IC / HAC | 63d IC / HAC | 126d IC / HAC | 252d IC / HAC |
|---|---:|---:|---:|---:|
| Production router | 0.0692 / 3.14 | 0.1101 / 3.37 | 0.1624 / 3.73 | 0.1827 / 3.55 |
| Net payout | 0.0225 / 0.99 | 0.0292 / 0.90 | 0.0603 / 1.17 | 0.0759 / 1.18 |
| Equal-weight blend | 0.0478 / 2.58 | 0.0773 / 2.95 | 0.1247 / 3.09 | 0.1520 / 3.23 |

The router dominates net payout and the blend at every horizon. Low correlation alone is not a
reason to add a weaker factor.

## Decision

Keep `financing_net_payout_yield` as an experimental, queryable production feature, but do not
promote it into the leading router and reject the equal-weight blend.

The factor's main value is architectural: it activates two important SEC cash-flow concepts,
establishes strict missing-component semantics, and makes incremental TTM refresh operationally
safe. A broader payout factor requires a tested zero-observation policy derived from complete cash-
flow statements or a second source—not `coalesce(missing, 0)`.

## Verification

- Targeted TTM-scope test: 1 passed.
- Targeted net-payout tests: 2 passed.
- New Loop 07/08 feature files: Ruff clean and Python compilation clean.
- Schema `0201`, migration checksums, checkpoint/reopen, duplicate, finiteness, and component counts:
  passed.
- Full-suite execution was intentionally avoided.

## Next loop

Research a broader quality/earnings signal with materially greater SEC coverage rather than forcing
an imputed payout measure. Candidate: standardized unexpected earnings or earnings acceleration,
using filing-time-safe quarterly EPS/net-income changes and explicit seasonal comparison.
