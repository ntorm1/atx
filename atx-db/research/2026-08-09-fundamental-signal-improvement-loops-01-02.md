# Fundamental signal improvement loops 01–02

Date: 2026-08-09  
Warehouse: `data/warehouse.duckdb`  
Production evaluation run: `fundamental-signals-loop2-20260809`

## Outcome

Loop 01 rejected gross profitability and the first quality/value composite as
stand-alone alpha candidates in the available sample. Loop 02 produced a much
stronger result: point-in-time Fama–French-style operating profitability has a
positive rank information coefficient at every tested horizon, survives
overlap-robust inference, turns over slowly, and is now materialized and scored
through governed production tables.

This is a validated research feature, not yet a claim of deployable net alpha.
The sample still lacks complete delisting-return stitching, minority interest,
and populated industry classifications. Those limitations are explicit below.

## Literature and hypotheses

Primary sources used for the two loops:

- Novy-Marx, *The Other Side of Value: The Gross Profitability Premium*:
  gross profits scaled by assets historically forecast returns and complement
  book-to-market. [NBER working paper](https://www.nber.org/system/files/working_papers/w15940/w15940.pdf)
- Piotroski, *Value Investing: The Use of Historical Financial Statement
  Information to Separate Winners from Losers*: accounting-strength signals
  can improve selection within value stocks. [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=249455)
- Fama and French, *A Five-Factor Asset Pricing Model*: operating profitability
  and investment extend the value/market framework. [Journal preprint](https://www.sciencedirect.com/science/article/pii/S0304405X14002323/pdf)
- Kenneth French Data Library: the current annual operating-profitability
  construction is revenue less COGS, interest expense, and SG&A, divided by
  book equity plus minority interest. [Data Library definitions](https://mba.tuck.dartmouth.edu/pages/faculty/ken.french/Data_Library.html)
- Ball, Gerakos, Linnainmaa, and Nikolaev, *Accruals, Cash Flows, and Operating
  Profitability in the Cross Section of Stock Returns*: cash operating
  profitability is a stronger predictor than accrual-inclusive profitability
  and largely subsumes the accrual anomaly. [SSRN](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2587199)
- Sloan-anomaly review and later decay evidence motivate treating accruals as a
  candidate interaction/control rather than assuming a stable stand-alone
  premium. [Review](https://papers.ssrn.com/sol3/papers.cfm?abstract_id=1793364),
  [decay study](https://pure.psu.edu/en/publications/going-going-gone-the-apparent-demise-of-the-accruals-anomaly/)
- Trading-cost evidence motivates explicit turnover measurement before signal
  promotion. [Journal abstract](https://www.sciencedirect.com/science/article/abs/pii/S0165410106000309)

## Production work completed

### Data surface

- Loaded the relevant portion of the local ten-year ticker-history archive.
- Expanded `equity_daily_bars` to 7,380,653 rows, 9,346 securities, and dates
  from 2012-03-26 through 2026-06-15.
- Corrected 322 recycled-ticker/share-class identity collisions during archive
  loading.
- Rebuilt `shares_outstanding_history` from 342 to 186,251 observations.
- Built the interval-keyed `us_common_equity_liquid_v1` governed universe:
  4,554 total intervals, 2,239 member intervals, and 1,033 historical member
  securities. The build is restricted to the 956 symbols used by the current
  fundamental research family and uses positive price, 20 observations of
  history, and trailing dollar volume of at least $1 million.

### Feature layer

`atx_db.fundamental_signals` now materializes four monthly point-in-time series:

| Factor ID | Definition | Current rows | Securities | Dates |
|---|---|---:|---:|---:|
| `profitability_gross_profitability` | annual gross profit / total assets | 73,989 | 589 | 174 |
| `profitability_operating_profitability` | (revenue − COGS − SG&A − interest) / positive common equity | 70,961 | 924 | 176 |
| `value_book_to_market` | common equity / raw-close market capitalization | 99,300 | 956 | 176 |
| `quality_value_gross_profitability` | mean percentile of gross profitability and book-to-market | 73,989 | 589 | 174 |

Counts above are materialized feature counts before the separately governed
universe view. The governed operating-profitability panel contains 70,595 rows,
922 securities, and 175 dates.

All features use the last available close in each calendar month, annual facts
with 330–380 day duration, same-accession/same-period statement joins, the newest
filing visible at formation time, share counts published by formation time, and
a maximum fundamental age of 550 days. Historical revisions remain eligible at
historical dates; filtering source facts to their present-day latest revision is
deliberately avoided. Cross-sections require at least 20 names and apply 1%/99%
winsorization followed by z-scoring.

Operating profitability has an independent cohort—it is not accidentally
restricted to companies that report gross profit. At least one expense component
must be reported; unavailable individual expense components are treated as zero
and disclosed in row lineage. Minority interest is not available and is also
disclosed in lineage.

### Evaluation layer

- `v_factor_panel` is now backed by a live PIT universe instead of returning an
  empty panel.
- Return targets use raw close back-adjusted with the product of future positive
  `split_factor` values. The vendor `adjusted_close` field is not used because it
  is unreliable in the cached sample.
- Return windows are exact 21/63/126/252 trading-observation horizons.
- The evaluator now scopes return construction to panel securities and formation
  keys in DuckDB, accepts an explicit research panel when needed, and persists
  actual PIT universe breadth.
- `factor_ic` now stores Bartlett-kernel Newey–West standard errors and t-stats.
  The default lag count is `ceil(horizon / 21)`, which addresses overlapping
  monthly forward-return windows.
- Reproducible operator commands were added for the governed universe and signal
  evaluation.

## Loop 01 — gross profitability plus value

### Hypothesis

Annual gross profit over assets should forecast returns, and combining it with
book-to-market should improve selection.

### Initial defects found and corrected

The first prototype mixed quarterly gross profit with annual assets, relied on a
price sample whose broad coverage ended in 2015, and allowed extremely stale
fundamentals. Those results were discarded. The final loop used annual-duration
facts, the broadened archive, and the 550-day freshness gate.

### Final result

| Horizon | Mean GP rank IC | HAC t-stat |
|---:|---:|---:|
| 21d | -0.00038 | -0.04 |
| 63d | 0.00445 | 0.29 |
| 126d | 0.00533 | 0.25 |
| 252d | 0.01080 | 0.36 |

At 252 days, the gross-profitability top-minus-bottom decile return was -3.28%
with a 48.4% hit rate and no useful monotonicity. Book-to-market had a -0.0446
rank IC (HAC t = -1.32), and the quality/value composite had a -0.0330 rank IC
(HAC t = -1.35). Size and size-plus-value neutralization did not rescue gross
profitability.

Decision: retain these rows as production data features, but reject all three as
promoted alpha signals in this sample.

Research artifacts:

- `data/signal_loop1_ic_final.parquet`
- `data/signal_loop1_ic_per_date_final.parquet`
- `data/signal_loop1_deciles_final.parquet`
- `data/signal_loop1_turnover_final.parquet`

## Loop 02 — operating profitability

### Hypothesis

The Fama–French operating-profitability numerator captures operating expenses
that gross profit omits and should be more comparable across companies whose
business models differ in the location of costs on the income statement.

### Production result

The table below is read from persisted `factor_ic` rows for run
`fundamental-signals-loop2-20260809` after independent universe gating.

| Horizon | Mean rank IC | Naive t | HAC lags | HAC t | Positive-sign frequency | Mean names |
|---:|---:|---:|---:|---:|---:|---:|
| 21d | 0.02061 | 2.86 | 1 | 2.83 | 56.7% | 404.1 |
| 63d | 0.03054 | 4.38 | 3 | 3.00 | 65.1% | 400.0 |
| 126d | 0.04350 | 6.84 | 6 | 3.45 | 71.7% | 393.8 |
| 252d | 0.05603 | 8.74 | 12 | 3.46 | 75.6% | 380.3 |

| Horizon | Top-minus-bottom decile return | Hit rate | Decile monotonicity |
|---:|---:|---:|---:|
| 21d | 0.294% | 52.0% | 0.527 |
| 63d | 0.890% | 57.4% | 0.515 |
| 126d | 1.282% | 56.6% | 0.673 |
| 252d | 2.068% | 61.9% | 0.600 |

Top-decile monthly turnover is 10.99%, bottom-decile turnover is 15.27%, and
month-to-month rank autocorrelation is 0.99195 over 174 rebalances. Average
feature coverage is 58.0% of the independently constructed governed universe;
coverage is materially thinner near the beginning of the sample.

Decision: promote operating profitability to a validated predictive feature for
continued portfolio research. Do not yet label it deployable net alpha.

Research artifacts:

- `data/signal_loop2_operating_profitability_ic_final.parquet`
- `data/signal_loop2_operating_profitability_ic_per_date_final.parquet`
- `data/signal_loop2_operating_profitability_deciles_final.parquet`
- `data/signal_loop2_operating_profitability_turnover_final.parquet`
- `data/signal_loop2_operating_profitability_hac_final.parquet`

## Remaining threats to validity

1. `delisting_events`, terminal-return data, and
   `forward_returns_survivorship_safe` are empty. Incomplete horizons are dropped,
   so delisted names can create survivorship bias.
2. Industry/classification tables are empty. Results are not industry-neutral and
   may partly reflect structural industry exposures.
3. Minority interest is unavailable for the Fama–French denominator.
4. Treating an individually missing cost component as zero can mismeasure issuers
   with incomplete presentation, even though the row lineage exposes the choice.
5. The sample begins in 2012 and depends on one local market-data archive. It does
   not span several older profitability regimes.
6. The decile spreads are equal-weight research diagnostics before explicit
   transaction costs, borrow constraints, portfolio risk controls, or capacity.

## Next loop — cash operating profitability

Update: this loop is complete; see
`research/2026-08-09-fundamental-signal-improvement-loop-03.md`.

The next hypothesis follows Ball et al.: remove operating accruals from operating
profitability and test whether the cash version subsumes both standard operating
profitability and a separate accrual signal.

Required build sequence:

1. Audit and normalize accounts receivable, inventory, prepaid expenses, deferred
   revenue, accounts payable, and accrued-expense concepts from the statement
   point store.
2. Build exact point-in-time changes from same-duration annual facts without using
   future restatements.
3. Materialize cash operating profitability, operating accruals, and a residualized
   cash-minus-standard-profitability feature with full input lineage.
4. Populate historical industry classifications and evaluate raw, size-neutral,
   and industry-neutral variants.
5. Land delisting/terminal returns and repeat the complete evaluation using the
   survivorship-safe target surface.
6. Promote only variants that retain positive HAC inference, monotone spreads,
   adequate breadth, and plausible net performance after turnover costs.
