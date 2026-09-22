"""Reported-quarter EPS bridge schema support."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _reported_eps_nullable_standardized_value(conn: duckdb.DuckDBPyConnection) -> None:
    """Represent a later, visible reported-EPS conflict as a PIT NULL state.

    No raw Company Facts or release row is changed.  ``value`` is nullable only
    because an explicitly conflicting direct EPS must replace an earlier valid
    state without fabricating a winner.  The bridge writes the reason and both
    source identities through the existing input-lineage fields and the
    standardization-exception relation.
    """

    conn.execute("ALTER TABLE fundamental_standardized ALTER COLUMN value DROP NOT NULL")
    conn.execute(
        """
        UPDATE table_catalog
        SET description = concat_ws(' ', nullif(description, ''), ?),
            pit_notes = concat_ws(' ', nullif(pit_notes, ''), ?),
            updated_at = now()
        WHERE table_name = 'fundamental_standardized'
          AND strpos(coalesce(pit_notes, ''), 'reported_eps_conflict_v1') = 0
        """,
        [
            'A NULL value can be an explicit unavailable standardized state when later visible source evidence conflicts.',
            'reported_eps_conflict_v1: rank the complete latest state before filtering usable values; a NULL conflict must not revive an earlier EPS value.',
        ],
    )
    conn.execute(
        """
        UPDATE field_catalog
        SET nullable = true,
            description = concat_ws(' ', nullif(description, ''), ?),
            updated_at = now()
        WHERE table_name = 'fundamental_standardized'
          AND field_name = 'value'
          AND strpos(coalesce(description, ''), 'reported_eps_conflict_v1') = 0
        """,
        [
            'reported_eps_conflict_v1: NULL may be an explicit later unavailable state; inspect input_codes_json and standardization exceptions before treating it as absent source data.',
        ],
    )
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=321,
        name='reported_eps_nullable_standardized_value',
        up=_reported_eps_nullable_standardized_value,
    )
]
