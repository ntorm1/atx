"""0327 pre-run5 bundle: identity evidence schema, row labels, est_actual period key.

One governed migration, applied by B0 while a backup is still cheap:

1. ``security_identity_evidence`` + ``historical_security_decisions`` (L2 §5.1;
   vocabularies/validators in :mod:`atx_db.historical_identity`).
2. ``market_daily_metrics`` row-level owner labels: ``owner_security_id``,
   ``identity_basis``, ``availability_basis``, ``link_method`` (nullable).
3. ``est_actual`` period geometry: ``period_start`` + ``duration_days`` join the
   primary key, so a filing's quarter, year-to-date and comparative values are
   distinct rows. This is **not additive**: a governed rebuild (create, copy,
   row-count proof, swap). Legacy rows take ``period_start`` from the exact
   Company Facts fact they were copied from; an unprovable row fails the
   migration instead of being dropped or guessed.
4. ``delisting_events`` reason-revision columns (``delisting_event_key``,
   ``existence_available_at``, ``reason_available_at``, ``reason_at_existence``,
   ``reason_revised_after_existence``), back-filled only from the values the
   A3 fold already stores in ``details_json``; the fold writer fills them from
   0327 on.
5. ``v_delisting_return_coverage`` counts a halt-gap stitch at the terminal's
   effective date (R3a ``effective_terminals_sql``), not only the exact date.
6. ``api_schema_coverage_slo`` rows for the six ``ATX.US.ISSUER_CONTENT`` schemas
   (added by 0322 after 0307 seeded the others), from ``DEFAULT_PROVIDER_COVERAGE_SLOS``.
7. Data correction: TickerHistory ``equity_daily_bars`` runs stored in vendor
   thousands get ``shares_outstanding * 1000`` and a recomputed ``market_cap_usd``;
   each run's decision is kept in the ``equity_bar_unit_corrections`` ledger. The
   unit of an old-loader run is its recorded input format, corroborated by the
   run's median; any doubt aborts the migration (nothing is scaled on a guess).
   :func:`ticker_history_unit_inventory` is the read-only B0 gate over the same rule.

DuckDB 1.5.5 cannot replay a WAL containing ``ALTER TABLE`` on a table with a
``DEFAULT now()`` column ("GetDefaultDatabase with no default database set").
Every table widened here has such a column, so columns are added by the same
governed swap as the est_actual key change (a replay-safe shape), with the old
column shapes, constraints and indexes verified afterwards.
"""

from __future__ import annotations

import json
from pathlib import PureWindowsPath

import duckdb

# Imported values are outside the migration checksum: the view body (item 5) and the SLO rows
# (item 6) are fixed when 0327 applies. Changing effective_terminals_sql or the ISSUER_CONTENT
# rows of DEFAULT_PROVIDER_COVERAGE_SLOS later needs its own refresh migration, or fresh
# bootstraps and upgraded warehouses diverge without a checksum failure.
from .._forward_return_publication import effective_terminals_sql
from ..provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables

_SCRATCH_SUFFIX = "__rebuild_0327"

MARKET_DAILY_LABEL_COLUMNS = (
    ("owner_security_id", "VARCHAR"),
    ("identity_basis", "VARCHAR"),
    ("availability_basis", "VARCHAR"),
    ("link_method", "VARCHAR"),
)
DELISTING_REVISION_COLUMNS = (
    ("delisting_event_key", "VARCHAR"),
    ("existence_available_at", "TIMESTAMP"),
    ("reason_available_at", "TIMESTAMP"),
    ("reason_at_existence", "VARCHAR"),
    ("reason_revised_after_existence", "BOOLEAN"),
)
EST_ACTUAL_LEGACY_COLUMNS = (
    "security_id", "measure_code", "fiscal_year", "fiscal_period", "period_end", "value", "unit",
    "form", "accession_number", "announce_date", "as_of_date", "available_at", "source_loaded_at",
    "run_id", "source", "basis", "is_latest_revision",
)
EST_ACTUAL_LEGACY_KEY = ("security_id", "measure_code", "fiscal_year", "fiscal_period", "accession_number")
EST_ACTUAL_KEY = (*EST_ACTUAL_LEGACY_KEY, "period_end", "period_start")
# ticker_history.SOURCE_NAME, frozen: the only source whose vendor `shares` field is in thousands.
TICKER_HISTORY_SOURCE = "tbltickerhistory3_10y"
SHARES_CORRECTION_ID = "tbltickerhistory3_10y:equity_daily_bars.shares_outstanding:thousands_to_shares"
ISSUER_CONTENT_DATASET = "ATX.US.ISSUER_CONTENT"

# Item 7 unit rule (A9 I1 ruling): an old-loader run's unit is its recorded input format AND the
# run's median must agree; every doubt aborts the migration. Constants are frozen here, not
# imported, so a later loader change cannot alter what this one-time correction decides.
#
# Format evidence: the input path the old loaders stored in dataset_runs.params_json (bulk
# publisher: source_path or legacy tsv_path via asdict(options); chunk loader: zip_path via
# encode_params) -> the loaders' own rule (ticker_history_bulk.SHARES_UNIT_BY_FORMAT over the
# ticker_history_quality.source_format suffixes; the chunk loader's ZIP holds only the units TSV).
FORMAT_EVIDENCE_KEYS = ("source_path", "tsv_path", "zip_path")
SHARES_EVIDENCE_BY_SUFFIX = {
    ".parquet": ("parquet", "thousands"), ".pq": ("parquet", "thousands"),
    ".tsv": ("tsv", "units"), ".txt": ("tsv", "units"), ".tab": ("tsv", "units"),
    ".zip": ("zip", "units"),
}
# A run spanning at least this many securities is a cross-section: its median is a property of
# the listed universe (A8-measured stored medians: parquet thousands 17.6K-29.5K in every year,
# TSV shares in the tens of millions), so it must sit clearly on one side of the ambiguous band.
CROSS_SECTION_MIN_SECURITIES = 1_000
# Cross-section median below this => thousands (read as shares, the median listed security would
# have < 200K shares); 6.8x above the highest measured thousands median.
CROSS_SECTION_THOUSANDS_BELOW = 200_000
# Cross-section median at or above this => shares (read as thousands, the median listed security
# would have >= 5B shares); a units universe sits in the tens of millions. [200K, 5M) = ambiguous.
CROSS_SECTION_SHARES_FROM = 5_000_000
# A narrow run (fewer securities) describes a few chosen names, so its median can only veto a
# unit that puts that median outside any real line's share count; the format then decides.
PLAUSIBLE_SHARES_MIN = 10_000  # new ETFs launch at 25K-50K shares
PLAUSIBLE_SHARES_MAX = 100_000_000_000  # ~4x NVDA's 24.6B, the largest US share count
# Per-row check of a run decided thousands: a stored value above this would exceed
# PLAUSIBLE_SHARES_MAX after x1000, so the row is already in shares (mixed-unit run) => abort.
THOUSANDS_ROW_CEILING = PLAUSIBLE_SHARES_MAX // 1000
# Without format evidence the median alone decides only a full-universe run
# (>= this many rows AND >= CROSS_SECTION_MIN_SECURITIES securities).
NO_EVIDENCE_MIN_ROWS = 1_000_000


def create_historical_identity_tables(conn: duckdb.DuckDBPyConnection) -> None:
    """Create the two L2 §5.1 tables (idempotent; also usable on small fixtures).

    No primary key/ART index: both tables can reach security-day scale, and
    optional ART maintenance was the bounded-write memory problem retired in
    0326. Keys are declared in ``table_catalog`` and enforced by the audits in
    :mod:`atx_db.historical_identity`; row invariants are CHECK constraints.
    """
    conn.execute("""
        CREATE TABLE IF NOT EXISTS security_identity_evidence (
            evidence_id VARCHAR NOT NULL,
            fact_kind VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            native_key_namespace VARCHAR NOT NULL,
            native_key VARCHAR NOT NULL,
            security_id VARCHAR,
            cik VARCHAR,
            symbol VARCHAR,
            value_json VARCHAR NOT NULL,
            valid_from DATE,
            valid_to DATE,
            source_locator VARCHAR,
            artifact_sha256 VARCHAR,
            source_revision_id VARCHAR,
            source_time_text VARCHAR,
            source_published_at TIMESTAMP,
            observed_at TIMESTAMP,
            available_at TIMESTAMP,
            evidence_status VARCHAR NOT NULL,
            availability_status VARCHAR NOT NULL,
            method VARCHAR NOT NULL,
            rejection_reason VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            CHECK (evidence_status IN ('verified_dated', 'snapshot', 'reconstructed', 'inferred',
                                       'unknown', 'conflicting')),
            CHECK (availability_status IN ('verified', 'modeled', 'unknown')),
            CHECK (length(trim(evidence_id)) > 0 AND length(trim(source)) > 0
                   AND length(trim(native_key_namespace)) > 0 AND length(trim(native_key)) > 0
                   AND length(trim(method)) > 0),
            CHECK (valid_to IS NULL OR (valid_from IS NOT NULL AND valid_to > valid_from)),
            CHECK (evidence_status <> 'verified_dated'
                   OR (length(trim(coalesce(artifact_sha256, ''))) > 0
                       AND length(trim(coalesce(source_locator, ''))) > 0
                       AND valid_from IS NOT NULL)),
            CHECK (availability_status <> 'verified'
                   OR (source_published_at IS NOT NULL AND available_at IS NOT NULL)),
            CHECK (available_at IS NULL OR source_published_at IS NULL
                   OR available_at >= source_published_at),
            CHECK (artifact_sha256 IS NULL OR regexp_full_match(artifact_sha256, '[0-9a-f]{64}')),
            CHECK (cik IS NULL OR regexp_full_match(cik, '[0-9]{10}')),
            CHECK (evidence_status NOT IN ('unknown', 'conflicting')
                   OR length(trim(coalesce(rejection_reason, ''))) > 0),
            CHECK (evidence_status IN ('unknown', 'conflicting') OR rejection_reason IS NULL)
        )
    """)
    conn.execute("""
        CREATE TABLE IF NOT EXISTS historical_security_decisions (
            run_id VARCHAR NOT NULL,
            universe_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            decision_date DATE NOT NULL,
            decision_at TIMESTAMP NOT NULL,
            decision_rule_version VARCHAR NOT NULL,
            eligibility VARCHAR NOT NULL,
            reason VARCHAR NOT NULL,
            identity_evidence_id VARCHAR,
            type_evidence_id VARCHAR,
            venue_evidence_id VARCHAR,
            cik VARCHAR,
            symbol VARCHAR,
            security_type VARCHAR,
            share_class VARCHAR,
            primary_mic VARCHAR,
            available_at TIMESTAMP,
            details_json VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            CHECK (eligibility IN ('eligible', 'excluded', 'unresolved')),
            CHECK (length(trim(reason)) > 0),
            CHECK (eligibility <> 'eligible'
                   OR (identity_evidence_id IS NOT NULL AND type_evidence_id IS NOT NULL
                       AND venue_evidence_id IS NOT NULL AND available_at IS NOT NULL
                       AND available_at <= decision_at)),
            CHECK (cik IS NULL OR regexp_full_match(cik, '[0-9]{10}'))
        )
    """)


def _columns(conn: duckdb.DuckDBPyConnection, table: str) -> list[tuple[object, ...]]:
    return conn.execute(
        """
        SELECT column_name, data_type, is_nullable, column_default
        FROM duckdb_columns()
        WHERE database_name = current_database() AND schema_name = current_schema()
          AND table_name = ?
        ORDER BY column_index
        """,
        [table],
    ).fetchall()


def _constraints(conn: duckdb.DuckDBPyConnection, table: str) -> list[tuple[object, ...]]:
    rows = conn.execute(
        """
        SELECT constraint_type, constraint_column_names, expression
        FROM duckdb_constraints()
        WHERE database_name = current_database() AND schema_name = current_schema()
          AND table_name = ?
        """,
        [table],
    ).fetchall()
    return sorted((kind, tuple(names or ()), expression or "") for kind, names, expression in rows)


def _primary_key(conn: duckdb.DuckDBPyConnection, table: str) -> tuple[str, ...]:
    for kind, names, _ in _constraints(conn, table):
        if kind == "PRIMARY KEY":
            return tuple(str(name) for name in names)
    return ()


def _index_sql(conn: duckdb.DuckDBPyConnection, table: str) -> list[str]:
    rows = conn.execute(
        """
        SELECT index_name, sql FROM duckdb_indexes()
        WHERE database_name = current_database() AND schema_name = current_schema()
          AND table_name = ?
        ORDER BY index_name
        """,
        [table],
    ).fetchall()
    missing = [name for name, sql in rows if not sql]
    if missing:
        raise RuntimeError(f"0327 cannot recreate {table} indexes without stored SQL: {missing}")
    return [str(sql) for _, sql in rows]


def _row_count(conn: duckdb.DuckDBPyConnection, table: str) -> int:
    row = conn.execute(f"SELECT count(*) FROM {table}").fetchone()
    assert row is not None
    return int(row[0])


def _quoted(names: tuple[str, ...] | list[str]) -> str:
    return ", ".join(f'"{name}"' for name in names)


def _swap_rebuild(
    conn: duckdb.DuckDBPyConnection, table: str, create_scratch_sql: str, copy_sql: str
) -> int:
    """Create ``<table>__rebuild_0327``, copy, prove the row count, swap, recreate indexes.

    Indexes are recreated after the rename: DuckDB refuses to rename a table
    that secondary indexes depend on. Returns the proven row count.
    """
    scratch = f"{table}{_SCRATCH_SUFFIX}"
    indexes = _index_sql(conn, table)
    before = _row_count(conn, table)
    conn.execute(f"DROP TABLE IF EXISTS {scratch}")
    conn.execute(create_scratch_sql)
    conn.execute(copy_sql)
    after = _row_count(conn, scratch)
    if after != before:
        raise RuntimeError(f"0327 {table} rebuild row-count proof failed: {before} source rows, {after} copied")
    conn.execute(f"DROP TABLE {table}")
    conn.execute(f"ALTER TABLE {scratch} RENAME TO {table}")
    for sql in indexes:
        conn.execute(sql)
    if _index_sql(conn, table) != indexes:
        raise RuntimeError(f"0327 {table} rebuild did not restore its indexes")
    return before


def _add_nullable_columns(
    conn: duckdb.DuckDBPyConnection, table: str, additions: tuple[tuple[str, str], ...]
) -> None:
    """Append nullable columns via the replay-safe swap; verify nothing else changed."""
    existing = _columns(conn, table)
    names = [str(row[0]) for row in existing]
    missing = tuple((name, data_type) for name, data_type in additions if name not in names)
    if not missing:
        return
    if len(missing) != len(additions):
        raise RuntimeError(f"0327 found {table} partially widened: missing {[n for n, _ in missing]}")
    ddl = conn.execute(
        """
        SELECT sql FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema()
          AND table_name = ?
        """,
        [table],
    ).fetchone()
    prefix = f"CREATE TABLE {table}("
    if ddl is None or not str(ddl[0]).startswith(prefix) or not str(ddl[0]).endswith(");"):
        raise RuntimeError(f"0327 cannot derive the {table} DDL for a governed rebuild")
    body = str(ddl[0])[len(prefix):-2]
    added = ", ".join(f'"{name}" {data_type}' for name, data_type in missing)
    constraints = _constraints(conn, table)
    _swap_rebuild(
        conn,
        table,
        f"CREATE TABLE {table}{_SCRATCH_SUFFIX}({body}, {added});",
        f"INSERT INTO {table}{_SCRATCH_SUFFIX} ({_quoted(names)}) SELECT {_quoted(names)} FROM {table}",
    )
    expected = [*existing, *((name, data_type, True, None) for name, data_type in missing)]
    if _columns(conn, table) != expected or _constraints(conn, table) != constraints:
        raise RuntimeError(f"0327 {table} rebuild changed existing column shapes or constraints")


def _replace_rows_by_swap(
    conn: duckdb.DuckDBPyConnection, table: str, columns: tuple[str, ...], select_sql: str,
    params: list[object] | None = None,
) -> None:
    """Replace every row of ``table`` with ``select_sql`` via a fresh table and a swap.

    No in-place UPDATE/DELETE touches the persisted indexed table: DuckDB 1.5.5 can
    replay such writes from the WAL into a corrupt ART (the next write to the table
    then invalidates the database). The fresh table takes the live catalog DDL (so its
    constraints and primary key), is filled INSERT-only, replaces the old one, and gets
    its secondary indexes back from their stored SQL after the rename.
    """
    ddl = conn.execute(
        """
        SELECT sql FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema() AND table_name = ?
        """,
        [table],
    ).fetchone()
    if ddl is None or not str(ddl[0]).startswith(f"CREATE TABLE {table}("):
        raise RuntimeError(f"cannot derive the {table} DDL for a row swap")
    indexes = _index_sql(conn, table)
    constraints = _constraints(conn, table)
    shape = _columns(conn, table)
    scratch = f"{table}__swap"
    conn.execute(f"DROP TABLE IF EXISTS {scratch}")
    conn.execute(f"CREATE TABLE {scratch}{str(ddl[0])[len(f'CREATE TABLE {table}'):]}")
    conn.execute(f"INSERT INTO {scratch} ({_quoted(columns)}) {select_sql}", params or [])
    conn.execute(f"DROP TABLE {table}")
    conn.execute(f"ALTER TABLE {scratch} RENAME TO {table}")
    for sql in indexes:
        conn.execute(sql)
    if _columns(conn, table) != shape or _constraints(conn, table) != constraints or _index_sql(conn, table) != indexes:
        raise RuntimeError(f"row swap changed the shape, constraints or indexes of {table}")


def refresh_schema_contract_pin_by_swap(conn: duckdb.DuckDBPyConnection) -> None:
    """Persist the live schema contract and its v2 pin without in-place writes.

    Replaces ``_refresh_schema_contract_v2_pin`` from 0327 on: that helper DELETEs and
    UPDATEs the indexed ``schema_contract`` in place, and a WAL replay of such a commit
    leaves a corrupt ART (probed: the next write invalidates the database; CHECKPOINT
    does not heal it). Here the manifest is computed first, then ``schema_contract`` and
    ``schema_contract_version`` are each rebuilt INSERT-only and swapped in. A row keeps
    its ``source_loaded_at`` when its contract fields are unchanged. Later migrations
    must call this, never the in-place helper.
    """
    import pandas as pd

    from ..schema_contract import (
        SCHEMA_CONTRACT_VERSION,
        assert_schema_contract_version,
        build_contract_manifest,
        schema_contract_sha256,
    )

    manifest = build_contract_manifest(conn)
    manifest_sha256 = schema_contract_sha256(manifest)
    fields = ("data_type", "nullable", "is_natural_key", "is_pit_column", "declared_in", "unit", "sign", "scale",
              "natural_key")
    seed = pd.DataFrame.from_records(
        [(table, spec.name, spec.data_type, bool(spec.nullable), bool(spec.is_natural_key), bool(spec.is_pit_column),
          spec.declared_in, spec.unit, spec.sign, spec.scale, bool(spec.natural_key))
         for table, specs in manifest.items() for spec in specs],
        columns=["table_name", "column_name", *fields],
    )
    conn.register("_schema_contract_pin_seed", seed)
    try:
        unchanged = " AND ".join(f"e.{name} IS NOT DISTINCT FROM s.{name}" for name in fields)
        _replace_rows_by_swap(
            conn, "schema_contract",
            ("table_name", "column_name", "data_type", "nullable", "is_natural_key", "is_pit_column", "declared_in",
             "manifest_sha256", "source_loaded_at", "unit", "sign", "scale", "natural_key"),
            f"""
            SELECT s.table_name, s.column_name, s.data_type, s.nullable, s.is_natural_key, s.is_pit_column,
                   s.declared_in, ?,
                   CASE WHEN e.table_name IS NOT NULL AND {unchanged} THEN e.source_loaded_at
                        ELSE CAST(now() AS TIMESTAMP) END,
                   CAST(s.unit AS VARCHAR), CAST(s.sign AS VARCHAR), CAST(s.scale AS VARCHAR), s.natural_key
            FROM _schema_contract_pin_seed s
            LEFT JOIN schema_contract e ON e.table_name = s.table_name AND e.column_name = s.column_name
            """,
            [manifest_sha256],
        )
    finally:
        conn.unregister("_schema_contract_pin_seed")
    _replace_rows_by_swap(
        conn, "schema_contract_version", ("version", "manifest_sha256", "declared_by_migration", "source_loaded_at"),
        """
        SELECT version, CASE WHEN version = ? THEN ? ELSE manifest_sha256 END, declared_by_migration, source_loaded_at
        FROM schema_contract_version
        """,
        [SCHEMA_CONTRACT_VERSION, manifest_sha256],
    )
    assert_schema_contract_version(conn, manifest=manifest)


def _market_daily_labels(conn: duckdb.DuckDBPyConnection) -> None:
    _add_nullable_columns(conn, "market_daily_metrics", MARKET_DAILY_LABEL_COLUMNS)
    rows = (
        ("owner_security_id",
         "Owner security whose fundamentals/DEI shares were joined to this price line by the "
         "market owner bridge (may differ from security_id for secondary class lines). NULL = no "
         "owner link (market-only row) or a legacy row written before the label existed."),
        ("identity_basis",
         "Owner-link identity basis: verified_dated (dated identity evidence; the only basis that "
         "counts toward certification), current_ticker_unverified (RX1 reconstructed backcast of "
         "the current SEC ticker map) or shared_security_id_unverified (legacy equal-id join). NULL "
         "= unlinked or unlabeled legacy row."),
        ("availability_basis",
         "Availability basis of the owner link: verified (publication evidence) or modeled (link "
         "assumed known from the line's first bar cutoff although only observed later). NULL = "
         "unlinked or unlabeled legacy row."),
        ("link_method",
         "Owner-bridge rule that linked the line: dated_evidence, current_sec_ticker, "
         "cik_security_id or shared_security_id. NULL = unlinked or unlabeled legacy row."),
    )
    _describe_fields(conn, "market_daily_metrics", rows)


def _delisting_reason_revisions(conn: duckdb.DuckDBPyConnection) -> None:
    _add_nullable_columns(conn, "delisting_events", DELISTING_REVISION_COLUMNS)
    # Copy only values the fold already stored; nothing is inferred. One row per event today,
    # so each existing row is its own (single) revision.
    conn.execute("""
        UPDATE delisting_events
        SET delisting_event_key = coalesce(delisting_event_key, delisting_event_id),
            existence_available_at = coalesce(existence_available_at, CASE WHEN json_valid(details_json)
                THEN TRY_CAST(json_extract_string(details_json, '$.existence_available_at') AS TIMESTAMP) END),
            reason_available_at = coalesce(reason_available_at, CASE WHEN json_valid(details_json)
                THEN TRY_CAST(json_extract_string(details_json, '$.reason_available_at') AS TIMESTAMP) END),
            reason_at_existence = coalesce(reason_at_existence, CASE WHEN json_valid(details_json)
                THEN json_extract_string(details_json, '$.reason_at_existence') END),
            reason_revised_after_existence = coalesce(reason_revised_after_existence,
                CASE WHEN json_valid(details_json)
                THEN TRY_CAST(json_extract(details_json, '$.reason_revised_after_existence') AS BOOLEAN) END)
        WHERE delisting_event_key IS NULL
    """)
    rows = (
        ("delisting_event_key",
         "Identity of one cessation event, shared by every revision row of it (delisting_event_id "
         "is the row id). The public-evidence fold writes one row per event, so today key = "
         "delisting_event_id (also back-filled so for rows present at 0327); a revision-chain "
         "writer adds rows under the same key. NULL for rows of writers without event clocks "
         "(the listing-status proxy builder)."),
        ("existence_available_at",
         "Clock at which the event itself existed (gap-qualified absence or corroborated notice), "
         "independent of when its reason became public (A3 existence clock). NULL when the writer "
         "records no existence clock."),
        ("reason_available_at",
         "Clock at which this row's delist_reason became public; available_at = max(existence, "
         "reason) so a reason is never visible before it was public."),
        ("reason_at_existence",
         "Reason visible at existence_available_at; differs from delist_reason when the reason was "
         "revised later (e.g. a merger form after cessation)."),
        ("reason_revised_after_existence",
         "True when delist_reason was first public after the event existed; a revision chain then "
         "publishes the reason_at_existence row from the existence clock."),
    )
    _describe_fields(conn, "delisting_events", rows)


def _est_actual_period_key(conn: duckdb.DuckDBPyConnection) -> None:
    columns = [str(row[0]) for row in _columns(conn, "est_actual")]
    if "period_start" in columns:
        if _primary_key(conn, "est_actual") != EST_ACTUAL_KEY:
            raise RuntimeError("0327 found est_actual.period_start without the 0327 primary key")
        return
    if tuple(columns) != EST_ACTUAL_LEGACY_COLUMNS or _primary_key(conn, "est_actual") != EST_ACTUAL_LEGACY_KEY:
        raise RuntimeError(f"0327 refuses to rebuild an unexpected est_actual shape: {columns}")
    legacy_key = " AND ".join(f"g.{name} = a.{name}" for name in EST_ACTUAL_LEGACY_KEY)
    geometry = """
        WITH concepts AS (
            SELECT measure_code, unnest(from_json(us_gaap_concepts, '["VARCHAR"]')) AS concept
            FROM est_measure
            WHERE us_gaap_concepts IS NOT NULL AND json_valid(us_gaap_concepts)
        ), facts AS MATERIALIZED (
            -- Semi-joins first: only est_measure concepts of accessions est_actual copied from.
            SELECT security_id, accession_number, concept, unit, period_start, period_end, value
            FROM sec_company_facts
            WHERE accession_number IN (SELECT accession_number FROM est_actual)
              AND concept IN (SELECT concept FROM concepts)
              AND period_start IS NOT NULL AND period_start <= period_end
        ), geometry AS (
            SELECT a.security_id, a.measure_code, a.fiscal_year, a.fiscal_period, a.accession_number,
                   count(DISTINCT f.period_start) AS starts, min(f.period_start) AS period_start
            FROM est_actual a
            LEFT JOIN concepts c ON c.measure_code = a.measure_code
            LEFT JOIN facts f
              ON f.security_id = a.security_id AND f.accession_number = a.accession_number
             AND f.period_end = a.period_end AND f.concept = c.concept
             AND f.value = a.value AND f.unit IS NOT DISTINCT FROM a.unit
            GROUP BY ALL
        )
    """
    if _row_count(conn, "est_actual"):
        unresolved = conn.execute(geometry + """
            SELECT count(*) FILTER (WHERE starts = 0), count(*) FILTER (WHERE starts > 1) FROM geometry
        """).fetchone()
        assert unresolved is not None
        if unresolved[0] or unresolved[1]:
            raise RuntimeError(
                "0327 cannot prove est_actual period geometry for legacy rows: "
                f"{unresolved[0]} without an exact Company Facts fact, {unresolved[1]} ambiguous "
                "(several period starts). est_actual is derived from sec_company_facts: delete "
                "those rows, migrate, then rerun the est_actual job."
            )
    scratch = f"est_actual{_SCRATCH_SUFFIX}"
    _swap_rebuild(
        conn,
        "est_actual",
        f"""
        CREATE TABLE {scratch} (
            security_id VARCHAR NOT NULL,
            measure_code VARCHAR NOT NULL,
            fiscal_year INTEGER NOT NULL,
            fiscal_period VARCHAR NOT NULL,
            period_end DATE NOT NULL,
            "value" DOUBLE,
            unit VARCHAR,
            form VARCHAR,
            accession_number VARCHAR NOT NULL,
            announce_date DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            "source" VARCHAR NOT NULL,
            basis VARCHAR,
            is_latest_revision BOOLEAN,
            period_start DATE NOT NULL,
            duration_days INTEGER NOT NULL,
            CHECK (duration_days >= 1 AND duration_days = date_diff('day', period_start, period_end) + 1),
            PRIMARY KEY (security_id, measure_code, fiscal_year, fiscal_period, accession_number,
                         period_end, period_start)
        )
        """,
        f"INSERT INTO {scratch} ({_quoted(list(EST_ACTUAL_LEGACY_COLUMNS))}, period_start, duration_days)"
        + geometry
        + f"""
        SELECT {", ".join(f'a."{name}"' for name in EST_ACTUAL_LEGACY_COLUMNS)},
               g.period_start, date_diff('day', g.period_start, a.period_end) + 1
        FROM est_actual a JOIN geometry g ON {legacy_key}
        """,
    )
    _catalog_fields_for_tables(conn, ("est_actual",))
    _describe_fields(conn, "est_actual", (
        ("period_start",
         "Start of the reported duration (Company Facts period start). With period_end it separates "
         "a filing's quarter, year-to-date and prior-period comparative values that share filing "
         "labels (fiscal_year/fiscal_period are the filing context, not the period)."),
        ("duration_days",
         "period_end - period_start + 1 (inclusive days). Quarterly readers select ~70-120 days, "
         "annual readers ~330-380 days."),
    ))
    conn.execute(
        """
        UPDATE table_catalog
        SET grain = ?, natural_key_json = ?,
            description = 'Reported actual values mapped from SEC companyfacts, one row per filing '
                          'fact geometry (period_start, period_end): quarter, year-to-date and '
                          'comparative values of one filing are distinct rows.',
            updated_at = now()
        WHERE table_name = 'est_actual'
        """,
        [",".join(EST_ACTUAL_KEY), "[" + ",".join(f'"{name}"' for name in EST_ACTUAL_KEY) + "]"],
    )


def _delisting_return_coverage_effective_view(conn: duckdb.DuckDBPyConnection) -> None:
    """Recreate the 0188 monitor so halt-gap stitches are not reported as dropped.

    The publisher (forward_return_publication_v2) dates a stitched terminal at
    the first session after the last trade when 1..31 sessions separate them.
    The last trade here is any positive print (adjusted or raw close) on the
    XNYS bar calendar, the survivorship DQC's rule. A stitch at the exact
    terminal date (v1 and legacy rows) also counts. Universe, terminal and
    missing counts are unchanged.
    """
    effective = effective_terminals_sql(terminals="delist_universe", bars="printed", calendar="cal")
    conn.execute(f"""
        CREATE OR REPLACE VIEW v_delisting_return_coverage AS
        WITH delist_universe AS (
            SELECT DISTINCT security_id, delist_date
            FROM delisting_events
            WHERE security_id IS NOT NULL
            UNION
            SELECT DISTINCT security_id, delist_date
            FROM delisting_terminal_returns
            WHERE is_latest_revision AND security_id IS NOT NULL
        ),
        terminal AS (
            SELECT security_id, delist_date, terminal_return_source
            FROM delisting_terminal_returns
            WHERE is_latest_revision
        ),
        printed AS (
            SELECT DISTINCT security_id, trade_date,
                   CAST(NULL AS DOUBLE) AS price, CAST(NULL AS TIMESTAMP) AS price_available_at
            FROM equity_daily_bars
            WHERE security_id IN (SELECT security_id FROM delist_universe)
              AND ((adjusted_close > 0 AND isfinite(adjusted_close)) OR (close > 0 AND isfinite(close)))
        ),
        cal AS (
            SELECT DISTINCT trade_date FROM trading_calendar
            WHERE calendar_id = 'XNYS' AND source = 'equity_daily_bars calendar' AND is_open
        ),
        effective AS ({effective}
        ),
        stitched AS (
            SELECT DISTINCT security_id, delist_date
            FROM forward_returns_survivorship_safe
            WHERE is_stitched AND is_latest_revision
        ),
        universe AS (
            SELECT e.security_id, e.delist_date,
                   EXISTS (
                       SELECT 1 FROM stitched s
                       WHERE s.security_id = e.security_id
                         AND s.delist_date IN (e.effective_delist_date, e.delist_date)
                   ) AS is_stitched
            FROM effective e
        )
        SELECT
            date_trunc('month', u.delist_date)                                    AS delist_cohort_month,
            count(*)                                                              AS delist_security_days,
            count(t.security_id) FILTER (WHERE t.terminal_return_source = 'observed')
                                                                                  AS observed_terminal_count,
            count(t.security_id) FILTER (WHERE t.terminal_return_source = 'policy')
                                                                                  AS policy_terminal_count,
            count(*) FILTER (WHERE t.security_id IS NULL)                         AS missing_terminal_count,
            count(*) FILTER (WHERE u.is_stitched)                                 AS stitched_count,
            count(*) FILTER (WHERE t.security_id IS NOT NULL AND NOT u.is_stitched)
                                                                                  AS dropped_count
        FROM universe u
        LEFT JOIN terminal t
          ON t.security_id = u.security_id AND t.delist_date = u.delist_date
        GROUP BY 1
        ORDER BY 1
    """)
    conn.execute("""
        UPDATE table_catalog
        SET description = 'Per delist-cohort-month terminal-return coverage: observed/policy/missing '
                          'terminal counts and stitched/dropped counts. A stitch counts at the '
                          'terminal''s halt-gap effective date (first session after the last trade '
                          'when 1..31 sessions apart; forward_return_publication_v2) or at its exact '
                          'date (v1/legacy rows). dropped_count is the coarse "has a terminal '
                          'return but no stitched row" monitor; the exact halt gate is the '
                          'survivorship_forward_return_drops_delisted_names anti-join.',
            updated_at = now()
        WHERE table_name = 'v_delisting_return_coverage'
    """)
    _describe_fields(conn, "v_delisting_return_coverage", (
        ("stitched_count",
         "Count of delisted security-days in the cohort with a stitched forward_returns_survivorship_safe "
         "row at the halt-gap effective terminal date or at the exact delist date."),
        ("dropped_count",
         "Count of delisted security-days in the cohort that carry a terminal return but no stitched "
         "forward-return row at either date (coarse survivorship-drop monitor)."),
    ))


def _describe_fields(
    conn: duckdb.DuckDBPyConnection, table: str, rows: tuple[tuple[str, str], ...]
) -> None:
    _catalog_fields_for_tables(conn, (table,))
    conn.executemany(
        """
        UPDATE field_catalog SET description = ?, updated_at = now()
        WHERE table_name = ? AND field_name = ?
        """,
        [(description, table, field) for field, description in rows],
    )


def _historical_identity_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    tables = (
        ("security_identity_evidence", "evidence_id",
         "L2 identity evidence: one source-stated or explicitly inferred fact (issuer_link, "
         "security_type, primary_listing, symbol_mapping, delisting_effective, terminal_return) per "
         "row with namespaced native key, raw value payload, exclusive validity interval, artifact "
         "SHA-256, locator, revision, and separate publication/observation/availability clocks.",
         "Clocks are distinct: source_published_at (publication), observed_at (retrieval), "
         "available_at (PIT). evidence_status labels the fact; availability_status labels the clock. "
         "Only verified_dated + verified rows count toward certification. valid_to is exclusive."),
        ("historical_security_decisions", "run_id,universe_id,security_id,decision_date",
         "Strict verified-basis decision ledger: every candidate security per decision session with "
         "selected identity/type/venue evidence ids, normalized CIK/type/class/MIC, eligibility and a "
         "specific reason. Reconstructed research views are separate projections.",
         "decision_date/decision_at are the as-of session and exact cutoff; available_at is at least "
         "the max selected evidence clock and never after decision_at for eligible rows."),
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog
          (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at)
        VALUES (?, 'silver', 'historical_identity', ?, ?, ?, ?, now())
        """,
        [(name, grain, description, '["' + grain.replace(",", '","') + '"]', notes)
         for name, grain, description, notes in tables],
    )
    _catalog_fields_for_tables(conn, tuple(name for name, *_ in tables))
    _describe_fields(conn, "security_identity_evidence", (
        ("evidence_status",
         "verified_dated | snapshot | reconstructed | inferred | unknown | conflicting (L2 §5.1). "
         "verified_dated does not imply verified availability."),
        ("availability_status",
         "verified (publication evidence at or before available_at) | modeled | unknown."),
        ("valid_to", "Exclusive end of economic validity; NULL = open. Universe outputs use inclusive "
                     "ends: convert at the boundary."),
        ("native_key_namespace", "Namespace of the source-native instrument key (ticker text is never "
                                 "globally unique across time)."),
        ("value_json", "Raw value payload as stated by the source (raw values and timezone text kept)."),
        ("source_time_text", "Raw source timestamp text exactly as published, including timezone."),
        ("rejection_reason", "Required for unknown/conflicting rows; NULL for accepted rows."),
    ))
    _describe_fields(conn, "historical_security_decisions", (
        ("eligibility", "eligible | excluded | unresolved; every row carries a specific reason."),
        ("available_at", "At least the max selected evidence available_at; <= decision_at when eligible."),
    ))
    conn.executemany(
        """
        INSERT OR REPLACE INTO pit_exemption
          (table_name, missing_columns, reason, exempted_by, exempted_at, source_loaded_at)
        VALUES (?, '["as_of_date"]', ?, 'A9 migration 0327', now(), now())
        """,
        [
            ("security_identity_evidence",
             "Evidence rows carry explicit valid_from/valid_to, source_published_at, observed_at and "
             "available_at clocks; a single as_of_date would conflate validity, publication and observation."),
            ("historical_security_decisions",
             "decision_date is the as-of session and decision_at the exact cutoff of each decision."),
        ],
    )


def _issuer_content_coverage_slos(conn: duckdb.DuckDBPyConnection) -> None:
    """Seed the six ISSUER_CONTENT schema SLOs (0322 added the schemas, 0307 predates them)."""
    rows = [slo for slo in DEFAULT_PROVIDER_COVERAGE_SLOS if slo.dataset_id == ISSUER_CONTENT_DATASET]
    if len(rows) != 6:
        raise RuntimeError(f"0327 expects six {ISSUER_CONTENT_DATASET} coverage SLOs, found {len(rows)}")
    conn.executemany(
        """
        INSERT OR REPLACE INTO api_schema_coverage_slo (
            dataset_id,schema_code,slo_version,expected_history_start,
            minimum_history_years,minimum_security_count,minimum_item_count,
            maximum_freshness_lag_days,citation,description,item_count_basis,is_active,
            valid_from,valid_to,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,?,true,TIMESTAMP '1900-01-01',NULL,now())
        """,
        [
            (slo.dataset_id, slo.schema_code, slo.slo_version, slo.expected_history_start,
             slo.minimum_history_years, slo.minimum_security_count, slo.minimum_item_count,
             slo.maximum_freshness_lag_days, slo.citation, slo.description, slo.item_count_basis)
            for slo in rows
        ],
    )


def _params_object(params_json: object) -> dict[str, object]:
    if not isinstance(params_json, str):
        return {}
    try:
        params = json.loads(params_json)
    except ValueError:
        return {}
    return params if isinstance(params, dict) else {}


def _format_evidence(params: dict[str, object]) -> tuple[str | None, str | None, str | None, str | None]:
    """``(source_format, stored_unit, evidence, conflict)`` from the old loaders' recorded input path."""
    found = []
    for key in FORMAT_EVIDENCE_KEYS:
        value = params.get(key)
        if isinstance(value, str) and value.strip():
            source_format, unit = SHARES_EVIDENCE_BY_SUFFIX.get(
                PureWindowsPath(value.strip()).suffix.lower(), (None, None)
            )
            found.append((key, value, source_format, unit))
    known = [item for item in found if item[3] is not None]
    if len({item[3] for item in known}) > 1:
        paths = "; ".join(f"{key}={value}" for key, value, _, _ in known)
        return None, None, paths, f"conflicting format evidence ({paths})"
    if not known:  # an unknown suffix is recorded but is not evidence
        return None, None, (f"{found[0][0]}={found[0][1]}" if found else None), None
    key, value, source_format, unit = known[0]
    return source_format, unit, f"{key}={value}", None


def _median_verdict(median: float | None, distinct_securities: int) -> str | None:
    """Unit(s) a run's median positive stored ``shares_outstanding`` is consistent with."""
    if median is None:
        return None
    if distinct_securities >= CROSS_SECTION_MIN_SECURITIES:
        if median < CROSS_SECTION_THOUSANDS_BELOW:
            return "thousands"
        return "units" if median >= CROSS_SECTION_SHARES_FROM else "ambiguous"
    as_units = PLAUSIBLE_SHARES_MIN <= median <= PLAUSIBLE_SHARES_MAX
    as_thousands = PLAUSIBLE_SHARES_MIN <= median * 1000 <= PLAUSIBLE_SHARES_MAX
    if as_units and as_thousands:
        return "either"
    return "units" if as_units else "thousands" if as_thousands else "implausible"


def _share_unit_decision(
    rows_in_run: int, distinct_securities: int, median: float | None,
    params_json: object, has_dataset_run: bool,
) -> dict[str, object]:
    """Apply the item 7 rule to one run; ``action`` is ``scale_x1000``, ``none`` or ``abort``."""
    params = _params_object(params_json)
    loader_unit = params.get("shares_unit")
    loader_unit = loader_unit if isinstance(loader_unit, str) and loader_unit.strip() else None
    source_format, format_unit, evidence, conflict = _format_evidence(params)
    verdict = _median_verdict(median, distinct_securities)
    decision: dict[str, object] = {
        "loader_shares_unit": loader_unit, "source_format": source_format, "format_unit": format_unit,
        "format_evidence": evidence, "median_verdict": verdict,
    }

    def decided(basis: str, stored_unit: str | None, reason: str) -> dict[str, object]:
        scale = stored_unit == "thousands"
        return {**decision, "decision_basis": basis, "stored_unit": stored_unit,
                "decision": ("scaled_thousands_to_shares" if scale else "already_shares"
                             if stored_unit == "units" else "undetermined_no_positive_shares"),
                "action": "scale_x1000" if scale else "none", "reason": reason}

    def abort(reason: str) -> dict[str, object]:
        return {**decision, "decision_basis": None, "stored_unit": None, "decision": None,
                "action": "abort", "reason": reason}

    if loader_unit is not None:
        return decided("loader_params", "units", f"unit-aware loader stored shares (vendor unit {loader_unit})")
    if median is None:
        return decided("no_positive_shares", None, "no positive shares_outstanding: nothing to scale")
    if conflict is not None:
        return abort(conflict)
    if verdict == "ambiguous":
        return abort(f"cross-section median {median:,.0f} is inside the ambiguous band "
                     f"[{CROSS_SECTION_THOUSANDS_BELOW:,}, {CROSS_SECTION_SHARES_FROM:,})")
    if verdict == "implausible":
        return abort(f"median {median:,.0f} is no real share count in either unit")
    if format_unit is not None:
        if verdict not in (format_unit, "either"):
            return abort(f"disagreement: {evidence} means {format_unit}, the median {median:,.0f} "
                         f"over {distinct_securities:,} securities means {verdict}")
        corroboration = (
            f"the median fits {verdict}" if verdict == format_unit
            else f"the median of this narrow run ({distinct_securities:,} securities) fits either unit"
        )
        return decided("format_and_median", format_unit, f"{source_format} input means {format_unit}; {corroboration}")
    if rows_in_run < NO_EVIDENCE_MIN_ROWS or distinct_securities < CROSS_SECTION_MIN_SECURITIES:
        missing = ("no dataset_runs row" if not has_dataset_run
                   else f"params_json names no known input format ({evidence or 'no input path'})")
        return abort(f"no format evidence ({missing}) and not a full-universe run "
                     f"({rows_in_run:,} rows, {distinct_securities:,} securities)")
    return decided("median_full_universe", verdict,
                   f"no format evidence; full-universe median {median:,.0f} means {verdict}")


def ticker_history_unit_inventory(conn: duckdb.DuckDBPyConnection) -> list[dict[str, object]]:
    """Read-only B0 gate: every TickerHistory run's unit evidence and 0327's action for it.

    Only SELECTs, so it runs on a read-only connection, before 0327 (no ledger yet) or
    after it. One row per ``run_id`` of source ``tbltickerhistory3_10y`` with non-null
    shares: rows, distinct securities, approx median positive shares, the unit-aware
    loader's recorded ``shares_unit`` or the old loader's input path and format, the
    median verdict, the decided stored unit and ``action``: ``scale_x1000``, ``none``,
    ``abort`` (0327 would raise; ``reason`` says why) or ``already_ledgered``.

    A run decided thousands is also checked row by row: rows already above
    ``THOUSANDS_ROW_CEILING`` (in shares, so x1000 would exceed ``PLAUSIBLE_SHARES_MAX``)
    make it a mixed-unit run => ``abort``, with up to five of the largest such rows in
    ``mixed_unit_examples``.
    """
    ledger = conn.execute(
        """
        SELECT count(*) FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema()
          AND table_name = 'equity_bar_unit_corrections'
        """
    ).fetchone()
    ledger_sql = (
        "SELECT run_id, decision FROM equity_bar_unit_corrections "
        "WHERE source = ? AND column_name = 'shares_outstanding'"
        if ledger is not None and ledger[0]
        else "SELECT CAST(NULL AS VARCHAR) AS run_id, CAST(NULL AS VARCHAR) AS decision WHERE false"
    )
    rows = conn.execute(
        f"""
        WITH runs AS (
            SELECT run_id, count(*) AS rows_in_run,
                   count(DISTINCT security_id) AS distinct_securities,
                   approx_quantile(shares_outstanding, 0.5) FILTER (WHERE shares_outstanding > 0) AS median_shares,
                   count(*) FILTER (WHERE shares_outstanding > {int(THOUSANDS_ROW_CEILING)}) AS rows_above_ceiling
            FROM equity_daily_bars
            WHERE "source" = ? AND shares_outstanding IS NOT NULL
            GROUP BY run_id
        ), ledger AS ({ledger_sql})
        SELECT r.run_id, r.rows_in_run, r.distinct_securities, r.median_shares, r.rows_above_ceiling,
               d.params_json, d.run_id IS NOT NULL AS has_dataset_run, l.decision
        FROM runs r
        LEFT JOIN dataset_runs d ON d.run_id = r.run_id
        LEFT JOIN ledger l ON l.run_id IS NOT DISTINCT FROM r.run_id
        ORDER BY r.run_id NULLS FIRST
        """,
        [TICKER_HISTORY_SOURCE, *([TICKER_HISTORY_SOURCE] if "?" in ledger_sql else [])],
    ).fetchall()
    inventory = []
    for run_id, rows_in_run, securities, median, above, params_json, has_dataset_run, ledgered in rows:
        median = None if median is None else float(median)
        decision = _share_unit_decision(int(rows_in_run), int(securities), median, params_json, bool(has_dataset_run))
        examples: list[dict[str, object]] = []
        if ledgered is not None:
            decision = {**decision, "action": "already_ledgered", "reason": f"0327 ledger decision: {ledgered}"}
        elif decision["action"] == "scale_x1000" and above:
            examples = [
                {"security_id": security_id, "trade_date": str(trade_date), "shares_outstanding": int(shares)}
                for security_id, trade_date, shares in conn.execute(
                    f"""
                    SELECT security_id, trade_date, shares_outstanding FROM equity_daily_bars
                    WHERE "source" = ? AND run_id IS NOT DISTINCT FROM ?
                      AND shares_outstanding > {int(THOUSANDS_ROW_CEILING)}
                    ORDER BY shares_outstanding DESC, security_id, trade_date
                    LIMIT 5
                    """,
                    [TICKER_HISTORY_SOURCE, run_id],
                ).fetchall()
            ]
            decision = {
                **decision, "decision_basis": None, "stored_unit": None, "decision": None, "action": "abort",
                "reason": f"mixed units: {int(above):,} rows of this run decided thousands ({decision['reason']}) "
                          f"already exceed {THOUSANDS_ROW_CEILING:,} stored, so x1000 would put them above "
                          f"{PLAUSIBLE_SHARES_MAX:,} shares",
            }
        inventory.append({"run_id": run_id, "rows_in_run": int(rows_in_run), "distinct_securities": int(securities),
                          "median_positive_shares": median, "rows_above_thousands_ceiling": int(above),
                          "has_dataset_run": bool(has_dataset_run), **decision, "mixed_unit_examples": examples})
    return inventory


def _ticker_history_share_units(conn: duckdb.DuckDBPyConnection) -> None:
    """Scale TickerHistory shares stored in vendor thousands to shares, once per load run.

    Both TickerHistory files publish under one source name but differ in unit: the
    parquet's ``shares`` is in thousands (AAPL 15,204,137 = 15.2B shares), the zip TSV
    in shares. Rows of one ``run_id`` come from one file, so the unit is decided per run
    by :func:`_share_unit_decision`:

    * ``dataset_runs.params_json`` names a ``shares_unit``: the unit-aware bulk loader (A8)
      stored shares; recorded ``already_shares`` (basis ``loader_params``), never rescaled.
    * Otherwise the old loader's recorded input path gives the unit by the loaders' own rule
      (parquet = thousands; TSV text or the chunk loader's ZIP = shares) and the run's median
      must agree: a cross-section (>= ``CROSS_SECTION_MIN_SECURITIES``) must fall on the
      format's side of the ambiguous band; a narrow run's median must be a real share count
      under the format's unit. Basis ``format_and_median``.
    * Without format evidence only a full-universe run is decided, by its median alone
      (basis ``median_full_universe``).

    Disagreement, conflicting paths, an ambiguous or implausible median, a small run
    without format evidence, or a thousands run holding rows already in shares (above
    ``THOUSANDS_ROW_CEILING``) raise before anything is written: the governed migrate rolls
    back and restores, and B0 stops for a manual unit ruling (read the same decisions
    beforehand with :func:`ticker_history_unit_inventory`). Thousands runs get
    ``shares_outstanding * 1000`` and ``market_cap_usd = scaled shares * close`` (the
    loaders' formula); every decision is written to ``equity_bar_unit_corrections``, and a
    run with a ledger row is never scaled again. Other sources are never touched. One
    set-based UPDATE per thousands run (no table rewrite, no ALTER).
    """
    conn.execute("""
        CREATE TABLE IF NOT EXISTS equity_bar_unit_corrections (
            correction_id VARCHAR NOT NULL PRIMARY KEY,
            table_name VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            column_name VARCHAR NOT NULL,
            rows_in_run BIGINT NOT NULL,
            distinct_securities BIGINT NOT NULL,
            median_positive_value DOUBLE,
            median_verdict VARCHAR,
            loader_shares_unit VARCHAR,
            source_format VARCHAR,
            format_unit VARCHAR,
            format_evidence VARCHAR,
            decision_basis VARCHAR NOT NULL,
            decision VARCHAR NOT NULL,
            factor DOUBLE NOT NULL,
            rows_corrected BIGINT NOT NULL,
            recomputed_columns VARCHAR NOT NULL,
            rule VARCHAR NOT NULL,
            applied_by VARCHAR NOT NULL,
            applied_at TIMESTAMP NOT NULL DEFAULT now(),
            CHECK (decision IN ('scaled_thousands_to_shares', 'already_shares', 'undetermined_no_positive_shares')),
            CHECK (decision_basis IN ('loader_params', 'format_and_median', 'median_full_universe',
                                      'no_positive_shares')),
            CHECK (decision_basis <> 'loader_params'
                   OR (decision = 'already_shares' AND loader_shares_unit IS NOT NULL)),
            CHECK (decision_basis <> 'format_and_median'
                   OR (format_unit IN ('thousands', 'units') AND median_verdict IS NOT NULL
                       AND median_verdict IN (format_unit, 'either'))),
            CHECK (decision_basis <> 'median_full_universe'
                   OR (format_unit IS NULL AND median_verdict IN ('thousands', 'units'))),
            CHECK ((decision_basis = 'no_positive_shares') = (decision = 'undetermined_no_positive_shares')),
            CHECK (factor = CASE WHEN decision = 'scaled_thousands_to_shares' THEN 1000.0 ELSE 1.0 END),
            CHECK (rows_corrected = CASE WHEN decision = 'scaled_thousands_to_shares' THEN rows_in_run ELSE 0 END)
        )
    """)
    inventory = ticker_history_unit_inventory(conn)
    refused = [run for run in inventory if run["action"] == "abort"]
    if refused:
        raise RuntimeError(
            f"0327 refuses to decide the TickerHistory share unit of {len(refused)} run(s); nothing was "
            "scaled and the migration rolls back. A manual unit ruling is needed for: "
            + "; ".join(
                f"run_id={run['run_id']!r} ({run['rows_in_run']:,} rows, {run['distinct_securities']:,} "
                f"securities, median {run['median_positive_shares']}): {run['reason']}"
                + (f" e.g. {run['mixed_unit_examples']}" if run["mixed_unit_examples"] else "")
                for run in refused
            )
        )
    rule = (
        "per run_id of tbltickerhistory3_10y: params_json.shares_unit (unit-aware loader stored shares) => "
        "never rescaled; else the old loader's input path (source_path/tsv_path/zip_path: .parquet/.pq => "
        "thousands; .tsv/.txt/.tab/.zip => shares) must agree with the approx median positive shares "
        f"(>= {CROSS_SECTION_MIN_SECURITIES} securities: < {CROSS_SECTION_THOUSANDS_BELOW} thousands, >= "
        f"{CROSS_SECTION_SHARES_FROM} shares, between ambiguous; fewer securities: median x unit within "
        f"[{PLAUSIBLE_SHARES_MIN}, {PLAUSIBLE_SHARES_MAX}] shares); without a known path only a run of >= "
        f"{NO_EVIDENCE_MIN_ROWS} rows and >= {CROSS_SECTION_MIN_SECURITIES} securities is decided by its "
        f"median; anything else, or a thousands run with any row above {THOUSANDS_ROW_CEILING} stored "
        "(mixed units), aborts 0327. thousands => shares_outstanding *= 1000; "
        "market_cap_usd = shares_outstanding * close"
    )
    for run in inventory:
        if run["action"] == "already_ledgered":
            continue
        run_id, rows_in_run = run["run_id"], run["rows_in_run"]
        scale = run["action"] == "scale_x1000"
        corrected = 0
        if scale:
            result = conn.execute(
                """
                UPDATE equity_daily_bars
                SET shares_outstanding = shares_outstanding * 1000,
                    market_cap_usd = CAST(shares_outstanding * 1000 AS DOUBLE) * "close"
                WHERE "source" = ? AND run_id IS NOT DISTINCT FROM ? AND shares_outstanding IS NOT NULL
                """,
                [TICKER_HISTORY_SOURCE, run_id],
            ).fetchone()
            corrected = int(result[0]) if result is not None else 0
            if corrected != rows_in_run:
                raise RuntimeError(
                    f"0327 share-unit correction touched {corrected} rows, expected {rows_in_run} (run {run_id!r})"
                )
        conn.execute(
            """
            INSERT INTO equity_bar_unit_corrections (
                correction_id, table_name, source, run_id, column_name, rows_in_run, distinct_securities,
                median_positive_value, median_verdict, loader_shares_unit, source_format, format_unit,
                format_evidence, decision_basis, decision, factor, rows_corrected, recomputed_columns,
                rule, applied_by
            ) VALUES (?, 'equity_daily_bars', ?, ?, 'shares_outstanding', ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?,
                      'migration 0327')
            """,
            [
                f"{SHARES_CORRECTION_ID}:{run_id if run_id is not None else '<null-run>'}",
                TICKER_HISTORY_SOURCE, run_id, rows_in_run, run["distinct_securities"],
                run["median_positive_shares"], run["median_verdict"], run["loader_shares_unit"],
                run["source_format"], run["format_unit"], run["format_evidence"],
                run["decision_basis"], run["decision"],
                1000.0 if scale else 1.0, corrected,
                "market_cap_usd = shares_outstanding * close" if scale else "none",
                rule,
            ],
        )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog
          (table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at)
        VALUES ('equity_bar_unit_corrections', 'control', 'equity_daily_bars', 'correction_id',
                'Ledger of stored equity_daily_bars unit corrections: one row per source load run with the '
                'measured median and its verdict, the loader-recorded unit or the input-file format '
                'evidence, the decision basis, the decision and the rows corrected. A run with a row is '
                'never corrected again.',
                '["correction_id"]',
                'Control ledger; corrections change stored values in place (no new revision rows).', now())
        """
    )
    _catalog_fields_for_tables(conn, ("equity_bar_unit_corrections",))
    _describe_fields(conn, "equity_daily_bars", (
        ("shares_outstanding",
         "Shares outstanding in shares. TickerHistory parquet deliveries state thousands: the bulk loader "
         "scales them and migration 0327 scaled rows loaded before (ledger equity_bar_unit_corrections)."),
        ("market_cap_usd",
         "shares_outstanding * close in USD, recomputed by migration 0327 for corrected TickerHistory runs."),
    ))


def _pre_run5_identity_bundle(conn: duckdb.DuckDBPyConnection) -> None:
    create_historical_identity_tables(conn)
    _historical_identity_catalog(conn)
    _market_daily_labels(conn)
    _delisting_reason_revisions(conn)
    _est_actual_period_key(conn)
    _delisting_return_coverage_effective_view(conn)
    _issuer_content_coverage_slos(conn)
    _ticker_history_share_units(conn)
    refresh_schema_contract_pin_by_swap(conn)


MIGRATIONS = [Migration(version=327, name="pre_run5_identity_bundle", up=_pre_run5_identity_bundle)]
