"""Permit bounded, constrained forward-return shadow publication."""

from __future__ import annotations

import duckdb

from ._runner import Migration


def _bounded_forward_return_publication(conn: duckdb.DuckDBPyConnection) -> None:
    """Remove only optional secondary indexes; preserve every logical constraint."""
    for index_name in ("idx_forward_returns_ss_key", "idx_forward_returns_ss_delisted"):
        conn.execute(f"DROP INDEX IF EXISTS {index_name}")


MIGRATIONS = [
    Migration(version=317, name="bounded_forward_return_publication", up=_bounded_forward_return_publication)
]
