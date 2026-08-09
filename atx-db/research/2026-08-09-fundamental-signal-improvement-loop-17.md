# Fundamental signal improvement loop 17: conservative asset growth

Date: 2026-08-09

## Research question

Does the published low-investment premium survive a strict point-in-time implementation using
annual total-asset growth in the modern U.S. liquid-equity panel?

## Primary-source basis

- Cooper, Gulen, and Schill define firm asset growth as the one-year percentage change in total
  assets and find it economically and statistically predicts U.S. returns, including among large
  stocks:
  <https://onlinelibrary.wiley.com/doi/10.1111/j.1540-6261.2008.01370.x>.
- Fama and French motivate investment from the dividend-discount identity: holding price and
  expected profitability fixed, higher expected book-equity growth implies a lower expected
  return. Their five-factor model adds conservative-minus-aggressive investment:
  <https://www.sciencedirect.com/science/article/pii/S0304405X14002323>.
- The authors' production data description requires total assets for two consecutive fiscal years
  and forms CMA as conservative minus aggressive investment portfolios:
  <https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library/f-f_5developed.html>.
- Hou, Xue, and Zhang independently include an investment factor in the q-factor model, which
  summarizes much of the anomaly cross-section:
  <https://academic.oup.com/rfs/article-abstract/28/3/650/1574802>.

The direction and transform were fixed before evaluation:

```text
asset_growth = (total_assets_t - total_assets_t-1) / total_assets_t-1
conservative_asset_growth = -asset_growth
```

The cross-sectional score is winsorized at the predeclared 1st/99th percentiles and z-scored. The
raw unbounded value is retained for audit.

## Important contrary evidence

Fu argues that the apparent benefit of negative asset growth can be driven by omitted delisting
returns and that aggressive growth overlaps external financing:
<https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2533316>.

That warning is material here. The governed survivorship-safe forward-return table is currently
empty, so this loop uses split-adjusted price returns and cannot claim delisting-robust validation.
The feature is productionized, but promotion decisions remain conservative.

## Production build

Added `atx_db.asset_growth`, migration `0214`, a standalone build CLI, and focused tests. The
feature is `investment_conservative_asset_growth`.

For every governed monthly universe decision, the loader selects the latest annual total-assets
observation visible at the close, then the closest visible prior annual period 300-430 days earlier.
The current observation may be at most 550 days old. It accepts annual 10-K, 10-K/A, 10-KT, 20-F,
20-F/A, 40-F, and 40-F/A filings; both values must be positive finite USD instant facts. Every row
retains accession numbers, statement-point IDs, periods, availability timestamps, and universe
lineage.

The live refresh materializes 116,768 rows across 953 securities and 175 dates from 2012-04-30 to
2026-06-15 in 22.2 seconds. Observed fiscal gaps are 336-371 days. There are no duplicate IDs,
non-finite scores, or statement timestamps after the factor's availability. Maximum lineage is
1,290 bytes.

Raw asset growth ranges from -93.7% contraction to an extreme 271,039% expansion caused by a tiny
prior denominator. The scoring transform caps such extremes by date; provenance does not erase
them.

## Analysis

Run ids:

- `loop17-conservative-asset-growth-production` (quintiles);
- `loop17-conservative-asset-growth-deciles` (paper-faithful tails).

### Continuous rank result

| Horizon | Rank IC | HAC t-stat | Sign consistency |
|---:|---:|---:|---:|
| 21d | -0.0088 | -1.30 | 57.3% negative |
| 63d | -0.0148 | -1.29 | 60.9% negative |
| 126d | -0.0166 | -0.97 | 60.2% negative |
| 252d | -0.0185 | -0.83 | 61.3% negative |

The continuous relation reverses the published orientation. This is not a sign error: higher factor
values are verified to represent lower asset growth. The return shape is non-monotonic.

### Paper-faithful conservative-minus-aggressive tails

| Horizon | Decile 10 minus decile 1 | HAC t-stat | Positive-date rate |
|---:|---:|---:|---:|
| 21d | 0.631% | 1.56 | 52.0% |
| 63d | 1.801% | 1.93 | 50.9% |
| 126d | 4.133% | 1.68 | 50.6% |
| 252d | 8.337% | 1.56 | 59.4% |

The top conservative decile has a 26.35% pooled one-year return versus 17.83% for the aggressive
decile. However, middle-decile returns generally decline before jumping in the most conservative
tail; decile monotonicity is -0.26 at one year. The tail spread is useful research evidence, not a
valid continuous alpha claim.

Top/bottom-decile turnover is 16.4%/15.3%, mean rank autocorrelation is 0.942, and 174 consecutive
monthly rebalances are available.

### Subperiod stability

Continuous IC:

| Period | 21d | 63d | 126d | 252d |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0057 | 0.0152 | 0.0295 | 0.0420 |
| 2016-2020 | -0.0154 | -0.0307 | -0.0433 | -0.0697 |
| 2021-2026 | -0.0126 | -0.0206 | -0.0237 | -0.0099 |
| 2023-2026 | -0.0258 | -0.0413 | -0.0623 | -0.0780 |

Decile-tail spread:

| Period | 21d | 63d | 126d | 252d |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.320% | 1.640% | 3.988% | 8.418% |
| 2016-2020 | 0.457% | 1.423% | 5.336% | 10.552% |
| 2021-2026 | 1.018% | 2.297% | 2.981% | 5.679% |
| 2023-2026 | 1.028% | 2.020% | 0.553% | -2.144% |

The tail spread persists through the broader recent period but the one-year leg reverses after
2023. Only the 2012-2015 one-year tail is individually significant (HAC 2.48). This instability and
the missing delisting target prevent promotion.

### Distinctiveness and router test

Mean cross-sectional correlation is 0.107 with the broad router, 0.268 with low net issuance,
-0.031 with cash-flow profitability, -0.023 with QMJ profitability, -0.127 with Altman, and 0.127
with book-to-market. The feature is distinct, but distinctiveness does not imply incremental alpha.

On its exact overlapping cohort, the baseline router IC is 0.02230 / 0.03499 / 0.05155 / 0.07057.
A 10% continuous overlay lowers it to 0.01704 / 0.02696 / 0.04257 / 0.06180. A sparse 10%
long-short decile-tail overlay also lowers it to 0.01942 / 0.03001 / 0.04597 / 0.06528. Both
overlays weaken every recent-period horizon as well.

Point-in-time Fama-French 12-industry neutralization fails closed at 0% historical coverage because
all landed classifications are dated 2026-08-09. The analysis does not leak those current labels
backward.

## Decision

Keep `investment_conservative_asset_growth` as a broad, production-queryable feature with exact
lineage. It supports a useful published tail sort, but it is not a monotonic alpha and must not enter
the production router. Do not materialize a separate tail factor yet: recent one-year reversal,
moderate HAC support, and absent delisting-safe targets outweigh the full-sample spread.

## Verification

- Two focused tests passed serially; Ruff and Python compilation passed.
- Schema `0214`; one direct `total_assets` dependency and explicit governed-universe input.
- Migration checksums, duplicate-key, finiteness, fiscal-gap, availability, and lineage-size checks
  passed.
- Full-suite execution was intentionally avoided.

## Next loop

Build the q-factor profitability definition based on timely return on equity. QMJ component
diagnostics found independently promising ROE, and Hou-Xue-Zhang specify ROE as their profitability
factor. Use quarterly point-in-time earnings and lagged book equity to increase update frequency,
then test it without using fitted component weights.
