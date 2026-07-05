"""PF3-S5 migration bodies: fundamentals completeness for valuation inputs."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _pf3_s5_share_count_extensions(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S5 S5-0: float, treasury, and per-share-class metadata."""

    statements = (
        "ALTER TABLE shares_outstanding_history ADD COLUMN IF NOT EXISTS share_class VARCHAR",
        "ALTER TABLE shares_outstanding_history ADD COLUMN IF NOT EXISTS share_count_category VARCHAR",
    )
    for statement in statements:
        conn.execute(statement)
    conn.execute(
        """
        UPDATE shares_outstanding_history
        SET share_count_category = CASE
                WHEN share_count_type IN ('shares_outstanding', 'shares_basic_avg', 'shares_diluted_avg')
                    THEN 'consolidated'
                WHEN share_count_type IN ('float', 'treasury')
                    THEN 'float_treasury'
                WHEN share_count_type LIKE 'class_%'
                    THEN 'share_class'
                ELSE share_count_category
            END,
            share_class = CASE
                WHEN share_class IS NULL AND share_count_type LIKE 'class_%'
                    THEN upper(substr(share_count_type, 7))
                ELSE share_class
            END
        WHERE share_count_category IS NULL
           OR (share_class IS NULL AND share_count_type LIKE 'class_%')
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'shares_outstanding_history',
            'sec_edgar',
            'Shares outstanding history',
            'PIT share-count history derived from SEC XBRL shares outstanding/basic average/diluted average, plus PF3-S5 float, treasury, and share-class facts when disclosed as share counts.',
            'security_id,share_count_type,effective_date,accession',
            'shares_outstanding_history',
            'effective_date',
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
            'shares_outstanding_history',
            'silver',
            'shares_outstanding',
            'source,security_id,share_count_type,effective_date,accession_number',
            'PIT share-count history derived from normalized SEC XBRL share-count facts, including float, treasury, and common-share-class counts where the source fact is share-denominated.',
            '["share_history_id"]',
            'Use effective_date/as_of_date/available_at for as-of-safe market-cap, float, treasury, and share-class research. float rows must not exceed the same-vintage shares_outstanding row when that row exists.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("shares_outstanding_history",))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('shares_outstanding_history', 'share_count_type', 'category', 'Share count type: shares_outstanding, shares_basic_avg, shares_diluted_avg, float, treasury, or class_a/class_b/class_c/class_d.', false, NULL, 'fundamental_statement_points.canonical_metric', now()),
            ('shares_outstanding_history', 'share_class', 'category', 'Common share-class label for per-class rows, such as A/B/C/D; NULL for consolidated, float, and treasury rows.', true, NULL, 'fundamental_statement_points.canonical_metric', now()),
            ('shares_outstanding_history', 'share_count_category', 'category', 'Share-count family: consolidated, float_treasury, or share_class.', true, NULL, NULL, now()),
            ('shares_outstanding_history', 'share_count', 'measure', 'Non-negative share count in raw shares. Treasury rows are treasury shares; float rows must be less than or equal to same-vintage shares_outstanding when disclosed.', false, 'shares', 'fundamental_statement_points.value', now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s5_enterprise_value_schema_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S5 S5-1: component-lineaged enterprise value surface."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS enterprise_value (
            enterprise_value_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            market_cap_source VARCHAR NOT NULL,
            market_cap_id VARCHAR,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            trade_date DATE NOT NULL,
            period_start DATE,
            period_end DATE NOT NULL,
            fiscal_year INTEGER,
            fiscal_period VARCHAR,
            price DOUBLE,
            share_count DOUBLE,
            share_count_type_used VARCHAR,
            market_cap DOUBLE NOT NULL,
            total_debt DOUBLE NOT NULL,
            preferred_equity DOUBLE NOT NULL,
            minority_interest DOUBLE NOT NULL,
            cash_and_equivalents DOUBLE NOT NULL,
            enterprise_value DOUBLE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            as_of_date DATE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            market_cap_available_at TIMESTAMP NOT NULL,
            price_available_at TIMESTAMP,
            share_available_at TIMESTAMP,
            total_debt_available_at TIMESTAMP NOT NULL,
            preferred_equity_available_at TIMESTAMP NOT NULL,
            minority_interest_available_at TIMESTAMP NOT NULL,
            cash_and_equivalents_available_at TIMESTAMP NOT NULL,
            input_codes_json VARCHAR NOT NULL,
            input_lineage_json VARCHAR NOT NULL,
            formula_version VARCHAR NOT NULL,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_enterprise_value_latest AS
        SELECT *
        FROM enterprise_value
        WHERE is_latest_revision
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'enterprise_value',
            'atx_warehouse',
            'Derived enterprise value',
            'Daily PIT enterprise value rows with explicit market cap, total debt, preferred equity, minority interest, cash, and full component lineage.',
            'source,market_cap_source,security_id,trade_date',
            'enterprise_value',
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
                'enterprise_value',
                'gold',
                'valuation',
                'source,market_cap_source,security_id,trade_date',
                'Component-lineaged enterprise value: market_cap + total_debt + preferred_equity + minority_interest - cash_and_equivalents.',
                '["source","market_cap_source","security_id","trade_date"]',
                'Rows are visible when every component input is visible. available_at is max(market_cap_available_at, total_debt_available_at, preferred_equity_available_at, minority_interest_available_at, cash_and_equivalents_available_at).',
                now()
            ),
            (
                'v_enterprise_value_latest',
                'view',
                'valuation',
                'source,market_cap_source,security_id,trade_date',
                'Latest-revision view over enterprise_value.',
                '["source","market_cap_source","security_id","trade_date"]',
                'Convenience latest-revision view; use enterprise_value_asof for timestamp-gated reads.',
                now()
            )
        """
    )
    _catalog_fields_for_tables(conn, ("enterprise_value", "v_enterprise_value_latest"))
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('enterprise_value', 'market_cap', 'currency', 'Market capitalization component; added in EV.', false, 'USD', 'market_cap.market_cap', now()),
            ('enterprise_value', 'total_debt', 'currency', 'Total debt component; added in EV.', false, 'USD', 'fundamental_statement_points.total_debt', now()),
            ('enterprise_value', 'preferred_equity', 'currency', 'Preferred equity component; added in EV.', false, 'USD', 'fundamental_statement_points.pref_stock', now()),
            ('enterprise_value', 'minority_interest', 'currency', 'Minority interest component; added in EV.', false, 'USD', 'fundamental_statement_points.minority_int_bs', now()),
            ('enterprise_value', 'cash_and_equivalents', 'currency', 'Cash and equivalents component; subtracted in EV.', false, 'USD', 'fundamental_statement_points.cash_st_inv', now()),
            ('enterprise_value', 'enterprise_value', 'currency', 'Enterprise value = market_cap + total_debt + preferred_equity + minority_interest - cash_and_equivalents.', false, 'USD', NULL, now()),
            ('enterprise_value', 'available_at', 'timestamp', 'Maximum visible-at timestamp across all EV inputs.', false, 'timestamp', NULL, now()),
            ('enterprise_value', 'input_lineage_json', 'json', 'Component-level lineage including source ids, availability timestamps, values, and add/subtract signs.', false, 'json', NULL, now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s5_delisting_return_resolution_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S5 S5-2: observed DLRET security-id resolution support."""

    statements = (
        "CREATE INDEX IF NOT EXISTS idx_delisting_return_observations_security_asof "
        "ON delisting_return_observations(security_id, delist_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_security_identifier_history_vendor_resolution "
        "ON security_identifier_history(id_type, id_value, valid_from, valid_to, as_of_date, available_at)",
    )
    for statement in statements:
        conn.execute(statement)
    conn.execute(
        """
        INSERT OR REPLACE INTO field_catalog (
            table_name, field_name, semantic_type, description,
            nullable, unit, source_field, updated_at
        )
        VALUES
            ('delisting_return_observations', 'security_id', 'identifier', 'Warehouse security identifier. When the injected observed-DLRET file omits security_id, the loader resolves vendor_security_id/vendor_security_id_type through security_identifier_history using the observation delist_date/as_of_date/available_at.', true, NULL, 'security_identifier_history.security_id', now()),
            ('delisting_return_observations', 'available_at', 'timestamp', 'Timestamp when the observed terminal return became visible; for CRSP-like files this should be the delisting-confirmation/load availability timestamp, never merely the delist event date.', false, 'timestamp', NULL, now())
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=144,
        name="pf3_s5_share_count_extensions",
        up=_pf3_s5_share_count_extensions,
    ),
    Migration(
        version=145,
        name="pf3_s5_enterprise_value_schema_catalog",
        up=_pf3_s5_enterprise_value_schema_catalog,
    ),
    Migration(
        version=146,
        name="pf3_s5_delisting_return_resolution_indexes",
        up=_pf3_s5_delisting_return_resolution_indexes,
    ),
]
