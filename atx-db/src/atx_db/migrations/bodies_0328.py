"""0328 post-B0 bundle: row share clocks, currency and eligibility labels, classification basis, rdq lineage.

Applied after B0 (0327). Every table widened here has a ``DEFAULT now()`` column, and
DuckDB 1.5.5 cannot replay a WAL ``ALTER TABLE`` on such a table (see 0327), so columns
are appended by the governed create/copy/swap with the old column shapes, constraints and
indexes verified afterwards. All new columns are nullable; NULL on rows written before
their writer stores them.

1. ``market_daily_metrics.shares_clock`` / ``shares_available_at`` (A8 I3): the market
   daily writer fills them automatically once present (``SHARE_CLOCK_ROW_COLUMNS``).
2. ``market_daily_metrics.currency_status`` (C1 concern 2): the owner's reporting-currency
   guard on the row, so desk readers can label currency-withheld valuation.
3. Release eligibility (C4): ``publication_releases.eligibility`` / ``gates_not_passed_json``
   and ``publication_release_datasets.eligibility`` / ``eligibility_reason``.
4. ``entity_classification.classification_basis`` / ``mapping_version`` (P6 M4), back-filled
   from the labels the P6 writer already puts in ``source``.
5. ``fundamental_periods.rdq_basis`` / ``rdq_available_at`` / ``rdq_accession_number``
   (FP-rdq): lineage of the 8-K that set ``rdq``; ``rdq`` stays NULL when no 8-K matched.
6. TODO(MIG0328-item6): P14 bar-revision tables, DDL pending (see
   :func:`_equity_bar_revision_tables`). 0328 must not be registered before it lands.

Cost: each swap copies its table once in the migration transaction and rebuilds its
primary key (and secondary indexes). Cheap while the tables are empty or small; a
populated ``market_daily_metrics`` (tens of millions of rows after run5 B2) makes the copy
and its VARCHAR primary-key ART the dominant cost (MIG0328-report.md).
"""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin
from .bodies_0327 import _columns, _constraints, _describe_fields, _index_sql, _quoted, _row_count

_SCRATCH_SUFFIX = "__rebuild_0328"

MARKET_DAILY_COLUMNS = (
    ("shares_clock", "VARCHAR"),
    ("shares_available_at", "TIMESTAMP"),
    ("currency_status", "VARCHAR"),
)
RELEASE_COLUMNS = (("eligibility", "VARCHAR"), ("gates_not_passed_json", "VARCHAR"))
RELEASE_DATASET_COLUMNS = (("eligibility", "VARCHAR"), ("eligibility_reason", "VARCHAR"))
CLASSIFICATION_COLUMNS = (("classification_basis", "VARCHAR"), ("mapping_version", "VARCHAR"))
FUNDAMENTAL_PERIOD_RDQ_COLUMNS = (
    ("rdq_basis", "VARCHAR"),
    ("rdq_available_at", "TIMESTAMP"),
    ("rdq_accession_number", "VARCHAR"),
)


def _widen(conn: duckdb.DuckDBPyConnection, table: str, additions: tuple[tuple[str, str], ...]) -> bool:
    """Append nullable columns by the replay-safe swap. False when they already exist.

    The scratch table takes the live catalog DDL plus the new columns; rows are copied
    with a row-count proof, the old table is dropped, the scratch renamed, and secondary
    indexes recreated after the rename (DuckDB refuses to rename an indexed table).
    Views over the table rebind by name.
    """
    existing = _columns(conn, table)
    names = [str(row[0]) for row in existing]
    missing = tuple((name, data_type) for name, data_type in additions if name not in names)
    if not missing:
        return False
    if len(missing) != len(additions):
        raise RuntimeError(f"0328 found {table} partially widened: missing {[name for name, _ in missing]}")
    ddl = conn.execute(
        """
        SELECT sql FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema() AND table_name = ?
        """,
        [table],
    ).fetchone()
    prefix = f"CREATE TABLE {table}("
    if ddl is None or not str(ddl[0]).startswith(prefix) or not str(ddl[0]).endswith(");"):
        raise RuntimeError(f"0328 cannot derive the {table} DDL for a governed rebuild")
    constraints = _constraints(conn, table)
    indexes = _index_sql(conn, table)
    scratch = f"{table}{_SCRATCH_SUFFIX}"
    before = _row_count(conn, table)
    added = ", ".join(f'"{name}" {data_type}' for name, data_type in missing)
    conn.execute(f"DROP TABLE IF EXISTS {scratch}")
    conn.execute(f"CREATE TABLE {scratch}({str(ddl[0])[len(prefix):-2]}, {added});")
    conn.execute(f"INSERT INTO {scratch} ({_quoted(names)}) SELECT {_quoted(names)} FROM {table}")
    after = _row_count(conn, scratch)
    if after != before:
        raise RuntimeError(f"0328 {table} rebuild row-count proof failed: {before} source rows, {after} copied")
    conn.execute(f"DROP TABLE {table}")
    conn.execute(f"ALTER TABLE {scratch} RENAME TO {table}")
    for sql in indexes:
        conn.execute(sql)
    expected = [*existing, *((name, data_type, True, None) for name, data_type in missing)]
    if (_columns(conn, table) != expected or _constraints(conn, table) != constraints
            or _index_sql(conn, table) != indexes):
        raise RuntimeError(f"0328 {table} rebuild changed existing column shapes, constraints or indexes")
    return True


def _market_daily_row_clocks(conn: duckdb.DuckDBPyConnection) -> None:
    _widen(conn, "market_daily_metrics", MARKET_DAILY_COLUMNS)
    _describe_fields(conn, "market_daily_metrics", (
        ("shares_clock",
         "Clock kind of the row's share count: dei_filing (a DEI cover count, known at its filing's "
         "available_at) or the vendor run clock split_derived | dei_matched | first_run | modeled_lag "
         "(market_daily.ARCHIVE_RUN_CLOCKS; modeled_lag is modeled, not verified). NULL when the share count "
         "is withheld, or on a row written before 0328."),
        ("shares_available_at",
         "When the row's share count was available: the DEI state's as-of instant for dei_filing, the "
         "vendor run's availability otherwise (class_sum rows: the latest over the issuer-day's class "
         "lines). NULL when withheld or on a row written before 0328."),
        ("currency_status",
         "Owner's reporting-currency guard at the row: non_usd | mixed | unknown withhold every valuation "
         "metric (valuation_withheld). NULL = no currency guard (USD reporting) or no owner link; also NULL "
         "on rows written before the market-daily writer stores it."),
    ))


def _release_eligibility(conn: duckdb.DuckDBPyConnection) -> None:
    _widen(conn, "publication_releases", RELEASE_COLUMNS)
    _widen(conn, "publication_release_datasets", RELEASE_DATASET_COLUMNS)
    _describe_fields(conn, "publication_releases", (
        ("eligibility",
         "Release eligibility from its manifest: eligible (every release gate passed) | candidate (a gate "
         "failed or is unmeasured). NULL for a release recorded before 0328 (its manifest, hashed in "
         "manifest_sha256, carries it)."),
        ("gates_not_passed_json",
         "JSON list of the release gates not passed ([] when eligible). NULL for a release recorded before "
         "0328."),
    ))
    _describe_fields(conn, "publication_release_datasets", (
        ("eligibility",
         "Dataset eligibility: eligible | candidate | research_only (research basis, never eligible). NULL "
         "for a dataset recorded before 0328."),
        ("eligibility_reason",
         "Why: release_gates (takes the release eligibility) | research_basis | not_produced_by_activation. "
         "NULL for a dataset recorded before 0328."),
    ))


def _classification_basis(conn: duckdb.DuckDBPyConnection) -> None:
    if _widen(conn, "entity_classification", CLASSIFICATION_COLUMNS):
        # Back-fill once, only from what the P6 writer stated in `source`:
        # "SEC submissions.zip SIC [<basis>] archive:<sha16>[; <mapping version>]". Nothing inferred.
        conn.execute(r"""
            UPDATE entity_classification
            SET classification_basis = nullif(regexp_extract(source, '\[([A-Za-z0-9_]+)\]', 1), ''),
                mapping_version = nullif(regexp_extract(source, ';\s*([A-Za-z0-9_.-]+)\s*$', 1), '')
        """)
    _describe_fields(conn, "entity_classification", (
        ("classification_basis",
         "Basis of the row: current_sic_snapshot = the filer's CURRENT SIC from a retained SEC submissions "
         "snapshot, valid from the snapshot receipt, not point-in-time SIC history. Back-filled by 0328 from "
         "the bracketed label in source; NULL for legacy rows whose source states no basis."),
        ("mapping_version",
         "Version of the SIC-to-taxonomy mapping of a derived row (e.g. french_siccodes12_v2, "
         "french_siccodes49_v1, sic2_partial_approximate_v1). NULL for SIC rows and for legacy rows. "
         "Back-filled by 0328 from the '; <version>' suffix of source."),
    ))


def _fundamental_period_rdq_lineage(conn: duckdb.DuckDBPyConnection) -> None:
    # Periods are rebuilt wholesale by refresh_fundamental_periods (the publication derives
    # its stage and shadow DDL from the live table, so appended columns survive): no backfill.
    _widen(conn, "fundamental_periods", FUNDAMENTAL_PERIOD_RDQ_COLUMNS)
    _describe_fields(conn, "fundamental_periods", (
        ("rdq",
         "Earnings report date from the earliest 8-K Item 2.02 whose report date lies in [period_end, fdate]: "
         "its report date when the 8-K was accepted within 4 business days of it (filed within 5 without a "
         "usable acceptance clock), else the acceptance America/New_York date (or the 8-K filing date); see "
         "rdq_basis. NULL when no 8-K matched (never substituted by the periodic filing date)."),
        ("pdate", "Preliminary earnings-release date; mirrors rdq (same 8-K, same rule)."),
        ("rdq_basis",
         "How rdq was set (fundamental_statements.RDQ_BASES): reported_date | "
         "acceptance_date_implausible_report | filing_date_implausible_report | filing_date_no_report_date. "
         "NULL when rdq is NULL or on a row written before its writer stores it."),
        ("rdq_available_at",
         "Acceptance time of the 8-K that set rdq (rdq is never known before it). NULL when rdq is NULL or "
         "on a row written before its writer stores it."),
        ("rdq_accession_number",
         "Accession number of the 8-K Item 2.02 filing that set rdq. NULL when rdq is NULL or on a row "
         "written before its writer stores it."),
    ))


def _equity_bar_revision_tables(conn: duckdb.DuckDBPyConnection) -> None:
    """Item 6 slot -- TODO(MIG0328-item6), intentionally empty until the DDL is final.

    P14 (ledger variant): ``equity_adjustment_rebases`` (one row per line per factor
    rebase) and the rare raw-restatement ``equity_daily_bar_revisions``. The exact DDL
    lands in claude-ctl/P14-report.md ("Fix (ledger)"); add it here (CREATE TABLE IF NOT
    EXISTS, catalog rows) before 0328 is registered.
    """
    _ = conn


def _post_b0_bundle(conn: duckdb.DuckDBPyConnection) -> None:
    _market_daily_row_clocks(conn)
    _release_eligibility(conn)
    _classification_basis(conn)
    _fundamental_period_rdq_lineage(conn)
    _equity_bar_revision_tables(conn)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=328, name="post_b0_bundle", up=_post_b0_bundle)]
