# Next desk acceptance: earnings improvement, valuation and cash quality

## Question and scope

At the production snapshot (2026-09-20, information cutoff22:00UTC), which
qualified listed US common equities have improving reported quarterly diluted
EPS and trailing operating margins, and what are their earnings yield,
operating cash-flow yield, accruals and leverage? How many otherwise eligible
names cannot be answered, and exactly which required input or join is absent?

Implement a read-only SQL acceptance artifact and short evidence document,
not a new factor engine or another backtester. This task follows the current
EPS/issuer-query integration and the full-universe materialization. Root will
execute it in the single guarded runtime slot; do not probe the live writer.
Freeze the SQL and source/version/as-of pins before observing forward returns.

## Expected warehouse surfaces

- `universe_us_listed_membership`: membership valid at the snapshot, information
  visible at the cutoff, with the production US-common universe identifier.
  Read the actual schema/seed/source contract; do not invent the universe ID.
- `market_daily_metrics`: the latest eligible observed session on or before
  the snapshot, and its own maximum-input availability at or before cutoff.
  Report the actual trade date; do not label a weekend snapshot as a session.
- `derived_metric_values`: latest eligible state per metric/period at cutoff,
  retaining NULL/unavailable/conflict states before ranking. Required codes:
  `eps_diluted_q_growth_yoy`, `operating_margin_change_yoy`, `total_accruals`,
  `net_debt_ebitda`. Market fields: `earnings_yield`, `cfo_to_ev`, `market_cap`.
- Preserve evidence owner and actual qualified security joins. A current
  ticker-directory CIK association is not historical security qualification.
  Do not glue owners with an inferred ticker or backdated current mapping.

## Outputs

1. An aggregate diagnostic row always exists, even if membership is empty.
   Count visible members, usable price/valuation matches, each metric, complete
   rows, invalid current states, stale periods and missing security joins.
   Empty universe must be a named failure, never an empty successful report.
2. A bounded name-level preview (maximum1000 output rows), with current ticker
   display only if separately qualified, each raw metric value/status, actual
   quarter end, price date, availability and lineage. Deterministic ordering.
3. Optional descriptive positive-EPS-growth/positive-margin-change filter
   reports both passes and exclusions. No claim this filter predicts returns.
   No new alpha threshold, optimization or holdout-driven feature selection.
4. Separate accounting missingness from identity or membership missingness.
   Do not lower production gates or replace unavailable values with zero.

One independent review, focused SQL/schema checks only after code is stable.
Measure current gaps and turn observed production blockers into bounded fixes.
The eight existing price/liquidity hypotheses and their planned21-session
decile evaluation remain the first statistical evaluation; no duplicate
significance engine is authorized by this readout task.
