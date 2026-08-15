"""Revision visibility on structured XBRL processor findings."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _xbrl_processor_finding_revision_visibility(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        ALTER TABLE xbrl_processor_findings
            ADD COLUMN IF NOT EXISTS is_latest_revision BOOLEAN DEFAULT true
        """
    )
    conn.execute(
        """
        UPDATE xbrl_processor_findings finding
        SET is_latest_revision=run.is_latest_revision
        FROM xbrl_processor_runs run
        WHERE run.processor_run_id=finding.processor_run_id
        """
    )
    _catalog_fields_for_tables(conn, ("xbrl_processor_findings",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=290,
        name="xbrl_processor_finding_revision_visibility",
        up=_xbrl_processor_finding_revision_visibility,
    )
]
