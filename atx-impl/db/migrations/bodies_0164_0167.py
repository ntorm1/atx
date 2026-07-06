"""PF3-S10 migration bodies: backtest-ready factor panel."""

from __future__ import annotations

import hashlib
import json

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _object_schema(conn: duckdb.DuckDBPyConnection, object_name: str) -> list[dict[str, object]]:
    rows = conn.execute(
        """
        SELECT column_name, data_type, is_nullable, column_index
        FROM duckdb_columns()
        WHERE schema_name = 'main'
          AND table_name = ?
        ORDER BY column_index
        """,
        [object_name],
    ).fetchall()
    return [
        {
            "ordinal": int(column_index),
            "name": str(column_name),
            "type": str(data_type),
            "nullable": bool(is_nullable),
        }
        for column_name, data_type, is_nullable, column_index in rows
    ]


def _schema_sha256(schema: list[dict[str, object]]) -> str:
    payload = json.dumps(schema, sort_keys=True, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _pf3_s10_factor_panel_views(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S10 S10-0: long and wide PIT factor-panel views."""

    conn.execute(
        """
        CREATE OR REPLACE VIEW v_factor_panel AS
        WITH factor_values AS (
            SELECT
                security_id,
                greatest(as_of_date, CAST(available_at AS DATE)) AS as_of_date,
                factor_id,
                value,
                available_at,
                source_loaded_at,
                run_id,
                input_lineage_json
            FROM fundamental_factor_values
            WHERE is_latest_revision
              AND value IS NOT NULL
            UNION ALL
            SELECT
                security_id,
                greatest(as_of_date, CAST(available_at AS DATE)) AS as_of_date,
                factor_id,
                value,
                available_at,
                source_loaded_at,
                run_id,
                input_lineage_json
            FROM cross_domain_factor_values
            WHERE is_latest_revision
              AND value IS NOT NULL
        ),
        universe_filtered AS (
            SELECT
                f.*,
                row_number() OVER (
                    PARTITION BY f.security_id, f.as_of_date, f.factor_id
                    ORDER BY u.valid_from DESC,
                             u.available_at DESC NULLS LAST,
                             u.source_loaded_at DESC NULLS LAST,
                             u.source DESC
                ) AS universe_rn
            FROM factor_values f
            JOIN universe_membership u
              ON u.universe_id = 'us_common_equity_liquid_v1'
             AND u.security_id = f.security_id
             AND u.valid_from <= f.as_of_date
             AND (u.valid_to IS NULL OR u.valid_to >= f.as_of_date)
             AND u.as_of_date <= f.as_of_date
             AND u.is_member
             AND u.is_latest_revision
             AND (
                 u.available_at IS NULL
                 OR CAST(u.available_at AS DATE) <= f.as_of_date
             )
        ),
        latest_factor AS (
            SELECT
                security_id,
                as_of_date,
                factor_id,
                value,
                available_at,
                source_loaded_at,
                run_id,
                input_lineage_json,
                row_number() OVER (
                    PARTITION BY security_id, as_of_date, factor_id
                    ORDER BY available_at DESC,
                             source_loaded_at DESC,
                             input_lineage_json DESC
                ) AS factor_rn
            FROM universe_filtered
            WHERE universe_rn = 1
              AND CAST(available_at AS DATE) <= as_of_date
        )
        SELECT
            security_id,
            as_of_date,
            factor_id,
            value,
            available_at,
            source_loaded_at,
            run_id,
            input_lineage_json
        FROM latest_factor
        WHERE factor_rn = 1
        """
    )
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_factor_panel_wide AS
        SELECT
            security_id,
            as_of_date,
            to_json(map(list(factor_id ORDER BY factor_id), list(value ORDER BY factor_id)))
                AS factor_values_json,
            count(*)::BIGINT AS factor_count,
            max(available_at) AS max_available_at,
            max(source_loaded_at) AS source_loaded_at
        FROM v_factor_panel
        GROUP BY security_id, as_of_date
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        )
        VALUES (
            'factor_panel',
            'atx_warehouse',
            'Backtest-ready factor panel',
            'Unified S8/S9 factor namespace materialized as a universe-filtered, decision-date PIT panel.',
            'security_id,as_of_date,factor_id',
            'v_factor_panel',
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
                'v_factor_panel',
                'view',
                'factor_panel_long',
                'security_id,as_of_date,factor_id',
                'Backtest-ready long factor panel over fundamental and cross-domain factor values, filtered to the governed common-equity universe as of each decision date.',
                '["security_id","as_of_date","factor_id"]',
                'Panel as_of_date is the decision date: max(source observation date, available_at date). Rows are emitted only when factor and universe membership are visible by that decision date.',
                now()
            ),
            (
                'v_factor_panel_wide',
                'view',
                'factor_panel_wide',
                'security_id,as_of_date',
                'Backtest-ready factor cross-section with factor_id/value pairs packed as deterministic JSON for dynamic factor namespaces.',
                '["security_id","as_of_date"]',
                'Derived from v_factor_panel; factor_values_json contains only PIT-safe factor values visible by the decision date.',
                now()
            )
        """
    )
    _catalog_fields_for_tables(conn, ("v_factor_panel", "v_factor_panel_wide"))
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s10_factor_panel_lake_export_registration(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S10 S10-1: partitioned lake export registration and schema contract."""

    conn.execute("ALTER TABLE lake_export_files ADD COLUMN IF NOT EXISTS expected_schema_sha256 VARCHAR")
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS lake_export_object_contract (
            object_name VARCHAR PRIMARY KEY,
            expected_schema_sha256 VARCHAR NOT NULL,
            schema_json VARCHAR NOT NULL,
            format VARCHAR NOT NULL DEFAULT 'parquet',
            compression VARCHAR,
            partition_columns_json VARCHAR NOT NULL DEFAULT '[]',
            watermark_column VARCHAR,
            enabled BOOLEAN NOT NULL DEFAULT true,
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    schema = _object_schema(conn, "v_factor_panel")
    schema_hash = _schema_sha256(schema)
    conn.execute(
        """
        INSERT OR REPLACE INTO lake_partition_specs (
            object_name, partition_columns_json, watermark_column,
            retention_runs, enabled, updated_at
        )
        VALUES ('v_factor_panel', '["as_of_date"]', 'available_at', 3, true, now())
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO lake_export_object_contract (
            object_name, expected_schema_sha256, schema_json, format, compression,
            partition_columns_json, watermark_column, enabled, updated_at
        )
        VALUES (?, ?, ?, 'parquet', 'zstd', '["as_of_date"]', 'available_at', true, now())
        """,
        ["v_factor_panel", schema_hash, json.dumps(schema, sort_keys=True, separators=(",", ":"))],
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        )
        VALUES (
            'lake_export_object_contract',
            'control',
            'lake_export_object_contract',
            'object_name',
            'Expected schema hashes and partition/export metadata for governed lake export objects.',
            '["object_name"]',
            'Control table; expected_schema_sha256 is checked at export time so schema drift fails before a governed object is written.',
            now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("lake_export_files", "lake_partition_specs", "lake_export_object_contract"))
    _refresh_schema_contract_v2_pin(conn)


def _pf3_s10_factor_panel_contract_gate(conn: duckdb.DuckDBPyConnection) -> None:
    """PF3-S10 S10-2: critical export-boundary contract/lookahead gate."""

    conn.execute(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name, dataset_id, table_name, severity, threshold_value,
            comparator, enabled, failure_status, source, updated_at
        )
        VALUES (
            'factor_panel_export_contract',
            'factor_panel',
            'v_factor_panel',
            'critical',
            0.0,
            'eq',
            true,
            'failed',
            'pf3_s10',
            now()
        )
        """
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(
        version=164,
        name="pf3_s10_factor_panel_views",
        up=_pf3_s10_factor_panel_views,
    ),
    Migration(
        version=165,
        name="pf3_s10_factor_panel_lake_export_registration",
        up=_pf3_s10_factor_panel_lake_export_registration,
    ),
    Migration(
        version=166,
        name="pf3_s10_factor_panel_contract_gate",
        up=_pf3_s10_factor_panel_contract_gate,
    ),
]
