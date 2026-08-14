# Fundamental signal improvement loop 86: price-controlled fundamental momentum

Status: **production data feature complete; rejected from production and shadow alpha**.

## Research and feature contract

Novy-Marx shows that fundamental momentum subsumes earnings momentum and improves the
performance of price momentum, while AQR's long-only integration research motivates combining
signals at the stock-score level rather than only mixing separately constructed sleeves:

- https://www.nber.org/papers/w20984
- https://www.aqr.com/Insights/Research/White-Papers/Long-Only-Style-Investing

Loop 86 adds `earnings_sue_price_momentum_residual_12_1`. At every governed SUE decision
timestamp it computes split-adjusted 12-minus-1-month price momentum from exact
daily-bar endpoints. The reference bar may be at most seven calendar days stale. Within each
decision-date cross-section, the transform rank-normalizes SUE and price momentum, fits SUE rank
on price-momentum rank with an intercept, and uses the winsorized, standardized residual as the
signal. Missing price-momentum endpoints are dropped rather than imputed.

The lineage records the governed SUE observation and lineage, both exact price endpoints and
their availability timestamps, adjusted endpoint prices, raw price momentum, cross-sectional
regression intercept and beta, sample size, and residual. Migration 262 governs the feature and
has been applied with a verified checksum.

## Breadth expansion

The local ten-year daily-bar archive contains 31,598,499 rows, 24,959 vendor IDs, and 12,218 IDs
on its latest date. Loop 86 converted the broad load into a resumable canonical projection:

- only the 12 columns needed by `equity_daily_bars` are decoded;
- projection-only loads bypass the wide raw-vendor table;
- security-ID mapping is vectorized;
- DuckDB memory and thread ceilings are explicit;
- every chunk commits independently and a retry resumes from a proven boundary;
- compressed-archive skip progress is logged.

After the resumable pandas writer exposed both repeated indexed-upsert cost and a corrupted
partial DuckDB segment, the production loader was replaced with a native bulk publisher. It
extracts once, projects with DuckDB, builds a complete validated side table, and publishes the
source partition transactionally. Migration 263 adds point-in-time `shares_outstanding` and
`market_cap_usd` to canonical bars, removing the old wide raw-table dependency from the 13F
mid-cap screen. The publication gate requires at least 30 million clean bars, 10,000 securities,
5,000 names on the latest date, zero duplicate security/date keys, and zero invalid OHLCV rows.

The first wide attempt exhausted memory after preserving its committed chunks. A raw-line resume
seek reduced the 15-million-row skip from roughly 13 minutes to 51 seconds, but repeated indexed
upserts still failed to make production progress. The native publisher replaced that architecture
and completed in 532.2 seconds: 31,173,360 clean bars across 34,495 canonical securities, with
12,206 names on 2026-06-15, zero duplicate keys, and zero invalid OHLCV rows. The originally
interrupted WAL was retained as a forensic artifact before the reproducible vendor partition was
rebuilt; unrelated warehouse surfaces were not cleared.

## Integrated portfolio construction

The preregistered search now evaluates both `sleeve_mix` and `integrated_rank` for each frozen
variant/allocation pair. Integrated rank aligns candidate and router observations on the full
eligible union, assigns a neutral cross-sectional score to a missing style, blends stock-level
rank scores, and constructs one capacity-constrained portfolio. Missing forward returns are never
neutralized. With six variants, three allocations, and two construction modes, Loop 86 commits 36
selection trials before observing validation results.

## Diagnostics and disposition

The production refresh materialized 93,178 rows across 1,098 securities and 164 decision dates
from 2013-03-28 through 2026-06-15. Price-controlled SUE remained positive at every diagnostic
horizon, with its strongest HAC evidence at the one-month horizon:

| Horizon | Mean rank IC | HAC t-stat | Conventional t-stat | Sign consistency | Dates | Mean names |
|---|---:|---:|---:|---:|---:|---:|
| 21 trading days | 0.0101 | 2.08 | 2.07 | 59.9% | 162 | 519.0 |
| 63 trading days | 0.0116 | 1.89 | 2.74 | 62.5% | 160 | 522.2 |
| 126 trading days | 0.0126 | 1.78 | 2.84 | 59.9% | 157 | 521.4 |
| 252 trading days | 0.0097 | 1.02 | 2.02 | 59.6% | 151 | 518.9 |

The 36 newly committed trials raised the durable multiple-testing count from 414 to 450. Fifteen
selection configurations passed the positive-candidate, turnover, participation, and deployment
support rule. Both stock-level integrated ranks and separately constructed sleeve mixes were
evaluated. The frozen selection winner was **decile tails, 10% sleeve allocation**: selection
candidate Sharpe 0.077, router Sharpe 0.331, combined Sharpe 0.344, and marginal Sharpe +0.013.
The integrated mode did not win; this result was observed only after the two-mode grid was
committed.

The untouched validation contains 64 monthly observations:

| Validation | Candidate sleeve | Router v6 | 90/10 combination | Doubled costs |
|---|---:|---:|---:|---:|
| Net Sharpe | -0.178 | 0.723 | 0.700 | 0.604 |
| Annualized return | -0.91% | 2.46% | 2.32% | 2.00% |
| Maximum drawdown | -11.60% | -10.34% | -10.19% | -10.44% |
| Average turnover | 0.400 | 0.061 | 0.096 | 0.096 |
| Annualized cost drag | 0.51% | 0.30% | 0.32% | 0.64% |
| Maximum ADV participation | 4.21% | 9.21% | 7.48% | 7.48% |
| Minimum gross deployment | 100.00% | 100.00% | 100.00% | 100.00% |
| Probabilistic Sharpe | 34.52% | 93.87% | 93.46% | 90.57% |
| DSR probability (450 trials) | 0.04% | 10.22% | 9.11% | 6.02% |

Candidate/router correlation was -0.050, but diversification did not compensate for negative
standalone performance: combined Sharpe fell by 0.023 versus the router. Three of six folds were
positive. Median validation signal breadth was 558.5 names, below the 1,000-name production
floor; the candidate portfolio held a median 111 names with effective breadth 110.4. The combined
portfolio held a median 642 names with effective breadth 468.3.

Decision: **reject price-controlled fundamental momentum from both production and shadow alpha**.
It failed institutional breadth, candidate Sharpe, DSR, and marginal mega-alpha Sharpe. Candidate
PSR was only 34.5%, so the under-breadth shadow exception does not apply. The production data
feature remains queryable. The exposed validation sample must not be reused for retuning.

Artifact: `atx-factor/research/loop86-fundamental-momentum-exploration.json`.
Evidence SHA-256:
`99906bd5d8943ba58b777c343acbfbeb8780307e84d4116cfd3021ee36c5ab28`.

## L1vsun 13F reproduction on institutional prices

The requested analysis was rerun after canonical breadth publication. It has 556 selected
point-in-time signals, 278 audited OpenFIGI mappings, 128 priced entries, and 123 complete 47-day
trades. At five, ten, 21, and 47 trading days, mean net short returns were -0.87%, -0.20%, -0.72%,
and -2.60%, respectively. The 47-day win rate was 48.8%. Only 16.61% of manager/security
positions were absent from the next disclosed filing, which cannot identify an exact trade date.

Decision: **the pooled post strategy remains rejected**. Its 47-day mean deteriorated from the
earlier narrow-price replay, and the claimed 3.1 Sharpe remains unidentifiable because the post
does not disclose its portfolio sizing or return-series construction. The restatement-only cohort
returned +2.93% across just 23 complete trades and remains a separately preregistered hypothesis,
not production evidence. Full evidence is in
`research/recreated-l1vsun-13f-amendment-analysis.md` and its JSON companion.

## Verification

- Two focused feature tests cover momentum residual orthogonality and split-adjusted 12-minus-1
  endpoint construction; the migration governance test passed.
- Focused explorer, CLI, production-registry, and shadow-registry tests passed for both combination
  modes; no full suite ran.
- Ruff passed on the feature, loader, deferred runner, explorer, registries, and focused tests.
- Native bulk success and rollback-gate tests passed; the focused 13F market-cap/backtest test
  passed after removing the legacy raw-table join.
- The live warehouse is at migration 263 and all applied migration checksums verify.
- Canonical bars have 31,173,360 non-null point-in-time share and market-cap values; latest-date
  breadth is 12,206; both canonical indexes exist; duplicate keys and invalid OHLCV rows are zero.
- The factor has zero duplicate keys, non-finite values, missing lineage, late parent/bar inputs,
  stale references, or invalid momentum endpoints.
- The durable trial ledger contains exactly 450 trials and one Loop 86 experiment. The evidence
  digest recomputes exactly; neither production nor shadow registry contains the rejected factor.
