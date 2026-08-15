"""Point-in-time revision marker for governed issuer-extension mappings."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _extension_mapping_pit_revision_marker(conn: duckdb.DuckDBPyConnection) -> None:
    conn.execute(
        """
        ALTER TABLE fundamental_extension_concept_map
            ADD COLUMN IF NOT EXISTS is_latest_revision BOOLEAN DEFAULT true;

        UPDATE fundamental_extension_concept_map
        SET is_latest_revision=true
        WHERE is_latest_revision IS NULL;

        UPDATE table_catalog
        SET pit_notes=(
                'available_at gates when an approved mapping may affect a PIT result; '
                'valid_from/valid_to scope economic periods; is_latest_revision marks '
                'the current governance revision without removing prior PIT evidence.'
            ),
            updated_at=now()
        WHERE table_name='fundamental_extension_concept_map';
        """
    )
    _catalog_fields_for_tables(conn, ("fundamental_extension_concept_map",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS = [
    Migration(
        version=280,
        name="extension_mapping_pit_revision_marker",
        up=_extension_mapping_pit_revision_marker,
    )
]
