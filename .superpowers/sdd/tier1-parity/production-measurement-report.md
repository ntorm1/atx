# Production measurement script handoff

`atx-db/scripts/measure_tier1_readiness.py` inventories the existing warehouse
without importing `atx_db`, running activation or quality checks, applying
migrations, or changing schema conditions. It requires explicit database,
observation date, JSON output and Markdown output paths. Existing evidence files
are never overwritten.

Implementation and static inspection are complete. **No database connection,
live scan, package import, runtime test, or production measurement was executed
by this implementation agent.** Independent static review found no Critical or
Important defects. Its nonblocking redaction wording caveat is addressed.
The controller's first live prepass failed during connection configuration,
before any measurement query. After the connection-order correction, the
controller's guarded rerun completed successfully and recorded the pre-refresh
warehouse snapshot summarized below. **Tier-1 parity and release readiness are
not certified.**

## Evidence included

- Latest retained activation attempt per stage and counts of retained attempt
  outcomes. The `(stage, run_id)` key can overwrite a reused attempt; the report
  does not call these counts a complete historical audit. A later upstream
  attempt is a conservative review signal, not a proven dependency violation.
- Current stored row, distinct security/item/metric/dimension counts, date ranges,
  latest-revision counts, and numeric NULL/nonfinite counts for raw companyfacts,
  standardized fundamentals, derived values, prices, market/risk panels, listings,
  the historical universe, delistings, terminals and survivorship forward returns.
  The report counts inventory after its cutoff without calling it a PIT violation;
  all stored revisions are included. Missing facts are not counted as NULL rows.
- Output-to-activation lineage using exact option run IDs, including known suffixes.
  Dataset UUIDs are linked only through succeeded `dataset_runs` with matching
  `params_json.run_id` inside the activation attempt's time interval. Unlinked rows
  remain unverified, rather than automatically stale. Matching IDs do not prove
  input vintage, economic correctness, or successful quality gates.
- Annual coverage from `fundamental_item_coverage` joined to
  `item_coverage_cohort_years`. Every completed FY2015+ year is shown, including
  missing or undersized cohorts. The numeric gate mirrors
  `item_coverage.coverage_gate_count_sql`: 110 items, each at 90% coverage in every
  required year, with a complete 3000-name cohort and consistent numerator and
  denominator. The result is separate from freshness and certification.
- Latest stored provider snapshots joined to active SLO contracts, with thresholds,
  recorded condition, failures, observation date, and run lineage. Empty records
  remain empty evidence even when their failure list is empty.
- Latest recorded quality result per dataset/table/check, full outer joined to
  the check registry so enabled checks without results remain `not_run`. Recorded
  severity/status, modeled and insertion timestamps, observed values, thresholds
  and bounded details remain visible. `data_quality_checks` has no `run_id`;
  same-day passes therefore remain unverified against a particular rebuild. Later
  upstream attempts require review. Missing, stale, disabled, skipped and empty
  evidence never becomes a pass.
- Aggregate historical listing/venue evidence, universe intervals and terminal
  gaps. Latest eligible terminal revisions are selected before validating values.
  No event rows is not proof of complete delisting coverage. This report does not
  recompute the survivorship stitch quality gate.
- Optional `--include-cf1` run/evaluation inventories, without significance,
  candidacy or production eligibility conclusions.

## Bounded query and memory contract

One read-only DuckDB connection runs all queries sequentially in one transaction.
The default is `--memory-limit 1GB`, one thread, insertion-order preservation off,
external access disabled, and a unique temporary spill directory capped at 2 GB.
The temporary directory is removed when the connection closes. No pandas or
security-level panel materialization occurs.

Connection configuration establishes the temporary directory and resource caps
first. The first session statement then disables external access, followed by
an explicit `SET TimeZone = 'UTC'`, before the transaction or any report query.
The availability cutoff is explicitly 22:00 UTC; session UTC also governs
timestamp conversions. Both the UTC cutoff and `time_zone` are recorded in the
JSON report. Database access remains read-only throughout setup and measurement.

Surface queries each return one aggregate row. `COUNT DISTINCT` operates in
DuckDB on identifier columns; no security lists cross into Python. Annual gate
queries aggregate item/year rows. Terminal gaps use a latest-revision window over
terminal records and an event join, returning counts by delist code. Registry,
SLO, stage and optional CF1 queries return bounded control records. Every
variable-size query has an outer SQL limit of 1001, returns at most 1000 rows,
and records truncation. Error/quality/SLO diagnostic text is limited in SQL to
2048 characters per row, with original-length truncation flags. Email addresses
in strings and sensitive keys in parsed structured objects are redacted.
Arbitrary secrets in plaintext or truncated JSON are not sanitized. Full
activation parameters and source contact fields are never selected.

These limits bound Python result materialization, not the cost of SQL scans or
hash/window operators. Use the controller's memory guard and one heavy workload
at a time. Missing tables/columns are explicitly unmeasured. SQL errors, memory
failures, connection conflicts and report publication failures propagate as
nonzero exits. There is no broad exception handler that turns query bugs into
missing data. The process exit code denotes measurement execution, not parity.

## Verification and operator command

Before the connection-order correction, static verification was performed from
`C:/atx` with the project's locked Python:

```powershell
.\atx-db\.venv\Scripts\python.exe -c "import ast,pathlib; p=pathlib.Path('atx-db/scripts/measure_tier1_readiness.py'); ast.parse(p.read_text(encoding='utf-8')); print('AST parse passed; script not imported or executed')"
git diff --check -- atx-db/scripts/measure_tier1_readiness.py .superpowers/sdd/tier1-parity/production-measurement-report.md
```

Both original checks passed. AST parsing does not import the script or package
and does not validate DuckDB query binding. The controller owns runtime checks
and live execution of the corrected script. The implementation agent inspected
source and the controller's saved artifacts only, and ran static Ruff on this
script. Ruff passed after removing an extra import-block blank line and combining
equivalent nested context managers; those syntax-only edits followed the live
rerun and preserve connection/spill setup and teardown order.

The controller's `production-measurement-prepass2` attempt used DuckDB 1.5.5 and
failed with `PermissionException: Modifying the temp_directory has been disabled
by configuration`. The previous connection configuration supplied both
`temp_directory` and `enable_external_access=false`, allowing the access lock to
prevent initialization of the spill directory. The failure occurred inside
`duckdb.connect`, before any report SQL. The guard receipt records exit code 1
and a peak job memory of approximately 0.033 GiB under its 3 GiB limit. This was
a configuration failure, not a completed measurement or an empty warehouse.
The failure artifacts are retained as
`production-measurement-prepass2.err` and
`production-measurement-prepass2-memory.json`.

The correction preserves the read-only connection and applies the access lock
after the spill settings are established. A narrow source check of the selected
surface schema declarations found no obvious mismatch. The controller's
`production-measurement-prepass2-fix1` rerun then completed all report queries
under DuckDB 1.5.5, including optional CF1 inventory. It exited 0 in
18.938282 seconds, with 1.5300712585 GiB peak job memory under the 3 GiB guard.
The saved stderr is empty, `read_only` is true, the session is UTC, and
`warehouse_file_metadata_changed_during_report` is false (unchanged size and
modification time, not a byte-for-byte content comparison).

The snapshot was generated at **2026-09-20 19:20:47 UTC**, with observation date
2026-09-20 and availability cutoff 22:00 UTC. It predates the next full raw-facts
refresh and does not establish that refresh's outcome.

| Stored surface | Rows | Distinct security IDs | Recorded evidence |
| --- | ---: | ---: | --- |
| `sec_company_facts` | 31,590,760 | 6,533 | 242 concepts; linked to older `activation-run4-companyfacts`; newer upstream attempt requires review |
| `equity_daily_bars` | 31,959,271 | 34,251 | 25,753 vendor IDs; dates 2012-03-26 through 2026-09-18; linked to latest price publication |
| `exchange_listings` | 45,820 | 36,762 | Zero rows with nonempty exchange code or MIC; no venue rows known by interval start |
| Standardized fundamentals, derived metrics, market daily metrics, price/risk metrics | 0 each | 0 each | Empty |
| Listing status, US-listed universe, delisting events, terminal returns, survivorship forward returns | 0 each | 0 each | Empty |

These are stored-row and identifier counts, not verified issuer counts. All price
`split_factor` values are NULL; the report does not infer adjustment economics
from the non-NULL close/adjusted-close values. Annual item coverage is empty,
every completed FY2015-2025 cohort is `not_run`, and zero items meet the annual
gate. All 12 provider SLO rows are `not_run`. Quality has 375 `not_run` rows and
four recorded outcomes requiring review because of newer upstream attempts
(three recorded passes and one warning). None is accepted as a current release
pass. Both CF1 run and evaluation inventories are empty.

There are no reported schema gaps or missing selected surface columns. All nine
query result sets have `truncated=false`. Two quality detail excerpts are capped
at 2048 characters and explicitly flagged `truncated=true`:
`tbltickerhistory_daily/institutional_latest_date_breadth` and
`tbltickerhistory_daily/source_preprojection_diagnostics`. Their recorded
outcomes remain visible, but their full diagnostic detail is not in this report.

The successful evidence is retained in
[`production-measurement-prepass2-fix1.json`](production-measurement-prepass2-fix1.json),
[`production-measurement-prepass2-fix1.md`](production-measurement-prepass2-fix1.md),
[`production-measurement-prepass2-fix1-memory.json`](production-measurement-prepass2-fix1-memory.json),
and its `.log`/`.err` receipts. Closed failed attempts, old stored results, empty
surfaces, and stale quality/provider evidence cannot certify current readiness.

The controller's successful production command from `C:/atx/atx-db` was:

```powershell
.venv/Scripts/python.exe ../.superpowers/sdd/tier1-parity/run_memory_guarded.py --job-gb 3 --receipt ../.superpowers/sdd/tier1-parity/production-measurement-prepass2-fix1-memory.json -- .venv/Scripts/python.exe scripts/measure_tier1_readiness.py --db-path data/warehouse.duckdb --as-of-date 2026-09-20 --output-json ../.superpowers/sdd/tier1-parity/production-measurement-prepass2-fix1.json --output-markdown ../.superpowers/sdd/tier1-parity/production-measurement-prepass2-fix1.md --memory-limit 1GB --include-cf1
```

For any later rerun, first let the active writer exit and the controller clear
the serialized memory slot, and choose fresh receipt/output names; the names
above already exist. Retain the guard receipt and stderr alongside both reports.
Review all schema gaps, truncation
flags, cohort years, provider conditions, quality severities and lineage notes
before interpreting the warehouse state. Vendor IDs and archive CIKs are never
promoted into verified issuer or historical common-equity populations.

The script, this handoff, independent static review, and failure/success receipts
are scoped deliverables. Their commit retains the requested literal trailer:

```text
Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
```
