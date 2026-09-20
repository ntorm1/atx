# Activation-run5 fundamentals capacity audit

Static audit, 2026-09-20. Scope: the imminent activation path from
`statement_points` through `derived_metrics`, at DuckDB 1 GB / one thread,
16 sequential reconciliation partitions, inside the outer 3 GiB Windows job.
Read the current top override in `continuation-queue.md` and
`production-resume-after-cf5.md` first. Archive4 retained the sole runtime slot.
No Python, imports, tests, lint/type checks, database connections, data probes,
network calls, source edits, or git mutations were performed. Graph tools were
not exposed; discovery used static `rg` and source reads.

**Result: 0 Critical / 2 Important capacity defects.** Both findings establish
an unbounded publication/index lifetime in source. Neither is a measured
activation-run5 failure or an estimate of its exact peak. Existing focused tests
do not establish full-universe capacity. CF5, P1 historical revisions, and AF1
annual fallback semantics are outside this audit and are not reopened.

## Exact activation call path and bounds already present

References below are relative to `atx-db/`. `activation.py` means
`src/atx_db/activation.py`; other module names also live under `src/atx_db/`.

`activation.py:46-62` orders the audited stages as:

`statement_points -> periods -> ttm -> calendarization -> standardized -> industry_templates -> reconciliation -> derived_metrics`

There is no activation stage named `statement_reconcile`. Its actual stage is
`reconciliation`, implemented by `fundamental_reconciliation.py`.
`run_activation` sets analytical resources once at `activation.py:1087-1092`;
`cli.py:77-92` also disables insertion-order preservation.
`connection.py:118-134` replays those resources after reopen. `--force` bypasses
completed-stage skipping (`activation.py:1093`). The 16-way setting is passed
only to reconciliation (`activation.py:753-765`), not to preceding refreshes.

| Stage | Actual calls / source references | Existing memory bound and remaining publication shape |
| --- | --- | --- |
| `statement_points` | `activation.py:610-618` -> `fundamentals.py:734-738` -> `xbrl_catalog.refresh_concept_catalog`; then `fundamentals.refresh_fundamental_fact_revisions:741`; then `fundamental_statements.refresh_fundamental_statement_points:982`, including industry routing and `_insert_derived_reit_statement_points:580`. | Catalog SQL aggregates raw facts first; Python gets one row per source/taxonomy/concept (`xbrl_catalog.py:48-90`), not all fact rows. Revision and mapped-point calculations stay in SQL, but each publication is one full indexed-table replacement transaction (I1). Map/rule seed buffers are catalog-sized. |
| `periods` | `activation.py:629-634` -> `fundamental_statements.refresh_fundamental_periods:1256`. | SQL grouping/window calculation; only final count crosses into Python (`:1482`). Lineage lists are grouped by issuer/period/accession (`:1357-1382`). Full-table replacement transaction begins at `:1259`. |
| `ttm` | `activation.py:637-642` -> `fundamental_statements.refresh_fundamental_ttm_points:1485`. | SQL reconstruction and count return (`:1873`); activation supplies no metric restriction. Full-table replacement transaction at `:1500-1510`. |
| `calendarization` | `activation.py:645-655` -> `calendarization.run_calendarization_refresh:920-930` -> calendar map, calendar TTM, aggregate coverage. | Map conversion transfers at most 2,048 period rows to pandas at once and deletes each Python input/output batch (`calendarization.py:20,337-345`); issuer FYR inference is complete before batching. SQL owns full input/output staging. One transaction still encompasses all map batches and final indexed publication (`:291-352`); calendar TTM also has one complete-source transaction (`:363-367`). Coverage and returned summary are aggregate-sized. |
| `standardized` | `activation.py:658-670` -> `standardization.refresh_fundamental_standardized:815-829` -> `_standardization_set_based.refresh_standardized_set_based:937`. | Activation explicitly sets `materialize_result_limit=0`. Production uses SQL candidates/output/exceptions (`_standardization_set_based.py:980-985`) and aggregate dictionaries (`:986-996`). Result DataFrames stay empty for nonempty results (`:1060-1073`); the old whole-input pandas helper `standardization.load_standardization_inputs:640-787` is not called here. All standardized/exception rows are published together in one transaction (`:998-1058`, I1). |
| `industry_templates` | `activation.py:684-690` -> `industry_templates.run_industry_template_refresh:486-506` -> routing `:186`, coverage `:396`. | SQL routing scales with security classification history; coverage is grouped by the fixed template/item catalogs (`:414-455`). Python returns counts only. Routing has a whole-source transaction (`:194-198`), but this audit does not infer a fact-scale failure for this smaller surface. |
| `reconciliation` | `activation.py:727-772` -> `scripts/refresh_reconciliation_sharded.py:79-140` -> `fundamental_reconciliation.refresh_fundamental_reconciliation_serving:425`. | Parent closes the database; the script synchronously runs one fresh child per partition (`script:101-128`). Each child uses the same memory/thread settings (`:132-140`). The script materializes only distinct symbol identifiers in Python (`:57-76`). Scoped standardized input is created before evaluating cloned views (`fundamental_reconciliation.py:76-136,463-490`), preventing a late filter from evaluating the full standardized universe. A partition's serving rows remain in SQL and publish atomically (`:517-605`). Final content hash is a SQL aggregate (`:150-161`), not a Python row list. Parent captures a small plan/partition event stream (`activation.py:693-720`), not issuer result rows. No additional proven memory defect assigned; per-partition peaks remain unmeasured. |
| `derived_metrics` | `activation.py:793-799` -> seed catalog -> `derived_metrics.refresh_derived_metrics:156` -> `_derived_pit.prepare_security:53`, `_derived_annual.prepare_security:80`, per-metric preparation/frames/compression, per-security publication. | Default identifier batch is one; cursor fetches at most 500 IDs (`derived_metrics.py:107-132`). Full issuer history remains in SQL, bounded by 250,000 input rows. Candidate bound is 250,000, frame bound 8,192, event chunk 1,024, publication scope 100,000 (`:48-57,192-248`). Each security commits separately, ordered by required PK, and temp state is cleaned in `finally` (`:249-263`). All issuer outputs are not buffered in Python. Connection/index lifetime across all securities is still unbounded (I2). |

## I1 — Important: pre-reconciliation rebuilds publish the universe in one indexed transaction

The first occurrence is unavoidable in run5: activation passes no concept
filter to `refresh_fundamental_fact_revisions`. At `fundamentals.py:761-775` it
opens one transaction, deletes the entire revisions table, and inserts the
revision projection of every eligible raw fact (`:858-873,944-946`). Its
required PK is a SHA-256 string (`schema.py:1144-1181`) and three secondary
indexes remain declared (`schema.py:1917-1919`). This is proportional to the
eligible raw fact universe, not to 16 shards or one issuer.

The same publication shape recurs for statement points
(`fundamental_statements.py:1002-1016,1245-1249`), periods (`:1259-1263`),
TTM (`:1500-1510`), calendar map/TTM (references in the table), and standardized
plus exceptions (`_standardization_set_based.py:998-1039`). Required keys and
secondary indexes are retained in `schema.py:1214-1355,1921-1930`,
`migrations/bodies_0001_0137.py:8131-8158,8243-8246,8257,8952,9074,9282-9286`,
and `migrations/bodies_0271.py:157-158`.
Migration 0314 drops only five price/bar indexes
(`migrations/bodies_0314.py:18-25`); it does not fix these tables.

Exact target/index inventory for a bounded implementation (source declarations,
not a new live `duckdb_indexes()` measurement):

| Target / required PK column | Refresh function | Secondary index names |
| --- | --- | --- |
| `fundamental_fact_revisions` / `fact_revision_id` | `fundamentals.refresh_fundamental_fact_revisions` | `idx_fundamental_fact_revisions_group`, `idx_fundamental_fact_revisions_security`, `idx_fundamental_fact_revisions_latest` |
| `fundamental_statement_points` / `statement_point_id` | `fundamental_statements.refresh_fundamental_statement_points` | `idx_fundamental_statement_points_metric_asof`, `idx_fundamental_statement_points_security`, `idx_fundamental_statement_points_revision`, `idx_fundamental_statement_points_source_accession` |
| `fundamental_periods` / `fundamental_period_id` | `fundamental_statements.refresh_fundamental_periods` | `idx_fundamental_periods_security`, `idx_fundamental_periods_group`, `idx_fundamental_periods_type` |
| `fundamental_ttm_points` / `ttm_point_id` | `fundamental_statements.refresh_fundamental_ttm_points` | `idx_fundamental_ttm_points_metric_asof`, `idx_fundamental_ttm_points_security`, `idx_fundamental_ttm_points_revision` |
| `fundamental_calendar_map` / `calendar_map_id` | `calendarization.refresh_fundamental_calendar_map` | `idx_fundamental_calendar_map_period`, `idx_fundamental_calendar_map_overlap`, `idx_fundamental_calendar_map_53_week` |
| `fundamental_calendar_ttm` / `calendar_ttm_id` | `calendarization.refresh_fundamental_calendar_ttm` | `idx_fundamental_calendar_ttm_window`, `idx_fundamental_calendar_ttm_latest` |
| `fundamental_standardized` / `standardized_id` | `_standardization_set_based.refresh_standardized_set_based` | `idx_fundamental_standardized_item`, `idx_fundamental_standardized_security`, `idx_fundamental_standardized_asof`, `idx_fundamental_standardized_latest`, `idx_fundamental_standardized_revision`, `idx_fundamental_standardized_validity` |
| `fundamental_standardization_exception` / `exception_id` | Same function, coupled with standardized publication | `idx_fundamental_std_exception_security`, `idx_fundamental_std_exception_reason`, `idx_fundamental_std_exception_asof` |

Exception index definitions are at
`migrations/bodies_0001_0137.py:8285-8287`; remaining definitions are at the
schema/migration references immediately above. Aggregate coverage and small
build-ledger tables are not proposed for shadow conversion.

SQL spill and bounded pandas batches do not bound the changed-row/index work
of these complete-table transactions. There is no intermediate commit or
connection recycling before each table's complete replacement commits.
The guard bounds host exposure by stopping the process; it cannot make the
stage complete. This is a concrete source-level capacity defect, not a request
to optimize ordinary scans. Whether the first revision publication or a later
surface crosses 1 GB on the current warehouse is **not measured here**.

Existing production evidence supports treating this as Important: the controller
records two price publication COMMIT failures at 1 GB and 2 GB on a 31,959,271-row
replacement (`continuation-queue.md`, historical 17:18 override). Its eventual
repair was physical bounded publication, not higher memory. Those receipts are
evidence for the mechanism on this warehouse, not proof that a fundamentals
stage has already failed.

**Smallest production-preserving implementation brief:** retain the existing
calculation SQL and full input/revision semantics; move the full-refresh output
to a disk-backed stage, then build a physical shadow with the exact current
columns/defaults/nullability/required logical keys in independently committed,
ordered key ranges. Recycle the configured connection at bounded intervals;
temporary relations/registered frames must not survive a recycle. Preserve
other sources/scopes when constructing replacement contents. Validate counts,
keys, and physical contracts, then perform a short atomic swap. Keep the old
canonical output usable on any staging/shadow failure. Standardized and
exception outputs plus their build-status update must publish atomically
together. Preserve REIT additions in the statement-point build. Do not replace
global atomic publication with partially visible issuer commits.

The existing design to reuse is `equity_price_metrics.py:504-576` and
`_bulk_publication.py:22-80`, not a new generic framework. Current secondary
indexes block that rename strategy; any narrowly scoped removal must be an
explicit governed migration, maintain required PK/UNIQUE constraints, update
fresh bootstrap/index declarations, and pass schema contracts. Do not reserve
a migration number in this audit or silently drop indexes at runtime.

Touched paths: `fundamentals.py`, `fundamental_statements.py`,
`calendarization.py`, `_standardization_set_based.py`; reuse/extend
`_bulk_publication.py` only as necessary for coupled swaps; `schema.py` plus one
root-assigned migration/registry entry and exact schema pins if index policy
changes; focused publication failure/contract/parity tests. Start with the first
raw-scale revision/statement publication, then apply the same bounded mechanism
to the enumerated fact-derived outputs. No catalog, metric-definition, CF5
ingestion, P1/AF1 arithmetic, RAM, or universe changes are needed.

## I2 — Important: derived per-security bounds do not bound retained PK lifetime

`derived_metrics.py:120-132` opens a result cursor for the entire identifier
stream. `:186-263` uses the same underlying store for all securities. It commits
each security and drops its temporary relations, but never closes/reopens the
store; the PK index persists across those commits. Required
`derived_metric_values.derived_value_id` remains a primary key
(`migrations/bodies_0302.py:117-131`), and only its optional secondary index was
removed by 0315 (`migrations/bodies_0315.py:12-15`). `ORDER BY derived_value_id`
sorts one issuer, not the cumulative full-universe hashed key stream.

Thus input/frame/scope limits bound a security's SQL and transaction work but
leave retained index state proportional to all previously published securities.
The long-lived cursor also prevents simply inserting a reopen into the loop.
This is the same specific connection-lifetime defect repaired for companyfacts:
the measured archive3 failure occurred after 3,750 loaded issuers / 20,205,629
attempt fact rows at the 1 GB COMMIT budget; see
`companyfacts-archive3-repair-brief.md:5-17,30-34` and the explicit repair comment
at `fundamentals.py:64-66`. No derived row count or failure threshold is inferred
from those fact counts, and no run5 derived failure was measured.

**Smallest production-preserving implementation brief:** replace the open
whole-run ID cursor with complete, deterministic keyset pages (or a persistent
identifier snapshot) whose result cursor is exhausted before processing the
page. Preserve the union of eligible standardized securities and existing
source-owned derived securities, including scopes that must be cleared. After a
small fixed number of completed security commits, clean all PIT temp relations,
close/reopen the configured persistent store, and continue from the explicit
identifier boundary. Do not recycle in a transaction or on an in-memory store;
do not retain a cursor across reopen. Preserve the existing per-security atomic
replacement, all metric/PIT states, fail-before-delete limits, and final totals.
The existing `connection.py:118-134` resource replay and the CF5 lifecycle
pattern (`_companyfacts_resume.py:278-289`) are references, not dependencies on
companyfacts semantics.

Touched paths: `derived_metrics.py`; a narrowly scoped lifecycle helper only if
needed; focused derived tests proving all IDs are covered once across recycle
boundaries, stale-only scopes are included, earlier committed securities remain
complete after failure, limits/settings persist, and required PKs remain.
No migration or P1/AF1 calculation changes are needed for this repair.

I1 and I2 are independently integrable tasks. They need no shared source edit
except root-owned integration documentation; the I1 migration/index decision
must remain with its assigned owner. I2 retains the existing per-security
atomic contract, while I1 retains each existing full-scope atomic contract.

## Limits, omissions, and pending runtime evidence

The derived input/candidate/frame/publication limits **raise**, including before
canonical deletion (`derived_metrics.py:200-201,230-248`,
`_derived_pit.py:46-61,170`). They do not truncate or skip oversized issuers.
`materialize_result_limit=0` controls only returned Python frames, not persisted
standardized rows. Period/calendar `LIMIT 1` selects a per-period RDQ match or
one aggregate summary, not the warehouse universe. No all-issuer Python output
buffer or universe-wide raw-fact pandas conversion is on this activation path.

One completeness precondition needs a root measurement: reconciliation plans
only distinct nonblank symbols in `fundamental_standardized`
(`scripts/refresh_reconciliation_sharded.py:64-69`), then resolves those symbols
to security scopes (`fundamental_reconciliation.py:465-476`). Source does not
prove every standardized security has such a symbol, nor that a serving-only
stale security remains in the planned scope. This audit establishes the filter,
not a measured omitted issuer; it does not classify an additional capacity
defect or authorize reduced-universe success.

After archive4 releases the runtime slot, root alone should measure actual
post-load input/output cardinalities and guarded memory across build and COMMIT
phases, confirm complete reconciliation security coverage, and observe all
derived limits/issuer outcomes. The known catalog helper previously aggregated
31.59M raw facts to 242 concepts at 1 GB / one thread (recorded in the controller);
that measured success does not validate the following indexed rebuilds.
Full-universe completion at the fixed 1 GB / one-thread / 3 GiB limits remains
pending. No parallel database work, increased memory, metric reduction, or
discarded historical/PIT output is proposed.
