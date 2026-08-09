"""Heteroskedasticity/autocorrelation-robust factor IC inference."""

from __future__ import annotations

import duckdb

from ._runner import Migration
from .bodies_0001_0137 import _catalog_fields_for_tables
from .bodies_0140_0143 import _refresh_schema_contract_v2_pin


def _factor_ic_hac_inference(conn: duckdb.DuckDBPyConnection) -> None:
    for statement in (
        "ALTER TABLE factor_ic ADD COLUMN IF NOT EXISTS hac_lags INTEGER",
        "ALTER TABLE factor_ic ADD COLUMN IF NOT EXISTS hac_standard_error DOUBLE",
        "ALTER TABLE factor_ic ADD COLUMN IF NOT EXISTS hac_tstat DOUBLE",
    ):
        conn.execute(statement)
    conn.execute(
        """
        UPDATE table_catalog
        SET description =
                'Per-factor aggregate rank-IC over the horizon ladder with both naive and Bartlett-kernel Newey-West inference.',
            pit_notes =
                'Rank-IC is computed per formation date. HAC lags default to ceil(horizon/21) to account for overlapping monthly forward-return windows.',
            updated_at = now()
        WHERE table_name = 'factor_ic'
        """
    )
    _catalog_fields_for_tables(conn, ("factor_ic",))
    _refresh_schema_contract_v2_pin(conn)


MIGRATIONS: list[Migration] = [
    Migration(version=197, name="factor_ic_hac_inference", up=_factor_ic_hac_inference)
]
