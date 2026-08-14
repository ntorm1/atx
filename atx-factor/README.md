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
Candidate admission also requires at least 95% deflated-Sharpe probability, stricter than the
engine's original 90% research prototype.

New characteristics must pass through bounded construction exploration before final admission.
The search space is committed to `research/trial-ledger.json` before returns are read: continuous
rank, quintile tails, decile tails, continuous raw-score, top-quintile-versus-universe, and
universe-versus-bottom-quintile books, each at 10%, 20%, and 30% candidate allocations. Every
construction/allocation pair tests both a capital-weighted mix of independently built sleeves and
an integrated stock-level blend of the two cross-sectional rank scores. Missing style observations
receive a neutral score, so integration preserves the eligible-name union without manufacturing a
favorable rank. Both modes count as trials in the durable ledger. The two
asymmetric books isolate whether predictiveness resides in only the long or short extreme while
remaining dollar neutral. The best feasible early-history construction is frozen, one rebalance is
embargoed, and only that winner is evaluated on the untouched final 40% of history. The final gate
uses the cumulative ledger trial count in the deflated Sharpe ratio and also requires positive fold
stability, realistic costs, doubled-cost survival, full deployment, capacity, drawdown control, and
marginal Sharpe improvement over the production mega-alpha. This separates "predictive
characteristic" from "deployable portfolio sleeve" without silently discarding a signal because one
arbitrary weighting rule failed.

Capacity is allocation-aware: a candidate tested at allocation `a` is constructed and costed as a
sleeve managing `a * total_aum`, while the blended portfolio is renormalized and capacity-checked
at total AUM. The durable trial specification records this capacity-model version. A sleeve that
cannot deploy its own gross budget at the selected allocation remains ineligible even if the
baseline can absorb the unused capital and make the blended book appear fully invested.

Weight construction is physically phase-separated: losing variants are built only on selection
dates, and validation-date weights are constructed only after the winner is frozen. This avoids
both wasted full-history normalization and accidental holdout preprocessing while preserving the
same per-date capacity and cost rules in each phase.

If every economic, capacity, cost, drawdown, correlation, and fold gate passes but cumulative-trial
DSR alone remains below 95%, a candidate may enter the zero-capital shadow registry. Shadow status
is not acceptance: it freezes the selected construction and records the evidence digest so only new
untouched formation dates can raise confidence. Production promotion still requires every original
gate; validation-period retuning is prohibited.

## Development

```powershell
uv sync --extra dev
uv run pytest -q -n 0 tests/test_portfolio.py tests/test_backtest.py
uv run ruff check .
```

Full repository test suites are not required for this standalone package; targeted tests are the
supported iteration path.

## Governed candidate evaluation

For a newly researched feature, use the construction-search command first:

```powershell
uv run atx-factor explore-candidate `
  --db-path C:\atx\atx-db\data\warehouse.duckdb `
  --candidate composite_cash_profitability_level_growth `
  --trial-ledger research\trial-ledger.json `
  --summary-only `
  --output research\candidate-exploration.json
```

The grid is selected only on the first 60% of common formation dates. One period is embargoed and
at least 48 untouched monthly observations are required for the final decision. The command is
currently restricted to monthly 21-trading-day returns; longer overlapping labels remain IC-decay
diagnostics and are not misrepresented as independently investable monthly portfolio returns.
At least one selection-grid construction must have positive standalone candidate Sharpe while
meeting turnover and capacity ceilings. An unsupported fallback winner may still be evaluated for
diagnostics, but it cannot enter either production or the shadow registry.
Production admission also requires median validation signal breadth of at least 1,000 names.
Candidates with at least 100 names may remain in shadow when institutional breadth and/or DSR are
their only failures. Every exploration artifact reports raw eligible names, nonzero holdings, and
the gross-weight effective number of bets so apparent diversification is not inferred from row
counts alone.

For a construction already frozen independently, use the direct admission command:

```powershell
uv run atx-factor evaluate-candidate `
  --db-path C:\atx\atx-db\data\warehouse.duckdb `
  --candidate profitability_quarterly_operating_profitability_change_yoy `
  --trial-count 32 `
  --summary-only `
  --output research\candidate-decision.json
```

The command reads DuckDB in read-only mode, emits an atomic JSON decision artifact, and returns
zero only when every admission gate passes. A normal research rejection returns code 2. Accepted
signals can additionally be admitted to an atomic registry with `--registry`; rejected signals are
never written to that registry. `--summary-only` keeps console output compact while the complete,
immutable decision remains in `--output`.

## Governed incumbent replacement

Use a distinct challenge when a strong factor is too correlated to qualify as
an additive sleeve:

```powershell
uv run atx-factor evaluate-replacement `
  --db-path C:\atx\atx-db\data\warehouse.duckdb `
  --challenger profitability_operating_profitability `
  --aum-usd 50000000 `
  --trial-count 32 `
  --summary-only `
  --output research\replacement-decision.json
```

Replacement requires a 0.95 deflated-Sharpe probability and at least 0.05
standalone Sharpe improvement over the incumbent. It retains the same cost,
capacity, deployment, drawdown, turnover, and chronological-fold controls but
does not use the additive-sleeve correlation gate.
Necessary gates are evaluated sequentially: an absolute challenger failure
serializes a checksummed rejection immediately, and cost stress runs only after
the challenger also beats the incumbent. Skipped stages are explicit in the
decision artifact.

## Capacity frontier

Calibrate AUM independently of candidate returns before freezing an admission run:

```powershell
uv run atx-factor capacity-frontier `
  --db-path C:\atx\atx-db\data\warehouse.duckdb `
  --aum-grid 1000000 2000000 5000000 10000000 20000000 50000000 100000000 `
  --output research\router-capacity.json
```

Each grid point reports participation, minimum and average gross deployment, turnover, cost drag,
return, and Sharpe. The reported maximum feasible AUM is the largest predeclared point satisfying
both the participation ceiling and gross-deployment floor.
