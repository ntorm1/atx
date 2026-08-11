# Recreated 13F amendment-spike analysis

Status: **complete**

This report independently implements the method described in the [original X
post](https://x.com/L1vsun/status/2085445915897176101) using the [SEC Form 13F data
sets](https://www.sec.gov/data-research/sec-markets-data/form-13f-data-sets). It does not assume the post's reported outcomes.

## Reproduction contract

- A RESTATEMENT replaces the prior information table; ADD NEW HOLDINGS
  supplements the latest full table, consistent with the [official Form 13F
  instructions](https://www.sec.gov/files/form13f.pdf) and [SEC amendment FAQ](https://www.sec.gov/rules-regulations/staff-guidance/division-investment-management-frequently-asked-questions/frequently-asked-questions-about-form-13f). The
  pooled result follows the post literally by including both filing types.
- Manager-quarter amendment rate is distinct corrected position keys divided by
  the final filed position count.
- The z-score uses only the manager's prior 24 reported quarters. A spike is at
  least 2.0 standard deviations above that trailing baseline. When all 24
  trailing rates are identical at zero, the first positive rate receives an
  explicit, rankable z-score cap of 10.0 rather than an undefined value.
- A security signal requires at least three distinct spike managers correcting
  the same CUSIP in the same report quarter. Availability is the last
  contributing amendment's filing date.
- The post says to rank signals but does not disclose a portfolio cutoff. This
  reproduction retains the top 20 average-z
  candidates per quarter and excludes stress quarters as a transparent,
  capacity-constrained evaluation cohort. The raw candidate count remains
  reported so this assumption is auditable rather than fitted invisibly.
- Instrument mappings use audited [OpenFIGI v3](https://www.openfigi.com/api/documentation) candidates.
- The price cohort is restricted at entry to US-listed common equity, ADR, or
  REIT instruments with market capitalization from
  $2B to
  $10B, using archived
  point-in-time shares and price.
- Backtests enter on the first price bar available strictly after the signal and
  deduct 5 bps each way. Short returns are reported at fixed trading-day
  horizons; no outcome-driven exit timing is used.

## Warehouse coverage

- SEC archives loaded: 53
- Submissions: 397,833
- Holdings: 120,181,801
- Filing-date coverage: 2013-05-20 through 2026-05-29
- Report-period coverage: 1987-03-31 through 2026-03-31
- Manager-quarter observations: 298,107
- Amended manager-quarters: 12,768
- Reconstructed position corrections: 4,379,243
- 2-sigma manager-quarter spikes: 1,757
- Raw three-filer security-quarter candidates: 78,077
- Selected top-20 quiet-quarter signals: 556

## Results versus the post

| Metric | Post claim | Independent result |
|---|---:|---:|
| Signals per year | 62 | 61.78 |
| Signals / trades | ~1,700 over 11 years | 556 signals; 123 price-complete 47-day trades |
| Position leaves portfolio | 71% within 47 trading days | 16.61% at next disclosed filing |
| Net return per trade | 0.38% | -1.23% at 47 trading days |
| Sharpe ratio | 3.1 | Not identifiable without the post's portfolio sizing and return-series construction |
| Average holding period | 9.4 days | Not reproducible from quarterly 13F disclosures; fixed horizons are used |

Mapped signals: 278 of 556. The
next-filing disclosed-exit sample contains 2,047
manager/security observations and 340 exits.
An absence in the next quarterly filing is not evidence of the exact trade date,
so the claimed 47-trading-day exit timing cannot be identified from Form 13F
alone.

## Fixed-horizon sensitivity

| Trading days | Complete trades | Mean net short return | Median | Short win rate |
|---:|---:|---:|---:|---:|
| 5 | 128 | 0.30% | 0.83% | 53.12% |
| 10 | 128 | 0.74% | 1.19% | 54.69% |
| 21 | 127 | -0.03% | -0.07% | 49.61% |
| 47 | 123 | -1.23% | -0.01% | 49.59% |

## Stress-regime split

- Quiet: 123 completed trades, -1.23% mean net short return.

## Amendment-type audit

RESTATEMENT corrects and supersedes a filing. ADD NEW HOLDINGS supplements it and can disclose
positions after confidential treatment expires or is denied. Pooling them therefore mixes distinct
events even though both use `13F-HR/A`.

| Cohort | Selected signals | Complete 47d trades | Mean net short | Median | Win rate |
|---|---:|---:|---:|---:|---:|
| MIXED | 393 | 100 | -2.30% | -0.88% | 48.00% |
| RESTATEMENT_ONLY | 163 | 23 | 3.41% | 2.00% | 56.52% |

The restatement-only result is exploratory: it was examined after the pooled claim failed and has
only 23 complete trades. It is a hypothesis for a separately pre-registered test, not evidence for
the post's pooled 3.1-Sharpe claim.

## Mega-alpha decision

**Reject.** The post's pooled signal has negative 47-day return, approximately random directional
accuracy, no identifiable 47-day institutional-exit timestamp, and no disclosed sizing methodology
from which to reconstruct the claimed Sharpe. No production router or mega-alpha registry change is
authorized by this result.

## Interpretation

The backtest is point-in-time with respect to public filing and price
availability. It is still subject to CUSIP-to-ticker coverage, quarterly
disclosure latency, confidential-treatment omissions, manager identity changes,
and the limits of the available daily-price archive. These coverage figures are
part of the result rather than silently dropped. The near-exact 62-signals/year
match is a consequence of the disclosed quiet-quarter rule plus the explicit
top-20 capacity assumption; it is not independent evidence for the post's
undisclosed implementation.
