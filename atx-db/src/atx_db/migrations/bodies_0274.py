"""Provider-facing schema range, condition, and coverage SLO contracts."""

from __future__ import annotations

import duckdb

from ..provider_coverage import DEFAULT_PROVIDER_COVERAGE_SLOS
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _provider_coverage_contract(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS api_schema_coverage_slo (
            dataset_id VARCHAR NOT NULL,
            schema_code VARCHAR NOT NULL,
            slo_version VARCHAR NOT NULL,
            expected_history_start DATE NOT NULL,
            minimum_history_years DOUBLE NOT NULL,
            minimum_security_count BIGINT NOT NULL,
            minimum_item_count BIGINT,
            maximum_freshness_lag_days DOUBLE NOT NULL,
            citation VARCHAR NOT NULL,
            description VARCHAR NOT NULL,
            is_active BOOLEAN NOT NULL DEFAULT true,
            valid_from TIMESTAMP NOT NULL DEFAULT TIMESTAMP '1900-01-01',
            valid_to TIMESTAMP,
            updated_at TIMESTAMP NOT NULL DEFAULT now(),
            PRIMARY KEY (dataset_id,schema_code,slo_version)
        );
        CREATE TABLE IF NOT EXISTS api_schema_coverage_snapshot (
            coverage_snapshot_id VARCHAR PRIMARY KEY,
            dataset_id VARCHAR NOT NULL,
            schema_code VARCHAR NOT NULL,
            schema_version VARCHAR NOT NULL,
            source_relation VARCHAR NOT NULL,
            time_column VARCHAR NOT NULL,
            observed_at TIMESTAMP NOT NULL,
            start_time TIMESTAMP,
            end_time TIMESTAMP,
            first_available_at TIMESTAMP,
            last_available_at TIMESTAMP,
            record_count BIGINT NOT NULL,
            security_count BIGINT NOT NULL,
            item_count BIGINT,
            basis_count BIGINT,
            history_years DOUBLE,
            freshness_lag_days DOUBLE,
            condition VARCHAR NOT NULL,
            failed_slos_json VARCHAR NOT NULL,
            slo_version VARCHAR NOT NULL,
            run_id VARCHAR NOT NULL,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO api_schema_coverage_slo (
            dataset_id,schema_code,slo_version,expected_history_start,
            minimum_history_years,minimum_security_count,minimum_item_count,
            maximum_freshness_lag_days,citation,description,is_active,
            valid_from,valid_to,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,?,true,TIMESTAMP '1900-01-01',NULL,now())
        """,
        [
            (
                slo.dataset_id,
                slo.schema_code,
                slo.slo_version,
                slo.expected_history_start,
                slo.minimum_history_years,
                slo.minimum_security_count,
                slo.minimum_item_count,
                slo.maximum_freshness_lag_days,
                slo.citation,
                slo.description,
            )
            for slo in DEFAULT_PROVIDER_COVERAGE_SLOS
        ],
    )
    conn.execute(
        """
        CREATE OR REPLACE VIEW v_api_schema_coverage_current AS
        WITH current_snapshot AS (
            SELECT * EXCLUDE (snapshot_rank)
            FROM (
                SELECT
                    snapshot.*,
                    row_number() OVER (
                        PARTITION BY dataset_id,schema_code
                        ORDER BY observed_at DESC,source_loaded_at DESC,
                                 coverage_snapshot_id DESC
                    ) AS snapshot_rank
                FROM api_schema_coverage_snapshot snapshot
            )
            WHERE snapshot_rank=1
        ),
        active_slo AS (
            SELECT * EXCLUDE (slo_rank)
            FROM (
                SELECT
                    slo.*,
                    row_number() OVER (
                        PARTITION BY dataset_id,schema_code
                        ORDER BY valid_from DESC,slo_version DESC
                    ) AS slo_rank
                FROM api_schema_coverage_slo slo
                WHERE is_active
                  AND valid_from <= now()
                  AND coalesce(valid_to,TIMESTAMP '9999-12-31') > now()
            )
            WHERE slo_rank=1
        )
        SELECT
            snapshot.*,
            slo.expected_history_start,
            slo.minimum_history_years,
            slo.minimum_security_count,
            slo.minimum_item_count,
            slo.maximum_freshness_lag_days,
            slo.citation,
            slo.description AS slo_description
        FROM current_snapshot snapshot
        JOIN active_slo slo USING (dataset_id,schema_code,slo_version)
        """
    )
    for statement in (
        "CREATE INDEX IF NOT EXISTS idx_api_schema_coverage_slo_active ON api_schema_coverage_slo(dataset_id,schema_code,is_active,valid_from)",
        "CREATE INDEX IF NOT EXISTS idx_api_schema_coverage_snapshot_current ON api_schema_coverage_snapshot(dataset_id,schema_code,observed_at)",
        "CREATE INDEX IF NOT EXISTS idx_api_schema_coverage_snapshot_condition ON api_schema_coverage_snapshot(condition,observed_at)",
    ):
        conn.execute(statement)

    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "api_schema_coverage_slo",
                "reference",
                "api_schema_coverage_slo",
                "dataset_id,schema_code,slo_version",
                "Versioned institutional history, breadth, item, and freshness targets.",
                '["dataset_id","schema_code","slo_version"]',
                "valid_from/valid_to version the target contract; snapshots pin slo_version.",
            ),
            (
                "api_schema_coverage_snapshot",
                "control",
                "api_schema_coverage_snapshot",
                "dataset_id,schema_code,observed_at",
                "Append-only measured public-schema ranges, breadth, freshness, and condition.",
                '["coverage_snapshot_id"]',
                "observed_at is the measurement cutoff; source data retains its own PIT fields.",
            ),
            (
                "v_api_schema_coverage_current",
                "api",
                "api_schema_coverage_current",
                "dataset_id,schema_code",
                "Latest provider-facing schema range, condition, measured coverage, and target SLO.",
                '["dataset_id","schema_code"]',
                "Selects the latest append-only snapshot and its pinned target definition.",
            ),
        ],
    )
    _catalog_fields_for_tables(
        conn,
        (
            "api_schema_coverage_slo",
            "api_schema_coverage_snapshot",
            "v_api_schema_coverage_current",
        ),
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=274,
        name="provider_schema_coverage_and_condition_contract",
        up=_provider_coverage_contract,
    )
]
