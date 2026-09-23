"""Store direct selected operands for rebuilt canonical derived metric events."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _derived_selected_input_refs(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute("ALTER TABLE derived_metric_values ADD COLUMN IF NOT EXISTS selected_input_refs_json VARCHAR")
    conn.execute("ALTER TABLE derived_metric_values ADD COLUMN IF NOT EXISTS selected_input_refs_hash VARCHAR")
    conn.execute("""
        UPDATE table_catalog SET
            pit_notes=concat_ws(' ', nullif(pit_notes, ''),
                'selected_input_refs_json v1 records direct selected operand states on rebuilt events; legacy NULL is unverified. inputs_hash remains the full candidate-frame fingerprint.'),
            updated_at=now()
        WHERE table_name='derived_metric_values'
          AND strpos(coalesce(pit_notes, ''), 'selected_input_refs_json v1')=0
    """)
    _catalog_fields_for_tables(conn, ('derived_metric_values',))
    conn.execute("""
        UPDATE field_catalog SET description=CASE field_name
            WHEN 'selected_input_refs_json' THEN 'Nullable canonical JSON v1 of direct selected item/metric operands, including missing states; legacy NULL is not issuer-qualified.'
            WHEN 'selected_input_refs_hash' THEN 'SHA256 of exact UTF-8 selected_input_refs_json payload; nullable for legacy rows.'
            END, updated_at=now()
        WHERE table_name='derived_metric_values'
          AND field_name IN ('selected_input_refs_json', 'selected_input_refs_hash')
    """)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=323, name='derived_selected_input_refs', up=_derived_selected_input_refs)]
