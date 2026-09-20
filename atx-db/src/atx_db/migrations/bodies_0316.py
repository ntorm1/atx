"""Inspectable annual fallback provenance in the canonical metric stream."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _derived_annual_origin(conn: duckdb.DuckDBPyConnection) -> None:
    for declaration in (
        "value_origin VARCHAR DEFAULT 'legacy_unspecified'",
        "fiscal_period_start DATE",
        "fiscal_period_end DATE",
    ):
        conn.execute(f"ALTER TABLE derived_metric_values ADD COLUMN IF NOT EXISTS {declaration}")
    conn.execute("""
        UPDATE table_catalog SET
            description='Canonical filing-event metrics with complete-quarter preference and exact fiscal-year fallback; NULL invalidations are retained.',
            pit_notes='Rank complete states before inspecting value. value_origin exposes selected arithmetic provenance; fiscal_period_start/end describe its current operand span, not a synthetic quarterly report. Reconstructed filing clocks do not certify delivery vintages.',
            updated_at=now()
        WHERE table_name='derived_metric_values'
    """)
    _catalog_fields_for_tables(conn, ('derived_metric_values',))
    conn.execute("""
        UPDATE field_catalog SET description=CASE field_name
            WHEN 'value_origin' THEN 'annual_fallback (direct FY), annual_dependency, quarterly, instant, scalar, incomparable, unavailable, or legacy_unspecified.'
            WHEN 'fiscal_period_start' THEN 'Selected current duration start; NULL for pure balance/scalar results or unavailable span evidence.'
            WHEN 'fiscal_period_end' THEN 'Selected current operand fiscal endpoint; prior annual comparison operands retain their own lineage.'
            END, updated_at=now()
        WHERE table_name='derived_metric_values'
          AND field_name IN ('value_origin', 'fiscal_period_start', 'fiscal_period_end')
    """)
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [Migration(version=316, name='derived_annual_origin', up=_derived_annual_origin)]
