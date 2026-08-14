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

## Data API

The versioned read API exposes only curated dataset/schema contracts; it never
accepts customer SQL or warehouse table names. Install the server extra, configure a
high-entropy API key, and serve a checkpointed warehouse snapshot:

```powershell
python -m pip install -e ".[server]"
$env:ATX_DB_PATH = "C:\data\atx-serving.duckdb"
$env:ATX_DB_API_KEYS_JSON = '{"atx_live_replace_me":{"account_id":"local","key_id":"local-1","scopes":["data:read","batch:read","batch:write"],"datasets":["ATX.US.FUNDAMENTALS"]}}'
atx-db-api
```

For restart-safe keys, schema-level entitlements, usage events, and batch jobs, initialize
a writable local control database and point artifact delivery at a separate directory:

```powershell
atx-db-control --path C:\data\atx-control.duckdb init
atx-db-control --path C:\data\atx-control.duckdb upsert-account --account-id research --name "Research Team"
atx-db-control --path C:\data\atx-control.duckdb grant --account-id research --dataset ATX.US.FUNDAMENTALS --schemas reported,standardized,ratios
atx-db-control --path C:\data\atx-control.duckdb set-price --dataset ATX.US.FUNDAMENTALS --schema standardized --usd-per-gb 2.50
atx-db-control --path C:\data\atx-control.duckdb issue-key --account-id research
$env:ATX_DB_CONTROL_PATH = "C:\data\atx-control.duckdb"
$env:ATX_DB_BATCH_ARTIFACT_DIR = "C:\data\atx-batch"
atx-db-api
```

To move artifact generation out of the API process, disable inline execution and run one
or more lease-based workers against the same control database and artifact store:

```powershell
$env:ATX_DB_BATCH_INLINE = "false"
atx-db-api
atx-db-worker --warehouse-path C:\data\atx-serving.duckdb --control-path C:\data\atx-control.duckdb --artifact-root C:\data\atx-batch
```

`issue-key` prints the secret once; the database retains only its prefix and SHA-256
digest. The v1 contract provides metadata discovery, historical symbology, bounded
point-in-time range queries, durable batch jobs, and checksummed Parquet, Arrow, CSV,
and JSONL downloads under `/v1`. Preflight methods expose record count, uncompressed
Arrow billable size, and configured USD cost before retrieval. Mutating batch submissions
accept an account-scoped `Idempotency-Key`; quota failures return `429` with reason and
retry headers. See
[`docs/SAAS_PLATFORM_ARCHITECTURE.md`](docs/SAAS_PLATFORM_ARCHITECTURE.md) for the
canonical model, API semantics, production topology, and delivery roadmap.

## Operations

Install the package, identify SEC traffic, and inspect warehouse readiness:

```powershell
python -m pip install -e ".[dev]"
$env:ATX_SEC_USER_AGENT = "atx-db/0.2 data-operations@your-domain.example"
atx-db status --strict
```

Build the revision-complete standardized fundamentals surface after statement and TTM
refreshes. Omit `--symbol` for the full warehouse; repeated filters are useful for proof
slices and incremental recovery:

```powershell
atx-db refresh-standardized-fundamentals --memory-limit 8GB --threads 4
atx-db refresh-standardized-fundamentals --symbol AAPL --symbol MSFT
atx-db refresh-provider-coverage
```

The command reports the build ID, rule-set digest, candidate/output/exception counts, and
basis distribution. Every build is also persisted in
`fundamental_standardization_builds`; the public `standardized` schema is version 2.0.0 and
retains original/restated revision chains rather than only the current value. Quarterly
output includes PIT-correct Q2/Q3/Q4 derivations from cumulative YTD filings and switches to
a directly reported discrete quarter when one becomes available.

Two additional fundamentals contracts sit on top of that revision stream. The
`industry-standardized` schema applies revision-complete `ALL`, bank, insurer, REIT,
utility, and broker-dealer statement routing. The `reconciliation` schema evaluates 19
governed accounting identities at every input or classification event, exposes exact
weighted-input lineage and tolerances, and distinguishes hard `mismatch` results from
diagnostic scope differences and PIT `not_applicable` transitions.
`refresh-provider-coverage` then appends the measured inclusive-start/exclusive-end
range, record/security/item breadth, freshness, target failures, and
`available`/`degraded`/`pending`/`missing` condition for every public schema.

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

For institutional price breadth, extract the local ticker-history archive once
and publish its complete canonical projection in one governed bulk operation:

```powershell
tar -xf $HOME\Downloads\tbltickerhistory3_10y.zip -C data\staging\broad-bars
atx-db publish-broad-bars --tsv-path data\staging\broad-bars\tbltickerhistory3_10y.txt
```

The bulk path reads only OHLCV, split factor, identifiers, and shares; validates
at least 30 million clean rows, 10,000 securities, and 5,000 latest-date names;
resolves recycled ticker/share-class collisions; and publishes the replacement
transactionally. Canonical bars carry point-in-time `shares_outstanding` and
`market_cap_usd`, so downstream capacity and 13F screens do not depend on the
wide raw vendor table.

`RESTATEMENT` amendments replace the prior information table. `ADD NEW
HOLDINGS` amendments supplement it. The materialized amendment-rate z-score is
lookback-only and excludes the current quarter from its 24-quarter baseline.
The fast amendment path scans and materializes only amended chains; omit
`--skip-effective-positions` when the complete manager-quarter position product
is also required. Mapping and backtests default to the top 20 ranked candidates
per quarter in non-stress regimes; both the rank cutoff and stress inclusion are
explicit CLI options because the source post does not disclose a portfolio
capacity cutoff.
