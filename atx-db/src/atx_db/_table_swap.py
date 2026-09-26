"""Governed create/copy/swap of a table's rows (runtime helper).

DuckDB 1.5.5 cannot be trusted to replay an in-place ``UPDATE``/``DELETE``/``ALTER``
of a table with a ``DEFAULT now()`` column (or an ART index) from the WAL. A batch
writer that must change existing rows instead rebuilds the table INSERT-only: a
fresh table takes the live catalog DDL (columns, defaults, constraints), is
filled from one ``SELECT``, replaces the old table by ``DROP`` + ``RENAME`` and
gets its secondary indexes back from their stored SQL -- all inside the caller's
transaction; the caller ``CHECKPOINT``s after the commit.

Runtime code imports this module, never a migration body: an applied
migration's helpers are frozen inside its checksum, so migrations keep their own
private copies (``bodies_0327._replace_rows_by_swap``).

M2: one statement touches at most ``max_rows`` rows (default ~2M). A larger
table fails loud before anything is written -- rewrite it bucketed instead.
"""

from __future__ import annotations

from collections.abc import Sequence

import duckdb

__all__ = ["DEFAULT_MAX_SWAP_ROWS", "SwapTooLargeError", "replace_rows_by_swap", "table_columns"]

#: M2 bound of one swap (rows read and rows written).
DEFAULT_MAX_SWAP_ROWS = 2_000_000
_SCRATCH_SUFFIX = "__swap"


class SwapTooLargeError(RuntimeError):
    """The table (or the rows selected for it) exceeds the M2 bound of one swap."""


def table_columns(conn: duckdb.DuckDBPyConnection, table: str) -> tuple[str, ...]:
    """Column names of ``table`` in the current database and schema, in column order."""
    return tuple(str(row[0]) for row in _shape(conn, table))


def replace_rows_by_swap(
    conn: duckdb.DuckDBPyConnection,
    table: str,
    columns: Sequence[str],
    select_sql: str,
    params: Sequence[object] | None = None,
    *,
    preserve_count: bool = False,
    max_rows: int = DEFAULT_MAX_SWAP_ROWS,
) -> int:
    """Replace every row of ``table`` with ``select_sql`` via a fresh table and a swap; returns the rows written.

    ``select_sql`` yields ``columns`` (any it omits take their defaults) and may
    read ``table`` itself. The swap keeps the column shapes, constraints and
    secondary indexes (verified afterwards). ``preserve_count`` proves that the
    rewrite kept the row count. Fails loud, before any write, when ``table``
    holds more than ``max_rows`` rows, and before the swap when the selection
    does. Run it inside the caller's transaction.
    """
    before = _count(conn, table)
    if before > max_rows:
        raise SwapTooLargeError(
            f"{table} holds {before:,} rows, above the {max_rows:,}-row bound of one swap (M2); rewrite it bucketed"
        )
    found = conn.execute(
        """
        SELECT sql FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema() AND table_name = ?
        """,
        [table],
    ).fetchone()
    prefix = f"CREATE TABLE {table}("
    if found is None or not str(found[0]).startswith(prefix):
        raise RuntimeError(f"cannot derive the {table} DDL for a row swap")
    indexes = _index_sql(conn, table)
    constraints = _constraints(conn, table)
    shape = _shape(conn, table)
    scratch = f"{table}{_SCRATCH_SUFFIX}"
    conn.execute(f"DROP TABLE IF EXISTS {scratch}")
    conn.execute(f"CREATE TABLE {scratch}{str(found[0])[len(f'CREATE TABLE {table}'):]}")
    conn.execute(f"INSERT INTO {scratch} ({_quoted(columns)}) {select_sql}", list(params or []))
    written = _count(conn, scratch)
    if written > max_rows:
        raise SwapTooLargeError(f"{table} swap selected {written:,} rows, above the {max_rows:,}-row bound (M2)")
    if preserve_count and written != before:
        raise RuntimeError(f"{table} row swap count proof failed: {before:,} rows before, {written:,} selected")
    conn.execute(f"DROP TABLE {table}")
    conn.execute(f"ALTER TABLE {scratch} RENAME TO {table}")
    for sql in indexes:
        conn.execute(sql)
    if _shape(conn, table) != shape or _constraints(conn, table) != constraints or _index_sql(conn, table) != indexes:
        raise RuntimeError(f"row swap changed the shape, constraints or indexes of {table}")
    return written


def _count(conn: duckdb.DuckDBPyConnection, table: str) -> int:
    found = conn.execute(f"SELECT count(*) FROM {table}").fetchone()
    return 0 if found is None else int(found[0])


def _quoted(names: Sequence[str]) -> str:
    return ", ".join(f'"{name}"' for name in names)


def _shape(conn: duckdb.DuckDBPyConnection, table: str) -> list[tuple[object, ...]]:
    return conn.execute(
        """
        SELECT column_name, data_type, is_nullable, column_default
        FROM duckdb_columns()
        WHERE database_name = current_database() AND schema_name = current_schema() AND table_name = ?
        ORDER BY column_index
        """,
        [table],
    ).fetchall()


def _constraints(conn: duckdb.DuckDBPyConnection, table: str) -> list[tuple[object, ...]]:
    rows = conn.execute(
        """
        SELECT constraint_type, constraint_column_names, expression
        FROM duckdb_constraints()
        WHERE database_name = current_database() AND schema_name = current_schema() AND table_name = ?
        """,
        [table],
    ).fetchall()
    return sorted((kind, tuple(names or ()), expression or "") for kind, names, expression in rows)


def _index_sql(conn: duckdb.DuckDBPyConnection, table: str) -> list[str]:
    rows = conn.execute(
        """
        SELECT index_name, sql FROM duckdb_indexes()
        WHERE database_name = current_database() AND schema_name = current_schema() AND table_name = ?
        ORDER BY index_name
        """,
        [table],
    ).fetchall()
    missing = [name for name, sql in rows if not sql]
    if missing:
        raise RuntimeError(f"cannot recreate {table} indexes without stored SQL: {missing}")
    return [str(sql) for _name, sql in rows]
