# Fundamental signal improvement loop 45: production capacity frontier

Status: preregistered; no Loop 45 capacity result inspected at registration.

## Primary research and production problem

Frazzini, Israel, and Moskowitz measure live institutional execution costs and
show that cost-aware construction materially increases anomaly capacity
(https://papers.ssrn.com/sol3/papers.cfm?abstract_id=2294498). Almgren, Thum,
Hauptmann, and Li estimate concave equity-market impact from institutional
orders, supporting an explicit participation-sensitive cost model.

Loops 40-44 revealed a production issue independent of candidate quality: at
$100 million, router v6 itself reached 13.12% ADV participation and only 83.81%
minimum gross deployment. Thus every blend inherited two automatic capacity
failures. Those failures must not be hidden by relaxing gates after seeing a
candidate.

## Frozen engineering and analysis

Add a reusable Polars-native capacity-frontier API and CLI to `atx-factor`.
For one factor panel it must rebuild the same constrained portfolio and costed
backtest at this predeclared AUM grid:

`$1m, $2m, $5m, $10m, $20m, $50m, $100m`.

Each point must report maximum ADV participation, minimum/average gross
deployment, turnover, annualized cost drag, net return, and Sharpe. A point is
feasible only when maximum participation is <=10% and minimum gross deployment
is >=95%. The maximum feasible grid AUM becomes the honest baseline capacity
for future preregistered candidate tests; prior $100m rejections remain
immutable and are not rerun or reclassified.

The tested factor is production router v6,
`composite_operating_profitability_or_net_issuance`, at the 21-day horizon with
the existing 0.25 bps commission, 2 bps half spread, 10 bps square-root impact,
50 bps annual borrow, gross 1.0, 5% name cap, and minimum 20 names.

Loop 45 is infrastructure/capacity calibration, not a signal-admission test. No
mega-alpha registry change is permitted.

## Implementation and result

Added `atx_factor.capacity` with immutable `CapacityPoint` and
`CapacityFrontier` result contracts, `evaluate_capacity_frontier`, JSON
serialization, public exports, a `capacity-frontier` CLI, README usage, and a
focused synthetic test. The test and targeted DuckDB-adapter test pass; Ruff is
clean on every touched file.

The fixed seven-point router-v6 run completed in 27.9 seconds. The maximum
feasible grid AUM is **$50 million**:

| AUM | Feasible | Max ADV participation | Min gross deployment | Net Sharpe |
|---:|:---:|---:|---:|---:|
| $1m | yes | 0.66% | 100.00% | 0.372 |
| $2m | yes | 1.32% | 100.00% | 0.372 |
| $5m | yes | 2.50% | 100.00% | 0.371 |
| $10m | yes | 4.82% | 100.00% | 0.365 |
| $20m | yes | 7.49% | 100.00% | 0.369 |
| $50m | yes | 9.21% | 100.00% | 0.398 |
| $100m | no | 13.12% | 83.81% | 0.446 |

The non-monotonic net Sharpe is not used to select capacity. Feasibility uses
only the frozen participation and deployment constraints. The immutable output
is `C:\atx\atx-factor\research\loop45-router-v6-capacity-frontier.json`.

Future candidate loops will use $50 million unless a new candidate has a lower
candidate-specific frontier. Prior $100 million decisions remain rejected and
are not reclassified. No mega-alpha registry was created or changed.
