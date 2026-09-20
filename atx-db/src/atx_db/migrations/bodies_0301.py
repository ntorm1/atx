"""Tier1-S2 T9: published per-item, per-fiscal-year standardized coverage."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _fundamental_item_coverage(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS fundamental_item_coverage (
            coverage_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL,
            universe_id VARCHAR NOT NULL,
            item_id INTEGER NOT NULL,
            canonical_code VARCHAR NOT NULL,
            basis VARCHAR NOT NULL,
            fiscal_year INTEGER NOT NULL,
            n_securities BIGINT NOT NULL,
            n_with_value BIGINT NOT NULL,
            coverage_pct DOUBLE NOT NULL,
            as_of_date DATE,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_fundamental_item_coverage_item
            ON fundamental_item_coverage(item_id, basis, fiscal_year)
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,
            pit_notes,updated_at
        ) VALUES (
            'fundamental_item_coverage','quality','standardized_item',
            'universe_id + item_id + basis + fiscal_year',
            'Share of the standardized universe reporting each canonical item per fiscal year.',
            '["coverage_id"]',
            'Derived from fundamental_standardized rows whose available_at is already gated upstream; coverage rows are a measurement artifact and are never used as a signal input.',
            now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (
            'standardized_item_breadth_target',
            'fundamental_standardized',
            'fundamental_item_coverage',
            'warning',110.0,'gte',true,'warning',
            'atx_tier1_parity',now()
        )
        """
    )
    # fundamental_item_coverage carries as_of_date/available_at/source_loaded_at/
    # run_id, but refresh_item_coverage fully deletes and reinserts every row for
    # a (source, universe_id) pair on each measurement run -- there is no
    # revision chain, so is_latest_revision would always be true and carries no
    # information. Exempt it rather than add a column with no meaning.
    conn.execute(
        """
        INSERT OR REPLACE INTO pit_exemption (
            table_name, missing_columns, reason, exempted_by, exempted_at, source_loaded_at
        )
        VALUES (
            'fundamental_item_coverage',
            '["is_latest_revision"]',
            'fundamental_item_coverage is a measurement artifact: refresh_item_coverage '
            'deletes and reinserts every row for a (source, universe_id) pair on each run, '
            'so there is no revision chain and is_latest_revision would always be true.',
            'tier1-s2-t9',
            now(),
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("fundamental_item_coverage",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=301,
        name="fundamental_item_coverage",
        up=_fundamental_item_coverage,
    )
]
