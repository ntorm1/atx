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
            security_id VARCHAR,
            measure_code VARCHAR,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            period_end DATE,
            low DOUBLE,
            high DOUBLE,
            mid DOUBLE,
            basis VARCHAR,
            guidance_date DATE,
            form VARCHAR,
            accession_number VARCHAR,
            as_of_date DATE,
            available_at TIMESTAMP,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            run_id VARCHAR,
            source VARCHAR
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
