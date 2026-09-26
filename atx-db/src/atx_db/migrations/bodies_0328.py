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
6. P14 ledger (251f1188): ``equity_adjustment_rebases`` (one row per line per vendor
   factor rebase) and ``equity_daily_bar_revisions`` (rare raw-value restatements), DDL
   frozen from the loader; new tables, no swap.

Cost: each swap copies its table once in the migration transaction and rebuilds its
primary key (and secondary indexes). Cheap while the tables are empty or small; a
populated ``market_daily_metrics`` (tens of millions of rows after run5 B2) makes the copy
and its VARCHAR primary-key ART the dominant cost (MIG0328-report.md). Ruling: B0 applies
0327 and 0328 together, while ``market_daily_metrics`` is empty.
"""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0327 import (
    _columns,
    _constraints,
    _describe_fields,
    _index_sql,
    _quoted,
    _row_count,
    refresh_schema_contract_pin_by_swap,
)

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
    """P14 (ledger variant, 251f1188): the vendor factor-rebase ledger and raw restatements.

    DDL frozen verbatim from ``ticker_history_incremental.REBASES_TABLE_DDL`` /
    ``REVISIONS_TABLE_DDL`` (P14-report "Fix (ledger)"): no ``DEFAULT now()`` column (WAL
    replay) and no ART index; keys are catalogued, not constrained. Until 0328 the loader
    creates them only with ``allow_unmigrated_revisions_table`` (non-production).
    """
    conn.execute("""
        CREATE TABLE IF NOT EXISTS equity_adjustment_rebases (
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            partition_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            multiplier DOUBLE NOT NULL,
            first_affected_date DATE NOT NULL,
            last_affected_date DATE NOT NULL,
            detection_basis VARCHAR NOT NULL,
            cumul_return_factor DOUBLE,
            return_factor DOUBLE,
            prior_close DOUBLE,
            prior_adjusted_close DOUBLE,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL,
            CHECK (multiplier > 0 AND isfinite(multiplier)),
            CHECK (first_affected_date <= last_affected_date),
            CHECK (last_affected_date < partition_date)
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS equity_daily_bar_revisions (
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            vendor_security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            trade_date DATE NOT NULL,
            open DOUBLE,
            high DOUBLE,
            low DOUBLE,
            close DOUBLE,
            adjusted_close DOUBLE,
            volume BIGINT,
            vwap DOUBLE,
            dividend_amount DOUBLE,
            split_factor DOUBLE,
            is_adjusted BOOLEAN NOT NULL,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL,
            as_of_date DATE,
            is_latest_revision BOOLEAN NOT NULL,
            shares_outstanding BIGINT,
            market_cap_usd DOUBLE,
            superseded_at TIMESTAMP NOT NULL,
            superseded_by_run_id VARCHAR NOT NULL,
            revision_reason VARCHAR NOT NULL,
            CHECK (NOT is_latest_revision),
            CHECK (revision_reason IN ('partition_restatement')),
            CHECK (available_at IS NULL OR superseded_at >= available_at)
        )
    """)
    pit_note = ("Read with equity_daily_bars through ticker_history_incremental.bars_asof_sql; ledger "
                "multipliers are undone for receipts after the cutoff.")
    tables = (
        ("equity_adjustment_rebases", "source,security_id,partition_date,available_at",
         "Vendor factor-rebase ledger: one row per (price line, rebase) with the receipt clock, the multiplier m "
         "and the first/last affected trade date. Stored adjusted_close of those bars is on the basis after every "
         "row; the value known before a row's available_at is stored / m. Raw prices and bar clocks never move."),
        ("equity_daily_bar_revisions", "source,security_id,trade_date,superseded_at",
         "Superseded equity_daily_bars rows from raw-value restatements of a re-delivered partition "
         "(revision_reason partition_restatement); equity_daily_bars keeps one latest row per key."),
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog
          (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at)
        VALUES (?, 'silver', 'equity_daily_bars', ?, ?, ?, ?, now())
        """,
        [(name, grain, description, '["' + grain.replace(",", '","') + '"]', pit_note)
         for name, grain, description in tables],
    )
    _describe_fields(conn, "equity_adjustment_rebases", (
        ("partition_date", "Daily partition whose delivery revealed the rebase."),
        ("available_at", "Receipt clock of the rebase: greatest(partition_date + 22h, receipt), the clock of the "
                         "partition's new bars."),
        ("multiplier", "m > 0: the line's stored adjusted_close from first_affected_date to last_affected_date "
                       "was multiplied by m; divide it out for cutoffs before available_at."),
        ("detection_basis", "How the rebase was detected (vendor_crf_return_factor_recurrence)."),
    ))
    _describe_fields(conn, "equity_daily_bar_revisions", (
        ("superseded_at", "When the restating partition replaced this row (at or after its available_at)."),
        ("revision_reason", "partition_restatement: a re-delivered partition changed raw values."),
    ))
    conn.execute(
        """
        INSERT OR REPLACE INTO pit_exemption
          (table_name, missing_columns, reason, exempted_by, exempted_at, source_loaded_at)
        VALUES ('equity_adjustment_rebases', '["as_of_date"]', ?, 'MIG0328 migration 0328', now(), now())
        """,
        ["Ledger rows carry the receipt clock available_at and the partition_date session; bars_asof_sql applies "
         "them by available_at against the cutoff."],
    )


def _post_b0_bundle(conn: duckdb.DuckDBPyConnection) -> None:
    _market_daily_row_clocks(conn)
    _release_eligibility(conn)
    _classification_basis(conn)
    _fundamental_period_rdq_lineage(conn)
    _equity_bar_revision_tables(conn)
    # Never the in-place _refresh_schema_contract_v2_pin: its WAL replay corrupts the ART.
    refresh_schema_contract_pin_by_swap(conn)


MIGRATIONS = [Migration(version=328, name="post_b0_bundle", up=_post_b0_bundle)]
