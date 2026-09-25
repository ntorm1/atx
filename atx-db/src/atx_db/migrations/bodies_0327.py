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
   A3 fold already stores in ``details_json``.
5. ``v_delisting_return_coverage`` counts a halt-gap stitch at the terminal's
   effective date (R3a ``effective_terminals_sql``), not only the exact date.

DuckDB 1.5.5 cannot replay a WAL containing ``ALTER TABLE`` on a table with a
``DEFAULT now()`` column ("GetDefaultDatabase with no default database set").
Every table widened here has such a column, so columns are added by the same
governed swap as the est_actual key change (a replay-safe shape), with the old
column shapes, constraints and indexes verified afterwards.
"""

from __future__ import annotations

import duckdb

from .._forward_return_publication import effective_terminals_sql
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

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
         "Stable identity of one cessation event shared by every revision row of it "
         "(delisting_event_id is the revision row id). Back-filled with delisting_event_id for rows "
         "written before revision chains; select one revision per key by available_at <= cutoff "
         "and is_latest_revision."),
        ("existence_available_at",
         "Clock at which the event itself existed (gap-qualified absence or corroborated notice), "
         "independent of when its reason became public (A3 existence clock)."),
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
            -- Semi-join first: only accessions est_actual copied from are read.
            SELECT security_id, accession_number, concept, unit, period_start, period_end, value
            FROM sec_company_facts
            WHERE accession_number IN (SELECT accession_number FROM est_actual)
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


def _pre_run5_identity_bundle(conn: duckdb.DuckDBPyConnection) -> None:
    create_historical_identity_tables(conn)
    _historical_identity_catalog(conn)
    _market_daily_labels(conn)
    _delisting_reason_revisions(conn)
    _est_actual_period_key(conn)
    _delisting_return_coverage_effective_view(conn)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=327, name="pre_run5_identity_bundle", up=_pre_run5_identity_bundle)]
