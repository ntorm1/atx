# atx-factor

`atx-factor` is the Polars-native research and portfolio-backtesting layer for ATX's US-equity
fundamental signals. It is deliberately separate from `atx-db`:

- `atx-db` owns point-in-time data, factor definitions, lineage, and governed forward inputs.
- `atx-factor` owns fast cross-sectional portfolio construction, realistic costs, chronological
  walk-forward evaluation, and mega-alpha admission decisions.
- `atx-engine` remains the event-driven execution, optimization, and risk engine used after a
  research book is accepted.

The canonical panel is long-form and uses Polars throughout:

| Column | Type | Meaning |
|---|---|---|
| `date` | `Date` | formation/rebalance date |
| `asset_id` | `String` | stable security identifier |
| `signal` | `Float64` | point-in-time signal value |
| `forward_return` | `Float64` | return beginning after formation |
| `available_at` | `Datetime` (optional) | signal knowledge timestamp; must not exceed formation day |
| `forward_end_date` | `Date` (optional) | label endpoint; must be after formation |
| `adv_usd` | `Float64` (optional) | average daily dollar volume for impact |
| `borrow_rate` | `Float64` (optional) | annualized decimal borrow rate |
| `group` | `String` (optional) | point-in-time industry/sector neutralization group |

Research defaults build a continuous dollar-neutral rank book at gross 1.0, cap names at 5%,
charge commissions, spread, square-root participation impact, and short borrow, then evaluate only
chronological out-of-sample folds. Candidate admission also requires marginal improvement over the
current mega-alpha, survival under doubled costs, and compliance with the configured ADV
participation ceiling. Capacity breaches remain visible in the report and reject admission rather
than aborting the rest of the diagnostic run. If the requested AUM cannot support gross 1.0, both
long and short sides are under-deployed symmetrically and the resulting minimum gross deployment is
an explicit admission gate. By default, one position may consume only 25% of the per-trade ADV
ceiling, reserving room for a full signal flip and an approximate 2x deterioration in liquidity.

## Development

```powershell
uv sync --extra dev
uv run pytest -q -n 0 tests/test_portfolio.py tests/test_backtest.py
uv run ruff check .
```

Full repository test suites are not required for this standalone package; targeted tests are the
supported iteration path.

## Governed candidate evaluation

```powershell
uv run atx-factor evaluate-candidate `
  --db-path C:\atx\atx-db\data\warehouse.duckdb `
  --candidate profitability_quarterly_operating_profitability_change_yoy `
  --trial-count 32 `
  --output research\candidate-decision.json
```

The command reads DuckDB in read-only mode, emits an atomic JSON decision artifact, and returns
zero only when every admission gate passes. A normal research rejection returns code 2. Accepted
signals can additionally be admitted to an atomic registry with `--registry`; rejected signals are
never written to that registry.
