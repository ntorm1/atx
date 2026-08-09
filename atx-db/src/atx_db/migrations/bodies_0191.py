"""OpenFIGI candidates for 13F consensus signals."""

from __future__ import annotations

import duckdb

from ..connection import DuckDBStore
from ..openfigi_signals import ensure_openfigi_signal_schema
from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _openfigi_signal_candidate_schema(conn: duckdb.DuckDBPyConnection) -> None:
    store = DuckDBStore(":memory:")
    store.connection = conn
    ensure_openfigi_signal_schema(store)
    conn.execute(
        """
        INSERT OR REPLACE INTO dataset_catalog (
            dataset_id, source_system_id, name, description, grain,
            primary_table, pit_column, available_at_column, updated_at
        ) VALUES (
            'thirteenf_signal_instrument_candidates', 'atx_warehouse',
            '13F signal OpenFIGI candidates',
            'Audited OpenFIGI v3 equity candidates for internal signal CUSIPs.',
            'cusip,figi,exch_code', 'thirteenf_signal_instrument_candidates',
            NULL, 'available_at', now()
        )
        """
    )
    conn.execute(
        """
        INSERT OR REPLACE INTO table_catalog (
            table_name, layer, entity, grain, description,
            natural_key_json, pit_notes, updated_at
        ) VALUES (
            'thirteenf_signal_instrument_candidates', 'silver', 'instrument_mapping_candidate',
            'cusip,figi,exch_code',
            'Ranked OpenFIGI v3 mapping candidates; exactly one eligible row is selected per mapped CUSIP.',
            '["mapping_id"]',
            'Internal mapping evidence only. CUSIPs must not be redistributed in public exports.', now()
        )
        """
    )
    _catalog_fields_for_tables(conn, ("thirteenf_signal_instrument_candidates",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=191, name="openfigi_signal_candidate_schema", up=_openfigi_signal_candidate_schema)
]
