# atx-db

`atx-db` is the Python data platform for point-in-time US equity fundamentals,
market data, ownership, estimates, corporate actions, and public alternative
datasets. It provides a revision-aware DuckDB warehouse, governed migrations,
data-quality gates, deterministic factor construction, and reproducible research
surfaces.

The import namespace is `atx_db`. Runtime databases and lake exports live under
`data/` in a source checkout and are never packaged or committed. Set
`ATX_DB_PATH` to select an explicit warehouse file, or `ATX_DB_DATA_DIR` to select
its parent directory.

## Development

```powershell
cd C:\atx\atx-db
python -m pip install -e ".[dev]"
python -m pytest tests/test_import.py tests/test_module_boundaries.py -q -n0
```

Use focused test modules during development. Slow integration tests remain
opt-in with `--run-slow`.

## Data safety

All facts intended for research carry point-in-time availability and source
lineage. Production migrations must use `scripts/warehouse_migrate.py`, which
checkpoints and backs up the warehouse before mutation. The embedded DuckDB file
has a single-writer operating model; ingestion is serialized while readers use
read-only connections or published lake snapshots.

Historical architecture and parity records are under `docs/`; sprint briefs are
retained under `plans/` as implementation evidence.

## Operations

Install the package, identify SEC traffic, and inspect warehouse readiness:

```powershell
python -m pip install -e ".[dev]"
$env:ATX_SEC_USER_AGENT = "atx-db/0.2 data-operations@your-domain.example"
atx-db status --strict
```

The native archive loader streams downloads, validates every ZIP, stages only
the four required TSV members, and lets DuckDB parse them directly:

```powershell
atx-db backfill-13f --start 2013-04-01
atx-db refresh-13f-amendments --skip-effective-positions
atx-db refresh-13f-signals
atx-db map-13f-signal-instruments
atx-db load-13f-signal-prices --archive-path C:\path\to\tbltickerhistory3_10y.zip
atx-db backtest-13f-signals
python scripts/recreate_l1vsun_13f_analysis.py
```

`RESTATEMENT` amendments replace the prior information table. `ADD NEW
HOLDINGS` amendments supplement it. The materialized amendment-rate z-score is
lookback-only and excludes the current quarter from its 24-quarter baseline.
The fast amendment path scans and materializes only amended chains; omit
`--skip-effective-positions` when the complete manager-quarter position product
is also required. Mapping and backtests default to the top 20 ranked candidates
per quarter in non-stress regimes; both the rank cutoff and stress inclusion are
explicit CLI options because the source post does not disclose a portfolio
capacity cutoff.
