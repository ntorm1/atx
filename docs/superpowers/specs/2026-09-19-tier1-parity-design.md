# atx-db Tier-1 Parity Design (US equities, quant-systematic core)

Date: 2026-09-19. Status: design contract for the Tier-1 parity sprint series.
Successor to `atx-db/docs/FUNDAMENTALS_PROVIDER_DESIGN.md` (migration 0297,
measured 2026-08-15) and `atx-db/plans/pf4/ROADMAP.md`.

## Objective

Make `atx-db` a competitive replacement for the institutional standardized
fundamentals + security-master + price/corporate-action stack (Compustat /
Capital IQ Fundamentals, FactSet Fundamentals, Worldscope, CRSP) for the
subset a US-equity quant systematic shop actually consumes:

1. **All listed US equities** (common stock, ADR, REIT, LP units; exclude ETFs
   from the fundamentals universe but keep them in the security master),
   including delisted names, survivorship-free.
2. **Point-in-time (PIT) fundamentals**: every value addressable by
   `(security_id, item, fiscal_period, available_at)`; original and restated
   values preserved; "as-of" queries return exactly what a consumer could have
   known at the as-of timestamp.
3. **Standardized statement items** at Compustat-core breadth (see item
   catalog) with deterministic tag→item rules, industry-aware templates for
   banks, insurers, REITs, utilities, and validated accounting identities.
4. **Derived metrics** (ratios, per-share, growth, TTM, rate-of-change,
   quality/accrual/leverage families) computed PIT-correctly from the
   standardized layer and joined to market data on the same clock.
5. **Market data & corporate actions** from the local `tbltickerhistory`
   archive (OHLCV, shares, cumulative adjustment factor) unified with the SEC
   entity/security master so every fundamentals row can be priced.
6. **Deterministic, reproducible builds**: pure functions over warehouse
   tables, no wall-clock in derived paths, stable ordering, manifest hashes.

Out of scope: analyst estimates, transcripts, non-XBRL KPI extraction from
presentations, any paid LLM API. ML is allowed for tag-classification
assist only where deterministic rules are recorded as the final artifact.

## Clocks and PIT contract

Every fact/derived row carries four times:

| Column | Meaning |
| --- | --- |
| `period_end` | fiscal period end (economic date) |
| `filed_at` | SEC acceptance datetime (UTC) of the filing that revealed the value |
| `available_at` | earliest datetime a PIT consumer may use the row; = `filed_at` for filing facts; for derived rows = max(available_at of inputs) |
| `loaded_at` | warehouse load time (lineage only, never used for signals) |

Rules:
- A derived value's `available_at` is the max over its inputs, including price
  data (`trade_date` end-of-day → available at that date's close + 0).
- Restatements produce a new row with a later `available_at`; the prior row is
  never deleted. `revision_seq` increments per (security_id, item, period_end).
- `as_of(ts)` views select the latest `revision_seq` with `available_at <= ts`.
- Fiscal calendarization: keep native fiscal periods; provide calendar-quarter
  alignment as a derived mapping, never by mutating the period key.

## Identity

- `security_id` (stable warehouse integer) is the primary key; `cik` is the
  entity key; ticker/CUSIP are dated mappings in `security_identifier_history`.
- Ticker-history archive rows join via a dated `(ticker, trade_date)` →
  `security_id` mapping resolved from SEC company tickers, Nasdaq directory,
  and 13F CUSIP evidence. Unresolved tickers are retained with
  `security_id IS NULL` and counted in coverage, never dropped.
- Share classes: one `security_id` per listing line; fundamentals attach to
  the entity (`cik`) and fan out to each security via `entity_security_map`
  with `primary_flag` for market-cap aggregation.

## Canonical item catalog (target: Compustat-core parity)

Item codes are provider-neutral snake_case; `cs` column gives the Compustat
mnemonic used only for documentation/benchmarking.

### Income statement (quarterly + annual)

| item | cs | notes |
| --- | --- | --- |
| revenue | sale/revt | total revenues |
| cost_of_revenue | cogs | |
| gross_profit | gp | derived if missing: revenue - cost_of_revenue |
| sga_expense | xsga | |
| rd_expense | xrd | |
| depreciation_amortization | dp | |
| operating_income | oiadp | |
| ebitda | oibdp | derived: operating_income + depreciation_amortization |
| interest_expense | xint | |
| nonoperating_income | nopi | |
| special_items | spi | |
| pretax_income | pi | |
| income_tax | txt | |
| income_before_extraordinary | ib | |
| minority_interest_income | mii | |
| net_income | ni | |
| net_income_common | ibcom | after preferred dividends |
| discontinued_operations | do | |
| extraordinary_items | xido | |
| eps_basic | epspx | |
| eps_diluted | epsfx | |
| shares_basic_weighted | cshpri | |
| shares_diluted_weighted | cshfd | |
| dividends_common | dvc | |
| dividends_preferred | dvp | |
| stock_compensation | stkco | |

### Balance sheet

| item | cs |
| --- | --- |
| cash_and_equivalents | che |
| short_term_investments | ivst |
| receivables | rect |
| inventory | invt |
| other_current_assets | aco |
| total_current_assets | act |
| ppe_gross | ppegt |
| ppe_net | ppent |
| goodwill | gdwl |
| intangibles | intan |
| long_term_investments | ivao |
| other_assets | ao |
| total_assets | at |
| accounts_payable | ap |
| short_term_debt | dlc |
| taxes_payable | txp |
| other_current_liabilities | lco |
| total_current_liabilities | lct |
| long_term_debt | dltt |
| deferred_taxes | txditc |
| other_liabilities | lo |
| total_liabilities | lt |
| minority_interest | mib |
| preferred_stock | pstk |
| common_equity | ceq |
| retained_earnings | re |
| treasury_stock | tstk |
| stockholders_equity | seq |
| shares_outstanding | csho |
| accumulated_depreciation | dpact |

### Cash flow

| item | cs |
| --- | --- |
| cfo | oancf |
| capex | capx |
| acquisitions | aqc |
| investing_cash_flow | ivncf |
| dividends_paid | dv |
| share_repurchase | prstkc |
| share_issuance | sstk |
| debt_issuance | dltis |
| debt_reduction | dltr |
| financing_cash_flow | fincf |
| change_in_cash | chech |
| cf_depreciation | dpc |
| cf_stock_compensation | sc |
| deferred_tax_cf | txdc |
| working_capital_change | wcapc |

### Supplemental / industry

| item | applies |
| --- | --- |
| net_interest_income, interest_income, interest_expense_bank, provision_for_loan_losses, loans_net, deposits, allowance_for_loan_losses, noninterest_income, noninterest_expense | banks |
| premiums_earned, benefits_and_claims, policy_reserves, investment_income_insurance | insurers |
| rental_revenue, ffo, real_estate_investments_net | REITs |
| operating_lease_liabilities, finance_lease_liabilities, capitalized_software | all |
| employees | all (annual) |

Target published breadth: ≥ 110 items with ≥ 90% coverage on the top-3000
by market cap for FY2015+, measured and published as coverage metrics.

## Derived metric catalog (PIT, from standardized layer + prices)

Computed by ONE generic engine from a declarative registry
(`derived_metric_definitions`), not per-metric modules. Each definition has:
`metric`, `expression` (restricted arithmetic DSL over items/metrics), `window`
(`q`, `ttm`, `annual`, `avg2` = average of current and 4-quarters-prior
balance), `requires` (list), `family`.

Families and members:

- **TTM/rollups**: every flow item gets `_ttm` (sum of last 4 quarterly with
  fiscal-year fallback), balance items get `_avg2`.
- **Per-share**: eps_ttm, sales_per_share, book_per_share, cfo_per_share,
  fcf_per_share, dividends_per_share (all on `shares_outstanding` PIT and on
  weighted diluted).
- **Market**: market_cap (price × shares_outstanding, using dated shares from
  either XBRL dei or ticker-history shares, with source flag), enterprise_value
  = market_cap + total_debt + preferred + minority − cash, ev_ebitda,
  ev_sales, pe_ttm, pb, ps_ttm, pcf_ttm, fcf_yield, dividend_yield,
  earnings_yield, shareholder_yield.
- **Profitability**: gross_margin, operating_margin, net_margin, ebitda_margin,
  roa, roe, roic, roic_ex_goodwill, gross_profitability (gp/at, Novy-Marx),
  cash_profitability, asset_turnover.
- **Growth / rate-of-change** (yoy and qoq, 1y/3y CAGR): revenue, gross_profit,
  operating_income, net_income, eps_diluted, cfo, fcf, total_assets,
  shares_outstanding (net issuance), book_value, capex, employees.
- **Leverage / liquidity**: total_debt, net_debt, debt_to_equity,
  debt_to_assets, net_debt_ebitda, interest_coverage, current_ratio,
  quick_ratio, cash_ratio.
- **Quality / accruals**: total_accruals (Sloan), percent_accruals,
  noa (net operating assets), delta_noa, piotroski_f, altman_z, beneish_m,
  ohlson_o, earnings_variability (std of yoy eps growth over 12q).
- **Investment**: asset_growth, capex_to_depreciation, capex_to_sales,
  external_financing, net_equity_issuance, net_debt_issuance.
- **Payout**: payout_ratio, buyback_yield, total_payout_yield.

Each derived row carries `available_at` = max of input availabilities and
`inputs_hash` (sha256 over the sorted (item, period_end, revision_seq, value)
tuples used) so any value is reproducible and attributable.

## Market data from `tbltickerhistory`

The archive provides per (ticker, trade_date): OHLC, volume, shares
outstanding, cumulative return factor (corporate-action adjusted). Loader
contract:

- Stream the zip; never fully extract; batch-insert into `equity_daily_bars`
  keyed by `(security_id nullable, ticker, trade_date)`; retain raw ticker.
- Derive `adj_close = close × cum_factor / cum_factor_latest` view; derive
  `total_return` daily from cum factor ratios.
- Derive `split_ratio` and `dividend_cash` events from day-over-day changes in
  cum factor vs price gap where separable; otherwise record `corporate_action`
  rows with `kind='adjustment'` and the factor ratio.
- `shares_outstanding_daily` from archive shares, reconciled against XBRL
  `dei:EntityCommonStockSharesOutstanding` with a `shares_source` flag.
- Delisting: last trade_date per ticker without a later re-listing →
  `delisting_events` with inferred delist date; CRSP-style delisting return
  placeholder = −30% for performance-related delistings when unknown
  (documented convention, configurable).

## Universe

`universe_us_listed(as_of_date)`: securities with a trade in the prior 20
trading days, exchange in {NYSE, NASDAQ, AMEX/NYSE American, ARCA, BATS},
security_type in {common, ADR, REIT, LP}, resolved `cik` (for fundamentals
universe) — plus the unresolved tail reported separately. Point-in-time index
membership is out of scope; market-cap deciles are provided.

## Quality gates (published metrics, not claims)

- Identity checks per filing: assets = liabilities + equity (+ minority);
  gross_profit = revenue − cost_of_revenue; net_income ties to CF start;
  cfo + cfi + cff + fx = change_in_cash. Tolerance 0.5% or $1M.
- Coverage per item per fiscal year over the universe; publish `degraded`
  when < target.
- Cross-source shares check: archive shares vs dei shares within 5% on
  overlapping dates for ≥ 95% of securities.
- Benchmark: Sharadar sample (public) and SEC Financial Statement Data Sets
  `num.txt` for spot checks of 50 tickers × 5 years, recorded in tests as
  fixtures (no network in tests).

## Serving

- Wide PIT panel exports: `panel_quarterly(as_of, items[])`,
  `panel_daily_market(as_of, metrics[])` to Parquet with manifests.
- Existing API/schema catalog registers new schemas: `fundamentals_core`,
  `derived_metrics`, `market_daily`, `security_master`, `delistings`.

## Non-goals restated

No LLM API spend. No new external data vendors. No estimates. No global.
