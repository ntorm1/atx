"""Retire optional source ART indexes before incremental issuer replacement."""

from __future__ import annotations

import duckdb

from ._runner import Migration

_OPTIONAL_SOURCE_INDEXES = {
    "idx_sec_company_facts_security_asof": "sec_company_facts",
    "idx_sec_company_facts_entity_asof": "sec_company_facts",
    "idx_fundamental_points_metric_asof": "fundamental_points",
}


def _bounded_source_indexes(conn: duckdb.DuckDBPyConnection) -> None:
    """Drop only known nonunique indexes without rewriting source history.

    Source replacement already commits one issuer at a time. Optional ART
    maintenance can still retain substantial memory across a large source
    history. These indexes impose no logical key or PIT constraint; column
    definitions, all source rows, and every PK/UNIQUE contract remain intact.
    Validate the complete set before mutating so an unexpected index fails
    closed even when this body is called outside the migration transaction.
    """
    for index, table in _OPTIONAL_SOURCE_INDEXES.items():
        row = conn.execute(
            """
            SELECT table_name, is_unique, is_primary FROM duckdb_indexes()
            WHERE database_name = current_database() AND schema_name = current_schema()
              AND index_name = ?
            """,
            [index],
        ).fetchone()
        if row is not None and (row[0] != table or row[1] or row[2]):
            raise RuntimeError(f"0326 refuses to remove a non-optional index: {index}")

    for index in _OPTIONAL_SOURCE_INDEXES:
        conn.execute(f"DROP INDEX IF EXISTS {index}")


MIGRATIONS = [Migration(version=326, name="bounded_source_indexes", up=_bounded_source_indexes)]
