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
retry headers. Every accepted batch pins the normalized query, `as_of`, public schema
version, and schema SHA-256. Generation adds an encoding-independent logical-content
SHA-256 plus byte-level artifact and manifest hashes. Completed jobs expose
`/v1/batch.get_manifest`; that small audit artifact remains account-scoped and available
after the paid data file expires. See
[`docs/SAAS_PLATFORM_ARCHITECTURE.md`](docs/SAAS_PLATFORM_ARCHITECTURE.md) for the
canonical model, API semantics, production topology, and delivery roadmap.

## Operations

Install the package, identify SEC traffic, and inspect warehouse readiness:

```powershell
python -m pip install -e ".[dev]"
$env:ATX_SEC_USER_AGENT = "atx-db/0.2 data-operations@your-domain.example"
atx-db status --strict
```

Load complete SEC filing histories for the full issuer universe from the official
bulk archive without per-issuer API traffic. The loader reads each
`CIK##########.json` member plus its paginated history members, applies the same
normalization and replace semantics as the API path, and records zip-member
provenance per row:

```powershell
atx-db load-sec-submissions-bulk --zip-path data\cache\submissions.zip
atx-db load-sec-submissions-bulk --zip-path data\cache\submissions.zip --cik 320193 --all-forms
```

Build the revision-complete standardized fundamentals surface after statement and TTM
refreshes. Omit `--symbol` for the full warehouse; repeated filters are useful for proof
slices and incremental recovery:

```powershell
atx-db refresh-standardized-fundamentals --memory-limit 8GB --threads 4
atx-db refresh-standardized-fundamentals --symbol AAPL --symbol MSFT
atx-db refresh-fundamental-reconciliation
atx-db refresh-filing-context-backfill-queue
atx-db run-filing-context-backfill --max-filings 10
atx-db refresh-provider-coverage
```

Install the optional certified XBRL processor and run a bounded, offline validation
sidecar when structured processor findings are required:

```powershell
uv sync --extra xbrl
atx-db capture-xbrl-filing-packages --accession 0000004904-23-000081 --max-filings 1
atx-db capture-xbrl-taxonomy-packages --accession 0000004904-23-000081 --max-filings 1
atx-db capture-xbrl-taxonomy-packages --accession 0000004904-23-000081 --fail-fast
atx-db run-arelle-validation --accession 0000004904-23-000081 --max-filings 1
atx-db run-arelle-validation --accession 0000004904-23-000081 --taxonomy-package C:\data\xbrl-taxonomy-packages\2022
```

Offline is the safe default. `--online` permits Arelle to resolve remote DTS resources
directly and is reserved for an operator-controlled environment with source-specific
fair-access controls. Core runs are labeled `xbrl21_calc11_round_to_nearest`; passing an
explicit official SEC EDGAR plugin checkout with `--efm-plugin-path` enables and labels
the EFM profile. The Arelle PyPI `EFM` extra supplies plugin dependencies but does not
include the SEC-maintained EDGAR plugin itself.
`capture-xbrl-filing-packages` uses the same paced SEC client and immutable source cache
to capture each filing's entrypoints, extension schema, and calculation, definition,
label, and presentation linkbases. Multi-document IXDS filings are assembled into a
deterministic ZIP keyed by the ordered member/digest manifest.
`capture-xbrl-taxonomy-packages` parses cached extension schemas and calculation,
definition, label, and presentation linkbases. It maps absolute taxonomy references to
official FASB, SEC, and XBRL US release ZIPs, stores immutable revisions plus filing
dependency edges, and materializes checksum-verified OASIS taxonomy packages for offline
processing. Publisher archives without OASIS metadata receive a deterministic catalog
wrapper while the original ZIP, digest, byte count, and source URL remain unchanged in
lineage. When an official SEC legacy family publishes only a directory of XSD/XML members,
the capture builds a deterministic package from those same-origin documents and records
the directory as the source. Arelle loads the linked processor packages automatically;
`--taxonomy-package`
remains an additive override that accepts a ZIP or directory of ZIPs and records every
expanded package's path, SHA-256, and byte count with the processor run.
Package capture is failure-isolated by default: one unavailable publisher artifact does
not discard successful packages or filing edges. Every requested package writes a durable
latest-revision attempt with stage, cache/network counts, hashes, and exact failure;
`--fail-fast` restores batch-abort behavior for controlled diagnostics.

The command reports the build ID, rule-set digest, candidate/output/exception counts, and
basis distribution. Every build is also persisted in
`fundamental_standardization_builds`; the public `standardized` schema is version 2.0.0 and
retains original/restated revision chains rather than only the current value. Quarterly
output includes PIT-correct Q2/Q3/Q4 derivations from cumulative YTD filings and switches to
a directly reported discrete quarter when one becomes available.

Two additional fundamentals contracts sit on top of that revision stream. The
`industry-standardized` schema applies revision-complete `ALL`, bank, insurer, REIT,
utility, and broker-dealer statement routing. The `restatements` schema (migration
0298) publishes one immutable event per standardized revision that changed a
previously published value — restating and superseded accessions, first-reported
baseline, per-event and cumulative deltas, and the point-in-time availability of
both vintages — keyed by `(revision_group_id, revision_sequence)` so PIT vintage
selection never collapses distinct events. The `reconciliation` schema evaluates 19
governed accounting identities at every input or classification event, exposes exact
weighted-input lineage and tolerances, and distinguishes hard `mismatch` results from
diagnostic scope differences and PIT `not_applicable` transitions.
Its indexed serving table is atomically published with a row count, content checksum,
input watermark, and freshness/parity gates. Filing-context verification reads both
modern inline XBRL and historical EX-101.INS XML; legacy instance documents are
discovered from the SEC accession directory's `index.json` and retain the filing primary
document separately from the exact instance source.
`refresh-filing-context-backfill-queue` converts the remaining single-filing context
gaps into a checksum-governed SEC work queue ranked by expected reconciliation
verification gain; blocked rows preserve missing-submission metadata debt explicitly.
`run-filing-context-backfill` claims a bounded priority batch one accession at a time,
records durable success/failure/retry state in `filing_context_backfill_attempts`,
recovers expired worker leases, and isolates a bad filing without rolling back earlier
successes. Automatic retries are limited to transient transport failures and retryable
HTTP statuses; deterministic parser/schema failures require intervention, with an
explicit `--retry-nonretryable` override for replay after a fix. Submission ingestion
accepts explicit CIKs as well as tickers so historical
entities remain loadable after ticker reuse or a successor-CIK transition. The shared
SEC client enforces process-wide 110 ms request spacing and bounded `Retry-After`-aware
retries for 429 and transient 5xx responses. Inline filings whose designated primary
document is only a cover page use a bounded accession-index fallback to the ranked
resource-bearing companion document; facts from the cover and companion are merged under
one logical IXDS instance while retaining each exact source-document URL. Downloaded filing
artifacts are cached immutably by SHA-256, recorded in `raw_source_files`, and reused only
after checksum verification; executor attempts distinguish network requests from verified
cache hits so historical builds are reproducible without repeated SEC traffic.
Migration 0287 adds versioned `xbrl_processor_runs` and structured
`xbrl_processor_findings`. These tables preserve the processor/version/profile, exact
entrypoint and command, connectivity and cache mode, exit state, finding codes/messages,
references, PIT availability, and latest-revision lineage. This sidecar is intentionally
outside the default DAG because it is optional and substantially heavier than SQL-native
quality checks.
Migrations 0288-0290 add processor package lineage, indexed semantic outcomes, and finding-level
revision visibility. A processor exit
is distinct from DTS resolution and filing validity: runs are classified as
`incomplete_dts`, `validation_errors`, `validation_issues`, or `valid`, so a successful
subprocess cannot be misrepresented as a valid filing when required taxonomy resources
were absent. Finding revisions follow their parent run, preserving PIT-safe latest and
historical validation views.
Migrations 0291-0295 add a governed `xbrl_standard_taxonomy_package_revisions` catalog,
processor-package lineage, and revision-aware `xbrl_filing_taxonomy_packages` reference
edges plus per-package capture attempts without overloading the existing
taxonomy-relationship extraction tables. A real
20-filing replay discovered 517 references across schemas and linkbases, mapped 380 to ten
governed packages, and created 306 new edges. Immediate replay used ten verified cache
hits and zero network requests. The captured official XBRL US 2009 distribution reduced
a legacy filing from 141 incomplete-DTS findings to two calculation inconsistencies with
no missing imports; current 2024/2025 filings likewise resolve fully offline.
Migrations 0296-0297 pin accepted batch queries to a versioned public schema digest,
persist an encoding-independent logical result digest, and add immutable manifest URI and
SHA-256 lineage to each completed job. Manifests embed the exact request and field contract
and are retrievable independently of the expiring data artifact.
`refresh-provider-coverage` then appends the measured inclusive-start/exclusive-end
range, record/security/item breadth, freshness, target failures, and
`available`/`degraded`/`pending`/`missing` condition for every public schema.

The current fundamentals architecture, source evidence, reconciliation policy, and
measured proof-slice results are recorded in
[`docs/FUNDAMENTALS_PROVIDER_DESIGN.md`](docs/FUNDAMENTALS_PROVIDER_DESIGN.md).

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
