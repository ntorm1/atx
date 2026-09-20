"""Canonical nullable filing-event states for quarterly derived metrics."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _derived_pit_states(conn: duckdb.DuckDBPyConnection) -> None:
    # Optional secondary ART index blocks ALTER and increases publication
    # memory. The physical table and required state primary key remain intact.
    conn.execute("DROP INDEX IF EXISTS idx_derived_metric_values_lookup")
    conn.execute("ALTER TABLE derived_metric_values ALTER COLUMN value DROP NOT NULL")
    for declaration in (
        "value_status VARCHAR DEFAULT 'valid'",
        "revision_group_id VARCHAR",
        "revision_sequence BIGINT DEFAULT 1",
        "revision_count BIGINT DEFAULT 1",
        "valid_to TIMESTAMP",
        "target_bucket BIGINT",
        "definition_hash VARCHAR",
        "arithmetic_available_at TIMESTAMP",
        "history_status VARCHAR DEFAULT 'legacy_latest_only'",
    ):
        conn.execute(f"ALTER TABLE derived_metric_values ADD COLUMN IF NOT EXISTS {declaration}")
    # Legacy values are retained as legacy, never represented as reconstructed
    # intervals. Their scopes must be rebuilt before PIT certification.
    conn.execute("""
        UPDATE table_catalog SET
            grain='source/security/metric/window/quarter bucket/definition/availability event',
            description='Quarterly derived filing-event states, including NULL invalid states; legacy_latest_only rows are incomplete history.',
            natural_key_json='["derived_value_id"]',
            pit_notes='Rank states before testing value_status. available_at is transition time; valid_to is exclusive. Reconstructed from admitted numeric source history, not local-observation vintages.',
            updated_at=now()
        WHERE table_name='derived_metric_values'
    """)
    conn.execute("""
        UPDATE dataset_catalog SET
            description='Quarterly derived event states preserving numeric revisions and clocked invalidation. Legacy scopes require reconstruction.',
            grain='derived state event', updated_at=now()
        WHERE primary_table='derived_metric_values'
    """)
    _catalog_fields_for_tables(conn, ('derived_metric_values',))
    conn.execute("""
        UPDATE field_catalog SET nullable=true,
            description='Derived numeric value; NULL represents a clocked invalid state.', updated_at=now()
        WHERE table_name='derived_metric_values' AND field_name='value'
    """)
    conn.execute("""
        UPDATE field_catalog SET
            description='Modeled filing-event state transition time, including selection and invalidation clocks; not local delivery vintage evidence.',
            updated_at=now()
        WHERE table_name='derived_metric_values' AND field_name='available_at'
    """)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=315, name='derived_pit_states', up=_derived_pit_states)]
