# Production metric surface

**Scope:** static repository and retained-evidence review on 2026-09-20. This
report did not open the warehouse, start a process, run tests, or change code.
It separates a defined/catalogued surface from a table that has actually been
materialised in production.

## What is implemented in the declarative catalog

`src/atx_db/seeds/derived_metric_definitions.csv` contains **173** version-1
definitions. The count is exact: 143 quarterly-grid definitions and 30 daily
market definitions. They are loaded through `derived_registry` and migration
0302 into `derived_metric_definitions`, then materialised by the same DSL into
the two consumer tables below.

| Family | Count | Consumer-relevant coverage |
| --- | ---: | --- |
| rollup | 45 | Quarterly core compositions plus TTM revenue, gross profit, operating income, EBITDA, net income/common income, diluted EPS, CFO, capex, FCF, dividends, repurchases/issuance/debt flows, and average balance-sheet denominators. |
| profitability | 18 | Gross/operating/net/EBITDA margin; tax rate and NOPAT; ROA, ROE, ROIC and goodwill-excluded ROIC; gross/cash/operating profitability, asset turnover, CFO/assets, and working-capital changes. |
| growth | 26 | TTM YoY growth for revenue, gross profit, operating income, net income, diluted EPS, CFO, FCF, capex, R&D and tax; asset/share/book growth; two QoQ fields; revenue/EPS/CFO/assets CAGRs; and margin/turnover/ROE changes. |
| quality | 29 | Accrual/NOA measures, earnings variability, Piotroski F, Altman book Z, all eight Beneish components plus M-score, and Ohlson components plus O-score. |
| market | 30 | Daily market cap/EV, P/E/P/B/P/S/P-CFO, EV multiples, payout and value yields, Altman market Z, 1/3/6/12-month adjusted returns, 12-1 momentum, 60/252-day volatility, and 20-day dollar volume. |
| leverage | 8 | Debt/equity, debt/assets, long-term-debt/assets, net-debt/EBITDA, interest coverage, and current/quick/cash ratios. |
| per_share | 8 | TTM EPS, sales, book, tangible book, CFO, FCF, dividends and cash per share. |
| investment | 7 | Capex/depreciation, capex/sales, R&D/sales, net equity/debt issuance, external financing and tax/book-income. |
| payout | 2 | Payout and buyback ratios. |

The windows are **103 TTM**, **31 quarterly**, **9 average-two-period**, and
**30 daily** definitions. Every daily definition is a market-required row;
there are no `annual` or `instant` definitions in this catalog. The seed is the
authoritative list; `derived_metric_values` is long-form, so the 30 daily rows
are deliberately stored instead as the migration-pinned wide daily table.

## Intended point-in-time consumption contract and implementation gap

Controller follow-up found a material gap in the current derived implementation:
`derived_metrics.build_metric_sql` selects only `is_latest_revision` inputs and
refresh replaces the prior metric rows. `market_daily` independently filters
standardized facts and DEI shares to latest-only inputs. The clocks described
below do not restore discarded earlier states. Full historical derived/market
consumption is therefore pending the focused revision-preservation repair;
see `derived-pit-revision-audit.md` when that audit is delivered. The table
describes intended selection rules, not certification of the current engine.

| Use | Production table/view and key | Clock a consumer must enforce | State of the evidence |
| --- | --- | --- | --- |
| Raw comparable fundamentals | `fundamental_standardized`; logical grain `(security_id, item_id, basis, period_end)` with revision history | `available_at` is the maximum input-fact availability. For historical reads, choose a revision visible at the cutoff; do not use today’s `is_latest_revision` alone. | Upstream input to the engine; last retained pre-refresh inventory had no standardized rows. |
| Quarterly ratios, growth, quality and per-share measures | `derived_metric_values`; `(security_id, metric_code, metric_window, period_end)`; `derived_value_id` is the physical PK | `available_at` is lowered with the expression and represents the maximum availability of its inputs. `inputs_hash` records consumed fact/metric values. Filter `available_at <= cutoff`, then select the appropriate revision/source. | Catalog and DDL are implemented. The 19:20 UTC retained snapshot reported 0 rows; it is not a claim about the active source pass. |
| Daily valuation and risk-aware market fields | `market_daily_metrics`; `(security_id, trade_date)`; `market_daily_id` is the physical PK | Bar cutoff is `trade_date + 22h`; fundamental inputs are ASOF-joined only when visible by that cutoff. Row `available_at` is the maximum of the bar, joined fundamental and trailing-window clocks; retain the declared engine `source`. | Catalog and DDL are implemented. The same retained snapshot reported 0 rows. |
| Reproducible consumer extracts | `export_panel_quarterly` and `export_panel_daily_market` | Quarterly export selects revisions visible by the requested cutoff and emits `panel_available_at`. Daily export requires `trade_date <= as_of` and `available_at <= as_of + 22h`. Both write query/schema/file hashes in a manifest. | Code/public schemas are implemented; an export is useful only after its source table is built. |
| Forward labels for research | `v_forward_returns_survivorship_safe` over `forward_returns_survivorship_safe`; `(source, security_id, as_of_date, horizon_days)` | `available_at = max(raw-window availability, terminal-return availability)`; the view only applies latest-revision filtering, so consumers must still enforce its clock. Default production dispatch uses corrected `adjusted_close` and horizons 1/5/10/21/63. | Built contract; last retained pre-refresh inventory had 0 rows. |
| Custom TickerHistory signals | `custom_features_daily`; scoped logical key `(feature_source, feature_version, security_id, decision_date)` | `decision_at=22:00 UTC`; inputs end at the previous observed session; the stored input clock is the max of stored clocks and the conservative noon-UTC next-calendar-day floor. Join labels at the next-session-close `entry_date`. | **Live-built:** `custom-features-build1` has 31,934,514 rows for 34,224 warehouse IDs (2012-03-27..2026-09-17). |
| Decile evidence and promotion state | `custom_feature_deciles` keyed by `(run_id, feature_id, horizon_sessions, decision_date, decile)` and `custom_feature_evaluations` keyed by `(run_id, feature_id, horizon_sessions, split)` | Feature ranks precede label joining; labels use the entry date and their own availability. Primary horizon is 21 sessions; 5/63 are sensitivity horizons. | **Not built:** the post-build inspection found 0 forward labels and 0 evaluation rows; no alpha or production eligibility conclusion exists. |

The public schema names are `ATX.US.FUNDAMENTALS/derived-metrics` and
`ATX.US.EQUITIES/market-daily-1d`. They expose the first two derived surfaces,
with their keys and PIT fields, but do not make a zero-row surface production
ready by themselves.

## Custom price/liquidity feature surface

CF1 contains eight fixed, causal feature columns: five-session reversal,
126-session momentum skipping 21 sessions, volatility-scaled 63-session
momentum, 5-vs-63 dollar-volume shock, 21-session close-location pressure,
5-vs-63 range compression, liquidity-conditioned reversal, and
compression-times-accumulation. The completed build recorded 16,338,033
price/liquidity cohort-eligible rows and no nonfinite feature values. It remains
a bar-observed research cohort, not certified historical US-common membership.

The custom feature table is therefore consumable for PIT feature research now,
subject to its documented modeled-vintage and price-adjustment limits. The
decile/evaluation tables are intentionally not a substitute for a completed
forward-return panel; they must remain empty/uninterpreted until that prerequisite
is built and measured.

## Material core gaps visible from current inputs

The standardized item seed already contains the needed inputs for the following
work: quarterly revenue, gross profit, operating income, net income, diluted and
basic EPS, CFO, capex/FCF, R&D, assets, equity, shares, receivables, inventory,
payables and cost of revenue. These are catalog gaps, not a request for niche
industry templates or a new source.

1. **Quarterly YoY/QoQ growth breadth.** The catalog has TTM YoY for most core
   flows, but QoQ only for `revenue_ttm` and `eps_diluted_ttm`; it has no
   quarterly YoY/QoQ family for gross profit, operating income, net income,
   CFO, FCF, capex or R&D. For earnings-quality and event-driven cross-sections,
   fiscal-quarter growth is a core distinct signal; TTM growth is not a
   substitute.
2. **CAGR breadth.** CAGRs stop at revenue (1y/3y), diluted EPS (3y), CFO (3y)
   and assets (3y). Three-year FCF, book-value/equity, gross-profit,
   operating-income and EBITDA CAGRs are derivable from existing rollups and are
   ordinary professional growth fields.
3. **Per-share and working-capital efficiency completion.** The existing
   per-share set lacks basic-EPS TTM and normalized-income/EPS variants despite
   standardized `eps_basic__1034`, `normalised_income`, and weighted-share
   inputs. The catalog also lacks derived DSO, DIO, DPO and cash-conversion
   cycle despite receivables, inventory, payables, revenue and cost-of-revenue
   inputs (and the corresponding standardized item codes). These are core
   operating-efficiency measures, not industry-specific additions.

## Small follow-on brief

Add one seed-only **core-growth-and-efficiency v1** wave after the current source
pass and before treating the catalog as complete: (a) quarterly YoY and QoQ for
the eight core flow/EPS series above, (b) the five 3-year CAGRs above, and (c)
basic/normalized EPS plus DSO/DIO/DPO/CCC. Use the current declarative DSL,
quarter grid, availability lowering and `derived_metric_values`; no new table,
source, activation stage or market-schema change is required. Validate only the
new definitions’ dependency closure and PIT availability propagation, then let
the normal derived stage materialise them after standardized facts exist.

The existing catalog covers the other requested core ratio, profitability,
quality, leverage, investment, payout, daily valuation and price-feature
families. Catalog breadth alone does not establish correctness: the historical
revision gap above must be repaired before treating the full metrics build as
point-in-time accurate.

## Evidence consulted

- `program.md` production rulings and retained 19:20 UTC inventory.
- S3 derived-engine plan; `derived_metric_definitions.csv`; `derived_registry.py`,
  `derived_metrics.py`, `market_daily.py`, and migration bodies 0302/0303.
- Public catalog, data dictionary, panel-export code, production runbook and
  production-panel dispatch.
- CF1 brief, implementation record, `CUSTOM_FEATURE_RESEARCH.md`, and the
  retained read-only custom-feature inspection.
