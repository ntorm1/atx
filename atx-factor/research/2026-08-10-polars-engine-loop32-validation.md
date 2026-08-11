# Polars engine validation: Loop 32 NOA-proxy turnover change

Date: 2026-08-10

Candidate: `efficiency_annual_noa_proxy_turnover_change`

Baseline: `composite_operating_profitability_or_net_issuance`

The governed candidate was evaluated from the read-only ATX DuckDB adapter at $100 million AUM,
21 trading-day holding periods, 10% ADV participation, 5% name caps, and the production cost model.
Nine disjoint 12-month test folds supplied 108 out-of-sample observations after a 60-period minimum
training window and one-period embargo.

| Portfolio | Sharpe | Annual return | Max drawdown | DSR probability | Min gross | Max participation |
|---|---:|---:|---:|---:|---:|---:|
| Candidate | 0.066 | 0.159% | -13.21% | 0.0296 | 20.0% | 8.69% |
| Router v6 | 0.580 | 1.893% | -9.00% | 0.3682 | 83.8% | 13.12% |
| 80/20 blend | 0.592 | 1.947% | -8.82% | 0.3802 | 83.8% | 12.56% |
| Blend, doubled costs | 0.493 | 1.604% | -9.02% | 0.2728 | 83.8% | 12.56% |

Marginal blend Sharpe is +0.0118 versus a +0.05 requirement. Candidate/baseline OOS return
correlation is 0.165. The engine rejects admission for candidate Sharpe, deflated Sharpe, marginal
improvement, participation, candidate deployment, and combined deployment failures. No registry
mutation was made.

Decision artifact: `research/loop32-noa-proxy-mega-alpha-decision.json`

Evidence digest: `26c346cbf1e0d4bc7c033b2ccc3391f7f825662ed09e4911d98830500eaf0fbd`
