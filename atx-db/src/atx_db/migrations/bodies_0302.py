"""Declarative derived-metric engine: definitions, values, and the daily market panel."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin

_DAILY_METRIC_COLUMNS = (
    "market_cap",
    "enterprise_value",
    "pe_ttm",
    "pb",
    "ps_ttm",
    "pcf_ttm",
    "ev_ebitda",
    "ev_sales",
    "fcf_yield",
    "dividend_yield",
    "earnings_yield",
    "shareholder_yield",
    "net_payout_yield",
    "total_payout_yield",
    "buyback_yield",
    "book_to_market",
    "rd_to_market_equity",
    "gross_profit_to_ev",
    "cfo_to_ev",
    "ebit_to_ev",
    "sales_to_ev",
    "altman_z",
    "total_return_1m",
    "total_return_3m",
    "total_return_6m",
    "total_return_12m",
    "momentum_12_1",
    "realized_vol_60d",
    "realized_vol_252d",
    "dollar_volume_20d",
)

_TABLE_CATALOG_ROWS = (
    (
        "derived_metric_definitions",
        "reference",
        "metric",
        "metric_code",
        "Declarative derived-metric catalog seeded from seeds/derived_metric_definitions.csv.",
        '["metric_code"]',
        "Registry table; carries no PIT columns. Re-seeded on every derived build.",
    ),
    (
        "derived_metric_values",
        "derived",
        "security",
        "security_id,metric_code,metric_window,period_end",
        "Point-in-time derived metric values produced by atx_db.derived_metrics from the declarative catalog.",
        '["derived_value_id"]',
        "available_at is the max availability of every input fact; inputs_hash is sha256 over the sorted (code, period_end, revision_sequence, value) tuples consumed.",
    ),
    (
        "market_daily_metrics",
        "derived",
        "security",
        "security_id,trade_date",
        "Daily market panel: prices, shares, market cap, enterprise value, valuation multiples, returns and realized volatility.",
        '["market_daily_id"]',
        "available_at = greatest(trade_date + 22h, fundamental_available_at). Never join on trade_date alone.",
    ),
)

_DATASET_CATALOG_ROWS = (
    (
        "derived_metrics",
        "Declarative PIT derived metrics",
        "Ratios, per-share, growth, leverage, quality and investment metrics computed by one engine from derived_metric_definitions.",
        "security_id,metric_code,metric_window,period_end",
        "derived_metric_values",
    ),
    (
        "market_daily",
        "Daily market and valuation panel",
        "Daily prices, shares, market cap, enterprise value, valuation multiples, total returns and realized volatility.",
        "security_id,trade_date",
        "market_daily_metrics",
    ),
)

_CATALOGUED_TABLES = (
    "derived_metric_definitions",
    "derived_metric_values",
    "market_daily_metrics",
)


def _derived_metric_engine(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS derived_metric_definitions (
            metric_code VARCHAR PRIMARY KEY,
            family VARCHAR NOT NULL,
            expression VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            inputs_json VARCHAR NOT NULL,
            requires_market BOOLEAN NOT NULL,
            description VARCHAR NOT NULL,
            version VARCHAR NOT NULL,
            topological_rank INTEGER NOT NULL,
            seeded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS derived_metric_values (
            derived_value_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            metric_code VARCHAR NOT NULL,
            metric_window VARCHAR NOT NULL,
            period_end DATE NOT NULL,
            value DOUBLE NOT NULL,
            available_at TIMESTAMP NOT NULL,
            inputs_hash VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_derived_metric_values_lookup "
        "ON derived_metric_values (security_id, metric_code, period_end)"
    )
    metric_columns = ",\n            ".join(f"{name} DOUBLE" for name in _DAILY_METRIC_COLUMNS)
    conn.execute(
        f"""
        CREATE TABLE IF NOT EXISTS market_daily_metrics (
            market_daily_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            symbol VARCHAR,
            trade_date DATE NOT NULL,
            close DOUBLE,
            adj_close DOUBLE,
            volume BIGINT,
            shares_outstanding DOUBLE,
            shares_source VARCHAR,
            shares_reconciliation_ratio DOUBLE,
            {metric_columns},
            fundamental_available_at TIMESTAMP,
            available_at TIMESTAMP NOT NULL,
            inputs_hash VARCHAR NOT NULL,
            as_of_date DATE NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        "CREATE INDEX IF NOT EXISTS idx_market_daily_metrics_lookup "
        "ON market_daily_metrics (security_id, trade_date)"
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        ) VALUES (?, ?, ?, ?, ?, ?, ?, now())
        """,
        list(_TABLE_CATALOG_ROWS),
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        ) VALUES (?, 'sec_edgar', ?, ?, ?, ?, 'as_of_date', 'available_at', now())
        """,
        list(_DATASET_CATALOG_ROWS),
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO lake_partition_specs (
            object_name, partition_columns_json, watermark_column, updated_at
        ) VALUES (?, ?, 'available_at', now())
        """,
        [
            ("derived_metric_values", '["metric_window"]'),
            ("market_daily_metrics", '["as_of_date"]'),
        ],
    )
    _catalog_fields_for_tables(conn, _CATALOGUED_TABLES)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=302,
        name="derived_metric_engine",
        up=_derived_metric_engine,
    )
]
