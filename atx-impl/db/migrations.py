"""Versioned migration framework for the atx-impl DuckDB warehouse.

Migrations are ordered, idempotent, and tracked in the schema_migrations table.
Call apply_pending_migrations(conn) after ensure_quant_schema to bring the schema
up to date. It is safe to call multiple times; only unapplied migrations run.
"""

from __future__ import annotations

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
]


def apply_pending_migrations(conn: duckdb.DuckDBPyConnection) -> list[int]:
    """Apply any MIGRATIONS whose version is not yet recorded in schema_migrations.

    Runs each migration inside a transaction. Inserts a tracking row on success.
    Returns the list of version numbers that were applied (empty list if all up to date).
    Must be called after ensure_quant_schema so that schema_migrations exists.

    The schema_migrations table was created by ensure_quant_schema with columns:
        version VARCHAR PRIMARY KEY, description VARCHAR NOT NULL,
        checksum VARCHAR, applied_at TIMESTAMP NOT NULL DEFAULT now()
    We use (version, description) and cast version int to VARCHAR for storage.
    """
    # Fetch already-applied versions as integers
    rows = conn.execute(
        "SELECT CAST(version AS INTEGER) FROM schema_migrations WHERE version ~ '^[0-9]+$'"
    ).fetchall()
    applied: set[int] = {row[0] for row in rows}

    applied_now: list[int] = []
    for migration in sorted(MIGRATIONS, key=lambda m: m.version):
        if migration.version in applied:
            continue
        # Run inside a transaction so a failure rolls back cleanly
        conn.execute("BEGIN TRANSACTION")
        try:
            migration.up(conn)
            conn.execute(
                """
                INSERT INTO schema_migrations (version, description, applied_at)
                VALUES (?, ?, CURRENT_TIMESTAMP)
                """,
                [str(migration.version).zfill(4), migration.name],
            )
            conn.execute("COMMIT")
        except Exception:
            conn.execute("ROLLBACK")
            raise
        applied_now.append(migration.version)

    return applied_now
