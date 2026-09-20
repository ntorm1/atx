# Native Parquet price input

The reviewed AR6 bulk projection and offline audit now accept local Parquet
directly through DuckDB `read_parquet(?)`. No full price frame enters Python,
and there is no intermediate TSV conversion. This change has not scanned or
published the updated production source; the controller owns those operations
after independent review.

## Input and publication contract

- `BulkTickerHistoryOptions(source_path=...)` and both scripts' `--source-path`
  accept `.parquet`/`.pq` or `.tsv`/`.txt`/`.tab`. Existing positional/keyword
  `tsv_path` callers and `--tsv-path` remain compatible. Supplying both dataclass
  paths, or neither, fails explicitly. ZIP extraction remains a separate step.
- Schema binding checks required columns and compatible scalar types before
  staging. Parquet dates and native numeric types are accepted; missing fields,
  list/struct columns, numeric ticker columns, and integer date encodings fail.
  Every file path is a bound DuckDB parameter. Schema inspection fetches only
  the small column description; the projected original rows stay in DuckDB.
- Numeric identifiers are cast to text only for the existing integral-token
  validation. Fractional values cannot round into positive vendor keys.
  Existing invalid-value diagnostics, same-row `close * cumulReturnFactor`,
  unknown split factors, original-key duplicate quarantine, display renames,
  stable vendor identity and breadth thresholds remain unchanged.
- Source receipts, quality details and the publication result include the
  detected format. Parquet provenance explicitly says that values retain their
  native typed representation and that original text precision and source
  vintage are unknown. Native float values are not described as exact original
  TSV values. Publication still hashes the complete original input file via
  streaming source-file recording; the audit hashes complete original bytes
  before scanning and checks size/mtime afterward.
- Original-source diagnostics retain the scanned `first_date`/`last_date` and
  add `latest_date_source_rows` and
  `latest_date_distinct_positive_vendor_ids`. The latter is original-source
  breadth before quarantine, OHLCV filtering or identity resolution; it does
  not certify US-common-equity membership or canonical publication breadth.
- Bulk API and CLI defaults are now 1 GB and one thread, matching the program's
  memory ruling. The external process-tree guard remains required for production;
  DuckDB's configured memory limit alone is not a whole-process limit.

## Validation

- Ran `tests/test_ticker_history_parquet.py`, `tests/test_ticker_history_bulk.py`
  and `tests/test_ticker_history_price_source.py` once with `-n 0 -q` under the
  reviewed 3 GiB process-tree guard. Of 32 cases, 31 passed initially. One new
  fixture compared identity diagnostics before and after its first publication
  had created ticker links. Seeding identical identity metadata for both inputs
  corrected that fixture; only the failed case was rerun and it passed.
- The new typed-Parquet fixtures exercise DATE/BIGINT/FLOAT/DOUBLE columns,
  an apostrophe in the bound path, matching TSV bars and diagnostics, complete
  source hash lineage, current adjusted prices through a two-for-one event,
  raw volume/shares, rename-stable keys, prefilter duplicate quarantine, invalid
  cumulative factors, strict fractional-ID handling, schema rejection, and
  both neutral/legacy CLI flags. No original quality threshold was changed.
- Memory receipts: `parquet-price-focused-memory.json` and
  `parquet-price-focused-rerun-memory.json`. Native job peak accounting was
  approximately 0.93 GiB and 0.70 GiB respectively; the guard observed ample
  physical/commit headroom. New DuckDB fixtures used 128 MB/one thread; existing
  covering bulk fixtures retained their explicit 1 GB/one-thread settings.
- Ruff passed on all five touched Python files; mypy passed on the four
  production/script files. `git diff --check` passed. No full suite, actual
  updated-file scan, production migration or warehouse publication was run.

## Controller handoff

Use the hash-verified staged source without copying or changing the Downloads
original: `C:/atx/atx-db/data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet`.
The existing staging receipt declares SHA256
`0ed96b2696f194deee0d297b51425d3daf96bbaf3b28030b614a34a6943abbae`;
this task did not independently rescan or rehash that file. Prior footer metadata
reported 32,323,644 rows and a final date of 2026-09-18; full diagnostics remain
pending and must establish actual latest-date breadth.

From `C:/atx/atx-db`, after review and obtaining the single heavy-workload slot:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 3 --receipt ../.superpowers/sdd/tier1-parity/updated-price-audit-memory.json -- .venv/Scripts/python.exe scripts/audit_ticker_history_source.py --source-path data/staging/broad-bars/2026-09-20-updated/TickerHistory3.parquet --output ../.superpowers/sdd/tier1-parity/updated-price-source-audit.json --memory-limit 1GB
```

Choose fresh receipt/output names if an attempt already exists. Governed live
migrations, source-audit disposition and the controller's production sequencing
must precede publication with `scripts/publish_broad_daily_bars.py --source-path`
using this same path, explicit warehouse/run ID, 1 GB and one thread. Existing
return-dependent outputs still require their separately assigned repairs and
rebuilds. This input addition makes no historical identity, availability-vintage,
economic-adjustment or full Tier-1 parity certification.
