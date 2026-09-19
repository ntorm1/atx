"""Resumable warehouse-activation stage ledger."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _activation_stage_runs(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE TABLE IF NOT EXISTS activation_stage_runs (
            stage VARCHAR NOT NULL,
            run_id VARCHAR NOT NULL,
            status VARCHAR NOT NULL,
            started_at TIMESTAMP NOT NULL,
            finished_at TIMESTAMP,
            "rows" BIGINT,
            params_json VARCHAR,
            error VARCHAR,
            PRIMARY KEY (stage, run_id)
        );

        CREATE INDEX IF NOT EXISTS idx_activation_stage_runs_status
            ON activation_stage_runs(stage, status, started_at);
        """
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name,layer,entity,grain,description,natural_key_json,pit_notes,updated_at
        ) VALUES (?,?,?,?,?,?,?,now())
        """,
        [
            (
                "activation_stage_runs",
                "control",
                "activation_stage_run",
                "stage,run_id",
                "One row per attempt of a full-warehouse activation stage; the ladder "
                "skips stages whose newest attempt completed unless --force is given.",
                '["stage","run_id"]',
                "Operational lineage only. started_at/finished_at are warehouse load "
                "stamps and must never be used as a point-in-time availability key.",
            )
        ],
    )
    conn.executemany(
        """
        INSERT OR REPLACE INTO quality_check_registry (
            check_name,dataset_id,table_name,severity,threshold_value,
            comparator,enabled,failure_status,source,updated_at
        ) VALUES (?,?,?,?,?,?,?,?,?,now())
        """,
        [
            (
                "activation_stage_runs_stuck_running",
                "warehouse_activation",
                "activation_stage_runs",
                "warning",
                0.0,
                "eq",
                True,
                "warning",
                "atx_tier1_parity",
            ),
            (
                "duplicate_equity_daily_bar_keys",
                "tbltickerhistory_daily",
                "equity_daily_bars",
                "critical",
                0.0,
                "eq",
                True,
                "failed",
                "atx_tier1_parity",
            ),
        ],
    )
    _catalog_fields_for_tables(conn, ("activation_stage_runs",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=300,
        name="activation_stage_runs",
        up=_activation_stage_runs,
    )
]
