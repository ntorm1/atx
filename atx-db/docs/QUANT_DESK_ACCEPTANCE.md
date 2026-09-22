# Quant desk acceptance queries

Development starts with a question an institutional equity long/short desk
needs answered, followed by executable SQL, measured results, and a generic
source or implementation repair. An empty result is a diagnostic, not success.

| Desk question | Query and evidence | Production work exposed |
| --- | --- | --- |
| What was an issuer's reported quarterly EPS growth, using the correct fiscal quarters and information cutoff? | [CVX acceptance case](CVX_EPS_ACCEPTANCE.md), including executed SQL and three measured rows | Activate accounting outputs; supply direct reported Q4 EPS; expose issuer queries with separate lookup and content clocks. |
| Do the eight predeclared price/liquidity features predict 21-session decile spreads after costs, including holdout performance? | [Custom-feature readout SQL](../sql/research/custom-feature-decile-acceptance.sql), prepared but not yet executed | Finish the survivorship-aware forward-return panel, then run the existing CF1 evaluator and publish all results. |

The custom-feature query reads the existing evaluation tables and pins build,
evaluation, source hash, version, horizon, and snapshot. It returns all eight
hypotheses across train, validation and holdout, including missing or failed
results. It reports both the evaluator's statistical screen and production
eligibility. An absent evaluation cannot disappear through an inner join.

The existing evaluator fixes ranks before label attrition, uses chronological
splits with label purging and a 63-session boundary embargo, reports
calendar-aware HAC uncertainty and eight-test Holm correction, and subtracts
four trading legs at 10/25/50 basis points. These are overlapping horizon
return spreads, not a daily portfolio PnL or a trading Sharpe ratio. Cost
scenarios do not measure borrow availability, financing, realized turnover,
market impact, or capacity. Those require portfolio and execution evidence
before deployment.

As of the executed CVX inspection, no custom-feature evaluation or statistically
significant alpha has been measured. The readout SQL will run after the active
bulk writer releases the database and the label/evaluation stages complete.
The query does not create a second feature or backtest engine.

The next fundamentals question is whether an investable cross-section combines
improving quarterly earnings and operating margins with attractive valuation.
It depends on the same accounting outputs, qualified market joins and label
machinery. Its signal definition and evaluation family must be fixed before
looking at forward performance; no post-hoc feature selection or threshold
changes based on holdout results.
