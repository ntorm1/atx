"""Persist annual PIT top-3000 coverage cohorts and measurement evidence."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _annual_item_coverage_cohort(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute("""
        CREATE TABLE IF NOT EXISTS item_coverage_cohort_years (
            universe_id VARCHAR NOT NULL, fiscal_year INTEGER NOT NULL, ranking_date DATE,
            as_of_date DATE NOT NULL, source VARCHAR NOT NULL, market_source VARCHAR NOT NULL,
            rules_json VARCHAR NOT NULL, candidate_count BIGINT NOT NULL,
            eligible_count BIGINT NOT NULL, selected_count BIGINT NOT NULL,
            excluded_listing BIGINT NOT NULL, excluded_market_cap BIGINT NOT NULL,
            excluded_rank BIGINT NOT NULL, is_completed_year BOOLEAN NOT NULL,
            status VARCHAR NOT NULL, available_at TIMESTAMP, run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (universe_id,fiscal_year)
        );
        CREATE TABLE IF NOT EXISTS item_coverage_annual_cohort (
            universe_id VARCHAR NOT NULL, fiscal_year INTEGER NOT NULL, security_id VARCHAR NOT NULL,
            ranking_date DATE NOT NULL, market_cap_rank INTEGER NOT NULL, market_cap DOUBLE NOT NULL,
            market_daily_id VARCHAR NOT NULL, membership_id VARCHAR NOT NULL,
            market_available_at TIMESTAMP NOT NULL, listing_available_at TIMESTAMP NOT NULL,
            available_at TIMESTAMP NOT NULL, as_of_date DATE NOT NULL, source VARCHAR NOT NULL,
            run_id VARCHAR, source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (universe_id,fiscal_year,security_id)
        );
        ALTER TABLE fundamental_item_coverage ADD COLUMN IF NOT EXISTS cohort_status VARCHAR;
        ALTER TABLE fundamental_item_coverage ADD COLUMN IF NOT EXISTS ranking_date DATE;
    """)
    for table, key, description in (
        (
            "item_coverage_cohort_years",
            '["universe_id","fiscal_year"]',
            "Annual top-3000 cohort evidence including missing, undersized and incomplete years.",
        ),
        (
            "item_coverage_annual_cohort",
            '["universe_id","fiscal_year","security_id"]',
            "Annual PIT US common-stock top-3000 market-cap constituents for item coverage.",
        ),
    ):
        conn.execute(
            "INSERT OR REPLACE INTO table_catalog "
            "(table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at) "
            "VALUES (?,'quality','coverage_cohort',?,?,?,"
            "'Measured snapshot, replaced within requested years; input clocks retained.',now())",
            [table, key, description, key],
        )
        conn.execute(
            "INSERT OR REPLACE INTO pit_exemption "
            "(table_name,missing_columns,reason,exempted_by,exempted_at,source_loaded_at) "
            "VALUES (?,'[\"is_latest_revision\"]',"
            "'Coverage measurement snapshot: scoped replacement has no revision chain.',"
            "'tier1-ar3',now(),now())",
            [table],
        )
    _catalog_fields_for_tables(
        conn, ("item_coverage_cohort_years", "item_coverage_annual_cohort", "fundamental_item_coverage")
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=311, name="annual_item_coverage_cohort", up=_annual_item_coverage_cohort)]
