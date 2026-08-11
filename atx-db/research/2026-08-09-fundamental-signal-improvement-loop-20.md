# Fundamental signal improvement loop 20: q5 expected-growth proxy

Date: 2026-08-09

## Research question

Can the q5 model's expected one-year investment-growth signal stabilize delta ROE by combining it
with Tobin's q and cash operating profitability, without fitting any parameter to ATX returns?

## Primary-source basis

- Hou, Mo, Xue, and Zhang forecast one-year-ahead investment-to-assets changes from log Tobin's
  q, cash operating profitability, and four-quarter change in ROE. Their expected-growth factor
  earns 0.84% per month with a 10.27 t-stat in 1967-2018:
  <https://academic.oup.com/rof/article-abstract/25/1/1/5727769>.
- The current global-q technical document defines Tobin's q as market equity plus long- and
  short-term debt divided by book assets; annual q and cash profitability must come from a fiscal
  year ending at least four months ago. Predictors and the investment-growth target are winsorized
  monthly at 1%/99%, and missing delta ROE is zero in the forecasting regressions:
  <https://global-q.org/uploads/1/2/2/6/122679606/factorstd_2025feb.pdf>.
- The paper estimates market-equity-weighted monthly cross-sectional regressions and combines
  current predictors with average slopes from the prior 120 months, requiring at least 30 months.
  Its reported multivariate one-year average slopes are -0.029 for `ln(q)`, 0.516 for cash operating
  profitability, and 0.771 for delta ROE:
  <https://global-q.org/uploads/1/2/2/6/122679606/houmoxuezhang2020rf.pdf>.
- The maintained global-q library continues to publish the expected-growth factor and its six
  size-by-growth benchmark portfolios:
  <https://global-q.org/factors.html>.

The predictor definitions, four-month annual lag, missing-delta-ROE policy, winsorization, and
published slopes were fixed before evaluation.

## Explicit proxy boundary

This loop deliberately implements a published-slope proxy, not a falsely labeled exact replication.
It applies the paper's reported full-sample average slopes:

`-0.029 * ln(Tobin's q) + 0.516 * Cop + 0.771 * dROE`

The cross-sectionally rank-invariant intercept is omitted. No coefficient is fit to ATX returns.
Replacing the paper's rolling 120-month slope history with published average slopes provides an
immediately testable external specification while the warehouse's point-in-time investment-growth
training surface is built in the next loop.

Additional SEC/Compustat adaptations are explicit in every row:

- monthly market equity is point-in-time close times the latest visible reported shares;
- `LongTermDebtNoncurrent` is long-term debt; `LongTermDebtCurrent`, with `DebtCurrent`
  fallback, is short-term debt; absent components are zero;
- governed Ball cash-profitability numerator is rescaled from prior-year to current assets;
- reported SEC net income/equity remains the previously documented qROE adaptation;
- historical financial-industry exclusion is unavailable without leaking current classifications.

## Production build

Added `atx_db.expected_growth`, migration `0219`, a standalone build CLI, and two focused tests.
The factor is `investment_q5_expected_growth_proxy`, sourced as
`atx-db PIT q5 published-slope expected growth proxy v1`.

The loader joins governed asset-growth and cash-profitability decisions only when security, monthly
date, annual accession, and fiscal period all match. Current and prior assets plus market equity come
from their existing lineage. Same-accession SEC company facts supply debt. Delta ROE is joined at
the same monthly decision and set to zero only when absent, matching the paper. Annual facts must be
120-550 days old and every dependency must be visible at the decision.

The pre-build audit found 53,617 unique eligible decisions across 734 securities and 175 dates, with
zero availability violations. Both debt components are reported on 23,562 rows; neither is reported
on 19,170 rows and those components become zero. Delta ROE is observed on 39,598 rows and imputed
to zero on 14,019 rows.

The full refresh materializes all 53,617 rows from 2012-04-30 to 2026-06-15 in 21.1 seconds.
Minimum daily breadth is 32 and maximum lineage is 1,961 bytes. Raw Tobin's q reaches 1.85 million
for small-asset cases, so the predeclared predictor-level monthly 1%/99% caps are essential. Final
proxy scores contain no duplicate, non-finite, or future-available rows and have exact sample
cross-sectional z-score normalization.

## Standalone analysis

Run id: `loop20-q5-expected-growth-proxy-production`. Deciles and split-adjusted forward returns
are used.

| Horizon | Rank IC | HAC t-stat | Q10-Q1 spread | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|---:|---:|
| 21d | 0.0090 | 1.42 | -0.528% | 45.0% | 0.04 |
| 63d | 0.0014 | 0.20 | -1.434% | 39.1% | -0.10 |
| 126d | 0.0005 | 0.06 | -2.808% | 38.6% | -0.13 |
| 252d | 0.0055 | 0.44 | -3.405% | 41.3% | -0.02 |

The imported published slopes do not transfer to the ATX sample. Average rank ordering is weak and
the literal high-minus-low trade loses at every horizon. Top/bottom-decile turnover is 32.0%/39.8%,
mean rank autocorrelation is 0.956, and 174 rebalances are available.

### Subperiod stability

| Period | 21d IC | 63d IC | 126d IC | 252d IC |
|---|---:|---:|---:|---:|
| 2012-2015 | 0.0023 | -0.0044 | 0.0062 | -0.0018 |
| 2016-2020 | 0.0163 | 0.0051 | -0.0011 | 0.0183 |
| 2021-2026 | 0.0066 | 0.0018 | -0.0022 | -0.0034 |
| 2023-2026 | 0.0168 | 0.0053 | -0.0046 | -0.0110 |

No regime has a reliable multi-horizon profile. The recent 21-day result has HAC 1.73, but recent
126/252-day results are negative. This does not support a broad production signal.

### Distinctiveness and component dominance

Mean cross-sectional correlation is 0.816 with cash operating profitability, 0.562 with operating
profitability, 0.504 with the production router, 0.441 with delta ROE, 0.223 with qROE level, 0.078
with conservative asset growth, and 0.053 with low net issuance. The raw-unit published slopes make
the ATX proxy predominantly a cash-profitability score rather than a balanced three-signal
composite. This is a likely reason its bottom-tail pathology resembles the weaker profitability
features instead of the production router.

## Router decision

Do not test or promote a router overlay. The proxy fails the standalone preregistration gates before
an overlay is justified: no horizon has HAC t-stat 2, all high-minus-low spreads are negative,
monotonicity is approximately zero or negative, and turnover is high. The validated production
operating-profitability/net-issuance router remains unchanged.

## Decision

Keep `investment_q5_expected_growth_proxy` as a production-queryable experimental feature and an
auditable external-coefficient benchmark. Do not present it as the exact rolling q5 forecast, do not
use its extreme tails in trading, and do not add it to the production router.

The next hypothesis is the exact point-in-time forecasting procedure: learn monthly
market-equity-weighted slopes from realized investment-growth targets only, average the prior
120 months with a 30-month minimum, and never fit against security returns. That should adapt the
component balance to the SEC-based data while preserving a clean research boundary.

## Verification

- Pure formula/lineage and isolated migration-governance tests passed.
- New and changed Python files pass Ruff and compilation.
- Live schema is `0219`; migration checksums verify and the factor records three governed-factor
  dependencies plus the SEC company-facts source.
- Coverage, duplicate-key, finiteness, annual-age, availability, imputation, minimum-breadth,
  normalization, and lineage-size checks passed.
- Full-suite execution was intentionally avoided.

## Next loop

Build the exact point-in-time q5 training target and rolling WLS slope surface. Validate target
alignment first: the realized one-year change in investment-to-assets must be known at the training
month, while each predictor is matched to its state 12 months earlier. Persist slope histories and
forecast lineage so every live expected-growth score can be reconstructed without return leakage.
