"""Lookup index for XBRL processor DTS and validation outcomes."""

from __future__ import annotations

import duckdb

from ._runner import Migration


def _xbrl_processor_outcome_index(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_xbrl_processor_run_outcome
            ON xbrl_processor_runs(
                validation_profile,dts_resolution_status,validation_outcome,is_latest_revision
            )
        """
    )


MIGRATIONS = [
    Migration(
        version=289,
        name="xbrl_processor_outcome_index",
        up=_xbrl_processor_outcome_index,
    )
]
