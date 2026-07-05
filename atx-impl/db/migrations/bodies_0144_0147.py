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


def _pf3_s5_valuation_input_coverage_catalog(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S5 S5-3: valuation input catalog, coverage view, and gates."""

    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS valuation_input_catalog (
            canonical_input VARCHAR PRIMARY KEY,
            display_name VARCHAR NOT NULL,
            source_table VARCHAR NOT NULL,
            source_field VARCHAR NOT NULL,
            source_filter VARCHAR,
            input_family VARCHAR NOT NULL,
            unit VARCHAR NOT NULL,
            sign_role VARCHAR NOT NULL,
            is_core_input BOOLEAN NOT NULL DEFAULT true,
            allow_zero BOOLEAN NOT NULL DEFAULT true,
            description VARCHAR NOT NULL,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute("DELETE FROM valuation_input_catalog")
    conn.executemany(
        """
        INSERT INTO valuation_input_catalog (
            canonical_input,
            display_name,
            source_table,
            source_field,
            source_filter,
            input_family,
            unit,
            sign_role,
            is_core_input,
            allow_zero,
            description
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            (
                "shares_diluted_avg",
                "Diluted Average Shares",
                "shares_outstanding_history",
                "share_count",
                "share_count_type = 'shares_diluted_avg'",
                "shares",
                "shares",
                "non_negative",
                True,
                False,
                "Diluted average shares used as the EV market-cap share denominator.",
            ),
            (
                "float",
                "Public Float Shares",
                "shares_outstanding_history",
                "share_count",
                "share_count_type = 'float'",
                "shares",
                "shares",
                "non_negative",
                True,
                True,
                "Public float shares when disclosed as shares.",
            ),
            (
                "treasury",
                "Treasury Shares",
                "shares_outstanding_history",
                "share_count",
                "share_count_type = 'treasury'",
                "shares",
                "shares",
                "non_negative",
                True,
                True,
                "Treasury shares.",
            ),
            (
                "market_cap",
                "Market Capitalization",
                "market_cap",
                "market_cap",
                "is_latest_revision",
                "valuation",
                "USD",
                "add",
                True,
                False,
                "PIT market capitalization component.",
            ),
            (
                "total_debt",
                "Total Debt",
                "fundamental_statement_points",
                "value",
                "canonical_metric = 'total_debt'",
                "valuation",
                "USD",
                "add",
                True,
                True,
                "Total debt component.",
            ),
            (
                "preferred_equity",
                "Preferred Equity",
                "fundamental_statement_points",
                "value",
                "canonical_metric = 'pref_stock'",
                "valuation",
                "USD",
                "add",
                True,
                True,
                "Preferred equity component.",
            ),
            (
                "minority_interest",
                "Minority Interest",
                "fundamental_statement_points",
                "value",
                "canonical_metric = 'minority_int_bs'",
                "valuation",
                "USD",
                "add",
                True,
                True,
                "Minority interest component.",
            ),
            (
                "cash_and_equivalents",
                "Cash And Equivalents",
                "fundamental_statement_points",
                "value",
                "canonical_metric = 'cash_st_inv'",
                "valuation",
                "USD",
                "subtract",
                True,
                True,
                "Cash and equivalents component; subtracted in EV.",
            ),
            (
                "enterprise_value",
                "Enterprise Value",
                "enterprise_value",
                "enterprise_value",
                "is_latest_revision",
                "valuation",
                "USD",
                "derived",
                True,
                True,
                "Derived enterprise value.",
            ),
        ],
    )
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_valuation_input_coverage AS
        SELECT
            share_count_type AS canonical_input,
            security_id,
            symbol,
            effective_date AS input_date,
            available_at,
            'shares_outstanding_history' AS source_table,
            source,
            share_history_id AS input_id,
            share_count AS value
        FROM shares_outstanding_history
        WHERE share_count_type IN ('shares_diluted_avg', 'float', 'treasury')
          AND share_count IS NOT NULL
          AND share_count >= 0

        UNION ALL

        SELECT
            'market_cap' AS canonical_input,
            security_id,
            symbol,
            trade_date AS input_date,
            available_at,
            'market_cap' AS source_table,
            source,
            market_cap_id AS input_id,
            market_cap AS value
        FROM market_cap
        WHERE is_latest_revision
          AND market_cap IS NOT NULL
          AND market_cap >= 0

        UNION ALL

        SELECT
            CASE canonical_metric
                WHEN 'pref_stock' THEN 'preferred_equity'
                WHEN 'minority_int_bs' THEN 'minority_interest'
                WHEN 'cash_st_inv' THEN 'cash_and_equivalents'
                ELSE canonical_metric
            END AS canonical_input,
            security_id,
            symbol,
            period_end AS input_date,
            available_at,
            'fundamental_statement_points' AS source_table,
            source,
            statement_point_id AS input_id,
            value
        FROM fundamental_statement_points
        WHERE is_latest_revision
          AND period_type = 'instant'
          AND canonical_metric IN ('total_debt', 'pref_stock', 'minority_int_bs', 'cash_st_inv')
          AND value IS NOT NULL
          AND value >= 0

        UNION ALL

        SELECT
            'enterprise_value' AS canonical_input,
            security_id,
            symbol,
            trade_date AS input_date,
            available_at,
            'enterprise_value' AS source_table,
            source,
            enterprise_value_id AS input_id,
            enterprise_value AS value
        FROM enterprise_value
        WHERE is_latest_revision
          AND enterprise_value IS NOT NULL
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'valuation_input_coverage',
            'atx_warehouse',
            'Valuation input coverage catalog',
            'Catalog and normalized coverage view for PF3-S5 core valuation inputs.',
            'canonical_input,security_id,input_date',
            'v_valuation_input_coverage',
            'input_date',
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
                'valuation_input_catalog',
                'control',
                'valuation_input',
                'canonical_input',
                'Data-owned list of core valuation inputs and their source surfaces.',
                '["canonical_input"]',
                'Quality gates read this table to determine which valuation inputs are required.',
                now()
            ),
            (
                'v_valuation_input_coverage',
                'view',
                'valuation_input',
                'canonical_input,security_id,input_date',
                'Normalized coverage rows for share, market-cap, fundamental, and enterprise-value inputs.',
                '["canonical_input","security_id","input_date","input_id"]',
                'Inputs are matched to universe dates with input_date <= universe date and available_at <= universe decision visibility.',
                now()
            )
        """
    )
    _catalog_fields_for_tables(conn, ("valuation_input_catalog", "v_valuation_input_coverage"))
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_enterprise_value_key "
        "ON enterprise_value(source, market_cap_source, security_id, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_enterprise_value_security_date "
        "ON enterprise_value(security_id, trade_date)",
        "CREATE INDEX IF NOT EXISTS idx_enterprise_value_asof "
        "ON enterprise_value(as_of_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_shares_outstanding_history_class "
        "ON shares_outstanding_history(security_id, share_count_type, share_class, effective_date, available_at)",
        "CREATE INDEX IF NOT EXISTS idx_valuation_input_catalog_core "
        "ON valuation_input_catalog(is_core_input, input_family)",
    ):
        conn.execute(statement)
    conn.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name, dataset_id, table_name, severity, threshold_value,
            comparator, enabled, failure_status, source, updated_at
        )
        VALUES
            (
                'valuation_input_core_completeness',
                'valuation_input_coverage',
                'v_valuation_input_coverage',
                'critical',
                0.0,
                'eq',
                true,
                'failed',
                'pf3_s5',
                now()
            ),
            (
                'valuation_core_item_stub_detector',
                'valuation_input_coverage',
                'v_valuation_input_coverage',
                'critical',
                0.0,
                'eq',
                true,
                'failed',
                'pf3_s5',
                now()
            )
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
    Migration(
        version=147,
        name="pf3_s5_valuation_input_coverage_catalog",
        up=_pf3_s5_valuation_input_coverage_catalog,
    ),
]
