"""Versioned migration framework for the atx-impl DuckDB warehouse.

Migrations are ordered, idempotent, and tracked in the schema_migrations table.
Call apply_pending_migrations(conn) after ensure_quant_schema to bring the schema
up to date. It is safe to call multiple times; only unapplied migrations run.
"""

from __future__ import annotations

import ast
import contextlib
import hashlib
import inspect
import textwrap
import uuid

import duckdb
from dataclasses import dataclass
from typing import Callable


@dataclass(frozen=True)
class Migration:
    version: int
    name: str
    up: Callable[[duckdb.DuckDBPyConnection], None]


def _noop(conn: duckdb.DuckDBPyConnection) -> None:
    """No-op body — baseline schema is already created by ensure_quant_schema."""


def _migration_version_text(version: int) -> str:
    return str(version).zfill(4)


def _migration_source_checksum(migration: Migration) -> str:
    """Stable sha256 digest of a migration's ``up`` source plus direct helper sources."""
    parts: list[tuple[str, str]] = []
    seen: set[int] = set()

    def normalize_source(func: Callable) -> str:
        source = inspect.getsource(func)
        return textwrap.dedent(source).replace("\r\n", "\n").replace("\r", "\n").strip()

    def direct_module_helper_names(source: str) -> set[str]:
        try:
            tree = ast.parse(source)
        except SyntaxError:
            return set()
        names: set[str] = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Call) and isinstance(node.func, ast.Name):
                name = node.func.id
                helper = migration.up.__globals__.get(name)
                if callable(helper) and getattr(helper, "__name__", "") != migration.up.__name__:
                    names.add(name)
        return names

    def collect(name: str, func: Callable, depth: int) -> None:
        identity = id(func)
        if identity in seen:
            return
        seen.add(identity)
        try:
            source = normalize_source(func)
        except (OSError, TypeError):
            source = repr(func)
        parts.append((name, source))
        if depth >= 4:
            return
        for helper_name in sorted(direct_module_helper_names(source)):
            helper = migration.up.__globals__.get(helper_name)
            if callable(helper):
                collect(helper_name, helper, depth + 1)

    collect(migration.up.__name__, migration.up, 0)
    payload = "\n\n".join(
        f"# symbol:{name}\n{source}" for name, source in sorted(parts, key=lambda item: item[0])
    )
    return hashlib.sha256(f"{payload}\n".encode("utf-8")).hexdigest()


def _migration_by_version() -> dict[int, Migration]:
    _validate_migration_registry()
    return {migration.version: migration for migration in MIGRATIONS}


def _validate_migration_registry() -> None:
    versions = [migration.version for migration in MIGRATIONS]
    if versions != sorted(versions):
        raise RuntimeError(f"MIGRATIONS must be sorted ascending: {versions}")
    duplicates = sorted({version for version in versions if versions.count(version) > 1})
    if duplicates:
        formatted = ", ".join(_migration_version_text(version) for version in duplicates)
        raise RuntimeError(f"MIGRATIONS contains duplicate versions: {formatted}")


def verify_migration_checksums(
    conn: duckdb.DuckDBPyConnection, *, allow_missing: bool = False
) -> None:
    """Verify the append-only invariant for every applied numeric migration."""
    migrations_by_version = _migration_by_version()
    rows = conn.execute(
        """
        SELECT CAST(version AS INTEGER) AS version_int, version, checksum
        FROM schema_migrations
        WHERE version ~ '^[0-9]+$'
        ORDER BY version_int
        """
    ).fetchall()

    failures: list[str] = []
    for version_int, _version_text, stored_checksum in rows:
        migration = migrations_by_version.get(version_int)
        display_version = _migration_version_text(version_int)
        if migration is None:
            failures.append(f"{display_version}: no migration source is registered")
            continue
        expected_checksum = _migration_source_checksum(migration)
        if stored_checksum in (None, ""):
            if allow_missing:
                continue
            failures.append(f"{display_version}: missing stored checksum")
            continue
        if stored_checksum != expected_checksum:
            failures.append(
                f"{display_version}: stored checksum {stored_checksum} "
                f"does not match current source {expected_checksum}"
            )

    if failures:
        raise RuntimeError(
            "Migration checksum verification failed: " + "; ".join(failures)
        )


def _backfill_missing_migration_checksums(conn: duckdb.DuckDBPyConnection) -> None:
    migrations_by_version = _migration_by_version()
    rows = conn.execute(
        """
        SELECT CAST(version AS INTEGER) AS version_int, version
        FROM schema_migrations
        WHERE version ~ '^[0-9]+$'
          AND (checksum IS NULL OR checksum = '')
        ORDER BY version_int
        """
    ).fetchall()
    updates = [
        (_migration_source_checksum(migrations_by_version[version_int]), version_text)
        for version_int, version_text in rows
        if version_int in migrations_by_version
    ]
    if updates:
        conn.executemany(
            """
            UPDATE schema_migrations
            SET checksum = ?
            WHERE version = ?
              AND (checksum IS NULL OR checksum = '')
            """,
            updates,
        )


def _ensure_apply_lock_table(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS migration_apply_lock (
            lock_name VARCHAR PRIMARY KEY,
            holder_run_id VARCHAR NOT NULL,
            heartbeat_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )


def release_apply_lock(conn: duckdb.DuckDBPyConnection, run_id: str) -> None:
    """Release the singleton migration apply lock held by ``run_id``."""
    table_exists = conn.execute(
        """
        SELECT count(*)
        FROM duckdb_tables()
        WHERE table_name = 'migration_apply_lock'
        """
    ).fetchone()[0]
    if not table_exists:
        return

    row = conn.execute(
        """
        SELECT holder_run_id
        FROM migration_apply_lock
        WHERE lock_name = 'schema_migrations'
        """
    ).fetchone()
    if row is None:
        return
    holder = row[0]
    if holder != run_id:
        raise RuntimeError(
            "migration apply lock is held "
            f"by run_id {holder}; refusing release by run_id {run_id}"
        )
    conn.execute(
        """
        DELETE FROM migration_apply_lock
        WHERE lock_name = 'schema_migrations'
          AND holder_run_id = ?
        """,
        [run_id],
    )


def claim_apply_lock(conn: duckdb.DuckDBPyConnection, run_id: str) -> None:
    """Persistently claim the migration apply sentinel row."""
    _ensure_apply_lock_table(conn)
    try:
        conn.execute(
            """
            INSERT INTO migration_apply_lock (lock_name, holder_run_id, heartbeat_at)
            VALUES ('schema_migrations', ?, CURRENT_TIMESTAMP)
            """,
            [run_id],
        )
    except Exception as exc:
        row = conn.execute(
            """
            SELECT holder_run_id, heartbeat_at
            FROM migration_apply_lock
            WHERE lock_name = 'schema_migrations'
            """
        ).fetchone()
        holder, heartbeat = row if row is not None else ("unknown", "unknown")
        raise RuntimeError(
            "migration apply lock is already held "
            f"by run_id {holder} heartbeat_at {heartbeat}; aborting"
        ) from exc


@contextlib.contextmanager
def acquire_apply_lock(conn: duckdb.DuckDBPyConnection, run_id: str):
    """Acquire the migration apply sentinel row or fail fast with the holder."""
    claim_apply_lock(conn, run_id)
    try:
        yield
    finally:
        release_apply_lock(conn, run_id)


def _schema_evolution_alters(conn: duckdb.DuckDBPyConnection) -> None:
    """Apply the ALTER TABLE evolution statements that were previously in _ensure_schema_evolution.

    All statements use ADD COLUMN IF NOT EXISTS so they are idempotent.
    """
    for statement in (
        "ALTER TABLE raw_source_files ADD COLUMN IF NOT EXISTS metadata_json VARCHAR",
        "ALTER TABLE security_identifier_history ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE security_identifier_history ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE exchange_listings ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE exchange_listings ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE equity_daily_bars ADD COLUMN IF NOT EXISTS vendor_security_id VARCHAR",
        "ALTER TABLE equity_daily_bars ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE equity_daily_bars ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE corporate_actions ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE corporate_actions ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE sec_company_facts ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE sec_company_facts ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE fundamental_points ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE fundamental_points ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE macro_observations ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE universe_memberships ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE universe_memberships ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE feature_values ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE feature_values ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE etl_job_definitions ADD COLUMN IF NOT EXISTS max_retries INTEGER DEFAULT 0",
        "ALTER TABLE etl_job_definitions ADD COLUMN IF NOT EXISTS retry_delay_seconds DOUBLE DEFAULT 0",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS attempt_count INTEGER DEFAULT 0",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS max_retries INTEGER DEFAULT 0",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS retry_delay_seconds DOUBLE DEFAULT 0",
    ):
        conn.execute(statement)


def _reference_classifications(conn: duckdb.DuckDBPyConnection) -> None:
    """Create the S1 reference-classification tables if they don't already exist.

    These tables are also created by ensure_quant_schema (CREATE TABLE IF NOT EXISTS),
    so this migration is a no-op for databases that run ensure_quant_schema first.
    It exists so the migration log has a versioned record of when the tables were introduced.
    """
    for statement in (
        """
        CREATE TABLE IF NOT EXISTS taxonomy (
            taxonomy_id VARCHAR PRIMARY KEY,
            code VARCHAR NOT NULL UNIQUE,
            name VARCHAR NOT NULL,
            provider VARCHAR,
            version VARCHAR,
            is_hierarchical BOOLEAN NOT NULL DEFAULT true,
            description VARCHAR,
            source VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS taxonomy_node (
            node_id VARCHAR PRIMARY KEY,
            taxonomy_id VARCHAR NOT NULL,
            node_code VARCHAR NOT NULL,
            node_label VARCHAR NOT NULL,
            parent_node_id VARCHAR,
            level INTEGER NOT NULL,
            sort_order INTEGER
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS entity_classification (
            classification_id VARCHAR PRIMARY KEY,
            security_id VARCHAR NOT NULL,
            taxonomy_id VARCHAR NOT NULL,
            node_id VARCHAR NOT NULL,
            node_code VARCHAR NOT NULL,
            is_primary BOOLEAN NOT NULL DEFAULT false,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR NOT NULL
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS taxonomy_mapping (
            mapping_id VARCHAR PRIMARY KEY,
            from_taxonomy_id VARCHAR NOT NULL,
            from_node_code VARCHAR NOT NULL,
            to_taxonomy_id VARCHAR NOT NULL,
            to_node_code VARCHAR NOT NULL,
            relationship VARCHAR NOT NULL,
            confidence DOUBLE,
            source VARCHAR
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_entity_classification_security ON entity_classification(security_id)",
        "CREATE INDEX IF NOT EXISTS idx_entity_classification_taxonomy_code ON entity_classification(taxonomy_id, node_code)",
        "CREATE INDEX IF NOT EXISTS idx_taxonomy_node_taxonomy_parent ON taxonomy_node(taxonomy_id, parent_node_id)",
    ):
        conn.execute(statement)


FUNDAMENTAL_STATEMENT_MAP_KEY = ("source", "taxonomy", "concept", "industry_template")


def _fundamental_statement_map_pk_columns(conn: duckdb.DuckDBPyConnection) -> tuple[str, ...]:
    try:
        row = conn.execute(
            """
            SELECT constraint_column_names
            FROM duckdb_constraints()
            WHERE table_name = 'fundamental_statement_map'
              AND constraint_type = 'PRIMARY KEY'
            """
        ).fetchone()
    except Exception:
        return ()
    if row is None or row[0] is None:
        return ()
    return tuple(str(col) for col in row[0])


def _create_fundamental_statement_map_table(
    conn: duckdb.DuckDBPyConnection,
    table_name: str,
) -> None:
    conn.execute(
        f"""
        CREATE TABLE IF NOT EXISTS {table_name} (
            source VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            statement_type VARCHAR NOT NULL,
            statement_section VARCHAR NOT NULL,
            canonical_metric VARCHAR NOT NULL,
            canonical_label VARCHAR NOT NULL,
            period_type VARCHAR NOT NULL,
            normal_balance VARCHAR NOT NULL,
            unit_type VARCHAR NOT NULL,
            value_multiplier DOUBLE NOT NULL DEFAULT 1.0,
            concept_priority INTEGER NOT NULL DEFAULT 100,
            is_core_metric BOOLEAN NOT NULL DEFAULT true,
            is_active BOOLEAN NOT NULL DEFAULT true,
            notes VARCHAR,
            item_id INTEGER,
            industry_template VARCHAR DEFAULT 'ALL',
            is_derived BOOLEAN DEFAULT FALSE,
            derivation_expr VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (source, taxonomy, concept, industry_template)
        )
        """
    )


def _fundamental_statement_map_industry_key(conn: duckdb.DuckDBPyConnection) -> None:
    """Allow the same source/taxonomy/concept to coexist under industry overlays."""

    _create_fundamental_statement_map_table(conn, "fundamental_statement_map")
    conn.execute("DROP INDEX IF EXISTS idx_fundamental_statement_map_lookup")
    for statement in (
        "ALTER TABLE fundamental_statement_map ADD COLUMN IF NOT EXISTS item_id INTEGER",
        "ALTER TABLE fundamental_statement_map ADD COLUMN IF NOT EXISTS industry_template VARCHAR DEFAULT 'ALL'",
        "ALTER TABLE fundamental_statement_map ADD COLUMN IF NOT EXISTS is_derived BOOLEAN DEFAULT FALSE",
        "ALTER TABLE fundamental_statement_map ADD COLUMN IF NOT EXISTS derivation_expr VARCHAR",
    ):
        conn.execute(statement)

    if _fundamental_statement_map_pk_columns(conn) != FUNDAMENTAL_STATEMENT_MAP_KEY:
        scratch = "fundamental_statement_map_rekey"
        conn.execute(f"DROP TABLE IF EXISTS {scratch}")
        _create_fundamental_statement_map_table(conn, scratch)
        conn.execute(
            f"""
            INSERT OR REPLACE INTO {scratch} (
                source,
                taxonomy,
                concept,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                period_type,
                normal_balance,
                unit_type,
                value_multiplier,
                concept_priority,
                is_core_metric,
                is_active,
                notes,
                item_id,
                industry_template,
                is_derived,
                derivation_expr,
                updated_at
            )
            SELECT
                source,
                taxonomy,
                concept,
                statement_type,
                statement_section,
                canonical_metric,
                canonical_label,
                period_type,
                normal_balance,
                unit_type,
                value_multiplier,
                concept_priority,
                is_core_metric,
                is_active,
                notes,
                item_id,
                coalesce(nullif(industry_template, ''), 'ALL') AS industry_template,
                coalesce(is_derived, FALSE) AS is_derived,
                derivation_expr,
                coalesce(updated_at, now()) AS updated_at
            FROM fundamental_statement_map
            """
        )
        conn.execute("DROP TABLE fundamental_statement_map")
        conn.execute(f"ALTER TABLE {scratch} RENAME TO fundamental_statement_map")

    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_fundamental_statement_map_lookup "
        "ON fundamental_statement_map(taxonomy, concept, is_active)"
    )


def _fundamental_concept_dictionary(conn: duckdb.DuckDBPyConnection) -> None:
    """S4a: add item_id, industry_template, is_derived, derivation_expr to fundamental_statement_map.

    DuckDB requires dropping dependent indexes before ALTER TABLE, then recreating them.
    All statements use ADD COLUMN IF NOT EXISTS so this is idempotent on fresh DBs where
    ensure_quant_schema already created the table with these columns.
    """
    _fundamental_statement_map_industry_key(conn)


def _fundamental_period_four_dates(conn: duckdb.DuckDBPyConnection) -> None:
    """S4c: expose Compustat-style period dates as named fields."""

    for statement in (
        "ALTER TABLE fundamental_periods ADD COLUMN IF NOT EXISTS datadate DATE",
        "ALTER TABLE fundamental_periods ADD COLUMN IF NOT EXISTS rdq DATE",
        "ALTER TABLE fundamental_periods ADD COLUMN IF NOT EXISTS pdate DATE",
        "ALTER TABLE fundamental_periods ADD COLUMN IF NOT EXISTS fdate DATE",
        "ALTER TABLE fundamental_periods ADD COLUMN IF NOT EXISTS ldate DATE",
    ):
        conn.execute(statement)


def _xbrl_validation_results(conn: duckdb.DuckDBPyConnection) -> None:
    """S4d: persist filing-level XBRL validation and DQC-style rule outcomes."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_validation_results (
            validation_id VARCHAR PRIMARY KEY,
            validation_run_id VARCHAR NOT NULL,
            rule_family VARCHAR NOT NULL,
            rule_code VARCHAR NOT NULL,
            severity VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            form VARCHAR,
            filing_date DATE,
            acceptance_datetime TIMESTAMP,
            primary_document VARCHAR NOT NULL,
            role_uri VARCHAR,
            parent_taxonomy VARCHAR,
            parent_concept VARCHAR NOT NULL,
            context_ref VARCHAR NOT NULL,
            unit_ref VARCHAR,
            parent_fact_id VARCHAR,
            parent_value DOUBLE,
            child_weighted_sum DOUBLE,
            absolute_difference DOUBLE,
            tolerance DOUBLE NOT NULL,
            child_count INTEGER NOT NULL,
            child_facts_json VARCHAR NOT NULL,
            message VARCHAR,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_xbrl_validation_results_filing ON xbrl_validation_results(security_id, accession_number, primary_document)",
        "CREATE INDEX IF NOT EXISTS idx_xbrl_validation_results_status ON xbrl_validation_results(rule_family, status, severity)",
    ):
        conn.execute(statement)


def _shares_outstanding_history(conn: duckdb.DuckDBPyConnection) -> None:
    """S5: dedicated PIT share-count history derived from SEC XBRL facts."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS shares_outstanding_history (
            share_history_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            share_count_type VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            effective_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            form VARCHAR,
            accession_number VARCHAR NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            share_count DOUBLE NOT NULL,
            source_url VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_shares_outstanding_history_security ON shares_outstanding_history(security_id, share_count_type, effective_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_shares_outstanding_history_latest ON shares_outstanding_history(is_latest_revision, security_id, share_count_type)",
    ):
        conn.execute(statement)


def _corporate_action_adjustment_factors(conn: duckdb.DuckDBPyConnection) -> None:
    """S5: corporate-action type dimension and event-level adjustment factors."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS corp_action_type_dim (
            type_code INTEGER PRIMARY KEY,
            event_type VARCHAR NOT NULL,
            category VARCHAR NOT NULL,
            sub_category VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            crsp_distcd INTEGER,
            dtcc_caev VARCHAR,
            bloomberg_type VARCHAR,
            factset_type VARCHAR,
            affects_price BOOLEAN NOT NULL,
            affects_shares BOOLEAN NOT NULL,
            taxable BOOLEAN,
            mandatory BOOLEAN NOT NULL DEFAULT true,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS adjustment_factor_history (
            adjustment_factor_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            source_action_source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            ex_date DATE NOT NULL,
            event_type VARCHAR NOT NULL,
            type_code INTEGER NOT NULL,
            event_ref_id VARCHAR NOT NULL,
            factor_price DOUBLE NOT NULL,
            factor_shares DOUBLE NOT NULL,
            factor_volume DOUBLE NOT NULL,
            ratio_numerator DOUBLE,
            ratio_denominator DOUBLE,
            cash_div_amount DOUBLE,
            cash_div_currency VARCHAR,
            cumulative_price_factor DOUBLE,
            cumulative_share_factor DOUBLE,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_corp_action_type_dim_event ON corp_action_type_dim(event_type, type_code)",
        "CREATE INDEX IF NOT EXISTS idx_adjustment_factor_history_security ON adjustment_factor_history(security_id, ex_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_adjustment_factor_history_event ON adjustment_factor_history(event_type, type_code, ex_date)",
    ):
        conn.execute(statement)


def _adjustment_factor_classification_reason(conn: duckdb.DuckDBPyConnection) -> None:
    """S5b: record why a raw corporate action normalized to an event type."""

    conn.execute(
        "ALTER TABLE adjustment_factor_history ADD COLUMN IF NOT EXISTS classification_reason VARCHAR"
    )
    conn.execute(
        """
        UPDATE adjustment_factor_history
        SET classification_reason = 'legacy_pre_0012_unclassified'
        WHERE classification_reason IS NULL
           OR classification_reason = ''
        """
    )


def _daily_adjustment_factors(conn: duckdb.DuckDBPyConnection) -> None:
    """S5c: PIT daily split and total-return adjustment factors."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS daily_adjustment_factors (
            daily_adjustment_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            bar_source VARCHAR NOT NULL,
            factor_source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            trade_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            split_price_factor DOUBLE NOT NULL,
            split_share_factor DOUBLE NOT NULL,
            dividend_total_return_factor DOUBLE NOT NULL,
            total_return_price_factor DOUBLE NOT NULL,
            raw_close DOUBLE NOT NULL,
            split_adjusted_close DOUBLE NOT NULL,
            total_return_adjusted_close DOUBLE NOT NULL,
            raw_volume BIGINT,
            split_adjusted_volume DOUBLE,
            visible_event_count INTEGER NOT NULL,
            split_event_count INTEGER NOT NULL,
            cash_div_event_count INTEGER NOT NULL,
            last_factor_ex_date DATE,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_daily_adjustment_factors_security ON daily_adjustment_factors(security_id, trade_date, as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_daily_adjustment_factors_symbol ON daily_adjustment_factors(symbol, trade_date, as_of_date)",
    ):
        conn.execute(statement)


def _delisting_events(conn: duckdb.DuckDBPyConnection) -> None:
    """S5d: public delisting evidence and explicit return-proxy policy fields."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS delist_code_dim (
            delist_code VARCHAR PRIMARY KEY,
            code_system VARCHAR NOT NULL,
            vendor_code VARCHAR,
            crsp_dlstcd INTEGER,
            crsp_dlstcd_family VARCHAR,
            reason_category VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            terminal_trading_status VARCHAR,
            imputation_allowed BOOLEAN NOT NULL DEFAULT false,
            default_imputed_return DOUBLE,
            imputation_policy VARCHAR NOT NULL DEFAULT 'none',
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS delisting_events (
            delisting_event_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            listing_status_source VARCHAR NOT NULL,
            source_listing_status_id VARCHAR NOT NULL,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            listing_venue_code VARCHAR,
            listing_venue_name VARCHAR,
            listing_exchange_code VARCHAR,
            delist_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            delist_code VARCHAR NOT NULL,
            delist_reason VARCHAR NOT NULL,
            delisting_return DOUBLE,
            delisting_return_type VARCHAR NOT NULL,
            is_return_imputed BOOLEAN NOT NULL DEFAULT false,
            return_policy VARCHAR NOT NULL,
            return_confidence VARCHAR NOT NULL,
            evidence_source VARCHAR NOT NULL,
            evidence_source_table VARCHAR NOT NULL,
            source_event_id VARCHAR,
            source_url VARCHAR,
            method VARCHAR NOT NULL,
            evidence_confidence VARCHAR NOT NULL,
            inferred_from_absence BOOLEAN NOT NULL DEFAULT false,
            details_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_delisting_events_security ON delisting_events(security_id, delist_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_events_symbol ON delisting_events(symbol, delist_date, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_events_asof ON delisting_events(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_events_code ON delisting_events(delist_code, delist_date)",
    ):
        conn.execute(statement)


def _delisting_return_observations(conn: duckdb.DuckDBPyConnection) -> None:
    """S5e: injectable observed delisting-return facts and event lineage fields."""

    for statement in (
        "ALTER TABLE delisting_events ADD COLUMN IF NOT EXISTS return_observation_id VARCHAR",
        "ALTER TABLE delisting_events ADD COLUMN IF NOT EXISTS return_observation_source VARCHAR",
        "ALTER TABLE delisting_events ADD COLUMN IF NOT EXISTS return_observation_provider VARCHAR",
    ):
        conn.execute(statement)
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS delisting_return_observations (
            delisting_return_observation_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            provider VARCHAR NOT NULL,
            source_file VARCHAR,
            source_file_sha256 VARCHAR,
            security_id VARCHAR,
            symbol VARCHAR,
            vendor_security_id VARCHAR,
            vendor_security_id_type VARCHAR,
            delist_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            delist_code VARCHAR,
            vendor_delist_code VARCHAR,
            crsp_dlstcd INTEGER,
            delist_amount DOUBLE,
            delist_price DOUBLE,
            delisting_return DOUBLE NOT NULL,
            delisting_return_ex_div DOUBLE,
            delist_pay_date DATE,
            next_pricing_date DATE,
            successor_security_id VARCHAR,
            successor_vendor_security_id VARCHAR,
            return_basis VARCHAR NOT NULL,
            currency VARCHAR,
            raw_payload_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_delisting_return_observations_security ON delisting_return_observations(security_id, delist_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_return_observations_symbol ON delisting_return_observations(symbol, delist_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_delisting_return_observations_vendor ON delisting_return_observations(provider, vendor_security_id_type, vendor_security_id, delist_date)",
    ):
        conn.execute(statement)


def _estimate_detail_panel(conn: duckdb.DuckDBPyConnection) -> None:
    """S5f: vendor-injectable detail estimate panel with PIT lineage dimensions."""

    for statement in (
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS est_detail_id VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS provider VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS symbol VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS vendor_security_id VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS vendor_security_id_type VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS source_vendor_table VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS vendor_broker_id VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS vendor_analyst_id VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS broker_mask_code VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS analyst_mask_code VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS broker_name VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS analyst_name VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS fpi VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS period_type VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS expected_report_date DATE",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS announce_date DATE",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS announce_time TIME",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS activation_date DATE",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS activation_time TIME",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS revision_date DATE",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS revision_time TIME",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS stop_date DATE",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS pdf VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS basis VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS is_gaap BOOLEAN",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS estimate_type VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS currency VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS unit VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS source_file VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS source_file_sha256 VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS raw_payload_json VARCHAR",
        "ALTER TABLE est_detail ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT now()",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS provider VARCHAR",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS vendor_broker_id VARCHAR",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS broker_mask_code VARCHAR",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS valid_from DATE",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS valid_to DATE",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS source_file_sha256 VARCHAR",
        "ALTER TABLE est_broker ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT now()",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS provider VARCHAR",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS vendor_analyst_id VARCHAR",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS analyst_mask_code VARCHAR",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS valid_from DATE",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS valid_to DATE",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS source_file_sha256 VARCHAR",
        "ALTER TABLE est_analyst ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT now()",
    ):
        conn.execute(statement)
    for statement in (
        """
        CREATE TABLE IF NOT EXISTS est_period_dim (
            est_period_id VARCHAR PRIMARY KEY,
            provider VARCHAR,
            measure_code VARCHAR,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            period_end DATE,
            fpi VARCHAR,
            period_type VARCHAR,
            expected_report_date DATE,
            valid_from DATE,
            valid_to DATE,
            as_of_date DATE,
            available_at TIMESTAMP,
            source VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS est_broker_alias (
            broker_alias_id VARCHAR PRIMARY KEY,
            broker_id VARCHAR NOT NULL,
            provider VARCHAR,
            alias_type VARCHAR NOT NULL,
            alias_value VARCHAR NOT NULL,
            valid_from DATE,
            valid_to DATE,
            available_at TIMESTAMP,
            source VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS est_analyst_alias (
            analyst_alias_id VARCHAR PRIMARY KEY,
            analyst_id VARCHAR NOT NULL,
            provider VARCHAR,
            alias_type VARCHAR NOT NULL,
            alias_value VARCHAR NOT NULL,
            valid_from DATE,
            valid_to DATE,
            available_at TIMESTAMP,
            source VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_est_detail_id ON est_detail(est_detail_id)",
        "CREATE INDEX IF NOT EXISTS idx_est_detail_security ON est_detail(security_id, measure_code, period_end, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_detail_symbol ON est_detail(symbol, measure_code, period_end, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_detail_vendor ON est_detail(provider, vendor_security_id_type, vendor_security_id, measure_code, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_est_detail_broker ON est_detail(broker_id, analyst_id, announce_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_period_dim_key ON est_period_dim(provider, measure_code, period_end, fpi)",
        "CREATE INDEX IF NOT EXISTS idx_est_broker_vendor ON est_broker(provider, vendor_broker_id, valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_est_analyst_vendor ON est_analyst(provider, vendor_analyst_id, valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_est_broker_alias_value ON est_broker_alias(provider, alias_type, alias_value, valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_est_analyst_alias_value ON est_analyst_alias(provider, alias_type, alias_value, valid_from)",
    ):
        conn.execute(statement)


def _estimates(conn: duckdb.DuckDBPyConnection) -> None:
    """Create S2 estimates tables: est_measure, est_actual, est_consensus, est_detail,
    est_broker, est_analyst, est_guidance, est_recommendation, est_surprise."""
    for statement in (
        """
        CREATE TABLE IF NOT EXISTS est_measure (
            measure_code VARCHAR PRIMARY KEY,
            label VARCHAR NOT NULL,
            statement VARCHAR,
            unit_type VARCHAR,
            is_per_share BOOLEAN NOT NULL DEFAULT false,
            higher_is_better BOOLEAN NOT NULL DEFAULT true,
            us_gaap_concepts VARCHAR,
            source VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS est_actual (
            security_id VARCHAR NOT NULL,
            measure_code VARCHAR NOT NULL,
            fiscal_year INTEGER NOT NULL,
            fiscal_period VARCHAR NOT NULL,
            period_end DATE NOT NULL,
            value DOUBLE,
            unit VARCHAR,
            form VARCHAR,
            accession_number VARCHAR,
            announce_date DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR NOT NULL,
            PRIMARY KEY (security_id, measure_code, fiscal_year, fiscal_period, accession_number)
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_est_actual_key ON est_actual(security_id, measure_code, period_end)",
        """
        CREATE TABLE IF NOT EXISTS est_consensus (
            security_id VARCHAR,
            measure_code VARCHAR,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            period_end DATE,
            consensus_date DATE,
            mean DOUBLE,
            median DOUBLE,
            high DOUBLE,
            low DOUBLE,
            stdev DOUBLE,
            num_estimates INTEGER,
            num_up INTEGER,
            num_down INTEGER,
            currency VARCHAR,
            as_of_date DATE,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_est_consensus_key ON est_consensus(security_id, measure_code, period_end)",
        """
        CREATE TABLE IF NOT EXISTS est_detail (
            security_id VARCHAR,
            measure_code VARCHAR,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            period_end DATE,
            broker_id VARCHAR,
            analyst_id VARCHAR,
            value DOUBLE,
            estimate_date DATE,
            as_of_date DATE,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS est_broker (
            broker_id VARCHAR PRIMARY KEY,
            broker_name VARCHAR,
            source VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS est_analyst (
            analyst_id VARCHAR PRIMARY KEY,
            analyst_name VARCHAR,
            broker_id VARCHAR,
            source VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS est_guidance (
            est_guidance_id VARCHAR,
            security_id VARCHAR,
            measure_code VARCHAR,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            period_end DATE,
            low DOUBLE,
            high DOUBLE,
            mid DOUBLE,
            guidance_type VARCHAR,
            basis VARCHAR,
            currency VARCHAR,
            unit VARCHAR,
            units_scale BIGINT,
            source_item VARCHAR,
            guidance_date DATE,
            form VARCHAR,
            accession_number VARCHAR,
            as_of_date DATE,
            available_at TIMESTAMP,
            extraction_confidence DOUBLE,
            evidence_text VARCHAR,
            source_file VARCHAR,
            source_file_sha256 VARCHAR,
            raw_payload_json VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_est_guidance_key ON est_guidance(security_id, measure_code, period_end)",
        """
        CREATE TABLE IF NOT EXISTS est_recommendation (
            security_id VARCHAR,
            broker_id VARCHAR,
            analyst_id VARCHAR,
            rating VARCHAR,
            rating_standardized VARCHAR,
            prior_rating VARCHAR,
            action VARCHAR,
            rating_date DATE,
            as_of_date DATE,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS est_surprise (
            security_id VARCHAR,
            measure_code VARCHAR,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            period_end DATE,
            actual DOUBLE,
            expected DOUBLE,
            surprise DOUBLE,
            sue DOUBLE,
            consensus_mean DOUBLE,
            surprise_pct DOUBLE,
            model VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR NOT NULL,
            PRIMARY KEY (security_id, measure_code, fiscal_year, fiscal_period)
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_est_surprise_key ON est_surprise(security_id, measure_code, period_end)",
    ):
        conn.execute(statement)


def _insider_ownership(conn: duckdb.DuckDBPyConnection) -> None:
    """Create S3 insider/public-ownership tables.

    The active loader populates Section 16 ownership XML tables first
    (insider, filing_form4, insider_relationship, insider_transaction,
    insider_holding, tradingplan_10b5_1). The adjacent ownership tables are
    schema landing zones for follow-on 13D/G, N-PORT, Form 144, N-PX, and
    STOCK Act loaders.
    """
    for statement in (
        """
        CREATE TABLE IF NOT EXISTS insider (
            insider_id VARCHAR PRIMARY KEY,
            reporting_owner_cik VARCHAR,
            full_name VARCHAR NOT NULL,
            full_name_norm VARCHAR NOT NULL,
            citizenship VARCHAR,
            resolution_source VARCHAR NOT NULL,
            first_filing_date DATE,
            last_filing_date DATE,
            filing_count BIGINT NOT NULL DEFAULT 0,
            notes_json VARCHAR,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS filing_form4 (
            filing_id VARCHAR PRIMARY KEY,
            accession_number VARCHAR NOT NULL UNIQUE,
            security_id VARCHAR NOT NULL,
            issuer_cik VARCHAR,
            issuer_name VARCHAR,
            issuer_trading_symbol VARCHAR,
            form_type VARCHAR NOT NULL,
            schema_version VARCHAR,
            period_of_report DATE,
            filing_date DATE,
            acceptance_datetime TIMESTAMP,
            available_at TIMESTAMP,
            remarks VARCHAR,
            footnotes_json VARCHAR,
            source_url VARCHAR NOT NULL,
            raw_document_sha256 VARCHAR,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS insider_relationship (
            relationship_id VARCHAR PRIMARY KEY,
            accession_number VARCHAR NOT NULL,
            insider_id VARCHAR NOT NULL,
            reporting_owner_cik VARCHAR,
            full_name VARCHAR NOT NULL,
            full_name_norm VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            issuer_cik VARCHAR,
            issuer_name VARCHAR,
            issuer_trading_symbol VARCHAR,
            is_director BOOLEAN NOT NULL DEFAULT false,
            is_officer BOOLEAN NOT NULL DEFAULT false,
            is_ten_percent_owner BOOLEAN NOT NULL DEFAULT false,
            is_other BOOLEAN NOT NULL DEFAULT false,
            officer_title_raw VARCHAR,
            officer_title_norm VARCHAR,
            other_text VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS insider_transaction (
            transaction_id VARCHAR PRIMARY KEY,
            filing_id VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            transaction_ordinal BIGINT NOT NULL,
            transaction_form_type VARCHAR,
            insider_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            issuer_cik VARCHAR,
            issuer_name VARCHAR,
            issuer_trading_symbol VARCHAR,
            security_title VARCHAR,
            transaction_date DATE,
            deemed_execution_date DATE,
            is_derivative BOOLEAN NOT NULL DEFAULT false,
            transaction_code VARCHAR,
            acquired_disposed VARCHAR,
            transaction_shares DOUBLE,
            transaction_price DOUBLE,
            shares_owned_following DOUBLE,
            direct_indirect VARCHAR,
            nature_of_ownership VARCHAR,
            rule_10b5_1_indicator BOOLEAN,
            plan_10b5_1_adoption_date DATE,
            plan_10b5_1_id VARCHAR,
            equity_swap_involved BOOLEAN NOT NULL DEFAULT false,
            underlying_security_title VARCHAR,
            underlying_shares DOUBLE,
            conversion_or_exercise_price DOUBLE,
            exercise_date DATE,
            expiration_date DATE,
            footnote_ids_json VARCHAR,
            reported_early BOOLEAN NOT NULL DEFAULT false,
            as_of_date DATE,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS insider_holding (
            holding_id VARCHAR PRIMARY KEY,
            filing_id VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            holding_ordinal BIGINT NOT NULL,
            insider_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            issuer_cik VARCHAR,
            issuer_name VARCHAR,
            issuer_trading_symbol VARCHAR,
            security_title VARCHAR,
            is_derivative BOOLEAN NOT NULL DEFAULT false,
            shares_owned_following DOUBLE,
            direct_indirect VARCHAR,
            nature_of_ownership VARCHAR,
            underlying_security_title VARCHAR,
            underlying_shares DOUBLE,
            conversion_or_exercise_price DOUBLE,
            exercise_date DATE,
            expiration_date DATE,
            as_of_date DATE,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS tradingplan_10b5_1 (
            plan_id VARCHAR PRIMARY KEY,
            insider_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            issuer_cik VARCHAR,
            adoption_date DATE NOT NULL,
            first_transaction_date DATE,
            last_transaction_date DATE,
            transaction_count BIGINT NOT NULL,
            cooling_off_days BIGINT,
            cooling_off_compliant BOOLEAN,
            source_filing_ids_json VARCHAR NOT NULL,
            source_transaction_ids_json VARCHAR NOT NULL,
            as_of_date DATE,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS blockholder_filing (
            filing_id VARCHAR PRIMARY KEY,
            accession_number VARCHAR NOT NULL UNIQUE,
            schedule_type VARCHAR NOT NULL,
            amendment_seq INTEGER,
            amends_filing_id VARCHAR,
            is_group_filing BOOLEAN NOT NULL DEFAULT false,
            security_id VARCHAR,
            issuer_cik VARCHAR,
            issuer_name VARCHAR,
            cusip VARCHAR,
            event_date DATE,
            filing_date DATE,
            filing_lag_business_days INTEGER,
            is_xml_filing BOOLEAN NOT NULL DEFAULT false,
            purpose_text VARCHAR,
            available_at TIMESTAMP,
            source_url VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS blockholder_reporting_person (
            reporting_person_id VARCHAR PRIMARY KEY,
            filing_id VARCHAR NOT NULL,
            reporting_person_seq INTEGER NOT NULL,
            insider_id VARCHAR,
            entity_id VARCHAR,
            reporting_person_name VARCHAR NOT NULL,
            type_of_reporting_person VARCHAR,
            citizenship_or_place_of_org VARCHAR,
            source_of_funds VARCHAR,
            sole_voting_power DOUBLE,
            shared_voting_power DOUBLE,
            sole_dispositive_power DOUBLE,
            shared_dispositive_power DOUBLE,
            aggregate_beneficially_owned DOUBLE,
            percent_of_class DOUBLE,
            excludes_certain_shares BOOLEAN NOT NULL DEFAULT false,
            legal_proceedings_flag BOOLEAN NOT NULL DEFAULT false,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS fund (
            fund_id VARCHAR PRIMARY KEY,
            registrant_cik VARCHAR NOT NULL,
            series_id VARCHAR NOT NULL,
            fund_name VARCHAR NOT NULL,
            fund_family_entity_id VARCHAR,
            adviser_cik VARCHAR,
            inception_date DATE,
            termination_date DATE,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS fund_class (
            class_id VARCHAR PRIMARY KEY,
            fund_id VARCHAR NOT NULL,
            class_external_id VARCHAR NOT NULL,
            class_name VARCHAR NOT NULL,
            ticker VARCHAR,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS filing_nport (
            filing_id VARCHAR PRIMARY KEY,
            accession_number VARCHAR NOT NULL UNIQUE,
            fund_id VARCHAR NOT NULL,
            form_type VARCHAR NOT NULL,
            period_of_report DATE NOT NULL,
            is_public BOOLEAN NOT NULL DEFAULT false,
            filing_date DATE,
            available_at TIMESTAMP,
            source_url VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS fund_holding (
            holding_id VARCHAR PRIMARY KEY,
            filing_id VARCHAR NOT NULL,
            fund_id VARCHAR NOT NULL,
            period_of_report DATE NOT NULL,
            security_id VARCHAR,
            issuer_name_raw VARCHAR,
            issuer_lei VARCHAR,
            title_of_issue VARCHAR,
            units VARCHAR,
            balance DOUBLE,
            value_usd DOUBLE,
            pct_of_net_assets DOUBLE,
            payoff_profile VARCHAR,
            asset_cat VARCHAR,
            issuer_cat VARCHAR,
            investment_country VARCHAR,
            is_restricted_security BOOLEAN NOT NULL DEFAULT false,
            fair_value_level INTEGER,
            derivative_json VARCHAR,
            debt_security_json VARCHAR,
            repo_json VARCHAR,
            is_on_loan BOOLEAN NOT NULL DEFAULT false,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS form144_intent (
            filing_id VARCHAR PRIMARY KEY,
            accession_number VARCHAR NOT NULL UNIQUE,
            seller_name VARCHAR NOT NULL,
            insider_id VARCHAR,
            security_id VARCHAR,
            issuer_cik VARCHAR,
            issuer_name VARCHAR,
            filing_date DATE,
            approx_sale_date DATE,
            shares_proposed DOUBLE,
            aggregate_market_value DOUBLE,
            brokers_json VARCHAR,
            acquisition_date DATE,
            acquisition_nature VARCHAR,
            past_3mo_sales_json VARCHAR,
            available_at TIMESTAMP,
            source_url VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS form144_to_form4_link (
            form144_filing_id VARCHAR NOT NULL,
            insider_transaction_id VARCHAR NOT NULL,
            match_confidence DOUBLE NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (form144_filing_id, insider_transaction_id)
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS proxy_vote (
            proxy_vote_id VARCHAR PRIMARY KEY,
            filing_id VARCHAR NOT NULL,
            fund_id VARCHAR,
            manager_filer_id VARCHAR,
            security_id VARCHAR,
            issuer_cik VARCHAR,
            meeting_date DATE,
            matter_description VARCHAR NOT NULL,
            vote_category VARCHAR NOT NULL,
            vote_subcategory VARCHAR,
            is_shareholder_proposed BOOLEAN NOT NULL DEFAULT false,
            management_recommendation VARCHAR,
            vote_cast VARCHAR NOT NULL,
            shares_voted DOUBLE,
            shares_on_loan_at_record_date DOUBLE,
            loan_recalled_to_vote BOOLEAN,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        """
        CREATE TABLE IF NOT EXISTS congressional_disclosure (
            disclosure_id VARCHAR PRIMARY KEY,
            chamber VARCHAR NOT NULL,
            member_id VARCHAR,
            member_name VARCHAR NOT NULL,
            filing_type VARCHAR NOT NULL,
            filing_date DATE NOT NULL,
            transaction_date DATE,
            disclosure_lag_days INTEGER,
            asset_name VARCHAR,
            asset_security_id VARCHAR,
            transaction_type VARCHAR,
            amount_range_low DOUBLE,
            amount_range_high DOUBLE,
            source_url VARCHAR NOT NULL,
            source_normalizer VARCHAR NOT NULL,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """,
        "CREATE INDEX IF NOT EXISTS idx_insider_cik ON insider(reporting_owner_cik)",
        "CREATE INDEX IF NOT EXISTS idx_insider_name_norm ON insider(full_name_norm)",
        "CREATE INDEX IF NOT EXISTS idx_filing_form4_issuer ON filing_form4(issuer_cik, period_of_report)",
        "CREATE INDEX IF NOT EXISTS idx_filing_form4_accession ON filing_form4(accession_number)",
        "CREATE INDEX IF NOT EXISTS idx_insider_relationship_insider ON insider_relationship(insider_id, security_id, valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_insider_relationship_security ON insider_relationship(security_id, valid_from, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_insider_transaction_insider_date ON insider_transaction(insider_id, transaction_date)",
        "CREATE INDEX IF NOT EXISTS idx_insider_transaction_security_date ON insider_transaction(security_id, transaction_date)",
        "CREATE INDEX IF NOT EXISTS idx_insider_transaction_code ON insider_transaction(transaction_code)",
        "CREATE INDEX IF NOT EXISTS idx_insider_transaction_asof ON insider_transaction(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_insider_holding_insider_date ON insider_holding(insider_id, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_tradingplan_10b5_1_insider ON tradingplan_10b5_1(insider_id, security_id, adoption_date)",
        "CREATE INDEX IF NOT EXISTS idx_blockholder_filing_security ON blockholder_filing(security_id, filing_date)",
        "CREATE INDEX IF NOT EXISTS idx_blockholder_reporting_person_insider ON blockholder_reporting_person(insider_id, filing_id)",
        "CREATE INDEX IF NOT EXISTS idx_fund_holding_fund_period ON fund_holding(fund_id, period_of_report)",
        "CREATE INDEX IF NOT EXISTS idx_fund_holding_security_period ON fund_holding(security_id, period_of_report)",
        "CREATE INDEX IF NOT EXISTS idx_form144_intent_security_date ON form144_intent(security_id, approx_sale_date)",
        "CREATE INDEX IF NOT EXISTS idx_proxy_vote_security_date ON proxy_vote(security_id, meeting_date)",
        "CREATE INDEX IF NOT EXISTS idx_congressional_disclosure_security_date ON congressional_disclosure(asset_security_id, transaction_date)",
    ):
        conn.execute(statement)


def _estimate_consensus_snapshots(conn: duckdb.DuckDBPyConnection) -> None:
    """S5g: vendor-injectable consensus snapshots with stable ids and PIT lineage."""

    conn.execute("DROP INDEX IF EXISTS idx_est_consensus_key")
    for statement in (
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS est_consensus_id VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS provider VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS symbol VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS vendor_security_id VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS vendor_security_id_type VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS source_vendor_table VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS fpi VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS period_type VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS expected_report_date DATE",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS pdf VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS basis VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS is_gaap BOOLEAN",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS unit VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS stale_after_date DATE",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS source_file VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS source_file_sha256 VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS raw_payload_json VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT now()",
    ):
        conn.execute(statement)


def _estimate_consensus_snapshot_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """S5g: index consensus snapshots after the additive schema migration commits."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_est_consensus_key ON est_consensus(security_id, measure_code, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_est_consensus_id ON est_consensus(est_consensus_id)",
        "CREATE INDEX IF NOT EXISTS idx_est_consensus_pit ON est_consensus(security_id, measure_code, period_end, consensus_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_consensus_symbol ON est_consensus(symbol, measure_code, period_end, consensus_date)",
        "CREATE INDEX IF NOT EXISTS idx_est_consensus_vendor ON est_consensus(provider, vendor_security_id_type, vendor_security_id, measure_code, period_end)",
    ):
        conn.execute(statement)


def _estimate_recommendation_events(conn: duckdb.DuckDBPyConnection) -> None:
    """S5h: vendor-injectable recommendation and price-target events."""

    for statement in (
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS est_recommendation_id VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS provider VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS symbol VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS vendor_security_id VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS vendor_security_id_type VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS cusip VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS source_vendor_table VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS vendor_broker_id VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS vendor_analyst_id VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS broker_mask_code VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS analyst_mask_code VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS broker_name VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS analyst_name VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS recommendation_code INTEGER",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS recommendation_label VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS prior_recommendation_code INTEGER",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS prior_recommendation_label VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS rating_scale VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS event_type VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS price_target DOUBLE",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS target_currency VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS target_horizon_months INTEGER",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS industry_code VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS is_industry_recommendation BOOLEAN",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS usfirm VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS announce_date DATE",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS announce_time TIME",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS activation_date DATE",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS activation_time TIME",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS revision_date DATE",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS revision_time TIME",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS stop_date DATE",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS source_file VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS source_file_sha256 VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS raw_payload_json VARCHAR",
        "ALTER TABLE est_recommendation ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT now()",
    ):
        conn.execute(statement)


def _estimate_recommendation_event_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """S5h: index recommendation and price-target events after schema migration."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_id ON est_recommendation(est_recommendation_id)",
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_security ON est_recommendation(security_id, event_type, rating_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_symbol ON est_recommendation(symbol, event_type, rating_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_vendor ON est_recommendation(provider, vendor_security_id_type, vendor_security_id, event_type, rating_date)",
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_broker ON est_recommendation(broker_id, analyst_id, rating_date, available_at)",
    ):
        conn.execute(statement)


def _estimate_recommendation_catalog_fields(conn: duckdb.DuckDBPyConnection) -> None:
    """S5h: catalog additive recommendation columns for already-migrated databases."""

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name,
            field_name,
            semantic_type,
            description,
            nullable,
            unit,
            source_field,
            updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN (
                    'security_id', 'source_id', 'run_id', 'broker_id', 'analyst_id',
                    'est_recommendation_id', 'symbol', 'vendor_security_id', 'cusip',
                    'vendor_broker_id', 'vendor_analyst_id', 'broker_mask_code',
                    'analyst_mask_code', 'source_file_sha256'
                ) THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%_date' OR upper(c.data_type) = 'DATE' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN upper(c.data_type) = 'TIME' THEN 'time'
                WHEN upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN lower(c.column_name) LIKE '%price%' THEN 'price'
                WHEN lower(c.column_name) LIKE '%value%'
                  OR lower(c.column_name) LIKE '%amount%'
                  OR lower(c.column_name) LIKE '%factor%'
                  OR lower(c.column_name) LIKE '%return%'
                  OR lower(c.column_name) LIKE '%percent%'
                  OR lower(c.column_name) LIKE '%rate%'
                  OR lower(c.column_name) LIKE '%weight%'
                  OR lower(c.column_name) LIKE '%ratio%'
                  OR upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL')
                THEN 'measure'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'est_recommendation_id' THEN 'Deterministic recommendation or price-target event identifier.'
                WHEN 'provider' THEN 'Provider namespace for the injected recommendation feed.'
                WHEN 'vendor_security_id' THEN 'Provider security identifier, such as an IBES ticker.'
                WHEN 'vendor_security_id_type' THEN 'Identifier type for vendor_security_id.'
                WHEN 'source_vendor_table' THEN 'Original vendor table family, such as recddet or ptgdet.'
                WHEN 'vendor_broker_id' THEN 'Broker identifier as published by the source file.'
                WHEN 'vendor_analyst_id' THEN 'Analyst identifier as published by the source file.'
                WHEN 'broker_mask_code' THEN 'Masked or alternate broker code from the vendor file.'
                WHEN 'analyst_mask_code' THEN 'Masked or alternate analyst code from the vendor file.'
                WHEN 'broker_name' THEN 'Broker or estimator display name from the source row.'
                WHEN 'analyst_name' THEN 'Analyst display name from the source row.'
                WHEN 'recommendation_code' THEN 'Canonical recommendation code where lower values are more bullish.'
                WHEN 'recommendation_label' THEN 'Canonical recommendation label.'
                WHEN 'prior_recommendation_code' THEN 'Prior canonical recommendation code when supplied or derived.'
                WHEN 'prior_recommendation_label' THEN 'Prior canonical recommendation label.'
                WHEN 'rating_scale' THEN 'Rating scale namespace used to interpret recommendation_code.'
                WHEN 'event_type' THEN 'Normalized recommendation event type.'
                WHEN 'price_target' THEN 'Broker price target value.'
                WHEN 'target_currency' THEN 'Currency for price_target.'
                WHEN 'target_horizon_months' THEN 'Target horizon in months.'
                WHEN 'industry_code' THEN 'Industry recommendation code when the row is not issuer-specific.'
                WHEN 'is_industry_recommendation' THEN 'True when the recommendation applies to an industry row.'
                WHEN 'usfirm' THEN 'Vendor flag for US-firm rows.'
                WHEN 'announce_date' THEN 'Date the recommendation or target was announced.'
                WHEN 'announce_time' THEN 'Time the recommendation or target was announced.'
                WHEN 'activation_date' THEN 'Date the vendor feed made the recommendation active.'
                WHEN 'activation_time' THEN 'Time the vendor feed made the recommendation active.'
                WHEN 'revision_date' THEN 'Date through which the recommendation remains active or was revised.'
                WHEN 'revision_time' THEN 'Revision or stop-window time from the vendor feed.'
                WHEN 'stop_date' THEN 'Explicit stop date when supplied by the provider.'
                WHEN 'source_file' THEN 'Path of the injected source file used for this row.'
                WHEN 'source_file_sha256' THEN 'SHA-256 hash of the injected source file.'
                WHEN 'raw_payload_json' THEN 'Normalized raw source payload for lineage and audit.'
                WHEN 'updated_at' THEN 'Warehouse update timestamp for this row.'
                ELSE replace(c.column_name, '_', ' ') || ' field on est_recommendation.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN c.column_name = 'price_target' THEN 'target_currency'
                WHEN c.column_name = 'target_horizon_months' THEN 'months'
                WHEN lower(c.column_name) LIKE '%_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'est_recommendation'
        """
    )


def _estimate_recommendation_catalog_semantics(conn: duckdb.DuckDBPyConnection) -> None:
    """S5h: tighten identifier semantics for recommendation catalog fields."""

    conn.execute(
        """
        UPDATE field_catalog
        SET semantic_type = 'identifier',
            updated_at = now()
        WHERE table_name = 'est_recommendation'
          AND field_name IN ('symbol', 'source_file_sha256')
        """
    )


def _estimate_recommendation_summary_snapshots(conn: duckdb.DuckDBPyConnection) -> None:
    """S5i: aggregate recommendation and price-target snapshot table."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS est_recommendation_summary (
            est_recommendation_summary_id VARCHAR PRIMARY KEY,
            security_id VARCHAR,
            symbol VARCHAR,
            vendor_security_id VARCHAR,
            vendor_security_id_type VARCHAR,
            cusip VARCHAR,
            provider VARCHAR NOT NULL,
            source_vendor_table VARCHAR,
            snapshot_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            mean_recommendation DOUBLE,
            median_recommendation DOUBLE,
            rating_scale VARCHAR,
            scale_direction VARCHAR,
            strong_buy_count INTEGER,
            buy_count INTEGER,
            hold_count INTEGER,
            underperform_count INTEGER,
            sell_count INTEGER,
            buy_equivalent_count INTEGER,
            sell_equivalent_count INTEGER,
            total_recommendations INTEGER,
            mean_price_target DOUBLE,
            median_price_target DOUBLE,
            high_price_target DOUBLE,
            low_price_target DOUBLE,
            price_target_count INTEGER,
            target_currency VARCHAR,
            target_horizon_months INTEGER,
            provider_scale_notes VARCHAR,
            source_file VARCHAR,
            source_file_sha256 VARCHAR,
            raw_payload_json VARCHAR,
            run_id VARCHAR,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )


def _estimate_recommendation_summary_indexes_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """S5i: index and catalog aggregate recommendation snapshots."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_summary_id ON est_recommendation_summary(est_recommendation_summary_id)",
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_summary_security ON est_recommendation_summary(security_id, snapshot_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_summary_symbol ON est_recommendation_summary(symbol, snapshot_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_recommendation_summary_vendor ON est_recommendation_summary(provider, vendor_security_id_type, vendor_security_id, source_vendor_table, snapshot_date)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,
            source_system_id,
            name,
            description,
            grain,
            primary_table,
            pit_column,
            available_at_column,
            updated_at
        )
        VALUES (
            'est_recommendation_summary',
            'injected_estimates',
            'Recommendation and price-target summary snapshots',
            'Injectable aggregate recommendation distributions and monthly price-target summaries from IBES recdsum/ptgsum-style or normalized provider files.',
            'security_or_vendor_id,snapshot_date,source_vendor_table',
            'est_recommendation_summary',
            'snapshot_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,
            layer,
            entity,
            grain,
            description,
            natural_key_json,
            pit_notes,
            updated_at
        )
        VALUES (
            'est_recommendation_summary',
            'silver',
            'estimate_recommendation_summary',
            'security_or_vendor_id,provider,source_vendor_table,snapshot_date',
            'Aggregate recommendation distributions and price-target summary snapshots with canonical IBES-direction rating means.',
            '["est_recommendation_summary_id"]',
            'Use available_at and snapshot_date for PIT visibility; scale_direction records provider numeric rating polarity before canonical conversion.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name,
            field_name,
            semantic_type,
            description,
            nullable,
            unit,
            source_field,
            updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN (
                    'est_recommendation_summary_id', 'security_id', 'symbol',
                    'vendor_security_id', 'cusip', 'source_file_sha256', 'run_id'
                ) THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%_date' OR upper(c.data_type) = 'DATE' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN lower(c.column_name) LIKE '%price%' OR lower(c.column_name) LIKE '%target%' THEN 'price'
                WHEN lower(c.column_name) LIKE '%count%'
                  OR lower(c.column_name) LIKE '%mean%'
                  OR lower(c.column_name) LIKE '%median%'
                  OR lower(c.column_name) LIKE '%high%'
                  OR lower(c.column_name) LIKE '%low%'
                  OR upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL')
                THEN 'measure'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'est_recommendation_summary_id' THEN 'Deterministic recommendation-summary snapshot identifier.'
                WHEN 'snapshot_date' THEN 'Provider summary snapshot date, such as IBES statpers/month-end.'
                WHEN 'available_at' THEN 'Timestamp when the summary snapshot is treated as visible for PIT research.'
                WHEN 'mean_recommendation' THEN 'Canonical mean recommendation on the IBES 1=Strong Buy to 5=Sell direction.'
                WHEN 'scale_direction' THEN 'Original provider numeric rating polarity before canonical conversion.'
                WHEN 'buy_equivalent_count' THEN 'Aggregate bullish recommendation count when source combines Strong Buy and Buy.'
                WHEN 'sell_equivalent_count' THEN 'Aggregate bearish recommendation count when source combines Underperform and Sell.'
                WHEN 'mean_price_target' THEN 'Mean provider price target for the snapshot.'
                WHEN 'price_target_count' THEN 'Number of price-target observations in the aggregate snapshot.'
                ELSE replace(c.column_name, '_', ' ') || ' field on est_recommendation_summary.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN c.column_name IN ('mean_price_target', 'median_price_target', 'high_price_target', 'low_price_target') THEN 'target_currency'
                WHEN c.column_name = 'target_horizon_months' THEN 'months'
                WHEN lower(c.column_name) LIKE '%_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'est_recommendation_summary'
        """
    )


def _estimate_security_link(conn: duckdb.DuckDBPyConnection) -> None:
    """S5j: PIT-safe estimate vendor identifier to warehouse security links."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS est_security_link (
            est_security_link_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            provider VARCHAR NOT NULL,
            vendor_security_id_type VARCHAR NOT NULL,
            vendor_security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cusip VARCHAR,
            target_security_id VARCHAR NOT NULL,
            target_id_type VARCHAR,
            target_id_value VARCHAR,
            link_method VARCHAR NOT NULL,
            link_status VARCHAR NOT NULL,
            confidence DOUBLE NOT NULL,
            decision_id VARCHAR,
            candidate_id VARCHAR,
            source_dataset_id VARCHAR,
            evidence_table VARCHAR,
            evidence_source VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            first_observed_date DATE NOT NULL,
            last_observed_date DATE NOT NULL,
            first_observed_available_at TIMESTAMP NOT NULL,
            observed_row_count BIGINT NOT NULL,
            observed_tables_json VARCHAR NOT NULL,
            details_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )


def _estimate_security_link_indexes_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """S5j: index and catalog estimate security identifier links."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_est_security_link_vendor ON est_security_link(provider, vendor_security_id_type, vendor_security_id, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_security_link_target ON est_security_link(target_security_id, valid_from, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_security_link_status ON est_security_link(link_status, confidence)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,
            source_system_id,
            name,
            description,
            grain,
            primary_table,
            pit_column,
            available_at_column,
            updated_at
        )
        VALUES (
            'est_security_link',
            'atx_warehouse',
            'Estimate security identifier links',
            'PIT-safe links from estimate vendor identifiers such as IBES tickers, FactSet fsym IDs, CIQ trading items, and CUSIPs to warehouse security_id values.',
            'provider,vendor_security_id_type,vendor_security_id,target_security_id',
            'est_security_link',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,
            layer,
            entity,
            grain,
            description,
            natural_key_json,
            pit_notes,
            updated_at
        )
        VALUES (
            'est_security_link',
            'core',
            'estimate_security_identifier',
            'provider,vendor_security_id_type,vendor_security_id,target_security_id',
            'Auditable PIT mapping from estimate feed security identifiers to warehouse security_id values.',
            '["est_security_link_id"]',
            'Use available_at and valid_from/valid_to before resolving vendor-keyed estimate rows; mappings accepted after a research date must not leak into earlier as-of queries.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name,
            field_name,
            semantic_type,
            description,
            nullable,
            unit,
            source_field,
            updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN (
                    'est_security_link_id', 'security_id', 'target_security_id',
                    'vendor_security_id', 'symbol', 'cusip', 'decision_id',
                    'candidate_id', 'source_dataset_id', 'run_id'
                ) THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%_date' OR upper(c.data_type) = 'DATE' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN lower(c.column_name) LIKE '%confidence%'
                  OR lower(c.column_name) LIKE '%count%'
                  OR upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL')
                THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'est_security_link_id' THEN 'Deterministic estimate security-link identifier.'
                WHEN 'provider' THEN 'Estimate provider namespace, such as IBES, FACTSET, BLOOMBERG, or CIQ.'
                WHEN 'vendor_security_id_type' THEN 'Provider identifier type, such as IBES_TICKER, FACTSET_FSYM_ID, CIQ_TRADING_ITEM_ID, or CUSIP.'
                WHEN 'vendor_security_id' THEN 'Provider security identifier as reported in the estimate feed.'
                WHEN 'target_security_id' THEN 'Warehouse security_id selected by the link evidence.'
                WHEN 'link_method' THEN 'Resolution method used to derive the link.'
                WHEN 'link_status' THEN 'accepted or conflict; only accepted links should be used by as-of estimate APIs.'
                WHEN 'available_at' THEN 'Timestamp when the mapping itself became usable for PIT research.'
                ELSE replace(c.column_name, '_', ' ') || ' field on est_security_link.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) LIKE '%_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'est_security_link'
        """
    )


def _estimate_guidance_extraction_columns_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """S5k: SEC 8-K guidance extraction lineage columns and catalog refresh."""

    for statement in (
        "DROP INDEX IF EXISTS idx_est_guidance_key",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS est_guidance_id VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS guidance_type VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS currency VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS unit VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS units_scale BIGINT",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS source_item VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS extraction_confidence DOUBLE",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS evidence_text VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS source_file VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS source_file_sha256 VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS raw_payload_json VARCHAR",
        "ALTER TABLE est_guidance ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP DEFAULT now()",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,
            source_system_id,
            name,
            description,
            grain,
            primary_table,
            pit_column,
            available_at_column,
            updated_at
        )
        VALUES (
            'est_guidance',
            'sec_edgar',
            'Management guidance observations',
            'Company-issued guidance from local SEC 8-K Item 2.02/7.01 text extraction or licensed/injectable guidance feeds.',
            'security_id,measure,period_end,guidance_date,source',
            'est_guidance',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,
            layer,
            entity,
            grain,
            description,
            natural_key_json,
            pit_notes,
            updated_at
        )
        VALUES (
            'est_guidance',
            'silver',
            'estimate_guidance',
            'security_id,measure_code,period_end,guidance_date,source',
            'Management guidance observations from SEC 8-K text extraction and injectable providers.',
            '["est_guidance_id"]',
            'Use available_at and as_of_date to model when extracted or vendor guidance became visible; extraction_confidence and evidence_text must be retained for public-data QA.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name,
            field_name,
            semantic_type,
            description,
            nullable,
            unit,
            source_field,
            updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN (
                    'est_guidance_id', 'security_id', 'accession_number',
                    'source_file_sha256', 'run_id'
                ) THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%_date' OR upper(c.data_type) = 'DATE' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('low', 'high', 'mid', 'extraction_confidence', 'units_scale')
                  OR upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL')
                THEN 'measure'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'est_guidance_id' THEN 'Deterministic management-guidance observation identifier.'
                WHEN 'guidance_type' THEN 'POINT, RANGE, OPEN_LOW, OPEN_HIGH, or QUAL guidance shape.'
                WHEN 'source_item' THEN 'Public guidance source such as 8-K_2.02 or 8-K_7.01.'
                WHEN 'extraction_confidence' THEN 'Parser confidence for public SEC text-extracted guidance, from 0 to 1.'
                WHEN 'evidence_text' THEN 'Short source text span supporting the extracted guidance row.'
                WHEN 'units_scale' THEN 'Scale factor applied to low/high/mid values, such as 1000000 for millions.'
                WHEN 'source_file_sha256' THEN 'SHA-256 of the local SEC guidance corpus or injected source file.'
                ELSE replace(c.column_name, '_', ' ') || ' field on est_guidance.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN c.column_name IN ('low', 'high', 'mid') THEN 'unit * units_scale'
                WHEN c.column_name = 'extraction_confidence' THEN '0..1'
                WHEN lower(c.column_name) LIKE '%_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'est_guidance'
        """
    )


def _estimate_guidance_extraction_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """S5k: indexes for SEC 8-K guidance extraction columns."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_est_guidance_key ON est_guidance(security_id, measure_code, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_est_guidance_id ON est_guidance(est_guidance_id)",
        "CREATE INDEX IF NOT EXISTS idx_est_guidance_accession ON est_guidance(accession_number, source_item, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_est_guidance_source_file ON est_guidance(source, source_file_sha256)",
    ):
        conn.execute(statement)


def _filer_13f_cik_alias(conn: duckdb.DuckDBPyConnection) -> None:
    """S6a: 13F filer entity-resolution (CIK alias / subadvisor / M&A-continuity).

    Bitemporal alias spine resolving each 13F filer CIK to a canonical primary CIK.
    The auto-derived layer is high precision (SELF identity + intra-CIK NAME_HISTORY),
    cross-CIK same-name links are emitted only as low-confidence NAME_MATCH_CANDIDATE
    rows, and authoritative subadvisor / M&A rollups arrive via an injectable seed.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS filer_13f_cik_alias (
            alias_id VARCHAR PRIMARY KEY,
            primary_cik VARCHAR NOT NULL,
            alias_cik VARCHAR NOT NULL,
            alias_type VARCHAR NOT NULL,
            manager_id VARCHAR,
            normalized_name VARCHAR,
            raw_name VARCHAR,
            cluster_key VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE,
            is_current BOOLEAN NOT NULL DEFAULT true,
            confidence DOUBLE NOT NULL DEFAULT 1.0,
            evidence VARCHAR,
            source VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_filer_alias_alias_cik ON filer_13f_cik_alias(alias_cik, alias_type, valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_filer_alias_primary_cik ON filer_13f_cik_alias(primary_cik, alias_type)",
        "CREATE INDEX IF NOT EXISTS idx_filer_alias_asof ON filer_13f_cik_alias(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_filer_alias_cluster ON filer_13f_cik_alias(cluster_key)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id,
            source_system_id,
            name,
            description,
            grain,
            primary_table,
            pit_column,
            available_at_column,
            updated_at
        )
        VALUES (
            'filer_13f_cik_alias',
            'sec_edgar',
            '13F filer entity-resolution aliases',
            'Bitemporal CIK alias / subadvisor / M&A-continuity rollup for 13F filing managers, derived from thirteenf_managers plus an injectable curated continuity seed.',
            'alias_cik,alias_type,valid_from',
            'filer_13f_cik_alias',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,
            layer,
            entity,
            grain,
            description,
            natural_key_json,
            pit_notes,
            updated_at
        )
        VALUES (
            'filer_13f_cik_alias',
            'silver',
            'filer_13f_entity_resolution',
            'alias_cik,alias_type,valid_from',
            '13F filer CIK alias / subadvisor / M&A-continuity entity-resolution spine for institutional-ownership rollups.',
            '["alias_id"]',
            'Resolve a filer CIK to its primary CIK only with rows whose valid_from/valid_to window covers the query date and whose available_at is visible; default resolution uses confidence>=1.0 so low-confidence NAME_MATCH_CANDIDATE links never silently merge unrelated managers.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name,
            field_name,
            semantic_type,
            description,
            nullable,
            unit,
            source_field,
            updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN (
                    'alias_id', 'primary_cik', 'alias_cik', 'manager_id', 'run_id'
                ) THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%_date' OR upper(c.data_type) = 'DATE' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) = 'confidence' OR upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                WHEN lower(c.column_name) = 'is_current' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'alias_id' THEN 'Deterministic 13F filer-alias row identifier.'
                WHEN 'primary_cik' THEN 'Canonical rollup CIK this alias resolves to.'
                WHEN 'alias_cik' THEN 'Filing-manager CIK being resolved.'
                WHEN 'alias_type' THEN 'SELF, NAME_HISTORY, NAME_MATCH_CANDIDATE, SUBADVISOR, MA_CONTINUITY, or MANUAL.'
                WHEN 'normalized_name' THEN 'Legal-form-stripped uppercase manager name used for clustering.'
                WHEN 'cluster_key' THEN 'Normalized-name cluster key for cross-CIK same-name candidates.'
                WHEN 'confidence' THEN 'Resolution confidence from 0 to 1; authoritative rollups are 1.0, same-name candidates 0.5.'
                WHEN 'is_current' THEN 'True when this alias window is the latest for the alias_cik.'
                WHEN 'evidence' THEN 'Short provenance note for the alias row.'
                ELSE replace(c.column_name, '_', ' ') || ' field on filer_13f_cik_alias.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN c.column_name = 'confidence' THEN '0..1'
                WHEN lower(c.column_name) LIKE '%_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'filer_13f_cik_alias'
        """
    )


def _offexchange_transparency(conn: duckdb.DuckDBPyConnection) -> None:
    """S7a: FINRA OTC Transparency off-exchange ATS / non-ATS volume surfaces.

    Three PIT tables plus a security-joined view:
      * offexchange_venue            -- MPID venue dimension (ATS vs non-ATS)
      * offexchange_volume           -- per (security, venue, period) volume fact
      * offexchange_security_period  -- derived per (security, period) ATS-share rollup
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS offexchange_venue (
            mpid VARCHAR PRIMARY KEY,
            venue_name VARCHAR,
            venue_class VARCHAR NOT NULL,
            firm_crd VARCHAR,
            first_seen_date DATE,
            last_seen_date DATE,
            source VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS offexchange_volume (
            volume_id VARCHAR PRIMARY KEY,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            mpid VARCHAR,
            venue_class VARCHAR NOT NULL,
            summary_type_code VARCHAR,
            period_type VARCHAR NOT NULL,
            tier VARCHAR,
            summary_start_date DATE NOT NULL,
            summary_end_date DATE,
            total_share_quantity DOUBLE,
            total_trade_count BIGINT,
            finra_last_update_date DATE,
            restatement_seq INTEGER NOT NULL DEFAULT 0,
            is_latest BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            source VARCHAR NOT NULL,
            source_file VARCHAR,
            source_file_sha256 VARCHAR,
            raw_payload_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS offexchange_security_period (
            security_period_id VARCHAR PRIMARY KEY,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            period_type VARCHAR NOT NULL,
            summary_start_date DATE NOT NULL,
            summary_end_date DATE,
            ats_share_quantity DOUBLE,
            non_ats_share_quantity DOUBLE,
            total_share_quantity DOUBLE,
            ats_share_pct DOUBLE,
            ats_venue_count INTEGER,
            restatement_detected BOOLEAN NOT NULL DEFAULT false,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_offexchange_volume_key ON offexchange_volume(symbol, period_type, summary_start_date, venue_class)",
        "CREATE INDEX IF NOT EXISTS idx_offexchange_volume_security ON offexchange_volume(security_id, summary_start_date)",
        "CREATE INDEX IF NOT EXISTS idx_offexchange_volume_asof ON offexchange_volume(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_offexchange_volume_latest ON offexchange_volume(is_latest, period_type, summary_start_date)",
        "CREATE INDEX IF NOT EXISTS idx_offexchange_secperiod_key ON offexchange_security_period(symbol, period_type, summary_start_date)",
        "CREATE INDEX IF NOT EXISTS idx_offexchange_secperiod_asof ON offexchange_security_period(as_of_date, available_at)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_offexchange_security_period AS
        SELECT
            sp.security_period_id,
            sp.security_id,
            coalesce(s.primary_symbol, sp.symbol) AS symbol,
            s.name AS security_name,
            sp.period_type,
            sp.summary_start_date,
            sp.summary_end_date,
            sp.ats_share_quantity,
            sp.non_ats_share_quantity,
            sp.total_share_quantity,
            sp.ats_share_pct,
            sp.ats_venue_count,
            sp.restatement_detected,
            sp.as_of_date,
            sp.available_at,
            sp.source
        FROM offexchange_security_period sp
        LEFT JOIN securities s ON s.security_id = sp.security_id
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'offexchange_volume',
            'finra',
            'FINRA off-exchange ATS/OTC transparency volume',
            'FINRA OTC Transparency weekly/monthly/block off-exchange ATS and non-ATS volume per security and venue, with a derived per-security ATS-share rollup.',
            'symbol,period_type,summary_start_date,venue_class,mpid',
            'offexchange_volume', 'as_of_date', 'available_at', now()
        )
        """
    )
    for table_name, entity, grain, description, natural_key, pit_notes in (
        (
            "offexchange_venue", "offexchange_venue", "mpid",
            "FINRA off-exchange venue (MPID) dimension distinguishing registered ATS dark pools from non-ATS member firms.",
            '["mpid"]',
            "venue_class is the authoritative ATS vs non_ATS discriminator; non-ATS mpid is a firm-CRD-derived identifier, not a registered MPID.",
        ),
        (
            "offexchange_volume", "offexchange_transparency", "symbol,period_type,summary_start_date,venue_class,mpid",
            "FINRA OTC Transparency per (security, venue, period) off-exchange volume fact.",
            '["volume_id"]',
            "Resolve with available_at <= query ts and is_latest to honour FINRA restatements; FINRA publishes on a tiered delay (Tier1 ~14d, Tier2/OTCE ~28d).",
        ),
        (
            "offexchange_security_period", "offexchange_transparency", "symbol,period_type,summary_start_date",
            "Derived per (security, period) ATS vs non-ATS off-exchange share rollup.",
            '["security_period_id"]',
            "Materialized from the latest visible offexchange_volume rows; ats_share_pct is ats_share_quantity / total_share_quantity * 100.",
        ),
    ):
        conn.execute(
            """
            INSERT OR REPLACE INTO table_catalog (
                table_name, layer, entity, grain, description,
                natural_key_json, pit_notes, updated_at
            )
            VALUES (?, 'silver', ?, ?, ?, ?, ?, now())
            """,
            [table_name, entity, grain, description, natural_key, pit_notes],
        )

    for table_name in (
        "offexchange_venue",
        "offexchange_volume",
        "offexchange_security_period",
        "v_offexchange_security_period",
    ):
        conn.execute(
            """
            INSERT OR REPLACE INTO field_catalog (
                table_name, field_name, semantic_type, description,
                nullable, unit, source_field, updated_at
            )
            SELECT
                c.table_name,
                c.column_name,
                CASE
                    WHEN lower(c.column_name) IN ('volume_id', 'security_period_id', 'security_id', 'mpid', 'run_id', 'source_file_sha256') THEN 'identifier'
                    WHEN lower(c.column_name) LIKE '%_date' OR upper(c.data_type) = 'DATE' THEN 'date'
                    WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                    WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                    WHEN lower(c.column_name) IN ('is_latest', 'restatement_detected') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                    WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                    ELSE 'text'
                END AS semantic_type,
                CASE c.column_name
                    WHEN 'venue_class' THEN 'ATS or non_ATS off-exchange venue class.'
                    WHEN 'summary_type_code' THEN 'FINRA summaryTypeCode: ATS_W_SMBL (ATS) or OTC_W_SMBL (non-ATS).'
                    WHEN 'period_type' THEN 'weekly, monthly, or block FINRA summary period.'
                    WHEN 'tier' THEN 'NMS tier T1/T2 or OTCE driving the publication delay.'
                    WHEN 'ats_share_pct' THEN 'ATS share of total off-exchange volume in percent (0..100).'
                    WHEN 'total_share_quantity' THEN 'Total reported off-exchange share volume for the row grain.'
                    WHEN 'finra_last_update_date' THEN 'FINRA lastUpdateDate used to detect metadata-only amends.'
                    WHEN 'is_latest' THEN 'True for the most recent restatement of this (security, venue, period).'
                    ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
                END AS description,
                coalesce(c.is_nullable, true) AS nullable,
                CASE
                    WHEN c.column_name = 'ats_share_pct' THEN 'percent'
                    WHEN lower(c.column_name) LIKE '%share_quantity' THEN 'shares'
                    WHEN lower(c.column_name) LIKE '%_date' THEN 'date'
                    WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                    ELSE NULL
                END AS unit,
                NULL AS source_field,
                now() AS updated_at
            FROM duckdb_columns() c
            WHERE c.schema_name = 'main'
              AND coalesce(c.internal, false) = false
              AND c.table_name = ?
            """,
            [table_name],
        )


def _catalog_backfill_reference_and_views(conn: duckdb.DuckDBPyConnection) -> None:
    """S7b: backfill table_catalog for reference-classification tables (built in
    migration 3 but never catalogued) and the off-exchange security-period view,
    closing the standing missing_table_catalog_entries quality failure for them."""

    rows = (
        (
            "taxonomy", "gold", "industry_taxonomy", "taxonomy_id",
            "Industry-classification taxonomy registry (SIC, NAICS, Fama-French, etc.).",
            '["taxonomy_id"]',
            "Reference dimension; taxonomy revisions are tracked via taxonomy_node bitemporal validity, not here.",
        ),
        (
            "taxonomy_node", "gold", "industry_taxonomy", "taxonomy_id,node_code",
            "Hierarchical nodes within an industry taxonomy (division/major-group/leaf).",
            '["node_id"]',
            "node_code is unique within a taxonomy; parent_node_id encodes the hierarchy.",
        ),
        (
            "taxonomy_mapping", "gold", "industry_taxonomy", "from_taxonomy_id,from_node_code,to_taxonomy_id,to_node_code",
            "Many-to-many crosswalk between taxonomy nodes (e.g. SIC to NAICS) with a confidence weight.",
            '["mapping_id"]',
            "Crosswalks are many-to-many; confidence captures approximate mappings.",
        ),
        (
            "entity_classification", "silver", "entity_classification", "security_id,taxonomy_id,node_code,valid_from",
            "PIT entity-to-taxonomy-node classifications (primary SIC plus derived Fama-French/NAICS).",
            '["classification_id"]',
            "Resolve with valid_from/valid_to covering the query date and available_at visible; is_primary marks the authoritative SIC.",
        ),
        (
            "v_offexchange_security_period", "silver", "offexchange_transparency", "symbol,period_type,summary_start_date",
            "Security-joined off-exchange ATS-share rollup view exposing ticker and security name.",
            '["security_period_id"]',
            "View over offexchange_security_period LEFT JOIN securities; inherits the rollup PIT columns.",
        ),
    )
    for table_name, layer, entity, grain, description, natural_key, pit_notes in rows:
        conn.execute(
            """
            INSERT OR REPLACE INTO table_catalog (
                table_name, layer, entity, grain, description,
                natural_key_json, pit_notes, updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, now())
            """,
            [table_name, layer, entity, grain, description, natural_key, pit_notes],
        )


def _fundamental_ratios(conn: duckdb.DuckDBPyConnection) -> None:
    """S9a: derived point-in-time financial-ratio surface.

    A single long-format fact table (one row per security, period, ratio_code)
    computed from the bitemporal TTM flows (``fundamental_ttm_points``) and the
    instant balances (``fundamental_statement_points``). Each ratio carries its
    own ``available_at`` = max availability of the specific inputs it consumes,
    so an as-of query returns a ratio only once every input it depends on was
    knowable. This is the Compustat/FactSet "calculated items" parity surface.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_ratios (
            ratio_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            upstream_source VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            ratio_code VARCHAR NOT NULL,
            ratio_category VARCHAR NOT NULL,
            ratio_kind VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            value DOUBLE,
            numerator_code VARCHAR,
            numerator_value DOUBLE,
            denominator_code VARCHAR,
            denominator_value DOUBLE,
            is_meaningful BOOLEAN NOT NULL DEFAULT true,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            input_codes_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ratios_key ON fundamental_ratios(security_id, ratio_code, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ratios_symbol ON fundamental_ratios(symbol, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ratios_asof ON fundamental_ratios(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ratios_code ON fundamental_ratios(ratio_code, basis)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_ratios',
            'sec_edgar',
            'Derived point-in-time financial ratios',
            'Compustat/FactSet-style calculated financial ratios (profitability, leverage, cash-flow, payout, per-share) derived from trailing-twelve-month fundamental flows and instant balances, with per-ratio bitemporal availability.',
            'security_id,ratio_code,basis,period_end',
            'fundamental_ratios', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'fundamental_ratios', 'gold', 'fundamental_ratios',
            'security_id,ratio_code,basis,period_end',
            'Long-format derived financial-ratio fact: one row per (security, period, ratio_code) over a TTM/instant basis.',
            '["ratio_id"]',
            'Each ratio''s available_at is the max availability of its specific inputs; resolve with available_at <= query ts. as_of_date is the period_end (TTM window close). v1 stores the latest-revision vintage; restatement-vintage ratio history is a planned refinement.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('ratio_id', 'security_id', 'cik', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('period_end', 'period_start', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN lower(c.column_name) IN ('is_meaningful', 'is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'ratio_code' THEN 'Stable ratio identifier (e.g. net_profit_margin, return_on_equity, free_cash_flow).'
                WHEN 'ratio_category' THEN 'Ratio family: profitability, leverage, efficiency, liquidity, cash_flow, payout, per_share, growth, or health.'
                WHEN 'ratio_kind' THEN 'ratio (dimensionless), level (currency sum), difference (currency net), per_share, growth (YoY fractional change), or score (weighted composite, e.g. Altman Z'''').'
                WHEN 'basis' THEN 'Input basis for the ratio (ttm = trailing-twelve-month flows over the instant balance at period_end).'
                WHEN 'value' THEN 'Computed ratio value; numerator_value / denominator_value for ratio/per_share kinds, numerator_value + denominator_value for level kinds.'
                WHEN 'numerator_value' THEN 'Effective numerator operand used to compute value (post sign/abs transform).'
                WHEN 'denominator_value' THEN 'Effective denominator operand used to compute value.'
                WHEN 'is_meaningful' THEN 'False when the denominator is non-positive (e.g. ROE with negative equity), so the sign is economically misleading.'
                WHEN 'is_latest_revision' THEN 'True for the latest-revision vintage of the inputs (v1 stores only the latest vintage).'
                WHEN 'available_at' THEN 'Max availability timestamp across the ratio''s inputs; the ratio is knowable only at/after this instant.'
                WHEN 'period_end' THEN 'TTM window close / instant balance date the ratio is computed at.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN c.column_name = 'value' THEN 'ratio_or_currency'
                WHEN lower(c.column_name) IN ('numerator_value', 'denominator_value') THEN 'currency'
                WHEN lower(c.column_name) IN ('period_end', 'period_start', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'fundamental_ratios'
        """
    )


def _fundamental_xbrl_metric(conn: duckdb.DuckDBPyConnection) -> None:
    """S10a: consolidated inline-XBRL metric extraction surface.

    Canonical metric rows pulled from the already-cached ``xbrl_filing_facts`` for
    concepts the narrow companyfacts feed never carried (current assets/liabilities,
    cash, inventory, ...). Only entity-level (non-segment) facts are kept — selected
    via the fact's filing context having zero dimension members — so values are
    consolidated totals, not segment breakdowns. Bitemporal: ``available_at`` is the
    filing acceptance time, ``as_of_date`` the balance/period date; all filing
    vintages are retained with ``is_latest_revision`` flagging the most recent.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_xbrl_metric (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            canonical_metric VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL DEFAULT 'us-gaap',
            unit VARCHAR,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            accession_number VARCHAR,
            value DOUBLE,
            raw_value VARCHAR,
            revision_seq INTEGER NOT NULL DEFAULT 0,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_xbrl_metric_key ON fundamental_xbrl_metric(security_id, canonical_metric, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_xbrl_metric_metric ON fundamental_xbrl_metric(canonical_metric, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_xbrl_metric_asof ON fundamental_xbrl_metric(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_xbrl_metric_latest ON fundamental_xbrl_metric(is_latest_revision, period_type)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_xbrl_metric',
            'sec_edgar',
            'Consolidated inline-XBRL canonical metrics',
            'Entity-level (non-segment) us-gaap facts extracted from cached inline-XBRL filings into canonical metrics (current assets/liabilities, cash, inventory, ...) that the companyfacts feed does not carry; bitemporal with restatement vintages.',
            'security_id,canonical_metric,period_end',
            'fundamental_xbrl_metric', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'fundamental_xbrl_metric', 'silver', 'fundamental_xbrl_metric',
            'security_id,canonical_metric,period_end',
            'Canonical metrics extracted from consolidated inline-XBRL facts (one row per security, metric, period, filing vintage).',
            '["metric_id"]',
            'Consolidated totals only (fact context has zero dimension members). Resolve with available_at <= query ts and is_latest_revision; available_at is the filing acceptance time.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('metric_id', 'security_id', 'cik', 'accession_number', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('period_end', 'period_start', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'canonical_metric' THEN 'Warehouse-canonical metric name (e.g. current_assets, current_liabilities, cash_and_equivalents, inventory).'
                WHEN 'concept' THEN 'Source us-gaap concept local name the metric was mapped from.'
                WHEN 'period_type' THEN 'instant (balance date) or duration (period flow).'
                WHEN 'is_latest_revision' THEN 'True for the most recent filing vintage of this (security, metric, period).'
                WHEN 'available_at' THEN 'Filing acceptance timestamp; the metric is knowable only at/after this instant.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) IN ('period_end', 'period_start', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'fundamental_xbrl_metric'
        """
    )


def _short_interest_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S11: derived FINRA short-interest analytics surface.

    One row per (security_id, settlement_date) computed from the cached
    ``finra_short_interest`` feed: per-security derived metrics (short-interest
    change, recomputed days-to-cover, short % of shares outstanding) plus the
    genuinely value-additive piece the raw feed lacks — the **cross-sectional
    percentile** of days-to-cover / short-interest change within each settlement
    cohort (the short-interest anomaly signal). Bitemporal: ``as_of_date`` is the
    settlement date, ``available_at`` the FINRA publication time; the latest filing
    vintage per key is flagged ``is_latest_revision``. PIT-safe — percentiles are
    computed within a single settlement cohort, all published together.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS short_interest_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            issue_name VARCHAR,
            settlement_date DATE NOT NULL,
            current_short_position BIGINT,
            previous_short_position BIGINT,
            average_daily_volume BIGINT,
            short_interest_change BIGINT,
            short_interest_change_pct DOUBLE,
            days_to_cover DOUBLE,
            days_to_cover_source DOUBLE,
            short_pct_shares_outstanding DOUBLE,
            days_to_cover_percentile DOUBLE,
            short_interest_change_pct_percentile DOUBLE,
            universe_count INTEGER,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_short_interest_metrics_key ON short_interest_metrics(security_id, settlement_date)",
        "CREATE INDEX IF NOT EXISTS idx_short_interest_metrics_settle ON short_interest_metrics(settlement_date)",
        "CREATE INDEX IF NOT EXISTS idx_short_interest_metrics_asof ON short_interest_metrics(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_short_interest_metrics_latest ON short_interest_metrics(is_latest_revision)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'short_interest_metrics',
            'finra',
            'Derived FINRA short-interest analytics',
            'Per-security short-interest analytics derived from the cached FINRA consolidated short-interest feed: short-interest change, recomputed days-to-cover, short percent of shares outstanding, and cross-sectional percentile ranks within each settlement cohort. Bitemporal with publication-time availability.',
            'security_id,settlement_date',
            'short_interest_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'short_interest_metrics', 'silver', 'short_interest_metrics',
            'security_id,settlement_date',
            'Derived short-interest metrics (one row per security, settlement date, filing vintage).',
            '["metric_id"]',
            'Resolve with available_at <= query ts and is_latest_revision; available_at is the FINRA publication time. Cross-sectional percentiles are within a single settlement cohort (all published together), so they are point-in-time safe.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('metric_id', 'security_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('settlement_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'current_short_position' THEN 'FINRA current short position quantity at the settlement date.'
                WHEN 'short_interest_change' THEN 'Current minus previous short position (share change over the settlement period).'
                WHEN 'short_interest_change_pct' THEN 'Short-interest change as a fraction of the previous position (NULL when previous <= 0).'
                WHEN 'days_to_cover' THEN 'Recomputed short interest / average daily volume (NULL when ADV <= 0).'
                WHEN 'days_to_cover_source' THEN 'FINRA-reported days-to-cover, retained for reconciliation.'
                WHEN 'short_pct_shares_outstanding' THEN 'Current short position / point-in-time shares outstanding (NULL when no share count is known as of the settlement).'
                WHEN 'days_to_cover_percentile' THEN 'Cross-sectional percentile (0-1) of days-to-cover within the settlement cohort; higher = more crowded short.'
                WHEN 'short_interest_change_pct_percentile' THEN 'Cross-sectional percentile (0-1) of short-interest change within the settlement cohort.'
                WHEN 'universe_count' THEN 'Number of securities in the settlement cohort the percentiles were ranked over.'
                WHEN 'is_latest_revision' THEN 'True for the most recent FINRA vintage of this (security, settlement).'
                WHEN 'available_at' THEN 'FINRA publication timestamp; the metric is knowable only at/after this instant.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) IN ('settlement_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%_pct%' OR lower(c.column_name) LIKE '%percentile%' THEN 'ratio'
                WHEN lower(c.column_name) = 'days_to_cover' OR lower(c.column_name) = 'days_to_cover_source' THEN 'days'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'short_interest_metrics'
        """
    )


def _short_interest_metrics_trend(conn: duckdb.DuckDBPyConnection) -> None:
    """S12: add the time-series trend + cross-sectional z-score columns.

    ``days_to_cover_zscore`` is the within-settlement-cohort standardized days-to-cover
    (population std; complements the percentile by capturing tail magnitude).
    ``days_to_cover_prev`` / ``days_to_cover_change`` are the prior-settlement
    days-to-cover and its change for the same security — rising days-to-cover signals
    building short pressure. All are recomputed wholesale by the loader.
    """
    for column in (
        "days_to_cover_zscore DOUBLE",
        "days_to_cover_prev DOUBLE",
        "days_to_cover_change DOUBLE",
    ):
        conn.execute(f"ALTER TABLE short_interest_metrics ADD COLUMN IF NOT EXISTS {column}")

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name, 'measure',
            CASE c.column_name
                WHEN 'days_to_cover_zscore' THEN 'Standardized days-to-cover within the settlement cohort (population std); NaN for a single-name cohort.'
                WHEN 'days_to_cover_prev' THEN 'Recomputed days-to-cover at the prior settlement for the same security.'
                WHEN 'days_to_cover_change' THEN 'Change in days-to-cover vs the prior settlement (rising = building short pressure).'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END AS description,
            true AS nullable,
            CASE WHEN c.column_name = 'days_to_cover_zscore' THEN 'zscore' ELSE 'days' END AS unit,
            NULL AS source_field, now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND c.table_name = 'short_interest_metrics'
          AND c.column_name IN ('days_to_cover_zscore', 'days_to_cover_prev', 'days_to_cover_change')
        """
    )


def _macro_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S13: derived macro-analytics surface.

    One row per (series_id, observation_date) computed from the cached FRED
    ``macro_observations`` feed: per-series level, change vs prior observation,
    year-over-year change/growth, and an expanding z-score (regime position vs
    history-to-date), plus a synthetic ``T10Y2Y`` term-spread series (DGS10 - DGS2,
    the canonical yield-curve regime signal). Bitemporal: ``as_of_date`` is the
    observation date, ``available_at`` carried from the source observation.

    PIT caveat: the raw feed is the latest-revision FRED graph CSV (not ALFRED
    vintages), so macro-revision PIT is approximate — ``available_at`` reflects the
    warehouse load, not the true first-release time. All derived values use only the
    current and earlier observations (YoY looks ~1y back; the z-score is expanding),
    so there is no forward leakage within the latest-revision series.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS macro_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            series_id VARCHAR NOT NULL,
            observation_date DATE NOT NULL,
            frequency VARCHAR,
            units VARCHAR,
            value DOUBLE,
            change_abs DOUBLE,
            change_yoy DOUBLE,
            yoy_growth DOUBLE,
            zscore DOUBLE,
            is_synthetic BOOLEAN NOT NULL DEFAULT false,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_macro_metrics_key ON macro_metrics(series_id, observation_date)",
        "CREATE INDEX IF NOT EXISTS idx_macro_metrics_date ON macro_metrics(observation_date)",
        "CREATE INDEX IF NOT EXISTS idx_macro_metrics_asof ON macro_metrics(as_of_date, available_at)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'macro_metrics',
            'fred',
            'Derived macro analytics',
            'Per-series macro analytics derived from the cached FRED observation feed (level, change, year-over-year change/growth, expanding z-score) plus a synthetic 10Y-2Y Treasury term-spread series; bitemporal, latest-revision FRED (not ALFRED vintages).',
            'series_id,observation_date',
            'macro_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'macro_metrics', 'silver', 'macro_metrics',
            'series_id,observation_date',
            'Derived macro metrics (one row per series, observation date).',
            '["metric_id"]',
            'Resolve with available_at <= query ts and is_latest_revision. Latest-revision FRED (not ALFRED), so macro-revision PIT is approximate; derived values use only current+earlier observations.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('metric_id', 'series_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('observation_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('is_latest_revision', 'is_synthetic') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'value' THEN 'Series level at the observation date (or the 10Y-2Y spread for the synthetic T10Y2Y series).'
                WHEN 'change_abs' THEN 'Change vs the prior observation of the same series.'
                WHEN 'change_yoy' THEN 'Value minus the value ~1 year earlier (absolute; percentage points for rate series).'
                WHEN 'yoy_growth' THEN 'Year-over-year growth (value / value_1y_ago - 1); for CPI this is the inflation rate. NULL when the year-ago base is non-positive.'
                WHEN 'zscore' THEN 'Expanding z-score of the level vs the series history up to and including this observation (regime position).'
                WHEN 'is_synthetic' THEN 'True for derived cross-series rows (e.g. T10Y2Y term spread).'
                WHEN 'available_at' THEN 'Source observation availability (warehouse load for latest-revision FRED; not ALFRED first-release).'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('observation_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN c.column_name = 'yoy_growth' THEN 'ratio'
                WHEN c.column_name = 'zscore' THEN 'zscore'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'macro_metrics'
        """
    )


def _equity_price_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S14: derived daily price-analytics surface.

    One row per (security_id, trade_date) computed from the cached
    ``equity_daily_bars`` feed: split/dividend-adjusted daily and log returns, the
    overnight gap, trailing realized volatility (20d/60d, annualized), trailing-return
    momentum (21d/126d), distance from the trailing 252-day high, and dollar volume.
    Bitemporal: ``as_of_date`` is the trade date, ``available_at`` carried from the bar.
    Every rolling/lag feature uses only the current and earlier bars, so there is no
    forward leakage.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS equity_price_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            trade_date DATE NOT NULL,
            close DOUBLE,
            adjusted_close DOUBLE,
            volume BIGINT,
            dollar_volume DOUBLE,
            daily_return DOUBLE,
            log_return DOUBLE,
            gap_return DOUBLE,
            realized_vol_20d DOUBLE,
            realized_vol_60d DOUBLE,
            momentum_21d DOUBLE,
            momentum_126d DOUBLE,
            pct_from_high_252d DOUBLE,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_equity_price_metrics_key ON equity_price_metrics(security_id, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_equity_price_metrics_date ON equity_price_metrics(trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_equity_price_metrics_asof ON equity_price_metrics(as_of_date, available_at)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'equity_price_metrics',
            'pricing',
            'Derived daily price analytics',
            'Per-security daily price analytics derived from the cached adjusted daily bars (daily/log return, overnight gap, trailing realized volatility, momentum, distance from trailing high, dollar volume); bitemporal.',
            'security_id,trade_date',
            'equity_price_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'equity_price_metrics', 'silver', 'equity_price_metrics',
            'security_id,trade_date',
            'Derived daily price metrics (one row per security, trade date).',
            '["metric_id"]',
            'Resolve with available_at <= query ts and is_latest_revision. Rolling/lag features use only current and earlier bars; returns are computed on the split/dividend-adjusted close.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('metric_id', 'security_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'adjusted_close' THEN 'Split/dividend back-adjusted close, computed from the bar close times the product of future split_factor values (the feed adjusted_close is unreliable in the cached sample).'
                WHEN 'daily_return' THEN 'Back-adjusted-close simple return vs the prior trading day.'
                WHEN 'log_return' THEN 'Natural-log return on the back-adjusted close.'
                WHEN 'gap_return' THEN 'Overnight gap on the back-adjusted basis: adjusted open / prior-day adjusted close - 1.'
                WHEN 'realized_vol_20d' THEN 'Annualized standard deviation of the trailing 20 daily returns.'
                WHEN 'realized_vol_60d' THEN 'Annualized standard deviation of the trailing 60 daily returns.'
                WHEN 'momentum_21d' THEN 'Trailing 21-trading-day back-adjusted-close return.'
                WHEN 'momentum_126d' THEN 'Trailing 126-trading-day back-adjusted-close return.'
                WHEN 'pct_from_high_252d' THEN 'Back-adjusted close divided by the trailing 252-day adjusted high minus 1 (drawdown from the rolling high; <= 0).'
                WHEN 'dollar_volume' THEN 'Close price times share volume (traded notional).'
                WHEN 'available_at' THEN 'Bar availability timestamp; the metric is knowable only at/after this instant.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%return%' OR lower(c.column_name) LIKE '%vol%' OR lower(c.column_name) LIKE 'momentum%' OR lower(c.column_name) LIKE 'pct_%' THEN 'ratio'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'equity_price_metrics'
        """
    )


def _short_interest_metrics_momentum(conn: duckdb.DuckDBPyConnection) -> None:
    """S15: multi-settlement short-interest momentum + squeeze-candidate flag.

    ``short_interest_momentum_3`` / ``days_to_cover_change_3`` are the short-interest
    growth and days-to-cover change over the trailing 3 settlements for the same
    security; ``is_squeeze_candidate`` flags a top-decile-crowded name whose
    days-to-cover is still rising. All recomputed wholesale by the loader.
    """
    for column in (
        "short_interest_momentum_3 DOUBLE",
        "days_to_cover_change_3 DOUBLE",
        "is_squeeze_candidate BOOLEAN",
    ):
        conn.execute(f"ALTER TABLE short_interest_metrics ADD COLUMN IF NOT EXISTS {column}")

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE WHEN c.column_name = 'is_squeeze_candidate' THEN 'flag' ELSE 'measure' END,
            CASE c.column_name
                WHEN 'short_interest_momentum_3' THEN 'Short-interest growth over the trailing 3 settlements ((current - 3-settlements-ago) / 3-settlements-ago).'
                WHEN 'days_to_cover_change_3' THEN 'Change in days-to-cover over the trailing 3 settlements for the same security.'
                WHEN 'is_squeeze_candidate' THEN 'True when days-to-cover is top-decile in the cohort (percentile >= 0.90) and the most-recent days-to-cover change is positive.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            true,
            CASE WHEN c.column_name = 'short_interest_momentum_3' THEN 'ratio'
                 WHEN c.column_name = 'days_to_cover_change_3' THEN 'days' ELSE NULL END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND c.table_name = 'short_interest_metrics'
          AND c.column_name IN ('short_interest_momentum_3', 'days_to_cover_change_3', 'is_squeeze_candidate')
        """
    )


def _short_interest_metrics_acceleration(conn: duckdb.DuckDBPyConnection) -> None:
    """S23: longer-horizon short-pressure persistence and acceleration fields.

    These columns extend the broad FINRA-derived ``short_interest_metrics`` surface
    without changing its grain. They are recomputed wholesale by the loader from
    already-published prior settlements for the same security.
    """
    for column in (
        "short_interest_momentum_6 DOUBLE",
        "days_to_cover_change_6 DOUBLE",
        "short_interest_change_pct_accel DOUBLE",
        "days_to_cover_change_accel DOUBLE",
        "short_pressure_score DOUBLE",
        "is_persistent_short_pressure BOOLEAN",
    ):
        conn.execute(f"ALTER TABLE short_interest_metrics ADD COLUMN IF NOT EXISTS {column}")

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE WHEN c.column_name = 'is_persistent_short_pressure' THEN 'flag' ELSE 'measure' END,
            CASE c.column_name
                WHEN 'short_interest_momentum_6' THEN 'Short-interest growth over the trailing 6 settlements ((current - 6-settlements-ago) / 6-settlements-ago).'
                WHEN 'days_to_cover_change_6' THEN 'Change in days-to-cover over the trailing 6 settlements for the same security.'
                WHEN 'short_interest_change_pct_accel' THEN 'Acceleration of short-interest change: current short-interest change percentage minus the prior-settlement short-interest change percentage.'
                WHEN 'days_to_cover_change_accel' THEN 'Acceleration of days-to-cover: current days-to-cover change minus the prior-settlement days-to-cover change.'
                WHEN 'short_pressure_score' THEN 'Bounded 0-100 pressure score combining days-to-cover crowding percentile, short-interest-change percentile, and positive recent/persistent days-to-cover trend flags.'
                WHEN 'is_persistent_short_pressure' THEN 'True when the row is a squeeze candidate and both 3-settlement days-to-cover change and 3-settlement short-interest momentum are positive.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            true,
            CASE
                WHEN c.column_name IN ('short_interest_momentum_6', 'short_interest_change_pct_accel') THEN 'ratio'
                WHEN c.column_name IN ('days_to_cover_change_6', 'days_to_cover_change_accel') THEN 'days'
                WHEN c.column_name = 'short_pressure_score' THEN 'score'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND c.table_name = 'short_interest_metrics'
          AND c.column_name IN (
              'short_interest_momentum_6',
              'days_to_cover_change_6',
              'short_interest_change_pct_accel',
              'days_to_cover_change_accel',
              'short_pressure_score',
              'is_persistent_short_pressure'
          )
        """
    )


def _short_interest_metrics_liquidity_pressure(conn: duckdb.DuckDBPyConnection) -> None:
    """S24: liquidity-aware and winsorized short-pressure diagnostics.

    These ALTER-only columns keep the raw short-pressure score intact while exposing
    a tail-resistant days-to-cover diagnostic and an explicit tradeability gate for
    names with at least 50k average daily volume and 100k reported short interest.
    """
    for column in (
        "average_daily_volume_percentile DOUBLE",
        "days_to_cover_winsorized DOUBLE",
        "days_to_cover_winsorized_zscore DOUBLE",
        "liquid_short_pressure_score DOUBLE",
        "is_liquid_short_pressure BOOLEAN",
    ):
        conn.execute(f"ALTER TABLE short_interest_metrics ADD COLUMN IF NOT EXISTS {column}")

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE WHEN c.column_name = 'is_liquid_short_pressure' THEN 'flag' ELSE 'measure' END,
            CASE c.column_name
                WHEN 'average_daily_volume_percentile' THEN 'Average-daily-volume percentile within the settlement cohort; a PIT liquidity rank published with the cohort.'
                WHEN 'days_to_cover_winsorized' THEN 'Days-to-cover capped at the settlement cohort 99th percentile, preserving the raw days_to_cover while reducing tiny-ADV tail leverage.'
                WHEN 'days_to_cover_winsorized_zscore' THEN 'Within-cohort z-score of 99th-percentile-winsorized days-to-cover, using population standard deviation.'
                WHEN 'liquid_short_pressure_score' THEN 'Short-pressure score retained only for rows meeting the tradeability floor (average_daily_volume >= 50,000 and current_short_position >= 100,000); NULL otherwise.'
                WHEN 'is_liquid_short_pressure' THEN 'True when the row is a squeeze candidate, passes the 50k ADV / 100k short-position floor, and has short_pressure_score >= 70.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            true,
            CASE
                WHEN c.column_name = 'average_daily_volume_percentile' THEN 'percentile'
                WHEN c.column_name = 'days_to_cover_winsorized' THEN 'days'
                WHEN c.column_name = 'days_to_cover_winsorized_zscore' THEN 'zscore'
                WHEN c.column_name = 'liquid_short_pressure_score' THEN 'score'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND c.table_name = 'short_interest_metrics'
          AND c.column_name IN (
              'average_daily_volume_percentile',
              'days_to_cover_winsorized',
              'days_to_cover_winsorized_zscore',
              'liquid_short_pressure_score',
              'is_liquid_short_pressure'
          )
        """
    )


def _thirteenf_position_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S16: derived 13F manager-level position-flow analytics surface.

    One row per (manager_id, security_id, report_period) computed from the cached
    ``thirteenf_security_positions`` + ``thirteenf_manager_reports`` feed: the
    quarter-over-quarter common-share position change and a NEW / ADDED / TRIMMED /
    UNCHANGED / EXITED action (the canonical 13F "smart-money flow" signal), plus
    voting-authority concentration. Complements the issuer-level rollup
    ``thirteenf_security_ownership`` (built by :mod:`db.ownership`) with the per-manager
    conviction layer. Bitemporal: ``as_of_date`` is the report period, ``available_at``
    the filing's availability instant (approximate — the cached feed carries the
    warehouse ingest time, never earlier than the true SEC filing). PIT-safe: every value
    uses only this manager's current and immediately-prior-quarter filings.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_position_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            manager_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cusip VARCHAR,
            name_of_issuer VARCHAR,
            report_period DATE NOT NULL,
            filing_date DATE,
            shares_held DOUBLE,
            value_usd DOUBLE,
            portfolio_weight DOUBLE,
            shares_held_prev DOUBLE,
            shares_change DOUBLE,
            shares_change_pct DOUBLE,
            value_change DOUBLE,
            position_action VARCHAR,
            is_new_position BOOLEAN,
            is_closed_position BOOLEAN,
            voting_sole_pct DOUBLE,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_position_metrics_key ON thirteenf_position_metrics(manager_id, security_id, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_position_metrics_security ON thirteenf_position_metrics(security_id, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_position_metrics_asof ON thirteenf_position_metrics(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_position_metrics_action ON thirteenf_position_metrics(position_action)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_position_metrics_latest ON thirteenf_position_metrics(is_latest_revision)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'thirteenf_position_metrics',
            'sec',
            'Derived 13F manager-level position analytics',
            'Per-manager institutional position analytics derived from the cached SEC Form 13F holdings feed: quarter-over-quarter common-share change, NEW/ADDED/TRIMMED/UNCHANGED/EXITED action, and voting-authority concentration. Complements the issuer-level thirteenf_security_ownership rollup. Bitemporal with filing-time availability.',
            'manager_id,security_id,report_period',
            'thirteenf_position_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'thirteenf_position_metrics', 'silver', 'thirteenf_position_metrics',
            'manager_id,security_id,report_period',
            'Derived 13F manager-level position-flow metrics (one row per manager, security, report period; plus synthetic EXITED rows for dropped holdings).',
            '["metric_id"]',
            'Resolve with available_at <= query ts and is_latest_revision. available_at is the filing availability instant; it is approximate (the cached feed carries the warehouse ingest time) but conservative — never earlier than the true SEC filing. Each value uses only the manager''s current and immediately-prior-quarter filings, so it is point-in-time safe.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('metric_id', 'manager_id', 'security_id', 'run_id', 'cusip') THEN 'identifier'
                WHEN lower(c.column_name) IN ('report_period', 'as_of_date', 'filing_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'shares_held' THEN 'Aggregate common-share quantity the manager held in the issuer at the report period (0 for an EXITED row).'
                WHEN 'value_usd' THEN 'Aggregate reported common-share market value (USD) the manager held in the issuer.'
                WHEN 'shares_held_prev' THEN 'Common-share quantity held in the immediately-prior quarter (NULL for a NEW position).'
                WHEN 'shares_change' THEN 'Common-share quantity change vs the prior quarter (NULL for NEW).'
                WHEN 'shares_change_pct' THEN 'Share change as a fraction of the prior-quarter holding (NULL when prior <= 0).'
                WHEN 'value_change' THEN 'Reported value change vs the prior quarter (NULL for NEW).'
                WHEN 'position_action' THEN 'NEW / ADDED / TRIMMED / UNCHANGED / EXITED classification of the quarter-over-quarter move.'
                WHEN 'is_new_position' THEN 'True when the manager had no prior-quarter holding of this issuer.'
                WHEN 'is_closed_position' THEN 'True for a synthetic EXITED row (manager filed the quarter but dropped a prior holding).'
                WHEN 'voting_sole_pct' THEN 'Sole voting authority as a fraction of total (sole+shared+none) voting authority.'
                WHEN 'available_at' THEN 'Filing availability instant; the metric is knowable only at/after this instant (approximate, never earlier than the true SEC filing).'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) IN ('report_period', 'as_of_date', 'filing_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%_pct%' THEN 'ratio'
                WHEN lower(c.column_name) = 'value_usd' OR lower(c.column_name) = 'value_change' THEN 'usd'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'thirteenf_position_metrics'
        """
    )


def _macro_metrics_regime_percentile(conn: duckdb.DuckDBPyConnection) -> None:
    """S17: add the expanding regime-percentile column to macro_metrics.

    ``expanding_pct_rank`` is where the current observation sits within the series' own
    history so far (0, 1] — the canonical macro regime signal ("VIX in its 95th
    percentile"). Recomputed wholesale by the loader; rides the existing as-of / jobs /
    lake / watermark wiring.
    """
    conn.execute("ALTER TABLE macro_metrics ADD COLUMN IF NOT EXISTS expanding_pct_rank DOUBLE")
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name, 'measure',
            'Expanding regime percentile (0-1] of the observation within the series own history so far; higher = elevated vs its past.',
            coalesce(c.is_nullable, true), 'ratio', NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND c.table_name = 'macro_metrics'
          AND c.column_name = 'expanding_pct_rank'
        """
    )


def _macro_metrics_real_rates_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """S25: catalog the real-fed-funds synthetic macro series."""
    conn.execute(
        """
        UPDATE dataset_catalog
        SET description = 'Per-series macro analytics derived from the cached FRED observation feed (level, change, year-over-year change/growth, expanding z-score) plus synthetic T10Y2Y Treasury term spread and REAL_FEDFUNDS real-rate series; bitemporal, latest-revision FRED (not ALFRED vintages).',
            updated_at = now()
        WHERE dataset_id = 'macro_metrics'
        """
    )
    conn.execute(
        """
        UPDATE table_catalog
        SET pit_notes = 'Resolve with available_at <= query ts and is_latest_revision. Latest-revision FRED (not ALFRED), so macro-revision PIT is approximate; derived values use only current+earlier observations. Synthetic cross-series values use the later available_at of their input observations.',
            updated_at = now()
        WHERE table_name = 'macro_metrics'
        """
    )


def _finra_daily_short_volume(conn: duckdb.DuckDBPyConnection) -> None:
    """S26: FINRA daily short-volume flow surfaces."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS finra_short_volume (
            volume_id VARCHAR PRIMARY KEY,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            trade_date DATE NOT NULL,
            market_code VARCHAR NOT NULL,
            short_volume DOUBLE,
            short_exempt_volume DOUBLE,
            total_volume DOUBLE,
            restatement_seq INTEGER NOT NULL DEFAULT 0,
            is_latest BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            source VARCHAR NOT NULL,
            source_file VARCHAR,
            source_file_sha256 VARCHAR,
            raw_payload_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS short_volume_metrics (
            metric_id VARCHAR PRIMARY KEY,
            security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            trade_date DATE NOT NULL,
            short_volume DOUBLE,
            short_exempt_volume DOUBLE,
            total_volume DOUBLE,
            short_volume_ratio DOUBLE,
            short_exempt_ratio DOUBLE,
            short_volume_ratio_percentile DOUBLE,
            short_exempt_ratio_percentile DOUBLE,
            market_count INTEGER,
            dominant_market_code VARCHAR,
            dominant_market_total_volume DOUBLE,
            dominant_market_share_pct DOUBLE,
            is_high_short_flow BOOLEAN,
            restatement_seq INTEGER NOT NULL DEFAULT 0,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_finra_short_volume_key ON finra_short_volume(symbol, trade_date, market_code)",
        "CREATE INDEX IF NOT EXISTS idx_finra_short_volume_asof ON finra_short_volume(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_finra_short_volume_latest ON finra_short_volume(source, is_latest, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_short_volume_metrics_key ON short_volume_metrics(symbol, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_short_volume_metrics_asof ON short_volume_metrics(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_short_volume_metrics_latest ON short_volume_metrics(source, is_latest_revision, trade_date)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES
        (
            'finra_short_volume',
            'finra',
            'FINRA daily short-volume flow',
            'Raw FINRA daily short-sale volume rows by symbol, trade date, and reporting market code. This is a public high-frequency short-flow proxy, not a substitute for settle-date short interest. Bitemporal with source-file lineage and restatement vintages.',
            'symbol,trade_date,market_code',
            'finra_short_volume', 'as_of_date', 'available_at', now()
        ),
        (
            'short_volume_metrics',
            'finra',
            'Derived FINRA daily short-volume metrics',
            'Per-symbol/trade-date short-volume flow metrics derived from FINRA daily short-volume rows: short-volume ratio, short-exempt ratio, dominant market code, and same-day cross-sectional percentiles.',
            'symbol,trade_date',
            'short_volume_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES
        (
            'finra_short_volume', 'bronze', 'short_volume',
            'symbol,trade_date,market_code,available_at',
            'Raw FINRA daily short-volume rows by reporting market code and publication vintage.',
            '["volume_id"]',
            'Resolve visible rows with available_at <= query ts, then choose the latest vintage per source/symbol/trade_date/market_code. FINRA daily short volume is a flow proxy; it is not outstanding short interest.',
            now()
        ),
        (
            'short_volume_metrics', 'silver', 'short_volume_metrics',
            'symbol,trade_date,available_at',
            'Derived per-symbol daily short-volume flow metrics and cross-sectional percentiles.',
            '["metric_id"]',
            'Resolve visible rows with available_at <= query ts, then choose the latest metric revision per source/symbol/trade_date. Percentiles are ranked within a single trade-date cohort, so they are point-in-time safe once the daily file is available.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('symbol', 'market_code', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'short_volume' THEN 'FINRA-reported daily short-sale volume for the symbol/date (summed across market codes in short_volume_metrics).'
                WHEN 'short_exempt_volume' THEN 'FINRA-reported daily short-exempt volume for the symbol/date.'
                WHEN 'total_volume' THEN 'FINRA-reported total volume denominator for the symbol/date.'
                WHEN 'market_code' THEN 'FINRA reporting market code: N=NYSE TRF, Q=NASDAQ TRF Carteret, B=NASDAQ TRF Chicago, D=ADF; ALL is accepted for pre-aggregated injectable files.'
                WHEN 'short_volume_ratio' THEN 'Short volume divided by total volume for the symbol/date.'
                WHEN 'short_exempt_ratio' THEN 'Short-exempt volume divided by total volume for the symbol/date.'
                WHEN 'short_volume_ratio_percentile' THEN 'Cross-sectional percentile of short_volume_ratio within the same trade date.'
                WHEN 'short_exempt_ratio_percentile' THEN 'Cross-sectional percentile of short_exempt_ratio within the same trade date.'
                WHEN 'market_count' THEN 'Number of FINRA reporting market codes represented for the symbol/date.'
                WHEN 'dominant_market_code' THEN 'Market code contributing the largest total volume for the symbol/date.'
                WHEN 'dominant_market_total_volume' THEN 'Total volume from the dominant FINRA market code.'
                WHEN 'dominant_market_share_pct' THEN 'Dominant market total volume divided by symbol/date total volume, in percent.'
                WHEN 'is_high_short_flow' THEN 'True when short_volume_ratio is in the top decile of its same-day cohort and total_volume is positive.'
                WHEN 'available_at' THEN 'Publication/knowledge timestamp for the raw daily file or derived metric revision.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%ratio%' OR lower(c.column_name) LIKE '%percentile%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%share_pct%' THEN 'percent'
                WHEN lower(c.column_name) LIKE '%volume%' OR lower(c.column_name) LIKE '%count%' THEN 'count'
                ELSE NULL
            END,
            NULL,
            now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name IN ('finra_short_volume', 'short_volume_metrics')
        """
    )


def _offexchange_quality_report(conn: duckdb.DuckDBPyConnection) -> None:
    """S28: materialized off-exchange / short-flow quality report."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS offexchange_quality_report (
            report_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            surface VARCHAR NOT NULL,
            input_source VARCHAR,
            period_type VARCHAR NOT NULL,
            period_start_date DATE NOT NULL,
            period_end_date DATE,
            row_count BIGINT,
            security_count BIGINT,
            venue_or_market_count BIGINT,
            total_volume DOUBLE,
            ats_volume DOUBLE,
            non_ats_volume DOUBLE,
            short_volume DOUBLE,
            short_exempt_volume DOUBLE,
            short_volume_ratio DOUBLE,
            ats_share_pct DOUBLE,
            high_short_flow_count BIGINT,
            restated_key_count BIGINT,
            multiple_latest_key_count BIGINT,
            bad_row_count BIGINT,
            missing_available_at_count BIGINT,
            max_publication_lag_days DOUBLE,
            restatement_seq INTEGER NOT NULL DEFAULT 0,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            source_inputs_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_offexchange_quality_report_key ON offexchange_quality_report(surface, input_source, period_type, period_start_date)",
        "CREATE INDEX IF NOT EXISTS idx_offexchange_quality_report_asof ON offexchange_quality_report(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_offexchange_quality_report_latest ON offexchange_quality_report(source, is_latest_revision, surface)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'offexchange_quality_report',
            'finra',
            'Off-exchange and short-flow quality report',
            'Materialized quality/control report over public FINRA off-exchange transparency and daily short-volume flow surfaces: period coverage, row/security/venue counts, volume totals, restatement counts, bad-row counts, publication-lag diagnostics, and PIT availability.',
            'surface,input_source,period_type,period_start_date',
            'offexchange_quality_report', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'offexchange_quality_report', 'control', 'offexchange_quality_report',
            'surface,input_source,period_type,period_start_date,available_at',
            'Period-level quality report over offexchange_volume and finra_short_volume/short_volume_metrics inputs.',
            '["report_id"]',
            'Resolve with available_at <= query ts and is_latest_revision. Report revisions preserve changed input vintages; restatement_seq orders successive quality reports for the same surface/source/period.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('source', 'surface', 'input_source', 'period_type', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%date' OR lower(c.column_name) = 'as_of_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'surface' THEN 'Input surface summarized by the report: offexchange_volume or finra_short_volume.'
                WHEN 'input_source' THEN 'Source value of the summarized input rows.'
                WHEN 'period_start_date' THEN 'Report period start date (week/month/block start for OTC transparency, trade date for daily short volume).'
                WHEN 'row_count' THEN 'Latest visible input rows summarized for the surface/source/period.'
                WHEN 'security_count' THEN 'Distinct symbols summarized for the surface/source/period.'
                WHEN 'venue_or_market_count' THEN 'Distinct ATS/non-ATS MPIDs or FINRA daily short-volume market codes in the period.'
                WHEN 'total_volume' THEN 'Total share volume summarized for the period.'
                WHEN 'ats_volume' THEN 'ATS share volume for OTC transparency periods.'
                WHEN 'non_ats_volume' THEN 'Non-ATS share volume for OTC transparency periods.'
                WHEN 'short_volume' THEN 'FINRA daily short-sale volume summarized for the period.'
                WHEN 'short_exempt_volume' THEN 'FINRA daily short-exempt volume summarized for the period.'
                WHEN 'short_volume_ratio' THEN 'Short volume divided by total volume for daily short-volume periods.'
                WHEN 'ats_share_pct' THEN 'ATS volume divided by total off-exchange volume, in percent.'
                WHEN 'high_short_flow_count' THEN 'Count of symbols flagged as high short-flow in short_volume_metrics for the period.'
                WHEN 'restated_key_count' THEN 'Number of natural keys with more than one source vintage for the period.'
                WHEN 'multiple_latest_key_count' THEN 'Number of natural keys with more than one row marked latest; should be zero.'
                WHEN 'bad_row_count' THEN 'Input rows in the period violating basic validity checks.'
                WHEN 'missing_available_at_count' THEN 'Input rows in the period missing availability timestamps.'
                WHEN 'max_publication_lag_days' THEN 'Maximum days between period end/trade date and input availability date.'
                WHEN 'source_inputs_json' THEN 'JSON description of source tables and input source values used for the report.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) LIKE '%date' OR lower(c.column_name) = 'as_of_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%pct%' THEN 'percent'
                WHEN lower(c.column_name) LIKE '%ratio%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%lag_days%' THEN 'days'
                WHEN lower(c.column_name) LIKE '%volume%' OR lower(c.column_name) LIKE '%count%' THEN 'count'
                ELSE NULL
            END,
            NULL,
            now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'offexchange_quality_report'
        """
    )


def _insider_transaction_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S29: derived Section 16 insider transaction signal metrics."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS insider_transaction_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            input_source VARCHAR,
            security_id VARCHAR NOT NULL,
            issuer_cik VARCHAR,
            issuer_name VARCHAR,
            issuer_trading_symbol VARCHAR,
            signal_date DATE NOT NULL,
            window_days INTEGER NOT NULL,
            cluster_min_buyers INTEGER NOT NULL,
            cluster_min_purchase_value DOUBLE NOT NULL,
            transaction_count BIGINT NOT NULL,
            open_market_purchase_count BIGINT NOT NULL,
            open_market_sale_count BIGINT NOT NULL,
            discretionary_sale_count BIGINT NOT NULL,
            plan_sale_count BIGINT NOT NULL,
            grant_count BIGINT NOT NULL,
            exercise_count BIGINT NOT NULL,
            tax_withholding_count BIGINT NOT NULL,
            unique_insider_count BIGINT NOT NULL,
            buyer_count BIGINT NOT NULL,
            seller_count BIGINT NOT NULL,
            director_count BIGINT NOT NULL,
            officer_count BIGINT NOT NULL,
            ten_percent_owner_count BIGINT NOT NULL,
            gross_purchase_shares DOUBLE,
            gross_sale_shares DOUBLE,
            net_purchase_shares DOUBLE,
            gross_purchase_value DOUBLE,
            gross_sale_value DOUBLE,
            discretionary_sale_value DOUBLE,
            plan_sale_value DOUBLE,
            net_purchase_value DOUBLE,
            cluster_purchase_count BIGINT NOT NULL,
            cluster_buyer_count BIGINT NOT NULL,
            cluster_purchase_value DOUBLE,
            cluster_sale_count BIGINT NOT NULL,
            cluster_seller_count BIGINT NOT NULL,
            cluster_sale_value DOUBLE,
            plan_sale_value_ratio DOUBLE,
            is_cluster_buy BOOLEAN NOT NULL DEFAULT false,
            is_discretionary_sell_pressure BOOLEAN NOT NULL DEFAULT false,
            is_10b5_1_heavy_sale BOOLEAN NOT NULL DEFAULT false,
            source_transaction_ids_json VARCHAR,
            restatement_seq INTEGER NOT NULL DEFAULT 0,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_insider_transaction_metrics_key ON insider_transaction_metrics(source, security_id, signal_date, window_days)",
        "CREATE INDEX IF NOT EXISTS idx_insider_transaction_metrics_symbol ON insider_transaction_metrics(issuer_trading_symbol, signal_date)",
        "CREATE INDEX IF NOT EXISTS idx_insider_transaction_metrics_asof ON insider_transaction_metrics(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_insider_transaction_metrics_latest ON insider_transaction_metrics(source, is_latest_revision, signal_date)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'insider_transaction_metrics',
            'sec_edgar',
            'Insider transaction analytics',
            'Derived issuer-day and trailing-window signals over SEC Section 16 ownership XML: open-market purchases, 10b5-1 plan sale contamination, discretionary sale pressure, and cluster-buy diagnostics.',
            'source,security_id,signal_date,window_days',
            'insider_transaction_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'insider_transaction_metrics', 'gold', 'insider_transaction',
            'source,security_id,signal_date,window_days,available_at',
            'Issuer-day insider transaction signal surface derived from filing_form4 and insider_transaction rows.',
            '["metric_id"]',
            'Resolve with available_at <= query timestamp and is_latest_revision. Late filings for older transaction dates create new revisions while preserving earlier visible issuer-day signals.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('source', 'input_source', 'issuer_cik', 'issuer_trading_symbol', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%date' OR lower(c.column_name) = 'as_of_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'signal_date' THEN 'Issuer-day signal date, usually the Form 4 transaction date.'
                WHEN 'window_days' THEN 'Trailing calendar-day window used for cluster metrics.'
                WHEN 'open_market_purchase_count' THEN 'Daily count of P-coded open-market/private purchase rows.'
                WHEN 'open_market_sale_count' THEN 'Daily count of S-coded open-market/private sale rows.'
                WHEN 'discretionary_sale_count' THEN 'Daily S-coded sale rows where rule_10b5_1_indicator is explicitly false.'
                WHEN 'plan_sale_count' THEN 'Daily S-coded sale rows reported under a Rule 10b5-1 plan.'
                WHEN 'cluster_buyer_count' THEN 'Distinct buyers in the trailing signal window.'
                WHEN 'cluster_purchase_value' THEN 'Open-market purchase dollars in the trailing signal window.'
                WHEN 'plan_sale_value_ratio' THEN 'Daily plan sale value divided by daily gross sale value.'
                WHEN 'is_cluster_buy' THEN 'True when trailing-window buyers and purchase value meet row thresholds.'
                WHEN 'is_discretionary_sell_pressure' THEN 'True when trailing discretionary sale value dominates purchases and meets the row threshold.'
                WHEN 'is_10b5_1_heavy_sale' THEN 'True when at least half of daily sale value is reported under 10b5-1.'
                WHEN 'source_transaction_ids_json' THEN 'JSON array of insider_transaction ids used by the trailing-window metrics.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) LIKE '%date' OR lower(c.column_name) = 'as_of_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%ratio%' OR lower(c.column_name) LIKE '%pct%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%value%' THEN 'usd'
                WHEN lower(c.column_name) LIKE '%shares%' OR lower(c.column_name) LIKE '%count%' OR lower(c.column_name) LIKE '%days%' THEN 'count'
                ELSE NULL
            END,
            NULL,
            now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'insider_transaction_metrics'
        """
    )


def _form144_reconciliation(conn: duckdb.DuckDBPyConnection) -> None:
    """S30: Form 144 intent-to-sell ingestion and Form 4 reconciliation metadata."""

    for statement in (
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS seller_name_norm VARCHAR",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS seller_cik VARCHAR",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS issuer_trading_symbol VARCHAR",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS security_title VARCHAR",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS notice_date DATE",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS approx_price_per_share DOUBLE",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS sale_window_end_date DATE",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS rule_10b5_1_indicator BOOLEAN",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS plan_adoption_date DATE",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS restatement_seq INTEGER",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS is_latest BOOLEAN",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS as_of_date DATE",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS is_amendment BOOLEAN",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS remarks VARCHAR",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS source_file VARCHAR",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS source_file_sha256 VARCHAR",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS raw_payload_json VARCHAR",
        "ALTER TABLE form144_intent ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS security_id VARCHAR",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS insider_id VARCHAR",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS issuer_cik VARCHAR",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS seller_cik VARCHAR",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS intent_notice_date DATE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS approx_sale_date DATE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS transaction_date DATE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS days_between INTEGER",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS shares_proposed DOUBLE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS transaction_shares DOUBLE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS execution_ratio DOUBLE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS as_of_date DATE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS match_method VARCHAR",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS match_status VARCHAR",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS shares_matched DOUBLE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS value_matched DOUBLE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS share_match_ratio DOUBLE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS sale_date DATE",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS available_at TIMESTAMP",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS details_json VARCHAR",
        "ALTER TABLE form144_to_form4_link ADD COLUMN IF NOT EXISTS updated_at TIMESTAMP",
        "CREATE INDEX IF NOT EXISTS idx_form144_intent_seller_security ON form144_intent(seller_name_norm, security_id, approx_sale_date)",
        "CREATE INDEX IF NOT EXISTS idx_form144_intent_seller_date ON form144_intent(seller_cik, security_id, approx_sale_date)",
        "CREATE INDEX IF NOT EXISTS idx_form144_intent_latest ON form144_intent(source, is_latest, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_form144_intent_asof ON form144_intent(filing_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_form144_intent_pit ON form144_intent(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_form144_link_intent ON form144_to_form4_link(form144_filing_id)",
        "CREATE INDEX IF NOT EXISTS idx_form144_link_transaction ON form144_to_form4_link(insider_transaction_id)",
        "CREATE INDEX IF NOT EXISTS idx_form144_link_asof ON form144_to_form4_link(available_at)",
        "CREATE INDEX IF NOT EXISTS idx_form144_link_pit ON form144_to_form4_link(as_of_date, available_at)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        UPDATE form144_intent
        SET restatement_seq = coalesce(restatement_seq, 0),
            is_latest = coalesce(is_latest, true),
            is_amendment = coalesce(is_amendment, false),
            as_of_date = coalesce(as_of_date, notice_date, filing_date, approx_sale_date),
            updated_at = coalesce(updated_at, now())
        """
    )
    conn.execute(
        """
        UPDATE form144_to_form4_link
        SET as_of_date = coalesce(as_of_date, sale_date, transaction_date),
            updated_at = coalesce(updated_at, now())
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES
            (
                'form144_intent',
                'sec_edgar',
                'SEC Form 144 intent-to-sell filings',
                'Restricted-stock and affiliate sale intent filings, injected from Form 144 XML or normalized CSV with source-file lineage and bitemporal availability.',
                'accession_number',
                'form144_intent', 'filing_date', 'available_at', now()
            ),
            (
                'form144_to_form4_link',
                'sec_edgar',
                'Form 144 to Form 4 execution reconciliation',
                'Conservative links from Form 144 sale intentions to subsequent Section 16 Form 4 sale transactions, with share/value match diagnostics and PIT availability.',
                'form144_filing_id,insider_transaction_id',
                'form144_to_form4_link', 'sale_date', 'available_at', now()
            ),
            (
                'sec_form144_intent',
                'sec_edgar',
                'SEC Form 144 sale intent',
                'Offline/injectable Form 144 notices of proposed sale plus reconciliation links to visible Form 4 sale transactions.',
                'accession_number; form144_filing_id,insider_transaction_id',
                'form144_intent', 'as_of_date', 'available_at', now()
            )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'form144_intent', 'bronze', 'form144_intent',
                'accession_number',
                'SEC Form 144 forward-looking restricted/control-security sale intent rows.',
                '["filing_id"]',
                'Use as_of_date and available_at for intent visibility; sale_window_end_date is approximate sale date plus the Rule 144 three-month window.',
                now()
            ),
            (
                'form144_to_form4_link', 'silver', 'form144_reconciliation',
                'form144_filing_id,insider_transaction_id',
                'Reconciliation links from Form 144 intent rows to subsequent Form 4 sale transactions.',
                '["form144_filing_id","insider_transaction_id"]',
                'as_of_date and available_at are the later visible source dates, preventing intent/execution matches before either filing is public.',
                now()
            )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('source', 'accession_number', 'issuer_cik', 'issuer_trading_symbol') THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%date' OR lower(c.column_name) = 'sale_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'seller_name_norm' THEN 'Normalized seller name used for conservative matching to Section 16 insiders.'
                WHEN 'sale_window_end_date' THEN 'Approximate end of the Rule 144 three-month sale window.'
                WHEN 'approx_price_per_share' THEN 'Aggregate market value divided by proposed shares when both are present.'
                WHEN 'source_file_sha256' THEN 'SHA-256 hash of the injected Form 144 XML/CSV source file.'
                WHEN 'match_confidence' THEN 'Deterministic confidence score for the Form 144 to Form 4 candidate link.'
                WHEN 'match_method' THEN 'Matching basis, such as insider_id_security_window or seller_name_security_window.'
                WHEN 'match_status' THEN 'Per-link share completion status: FULL, PARTIAL, EXCESS, or UNKNOWN.'
                WHEN 'share_match_ratio' THEN 'Matched Form 4 sale shares divided by the Form 144 proposed-share count.'
                WHEN 'details_json' THEN 'JSON diagnostics explaining the link, dates, and share/value comparison.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) LIKE '%date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%ratio%' OR lower(c.column_name) LIKE '%confidence%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%value%' OR lower(c.column_name) LIKE '%price%' THEN 'usd'
                WHEN lower(c.column_name) LIKE '%shares%' OR lower(c.column_name) LIKE '%count%' THEN 'count'
                ELSE NULL
            END,
            NULL,
            now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name IN ('form144_intent', 'form144_to_form4_link')
        """
    )

def _corporate_action_dividend_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S18: derived cash-dividend analytics (corporate actions x pricing).

    One row per (security_id, ex_date) cash-dividend event from ``corporate_actions``,
    enriched with the ex-date close from ``equity_daily_bars`` to derive spot and
    trailing-twelve-month dividend yield, the TTM dividend sum/count, and the
    year-over-year dividend growth. Bitemporal: ``as_of_date`` is the ex-date,
    ``available_at`` the later of the dividend-inference and bar availabilities. Every
    value uses only this security's current and earlier dividends/bars — no forward
    leakage.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS corporate_action_dividend_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            ex_date DATE NOT NULL,
            record_date DATE,
            payable_date DATE,
            cash_amount DOUBLE,
            close_on_ex DOUBLE,
            dividend_yield_spot DOUBLE,
            ttm_dividend DOUBLE,
            ttm_dividend_count INTEGER,
            dividend_yield_ttm DOUBLE,
            dividend_growth_yoy DOUBLE,
            dividend_ordinal INTEGER,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_corp_div_metrics_key ON corporate_action_dividend_metrics(security_id, ex_date)",
        "CREATE INDEX IF NOT EXISTS idx_corp_div_metrics_exdate ON corporate_action_dividend_metrics(ex_date)",
        "CREATE INDEX IF NOT EXISTS idx_corp_div_metrics_asof ON corporate_action_dividend_metrics(as_of_date, available_at)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'corporate_action_dividend_metrics',
            'corporate_actions',
            'Derived cash-dividend analytics',
            'Per cash-dividend event analytics derived from corporate_actions joined to the ex-date close in equity_daily_bars: spot and trailing-twelve-month dividend yield, TTM dividend sum/count, and year-over-year dividend growth; bitemporal.',
            'security_id,ex_date',
            'corporate_action_dividend_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'corporate_action_dividend_metrics', 'silver', 'corporate_action_dividend_metrics',
            'security_id,ex_date',
            'Derived per-dividend-event metrics (one row per security, ex-date).',
            '["metric_id"]',
            'Resolve with available_at <= query ts and is_latest_revision. TTM/growth windows use only current and earlier dividends; yield uses the ex-date close.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('metric_id', 'security_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('ex_date', 'record_date', 'payable_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'cash_amount' THEN 'Cash dividend per share at the ex-date.'
                WHEN 'close_on_ex' THEN 'Unadjusted close from equity_daily_bars on the ex-date.'
                WHEN 'dividend_yield_spot' THEN 'Single-payment yield: cash_amount / ex-date close.'
                WHEN 'ttm_dividend' THEN 'Sum of cash dividends over the trailing 365 days (inclusive of this one).'
                WHEN 'ttm_dividend_count' THEN 'Number of cash dividends in the trailing 365 days.'
                WHEN 'dividend_yield_ttm' THEN 'Trailing-twelve-month dividend yield: ttm_dividend / ex-date close.'
                WHEN 'dividend_growth_yoy' THEN 'Growth of this dividend vs the nearest dividend ~1 year earlier ((cur - prior)/prior).'
                WHEN 'dividend_ordinal' THEN 'Sequential index of this dividend within the security (1 = earliest observed).'
                WHEN 'available_at' THEN 'Later of the dividend-inference and ex-date bar availability; knowable only at/after this instant.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('ex_date', 'record_date', 'payable_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%yield%' OR lower(c.column_name) LIKE '%growth%' THEN 'ratio'
                WHEN lower(c.column_name) IN ('cash_amount', 'close_on_ex', 'ttm_dividend') THEN 'currency'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'corporate_action_dividend_metrics'
        """
    )


def _corporate_action_split_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S19: split-event factor reconciliation against daily adjustment factors.

    One row per normalized split event from ``adjustment_factor_history`` and daily
    bar source. The table records the adjacent pre/post daily adjustment-factor
    rows and verifies that the daily split-factor step equals the event-level
    price/share factors, preserving missing daily coverage as an explicit status.
    """

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS corporate_action_split_metrics (
            split_metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            factor_source VARCHAR NOT NULL,
            daily_adjustment_source VARCHAR NOT NULL,
            bar_source VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            ex_date DATE NOT NULL,
            event_ref_id VARCHAR NOT NULL,
            source_action_source VARCHAR,
            classification_reason VARCHAR,
            factor_price DOUBLE NOT NULL,
            factor_shares DOUBLE NOT NULL,
            ratio_numerator DOUBLE,
            ratio_denominator DOUBLE,
            split_ratio DOUBLE,
            pre_trade_date DATE,
            post_trade_date DATE,
            pre_raw_close DOUBLE,
            post_raw_close DOUBLE,
            pre_split_adjusted_close DOUBLE,
            post_split_adjusted_close DOUBLE,
            raw_close_return DOUBLE,
            split_adjusted_return DOUBLE,
            pre_split_price_factor DOUBLE,
            post_split_price_factor DOUBLE,
            observed_factor_price_step DOUBLE,
            factor_price_error DOUBLE,
            pre_split_share_factor DOUBLE,
            post_split_share_factor DOUBLE,
            observed_factor_share_step DOUBLE,
            factor_share_error DOUBLE,
            reconciliation_status VARCHAR NOT NULL,
            is_reconciled BOOLEAN NOT NULL DEFAULT false,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_corp_split_metrics_key ON corporate_action_split_metrics(security_id, ex_date)",
        "CREATE INDEX IF NOT EXISTS idx_corp_split_metrics_event ON corporate_action_split_metrics(event_ref_id)",
        "CREATE INDEX IF NOT EXISTS idx_corp_split_metrics_asof ON corporate_action_split_metrics(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_corp_split_metrics_status ON corporate_action_split_metrics(reconciliation_status)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'corporate_action_split_metrics',
            'atx_warehouse',
            'Derived split adjustment reconciliation',
            'Per split-event analytics that reconcile normalized adjustment_factor_history split factors against adjacent daily_adjustment_factors rows and expose raw vs split-adjusted event-window returns.',
            'security_id,ex_date,event_ref_id,bar_source',
            'corporate_action_split_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'corporate_action_split_metrics', 'silver', 'corporate_action_split_metric',
            'security_id,ex_date,event_ref_id,bar_source',
            'Derived per-split-event adjustment-factor reconciliation metrics.',
            '["split_metric_id"]',
            'Resolve with available_at <= query ts and is_latest_revision. Missing daily rows are preserved with reconciliation_status=MISSING_DAILY_FACTOR; mismatches are quality failures.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('event_ref_id', 'security_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('ex_date', 'pre_trade_date', 'post_trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'factor_price' THEN 'Event-level split price factor from adjustment_factor_history.'
                WHEN 'factor_shares' THEN 'Event-level split share factor from adjustment_factor_history.'
                WHEN 'split_ratio' THEN 'Split ratio expressed as post-split shares per pre-split share.'
                WHEN 'pre_trade_date' THEN 'Nearest trading date before the split ex-date used for daily-factor reconciliation.'
                WHEN 'post_trade_date' THEN 'First trading date on or after the split ex-date used for daily-factor reconciliation.'
                WHEN 'observed_factor_price_step' THEN 'Observed daily split price-factor step: pre_split_price_factor / post_split_price_factor.'
                WHEN 'observed_factor_share_step' THEN 'Observed daily split share-factor step: pre_split_share_factor / post_split_share_factor.'
                WHEN 'factor_price_error' THEN 'Observed minus event-level split price factor.'
                WHEN 'factor_share_error' THEN 'Observed minus event-level split share factor.'
                WHEN 'raw_close_return' THEN 'Raw close return across the split boundary (post_raw_close / pre_raw_close - 1).'
                WHEN 'split_adjusted_return' THEN 'Split-adjusted close return across the split boundary.'
                WHEN 'reconciliation_status' THEN 'RECONCILED, MISMATCH, or MISSING_DAILY_FACTOR.'
                WHEN 'available_at' THEN 'Max availability of the split event and adjacent daily factor rows used for reconciliation.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('ex_date', 'pre_trade_date', 'post_trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%return%' OR lower(c.column_name) LIKE '%error%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%factor%' OR lower(c.column_name) LIKE '%ratio%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%close%' THEN 'currency'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'corporate_action_split_metrics'
        """
    )


def _corporate_action_factor_reconciliation(conn: duckdb.DuckDBPyConnection) -> None:
    """S21: event-level factor reconciliation across all adjustment-factor events."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS corporate_action_factor_reconciliation (
            reconciliation_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            factor_source VARCHAR NOT NULL,
            daily_adjustment_source VARCHAR NOT NULL,
            bar_source VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            ex_date DATE NOT NULL,
            event_type VARCHAR NOT NULL,
            type_code INTEGER,
            event_ref_id VARCHAR NOT NULL,
            source_action_source VARCHAR,
            classification_reason VARCHAR,
            factor_price DOUBLE NOT NULL,
            factor_shares DOUBLE NOT NULL,
            factor_volume DOUBLE,
            cash_div_amount DOUBLE,
            ratio_numerator DOUBLE,
            ratio_denominator DOUBLE,
            same_day_event_count INTEGER NOT NULL DEFAULT 1,
            pre_trade_date DATE,
            post_trade_date DATE,
            pre_raw_close DOUBLE,
            post_raw_close DOUBLE,
            raw_close_return DOUBLE,
            pre_split_adjusted_close DOUBLE,
            post_split_adjusted_close DOUBLE,
            split_adjusted_return DOUBLE,
            pre_total_return_adjusted_close DOUBLE,
            post_total_return_adjusted_close DOUBLE,
            total_return_adjusted_return DOUBLE,
            pre_split_price_factor DOUBLE,
            post_split_price_factor DOUBLE,
            observed_split_price_step DOUBLE,
            expected_split_price_step DOUBLE,
            split_price_error DOUBLE,
            pre_split_share_factor DOUBLE,
            post_split_share_factor DOUBLE,
            observed_split_share_step DOUBLE,
            expected_split_share_step DOUBLE,
            split_share_error DOUBLE,
            pre_dividend_total_return_factor DOUBLE,
            post_dividend_total_return_factor DOUBLE,
            observed_dividend_return_step DOUBLE,
            expected_dividend_return_step DOUBLE,
            dividend_return_error DOUBLE,
            pre_total_return_price_factor DOUBLE,
            post_total_return_price_factor DOUBLE,
            observed_total_return_step DOUBLE,
            expected_total_return_step DOUBLE,
            total_return_error DOUBLE,
            reconciliation_status VARCHAR NOT NULL,
            is_reconciled BOOLEAN NOT NULL DEFAULT false,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_corp_factor_recon_key ON corporate_action_factor_reconciliation(security_id, ex_date)",
        "CREATE INDEX IF NOT EXISTS idx_corp_factor_recon_type_status ON corporate_action_factor_reconciliation(event_type, reconciliation_status)",
        "CREATE INDEX IF NOT EXISTS idx_corp_factor_recon_asof ON corporate_action_factor_reconciliation(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_corp_factor_recon_event ON corporate_action_factor_reconciliation(event_ref_id)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'corporate_action_factor_reconciliation',
            'atx_warehouse',
            'Derived corporate-action factor reconciliation',
            'Per event and bar-source controls that reconcile adjustment_factor_history event factors against adjacent daily_adjustment_factors split, dividend, and total-return factor steps.',
            'security_id,ex_date,event_ref_id,event_type,bar_source',
            'corporate_action_factor_reconciliation', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'corporate_action_factor_reconciliation', 'silver', 'corporate_action_factor_reconciliation',
            'security_id,ex_date,event_ref_id,event_type,bar_source',
            'Derived event-level adjustment-factor reconciliation controls spanning splits, cash dividends, unsupported event types, missing daily-factor coverage, and compound same-day events.',
            '["reconciliation_id"]',
            'Resolve with available_at <= query ts and is_latest_revision. Status values distinguish reconciled events from missing daily factors, compound same-day events, unsupported event types, and true mismatches.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('event_ref_id', 'security_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('ex_date', 'pre_trade_date', 'post_trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'event_type' THEN 'Adjustment-factor event type from adjustment_factor_history.'
                WHEN 'same_day_event_count' THEN 'Number of adjustment events sharing security_id and ex_date; values above one are compound events.'
                WHEN 'observed_split_price_step' THEN 'Observed daily split price-factor step: pre_split_price_factor / post_split_price_factor.'
                WHEN 'observed_split_share_step' THEN 'Observed daily split share-factor step: pre_split_share_factor / post_split_share_factor.'
                WHEN 'observed_dividend_return_step' THEN 'Observed daily dividend total-return factor step.'
                WHEN 'observed_total_return_step' THEN 'Observed daily total-return price-factor step across the event boundary.'
                WHEN 'expected_split_price_step' THEN 'Expected split price-factor step from the event factor; neutral for non-split rows.'
                WHEN 'expected_dividend_return_step' THEN 'Expected dividend total-return step from the event factor; neutral for non-cash-dividend rows.'
                WHEN 'expected_total_return_step' THEN 'Expected total-return price-factor step for supported price-affecting event types.'
                WHEN 'reconciliation_status' THEN 'RECONCILED, MISMATCH, MISSING_DAILY_FACTOR, COMPOUND_EVENT, or UNSUPPORTED_EVENT_TYPE.'
                WHEN 'available_at' THEN 'Max availability of the event and adjacent daily factor rows used for reconciliation.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('ex_date', 'pre_trade_date', 'post_trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%return%' OR lower(c.column_name) LIKE '%error%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%factor%' OR lower(c.column_name) LIKE '%ratio%' OR lower(c.column_name) LIKE '%step%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%close%' OR lower(c.column_name) LIKE '%amount%' THEN 'currency'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'corporate_action_factor_reconciliation'
        """
    )


def _thirteenf_option_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S20: issuer-level 13F option-positioning analytics."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_option_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR,
            symbol VARCHAR,
            cusip VARCHAR,
            name_of_issuer VARCHAR,
            report_period DATE NOT NULL,
            source_period VARCHAR,
            filing_date DATE,
            filing_count BIGINT NOT NULL,
            option_manager_count BIGINT NOT NULL,
            call_manager_count BIGINT NOT NULL,
            put_manager_count BIGINT NOT NULL,
            option_position_count BIGINT NOT NULL,
            call_position_count BIGINT NOT NULL,
            put_position_count BIGINT NOT NULL,
            call_share_quantity DOUBLE,
            put_share_quantity DOUBLE,
            net_call_share_quantity DOUBLE,
            put_call_share_ratio DOUBLE,
            call_value_usd DOUBLE,
            put_value_usd DOUBLE,
            net_call_value_usd DOUBLE,
            put_call_value_ratio DOUBLE,
            option_value_usd DOUBLE,
            common_share_quantity DOUBLE,
            common_value_usd DOUBLE,
            call_to_common_share_pct DOUBLE,
            put_to_common_share_pct DOUBLE,
            option_to_common_value_pct DOUBLE,
            avg_option_portfolio_weight DOUBLE,
            max_option_portfolio_weight DOUBLE,
            top_call_manager_id VARCHAR,
            top_call_manager_value_usd DOUBLE,
            top_put_manager_id VARCHAR,
            top_put_manager_value_usd DOUBLE,
            option_bias VARCHAR NOT NULL,
            prior_report_period DATE,
            prior_net_call_share_quantity DOUBLE,
            net_call_share_change DOUBLE,
            net_call_share_change_pct DOUBLE,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_option_metrics_key ON thirteenf_option_metrics(security_id, cusip, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_option_metrics_asof ON thirteenf_option_metrics(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_option_metrics_bias ON thirteenf_option_metrics(option_bias)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'thirteenf_option_metrics',
            'sec_edgar',
            'Derived 13F issuer-level option positioning',
            'Issuer/report-period aggregate call and put option positioning from cached SEC Form 13F holdings, including put/call ratios, common-share denominators, top managers, and QoQ net-call flow.',
            'security_id,cusip,report_period,source_period',
            'thirteenf_option_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'thirteenf_option_metrics', 'gold', '13f_option_positioning',
            'security_id,cusip,report_period,source_period',
            'Derived issuer-level 13F call/put option-positioning metrics.',
            '["metric_id"]',
            'Use report_period/as_of_date plus available_at for PIT-safe ownership-option signals; rows reflect currently loaded 13F holdings coverage.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('cusip', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('report_period', 'filing_date', 'prior_report_period', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'option_manager_count' THEN 'Distinct 13F managers reporting call or put option rows for the issuer/report period.'
                WHEN 'call_share_quantity' THEN 'Aggregate reported call-option share-equivalent quantity.'
                WHEN 'put_share_quantity' THEN 'Aggregate reported put-option share-equivalent quantity.'
                WHEN 'net_call_share_quantity' THEN 'Call share-equivalent quantity minus put share-equivalent quantity.'
                WHEN 'put_call_share_ratio' THEN 'Put/call ratio by share-equivalent quantity.'
                WHEN 'call_value_usd' THEN 'Aggregate reported value of 13F call-option rows.'
                WHEN 'put_value_usd' THEN 'Aggregate reported value of 13F put-option rows.'
                WHEN 'net_call_value_usd' THEN 'Call reported value minus put reported value.'
                WHEN 'put_call_value_ratio' THEN 'Put/call ratio by reported value.'
                WHEN 'call_to_common_share_pct' THEN 'Call share-equivalent quantity divided by aggregate common shares reported by 13F managers.'
                WHEN 'put_to_common_share_pct' THEN 'Put share-equivalent quantity divided by aggregate common shares reported by 13F managers.'
                WHEN 'option_to_common_value_pct' THEN 'Call plus put reported value divided by aggregate common-share value.'
                WHEN 'option_bias' THEN 'CALL_HEAVY, PUT_HEAVY, BALANCED, or NO_OPTIONS based on share-equivalent call versus put totals.'
                WHEN 'net_call_share_change' THEN 'Quarter-over-quarter change in net call share-equivalent quantity.'
                WHEN 'available_at' THEN 'Latest filing availability among the visible 13F rows used for the aggregate.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('report_period', 'filing_date', 'prior_report_period', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%ratio%' OR lower(c.column_name) LIKE '%pct%' OR lower(c.column_name) LIKE '%change_pct%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%value_usd%' THEN 'usd'
                WHEN lower(c.column_name) LIKE '%share%' OR lower(c.column_name) LIKE '%count%' THEN 'count'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'thirteenf_option_metrics'
        """
    )


def _thirteenf_concentration_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S22: issuer-level 13F holder-concentration analytics."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_concentration_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR,
            symbol VARCHAR,
            cusip VARCHAR,
            name_of_issuer VARCHAR,
            report_period DATE NOT NULL,
            source_period VARCHAR,
            filing_date DATE,
            filing_count BIGINT NOT NULL,
            holder_count BIGINT NOT NULL,
            common_value_usd DOUBLE,
            common_share_quantity DOUBLE,
            top_holder_manager_id VARCHAR,
            top_holder_value_usd DOUBLE,
            top_holder_share_quantity DOUBLE,
            top_holder_value_pct DOUBLE,
            top_holder_share_pct DOUBLE,
            top_holder_portfolio_weight DOUBLE,
            top_3_holder_value_usd DOUBLE,
            top_3_holder_value_pct DOUBLE,
            top_5_holder_value_usd DOUBLE,
            top_5_holder_value_pct DOUBLE,
            top_10_holder_value_usd DOUBLE,
            top_10_holder_value_pct DOUBLE,
            value_hhi DOUBLE,
            share_hhi DOUBLE,
            effective_holder_count_value DOUBLE,
            effective_holder_count_share DOUBLE,
            avg_holder_value_usd DOUBLE,
            median_holder_value_usd DOUBLE,
            concentration_bucket VARCHAR NOT NULL,
            prior_report_period DATE,
            prior_value_hhi DOUBLE,
            value_hhi_change DOUBLE,
            prior_holder_count BIGINT,
            holder_count_change BIGINT,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_concentration_metrics_key ON thirteenf_concentration_metrics(security_id, cusip, report_period)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_concentration_metrics_asof ON thirteenf_concentration_metrics(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_concentration_metrics_bucket ON thirteenf_concentration_metrics(concentration_bucket)",
        "CREATE INDEX IF NOT EXISTS idx_thirteenf_concentration_metrics_top_holder ON thirteenf_concentration_metrics(top_holder_manager_id)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'thirteenf_concentration_metrics',
            'sec_edgar',
            'Derived 13F issuer-level holder concentration',
            'Issuer/report-period common-share ownership concentration from cached SEC Form 13F holdings, including top-holder shares, top-3/5/10 concentration, HHI, effective holder count, and QoQ concentration changes.',
            'security_id,cusip,report_period,source_period',
            'thirteenf_concentration_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'thirteenf_concentration_metrics', 'gold', '13f_holder_concentration',
            'security_id,cusip,report_period,source_period',
            'Derived issuer-level 13F common-share holder-concentration metrics.',
            '["metric_id"]',
            'Use report_period/as_of_date plus available_at for PIT-safe ownership-concentration signals; rows reflect currently loaded common-share 13F holdings coverage.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name, c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('cusip', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('report_period', 'filing_date', 'prior_report_period', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'holder_count' THEN 'Distinct 13F managers reporting common-share rows for the issuer/report period.'
                WHEN 'common_value_usd' THEN 'Aggregate reported common-share market value in dollars.'
                WHEN 'common_share_quantity' THEN 'Aggregate reported common-share quantity.'
                WHEN 'top_holder_manager_id' THEN 'Manager with the largest reported common-share value for the issuer/report period.'
                WHEN 'top_holder_value_pct' THEN 'Top holder reported common-share value divided by aggregate common-share value.'
                WHEN 'top_3_holder_value_pct' THEN 'Top three holders reported common-share value divided by aggregate common-share value.'
                WHEN 'top_5_holder_value_pct' THEN 'Top five holders reported common-share value divided by aggregate common-share value.'
                WHEN 'top_10_holder_value_pct' THEN 'Top ten holders reported common-share value divided by aggregate common-share value.'
                WHEN 'value_hhi' THEN 'Herfindahl-Hirschman index of manager common-share value weights.'
                WHEN 'share_hhi' THEN 'Herfindahl-Hirschman index of manager common-share quantity weights.'
                WHEN 'effective_holder_count_value' THEN 'Inverse value HHI: the equally weighted holder count implied by the value concentration.'
                WHEN 'concentration_bucket' THEN 'DISPERSED, MODERATE, CONCENTRATED, or HIGHLY_CONCENTRATED based on value HHI and top-five value share.'
                WHEN 'value_hhi_change' THEN 'Quarter-over-quarter change in value HHI for the issuer.'
                WHEN 'holder_count_change' THEN 'Quarter-over-quarter change in visible common-share 13F holder count for the issuer.'
                WHEN 'available_at' THEN 'Latest filing availability among the visible 13F common-share rows used for the aggregate.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('report_period', 'filing_date', 'prior_report_period', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%pct%' OR lower(c.column_name) LIKE '%hhi%' OR lower(c.column_name) LIKE '%change%' THEN 'ratio'
                WHEN lower(c.column_name) LIKE '%value_usd%' THEN 'usd'
                WHEN lower(c.column_name) LIKE '%share_quantity%' OR lower(c.column_name) LIKE '%count%' THEN 'count'
                ELSE NULL
            END,
            NULL, now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'thirteenf_concentration_metrics'
        """
    )


def _security_listing_metrics(conn: duckdb.DuckDBPyConnection) -> None:
    """S31: derived security listing / reference-compliance metrics."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS security_listing_metrics (
            metric_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            input_listing_source VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            directory VARCHAR,
            listing_status VARCHAR,
            listing_venue_code VARCHAR,
            listing_venue_name VARCHAR,
            listing_exchange_code VARCHAR,
            listing_exchange_name VARCHAR,
            market_category VARCHAR,
            market_tier VARCHAR,
            security_name VARCHAR,
            round_lot_size INTEGER,
            is_etf BOOLEAN,
            is_test_issue BOOLEAN,
            is_next_shares BOOLEAN,
            financial_status_code VARCHAR,
            financial_status_label VARCHAR,
            has_financial_status BOOLEAN NOT NULL DEFAULT false,
            is_listing_compliant BOOLEAN NOT NULL DEFAULT false,
            is_deficient BOOLEAN NOT NULL DEFAULT false,
            is_delinquent BOOLEAN NOT NULL DEFAULT false,
            is_bankrupt BOOLEAN NOT NULL DEFAULT false,
            is_noncompliant BOOLEAN NOT NULL DEFAULT false,
            restatement_seq INTEGER NOT NULL DEFAULT 0,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_security_listing_metrics_key ON security_listing_metrics(source, security_id, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_security_listing_metrics_symbol ON security_listing_metrics(symbol, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_security_listing_metrics_asof ON security_listing_metrics(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_security_listing_metrics_latest ON security_listing_metrics(source, is_latest_revision, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_security_listing_metrics_noncompliant ON security_listing_metrics(is_noncompliant, as_of_date)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'security_listing_metrics',
            'nasdaq_trader',
            'Security listing reference metrics',
            'Derived per-security listing reference surface joining listing_status_intervals (CIK-resolved venue/exchange/status) to the Nasdaq Trader symbol directory; decodes the Nasdaq financial_status listing-compliance code into deficiency/delinquency/bankruptcy flags plus ETF/test-issue/market-tier classification.',
            'source,security_id,as_of_date',
            'security_listing_metrics', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'security_listing_metrics', 'gold', 'security',
            'source,security_id,as_of_date,available_at',
            'Per-snapshot security listing / reference-compliance surface derived from listing_status_intervals and nasdaq_symbol_directory.',
            '["metric_id"]',
            'Resolve with available_at <= query timestamp and is_latest_revision, then pick the latest as_of_date snapshot at/under the query date. financial_status flips between snapshots create new later-dated rows rather than mutating history.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%id' OR lower(c.column_name) IN ('source', 'input_listing_source', 'symbol', 'directory', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%date' OR lower(c.column_name) = 'as_of_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR lower(c.column_name) LIKE 'has_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'as_of_date' THEN 'Nasdaq symbol-directory snapshot date the reference row was observed on.'
                WHEN 'directory' THEN 'Nasdaq directory file the symbol was sourced from (nasdaqlisted or otherlisted).'
                WHEN 'listing_status' THEN 'Listing spine status (active or inactive) for the security.'
                WHEN 'market_tier' THEN 'Decoded Nasdaq market tier (Global Select / Global Market / Capital Market).'
                WHEN 'financial_status_code' THEN 'Raw Nasdaq financial_status letter (N/D/E/Q and combined G/H/J/K).'
                WHEN 'financial_status_label' THEN 'Human-readable expansion of the financial_status code.'
                WHEN 'has_financial_status' THEN 'True when a Nasdaq financial_status code is present (nasdaqlisted securities).'
                WHEN 'is_listing_compliant' THEN 'True when financial_status is N (Normal / compliant).'
                WHEN 'is_deficient' THEN 'True when financial_status indicates a continued-listing deficiency (D/G/H/K).'
                WHEN 'is_delinquent' THEN 'True when financial_status indicates a filing delinquency (E/H/J/K).'
                WHEN 'is_bankrupt' THEN 'True when financial_status indicates bankruptcy (Q/G/J/K).'
                WHEN 'is_noncompliant' THEN 'True when a financial_status code is present and is not Normal.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) LIKE '%date' OR lower(c.column_name) = 'as_of_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%lot_size%' THEN 'shares'
                ELSE NULL
            END,
            NULL,
            now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'security_listing_metrics'
        """
    )


def _equity_price_metrics_liquidity(conn: duckdb.DuckDBPyConnection) -> None:
    """S35: ADV and Amihud illiquidity liquidity factors on equity_price_metrics."""
    for statement in (
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS avg_dollar_volume_21d DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS amihud_illiquidity_21d DOUBLE",
    ):
        conn.execute(statement)
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('equity_price_metrics', 'avg_dollar_volume_21d', 'measure',
             'Trailing 21-trading-day average daily dollar volume (close x volume); a liquidity/capacity measure.',
             true, 'usd', NULL, now()),
            ('equity_price_metrics', 'amihud_illiquidity_21d', 'measure',
             'Amihud (2002) illiquidity: trailing 21-day mean of abs(daily_return)/dollar_volume, scaled by 1e9 (price impact per $1B traded). Higher = more illiquid.',
             true, 'ratio', NULL, now())
        """
    )


def _equity_price_metrics_risk(conn: duckdb.DuckDBPyConnection) -> None:
    """S36: trailing max drawdown and downside deviation on equity_price_metrics."""
    for statement in (
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS max_drawdown_126d DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS downside_deviation_60d DOUBLE",
    ):
        conn.execute(statement)
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('equity_price_metrics', 'max_drawdown_126d', 'measure',
             'Trailing 126-trading-day maximum drawdown: the most negative (price / running expanding peak - 1) observed in the window. <= 0.',
             true, 'ratio', NULL, now()),
            ('equity_price_metrics', 'downside_deviation_60d', 'measure',
             'Trailing 60-day annualized downside deviation (Sortino denominator, MAR=0): sqrt(mean(min(daily_return,0)^2)) x sqrt(252).',
             true, 'ratio', NULL, now())
        """
    )


def _equity_price_metrics_market_relative(conn: duckdb.DuckDBPyConnection) -> None:
    """S37: market-relative one-factor risk diagnostics on equity_price_metrics."""
    for statement in (
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS market_return_ew DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS beta_60d DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS market_correlation_60d DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS idiosyncratic_vol_60d DOUBLE",
    ):
        conn.execute(statement)
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('equity_price_metrics', 'market_return_ew', 'measure',
             'Same-day equal-weight market proxy return computed from the loaded daily-bar universe; used for market-relative risk factors.',
             true, 'ratio', NULL, now()),
            ('equity_price_metrics', 'beta_60d', 'measure',
             'Trailing 60-trading-day beta of the security daily return versus the equal-weight market proxy; NULL when the proxy variance is zero or missing.',
             true, 'ratio', NULL, now()),
            ('equity_price_metrics', 'market_correlation_60d', 'measure',
             'Trailing 60-trading-day correlation of the security daily return versus the equal-weight market proxy, bounded to [-1, 1].',
             true, 'ratio', NULL, now()),
            ('equity_price_metrics', 'idiosyncratic_vol_60d', 'measure',
             'Trailing 60-trading-day annualized residual volatility from a one-factor market model against the equal-weight market proxy.',
             true, 'ratio', NULL, now())
        """
    )


def _equity_price_metrics_cross_sectional_ranks(conn: duckdb.DuckDBPyConnection) -> None:
    """S38: same-day cross-sectional percentile ranks on equity_price_metrics."""
    for statement in (
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS daily_return_cs_pct_rank DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS momentum_21d_cs_pct_rank DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS realized_vol_20d_cs_pct_rank DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS dollar_volume_cs_pct_rank DOUBLE",
        "ALTER TABLE equity_price_metrics ADD COLUMN IF NOT EXISTS amihud_illiquidity_21d_cs_pct_rank DOUBLE",
    ):
        conn.execute(statement)
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('equity_price_metrics', 'daily_return_cs_pct_rank', 'measure',
             'Same-trade-date percentile rank of daily_return across the loaded bar universe; higher = stronger same-day return.',
             true, 'percentile', NULL, now()),
            ('equity_price_metrics', 'momentum_21d_cs_pct_rank', 'measure',
             'Same-trade-date percentile rank of trailing 21-day momentum across the loaded bar universe; higher = stronger short-term momentum.',
             true, 'percentile', NULL, now()),
            ('equity_price_metrics', 'realized_vol_20d_cs_pct_rank', 'measure',
             'Same-trade-date percentile rank of 20-day realized volatility across the loaded bar universe; higher = more volatile.',
             true, 'percentile', NULL, now()),
            ('equity_price_metrics', 'dollar_volume_cs_pct_rank', 'measure',
             'Same-trade-date percentile rank of dollar volume across the loaded bar universe; higher = more liquid/capacity.',
             true, 'percentile', NULL, now()),
            ('equity_price_metrics', 'amihud_illiquidity_21d_cs_pct_rank', 'measure',
             'Same-trade-date percentile rank of trailing Amihud illiquidity across the loaded bar universe; higher = more illiquid.',
             true, 'percentile', NULL, now())
        """
    )


def _macro_metrics_sahm_rule_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """S34: catalog the Sahm Rule recession-indicator synthetic macro series."""
    conn.execute(
        """
        UPDATE dataset_catalog
        SET description = 'Per-series macro analytics derived from the cached FRED observation feed (level, change, year-over-year change/growth, expanding z-score, expanding percentile rank) plus synthetic T10Y2Y Treasury term spread, REAL_FEDFUNDS real-rate, and SAHM_RULE recession-indicator series; bitemporal, latest-revision FRED (not ALFRED vintages).',
            updated_at = now()
        WHERE dataset_id = 'macro_metrics'
        """
    )


def _fundamental_item_registry_schema(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S1 S1-0: canonical item dimension + alias + vendor cross-walk schema.

    Creates fundamental_item (one row per canonical metric, item_id 1001..1440
    fundamentals, 1501/1601/1701 industry overlays, 2001..2044 estimates),
    fundamental_item_alias (us-gaap/dei concept -> item_id edges with
    coalesce_priority and bitemporal valid_from/valid_to), and
    fundamental_item_vendor_map (item_id -> vendor field cross-walk). Schema
    only; indexes land in migration 0062, seed data lands in S1-1. No seed
    rows are inserted here; this migration only creates structure and
    catalogs it.
    """
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_item (
            item_id INTEGER PRIMARY KEY,
            canonical_code VARCHAR NOT NULL,
            statement VARCHAR,
            section VARCHAR,
            data_type VARCHAR,
            unit_type VARCHAR,
            sign_convention VARCHAR,
            is_derived BOOLEAN DEFAULT FALSE,
            definition VARCHAR,
            citation VARCHAR
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_item_alias (
            item_id INTEGER,
            alias_scheme VARCHAR,
            alias_code VARCHAR,
            coalesce_priority INTEGER,
            valid_from DATE,
            valid_to DATE
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_item_vendor_map (
            item_id INTEGER,
            vendor VARCHAR,
            vendor_field VARCHAR,
            sign_note VARCHAR
        )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,
            layer,
            entity,
            grain,
            description,
            natural_key_json,
            pit_notes,
            updated_at
        )
        VALUES
            (
                'fundamental_item',
                'gold',
                'fundamental_item',
                'item_id',
                'Canonical cross-vendor fundamental item dimension: one row per governed metric (income/balance/cashflow/derived statement items, industry overlays, estimate items), mirroring the cross_vendor_field_map.md headline dictionary.',
                '["item_id"]',
                'Reference data with no as_of_date fact time; carries no bitemporal window itself.',
                now()
            ),
            (
                'fundamental_item_alias',
                'gold',
                'fundamental_item_alias',
                'item_id,alias_scheme,alias_code,coalesce_priority,valid_from,valid_to',
                'Concept-to-item edges mapping vendor taxonomy concepts (us-gaap/dei) onto a canonical fundamental_item, with COALESCE priority ordering and bitemporal validity.',
                '["item_id", "alias_scheme", "alias_code", "coalesce_priority", "valid_from", "valid_to"]',
                'Bitemporal alias validity via valid_from/valid_to; resolution shim gates alias_scheme/alias_code on valid_from <= as_of_date < coalesce(valid_to, DATE 9999-12-31).',
                now()
            ),
            (
                'fundamental_item_vendor_map',
                'gold',
                'fundamental_item_vendor_map',
                'item_id,vendor,vendor_field',
                'Cross-vendor field cross-walk mapping a canonical fundamental_item to vendor-specific field codes (compustat/factset/ibes/worldscope/bloomberg/ciq/sharadar).',
                '["item_id", "vendor", "vendor_field"]',
                'Reference data with no as_of_date fact time; carries no bitemporal window itself.',
                now()
            )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('fundamental_item', 'item_id', 'identifier',
             'Canonical fundamental item identifier (1001..1440 fundamentals, 1501/1601/1701 industry overlays, 2001..2044 estimates).',
             false, NULL, NULL, now()),
            ('fundamental_item', 'canonical_code', 'text',
             'Canonical metric code (e.g. revenue, total_assets) used as the single source of truth for derived-layer inputs.',
             false, NULL, NULL, now()),
            ('fundamental_item', 'statement', 'text',
             'Owning financial statement (income, balance, cashflow, derived, estimate).',
             true, NULL, NULL, now()),
            ('fundamental_item', 'section', 'text',
             'Statement section/grouping (e.g. section 2.1 income, section 2.5 banks) mirroring cross_vendor_field_map.md.',
             true, NULL, NULL, now()),
            ('fundamental_item', 'data_type', 'text',
             'XBRL duration semantics of the item: flow (duration) or instant.',
             true, NULL, NULL, now()),
            ('fundamental_item', 'unit_type', 'text',
             'Unit family of the item value (e.g. USD, shares, per_share, ratio).',
             true, NULL, NULL, now()),
            ('fundamental_item', 'sign_convention', 'text',
             'Expected sign convention for the item value (e.g. positive, negative_is_expense).',
             true, NULL, NULL, now()),
            ('fundamental_item', 'is_derived', 'flag',
             'True when the item is computed from other items rather than sourced directly from a filing/vendor.',
             true, NULL, NULL, now()),
            ('fundamental_item', 'definition', 'text',
             'Human-readable definition of the canonical item.',
             true, NULL, NULL, now()),
            ('fundamental_item', 'citation', 'text',
             'Source citation for the item definition (e.g. cross_vendor_field_map.md section reference).',
             true, NULL, NULL, now()),
            ('fundamental_item_alias', 'item_id', 'identifier',
             'Canonical fundamental_item this alias resolves to.',
             true, NULL, NULL, now()),
            ('fundamental_item_alias', 'alias_scheme', 'text',
             'Vendor taxonomy scheme of the alias concept (us-gaap or dei).',
             true, NULL, NULL, now()),
            ('fundamental_item_alias', 'alias_code', 'text',
             'Taxonomy concept name within alias_scheme (e.g. Revenues, SalesRevenueNet).',
             true, NULL, NULL, now()),
            ('fundamental_item_alias', 'coalesce_priority', 'measure',
             'COALESCE ordering priority when multiple aliases map to the same item_id (lower resolves first), mirroring fundamental_statement_map.concept_priority.',
             true, NULL, NULL, now()),
            ('fundamental_item_alias', 'valid_from', 'date',
             'Date the alias becomes valid for PIT resolution (bitemporal alias validity).',
             true, NULL, NULL, now()),
            ('fundamental_item_alias', 'valid_to', 'date',
             'Date the alias stops being valid for PIT resolution; NULL means still valid (coalesce to DATE 9999-12-31).',
             true, 'date', NULL, now()),
            ('fundamental_item_vendor_map', 'item_id', 'identifier',
             'Canonical fundamental_item this vendor field maps to.',
             true, NULL, NULL, now()),
            ('fundamental_item_vendor_map', 'vendor', 'text',
             'Vendor identifier (compustat, factset, ibes, worldscope, bloomberg, ciq, sharadar).',
             true, NULL, NULL, now()),
            ('fundamental_item_vendor_map', 'vendor_field', 'text',
             'Vendor-specific field code for the mapped item (e.g. compustat revt, factset FF_SALES).',
             true, NULL, NULL, now()),
            ('fundamental_item_vendor_map', 'sign_note', 'text',
             'Free-text note on vendor sign convention differences relative to the canonical item.',
             true, NULL, NULL, now())
        """
    )


def _fundamental_item_registry_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S1 S1-0: index the item registry after the additive schema migration commits."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_item_alias_lookup ON fundamental_item_alias(alias_scheme, alias_code)",
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_fundamental_item_canonical ON fundamental_item(canonical_code)",
        """
        CREATE UNIQUE INDEX IF NOT EXISTS idx_fundamental_item_alias_unique
        ON fundamental_item_alias (
            (CASE WHEN item_id IS NULL THEN 'N' ELSE 'V' || CAST(item_id AS VARCHAR) END),
            (CASE WHEN alias_scheme IS NULL THEN 'N' ELSE 'V' || alias_scheme END),
            (CASE WHEN alias_code IS NULL THEN 'N' ELSE 'V' || alias_code END),
            (CASE WHEN coalesce_priority IS NULL THEN 'N' ELSE 'V' || CAST(coalesce_priority AS VARCHAR) END),
            (CASE WHEN valid_from IS NULL THEN 'N' ELSE 'V' || CAST(valid_from AS VARCHAR) END),
            (CASE WHEN valid_to IS NULL THEN 'N' ELSE 'V' || CAST(valid_to AS VARCHAR) END)
        )
        """,
        """
        CREATE UNIQUE INDEX IF NOT EXISTS idx_fundamental_item_vendor_map_unique
        ON fundamental_item_vendor_map (
            (CASE WHEN item_id IS NULL THEN 'N' ELSE 'V' || CAST(item_id AS VARCHAR) END),
            (CASE WHEN vendor IS NULL THEN 'N' ELSE 'V' || vendor END),
            (CASE WHEN vendor_field IS NULL THEN 'N' ELSE 'V' || vendor_field END)
        )
        """,
    ):
        conn.execute(statement)


def _fundamental_ratios_input_item_ids(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S1 S1-3: additive item-linkage column on fundamental_ratios.

    Adds ``input_item_ids_json`` (nullable JSON text, mirroring the pre-existing
    ``input_codes_json`` pattern) recording the sorted list of governed
    fundamental_item item_ids a ratio row's inputs resolve to (see
    db.item_registry.ratio_input_item_ids / input_item_ids_for_ratio). This is
    purely additive metadata -- it is not part of ratio_id and carries no bearing
    on any pre-existing column's value (byte-identity of ratio VALUES is
    unaffected). Idempotent (IF NOT EXISTS); catalogs the new column in
    field_catalog in this same migration.
    """
    conn.execute(
        "ALTER TABLE fundamental_ratios ADD COLUMN IF NOT EXISTS input_item_ids_json VARCHAR"
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('fundamental_ratios', 'input_item_ids_json', 'json',
             'Sorted JSON list of governed fundamental_item item_ids the ratio''s inputs resolve to '
             '(db.item_registry governed ratio_input map); null/omitted for documented S1-3 '
             'registry gaps. Additive metadata alongside input_codes_json; not part of ratio_id.',
             true, NULL, NULL, now())
        """
    )


def _fundamental_fact_item_links(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S1 S1-4: additive item_id links on statement and raw XBRL fact tables.

    Adds nullable ``item_id`` columns to ``fundamental_statement_points`` and
    ``fundamental_points``, catalogs both fields, then deterministically backfills
    them from existing governed mappings. Statement points prefer the exact
    statement-map row keyed by source/taxonomy/concept/canonical_metric, which
    preserves industry-template overlays without altering rebuild logic. Raw
    companyfacts points resolve taxonomy/metric through ``fundamental_item_alias``.
    Alias duplicates are intentionally excluded from the backfill; the quality
    path fails them explicitly so no nondeterministic item assignment is made.
    """
    for statement in (
        "ALTER TABLE fundamental_statement_points ADD COLUMN IF NOT EXISTS item_id INTEGER",
        "ALTER TABLE fundamental_points ADD COLUMN IF NOT EXISTS item_id INTEGER",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('fundamental_statement_points', 'item_id', 'identifier',
             'Nullable canonical fundamental_item.item_id resolved from the statement map/alias registry for the fact taxonomy and concept. Additive PF-S1 reference linkage; not part of the statement point key.',
             true, NULL, 'fundamental_statement_map.item_id', now()),
            ('fundamental_points', 'item_id', 'identifier',
             'Nullable canonical fundamental_item.item_id resolved from fundamental_item_alias using taxonomy and raw XBRL metric/concept. Additive PF-S1 reference linkage; not part of any fact key.',
             true, NULL, 'fundamental_item_alias.item_id', now())
        """
    )

    conn.execute(
        """
        UPDATE fundamental_statement_points AS p
        SET item_id = resolved.item_id
        FROM (
            SELECT
                source,
                taxonomy,
                concept,
                canonical_metric,
                min(item_id) AS item_id
            FROM fundamental_statement_map
            WHERE is_active
              AND NOT coalesce(is_derived, false)
              AND item_id IS NOT NULL
            GROUP BY 1, 2, 3, 4
            HAVING count(DISTINCT item_id) = 1
        ) AS resolved
        WHERE p.source = resolved.source
          AND p.taxonomy = resolved.taxonomy
          AND p.concept = resolved.concept
          AND p.canonical_metric = resolved.canonical_metric
          AND p.item_id IS DISTINCT FROM resolved.item_id
        """
    )

    conn.execute(
        """
        UPDATE fundamental_statement_points AS p
        SET item_id = resolved.item_id
        FROM (
            SELECT
                alias_scheme,
                alias_code,
                min(item_id) AS item_id
            FROM fundamental_item_alias
            WHERE alias_scheme IS NOT NULL
              AND alias_code IS NOT NULL
              AND item_id IS NOT NULL
            GROUP BY 1, 2
            HAVING count(DISTINCT item_id) = 1
        ) AS resolved
        WHERE p.item_id IS NULL
          AND p.taxonomy = resolved.alias_scheme
          AND p.concept = resolved.alias_code
        """
    )

    conn.execute(
        """
        UPDATE fundamental_points AS p
        SET item_id = resolved.item_id
        FROM (
            SELECT
                alias_scheme,
                alias_code,
                min(item_id) AS item_id
            FROM fundamental_item_alias
            WHERE alias_scheme IS NOT NULL
              AND alias_code IS NOT NULL
              AND item_id IS NOT NULL
            GROUP BY 1, 2
            HAVING count(DISTINCT item_id) = 1
        ) AS resolved
        WHERE p.taxonomy = resolved.alias_scheme
          AND p.metric = resolved.alias_code
          AND p.item_id IS DISTINCT FROM resolved.item_id
        """
    )


def _etl_job_orchestrator_manifest_schema(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S2 S2-1: parent run manifests, steps, and audit substrate.

    The legacy ``etl_job_runs`` table is one row per single-dataset job. PF-S2
    reconciles the roadmap-mandated parent manifest name in place: parent
    orchestrator manifests use ``run_kind = 'orchestrator'`` and
    ``job_run_id = run_id`` with sentinel ``job_name``/``dataset_id`` values for
    compatibility with the pre-existing NOT NULL columns. Existing single-
    dataset rows remain ``run_kind = 'dataset'`` and may later point at a parent
    through nullable ``parent_run_id``.
    """

    for statement in (
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS run_id VARCHAR",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS run_kind VARCHAR DEFAULT 'dataset'",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS parent_run_id VARCHAR",
        "ALTER TABLE etl_job_runs ADD COLUMN IF NOT EXISTS git_sha VARCHAR",
    ):
        conn.execute(statement)
    conn.execute("UPDATE etl_job_runs SET run_id = job_run_id WHERE run_id IS NULL")
    conn.execute("UPDATE etl_job_runs SET run_kind = 'dataset' WHERE run_kind IS NULL")

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS etl_job_steps (
            run_id VARCHAR NOT NULL,
            dataset_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            rows BIGINT,
            started_at TIMESTAMP,
            finished_at TIMESTAMP,
            watermark_before VARCHAR,
            watermark_after VARCHAR,
            error VARCHAR,
            PRIMARY KEY (run_id, dataset_id)
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS etl_job_audit (
            audit_id VARCHAR PRIMARY KEY,
            run_id VARCHAR NOT NULL,
            dataset_id VARCHAR,
            actor VARCHAR NOT NULL,
            ts TIMESTAMP NOT NULL DEFAULT now(),
            action VARCHAR NOT NULL,
            details_json VARCHAR
        )
        """
    )

    conn.execute(
        """
        UPDATE dataset_catalog
        SET description = 'Warehouse ETL job definitions plus PF-S2 parent run manifests, per-dataset steps, retry policy, and append-only audit metadata.',
            grain = 'job_name,job_run_id,run_id,dataset_id',
            updated_at = now()
        WHERE dataset_id = 'warehouse_jobs'
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'etl_job_runs',
                'control',
                'etl_job_run',
                'job_run_id/run_id',
                'ETL run table reconciled for PF-S2: legacy single-dataset job rows and parent orchestrator manifests share one table, distinguished by run_kind. Parent rows use job_run_id = run_id and carry params_json plus git_sha.',
                '["job_run_id"]',
                'Control manifest timestamps; parent orchestrator rows have run_kind=orchestrator and child dataset jobs may reference parent_run_id.'
                , now()
            ),
            (
                'etl_job_steps',
                'control',
                'etl_job_step',
                'run_id,dataset_id',
                'One row per orchestrator DAG node per parent run, holding status, row count, watermarks, and terminal error text for checkpoint/resume.',
                '["run_id", "dataset_id"]',
                'Control metadata only; watermarks record PIT-safe incremental planning inputs/outputs without changing dataset facts.'
                , now()
            ),
            (
                'etl_job_audit',
                'audit',
                'etl_job_audit',
                'audit_id',
                'Append-only orchestrator audit trail for run/step actions such as run_start, step_retry, step_skip_incremental, run_resume, and run_fail.',
                '["audit_id"]',
                'Chronological audit timestamps describe orchestration actions, not fact availability.'
                , now()
            )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('etl_job_runs', 'run_id', 'identifier',
             'Logical parent orchestrator run id; for PF-S2 parent manifest rows this equals job_run_id.',
             true, NULL, NULL, now()),
            ('etl_job_runs', 'run_kind', 'text',
             'Run row kind: dataset for legacy single-dataset job rows, orchestrator for PF-S2 parent manifests.',
             true, NULL, NULL, now()),
            ('etl_job_runs', 'parent_run_id', 'identifier',
             'Nullable parent orchestrator run id for single-dataset child job rows.',
             true, NULL, 'etl_job_runs.run_id', now()),
            ('etl_job_runs', 'git_sha', 'identifier',
             'Git commit SHA captured for a parent orchestrator manifest when available.',
             true, NULL, NULL, now()),
            ('etl_job_steps', 'run_id', 'identifier',
             'Parent orchestrator run id owning this DAG step.',
             false, NULL, 'etl_job_runs.run_id', now()),
            ('etl_job_steps', 'dataset_id', 'identifier',
             'Dataset registry id for this DAG node.',
             false, NULL, 'DATASET_REGISTRY.dataset_id', now()),
            ('etl_job_steps', 'status', 'text',
             'Step status: pending, running, succeeded, failed, or skipped.',
             false, NULL, NULL, now()),
            ('etl_job_steps', 'rows', 'measure',
             'Rows loaded by the dataset step when it runs; NULL while pending or skipped before S2 incremental accounting.',
             true, 'rows', NULL, now()),
            ('etl_job_steps', 'started_at', 'timestamp',
             'Timestamp when the orchestrator began this step.',
             true, 'timestamp', NULL, now()),
            ('etl_job_steps', 'finished_at', 'timestamp',
             'Timestamp when the orchestrator finished this step.',
             true, 'timestamp', NULL, now()),
            ('etl_job_steps', 'watermark_before', 'json',
             'Serialized input/current watermark snapshot before the step.',
             true, NULL, 'dataset_watermarks', now()),
            ('etl_job_steps', 'watermark_after', 'json',
             'Serialized watermark snapshot after the step.',
             true, NULL, 'dataset_watermarks', now()),
            ('etl_job_steps', 'error', 'text',
             'Terminal error text for failed steps.',
             true, NULL, NULL, now()),
            ('etl_job_audit', 'audit_id', 'identifier',
             'Unique append-only audit event id.',
             false, NULL, NULL, now()),
            ('etl_job_audit', 'run_id', 'identifier',
             'Parent orchestrator run id for this audit event.',
             false, NULL, 'etl_job_runs.run_id', now()),
            ('etl_job_audit', 'dataset_id', 'identifier',
             'Optional dataset id context for step-level audit events.',
             true, NULL, 'DATASET_REGISTRY.dataset_id', now()),
            ('etl_job_audit', 'actor', 'text',
             'Actor or subsystem that recorded the audit event.',
             false, NULL, NULL, now()),
            ('etl_job_audit', 'ts', 'timestamp',
             'Audit event timestamp.',
             false, 'timestamp', NULL, now()),
            ('etl_job_audit', 'action', 'text',
             'Append-only orchestration action name such as run_start, step_retry, or run_fail.',
             false, NULL, NULL, now()),
            ('etl_job_audit', 'details_json', 'json',
             'Deterministic JSON details for the audit action.',
             true, NULL, NULL, now())
        """
    )


def _etl_job_orchestrator_manifest_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S2 S2-1: split resume/audit indexes from the schema migration."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_etl_job_steps_run_status ON etl_job_steps(run_id, status)",
        "CREATE INDEX IF NOT EXISTS idx_etl_job_audit_run_ts ON etl_job_audit(run_id, ts)",
    ):
        conn.execute(statement)


def _sql_literal(value: object) -> str:
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "TRUE" if value else "FALSE"
    if isinstance(value, int | float):
        return str(value)
    return "'" + str(value).replace("'", "''") + "'"


def _fundamental_concept_coverage_reports(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S3 S3-3: catalogued concept coverage, unmapped, and universe-gap reports."""

    from .fundamental_statements import statement_map_overlay_exception_rows

    allowlist_values = ",\n            ".join(
        "("
        + ", ".join(
            (
                _sql_literal(row.taxonomy),
                _sql_literal(row.concept),
                _sql_literal(row.industry_template),
                _sql_literal(row.item_id),
                _sql_literal(row.canonical_metric),
                _sql_literal(row.reason),
            )
        )
        + ")"
        for row in statement_map_overlay_exception_rows()
    )
    conn.execute(
        f"""
        CREATE OR REPLACE VIEW fundamental_statement_overlay_allowlist AS
        SELECT *
        FROM (
            VALUES
            {allowlist_values}
        ) AS t(
            taxonomy,
            concept,
            industry_template,
            item_id,
            canonical_metric,
            reason
        )
        """
    )

    conn.execute(
        """
        CREATE OR REPLACE VIEW fundamental_concept_coverage_report AS
        WITH item_universe AS (
            SELECT
                i.item_id,
                i.canonical_code,
                CASE i.statement
                    WHEN 'income' THEN 'income_statement'
                    WHEN 'balance' THEN 'balance_sheet'
                    WHEN 'cashflow' THEN 'cash_flow'
                    WHEN 'bank' THEN 'bank_statement'
                    WHEN 'insurance' THEN 'insurance_statement'
                    WHEN 'reit' THEN 'reit_statement'
                    WHEN 'utility' THEN 'utility_statement'
                    WHEN 'broker_dealer' THEN 'broker_dealer_statement'
                    ELSE i.statement
                END AS fallback_statement_type,
                CASE i.statement
                    WHEN 'bank' THEN 'BK'
                    WHEN 'insurance' THEN 'IS'
                    WHEN 'reit' THEN 'RT'
                    WHEN 'utility' THEN 'UT'
                    WHEN 'broker_dealer' THEN 'BD'
                    ELSE 'ALL'
                END AS fallback_industry_template
            FROM fundamental_item i
            WHERE i.item_id < 2000
              AND coalesce(i.statement, '') IN ('income', 'balance', 'cashflow', 'bank', 'insurance', 'reit', 'utility', 'broker_dealer')
              AND coalesce(i.is_derived, false) = false
        ),
        active_loadable_map AS (
            SELECT source, taxonomy, concept, item_id, statement_type, industry_template
            FROM fundamental_statement_map
            WHERE is_active
              AND item_id IS NOT NULL
              AND coalesce(is_derived, false) = false
              AND taxonomy IN ('us-gaap', 'dei')
              AND left(concept, 2) <> '__'
        ),
        item_groups AS (
            SELECT DISTINCT
                u.item_id,
                u.canonical_code,
                coalesce(m.statement_type, u.fallback_statement_type) AS statement_type,
                coalesce(m.industry_template, u.fallback_industry_template) AS industry_template
            FROM item_universe u
            LEFT JOIN active_loadable_map m
              ON m.item_id = u.item_id
        ),
        per_item AS (
            SELECT
                g.statement_type,
                g.industry_template,
                g.item_id,
                g.canonical_code,
                count(DISTINCT m.taxonomy || ':' || m.concept) AS active_mapped_concept_count,
                count(DISTINCT c.taxonomy || ':' || c.concept) AS extracted_concept_count
            FROM item_groups g
            LEFT JOIN active_loadable_map m
              ON m.item_id = g.item_id
             AND m.statement_type = g.statement_type
             AND m.industry_template = g.industry_template
            LEFT JOIN xbrl_concept_catalog c
              ON c.source = m.source
             AND c.taxonomy = m.taxonomy
             AND c.concept = m.concept
            GROUP BY 1, 2, 3, 4
        ),
        breakdown AS (
            SELECT
                statement_type,
                industry_template,
                count(*) AS canonical_item_count,
                count(*) FILTER (WHERE active_mapped_concept_count > 0) AS mapped_item_count,
                count(*) FILTER (WHERE extracted_concept_count > 0) AS extracted_item_count,
                sum(active_mapped_concept_count) AS active_mapped_concept_count,
                sum(extracted_concept_count) AS extracted_concept_count
            FROM per_item
            GROUP BY 1, 2
        ),
        total AS (
            SELECT
                'TOTAL' AS statement_type,
                'ALL' AS industry_template,
                count(*) AS canonical_item_count,
                count(*) FILTER (WHERE active_mapped_concept_count > 0) AS mapped_item_count,
                count(*) FILTER (WHERE extracted_concept_count > 0) AS extracted_item_count,
                coalesce(sum(active_mapped_concept_count), 0) AS active_mapped_concept_count,
                coalesce(sum(extracted_concept_count), 0) AS extracted_concept_count
            FROM per_item
        )
        SELECT
            statement_type,
            industry_template,
            canonical_item_count,
            mapped_item_count,
            extracted_item_count,
            active_mapped_concept_count,
            extracted_concept_count,
            CASE
                WHEN canonical_item_count = 0 THEN NULL
                ELSE round(100.0 * mapped_item_count / canonical_item_count, 6)
            END AS mapped_item_pct,
            CASE
                WHEN canonical_item_count = 0 THEN NULL
                ELSE round(100.0 * extracted_item_count / canonical_item_count, 6)
            END AS extracted_item_pct
        FROM breakdown
        UNION ALL
        SELECT
            statement_type,
            industry_template,
            canonical_item_count,
            mapped_item_count,
            extracted_item_count,
            active_mapped_concept_count,
            extracted_concept_count,
            CASE
                WHEN canonical_item_count = 0 THEN NULL
                ELSE round(100.0 * mapped_item_count / canonical_item_count, 6)
            END AS mapped_item_pct,
            CASE
                WHEN canonical_item_count = 0 THEN NULL
                ELSE round(100.0 * extracted_item_count / canonical_item_count, 6)
            END AS extracted_item_pct
        FROM total
        """
    )

    conn.execute(
        """
        CREATE OR REPLACE VIEW fundamental_unmapped_concept_report AS
        WITH item_universe AS (
            SELECT
                i.item_id,
                i.canonical_code,
                CASE i.statement
                    WHEN 'income' THEN 'income_statement'
                    WHEN 'balance' THEN 'balance_sheet'
                    WHEN 'cashflow' THEN 'cash_flow'
                    WHEN 'bank' THEN 'bank_statement'
                    WHEN 'insurance' THEN 'insurance_statement'
                    WHEN 'reit' THEN 'reit_statement'
                    WHEN 'utility' THEN 'utility_statement'
                    WHEN 'broker_dealer' THEN 'broker_dealer_statement'
                    ELSE i.statement
                END AS statement_type,
                CASE i.statement
                    WHEN 'bank' THEN 'BK'
                    WHEN 'insurance' THEN 'IS'
                    WHEN 'reit' THEN 'RT'
                    WHEN 'utility' THEN 'UT'
                    WHEN 'broker_dealer' THEN 'BD'
                    ELSE 'ALL'
                END AS industry_template
            FROM fundamental_item i
            WHERE i.item_id < 2000
              AND coalesce(i.statement, '') IN ('income', 'balance', 'cashflow', 'bank', 'insurance', 'reit', 'utility', 'broker_dealer')
              AND coalesce(i.is_derived, false) = false
        ),
        active_item_maps AS (
            SELECT DISTINCT item_id
            FROM fundamental_statement_map
            WHERE is_active
              AND item_id IS NOT NULL
        ),
        unmapped_items AS (
            SELECT
                'item_without_active_concept' AS gap_type,
                CAST(NULL AS VARCHAR) AS source,
                CAST(NULL AS VARCHAR) AS taxonomy,
                CAST(NULL AS VARCHAR) AS concept,
                i.item_id,
                i.canonical_code,
                i.statement_type,
                i.industry_template,
                'canonical item has zero active statement-map concept' AS reason
            FROM item_universe i
            LEFT JOIN active_item_maps m
              ON m.item_id = i.item_id
            WHERE m.item_id IS NULL
              AND NOT EXISTS (
                  SELECT 1
                  FROM fundamental_statement_overlay_allowlist a
                  WHERE a.item_id = i.item_id
                    AND a.industry_template = i.industry_template
              )
        ),
        unmapped_fetched_concepts AS (
            SELECT
                'fetched_concept_without_active_map' AS gap_type,
                c.source,
                c.taxonomy,
                c.concept,
                CAST(NULL AS INTEGER) AS item_id,
                CAST(NULL AS VARCHAR) AS canonical_code,
                CAST(NULL AS VARCHAR) AS statement_type,
                CAST(NULL AS VARCHAR) AS industry_template,
                'loaded xbrl_concept_catalog concept has no active statement-map row' AS reason
            FROM xbrl_concept_catalog c
            LEFT JOIN fundamental_statement_map m
              ON m.source = c.source
             AND m.taxonomy = c.taxonomy
             AND m.concept = c.concept
             AND m.is_active
            WHERE m.concept IS NULL
              AND NOT EXISTS (
                  SELECT 1
                  FROM fundamental_statement_overlay_allowlist a
                  WHERE a.taxonomy = c.taxonomy
                    AND a.concept = c.concept
              )
        )
        SELECT * FROM unmapped_items
        UNION ALL
        SELECT * FROM unmapped_fetched_concepts
        """
    )

    conn.execute(
        """
        CREATE OR REPLACE VIEW fundamental_xbrl_metric_ratio_universe_gap AS
        WITH xbrl_metric_universe AS (
            SELECT DISTINCT security_id
            FROM fundamental_xbrl_metric
            WHERE security_id IS NOT NULL
              AND security_id <> ''
        ),
        ratio_universe AS (
            SELECT DISTINCT security_id
            FROM fundamental_ratios
            WHERE security_id IS NOT NULL
              AND security_id <> ''
        ),
        missing_ratio_securities AS (
            SELECT r.security_id
            FROM ratio_universe r
            LEFT JOIN xbrl_metric_universe x
              ON x.security_id = r.security_id
            WHERE x.security_id IS NULL
        )
        SELECT
            'xbrl_metric_vs_fundamental_ratios' AS report_name,
            (SELECT count(*) FROM xbrl_metric_universe) AS xbrl_metric_security_count,
            (SELECT count(*) FROM ratio_universe) AS ratio_security_count,
            (SELECT count(*) FROM missing_ratio_securities) AS ratio_minus_xbrl_security_count,
            (
                (SELECT count(*) FROM xbrl_metric_universe)
                >= (SELECT count(*) FROM ratio_universe)
                AND (SELECT count(*) FROM missing_ratio_securities) = 0
            ) AS xbrl_covers_ratio_universe,
            (
                SELECT string_agg(security_id, ',' ORDER BY security_id)
                FROM (
                    SELECT security_id
                    FROM missing_ratio_securities
                    ORDER BY security_id
                    LIMIT 50
                )
            ) AS missing_ratio_security_ids_sample
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,
            layer,
            entity,
            grain,
            description,
            natural_key_json,
            pit_notes,
            updated_at
        )
        VALUES
            (
                'fundamental_statement_overlay_allowlist',
                'catalog',
                'fundamental_statement_overlay_allowlist',
                'taxonomy,concept,industry_template,item_id',
                'Explicit PF-S3 S3-1 allowlist of bank/insurance/REIT overlay rows that are intentionally non-loadable from SEC companyfacts.',
                '["taxonomy","concept","industry_template","item_id"]',
                'Reference projection only; the reasons explain why these overlay items are excluded from the S3 unmapped-concept gate.',
                now()
            ),
            (
                'fundamental_concept_coverage_report',
                'control',
                'fundamental_concept_coverage_report',
                'statement_type,industry_template',
                'PF-S3 concept coverage report: percent of canonical fundamental_item entries with active mapped concepts and with at least one loaded xbrl_concept_catalog concept, broken out by statement type and industry template.',
                '["statement_type","industry_template"]',
                'Pure projection over current item registry, statement map, and loaded concept catalog; it does not change fact availability or PIT semantics.',
                now()
            ),
            (
                'fundamental_unmapped_concept_report',
                'control',
                'fundamental_unmapped_concept_report',
                'gap_type,item_id,taxonomy,concept',
                'PF-S3 unmapped report combining canonical non-estimate item-dim entries with zero active statement-map concept and loaded XBRL concepts with no active map row, excluding exactly the explicit overlay allowlist.',
                '["gap_type","item_id","taxonomy","concept"]',
                'Pure projection used as a quality gate; overlay exceptions are explicit rows in fundamental_statement_overlay_allowlist.',
                now()
            ),
            (
                'fundamental_xbrl_metric_ratio_universe_gap',
                'control',
                'fundamental_xbrl_metric_ratio_universe_gap',
                'report_name',
                'PF-S3 universe report comparing distinct securities in fundamental_xbrl_metric with the fundamental_ratios universe; ratio securities absent from xbrl_metric are quality failures.',
                '["report_name"]',
                'Pure projection over derived tables; no lookahead or fact mutation.',
                now()
            )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name,
            field_name,
            semantic_type,
            description,
            nullable,
            unit,
            source_field,
            updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('item_id') THEN 'identifier'
                WHEN lower(c.column_name) LIKE '%count' THEN 'measure'
                WHEN lower(c.column_name) LIKE '%pct' THEN 'measure'
                WHEN lower(c.column_name) LIKE '%covers%' THEN 'flag'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'mapped_item_pct' THEN 'Percent of canonical items in this statement/template bucket with at least one active mapped statement-map row.'
                WHEN 'extracted_item_pct' THEN 'Percent of canonical items in this statement/template bucket with at least one mapped concept present in xbrl_concept_catalog.'
                WHEN 'ratio_minus_xbrl_security_count' THEN 'Distinct fundamental_ratios securities absent from the fundamental_xbrl_metric universe.'
                WHEN 'reason' THEN 'Explicit explanation for the allowlist entry or unmapped report row.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) LIKE '%pct' THEN 'percent'
                WHEN lower(c.column_name) LIKE '%count' THEN 'rows'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name IN (
              'fundamental_statement_overlay_allowlist',
              'fundamental_concept_coverage_report',
              'fundamental_unmapped_concept_report',
              'fundamental_xbrl_metric_ratio_universe_gap'
          )
        """
    )


def _identifier_spine_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S5 S5-0: additive entity/security substrate columns and catalog.

    ``securities.entity_id`` records the current sticky corporate entity above
    share-class ``security_id``. Bitemporal ENTITY_ID rows in
    ``security_identifier_history`` preserve PIT entity mappings; the current
    column is a non-breaking convenience/backfill for existing joins.

    ``security_identifier_history.internal_cusip`` is an internal-only matching
    support column for later FIGI/CUSIP resolution. It is deliberately not a
    public identifier surface.
    """
    for statement in (
        "ALTER TABLE securities ADD COLUMN IF NOT EXISTS entity_id VARCHAR",
        "ALTER TABLE security_identifier_history ADD COLUMN IF NOT EXISTS internal_cusip VARCHAR",
    ):
        conn.execute(statement)

    conn.execute(
        """
        UPDATE securities
        SET entity_id = CASE
                WHEN issuer_id IS NOT NULL AND issuer_id <> '' THEN issuer_id
                WHEN security_id LIKE 'SEC-CIK-%' THEN 'CIK-' || substr(security_id, 9)
                ELSE 'ENTITY-' || security_id
            END
        WHERE entity_id IS NULL
           OR entity_id = ''
        """
    )

    conn.execute(
        """
        INSERT INTO security_identifier_history (
            security_id,
            id_type,
            id_value,
            valid_from,
            valid_to,
            as_of_date,
            available_at,
            source,
            run_id
        )
        WITH earliest_identifier AS (
            SELECT
                security_id,
                min(valid_from) AS first_valid_from
            FROM security_identifier_history
            GROUP BY security_id
        )
        SELECT
            s.security_id,
            'ENTITY_ID' AS id_type,
            s.entity_id AS id_value,
            coalesce(e.first_valid_from, s.first_seen_date, current_date) AS valid_from,
            NULL AS valid_to,
            coalesce(e.first_valid_from, s.first_seen_date, current_date) AS as_of_date,
            current_timestamp AS available_at,
            'migration_0079_identifier_spine' AS source,
            'migration-0079' AS run_id
        FROM securities s
        LEFT JOIN earliest_identifier e
          ON e.security_id = s.security_id
        WHERE s.entity_id IS NOT NULL
          AND s.entity_id <> ''
          AND NOT EXISTS (
              SELECT 1
              FROM security_identifier_history h
              WHERE h.security_id = s.security_id
                AND h.id_type = 'ENTITY_ID'
                AND h.id_value = s.entity_id
                AND h.source = 'migration_0079_identifier_spine'
                AND h.valid_to IS NULL
          )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'securities',
                'core',
                'security',
                'security_id',
                'Stable internal security master with PF-S5 current entity/security split.',
                '["security_id"]',
                'security_id remains the existing join key; entity_id is the current sticky corporate entity. Bitemporal ENTITY_ID rows in security_identifier_history are authoritative for as-of entity resolution.',
                now()
            ),
            (
                'security_identifier_history',
                'core',
                'security_identifier',
                'security_id,id_type,id_value,valid_from',
                'PIT identifier bridge for CIK, ticker, ENTITY_ID, FIGI, LEI, ISIN, and internal matching evidence.',
                '["security_id","id_type","id_value","valid_from"]',
                'Use valid_from/valid_to and available_at for as-of joins. internal_cusip is non-redistributable support data and must not be exported.',
                now()
            )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            (
                'securities',
                'entity_id',
                'identifier',
                'Current sticky corporate entity identifier above share-class security_id. Backfilled from current CIK issuer identity for existing rows; use bitemporal ENTITY_ID history for as-of reads.',
                true,
                NULL,
                'security_identifier_history.id_value where id_type=ENTITY_ID',
                now()
            ),
            (
                'security_identifier_history',
                'internal_cusip',
                'identifier',
                'Internal-only, non-redistributable CUSIP matching support for later FIGI/security resolution. Do not expose in public, lake, or downstream export objects.',
                true,
                NULL,
                NULL,
                now()
            )
        """
    )

    from .schema import create_security_master_current_view

    create_security_master_current_view(conn)


def _identifier_spine_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S5 S5-0: covering indexes split from schema migration 0079."""
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_securities_entity_id ON securities(entity_id)",
        "CREATE INDEX IF NOT EXISTS idx_security_identifier_history_entity_asof ON security_identifier_history(id_type, id_value, valid_from, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_security_identifier_history_internal_cusip ON security_identifier_history(internal_cusip, valid_from, available_at)",
    ):
        conn.execute(statement)


def _entity_parent_edges_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S5 S5-2: entity->entity parent-edge table for the GLEIF Level-2 rollup.

    ``entity_parent_edges`` records direct/ultimate parent relationships between
    sticky corporate entities (``entity_id``, see migration 0079) as reported by
    GLEIF Level-2 relationship records. Each edge carries its OWN GLEIF-reported
    bitemporal validity window (valid_from/valid_to) plus available_at/run_id for
    warehouse provenance -- distinct from the window GLEIF assigns the relationship
    itself. Supports the sticky-entity rollup S5-0 introduced (who-owns-whom).
    """
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS entity_parent_edges (
            child_entity_id VARCHAR NOT NULL,
            parent_entity_id VARCHAR NOT NULL,
            relationship_type VARCHAR NOT NULL,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP,
            source VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_entity_parent_edges_child "
        "ON entity_parent_edges(child_entity_id, relationship_type, valid_from, available_at)"
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_entity_parent_edges_parent "
        "ON entity_parent_edges(parent_entity_id, relationship_type, valid_from, available_at)"
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'entity_parent_edges',
                'core',
                'entity_relationship',
                'child_entity_id,parent_entity_id,relationship_type,valid_from',
                'GLEIF Level-2 direct/ultimate parent relationships between sticky corporate entity_ids, supporting the S5-0 entity rollup.',
                '["child_entity_id","parent_entity_id","relationship_type","valid_from"]',
                'Each edge carries GLEIF''s own reported valid_from/valid_to window; use available_at for as-of no-lookahead reads.',
                now()
            )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            (
                'entity_parent_edges',
                'child_entity_id',
                'identifier',
                'Sticky corporate entity_id of the subsidiary/child side of the relationship.',
                false,
                NULL,
                'GLEIF Level-2 Relationship.StartNode.NodeID (LEI), resolved to entity_id',
                now()
            ),
            (
                'entity_parent_edges',
                'parent_entity_id',
                'identifier',
                'Sticky corporate entity_id of the direct or ultimate parent side of the relationship.',
                false,
                NULL,
                'GLEIF Level-2 Relationship.EndNode.NodeID (LEI), resolved to entity_id',
                now()
            ),
            (
                'entity_parent_edges',
                'relationship_type',
                'category',
                'GLEIF relationship type, e.g. IS_DIRECTLY_CONSOLIDATED_BY or IS_ULTIMATELY_CONSOLIDATED_BY.',
                false,
                NULL,
                'GLEIF Level-2 Relationship.RelationshipType',
                now()
            ),
            (
                'entity_parent_edges',
                'valid_from',
                'date',
                'Start of GLEIF''s own reported validity window for this relationship (not the warehouse load time).',
                false,
                NULL,
                'GLEIF Level-2 Relationship.RelationshipPeriods.StartDate',
                now()
            ),
            (
                'entity_parent_edges',
                'valid_to',
                'date',
                'End of GLEIF''s own reported validity window for this relationship, NULL while still open/current.',
                true,
                NULL,
                'GLEIF Level-2 Relationship.RelationshipPeriods.EndDate',
                now()
            ),
            (
                'entity_parent_edges',
                'as_of_date',
                'date',
                'Warehouse as-of date this edge row was derived under (load-time, distinct from GLEIF''s own valid_from/valid_to window).',
                false,
                NULL,
                NULL,
                now()
            ),
            (
                'entity_parent_edges',
                'available_at',
                'timestamp',
                'Timestamp when this edge became visible for PIT research; use for as-of no-lookahead reads (see pit_notes).',
                true,
                NULL,
                NULL,
                now()
            ),
            (
                'entity_parent_edges',
                'source',
                'identifier',
                'Loader source name that wrote this edge (e.g. GLEIF).',
                false,
                NULL,
                NULL,
                now()
            ),
            (
                'entity_parent_edges',
                'run_id',
                'identifier',
                'Identifier of the ingestion run that wrote this edge, for provenance/reproducibility.',
                true,
                NULL,
                NULL,
                now()
            ),
            (
                'entity_parent_edges',
                'source_loaded_at',
                'timestamp',
                'Warehouse-assigned timestamp when this row was physically loaded (DEFAULT now()), distinct from available_at.',
                false,
                NULL,
                NULL,
                now()
            )
        """
    )


def _sec_company_facts_entity_id_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S5 S5-3: carried entity_id column on sec_company_facts.

    ``sec_company_facts.security_id`` already exists but, prior to this
    migration, is populated as a direct passthrough of the loader's ticker/CIK
    resolution rather than a value resolved through the S5-0 identifier spine
    (``security_identifier_history`` CIK history joined through to ENTITY_ID
    history). ``entity_id`` did not exist on this table at all.

    This migration is additive-only: it adds the ``entity_id`` column (mirroring
    ``securities.entity_id`` from migration 0079) and catalogs both
    ``security_id`` and ``entity_id`` on ``sec_company_facts``. It does NOT
    backfill values -- the loader (db/fundamentals.py, via the new
    ``security_and_entity_ids_for_ciks_asof`` PIT reader in db/security_master.py)
    resolves and writes security_id/entity_id per fact at load time, honoring
    each fact's own available_at (no lookahead). Existing rows keep whatever
    security_id they already had (their prior best-effort passthrough value)
    and NULL entity_id until re-loaded.

    fundamental_statement_points, fundamental_ttm_points, and the ratio engine
    (fundamental_ratios.py) all read security_id by copying it forward from
    sec_company_facts -- they are NOT touched here. Fixing the source column
    is sufficient for security_id to carry through by inheritance.
    """
    conn.execute("ALTER TABLE sec_company_facts ADD COLUMN IF NOT EXISTS entity_id VARCHAR")

    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_sec_company_facts_entity_asof "
        "ON sec_company_facts(entity_id, filed_date)"
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            (
                'sec_company_facts',
                'security_id',
                'identifier',
                'Warehouse security_id resolved through the S5-0 identifier spine (security_identifier_history CIK history) as of this fact''s own available_at -- not a raw passthrough. Unresolved CIKs are routed to identifier_resolution_candidates rather than dropped.',
                false,
                NULL,
                'security_identifier_history.security_id where id_type=CIK, as-of fact.available_at',
                now()
            ),
            (
                'sec_company_facts',
                'entity_id',
                'identifier',
                'Sticky corporate entity_id resolved through the S5-0/S5-2 entity spine as of this fact''s own available_at (no lookahead). NULL when the fact''s CIK does not yet resolve to a known entity; such facts are also routed to identifier_resolution_candidates.',
                true,
                NULL,
                'security_identifier_history.id_value where id_type=ENTITY_ID, as-of fact.available_at',
                now()
            )
        """
    )


def _repair_identifier_spine_self_overlaps_s5(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S5 S5-4: one-time repair, collapse open-ended identifier_history
    duplicates that reaccumulated after the S32 (migration 0054) fix.

    S32 drove ``identifier_same_source_self_overlaps`` to 0 by collapsing
    redundant open-ended rows once. It reappeared (528 rows, per PARITY_GAP.md
    Domain 3/S9a) because later write paths -- the PF-S5 FIGI/LEI alias
    loaders and ``identifier_resolution_decisions._apply_accepted_identifiers``
    -- did not all guard against re-inserting an open-ended row for a key that
    already has one. S5-4 wires ``dedupe_open_identifier_intervals`` into the
    seeder write paths (``upsert_security_master_from_frame``,
    ``identifiers_figi.py``, ``identifiers_lei.py``) so new duplicates stop
    landing; this migration is the matching one-time repair for what already
    landed, mirroring migration 0054's ``_repair_identifier_history_overlaps``.

    Only open-ended (``valid_to IS NULL``) duplicate rows are removed, keeping
    the earliest ``(valid_from, available_at, rowid)`` per
    ``(security_id, id_type, id_value, source)`` -- the true first disclosure.
    Closed intervals (e.g. a real ticker change) are never touched.
    """
    from .security_master import collapse_identifier_history_open_duplicates

    collapse_identifier_history_open_duplicates(conn)


def _repair_identifier_history_overlaps(conn: duckdb.DuckDBPyConnection) -> None:
    """S32: collapse redundant open-ended security_identifier_history intervals.

    The SEC ownership issuer seed historically wrote one open-ended identifier
    row per filing, producing many overlapping intervals for the same
    (security_id, id_type, id_value, source). This one-time repair keeps the
    earliest interval per key and removes the redundant duplicates, clearing the
    standing duplicate_identifier_history_keys and
    identifier_same_source_self_overlaps quality failures. The loader is now
    idempotent, so the failures do not reappear.
    """
    from .security_master import collapse_identifier_history_open_duplicates

    collapse_identifier_history_open_duplicates(conn)


def _formula_registry_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S4 S4-0: formula_registry table (definition-as-data formula catalog).

    Turns the ratio/score engine's formulas from Python closures into
    declarative rows: one row per ``formula_code`` (matches today's
    ``fundamental_ratios.ratio_code``), carrying the reducer selector
    (``transform``: divide/sum/difference/pct_change/identity) plus a
    declarative ``expression`` mini-grammar string for multi-term formulas
    (DuPont, the coverage/accrual composites, the 4 distress scores),
    ``is_meaningful_rule`` (data-encoded gate, e.g. Beneish's
    ``sales>0 and receivables0>0``, replacing a Python ``if``), and a
    ``citation`` (free text; empty for plain accounting ratios, required for
    scores and named academic formulas per PF-S4 review discipline -- not a
    DB-level NOT NULL).

    ``numerator_item_ids_json`` / ``denominator_item_ids_json`` are
    JSON-array-encoded lists of PF-S1 ``fundamental_item.item_id`` values
    (the warehouse's existing ``*_json`` VARCHAR convention -- see
    ``fundamental_ratios.input_item_ids_json`` -- there is no existing
    native-array column precedent in this schema). ``inputs_json`` is the
    JSON-array-encoded raw input-key gating list, mirroring
    ``RatioDef.inputs``.

    ``valid_from``/``valid_to`` make the formula DEFINITION itself
    bitemporal (a formula's coefficients or citation can be revised without
    losing the prior definition), feeding S4-3's catalog reader and PF-S8
    vintage history.

    Schema only; the index lands in migration 0076 (split per the S5g/S5k
    WAL-replay precedent -- schema and index in separate migration numbers).
    S4-0 seeds NO formula rows (the committed seed CSV is header-only); S4-1
    ports the 53 existing codes under a byte-identity gate, S4-2 adds new
    families.
    """
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS formula_registry (
            formula_code VARCHAR PRIMARY KEY,
            family VARCHAR NOT NULL,
            kind VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            numerator_code VARCHAR,
            denominator_code VARCHAR,
            numerator_item_ids_json VARCHAR,
            denominator_item_ids_json VARCHAR,
            inputs_json VARCHAR NOT NULL,
            transform VARCHAR NOT NULL,
            expression VARCHAR,
            is_meaningful_rule VARCHAR,
            definition VARCHAR NOT NULL,
            citation VARCHAR,
            valid_from DATE NOT NULL,
            valid_to DATE,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'formula_registry',
                'gold',
                'formula',
                'formula_code',
                'Definition-as-data catalog of ratio/score formulas: reducer transform, multi-term expression mini-grammar, item-id-linked numerator/denominator, meaningfulness gate, and citation -- replacing the fundamental_ratios.py RatioDef Python closures.',
                '["formula_code"]',
                'valid_from/valid_to make the formula DEFINITION itself bitemporal (as-of-able); this is independent of the bitemporal validity of any VALUE computed from the formula (see fundamental_ratios.available_at/as_of_date).',
                now()
            )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('formula_registry', 'formula_code', 'identifier',
             'Formula identifier; matches fundamental_ratios.ratio_code for ported formulas.',
             false, NULL, NULL, now()),
            ('formula_registry', 'family', 'category',
             'Governed formula family (e.g. profitability, leverage, cash_flow, payout, per_share, efficiency, growth, liquidity, health), a superset of fundamental_ratios.ratio_category.',
             false, NULL, NULL, now()),
            ('formula_registry', 'kind', 'category',
             'Formula shape: ratio | level | difference | growth | per_share | score, mirroring fundamental_ratios.RatioDef.kind.',
             false, NULL, NULL, now()),
            ('formula_registry', 'unit', 'category',
             'Output unit family (e.g. ratio, currency, currency_per_share, score).',
             false, NULL, NULL, now()),
            ('formula_registry', 'numerator_code', 'text',
             'Human-readable numerator canonical code; NULL for score formulas whose terms live in expression.',
             true, NULL, NULL, now()),
            ('formula_registry', 'denominator_code', 'text',
             'Human-readable denominator canonical code; NULL for score formulas whose terms live in expression.',
             true, NULL, NULL, now()),
            ('formula_registry', 'numerator_item_ids_json', 'json',
             'JSON array of PF-S1 fundamental_item.item_id values feeding the numerator; NULL for score formulas.',
             true, NULL, 'fundamental_item.item_id', now()),
            ('formula_registry', 'denominator_item_ids_json', 'json',
             'JSON array of PF-S1 fundamental_item.item_id values feeding the denominator; NULL for score formulas.',
             true, NULL, 'fundamental_item.item_id', now()),
            ('formula_registry', 'inputs_json', 'json',
             'JSON array of raw input keys gating formula availability, mirroring RatioDef.inputs.',
             false, NULL, NULL, now()),
            ('formula_registry', 'transform', 'category',
             'Reducer selector: divide | sum | difference | pct_change | identity. identity defers to expression for multi-term formulas.',
             false, NULL, NULL, now()),
            ('formula_registry', 'expression', 'text',
             'Declarative mini-grammar expression over named item ids + literals for multi-term formulas (DuPont, coverage/accrual composites, the 4 distress scores). Evaluated by a fixed, unit-tested dispatch table -- no arbitrary eval. NULL when transform alone suffices.',
             true, NULL, NULL, now()),
            ('formula_registry', 'is_meaningful_rule', 'text',
             'Data-encoded meaningfulness gate (e.g. require_positive_denominator, or a named per-formula gate such as Beneish''s sales>0 and receivables0>0), replacing a Python if. NULL means always meaningful when inputs are present.',
             true, NULL, NULL, now()),
            ('formula_registry', 'definition', 'text',
             'Human-readable definition of the formula.',
             false, NULL, NULL, now()),
            ('formula_registry', 'citation', 'text',
             'Academic or vendor citation. Empty for plain accounting ratios; required for scores and named academic formulas by PF-S4 review discipline.',
             true, NULL, NULL, now()),
            ('formula_registry', 'valid_from', 'date',
             'Date this formula definition becomes valid (bitemporal definition validity, independent of any computed value''s own as_of_date/available_at).',
             false, NULL, NULL, now()),
            ('formula_registry', 'valid_to', 'date',
             'Date this formula definition stops being valid; NULL means still current (coalesce to DATE 9999-12-31).',
             true, NULL, NULL, now()),
            ('formula_registry', 'run_id', 'identifier',
             'Identifier of the ingestion/seed run that wrote this row, for provenance/reproducibility.',
             true, NULL, NULL, now()),
            ('formula_registry', 'source_loaded_at', 'timestamp',
             'Warehouse-assigned timestamp when this row was physically loaded (DEFAULT now()).',
             false, NULL, NULL, now())
        """
    )


def _formula_registry_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S4 S4-0: lookup indexes split from schema migration 0075."""
    for statement in (
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_formula_registry_formula_code ON formula_registry(formula_code)",
        "CREATE INDEX IF NOT EXISTS idx_formula_registry_family ON formula_registry(family)",
    ):
        conn.execute(statement)


def _formula_registry_catalog_view(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S4 S4-3: v_formula_registry -- the queryable/as-of formula catalog surface.

    "What is EV/EBITDA?" is answerable today only by reading Python. This
    view is a plain pass-through SELECT over ``formula_registry`` (schema:
    migration 0075; indexes: 0076) exposing every catalog column --
    ``formula_code -> family, kind, unit, numerator/denominator item ids,
    expression, citation, is_meaningful_rule, valid_from/valid_to`` -- as one
    ``SELECT``-able surface, so a formula's full definition (with citation)
    is data, not a closure.

    Per (B)/S7a, views are catalogued like tables: this view gets its own
    table_catalog + field_catalog rows (mirroring the
    ``v_offexchange_security_period`` precedent -- migration_2 /
    ``_catalog_backfill_reference_and_views``, migrations.py:2213-2386 --
    which is the only prior catalogued-view example in this warehouse;
    table_catalog/field_catalog have no table-vs-view discriminator column,
    so a view row looks exactly like a table row).

    PIT note: ``formula_registry`` carries no ``available_at`` column (only
    ``valid_from``/``valid_to`` bitemporal DEFINITION validity -- see the
    migration-0075 docstring). The as-of PYTHON reader that filters this
    view by ``valid_from <= as_of_date < coalesce(valid_to, 9999-12-31)`` is
    ``formula_registry_asof`` in ``db/asof.py``; this view itself is the
    always-current (unfiltered) catalog surface a plain ``SELECT`` sees.
    """
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_formula_registry AS
        SELECT
            formula_code,
            family,
            kind,
            unit,
            numerator_code,
            denominator_code,
            numerator_item_ids_json,
            denominator_item_ids_json,
            inputs_json,
            transform,
            expression,
            is_meaningful_rule,
            definition,
            citation,
            valid_from,
            valid_to
        FROM formula_registry
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'v_formula_registry',
                'gold',
                'formula',
                'formula_code',
                'Queryable formula catalog surface: every formula_registry column (definition, citation, numerator/denominator item ids, transform/expression, meaningfulness gate) as one SELECT-able view, so a formula''s full definition is data, not a Python closure.',
                '["formula_code"]',
                'View over formula_registry (no additional filtering). For a point-in-time read honoring the formula DEFINITION''s own bitemporal validity, use formula_registry_asof(as_of_date, ...) in db/asof.py, which applies valid_from <= as_of_date < coalesce(valid_to, DATE 9999-12-31) over this same column set.',
                now()
            )
        """
    )

    # Field descriptions are pulled straight from formula_registry's own
    # field_catalog rows (migration 0075), just re-parented onto the view
    # name, so the view's documentation never drifts from the base table's --
    # self-contained (no dependency on duckdb_columns() column ordering) and
    # single-sourced.
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        SELECT
            'v_formula_registry',
            field_name,
            semantic_type,
            description,
            nullable,
            unit,
            source_field,
            now()
        FROM field_catalog
        WHERE table_name = 'formula_registry'
          AND field_name NOT IN ('run_id', 'source_loaded_at')
        """
    )


def _market_cap_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S6 S6-1: market cap = raw daily close x PIT-visible shares."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS market_cap (
            market_cap_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            price_source VARCHAR NOT NULL,
            share_source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            trade_date DATE NOT NULL,
            close DOUBLE NOT NULL,
            share_count DOUBLE NOT NULL,
            share_count_type_used VARCHAR NOT NULL,
            market_cap DOUBLE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            price_available_at TIMESTAMP NOT NULL,
            share_available_at TIMESTAMP NOT NULL,
            price_run_id VARCHAR,
            share_run_id VARCHAR,
            share_history_id VARCHAR,
            input_codes_json VARCHAR NOT NULL,
            input_lineage_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'market_cap',
            'atx_warehouse',
            'Derived market capitalization',
            'Daily market capitalization computed as raw equity_daily_bars.close times the PIT-visible shares_outstanding_history share count; bitemporal and source-lineaged.',
            'security_id,trade_date',
            'market_cap', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'market_cap', 'gold', 'valuation',
            'source,security_id,trade_date',
            'Daily market capitalization level struck from raw daily close and the latest PIT-visible share count, preferring shares_outstanding and falling back to shares_diluted_avg.',
            '["source","security_id","trade_date"]',
            'No lookahead on applicability: selected share row must have effective_date/as_of_date <= trade_date. Later-filed applicable share rows are allowed, and available_at is max(price_available_at, share_available_at).',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN (
                    'market_cap_id', 'security_id', 'run_id', 'price_run_id',
                    'share_run_id', 'share_history_id'
                ) THEN 'identifier'
                WHEN lower(c.column_name) IN ('trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN lower(c.column_name) IN ('is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN lower(c.column_name) IN ('close') THEN 'price'
                WHEN lower(c.column_name) IN ('market_cap') THEN 'currency'
                WHEN lower(c.column_name) LIKE '%count%' OR upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'close' THEN 'Raw daily close from equity_daily_bars used deliberately for same-day market-cap levels; adjusted_close is not used.'
                WHEN 'share_count_type_used' THEN 'PIT share-count type selected: shares_outstanding preferred; shares_diluted_avg used only when no visible instant count exists.'
                WHEN 'market_cap' THEN 'Raw close multiplied by selected PIT-visible share_count.'
                WHEN 'available_at' THEN 'Maximum of price_available_at and share_available_at.'
                WHEN 'input_lineage_json' THEN 'JSON lineage for the selected price and share input rows, including source/run_id/availability.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('trade_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('close', 'market_cap') THEN 'USD'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                ELSE NULL
            END,
            NULL,
            now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'market_cap'
        """
    )


def _market_cap_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S6 S6-1 indexes for market_cap split from schema/catalog DDL."""

    for statement in (
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_market_cap_key ON market_cap(source, security_id, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_market_cap_security_date ON market_cap(security_id, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_market_cap_asof ON market_cap(as_of_date, available_at)",
    ):
        conn.execute(statement)


def _valuation_multiples_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S6 S6-2: valuation multiples sibling table for market_cap + fundamentals."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS valuation_multiples (
            valuation_multiple_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            market_cap_source VARCHAR NOT NULL,
            market_cap_id VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            trade_date DATE NOT NULL,
            formula_code VARCHAR NOT NULL,
            category VARCHAR NOT NULL,
            kind VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            value DOUBLE NOT NULL,
            numerator_code VARCHAR,
            numerator_value DOUBLE,
            denominator_code VARCHAR,
            denominator_value DOUBLE,
            price DOUBLE,
            market_cap DOUBLE NOT NULL,
            enterprise_value DOUBLE,
            is_meaningful BOOLEAN NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            market_cap_available_at TIMESTAMP NOT NULL,
            price_available_at TIMESTAMP,
            input_codes_json VARCHAR NOT NULL,
            input_lineage_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'valuation_multiples',
            'atx_warehouse',
            'Derived valuation multiples',
            'Daily PIT-safe valuation multiples combining market_cap with latest applicable TTM, balance-sheet, and XBRL-derived fundamentals.',
            'security_id,trade_date,formula_code',
            'valuation_multiples', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'valuation_multiples', 'gold', 'valuation',
            'source,market_cap_source,security_id,trade_date,formula_code',
            'PIT valuation multiples and enterprise value levels derived from market_cap plus latest applicable fundamentals.',
            '["source","market_cap_source","security_id","trade_date","formula_code"]',
            'For each market-cap row, the latest fundamental period with period_end <= trade_date is selected. Each row available_at is the maximum of the market-cap leg and that formula-specific fundamental inputs.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN (
                    'valuation_multiple_id', 'market_cap_id', 'security_id', 'run_id'
                ) THEN 'identifier'
                WHEN lower(c.column_name) IN ('trade_date', 'period_start', 'period_end', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN lower(c.column_name) IN ('is_meaningful', 'is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN lower(c.column_name) IN ('price') THEN 'price'
                WHEN lower(c.column_name) IN ('market_cap', 'enterprise_value') THEN 'currency'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END,
            CASE c.column_name
                WHEN 'formula_code' THEN 'Valuation formula code registered in formula_registry.'
                WHEN 'value' THEN 'Computed valuation multiple value, or currency level for enterprise_value.'
                WHEN 'is_meaningful' THEN 'False when the formula is computable but economically sign-ambiguous, such as a non-positive denominator.'
                WHEN 'available_at' THEN 'Maximum availability timestamp of the market-cap leg and formula-specific fundamental inputs.'
                WHEN 'input_lineage_json' THEN 'JSON lineage for the selected market-cap and formula-specific fundamental input rows.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('trade_date', 'period_start', 'period_end', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('market_cap', 'enterprise_value', 'price') THEN 'USD'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                ELSE NULL
            END,
            NULL,
            now()
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'valuation_multiples'
        """
    )


def _valuation_multiples_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S6 S6-2 indexes for valuation_multiples split from schema/catalog DDL."""

    for statement in (
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_valuation_multiples_key ON valuation_multiples(source, market_cap_source, security_id, trade_date, formula_code)",
        "CREATE INDEX IF NOT EXISTS idx_valuation_multiples_security_date ON valuation_multiples(security_id, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_valuation_multiples_asof ON valuation_multiples(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_valuation_multiples_formula ON valuation_multiples(formula_code, category)",
    ):
        conn.execute(statement)


def _valuation_overlap_slice_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S9 S9-0: persisted price x fundamental overlap provenance."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS valuation_overlap_slice (
            overlap_slice_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            market_cap_sources_json VARCHAR NOT NULL,
            symbol_scope_json VARCHAR NOT NULL,
            start_date DATE,
            end_date DATE,
            as_of_ts TIMESTAMP,
            max_visible_available_at TIMESTAMP,
            numerator_security_count INTEGER NOT NULL,
            denominator_security_count INTEGER NOT NULL,
            coverage_ratio DOUBLE,
            valuation_row_count INTEGER NOT NULL,
            min_valuation_trade_date DATE,
            max_valuation_trade_date DATE,
            min_valuation_period_end DATE,
            max_valuation_period_end DATE,
            stale_price_fundamental_gap_days INTEGER NOT NULL,
            stale_valuation_row_count INTEGER NOT NULL,
            max_price_fundamental_gap_days INTEGER,
            denominator_definition VARCHAR NOT NULL,
            details_json VARCHAR NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'valuation_overlap_slice',
            'atx_warehouse',
            'Valuation overlap slice provenance',
            'Persisted proof-slice coverage summary for the PIT price x fundamental overlap used by valuation_multiples.',
            'source,scope,as_of_ts,run_id',
            'valuation_overlap_slice', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'valuation_overlap_slice', 'gold', 'valuation',
            'source,market_cap_sources_json,symbol_scope_json,start_date,end_date,as_of_ts,run_id',
            'Durable provenance row for each valuation-multiple overlap refresh, including denominator securities, populated securities, date span, coverage ratio, and stale price/fundamental gap counts.',
            '["source","market_cap_sources_json","symbol_scope_json","start_date","end_date","as_of_ts","run_id"]',
            'Coverage is point-in-time: denominator and numerator are evaluated with available_at <= as_of_ts when an as-of timestamp is supplied. available_at records the coverage horizon used for the row.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("valuation_overlap_slice",))


def _valuation_overlap_slice_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S9 S9-0/S9-3 lookup indexes for valuation overlap provenance."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_valuation_overlap_slice_scope ON valuation_overlap_slice(source, start_date, end_date, as_of_ts)",
        "CREATE INDEX IF NOT EXISTS idx_valuation_overlap_slice_available ON valuation_overlap_slice(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_valuation_overlap_slice_run ON valuation_overlap_slice(run_id)",
    ):
        conn.execute(statement)


def _fact_disagreement_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S9 S9-1: cross-vendor standardized fact reconciliation."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS vendor_baseline_facts (
            baseline_fact_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            vendor VARCHAR NOT NULL,
            vendor_fact_id VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            item_id INTEGER NOT NULL,
            canonical_code VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            value DOUBLE NOT NULL,
            unit_type VARCHAR,
            source_accession VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            input_lineage_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fact_disagreement (
            disagreement_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            baseline_source VARCHAR NOT NULL,
            vendor VARCHAR NOT NULL,
            baseline_fact_id VARCHAR,
            standardized_id VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            item_id INTEGER NOT NULL,
            canonical_code VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            warehouse_value DOUBLE,
            vendor_value DOUBLE,
            absolute_difference DOUBLE,
            relative_difference DOUBLE,
            tolerance_abs DOUBLE NOT NULL,
            tolerance_rel DOUBLE NOT NULL,
            agreement_status VARCHAR NOT NULL,
            vintage_status VARCHAR NOT NULL,
            warehouse_available_at TIMESTAMP,
            vendor_available_at TIMESTAMP,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            input_lineage_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES
            (
                'vendor_baseline_facts',
                'vendor_offline',
                'Vendor baseline comparable facts',
                'Offline Sharadar/SimFin-style comparable fact rows normalized for like-for-like reconciliation against fundamental_standardized.',
                'vendor,security_id,item_id,period_end,basis',
                'vendor_baseline_facts', 'as_of_date', 'available_at', now()
            ),
            (
                'fact_disagreement',
                'atx_warehouse',
                'Cross-vendor fact disagreement',
                'Agreement/disagreement rows comparing fundamental_standardized to an injectable offline vendor baseline with basis and vintage alignment.',
                'vendor,security_id,item_id,period_end,basis',
                'fact_disagreement', 'as_of_date', 'available_at', now()
            )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'vendor_baseline_facts', 'silver', 'fundamentals',
                'source,vendor,security_id,item_id,period_end,basis',
                'Normalized offline vendor comparable facts used as a cross-vendor baseline for standardized XBRL facts.',
                '["source","vendor","security_id","item_id","period_end","basis"]',
                'Baseline facts carry as_of_date and available_at so reconciliation can compare only facts visible at the same as-of timestamp.',
                now()
            ),
            (
                'fact_disagreement', 'gold', 'fundamental_reconciliation',
                'source,baseline_source,vendor,security_id,item_id,period_end,basis',
                'Like-for-like cross-vendor comparison of warehouse standardized facts against vendor_baseline_facts, including tolerance, status, and lineage.',
                '["source","baseline_source","vendor","security_id","item_id","period_end","basis"]',
                'Rows compare latest visible warehouse and vendor vintages using available_at <= the reconciliation as-of timestamp; available_at is the maximum visible input timestamp.',
                now()
            )
        """
    )
    _catalog_fields_for_tables(conn, ("vendor_baseline_facts", "fact_disagreement"))


def _fact_disagreement_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S9 S9-1 lookup indexes for baseline and disagreement rows."""

    for statement in (
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_vendor_baseline_facts_key ON vendor_baseline_facts(source, vendor, security_id, item_id, period_end, basis)",
        "CREATE INDEX IF NOT EXISTS idx_vendor_baseline_facts_asof ON vendor_baseline_facts(as_of_date, available_at)",
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_fact_disagreement_key ON fact_disagreement(source, baseline_source, vendor, security_id, item_id, period_end, basis)",
        "CREATE INDEX IF NOT EXISTS idx_fact_disagreement_status ON fact_disagreement(agreement_status, vendor, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fact_disagreement_asof ON fact_disagreement(as_of_date, available_at)",
    ):
        conn.execute(statement)


def _xbrl_dqc_catalog_seed(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S9 S9-2: SQL-native DQC subset catalog rows and documented skips."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS xbrl_dqc_rule_catalog (
            rule_code VARCHAR PRIMARY KEY,
            rule_family VARCHAR NOT NULL,
            port_status VARCHAR NOT NULL,
            sql_subset_description VARCHAR NOT NULL,
            skipped_reason VARCHAR,
            official_rule_url VARCHAR,
            guidance_url VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO xbrl_dqc_rule_catalog (
            rule_code, rule_family, port_status, sql_subset_description,
            skipped_reason, official_rule_url, guidance_url, updated_at
        )
        VALUES
            (
                'DQC_0004', 'dqc', 'ported_sql_subset',
                'Assets = Liabilities + StockholdersEquity foots for same context/unit when all three facts are present.',
                NULL, 'https://xbrl.us/data-rule/dqc_0004/',
                'https://xbrl.us/home/priorities/data-quality/rules-guidance/', now()
            ),
            (
                'DQC_0015', 'dqc', 'ported_sql_subset',
                'Curated US-GAAP non-negative concept list flags negative facts.',
                NULL, 'https://xbrl.us/data-rule/dqc_0015/',
                'https://xbrl.us/home/priorities/data-quality/rules-guidance/', now()
            ),
            (
                'DQC_0080', 'dqc', 'ported_sql_subset',
                'Curated IFRS non-negative concept list flags negative facts.',
                NULL, 'https://xbrl.us/data-rule/dqc_0080/',
                'https://xbrl.us/home/priorities/data-quality/rules-guidance/', now()
            ),
            (
                'DQC_0018', 'dqc', 'ported_sql_subset',
                'Curated deprecated element list flags facts reported on concepts replaced by current taxonomy concepts.',
                NULL, 'https://xbrl.us/data-rule/dqc_0018/',
                'https://xbrl.us/home/priorities/data-quality/rules-guidance/', now()
            ),
            (
                'DQC_0041', 'dqc', 'ported_sql_subset',
                'Explicit dimension member must be reachable through a direct local dimension-domain/domain-member edge or the curated allowed pair list.',
                NULL, 'https://xbrl.us/data-rule/dqc_0041/',
                'https://xbrl.us/home/priorities/data-quality/rules-guidance/', now()
            ),
            (
                'DQC_0053', 'dqc', 'ported_sql_subset',
                'Curated excluded member-axis pairs and direct usable=false dimension edges.',
                NULL, 'https://xbrl.us/data-rule/dqc_0053/',
                'https://xbrl.us/home/priorities/data-quality/rules-guidance/', now()
            ),
            (
                'DQC_0135', 'dqc', 'skipped_requires_full_processor',
                'Extensible-enumeration validation is not ported to SQL.',
                'Requires full Arelle/XULE extensible-enumeration semantics and taxonomy plugin metadata.',
                'https://xbrl.us/data-rule/dqc_0135/',
                'https://xbrl.us/home/priorities/data-quality/rules-guidance/', now()
            ),
            (
                'DQC_0118', 'dqc', 'skipped_requires_full_processor',
                'Financial-statement-table calculation validation is not ported to SQL.',
                'Requires full statement-table calculation semantics beyond the local fact/context/linkbase tables.',
                'https://xbrl.us/data-rule/dqc_0118/',
                'https://xbrl.us/home/priorities/data-quality/rules-guidance/', now()
            )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'xbrl_dqc_rule_catalog', 'reference', 'xbrl_validation',
            'rule_code',
            'Catalog of DQC rules ported as SQL subsets or explicitly skipped because they need a full XBRL/Arelle processor.',
            '["rule_code"]',
            'Reference metadata only. updated_at is knowledge time for the local DQC coverage note.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("xbrl_dqc_rule_catalog",))


def _pf2_s9_indexes_report(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S9 S9-3: lookup indexes for S9 coverage/reconciliation surfaces."""

    _valuation_overlap_slice_indexes(conn)
    _fact_disagreement_indexes(conn)
    _schema_contract_schema_catalog(conn)


def _xbrl_validation_dimensional_evidence(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S7 S7-0: per-row dimensional-context evidence on xbrl_validation_results.

    The v1 calc-linkbase validator joined a parent fact to its children on
    ``context_ref`` as an opaque string and never consulted
    ``xbrl_filing_contexts`` / ``xbrl_filing_dimensions``. When an issuer reports
    a parent total in the default (no-dimension) context but breaks a child out
    across explicit members of a ``*Axis``, the default-context parent has an
    INCOMPLETE comparable child set and the row false-failed -- the entire
    mechanism behind the 1,364 standing calculation-linkbase failures.

    S7-0 makes ``refresh_xbrl_validation_results`` dimension-aware (it groups a
    parent only against children sharing the SAME period/unit/dimensional
    signature, never summing across signatures) and attaches, per validation
    row, the evidence used to triage the outcome. This migration is additive-only:
    it adds the ``dimensional_evidence_json`` column and catalogs it. The column
    holds a JSON object with the parent's dimensional signature, the comparability
    ``verdict`` (``complete_footing_ok`` / ``genuine_footing_error`` /
    ``resolved_dimensional_artifact`` / ``genuine_missing_child``), the expected
    vs matched child counts, and any linkbase children absent from the parent's
    signature together with whether each was found under a DIFFERENT dimensional
    context in the same filing (the positive proof required to resolve an
    artifact rather than absorb a real error).

    S7-3 (migration 0089) adds the distinct, coarser ``resolution_status`` column;
    this migration deliberately does NOT pre-empt it. ``ADD COLUMN IF NOT EXISTS``
    keeps it idempotent on databases where schema.py already created the column.
    """

    conn.execute(
        "ALTER TABLE xbrl_validation_results "
        "ADD COLUMN IF NOT EXISTS dimensional_evidence_json VARCHAR"
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            (
                'xbrl_validation_results',
                'dimensional_evidence_json',
                'json',
                'Per-row dimensional-context triage evidence for calculation_linkbase results (PF-S7 S7-0). JSON object: parent_signature (the parent fact''s ordered axis->member signature, "DEFAULT" when dimensionless), verdict (complete_footing_ok / genuine_footing_error / resolved_dimensional_artifact / genuine_missing_child), total_edge_count, matched_child_count, and missing_children (linkbase child concepts absent from the parent''s dimensional signature, each flagged present_in_other_context = reported under a different dimensional context in the same period/unit). A parent is only resolved as a dimensional artifact when every missing child is positively found under another context; otherwise it stays failed.',
                true,
                NULL,
                'derived from xbrl_filing_contexts + xbrl_filing_dimensions during validation',
                now()
            )
        """
    )


def _xbrl_validation_resolution_status(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S7 S7-3: coarse validation-resolution status for reporting.

    ``dimensional_evidence_json`` records the detailed S7-0 verdict/evidence.
    This column is the reportable status used by dataset loads and quality checks
    to separate resolved artifacts from genuine residual errors without hiding
    the row-level audit trail.
    """

    conn.execute(
        "ALTER TABLE xbrl_validation_results "
        "ADD COLUMN IF NOT EXISTS resolution_status VARCHAR"
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            (
                'xbrl_validation_results',
                'resolution_status',
                'status',
                'PF-S7 S7-3 reportable resolution status for validation rows. Calculation-linkbase rows use resolved_ok for complete footings, resolved_dimensional_artifact for positively explained dimensional artifacts, and genuine_error for residual footing or missing-child errors. DQC rows emitted by the SQL subset are violations only, so failed DQC rows use genuine_error; future warning/skipped rules may use unresolved.',
                true,
                NULL,
                'derived by refresh_xbrl_validation_results from the dimensional verdict or DQC violation status',
                now()
            )
        """
    )


def _fundamental_ratio_provenance_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S8 S8-0: accession/filed-date provenance on statement points and ratios."""

    for statement in (
        "ALTER TABLE fundamental_statement_points ADD COLUMN IF NOT EXISTS source_accession VARCHAR",
        "ALTER TABLE fundamental_statement_points ADD COLUMN IF NOT EXISTS filed_date DATE",
        "ALTER TABLE fundamental_ratios ADD COLUMN IF NOT EXISTS source_accession VARCHAR",
        "ALTER TABLE fundamental_ratios ADD COLUMN IF NOT EXISTS filed_date DATE",
    ):
        conn.execute(statement)

    conn.execute(
        """
        UPDATE fundamental_statement_points
        SET source_accession = accession_number
        WHERE source_accession IS NULL
          AND accession_number IS NOT NULL
        """
    )
    conn.execute(
        """
        UPDATE fundamental_statement_points
        SET filed_date = as_of_date
        WHERE filed_date IS NULL
          AND as_of_date IS NOT NULL
        """
    )
    ratio_backfill_needed = bool(conn.execute(
        """
        SELECT EXISTS (
            SELECT 1
            FROM fundamental_ratios
            WHERE source_accession IS NULL
               OR filed_date IS NULL
            LIMIT 1
        )
        """
    ).fetchone()[0])
    table_names = {
        row[0]
        for row in conn.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE schema_name = 'main'
            """
        ).fetchall()
    }
    formula_registry_exists = "formula_registry" in table_names
    ttm_source = (
        "fundamental_ttm_points"
        if "fundamental_ttm_points" in table_names else
        """
        (
            SELECT
                CAST(NULL AS VARCHAR) AS ttm_point_id,
                CAST(NULL AS VARCHAR) AS security_id,
                CAST(NULL AS VARCHAR) AS canonical_metric,
                CAST(NULL AS DATE) AS ttm_end_date,
                CAST(NULL AS DATE) AS as_of_date,
                CAST(NULL AS TIMESTAMP) AS available_at,
                CAST(NULL AS VARCHAR) AS accession_number,
                CAST(NULL AS BOOLEAN) AS is_latest_revision
            WHERE false
        )
        """
    )
    xbrl_source = (
        "fundamental_xbrl_metric"
        if "fundamental_xbrl_metric" in table_names else
        """
        (
            SELECT
                CAST(NULL AS VARCHAR) AS metric_id,
                CAST(NULL AS VARCHAR) AS security_id,
                CAST(NULL AS VARCHAR) AS canonical_metric,
                CAST(NULL AS VARCHAR) AS period_type,
                CAST(NULL AS DATE) AS period_end,
                CAST(NULL AS DATE) AS as_of_date,
                CAST(NULL AS TIMESTAMP) AS available_at,
                CAST(NULL AS VARCHAR) AS accession_number,
                CAST(NULL AS BOOLEAN) AS is_latest_revision
            WHERE false
        )
        """
    )
    formula_join = (
        """
            LEFT JOIN formula_registry f
              ON f.formula_code = r.ratio_code
             AND f.valid_to IS NULL
        """
        if formula_registry_exists else ""
    )
    inputs_json_expr = "coalesce(r.input_codes_json, f.inputs_json)" if formula_registry_exists else "r.input_codes_json"

    if ratio_backfill_needed: conn.execute(
        f"""
        WITH base_input_map(input_key, source_group, canonical_metric) AS (
            VALUES
                ('rev', 'ttm', 'revenue'),
                ('ni', 'ttm', 'net_income'),
                ('oi', 'ttm', 'operating_income'),
                ('ocf', 'ttm', 'operating_cash_flow'),
                ('capex', 'ttm', 'capital_expenditures'),
                ('div', 'ttm', 'dividends_paid'),
                ('repurch', 'ttm', 'share_repurchases'),
                ('assets', 'balance', 'assets'),
                ('liabilities', 'balance', 'liabilities'),
                ('equity', 'balance', 'stockholders_equity'),
                ('shares', 'balance', 'shares_outstanding'),
                ('current_assets', 'xbrl_balance', 'current_assets'),
                ('current_liabilities', 'xbrl_balance', 'current_liabilities'),
                ('cash_and_equivalents', 'xbrl_balance', 'cash_and_equivalents'),
                ('inventory', 'xbrl_balance', 'inventory'),
                ('long_term_debt', 'xbrl_balance', 'long_term_debt'),
                ('retained_earnings', 'xbrl_balance', 'retained_earnings'),
                ('common_shares_outstanding', 'xbrl_balance', 'common_shares_outstanding'),
                ('property_plant_equipment_net', 'xbrl_balance', 'property_plant_equipment_net'),
                ('accounts_receivable', 'xbrl_balance', 'accounts_receivable'),
                ('accounts_payable', 'xbrl_balance', 'ap'),
                ('goodwill', 'xbrl_balance', 'goodwill'),
                ('intangibles_other', 'xbrl_balance', 'intangibles_other'),
                ('gross_profit', 'xbrl_flow', 'gross_profit'),
                ('cost_of_revenue', 'xbrl_flow', 'cost_of_revenue'),
                ('interest_expense', 'xbrl_flow', 'interest_expense'),
                ('depreciation_amortization', 'xbrl_flow', 'depreciation_amortization'),
                ('selling_general_and_administrative_expense', 'xbrl_flow', 'selling_general_and_administrative_expense'),
                ('pretax_income', 'xbrl_flow', 'pretax_income'),
                ('income_tax', 'xbrl_flow', 'income_tax'),
                ('shares_basic_avg', 'xbrl_flow', 'shares_basic_avg'),
                ('shares_diluted_avg', 'xbrl_flow', 'shares_diluted_avg')
        ),
        input_map AS (
            SELECT input_key, source_group, canonical_metric, false AS is_prior
            FROM base_input_map
            UNION ALL
            SELECT input_key || '_prior', source_group, canonical_metric, true AS is_prior
            FROM base_input_map
            WHERE input_key IN (
                'rev', 'ni', 'oi', 'ocf', 'assets', 'equity',
                'liabilities', 'long_term_debt', 'current_assets', 'current_liabilities',
                'common_shares_outstanding', 'gross_profit', 'cost_of_revenue',
                'depreciation_amortization', 'property_plant_equipment_net',
                'accounts_receivable', 'selling_general_and_administrative_expense',
                'cash_and_equivalents', 'inventory', 'accounts_payable'
            )
        ),
        ratio_seed AS (
            SELECT
                r.ratio_id,
                r.ratio_code,
                r.security_id,
                r.period_end,
                r.available_at,
                {inputs_json_expr} AS inputs_json
            FROM fundamental_ratios r
            {formula_join}
            WHERE {inputs_json_expr} IS NOT NULL
        ),
        ratio_inputs AS (
            SELECT
                rs.ratio_id,
                rs.security_id,
                rs.period_end,
                rs.available_at,
                u.input_key,
                u.ordinal
            FROM ratio_seed rs
            CROSS JOIN LATERAL unnest(json_extract(rs.inputs_json, '$')::VARCHAR[])
                WITH ORDINALITY AS u(input_key, ordinal)
        ),
        known_inputs AS (
            SELECT
                ri.ratio_id,
                ri.security_id,
                ri.period_end,
                ri.available_at,
                ri.input_key,
                ri.ordinal,
                im.source_group,
                im.canonical_metric,
                im.is_prior
            FROM ratio_inputs ri
            JOIN input_map im ON im.input_key = ri.input_key
        ),
        ttm_candidates AS (
            SELECT
                ki.ratio_id,
                ki.input_key,
                ki.ordinal,
                coalesce(t.available_at, CAST(t.as_of_date AS TIMESTAMP)) AS candidate_available_at,
                t.accession_number AS source_accession,
                t.as_of_date AS filed_date
            FROM known_inputs ki
            JOIN {ttm_source} t
              ON t.security_id = ki.security_id
             AND t.canonical_metric = ki.canonical_metric
             AND t.is_latest_revision
             AND (
                    (NOT ki.is_prior AND t.ttm_end_date = ki.period_end)
                 OR (ki.is_prior
                     AND t.ttm_end_date < ki.period_end
                     AND date_diff('day', t.ttm_end_date, ki.period_end) BETWEEN 350 AND 380)
             )
            WHERE ki.source_group = 'ttm'
            QUALIFY row_number() OVER (
                PARTITION BY ki.ratio_id, ki.input_key
                ORDER BY
                    CASE
                        WHEN ki.is_prior THEN abs(date_diff('day', t.ttm_end_date, ki.period_end) - 365)
                        ELSE 0
                    END,
                    candidate_available_at DESC NULLS LAST,
                    t.as_of_date DESC NULLS LAST,
                    t.accession_number DESC NULLS LAST,
                    t.ttm_point_id DESC NULLS LAST
            ) = 1
        ),
        balance_candidates AS (
            SELECT
                ki.ratio_id,
                ki.input_key,
                ki.ordinal,
                coalesce(
                    s.available_at,
                    CAST(coalesce(s.filed_date, s.as_of_date) AS TIMESTAMP)
                ) AS candidate_available_at,
                coalesce(s.source_accession, s.accession_number) AS source_accession,
                coalesce(s.filed_date, s.as_of_date) AS filed_date
            FROM known_inputs ki
            JOIN fundamental_statement_points s
              ON s.security_id = ki.security_id
             AND s.canonical_metric = ki.canonical_metric
             AND s.is_latest_revision
             AND s.period_type = 'instant'
             AND (
                    (NOT ki.is_prior AND s.period_end = ki.period_end)
                 OR (ki.is_prior
                     AND s.period_end < ki.period_end
                     AND date_diff('day', s.period_end, ki.period_end) BETWEEN 350 AND 380)
             )
            WHERE ki.source_group = 'balance'
            QUALIFY row_number() OVER (
                PARTITION BY ki.ratio_id, ki.input_key
                ORDER BY
                    CASE
                        WHEN ki.is_prior THEN abs(date_diff('day', s.period_end, ki.period_end) - 365)
                        ELSE 0
                    END,
                    candidate_available_at DESC NULLS LAST,
                    filed_date DESC NULLS LAST,
                    source_accession DESC NULLS LAST,
                    s.statement_point_id DESC NULLS LAST
            ) = 1
        ),
        xbrl_balance_candidates AS (
            SELECT
                ki.ratio_id,
                ki.input_key,
                ki.ordinal,
                x.available_at AS candidate_available_at,
                x.accession_number AS source_accession,
                x.as_of_date AS filed_date
            FROM known_inputs ki
            JOIN {xbrl_source} x
              ON x.security_id = ki.security_id
             AND x.canonical_metric = ki.canonical_metric
             AND x.is_latest_revision
             AND x.period_type = 'instant'
             AND (
                    (NOT ki.is_prior AND x.period_end = ki.period_end)
                 OR (ki.is_prior
                     AND x.period_end < ki.period_end
                     AND date_diff('day', x.period_end, ki.period_end) BETWEEN 350 AND 380)
             )
            WHERE ki.source_group = 'xbrl_balance'
            QUALIFY row_number() OVER (
                PARTITION BY ki.ratio_id, ki.input_key
                ORDER BY
                    CASE
                        WHEN ki.is_prior THEN abs(date_diff('day', x.period_end, ki.period_end) - 365)
                        ELSE 0
                    END,
                    candidate_available_at DESC NULLS LAST,
                    x.as_of_date DESC NULLS LAST,
                    x.accession_number DESC NULLS LAST,
                    x.metric_id DESC NULLS LAST
            ) = 1
        ),
        xbrl_flow_candidates AS (
            SELECT
                ki.ratio_id,
                ki.input_key,
                ki.ordinal,
                x.available_at AS candidate_available_at,
                x.accession_number AS source_accession,
                x.as_of_date AS filed_date
            FROM known_inputs ki
            JOIN {xbrl_source} x
              ON x.security_id = ki.security_id
             AND x.canonical_metric = ki.canonical_metric
             AND x.is_latest_revision
             AND x.period_type = 'duration'
             AND (
                    (NOT ki.is_prior AND x.period_end = ki.period_end)
                 OR (ki.is_prior
                     AND x.period_end < ki.period_end
                     AND date_diff('day', x.period_end, ki.period_end) BETWEEN 350 AND 380)
             )
            WHERE ki.source_group = 'xbrl_flow'
            QUALIFY row_number() OVER (
                PARTITION BY ki.ratio_id, ki.input_key
                ORDER BY
                    CASE
                        WHEN ki.is_prior THEN abs(date_diff('day', x.period_end, ki.period_end) - 365)
                        ELSE 0
                    END,
                    candidate_available_at DESC NULLS LAST,
                    x.as_of_date DESC NULLS LAST,
                    x.accession_number DESC NULLS LAST,
                    x.metric_id DESC NULLS LAST
            ) = 1
        ),
        source_candidates AS (
            SELECT * FROM ttm_candidates
            UNION ALL
            SELECT * FROM balance_candidates
            UNION ALL
            SELECT * FROM xbrl_balance_candidates
            UNION ALL
            SELECT * FROM xbrl_flow_candidates
        ),
        ranked_provenance AS (
            SELECT
                sc.*,
                rs.available_at AS ratio_available_at,
                row_number() OVER (
                    PARTITION BY sc.ratio_id
                    ORDER BY
                        CASE
                            WHEN sc.candidate_available_at = rs.available_at
                             AND (sc.source_accession IS NOT NULL OR sc.filed_date IS NOT NULL)
                                THEN 0
                            WHEN sc.source_accession IS NOT NULL OR sc.filed_date IS NOT NULL
                                THEN 1
                            WHEN sc.candidate_available_at = rs.available_at
                                THEN 2
                            ELSE 3
                        END,
                        sc.candidate_available_at DESC NULLS LAST,
                        sc.ordinal ASC,
                        sc.source_accession DESC NULLS LAST,
                        sc.filed_date DESC NULLS LAST
                ) AS provenance_rank
            FROM source_candidates sc
            JOIN ratio_seed rs ON rs.ratio_id = sc.ratio_id
        )
        UPDATE fundamental_ratios AS r
        SET
            source_accession = coalesce(p.source_accession, r.source_accession),
            filed_date = coalesce(p.filed_date, r.filed_date)
        FROM ranked_provenance p
        WHERE r.ratio_id = p.ratio_id
          AND p.provenance_rank = 1
          AND (p.source_accession IS NOT NULL OR p.filed_date IS NOT NULL)
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            (
                'fundamental_statement_points',
                'source_accession',
                'identifier',
                'SEC accession number copied from accession_number for statement-point provenance. This is the source filing for the normalized fact revision.',
                true,
                NULL,
                'fundamental_fact_revisions.accession_number',
                now()
            ),
            (
                'fundamental_statement_points',
                'filed_date',
                'date',
                'SEC filing date for the normalized statement-point source fact; mirrors as_of_date in the current companyfacts revision path.',
                true,
                'date',
                'fundamental_fact_revisions.filed_date',
                now()
            ),
            (
                'fundamental_ratios',
                'source_accession',
                'identifier',
                'SEC accession number for the consumed input row whose availability drives this derived ratio vintage; if the driver lacks provenance, the latest consumed input with provenance is used deterministically.',
                true,
                NULL,
                'consumed ratio input source accession',
                now()
            ),
            (
                'fundamental_ratios',
                'filed_date',
                'date',
                'SEC filing date for the consumed input row whose availability drives this derived ratio vintage; if the driver lacks provenance, the latest consumed input with provenance is used deterministically.',
                true,
                'date',
                'consumed ratio input source filed date',
                now()
            )
        """
    )


def _fundamental_ratio_provenance_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF-S8 S8-0 lookup indexes for accession-level statement/ratio lineage."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ratios_source_accession ON fundamental_ratios(security_id, period_end, source_accession)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_statement_points_source_accession ON fundamental_statement_points(security_id, period_end, source_accession)",
    ):
        conn.execute(statement)


def _schema_contract_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S1 S1-0/S1-2: schema_contract table -- the declarative manifest persisted as data.

    Persists db/schema_contract.py::build_contract_manifest(conn) (one row per (table,
    column) pair across EVERY live table, both imperative schema paths reconciled and
    each tagged declared_in in {schema_py, migration}) into a queryable schema_contract
    table, plus its own table_catalog/field_catalog rows per (B). Indexes land in migration
    0098 (schema-vs-index split per the S5g/S5k WAL-replay precedent -- see
    _formula_registry_schema_catalog/_formula_registry_indexes for the same split).

    PF2-S1 S1-2 made this seed source build_contract_manifest(conn) rather than the bare
    CONTRACT subset, so the persisted rows and the Python manifest are one source of
    truth: build_contract_manifest() reuses CONTRACT verbatim wherever CONTRACT already
    declares a table and derives every other live table (including schema_contract
    itself, since this function creates the table before computing the manifest) from
    this connection's own duckdb_tables()/duckdb_columns(). detect_schema_drift() can
    still be called with an explicit narrower contract (e.g. the bare CONTRACT, or a
    fixture) for scoped checks; this table is the durable, plain-SQL-queryable mirror of
    the FULL reconciled manifest. manifest_sha256 (schema_contract_sha256() over the
    sorted manifest) is denormalized onto every row so a single `SELECT DISTINCT
    manifest_sha256` gives the stable baseline to compare across bootstraps/versions.
    """
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS schema_contract (
            table_name VARCHAR NOT NULL,
            column_name VARCHAR NOT NULL,
            data_type VARCHAR NOT NULL,
            nullable BOOLEAN NOT NULL,
            is_natural_key BOOLEAN NOT NULL DEFAULT false,
            is_pit_column BOOLEAN NOT NULL DEFAULT false,
            declared_in VARCHAR NOT NULL,
            manifest_sha256 VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (table_name, column_name)
        )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'schema_contract',
            'control',
            'schema_contract_row',
            'table_name,column_name',
            'Declarative warehouse schema contract: one row per manifest (table, column) pair persisted from db/schema_contract.py::CONTRACT, tagged declared_in (schema_py|migration). The queryable/durable mirror of the manifest detect_schema_drift diffs live duckdb_tables()/duckdb_columns() against.',
            '["table_name","column_name"]',
            'manifest_sha256 is the stable hash (schema_contract.py::schema_contract_sha256) over the sorted manifest at seed time; compare across bootstraps to detect manifest drift itself. This table documents the CONTRACT, not fact rows, so it carries no as_of_date/available_at PIT columns of its own.',
            now()
        )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('schema_contract', 'table_name', 'identifier', 'Contracted table name.', false, NULL, NULL, now()),
            ('schema_contract', 'column_name', 'identifier', 'Contracted column name within table_name.', false, NULL, NULL, now()),
            ('schema_contract', 'data_type', 'category', 'Declared DuckDB data type (matches duckdb_columns().data_type spelling, e.g. VARCHAR, TIMESTAMP).', false, NULL, NULL, now()),
            ('schema_contract', 'nullable', 'flag', 'Declared nullability for the column.', false, NULL, NULL, now()),
            ('schema_contract', 'is_natural_key', 'flag', 'True when the column participates in the table''s natural/business key (per table_catalog.natural_key_json), independent of its surrogate PRIMARY KEY if any.', false, NULL, NULL, now()),
            ('schema_contract', 'is_pit_column', 'flag', 'True for the canonical bitemporal PIT columns (as_of_date, available_at, source_loaded_at, run_id, is_latest_revision) where the fact/derived-row mandate applies to this table.', false, NULL, NULL, now()),
            ('schema_contract', 'declared_in', 'category', 'Which imperative code path declares this column: schema_py (schema.py::ensure_quant_schema) or migration (migrations.py::MIGRATIONS). Column-level, not table-level: a schema.py table can gain later columns via a migration ALTER TABLE.', false, NULL, NULL, now()),
            ('schema_contract', 'manifest_sha256', 'identifier', 'schema_contract_sha256() over the sorted manifest at seed time; a stable baseline for detecting manifest changes across re-bootstraps.', false, NULL, NULL, now()),
            ('schema_contract', 'source_loaded_at', 'timestamp', 'Warehouse-assigned timestamp when this manifest row was seeded (DEFAULT now()).', false, NULL, NULL, now())
        """
    )

    from .schema_contract import build_contract_manifest, schema_contract_sha256

    manifest = build_contract_manifest(conn)
    manifest_sha256 = schema_contract_sha256(manifest)
    columns = [
        "table_name",
        "column_name",
        "data_type",
        "nullable",
        "is_natural_key",
        "is_pit_column",
        "declared_in",
        "manifest_sha256",
    ]
    rows = [
        (
            table_name,
            spec.name,
            spec.data_type,
            spec.nullable,
            spec.is_natural_key,
            spec.is_pit_column,
            spec.declared_in,
            manifest_sha256,
        )
        for table_name, specs in manifest.items()
        for spec in specs
    ]
    import pandas as pd

    seed = pd.DataFrame.from_records(rows, columns=columns)
    conn.register("_schema_contract_seed", seed)
    try:
        conn.execute(
            """
            INSERT OR REPLACE INTO schema_contract (
                table_name, column_name, data_type, nullable, is_natural_key, is_pit_column,
                declared_in, manifest_sha256, source_loaded_at
            )
            SELECT
                table_name, column_name, data_type, nullable, is_natural_key, is_pit_column,
                declared_in, manifest_sha256, now()
            FROM _schema_contract_seed
            """
        )
    finally:
        conn.unregister("_schema_contract_seed")


def _schema_contract_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S1 S1-0: lookup indexes split from schema migration 0097."""
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_schema_contract_table_name ON schema_contract(table_name)",
        "CREATE INDEX IF NOT EXISTS idx_schema_contract_declared_in ON schema_contract(declared_in)",
    ):
        conn.execute(statement)


def _warehouse_catalog_view(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S1 S1-3: v_warehouse_catalog -- the queryable/as-of warehouse data-catalog surface.

    pf1-S4-3 made *formulas* queryable (v_formula_registry / formula_registry_asof), but
    "what did the warehouse catalog look like" (which tables/fields exist, their layer,
    grain, PIT notes) was only readable by introspecting a live connection. This view is
    one row per (table_name, field_name), LEFT JOINing table_catalog + field_catalog (so a
    catalogued table with no field rows -- e.g. a fresh view row inserted before its column
    descriptions land -- still appears), plus a best-effort LEFT JOIN onto
    v_formula_registry for catalogued formula surfaces (table_catalog.entity = 'formula'),
    keyed on field_catalog.field_name = v_formula_registry.formula_code. Today's only
    entity='formula' catalog row is formula_registry/v_formula_registry itself, whose own
    field_catalog rows describe SCHEMA columns (formula_code, family, kind, ...) rather than
    literal formula codes, so that join is structurally present but ordinarily contributes no
    matches in production -- exactly the "best-effort" formula-lineage column set the S1-3
    task brief calls for; a future entity='formula' catalog row keyed by literal formula_code
    values would light these columns up for real.

    Per (B)/S7a, views are catalogued like tables: this view gets its own table_catalog +
    field_catalog rows (mirroring the _formula_registry_catalog_view precedent --
    migrations.py's formula_registry-view migration -- and, further back,
    _catalog_backfill_reference_and_views' v_offexchange_security_period). Unlike that
    precedent, this view's output columns are renamed/aliased (table_description vs
    field_description, table_updated_at vs field_updated_at, formula_* lineage columns), so
    the field_catalog rows below are hand-written literals (mirroring
    _schema_contract_schema_catalog's own-column seeding) rather than re-parented verbatim
    from a single source table's field_catalog rows.

    PIT note: table_catalog/field_catalog carry ``updated_at`` (knowledge time), NOT
    valid_from/valid_to DEFINITION validity like formula_registry. The as-of PYTHON reader
    that filters this view by ``updated_at <= effective_ts`` (no lookahead: a catalog row
    updated AFTER the as-of instant is excluded) is ``warehouse_catalog_asof`` in
    ``db/asof.py``; this view itself is the always-current (unfiltered) catalog surface a
    plain ``SELECT`` sees.
    """
    formula_registry_view_exists = (
        conn.execute(
            "SELECT count(*) FROM duckdb_views() WHERE view_name = 'v_formula_registry'"
        ).fetchone()[0]
        > 0
    )
    if formula_registry_view_exists:
        formula_columns_sql = """
            vfr.formula_code,
            vfr.family AS formula_family,
            vfr.kind AS formula_kind,
            vfr.unit AS formula_unit,
            vfr.expression AS formula_expression,
            vfr.citation AS formula_citation,
            vfr.valid_from AS formula_valid_from,
            vfr.valid_to AS formula_valid_to
        """
        formula_join_sql = """
        LEFT JOIN v_formula_registry vfr
            ON t.entity = 'formula' AND vfr.formula_code = f.field_name
        """
    else:
        # Some legacy bootstraps have formula-registry migrations recorded without the
        # physical view present. Keep 0099 append-only and expose the same catalog shape.
        formula_columns_sql = """
            CAST(NULL AS VARCHAR) AS formula_code,
            CAST(NULL AS VARCHAR) AS formula_family,
            CAST(NULL AS VARCHAR) AS formula_kind,
            CAST(NULL AS VARCHAR) AS formula_unit,
            CAST(NULL AS VARCHAR) AS formula_expression,
            CAST(NULL AS VARCHAR) AS formula_citation,
            CAST(NULL AS DATE) AS formula_valid_from,
            CAST(NULL AS DATE) AS formula_valid_to
        """
        formula_join_sql = ""

    conn.execute(
        f"""
        CREATE OR REPLACE VIEW v_warehouse_catalog AS
        SELECT
            t.table_name,
            t.layer,
            t.entity,
            t.grain,
            t.description AS table_description,
            t.natural_key_json,
            t.pit_notes,
            t.updated_at AS table_updated_at,
            f.field_name,
            f.semantic_type,
            f.description AS field_description,
            f.nullable AS field_nullable,
            f.unit AS field_unit,
            f.source_field,
            f.updated_at AS field_updated_at,
            {formula_columns_sql}
        FROM table_catalog t
        LEFT JOIN field_catalog f ON f.table_name = t.table_name
        {formula_join_sql}
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'v_warehouse_catalog',
                'gold',
                'catalog',
                'table_name,field_name',
                'Queryable as-of warehouse data-catalog surface: one row per (table_name, field_name) joining table_catalog + field_catalog (LEFT JOIN so a catalogued table with no field rows still appears), plus best-effort formula lineage via v_formula_registry for catalogued formula surfaces (entity = ''formula''). Answers "what did the warehouse catalog look like" as one SELECT-able view, so table/field documentation is data, not a live-connection introspection.',
                '["table_name","field_name"]',
                'View over table_catalog/field_catalog/v_formula_registry (no filtering in the view itself). For a point-in-time read honoring catalog KNOWLEDGE time, use warehouse_catalog_asof(as_of_date, ...) in db/asof.py, which filters table_updated_at/field_updated_at <= effective_ts (no lookahead) -- unlike formula_registry_asof, which instead gates on the formula DEFINITION''s own valid_from/valid_to.',
                now()
            )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('v_warehouse_catalog', 'table_name', 'identifier',
             'Catalogued table or view name (table_catalog.table_name).',
             false, NULL, 'table_catalog.table_name', now()),
            ('v_warehouse_catalog', 'layer', 'category',
             'Warehouse layer (bronze|silver|gold|control|audit|dimension|catalog|...).',
             true, NULL, 'table_catalog.layer', now()),
            ('v_warehouse_catalog', 'entity', 'category',
             'Business entity the table/view represents.',
             true, NULL, 'table_catalog.entity', now()),
            ('v_warehouse_catalog', 'grain', 'text',
             'Row grain of the table/view (natural key columns, human-readable).',
             true, NULL, 'table_catalog.grain', now()),
            ('v_warehouse_catalog', 'table_description', 'text',
             'Table/view-level catalog description.',
             true, NULL, 'table_catalog.description', now()),
            ('v_warehouse_catalog', 'natural_key_json', 'json',
             'JSON array of natural/business key column names.',
             true, NULL, 'table_catalog.natural_key_json', now()),
            ('v_warehouse_catalog', 'pit_notes', 'text',
             'Free-text point-in-time usage notes for the table/view.',
             true, NULL, 'table_catalog.pit_notes', now()),
            ('v_warehouse_catalog', 'table_updated_at', 'timestamp',
             'Knowledge-time timestamp the table_catalog row was last updated; the PIT gate warehouse_catalog_asof filters on (no lookahead).',
             false, NULL, 'table_catalog.updated_at', now()),
            ('v_warehouse_catalog', 'field_name', 'identifier',
             'Catalogued column name within table_name; NULL when the table has no field_catalog rows yet (LEFT JOIN).',
             true, NULL, 'field_catalog.field_name', now()),
            ('v_warehouse_catalog', 'semantic_type', 'category',
             'Column-level semantic type (identifier, category, text, json, flag, timestamp, date, ...).',
             true, NULL, 'field_catalog.semantic_type', now()),
            ('v_warehouse_catalog', 'field_description', 'text',
             'Column-level catalog description.',
             true, NULL, 'field_catalog.description', now()),
            ('v_warehouse_catalog', 'field_nullable', 'flag',
             'Declared nullability of the column per field_catalog.',
             true, NULL, 'field_catalog.nullable', now()),
            ('v_warehouse_catalog', 'field_unit', 'category',
             'Column-level unit, where applicable.',
             true, NULL, 'field_catalog.unit', now()),
            ('v_warehouse_catalog', 'source_field', 'text',
             'Upstream/source field this column derives from, where applicable.',
             true, NULL, 'field_catalog.source_field', now()),
            ('v_warehouse_catalog', 'field_updated_at', 'timestamp',
             'Knowledge-time timestamp the field_catalog row was last updated; the PIT gate warehouse_catalog_asof filters on (no lookahead) when a field row is present.',
             true, NULL, 'field_catalog.updated_at', now()),
            ('v_warehouse_catalog', 'formula_code', 'identifier',
             'Best-effort formula lineage: matching v_formula_registry.formula_code when table_catalog.entity = ''formula''; NULL otherwise or when no formula_registry row matches.',
             true, NULL, 'v_formula_registry.formula_code', now()),
            ('v_warehouse_catalog', 'formula_family', 'category',
             'Best-effort formula lineage: v_formula_registry.family.',
             true, NULL, 'v_formula_registry.family', now()),
            ('v_warehouse_catalog', 'formula_kind', 'category',
             'Best-effort formula lineage: v_formula_registry.kind.',
             true, NULL, 'v_formula_registry.kind', now()),
            ('v_warehouse_catalog', 'formula_unit', 'category',
             'Best-effort formula lineage: v_formula_registry.unit.',
             true, NULL, 'v_formula_registry.unit', now()),
            ('v_warehouse_catalog', 'formula_expression', 'text',
             'Best-effort formula lineage: v_formula_registry.expression.',
             true, NULL, 'v_formula_registry.expression', now()),
            ('v_warehouse_catalog', 'formula_citation', 'text',
             'Best-effort formula lineage: v_formula_registry.citation.',
             true, NULL, 'v_formula_registry.citation', now()),
            ('v_warehouse_catalog', 'formula_valid_from', 'date',
             'Best-effort formula lineage: v_formula_registry.valid_from.',
             true, NULL, 'v_formula_registry.valid_from', now()),
            ('v_warehouse_catalog', 'formula_valid_to', 'date',
             'Best-effort formula lineage: v_formula_registry.valid_to.',
             true, NULL, 'v_formula_registry.valid_to', now())
        """
    )


def _migration_governance_schema(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S2 S2-0: migration governance lock, backup registry shell, and checksum baseline."""
    _ensure_apply_lock_table(conn)
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS migration_backup_registry (
            backup_id VARCHAR PRIMARY KEY,
            run_id VARCHAR NOT NULL,
            label VARCHAR,
            database_path VARCHAR,
            backup_path VARCHAR NOT NULL,
            wal_backup_path VARCHAR,
            sha256 VARCHAR NOT NULL,
            byte_size BIGINT NOT NULL,
            versions_before VARCHAR NOT NULL,
            versions_after VARCHAR,
            created_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description, natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'migration_apply_lock',
                'control',
                'migration_governance',
                'singleton lock_name',
                'Singleton advisory apply-lock row used to prevent concurrent warehouse migration apply loops.',
                '["lock_name"]',
                'Control table only. holder_run_id and heartbeat_at describe the current migration apply holder, not business-time facts.',
                now()
            ),
            (
                'migration_backup_registry',
                'control',
                'migration_backup',
                'backup_id',
                'Schema-only registry for governed migration database backups. S2-1 writes backup artifacts and version ranges here.',
                '["backup_id"]',
                'Control table only. created_at is registry knowledge time for backup artifacts, not a fact effective date.',
                now()
            )
        """
    )

    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES
            ('migration_apply_lock', 'lock_name', 'identifier', 'Singleton lock key for the migration apply loop.', false, NULL, NULL, now()),
            ('migration_apply_lock', 'holder_run_id', 'identifier', 'Run id that currently holds the migration apply lock.', false, NULL, NULL, now()),
            ('migration_apply_lock', 'heartbeat_at', 'timestamp', 'Timestamp when the apply-lock row was acquired or last heartbeated.', false, NULL, NULL, now()),
            ('migration_backup_registry', 'backup_id', 'identifier', 'Stable id for a migration backup artifact set.', false, NULL, NULL, now()),
            ('migration_backup_registry', 'run_id', 'identifier', 'Governed migration run id that created the backup.', false, NULL, NULL, now()),
            ('migration_backup_registry', 'label', 'category', 'Operator or tool-supplied backup label.', true, NULL, NULL, now()),
            ('migration_backup_registry', 'database_path', 'path', 'Source DuckDB database path that was backed up.', true, NULL, NULL, now()),
            ('migration_backup_registry', 'backup_path', 'path', 'Path to the primary DuckDB database backup artifact.', false, NULL, NULL, now()),
            ('migration_backup_registry', 'wal_backup_path', 'path', 'Path to the copied DuckDB WAL backup artifact when one existed.', true, NULL, NULL, now()),
            ('migration_backup_registry', 'sha256', 'identifier', 'SHA-256 digest recorded for the primary backup artifact.', false, NULL, NULL, now()),
            ('migration_backup_registry', 'byte_size', 'count', 'Size in bytes recorded for the primary backup artifact.', false, 'bytes', NULL, now()),
            ('migration_backup_registry', 'versions_before', 'json', 'JSON-encoded numeric migration versions present before the governed apply.', false, NULL, NULL, now()),
            ('migration_backup_registry', 'versions_after', 'json', 'JSON-encoded numeric migration versions present after the governed apply.', true, NULL, NULL, now()),
            ('migration_backup_registry', 'created_at', 'timestamp', 'Warehouse timestamp when the backup registry row was created.', false, NULL, NULL, now())
        """
    )

    _backfill_missing_migration_checksums(conn)
    _schema_contract_schema_catalog(conn)


def _migration_governance_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S2 S2-0: lookup indexes split from migration governance schema."""
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_migration_apply_lock_heartbeat_at ON migration_apply_lock(heartbeat_at)",
        "CREATE INDEX IF NOT EXISTS idx_migration_backup_registry_run_id ON migration_backup_registry(run_id)",
        "CREATE INDEX IF NOT EXISTS idx_migration_backup_registry_created_at ON migration_backup_registry(created_at)",
        "CREATE INDEX IF NOT EXISTS idx_migration_backup_registry_backup_path ON migration_backup_registry(backup_path)",
    ):
        conn.execute(statement)


def _fundamental_standardized_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S3 S3-0: standardized comparable fundamental item fact."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_standardized (
            standardized_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            upstream_source VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            item_id INTEGER NOT NULL,
            canonical_code VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            value DOUBLE NOT NULL,
            unit_type VARCHAR,
            source_accession VARCHAR,
            filed_date DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            input_codes_json VARCHAR NOT NULL,
            input_item_ids_json VARCHAR NOT NULL,
            rule_id VARCHAR NOT NULL,
            combination_rule VARCHAR NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_standardized',
            'sec_edgar',
            'Standardized comparable fundamental items',
            'Comparable long-format PIT facts produced by db.standardization from declarative standardization_rules.csv, one row per canonical fundamental_item item and reporting basis.',
            'security_id,item_id,period_end,basis',
            'fundamental_standardized', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'fundamental_standardized',
            'gold',
            'fundamental_standardized',
            'security_id,item_id,period_end,basis',
            'Comparable standardized fundamental item values emitted by a deterministic rule engine over statement, TTM, and XBRL candidate facts.',
            '["source", "security_id", "item_id", "period_end", "basis", "available_at"]',
            'Use as_of_date/available_at and is_latest_revision for PIT-safe standardized item reads; available_at is the max availability of the input facts used by the rule.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('standardized_id', 'security_id', 'cik', 'source_accession', 'run_id', 'rule_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('period_start', 'period_end', 'filed_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN lower(c.column_name) IN ('is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'value' THEN 'Standardized comparable item value after the rule combination, sign, and scale dispatch.'
                WHEN 'item_id' THEN 'Canonical fundamental_item.item_id emitted by the standardization rule.'
                WHEN 'basis' THEN 'Reporting basis of the standardized item: instant, annual, or ttm.'
                WHEN 'input_codes_json' THEN 'JSON list of source taxonomy/concept or warehouse metric codes consumed by the rule.'
                WHEN 'input_item_ids_json' THEN 'JSON list of source item_ids consumed by the rule.'
                WHEN 'available_at' THEN 'Timestamp when all input facts used by the standardized value were knowable.'
                ELSE replace(c.column_name, '_', ' ') || ' field on fundamental_standardized.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) IN ('period_start', 'period_end', 'filed_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'fundamental_standardized'
        """
    )


def _fundamental_standardized_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S3 S3-0: lookup indexes for standardized fundamentals."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_standardized_item ON fundamental_standardized(item_id, basis, period_end)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_standardized_security ON fundamental_standardized(security_id, period_end, basis)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_standardized_asof ON fundamental_standardized(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_standardized_latest ON fundamental_standardized(is_latest_revision, basis)",
    ):
        conn.execute(statement)


def _fundamental_standardization_exception_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S3 S3-1: exception table for unrouted standardization inputs."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_standardization_exception (
            exception_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            upstream_source VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            basis VARCHAR,
            period_start DATE,
            period_end DATE,
            accession_number VARCHAR,
            concept VARCHAR,
            taxonomy VARCHAR,
            unit VARCHAR,
            value DOUBLE,
            reason VARCHAR NOT NULL,
            as_of_date DATE,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        "ALTER TABLE fundamental_standardization_exception ADD COLUMN IF NOT EXISTS is_latest_revision BOOLEAN DEFAULT true"
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_std_exception_security ON fundamental_standardization_exception(security_id, period_end, basis)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_std_exception_reason ON fundamental_standardization_exception(reason)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_std_exception_asof ON fundamental_standardization_exception(as_of_date, available_at)",
    ):
        conn.execute(statement)

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_standardization_exception',
            'sec_edgar',
            'Fundamental standardization exceptions',
            'Every candidate filing tag that could not route to a canonical standardization rule, preserving custom-extension and unmapped-concept evidence instead of silently dropping it.',
            'security_id,period_end,concept,reason',
            'fundamental_standardization_exception', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'fundamental_standardization_exception',
            'silver',
            'fundamental_standardization_exception',
            'security_id,period_end,concept,reason',
            'Audit table for standardization inputs that did not resolve to an active rule, including unmapped custom XBRL extensions.',
            '["exception_id"]',
            'Use available_at and as_of_date to inspect exception visibility without lookahead; exception rates feed the S3 quality gate.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('exception_id', 'security_id', 'cik', 'accession_number', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('period_start', 'period_end', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'concept' THEN 'Raw source concept or metric that failed routing.'
                WHEN 'reason' THEN 'Closed exception reason, such as unmapped_concept or no_active_standardization_rule.'
                WHEN 'available_at' THEN 'Timestamp when the failed candidate input was knowable.'
                ELSE replace(c.column_name, '_', ' ') || ' field on fundamental_standardization_exception.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) IN ('period_start', 'period_end', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'fundamental_standardization_exception'
        """
    )


def _fundamental_standardization_coverage_view(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S3 S3-3: coverage view over standardized item population."""

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_fundamental_standardization_coverage AS
        WITH template AS (
            SELECT 'annual' AS basis, 126.0 AS template_item_count
            UNION ALL SELECT 'ttm' AS basis, 126.0 AS template_item_count
            UNION ALL SELECT 'instant' AS basis, 48.0 AS template_item_count
        ),
        std AS (
            SELECT
                source,
                security_id,
                any_value(symbol) AS symbol,
                any_value(cik) AS cik,
                period_end,
                basis,
                max(available_at) AS available_at,
                count(DISTINCT item_id)::DOUBLE AS populated_item_count,
                sum(CASE WHEN combination_rule IN ('identity', 'coalesce_priority') THEN 1 ELSE 0 END)::DOUBLE AS direct_or_coalesce_count,
                sum(CASE WHEN combination_rule = 'sum' THEN 1 ELSE 0 END)::DOUBLE AS sum_count,
                sum(CASE WHEN combination_rule = 'difference' THEN 1 ELSE 0 END)::DOUBLE AS difference_count,
                sum(CASE WHEN combination_rule = 'first_non_null' THEN 1 ELSE 0 END)::DOUBLE AS first_non_null_count
            FROM fundamental_standardized
            WHERE is_latest_revision
            GROUP BY source, security_id, period_end, basis
        ),
        exc AS (
            SELECT
                source,
                security_id,
                any_value(symbol) AS symbol,
                any_value(cik) AS cik,
                period_end,
                basis,
                max(available_at) AS available_at,
                count(*)::DOUBLE AS exception_count
            FROM fundamental_standardization_exception
            GROUP BY source, security_id, period_end, basis
        ),
        keys AS (
            SELECT source, security_id, symbol, cik, period_end, basis FROM std
            UNION
            SELECT source, security_id, symbol, cik, period_end, basis FROM exc
        )
        SELECT
            k.source,
            k.security_id,
            coalesce(s.symbol, e.symbol, k.symbol) AS symbol,
            coalesce(s.cik, e.cik, k.cik) AS cik,
            k.period_end,
            k.basis,
            greatest(coalesce(s.available_at, TIMESTAMP '1900-01-01'), coalesce(e.available_at, TIMESTAMP '1900-01-01')) AS available_at,
            coalesce(s.populated_item_count, 0.0) AS populated_item_count,
            coalesce(t.template_item_count, 0.0) AS template_item_count,
            CASE
                WHEN coalesce(t.template_item_count, 0.0) = 0.0 THEN NULL
                ELSE coalesce(s.populated_item_count, 0.0) / t.template_item_count
            END AS coverage_ratio,
            coalesce(s.direct_or_coalesce_count, 0.0) AS direct_or_coalesce_count,
            coalesce(s.sum_count, 0.0) AS sum_count,
            coalesce(s.difference_count, 0.0) AS difference_count,
            coalesce(s.first_non_null_count, 0.0) AS first_non_null_count,
            coalesce(e.exception_count, 0.0) AS exception_count
        FROM keys k
        LEFT JOIN std s
          ON s.source = k.source
         AND s.security_id = k.security_id
         AND s.period_end = k.period_end
         AND s.basis = k.basis
        LEFT JOIN exc e
          ON e.source = k.source
         AND e.security_id = k.security_id
         AND e.period_end = k.period_end
         AND e.basis = k.basis
        LEFT JOIN template t ON t.basis = k.basis
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'v_fundamental_standardization_coverage',
            'gold',
            'fundamental_standardization_coverage',
            'security_id,period_end,basis',
            'Coverage view reporting populated standardized item count versus the canonical template count, plus exception counts by security-period-basis.',
            '["source", "security_id", "period_end", "basis"]',
            'Coverage is derived from standardized rows and exception rows using their available_at timestamps; filter by available_at for PIT-safe proof-slice reporting.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('security_id') THEN 'identifier'
                WHEN lower(c.column_name) = 'period_end' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            replace(c.column_name, '_', ' ') || ' field on v_fundamental_standardization_coverage.' AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) = 'period_end' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%ratio%' THEN 'ratio'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'v_fundamental_standardization_coverage'
        """
    )
    _schema_contract_schema_catalog(conn)


def _fundamental_pit_snapshot_schema(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S4 S4-0/S4-1: ratio vintage column + PIT month snapshot table."""

    conn.execute(
        "ALTER TABLE fundamental_ratios ADD COLUMN IF NOT EXISTS vintage_class VARCHAR DEFAULT 'most_recently_restated'"
    )
    conn.execute(
        """
        UPDATE fundamental_ratios
        SET vintage_class = CASE
            WHEN is_latest_revision THEN 'most_recently_restated'
            ELSE 'as_first_reported'
        END
        WHERE vintage_class IS NULL
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_pit_snapshot (
            snapshot_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            snapshot_month DATE NOT NULL,
            upstream_source VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            canonical_metric VARCHAR NOT NULL,
            item_id INTEGER,
            basis VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            value DOUBLE,
            unit VARCHAR,
            unit_type VARCHAR,
            vintage_class VARCHAR NOT NULL,
            source_accession VARCHAR,
            filed_date DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            input_codes_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )


def _fundamental_pit_snapshot_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S4 S4-0/S4-3: lookup indexes for month snapshots and ratio vintages."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_ratios_vintage ON fundamental_ratios(security_id, ratio_code, period_end, vintage_class, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_pit_snapshot_month ON fundamental_pit_snapshot(snapshot_month, security_id, canonical_metric)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_pit_snapshot_asof ON fundamental_pit_snapshot(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_pit_snapshot_vintage ON fundamental_pit_snapshot(vintage_class, is_latest_revision)",
    ):
        conn.execute(statement)


def _fundamental_pit_snapshot_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S4 S4-0/S4-3: catalog PIT snapshot and ratio-vintage metadata."""

    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_pit_snapshot',
            'sec_edgar',
            'Fundamental PIT month snapshots',
            'End-of-month reconstruction of visible bitemporal fundamental facts with as-first-reported/restated vintage classification.',
            'snapshot_month,security_id,canonical_metric,period_end,basis',
            'fundamental_pit_snapshot', 'as_of_date', 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'fundamental_pit_snapshot',
            'gold',
            'fundamental_pit_snapshot',
            'snapshot_month,security_id,canonical_metric,period_end,basis',
            'Compustat Snapshot-style month-end reconstruction over loaded bitemporal fundamentals, preserving vintage class and source accession.',
            '["source", "snapshot_month", "security_id", "canonical_metric", "period_end", "basis"]',
            'Rows are materialized only for requested proof/backfill months. Month reconstruction gates on available_at <= month end; use snapshot_month plus available_at/as_of_date for PIT reads.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('snapshot_id', 'security_id', 'cik', 'source_accession', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('snapshot_month', 'period_start', 'period_end', 'filed_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'snapshot_month' THEN 'End-of-month snapshot date this row reconstructs.'
                WHEN 'vintage_class' THEN 'as_first_reported, most_recently_restated, or intermediate_restatement.'
                WHEN 'available_at' THEN 'Timestamp when the selected vintage became visible; must be <= snapshot month end.'
                ELSE replace(c.column_name, '_', ' ') || ' field on fundamental_pit_snapshot.'
            END AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) IN ('snapshot_month', 'period_start', 'period_end', 'filed_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'fundamental_pit_snapshot'
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description, nullable, unit, source_field, updated_at
        )
        VALUES (
            'fundamental_ratios',
            'vintage_class',
            'category',
            'Ratio vintage classification: as_first_reported, most_recently_restated, or intermediate_restatement.',
            true,
            NULL,
            NULL,
            now()
        )
        """
    )
    _schema_contract_schema_catalog(conn)


def _industry_templates_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S5 S5-0: industry-template dim, item requirements, and entity routing."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS industry_template (
            template_code VARCHAR PRIMARY KEY,
            label VARCHAR NOT NULL,
            vendor_profile VARCHAR NOT NULL,
            accounting_class VARCHAR NOT NULL,
            required_item_set VARCHAR NOT NULL,
            description VARCHAR,
            is_active BOOLEAN NOT NULL DEFAULT true,
            valid_from DATE NOT NULL DEFAULT DATE '1900-01-01',
            valid_to DATE,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS industry_template_item (
            template_code VARCHAR NOT NULL,
            item_id INTEGER NOT NULL,
            canonical_code VARCHAR NOT NULL,
            requirement_level VARCHAR NOT NULL,
            not_available BOOLEAN NOT NULL DEFAULT false,
            notes VARCHAR,
            valid_from DATE NOT NULL DEFAULT DATE '1900-01-01',
            valid_to DATE,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (template_code, item_id)
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS entity_industry_template (
            route_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            industry_template VARCHAR NOT NULL,
            matched_taxonomy VARCHAR,
            matched_node_code VARCHAR,
            match_reason VARCHAR NOT NULL,
            valid_from DATE NOT NULL,
            valid_to DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )

    import pandas as pd
    from dataclasses import astuple
    from .industry_templates import INDUSTRY_TEMPLATES, TEMPLATE_ITEMS

    template_frame = pd.DataFrame.from_records(
        [astuple(row) for row in INDUSTRY_TEMPLATES],
        columns=[
            "template_code",
            "label",
            "vendor_profile",
            "accounting_class",
            "required_item_set",
            "description",
            "is_active",
            "valid_from",
            "valid_to",
        ],
    )
    item_frame = pd.DataFrame.from_records(
        [astuple(row) for row in TEMPLATE_ITEMS],
        columns=[
            "template_code",
            "item_id",
            "canonical_code",
            "requirement_level",
            "not_available",
            "notes",
            "valid_from",
            "valid_to",
        ],
    )
    conn.register("_industry_template_seed", template_frame)
    conn.register("_industry_template_item_seed", item_frame)
    try:
        conn.execute(
            """
            INSERT OR REPLACE INTO industry_template (
                template_code, label, vendor_profile, accounting_class,
                required_item_set, description, is_active, valid_from, valid_to, updated_at
            )
            SELECT
                template_code, label, vendor_profile, accounting_class,
                required_item_set, description, is_active,
                CAST(valid_from AS DATE), CAST(valid_to AS DATE), now()
            FROM _industry_template_seed
            """
        )
        conn.execute(
            """
            INSERT OR REPLACE INTO industry_template_item (
                template_code, item_id, canonical_code, requirement_level,
                not_available, notes, valid_from, valid_to, updated_at
            )
            SELECT
                template_code, item_id, canonical_code, requirement_level,
                not_available, notes, CAST(valid_from AS DATE), CAST(valid_to AS DATE), now()
            FROM _industry_template_item_seed
            """
        )
    finally:
        conn.unregister("_industry_template_seed")
        conn.unregister("_industry_template_item_seed")

    for row in (
        (
            "industry_template",
            "reference",
            "industry_template",
            "template_code",
            "Industry statement-template dimension for commercial, bank, insurance, utility, broker-dealer, and REIT profiles.",
            '["template_code"]',
            "Reference dimension; use entity_industry_template for PIT entity routing.",
        ),
        (
            "industry_template_item",
            "reference",
            "industry_template_item",
            "template_code,item_id",
            "Required item set for each industry statement template, including explicit Not-Available/vendor-only markers.",
            '["template_code","item_id"]',
            "Reference item-set rows; valid_from/valid_to bound template definitions.",
        ),
        (
            "entity_industry_template",
            "silver",
            "entity_industry_template",
            "source,security_id,valid_from",
            "PIT security-to-industry-template routing materialized from SIC classifications.",
            '["route_id"]',
            "Resolve with valid_from/valid_to and available_at <= query timestamp; current refresh writes latest open interval rows.",
        ),
    ):
        conn.execute(
            """
            INSERT OR REPLACE INTO table_catalog (
                table_name, layer, entity, grain, description,
                natural_key_json, pit_notes, updated_at
            )
            VALUES (?, ?, ?, ?, ?, ?, ?, now())
            """,
            list(row),
        )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('template_code', 'route_id', 'security_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('valid_from', 'valid_to', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.' AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) IN ('valid_from', 'valid_to', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name IN ('industry_template', 'industry_template_item', 'entity_industry_template')
        """
    )
    _schema_contract_schema_catalog(conn)


def _industry_templates_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S5 S5-0: lookup indexes for template routing."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_entity_industry_template_security ON entity_industry_template(security_id, valid_from, valid_to)",
        "CREATE INDEX IF NOT EXISTS idx_entity_industry_template_template ON entity_industry_template(industry_template, is_latest_revision)",
        "CREATE INDEX IF NOT EXISTS idx_industry_template_item_template ON industry_template_item(template_code, requirement_level)",
    ):
        conn.execute(statement)


def _industry_template_coverage_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S5 S5-3: per-template coverage report table."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS industry_template_coverage (
            coverage_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            industry_template VARCHAR NOT NULL,
            routed_entity_count INTEGER NOT NULL DEFAULT 0,
            required_item_count INTEGER NOT NULL DEFAULT 0,
            present_item_count INTEGER NOT NULL DEFAULT 0,
            not_available_item_count INTEGER NOT NULL DEFAULT 0,
            missing_item_count INTEGER NOT NULL DEFAULT 0,
            missing_item_ids_json VARCHAR NOT NULL DEFAULT '[]',
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'industry_template_coverage',
            'gold',
            'industry_template_coverage',
            'source,industry_template,as_of_date',
            'Per-template required-item coverage and Not-Available accounting report.',
            '["coverage_id"]',
            'Coverage is a PIT report row; resolve with as_of_date and available_at <= query timestamp.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('coverage_id', 'industry_template', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            replace(c.column_name, '_', ' ') || ' field on industry_template_coverage.' AS description,
            coalesce(c.is_nullable, true) AS nullable,
            CASE
                WHEN lower(c.column_name) = 'as_of_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'industry_template_coverage'
        """
    )
    _schema_contract_schema_catalog(conn)


def _industry_template_coverage_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S5 S5-3: coverage lookup indexes and schema-contract refresh."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_industry_template_coverage_template ON industry_template_coverage(industry_template, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_industry_template_coverage_latest ON industry_template_coverage(source, is_latest_revision)",
    ):
        conn.execute(statement)
    _schema_contract_schema_catalog(conn)


def _calendarization_map_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S6 S6-0: fiscal-to-calendar map and 52/53-week flags."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_calendar_map (
            calendar_map_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            upstream_source VARCHAR NOT NULL,
            fundamental_period_id VARCHAR NOT NULL,
            period_group_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            accession_number VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            normalized_period_type VARCHAR NOT NULL,
            fyr INTEGER NOT NULL,
            period_length_days INTEGER,
            week_count INTEGER,
            is_53_week BOOLEAN NOT NULL,
            reported_fiscal_year INTEGER,
            reported_fiscal_period VARCHAR,
            fiscal_scheme_year INTEGER NOT NULL,
            fiscal_scheme_quarter INTEGER NOT NULL,
            fiscal_scheme_period VARCHAR,
            containing_calendar_year INTEGER NOT NULL,
            containing_calendar_quarter INTEGER NOT NULL,
            containing_calendar_period VARCHAR,
            greatest_overlap_calendar_year INTEGER NOT NULL,
            greatest_overlap_calendar_quarter INTEGER NOT NULL,
            greatest_overlap_calendar_period VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'calendarization',
            'atx_warehouse',
            'Fundamental calendarization',
            'Fiscal-to-calendar period labels, 52/53-week flags, calendar-aligned TTM, and coverage checks for fundamental statement periods.',
            'security_id,period_end,accession_number',
            'fundamental_calendar_map',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'fundamental_calendar_map',
            'gold',
            'fundamental_calendar_map',
            'source,upstream_source,fundamental_period_id',
            'Fiscal-to-calendar map for fundamental_periods, including fiscal-reported, containing-calendar, and Compustat-FYR greatest-overlap labels plus period length and 52/53-week flags.',
            '["calendar_map_id"]',
            'Derived from fundamental_periods; resolve with as_of_date/available_at and is_latest_revision. source identifies the calendarization run, upstream_source the original statement source.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('calendar_map_id', 'fundamental_period_id', 'period_group_id', 'security_id', 'cik', 'accession_number', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('period_start', 'period_end', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('is_53_week', 'is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'fyr' THEN 'Fiscal-year-end calendar month under the Compustat FYR convention.'
                WHEN 'period_length_days' THEN 'Inclusive day count from period_start through period_end.'
                WHEN 'week_count' THEN 'Rounded fiscal-week count implied by period_length_days.'
                WHEN 'is_53_week' THEN 'True when the period is one fiscal week longer than its normal quarter/YTD/year bucket.'
                WHEN 'greatest_overlap_calendar_period' THEN 'Calendar period label under the Compustat-FYR greatest-overlap scheme.'
                ELSE replace(c.column_name, '_', ' ') || ' field on fundamental_calendar_map.'
            END AS description,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('period_start', 'period_end', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%days' THEN 'days'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'fundamental_calendar_map'
        """
    )
    _schema_contract_schema_catalog(conn)


def _calendarization_ttm_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S6 S6-1: calendar-aligned trailing-twelve-month surface."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_calendar_ttm (
            calendar_ttm_id VARCHAR PRIMARY KEY,
            calendar_ttm_revision_group_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            upstream_source VARCHAR NOT NULL,
            anchor_statement_point_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR NOT NULL,
            statement_type VARCHAR NOT NULL,
            statement_section VARCHAR NOT NULL,
            canonical_metric VARCHAR NOT NULL,
            canonical_label VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            unit_type VARCHAR NOT NULL,
            calendar_year INTEGER NOT NULL,
            calendar_quarter INTEGER NOT NULL,
            calendar_period VARCHAR NOT NULL,
            calendar_period_end DATE NOT NULL,
            ttm_start_date DATE NOT NULL,
            ttm_end_date DATE NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            quarter_count INTEGER NOT NULL,
            coverage_days INTEGER NOT NULL,
            is_complete BOOLEAN NOT NULL,
            min_input_available_at TIMESTAMP,
            max_input_available_at TIMESTAMP,
            input_statement_point_ids_json VARCHAR NOT NULL,
            input_accessions_json VARCHAR NOT NULL,
            input_calendar_periods_json VARCHAR NOT NULL,
            ttm_value DOUBLE,
            previous_ttm_value DOUBLE,
            ttm_value_delta DOUBLE,
            ttm_value_delta_percent DOUBLE,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            is_value_changed BOOLEAN NOT NULL,
            calculation_method VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'fundamental_calendar_ttm',
            'atx_warehouse',
            'Calendar-aligned fundamental TTM',
            'Trailing-twelve-month statement values re-expressed on common calendar quarter ends from the fiscal calendar map.',
            'security_id,canonical_metric,calendar_period,unit',
            'fundamental_calendar_ttm',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'fundamental_calendar_ttm',
            'gold',
            'fundamental_calendar_ttm',
            'source,upstream_source,security_id,canonical_metric,calendar_period,unit',
            'Calendar-aligned trailing-twelve-month statement values built from PIT-visible quarter-like statement facts mapped to greatest-overlap calendar quarters.',
            '["calendar_ttm_id"]',
            'Use as_of_date/available_at and is_latest_revision for PIT-safe calendar-aligned TTM reads; is_complete distinguishes four-quarter complete windows from partial proof-slice visibility.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('calendar_ttm_id', 'calendar_ttm_revision_group_id', 'anchor_statement_point_id', 'security_id', 'cik', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('calendar_period_end', 'ttm_start_date', 'ttm_end_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%json%' THEN 'json'
                WHEN lower(c.column_name) IN ('is_complete', 'is_latest_revision', 'is_value_changed') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'calendar_period' THEN 'Calendar quarter label under the greatest-overlap scheme.'
                WHEN 'coverage_days' THEN 'Inclusive day span covered by the visible trailing quarter inputs.'
                WHEN 'is_complete' THEN 'True when the row has four visible quarters and coverage_days is within the annual tolerance.'
                WHEN 'input_statement_point_ids_json' THEN 'JSON lineage for statement points consumed in the calendar TTM window.'
                ELSE replace(c.column_name, '_', ' ') || ' field on fundamental_calendar_ttm.'
            END AS description,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('calendar_period_end', 'ttm_start_date', 'ttm_end_date', 'as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%days' THEN 'days'
                WHEN lower(c.column_name) LIKE '%percent' THEN 'ratio'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'fundamental_calendar_ttm'
        """
    )
    _schema_contract_schema_catalog(conn)


def _calendarization_coverage_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S6 S6-3: coverage report, quality-check support indexes, and contract."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS calendarization_coverage (
            coverage_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            period_count INTEGER NOT NULL DEFAULT 0,
            map_row_count INTEGER NOT NULL DEFAULT 0,
            fiscal_scheme_unmapped_count INTEGER NOT NULL DEFAULT 0,
            containing_scheme_unmapped_count INTEGER NOT NULL DEFAULT 0,
            overlap_scheme_unmapped_count INTEGER NOT NULL DEFAULT 0,
            duplicate_map_count INTEGER NOT NULL DEFAULT 0,
            overlength_period_count INTEGER NOT NULL DEFAULT 0,
            unflagged_53_week_count INTEGER NOT NULL DEFAULT 0,
            calendar_ttm_row_count INTEGER NOT NULL DEFAULT 0,
            incomplete_calendar_ttm_count INTEGER NOT NULL DEFAULT 0,
            duplicate_calendar_ttm_window_count INTEGER NOT NULL DEFAULT 0,
            stitched_ttm_row_count INTEGER NOT NULL DEFAULT 0,
            duplicate_stitched_ttm_window_count INTEGER NOT NULL DEFAULT 0,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'calendarization_coverage',
            'gold',
            'calendarization_coverage',
            'source,as_of_date',
            'Coverage report for fiscal-to-calendar map completeness, 52/53-week flags, and calendar/stiched TTM duplicate windows.',
            '["coverage_id"]',
            'Coverage is a PIT report row; resolve with as_of_date and available_at <= query timestamp.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) IN ('coverage_id', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('as_of_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) IN ('is_latest_revision') OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            replace(c.column_name, '_', ' ') || ' field on calendarization_coverage.' AS description,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) = 'as_of_date' THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name = 'calendarization_coverage'
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_fundamental_calendar_map_period ON fundamental_calendar_map(upstream_source, security_id, period_end, accession_number)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_calendar_map_overlap ON fundamental_calendar_map(greatest_overlap_calendar_period, is_latest_revision)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_calendar_map_53_week ON fundamental_calendar_map(is_53_week, period_length_days)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_calendar_ttm_window ON fundamental_calendar_ttm(source, upstream_source, security_id, canonical_metric, calendar_period, unit)",
        "CREATE INDEX IF NOT EXISTS idx_fundamental_calendar_ttm_latest ON fundamental_calendar_ttm(is_latest_revision, is_complete)",
        "CREATE INDEX IF NOT EXISTS idx_calendarization_coverage_source ON calendarization_coverage(source, is_latest_revision, as_of_date)",
    ):
        conn.execute(statement)
    _schema_contract_schema_catalog(conn)


def _catalog_fields_for_tables(conn: duckdb.DuckDBPyConnection, table_names: tuple[str, ...]) -> None:
    table_list = ", ".join(f"'{name}'" for name in table_names)
    conn.execute(
        f"""
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        SELECT
            c.table_name,
            c.column_name,
            CASE
                WHEN lower(c.column_name) LIKE '%_json' THEN 'json'
                WHEN lower(c.column_name) LIKE '%_id' OR lower(c.column_name) IN ('source', 'security_id', 'cik', 'accession_number', 'run_id') THEN 'identifier'
                WHEN lower(c.column_name) IN ('period_start', 'period_end', 'instant_date', 'as_of_date', 'filing_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' OR upper(c.data_type) LIKE '%TIMESTAMP%' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE 'is_%' OR upper(c.data_type) = 'BOOLEAN' THEN 'flag'
                WHEN upper(c.data_type) IN ('DOUBLE', 'FLOAT', 'INTEGER', 'BIGINT', 'DECIMAL') THEN 'measure'
                ELSE 'text'
            END AS semantic_type,
            CASE c.column_name
                WHEN 'available_at' THEN 'PIT availability timestamp inherited from the filing context acceptance datetime, falling back to filing date.'
                WHEN 'source_loaded_at' THEN 'Warehouse/source load timestamp for lineage and replay diagnostics.'
                WHEN 'as_of_date' THEN 'PIT as-of date for the period or instant represented by this row.'
                WHEN 'is_latest_revision' THEN 'True for the latest visible revision within the row natural key.'
                WHEN 'reconciliation_status' THEN 'Tolerance-banded segment-to-consolidated status: reconciled, flagged_divergent, or no_consolidated.'
                WHEN 'reconciliation_tolerance' THEN 'Relative tolerance used for segment-to-consolidated reconciliation.'
                WHEN 'input_codes_json' THEN 'JSON lineage of source axis/member/concept/fact identifiers consumed by the derived row.'
                ELSE replace(c.column_name, '_', ' ') || ' field on ' || c.table_name || '.'
            END AS description,
            coalesce(c.is_nullable, true),
            CASE
                WHEN lower(c.column_name) IN ('period_start', 'period_end', 'instant_date', 'as_of_date', 'filing_date') THEN 'date'
                WHEN lower(c.column_name) LIKE '%_at' THEN 'timestamp'
                WHEN lower(c.column_name) LIKE '%percent' OR lower(c.column_name) LIKE '%tolerance' THEN 'ratio'
                ELSE NULL
            END AS unit,
            NULL AS source_field,
            now() AS updated_at
        FROM duckdb_columns() c
        WHERE c.schema_name = 'main'
          AND coalesce(c.internal, false) = false
          AND c.table_name IN ({table_list})
        """
    )


def _segment_surfaces_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S7 S7-0: ASC 280 segment dimension and fact surfaces."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS segment_dim (
            segment_dim_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            accession_number VARCHAR NOT NULL,
            filing_context_id VARCHAR NOT NULL,
            filing_dimension_id VARCHAR NOT NULL,
            segment_type VARCHAR NOT NULL,
            axis_qname VARCHAR NOT NULL,
            axis_taxonomy VARCHAR,
            axis_concept VARCHAR,
            member_qname VARCHAR,
            member_taxonomy VARCHAR,
            member_concept VARCHAR,
            member_label VARCHAR,
            member_kind VARCHAR NOT NULL,
            typed_member_value VARCHAR,
            context_element VARCHAR,
            segment_sic VARCHAR,
            segment_naics VARCHAR,
            major_customer_name VARCHAR,
            major_customer_type VARCHAR,
            axis_legal_status VARCHAR NOT NULL DEFAULT 'unchecked',
            input_codes_json VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS segment_fact (
            segment_fact_id VARCHAR PRIMARY KEY,
            segment_dim_id VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            accession_number VARCHAR NOT NULL,
            filing_context_id VARCHAR NOT NULL,
            filing_fact_id VARCHAR NOT NULL,
            segment_type VARCHAR NOT NULL,
            axis_qname VARCHAR NOT NULL,
            member_qname VARCHAR,
            member_label VARCHAR,
            canonical_item VARCHAR NOT NULL,
            consolidated_code VARCHAR NOT NULL,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE,
            instant_date DATE,
            value DOUBLE NOT NULL,
            raw_value VARCHAR,
            segment_sum_value DOUBLE,
            consolidated_value DOUBLE,
            reconciliation_difference DOUBLE,
            reconciliation_relative_difference DOUBLE,
            reconciliation_tolerance DOUBLE NOT NULL DEFAULT 0.02,
            reconciliation_status VARCHAR NOT NULL DEFAULT 'no_consolidated',
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            input_codes_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'segments',
            'atx_warehouse',
            'Inline-XBRL segment surfaces',
            'Business/geographic/product/customer segment dimensions and facts mined from dimensional inline-XBRL contexts, with tolerance-banded reconciliation to consolidated totals.',
            'security_id,axis_member,canonical_item,period',
            'segment_fact',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES
            (
                'segment_dim',
                'gold',
                'segment_dim',
                'source,security_id,filing_context_id,axis_qname,member_qname',
                'Decoded segment dimension members from dimensional inline-XBRL contexts, including ASC 280 segment family and major-customer typed-member capture.',
                '["segment_dim_id"]',
                'Derived from xbrl_filing_contexts/xbrl_filing_dimensions; resolve with as_of_date/available_at and is_latest_revision.',
                now()
            ),
            (
                'segment_fact',
                'gold',
                'segment_fact',
                'source,security_id,segment_dim_id,canonical_item,period,accession_number',
                'Segment-level sales, operating income, assets, D&A, and capex facts mined from dimensional inline-XBRL facts with tolerance-banded consolidated reconciliation.',
                '["segment_fact_id"]',
                'Derived from dimensional xbrl_filing_facts; available_at is inherited from the filing context, and reconciliation_status is warning-oriented rather than a hard assertion.',
                now()
            )
        """
    )
    _catalog_fields_for_tables(conn, ("segment_dim", "segment_fact"))
    _schema_contract_schema_catalog(conn)


def _footnote_subledger_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S7 S7-1/S7-2: pension, deferred-tax, lease, and SBC footnote ledgers."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS footnote_pension (
            pension_id VARCHAR PRIMARY KEY,
            plan_type VARCHAR NOT NULL,
            line_item VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            accession_number VARCHAR NOT NULL,
            filing_context_id VARCHAR NOT NULL,
            filing_fact_id VARCHAR NOT NULL,
            axis_qname VARCHAR,
            member_qname VARCHAR,
            member_label VARCHAR,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE,
            instant_date DATE,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR,
            value DOUBLE NOT NULL,
            raw_value VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            input_codes_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS footnote_deferred_tax (
            deferred_tax_id VARCHAR PRIMARY KEY,
            tax_component VARCHAR NOT NULL,
            line_item VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            accession_number VARCHAR NOT NULL,
            filing_context_id VARCHAR NOT NULL,
            filing_fact_id VARCHAR NOT NULL,
            axis_qname VARCHAR,
            member_qname VARCHAR,
            member_label VARCHAR,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE,
            instant_date DATE,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR,
            value DOUBLE NOT NULL,
            raw_value VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            input_codes_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS footnote_lease (
            lease_id VARCHAR PRIMARY KEY,
            lease_standard VARCHAR NOT NULL,
            lease_class VARCHAR NOT NULL,
            maturity_bucket VARCHAR,
            line_item VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            accession_number VARCHAR NOT NULL,
            filing_context_id VARCHAR NOT NULL,
            filing_fact_id VARCHAR NOT NULL,
            axis_qname VARCHAR,
            member_qname VARCHAR,
            member_label VARCHAR,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE,
            instant_date DATE,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR,
            value DOUBLE NOT NULL,
            raw_value VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            input_codes_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS footnote_sbc (
            sbc_id VARCHAR PRIMARY KEY,
            award_type VARCHAR NOT NULL,
            line_item VARCHAR NOT NULL,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            accession_number VARCHAR NOT NULL,
            filing_context_id VARCHAR NOT NULL,
            filing_fact_id VARCHAR NOT NULL,
            axis_qname VARCHAR,
            member_qname VARCHAR,
            member_label VARCHAR,
            period_type VARCHAR NOT NULL,
            period_start DATE,
            period_end DATE,
            instant_date DATE,
            taxonomy VARCHAR NOT NULL,
            concept VARCHAR NOT NULL,
            unit VARCHAR,
            value DOUBLE NOT NULL,
            raw_value VARCHAR,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            revision_sequence INTEGER NOT NULL,
            revision_count INTEGER NOT NULL,
            is_latest_revision BOOLEAN NOT NULL,
            input_codes_json VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'footnotes',
            'atx_warehouse',
            'Inline-XBRL footnote sub-ledgers',
            'Pension/OPEB, deferred-tax, lease, and stock-compensation sub-ledgers mined from dimensional inline-XBRL facts.',
            'security_id,subledger,line_item,period',
            'footnote_pension',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES
            ('footnote_pension', 'gold', 'footnote_pension', 'source,security_id,plan_type,line_item,period', 'Pension and OPEB benefit-obligation, asset, funded-status, cost, and assumption facts from dimensional inline-XBRL.', '["pension_id"]', 'Derived from xbrl_filing_facts/xbrl_filing_dimensions with available_at inherited from filing acceptance time.', now()),
            ('footnote_deferred_tax', 'gold', 'footnote_deferred_tax', 'source,security_id,tax_component,line_item,period', 'Deferred-tax balance-sheet and income-statement facts plus dimensional tax components.', '["deferred_tax_id"]', 'Derived from dimensional inline-XBRL facts; resolve with as_of_date/available_at and is_latest_revision.', now()),
            ('footnote_lease', 'gold', 'footnote_lease', 'source,security_id,lease_class,maturity_bucket,line_item,period', 'ASC 842 and pre-842 lease liability, ROU asset, and maturity facts from dimensional inline-XBRL.', '["lease_id"]', 'Derived from dimensional inline-XBRL facts; lease_standard separates ASC 842 from pre-842 disclosure concepts.', now()),
            ('footnote_sbc', 'gold', 'footnote_sbc', 'source,security_id,award_type,line_item,period', 'Aggregate and award-type stock-based compensation expense and tax-benefit facts; per-executive grant detail is out of scope.', '["sbc_id"]', 'Derived from dimensional inline-XBRL facts; captures aggregate/award-type SBC, not Execucomp grant detail.', now())
        """
    )
    _catalog_fields_for_tables(
        conn,
        ("footnote_pension", "footnote_deferred_tax", "footnote_lease", "footnote_sbc"),
    )
    _schema_contract_schema_catalog(conn)


def _segment_footnote_indexes_reconciliation(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S7 S7-3: indexes for segment reconciliation and footnote coverage checks."""

    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_segment_dim_security_axis ON segment_dim(source, security_id, axis_qname, member_qname)",
        "CREATE INDEX IF NOT EXISTS idx_segment_dim_latest ON segment_dim(source, is_latest_revision, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_segment_fact_reconciliation ON segment_fact(source, reconciliation_status, is_latest_revision)",
        "CREATE INDEX IF NOT EXISTS idx_segment_fact_item_period ON segment_fact(source, security_id, canonical_item, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_footnote_pension_security ON footnote_pension(source, security_id, plan_type, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_footnote_deferred_tax_security ON footnote_deferred_tax(source, security_id, tax_component, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_footnote_lease_security ON footnote_lease(source, security_id, lease_standard, lease_class, as_of_date)",
        "CREATE INDEX IF NOT EXISTS idx_footnote_sbc_security ON footnote_sbc(source, security_id, award_type, as_of_date)",
    ):
        conn.execute(statement)
    _catalog_fields_for_tables(
        conn,
        (
            "segment_dim",
            "segment_fact",
            "footnote_pension",
            "footnote_deferred_tax",
            "footnote_lease",
            "footnote_sbc",
        ),
    )
    _schema_contract_schema_catalog(conn)


def _segment_footnote_coverage_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S7 S7-3: coverage report for segment/footnote mining and reconciliation."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS segment_footnote_coverage (
            coverage_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            segment_dim_count INTEGER NOT NULL DEFAULT 0,
            segment_fact_count INTEGER NOT NULL DEFAULT 0,
            segment_reconciled_count INTEGER NOT NULL DEFAULT 0,
            segment_flagged_divergent_count INTEGER NOT NULL DEFAULT 0,
            segment_no_consolidated_count INTEGER NOT NULL DEFAULT 0,
            footnote_pension_count INTEGER NOT NULL DEFAULT 0,
            footnote_deferred_tax_count INTEGER NOT NULL DEFAULT 0,
            footnote_lease_count INTEGER NOT NULL DEFAULT 0,
            footnote_sbc_count INTEGER NOT NULL DEFAULT 0,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'segment_footnote_coverage',
            'atx_warehouse',
            'Segment and footnote coverage',
            'Coverage counts and segment reconciliation status split for dimensional inline-XBRL mining.',
            'source,as_of_date',
            'segment_footnote_coverage',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'segment_footnote_coverage',
            'gold',
            'segment_footnote_coverage',
            'source,as_of_date',
            'Coverage counts for segment dimensions/facts, footnote sub-ledgers, and segment reconciliation status split.',
            '["coverage_id"]',
            'Coverage is a PIT report row; resolve with as_of_date and available_at <= query timestamp.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("segment_footnote_coverage",))
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_segment_footnote_coverage_source ON segment_footnote_coverage(source, is_latest_revision, as_of_date)"
    )
    _schema_contract_schema_catalog(conn)


def _press_release_facts_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S8 S8-0: preliminary 8-K Item 2.02 press-release facts."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS press_release_facts (
            press_release_fact_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            accession_number VARCHAR,
            form VARCHAR,
            source_item VARCHAR NOT NULL,
            source_url VARCHAR,
            measure_code VARCHAR NOT NULL,
            fiscal_year INTEGER NOT NULL,
            fiscal_period VARCHAR NOT NULL,
            period_end DATE NOT NULL,
            value DOUBLE NOT NULL,
            unit VARCHAR,
            basis VARCHAR NOT NULL,
            is_preliminary BOOLEAN NOT NULL DEFAULT true,
            extraction_confidence DOUBLE NOT NULL,
            evidence_text VARCHAR,
            source_file VARCHAR,
            source_file_sha256 VARCHAR,
            filing_date DATE,
            release_date DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            input_codes_json VARCHAR NOT NULL,
            raw_payload_json VARCHAR,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'press_release_facts',
            'atx_warehouse',
            'Preliminary earnings press-release facts',
            'Revenue, EPS, operating-income, and net-income facts extracted from 8-K Item 2.02 / EX-99 earnings-release text before final 10-Q/K statements arrive.',
            'security_id,measure_code,fiscal_period,press_release',
            'press_release_facts',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'press_release_facts',
            'gold',
            'press_release_fact',
            'source,security_id,measure_code,fiscal_year,fiscal_period,accession_number,basis',
            'Preliminary earnings figures mined from 8-K Item 2.02 / EX-99 text with evidence, confidence, source file hash, and PIT availability timestamp.',
            '["press_release_fact_id"]',
            'available_at is the release/acceptance timestamp; preliminary rows remain immutable and visible only when available_at <= the as-of timestamp.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("press_release_facts",))
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_press_release_fact_key ON press_release_facts(source, security_id, measure_code, period_end, basis)",
        "CREATE INDEX IF NOT EXISTS idx_press_release_fact_pit ON press_release_facts(is_latest_revision, as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_press_release_fact_file ON press_release_facts(source, source_file_sha256)",
    ):
        conn.execute(statement)
    _schema_contract_schema_catalog(conn)


def _press_release_reconciliation_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S8 S8-1: preliminary-to-final reconciliation and pdate/rdq surface."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS press_release_reconciliation (
            press_release_reconciliation_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            press_release_fact_id VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            cik VARCHAR,
            accession_number VARCHAR,
            measure_code VARCHAR NOT NULL,
            fiscal_year INTEGER NOT NULL,
            fiscal_period VARCHAR NOT NULL,
            period_end DATE NOT NULL,
            basis VARCHAR NOT NULL,
            preliminary_value DOUBLE NOT NULL,
            preliminary_available_at TIMESTAMP NOT NULL,
            final_actual_value DOUBLE,
            final_actual_available_at TIMESTAMP,
            final_actual_accession_number VARCHAR,
            value_difference DOUBLE,
            relative_difference DOUBLE,
            reconciliation_tolerance DOUBLE NOT NULL DEFAULT 0.02,
            reconciliation_status VARCHAR NOT NULL,
            pdate DATE,
            rdq DATE,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'press_release_reconciliation',
            'atx_warehouse',
            'Press-release preliminary-to-final reconciliation',
            'Reconciles preliminary earnings-release facts to final reported estimate actuals while retaining both vintages and exposing pdate/rdq flash dates.',
            'press_release_fact_id',
            'press_release_reconciliation',
            'as_of_date',
            'available_at',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'press_release_reconciliation',
            'gold',
            'press_release_reconciliation',
            'source,press_release_fact_id',
            'Preliminary-to-final reconciliation rows linking press_release_facts to final est_actual values when available, with matched/pending/divergent status and flash-date propagation fields.',
            '["press_release_reconciliation_id"]',
            'available_at is the final actual timestamp when matched, otherwise the preliminary release timestamp; this prevents final-match knowledge before final reporting.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("press_release_reconciliation",))
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_press_release_recon_key ON press_release_reconciliation(source, security_id, measure_code, period_end, basis)",
        "CREATE INDEX IF NOT EXISTS idx_press_release_recon_status ON press_release_reconciliation(source, reconciliation_status, is_latest_revision)",
        "CREATE INDEX IF NOT EXISTS idx_press_release_recon_pit ON press_release_reconciliation(is_latest_revision, as_of_date, available_at)",
    ):
        conn.execute(statement)
    _schema_contract_schema_catalog(conn)


def _estimate_actual_surprise_basis_extensions(conn: duckdb.DuckDBPyConnection) -> None:
    """PF2-S8 S8-2/S8-3: basis tags for actuals and basis-aware surprises."""

    existing_tables = {
        row[0]
        for row in conn.execute(
            """
            SELECT table_name
            FROM duckdb_tables()
            WHERE schema_name = 'main'
              AND coalesce(internal, false) = false
            """
        ).fetchall()
    }
    if not {"est_actual", "est_consensus", "est_surprise"}.issubset(existing_tables):
        _estimates(conn)
    # DuckDB blocks ALTER TABLE on est_surprise while the secondary index exists.
    # Recreating it in this same legacy-forward transaction can trip DuckDB's
    # BoundIndex delta-index assertion, so this migration keeps the schema change
    # primary and lets future/index maintenance handle secondary index recreation.
    conn.execute("DROP INDEX IF EXISTS idx_est_surprise_key")
    for statement in (
        "ALTER TABLE est_actual ADD COLUMN IF NOT EXISTS basis VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS basis VARCHAR",
        "ALTER TABLE est_consensus ADD COLUMN IF NOT EXISTS is_gaap BOOLEAN",
        "ALTER TABLE est_surprise ADD COLUMN IF NOT EXISTS actual_basis VARCHAR",
        "ALTER TABLE est_surprise ADD COLUMN IF NOT EXISTS consensus_basis VARCHAR",
        "ALTER TABLE est_surprise ADD COLUMN IF NOT EXISTS basis_mismatch BOOLEAN DEFAULT false",
    ):
        conn.execute(statement)
    conn.execute("UPDATE est_actual SET basis = 'GAAP' WHERE basis IS NULL OR basis = ''")
    conn.execute("UPDATE est_surprise SET actual_basis = 'GAAP' WHERE actual_basis IS NULL OR actual_basis = ''")
    conn.execute("UPDATE est_surprise SET basis_mismatch = false WHERE basis_mismatch IS NULL")
    _catalog_fields_for_tables(conn, ("est_actual", "est_consensus", "est_surprise"))
    conn.execute(
        """
        UPDATE field_catalog
        SET description = 'Actual value basis tag. SEC companyfacts rows are GAAP; preliminary/non-GAAP rows must be explicitly tagged before surprise matching.'
        WHERE table_name = 'est_actual'
          AND field_name = 'basis'
        """
    )
    conn.execute(
        """
        UPDATE field_catalog
        SET description = 'Actual basis used for surprise computation; compared to consensus_basis before surprise_pct is populated.'
        WHERE table_name = 'est_surprise'
          AND field_name = 'actual_basis'
        """
    )
    conn.execute(
        """
        UPDATE field_catalog
        SET description = 'Consensus basis selected for surprise_pct comparison. Untagged legacy consensus inherits the actual basis for backward-compatible arithmetic.'
        WHERE table_name = 'est_surprise'
          AND field_name = 'consensus_basis'
        """
    )
    conn.execute(
        """
        UPDATE field_catalog
        SET description = 'True when actual_basis and consensus_basis are both present and different; surprise_pct is NULL in that case.'
        WHERE table_name = 'est_surprise'
          AND field_name = 'basis_mismatch'
        """
    )
    _schema_contract_schema_catalog(conn)


# Ordered registry of all migrations. Add new entries at the END only.
MIGRATIONS: list[Migration] = [
    Migration(
        version=1,
        name="baseline_schema",
        up=_noop,
    ),
    Migration(
        version=2,
        name="schema_evolution_alters",
        up=_schema_evolution_alters,
    ),
    Migration(
        version=3,
        name="reference_classifications",
        up=_reference_classifications,
    ),
    Migration(
        version=4,
        name="estimates",
        up=_estimates,
    ),
    Migration(
        version=5,
        name="fundamental_concept_dictionary",
        up=_fundamental_concept_dictionary,
    ),
    Migration(
        version=6,
        name="insider_ownership",
        up=_insider_ownership,
    ),
    Migration(
        version=7,
        name="fundamental_statement_map_industry_key",
        up=_fundamental_statement_map_industry_key,
    ),
    Migration(
        version=8,
        name="fundamental_period_four_dates",
        up=_fundamental_period_four_dates,
    ),
    Migration(
        version=9,
        name="xbrl_validation_results",
        up=_xbrl_validation_results,
    ),
    Migration(
        version=10,
        name="shares_outstanding_history",
        up=_shares_outstanding_history,
    ),
    Migration(
        version=11,
        name="corporate_action_adjustment_factors",
        up=_corporate_action_adjustment_factors,
    ),
    Migration(
        version=12,
        name="adjustment_factor_classification_reason",
        up=_adjustment_factor_classification_reason,
    ),
    Migration(
        version=13,
        name="daily_adjustment_factors",
        up=_daily_adjustment_factors,
    ),
    Migration(
        version=14,
        name="delisting_events",
        up=_delisting_events,
    ),
    Migration(
        version=15,
        name="delisting_return_observations",
        up=_delisting_return_observations,
    ),
    Migration(
        version=16,
        name="estimate_detail_panel",
        up=_estimate_detail_panel,
    ),
    Migration(
        version=17,
        name="estimate_consensus_snapshots",
        up=_estimate_consensus_snapshots,
    ),
    Migration(
        version=18,
        name="estimate_consensus_snapshot_indexes",
        up=_estimate_consensus_snapshot_indexes,
    ),
    Migration(
        version=19,
        name="estimate_recommendation_events",
        up=_estimate_recommendation_events,
    ),
    Migration(
        version=20,
        name="estimate_recommendation_event_indexes",
        up=_estimate_recommendation_event_indexes,
    ),
    Migration(
        version=21,
        name="estimate_recommendation_catalog_fields",
        up=_estimate_recommendation_catalog_fields,
    ),
    Migration(
        version=22,
        name="estimate_recommendation_catalog_semantics",
        up=_estimate_recommendation_catalog_semantics,
    ),
    Migration(
        version=23,
        name="estimate_recommendation_summary_snapshots",
        up=_estimate_recommendation_summary_snapshots,
    ),
    Migration(
        version=24,
        name="estimate_recommendation_summary_indexes_catalog",
        up=_estimate_recommendation_summary_indexes_catalog,
    ),
    Migration(
        version=25,
        name="estimate_security_link",
        up=_estimate_security_link,
    ),
    Migration(
        version=26,
        name="estimate_security_link_indexes_catalog",
        up=_estimate_security_link_indexes_catalog,
    ),
    Migration(
        version=27,
        name="estimate_guidance_extraction_columns_catalog",
        up=_estimate_guidance_extraction_columns_catalog,
    ),
    Migration(
        version=28,
        name="estimate_guidance_extraction_indexes",
        up=_estimate_guidance_extraction_indexes,
    ),
    Migration(
        version=29,
        name="filer_13f_cik_alias",
        up=_filer_13f_cik_alias,
    ),
    Migration(
        version=30,
        name="offexchange_transparency",
        up=_offexchange_transparency,
    ),
    Migration(
        version=31,
        name="catalog_backfill_reference_and_views",
        up=_catalog_backfill_reference_and_views,
    ),
    Migration(
        version=32,
        name="fundamental_ratios",
        up=_fundamental_ratios,
    ),
    Migration(
        version=33,
        name="fundamental_xbrl_metric",
        up=_fundamental_xbrl_metric,
    ),
    Migration(
        version=34,
        name="short_interest_metrics",
        up=_short_interest_metrics,
    ),
    Migration(
        version=35,
        name="short_interest_metrics_trend",
        up=_short_interest_metrics_trend,
    ),
    Migration(
        version=36,
        name="macro_metrics",
        up=_macro_metrics,
    ),
    Migration(
        version=37,
        name="equity_price_metrics",
        up=_equity_price_metrics,
    ),
    Migration(
        version=38,
        name="short_interest_metrics_momentum",
        up=_short_interest_metrics_momentum,
    ),
    Migration(
        version=39,
        name="thirteenf_position_metrics",
        up=_thirteenf_position_metrics,
    ),
    Migration(
        version=40,
        name="macro_metrics_regime_percentile",
        up=_macro_metrics_regime_percentile,
    ),
    Migration(
        version=41,
        name="corporate_action_dividend_metrics",
        up=_corporate_action_dividend_metrics,
    ),
    Migration(
        version=42,
        name="corporate_action_split_metrics",
        up=_corporate_action_split_metrics,
    ),
    Migration(
        version=43,
        name="thirteenf_option_metrics",
        up=_thirteenf_option_metrics,
    ),
    Migration(
        version=44,
        name="corporate_action_factor_reconciliation",
        up=_corporate_action_factor_reconciliation,
    ),
    Migration(
        version=45,
        name="thirteenf_concentration_metrics",
        up=_thirteenf_concentration_metrics,
    ),
    Migration(
        version=46,
        name="short_interest_metrics_acceleration",
        up=_short_interest_metrics_acceleration,
    ),
    Migration(
        version=47,
        name="short_interest_metrics_liquidity_pressure",
        up=_short_interest_metrics_liquidity_pressure,
    ),
    Migration(
        version=48,
        name="macro_metrics_real_rates_catalog",
        up=_macro_metrics_real_rates_catalog,
    ),
    Migration(
        version=49,
        name="finra_daily_short_volume",
        up=_finra_daily_short_volume,
    ),
    Migration(
        version=50,
        name="offexchange_quality_report",
        up=_offexchange_quality_report,
    ),
    Migration(
        version=51,
        name="insider_transaction_metrics",
        up=_insider_transaction_metrics,
    ),
    Migration(
        version=52,
        name="form144_reconciliation",
        up=_form144_reconciliation,
    ),
    Migration(
        version=53,
        name="security_listing_metrics",
        up=_security_listing_metrics,
    ),
    Migration(
        version=54,
        name="repair_identifier_history_overlaps",
        up=_repair_identifier_history_overlaps,
    ),
    Migration(
        version=55,
        name="macro_metrics_sahm_rule_catalog",
        up=_macro_metrics_sahm_rule_catalog,
    ),
    Migration(
        version=56,
        name="equity_price_metrics_liquidity",
        up=_equity_price_metrics_liquidity,
    ),
    Migration(
        version=57,
        name="equity_price_metrics_risk",
        up=_equity_price_metrics_risk,
    ),
    Migration(
        version=58,
        name="equity_price_metrics_market_relative",
        up=_equity_price_metrics_market_relative,
    ),
    Migration(
        version=59,
        name="equity_price_metrics_cross_sectional_ranks",
        up=_equity_price_metrics_cross_sectional_ranks,
    ),
    Migration(
        version=61,
        name="fundamental_item_registry_schema",
        up=_fundamental_item_registry_schema,
    ),
    Migration(
        version=62,
        name="fundamental_item_registry_indexes",
        up=_fundamental_item_registry_indexes,
    ),
    Migration(
        version=63,
        name="fundamental_ratios_input_item_ids",
        up=_fundamental_ratios_input_item_ids,
    ),
    Migration(
        version=64,
        name="fundamental_fact_item_links",
        up=_fundamental_fact_item_links,
    ),
    Migration(
        version=65,
        name="etl_job_orchestrator_manifest_schema",
        up=_etl_job_orchestrator_manifest_schema,
    ),
    Migration(
        version=66,
        name="etl_job_orchestrator_manifest_indexes",
        up=_etl_job_orchestrator_manifest_indexes,
    ),
    Migration(
        version=69,
        name="fundamental_concept_coverage_reports",
        up=_fundamental_concept_coverage_reports,
    ),
    Migration(
        version=75,
        name="formula_registry_schema_catalog",
        up=_formula_registry_schema_catalog,
    ),
    Migration(
        version=76,
        name="formula_registry_indexes",
        up=_formula_registry_indexes,
    ),
    Migration(
        version=77,
        name="formula_registry_catalog_view",
        up=_formula_registry_catalog_view,
    ),
    Migration(
        version=79,
        name="identifier_spine_schema_catalog",
        up=_identifier_spine_schema_catalog,
    ),
    Migration(
        version=80,
        name="identifier_spine_indexes",
        up=_identifier_spine_indexes,
    ),
    Migration(
        version=81,
        name="entity_parent_edges_schema_catalog",
        up=_entity_parent_edges_schema_catalog,
    ),
    Migration(
        version=82,
        name="sec_company_facts_entity_id_schema_catalog",
        up=_sec_company_facts_entity_id_schema_catalog,
    ),
    Migration(
        version=83,
        name="repair_identifier_spine_self_overlaps_s5",
        up=_repair_identifier_spine_self_overlaps_s5,
    ),
    Migration(
        version=84,
        name="market_cap_schema_catalog",
        up=_market_cap_schema_catalog,
    ),
    Migration(
        version=85,
        name="market_cap_indexes",
        up=_market_cap_indexes,
    ),
    Migration(
        version=86,
        name="valuation_multiples_schema_catalog",
        up=_valuation_multiples_schema_catalog,
    ),
    Migration(
        version=87,
        name="valuation_multiples_indexes",
        up=_valuation_multiples_indexes,
    ),
    Migration(
        version=88,
        name="xbrl_validation_dimensional_evidence",
        up=_xbrl_validation_dimensional_evidence,
    ),
    Migration(
        version=89,
        name="xbrl_validation_resolution_status",
        up=_xbrl_validation_resolution_status,
    ),
    Migration(
        version=92,
        name="fundamental_ratio_provenance_schema_catalog",
        up=_fundamental_ratio_provenance_schema_catalog,
    ),
    Migration(
        version=93,
        name="fundamental_ratio_provenance_indexes",
        up=_fundamental_ratio_provenance_indexes,
    ),
    Migration(
        version=97,
        name="schema_contract_schema_catalog",
        up=_schema_contract_schema_catalog,
    ),
    Migration(
        version=98,
        name="schema_contract_indexes",
        up=_schema_contract_indexes,
    ),
    Migration(
        version=99,
        name="warehouse_catalog_view",
        up=_warehouse_catalog_view,
    ),
    Migration(
        version=100,
        name="migration_governance_schema",
        up=_migration_governance_schema,
    ),
    Migration(
        version=101,
        name="migration_governance_indexes",
        up=_migration_governance_indexes,
    ),
    Migration(
        version=103,
        name="fundamental_standardized_schema_catalog",
        up=_fundamental_standardized_schema_catalog,
    ),
    Migration(
        version=104,
        name="fundamental_standardized_indexes",
        up=_fundamental_standardized_indexes,
    ),
    Migration(
        version=105,
        name="fundamental_standardization_exception_schema_catalog",
        up=_fundamental_standardization_exception_schema_catalog,
    ),
    Migration(
        version=106,
        name="fundamental_standardization_coverage_view",
        up=_fundamental_standardization_coverage_view,
    ),
    Migration(
        version=107,
        name="fundamental_pit_snapshot_schema",
        up=_fundamental_pit_snapshot_schema,
    ),
    Migration(
        version=108,
        name="fundamental_pit_snapshot_indexes",
        up=_fundamental_pit_snapshot_indexes,
    ),
    Migration(
        version=109,
        name="fundamental_pit_snapshot_catalog",
        up=_fundamental_pit_snapshot_catalog,
    ),
    Migration(
        version=110,
        name="industry_templates_schema_catalog",
        up=_industry_templates_schema_catalog,
    ),
    Migration(
        version=111,
        name="industry_templates_indexes",
        up=_industry_templates_indexes,
    ),
    Migration(
        version=112,
        name="industry_template_coverage_schema_catalog",
        up=_industry_template_coverage_schema_catalog,
    ),
    Migration(
        version=113,
        name="industry_template_coverage_indexes",
        up=_industry_template_coverage_indexes,
    ),
    Migration(
        version=114,
        name="calendarization_map_schema_catalog",
        up=_calendarization_map_schema_catalog,
    ),
    Migration(
        version=115,
        name="calendarization_ttm_schema_catalog",
        up=_calendarization_ttm_schema_catalog,
    ),
    Migration(
        version=116,
        name="calendarization_coverage_indexes",
        up=_calendarization_coverage_indexes,
    ),
    Migration(
        version=117,
        name="segment_surfaces_schema_catalog",
        up=_segment_surfaces_schema_catalog,
    ),
    Migration(
        version=118,
        name="footnote_subledger_schema_catalog",
        up=_footnote_subledger_schema_catalog,
    ),
    Migration(
        version=119,
        name="segment_footnote_indexes_reconciliation",
        up=_segment_footnote_indexes_reconciliation,
    ),
    Migration(
        version=120,
        name="segment_footnote_coverage_schema_catalog",
        up=_segment_footnote_coverage_schema_catalog,
    ),
    Migration(
        version=121,
        name="press_release_facts_schema_catalog",
        up=_press_release_facts_schema_catalog,
    ),
    Migration(
        version=122,
        name="press_release_reconciliation_schema_catalog",
        up=_press_release_reconciliation_schema_catalog,
    ),
    Migration(
        version=123,
        name="estimate_actual_surprise_basis_extensions",
        up=_estimate_actual_surprise_basis_extensions,
    ),
    Migration(
        version=124,
        name="valuation_overlap_slice_schema_catalog",
        up=_valuation_overlap_slice_schema_catalog,
    ),
    Migration(
        version=125,
        name="fact_disagreement_schema_catalog",
        up=_fact_disagreement_schema_catalog,
    ),
    Migration(
        version=126,
        name="xbrl_dqc_catalog_seed",
        up=_xbrl_dqc_catalog_seed,
    ),
    Migration(
        version=127,
        name="pf2_s9_indexes_report",
        up=_pf2_s9_indexes_report,
    ),
]


def _apply_pending_migrations_unlocked(conn: duckdb.DuckDBPyConnection) -> list[int]:
    rows = conn.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchall()
    applied: set[int] = {row[0] for row in rows}

    verify_migration_checksums(conn, allow_missing=100 not in applied)

    applied_now: list[int] = []
    for migration in sorted(MIGRATIONS, key=lambda m: m.version):
        if migration.version in applied:
            continue
        checksum = _migration_source_checksum(migration)
        # Run inside a transaction so a failure rolls back cleanly.
        conn.execute("BEGIN TRANSACTION")
        try:
            migration.up(conn)
            conn.execute(
                """
                INSERT INTO schema_migrations (version, description, checksum, applied_at)
                VALUES (?, ?, ?, CURRENT_TIMESTAMP)
                """,
                [_migration_version_text(migration.version), migration.name, checksum],
            )
            conn.execute("COMMIT")
        except Exception:
            conn.execute("ROLLBACK")
            raise
        applied_now.append(migration.version)

    return applied_now


def apply_pending_migrations(
    conn: duckdb.DuckDBPyConnection,
    *,
    run_id: str | None = None,
    acquire_lock: bool = True,
) -> list[int]:
    """Apply any MIGRATIONS whose version is not yet recorded in schema_migrations.

    Runs each migration inside a transaction. Inserts a tracking row on success.
    Returns the list of version numbers that were applied (empty list if all up to date).
    Must be called after ensure_quant_schema so that schema_migrations exists.

    The schema_migrations table was created by ensure_quant_schema with columns:
        version VARCHAR PRIMARY KEY, description VARCHAR NOT NULL,
        checksum VARCHAR, applied_at TIMESTAMP NOT NULL DEFAULT now()
    We cast version int to zero-padded VARCHAR for storage.
    """
    if not acquire_lock:
        return _apply_pending_migrations_unlocked(conn)

    lock_run_id = run_id or f"migration-apply-{uuid.uuid4()}"
    with acquire_apply_lock(conn, lock_run_id):
        return _apply_pending_migrations_unlocked(conn)
