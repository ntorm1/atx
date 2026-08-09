# Fundamental signal improvement loop 03 — cash operating profitability

Date: 2026-08-09  
Feature run: `fundamental-signals-loop3-20260809`  
Common-cohort evaluation: `fundamental-signals-loop3-common-cohort-20260809`

## Decision

Reject cash operating profitability as an improvement over the existing
operating-profitability signal in the current ATX sample. It has positive rank
IC, but is slightly weaker than accrual-inclusive Ball operating profitability,
has non-monotone/negative extreme-decile spreads beyond one month, and the
low-accrual leg has the opposite sign from the literature. Retain all three
series as governed data features for future interactions and diagnostics; do
not promote them to the leading alpha family.

The existing Fama–French-style operating-profitability feature remains the
strongest profitability candidate, especially at 126–252 trading days.

## Primary-source hypothesis

Ball, Gerakos, Linnainmaa, and Nikolaev report that cash operating
profitability outperforms operating profitability and largely subsumes the
accrual anomaly. Their appendix defines all measures over prior-year total
assets and specifies:

```text
operating profit = revenue - COGS - reported SG&A excluding R&D
cash operating profit = operating profit
                      - Δreceivables - Δinventory - Δprepaids
                      + Δdeferred revenue + Δpayables + Δaccrued expenses
```

Missing balance-sheet accounts are replaced with zero in the authors'
construction. The paper also distinguishes this measure from GAAP operating
cash flow because GAAP CFO is net of interest and taxes.

Source: [Ball et al., *Accruals, cash flows, and operating profitability in the
cross section of stock returns*](https://www.ivey.uwo.ca/media/3775325/gerakos.pdf),
especially the appendix on pages 24–26. The [SSRN record](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2587199)
provides the publication metadata.

## Data build

The live SEC company-facts load predated the current 131-concept source
contract. A fresh SEC bulk archive was cached at `data/cache/companyfacts.zip`
and expanded the raw table to:

- 10,963,558 fact rows
- 123 concepts actually reported by the 1,598 loaded filers
- source periods through 2026-08-07

The broad operator exceeded its one-hour wrapper while its Python child
continued. Raw facts committed, but the child exited before all chained
surfaces were rebuilt. To avoid another full 11-million-row sort, the revisions
and statement materializers gained an optional concept scope and only the five
missing concepts were rebuilt.

| Canonical metric | Statement rows | Securities |
|---|---:|---:|
| `ap` | 111,379 | 1,045 |
| `rd_expense` | 78,313 | 626 |
| `accrued_liabilities` | 65,184 | 727 |
| `prepaid_expense` | 25,966 | 492 |
| `deferred_revenue` | 9,821 | 362 |

Existing receivables and inventory coverage was 973 and 905 securities,
respectively. The canonical statement surface now has 4,713,514 rows.

The obsolete `data/warehouse.legacy.duckdb` backup (14.82 GB, dated
2026-06-30) was permanently removed after path verification to prevent the SEC
transactional spill from filling the drive. The active warehouse and downloaded
SEC archive remain intact.

## Feature implementation

`atx_db.cash_profitability` materializes three monthly PIT features:

| Factor | Rows | Securities | Dates |
|---|---:|---:|---:|
| `profitability_ball_operating_profitability` | 69,270 | 777 | 176 |
| `profitability_cash_operating_profitability` | 69,270 | 777 | 176 |
| `quality_low_operating_working_capital_accruals` | 69,270 | 777 | 176 |

The loader selects the newest annual report visible at each month-end close,
matches a visible prior annual balance sheet 300–430 days earlier, scales by
positive prior-year assets, applies the same $100 million market-cap, $1 million
ADV21, and 550-day freshness screens used by the prior loops, then winsorizes
and z-scores each date. Low accruals are direction-oriented so higher factor
values mean lower raw working-capital accruals. Every row records current and
prior fact IDs, availability times, accessions, missing-as-zero decisions, and
the full formula in lineage JSON.

## Sample-controlled results

The horse race uses the cash feature's governed keys. Ball and cash measures
have 66,218 observations across 742 securities and 175 dates; the existing
Fama–French-style operating-profitability series overlaps 60,742 observations
across 733 securities.

| Factor | 21d IC / HAC t | 63d IC / HAC t | 126d IC / HAC t | 252d IC / HAC t |
|---|---:|---:|---:|---:|
| Ball operating profitability | 0.0214 / 2.70 | 0.0279 / 2.34 | 0.0300 / 1.91 | 0.0296 / 1.34 |
| Cash operating profitability | 0.0204 / 2.75 | 0.0249 / 2.22 | 0.0258 / 1.80 | 0.0254 / 1.26 |
| Existing Fama–French-style OP | 0.0188 / 2.46 | 0.0279 / 2.67 | 0.0375 / 2.95 | 0.0429 / 2.79 |
| Low working-capital accruals | -0.0058 / -0.93 | -0.0157 / -1.69 | -0.0263 / -2.24 | -0.0369 / -2.25 |

Cash and Ball operating profitability are 0.9884 rank-correlated. The existing
Fama–French-style feature is only about 0.58 correlated with either, explaining
why its longer-horizon behavior differs materially.

Extreme-decile diagnostics reinforce the rejection:

| Factor | 21d spread | 63d spread | 126d spread | 252d spread |
|---|---:|---:|---:|---:|
| Ball operating profitability | 0.06% | -0.17% | -1.28% | -2.82% |
| Cash operating profitability | -0.02% | -0.37% | -1.78% | -4.08% |
| Existing Fama–French-style OP | 0.15% | 0.34% | 0.23% | -1.16% |
| Low working-capital accruals | -0.04% | -1.01% | -4.36% | -10.12% |

The low-accrual 252-day hit rate is 32.5% and its decile monotonicity is -0.782,
showing a strong inverse ordering rather than a weak null. Cash and Ball top
deciles turn over about 11.3% monthly; the accrual leg turns over about 20.1%.

## Interpretation

The literature result does not replicate in this 2012–2026 sample. Plausible,
non-exclusive explanations are:

1. the accrual anomaly is documented to have weakened in more recent samples;
2. SEC concepts are not Compustat-standardized fields, especially for SG&A/R&D
   presentation and sparse deferred-revenue/prepaid reporting;
3. missing-as-zero is faithful to the paper but more consequential with SEC tag
   heterogeneity;
4. the sample lacks industry neutralization and excludes financials only through
   text/common-equity screens rather than historical SIC classification;
5. incomplete delisting/terminal returns can bias long horizons; and
6. a 2012 start is much shorter and later than the paper's 1963–2013 sample.

## Next production gate

Before accepting another accounting signal, populate historical industry
classifications and survivorship-safe forward returns, then rerun loops 02–03.
The negative accrual result should also be tested separately for concept-complete
issuers versus missing-as-zero issuers. This distinguishes a genuine modern sign
reversal from SEC taxonomy sparsity.
