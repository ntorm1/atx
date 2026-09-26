"""Migration 0328 (post-B0 bundle): one upgrade check on a populated 0327 warehouse.

The body runs exactly as the runner does (one transaction) on a copy of the 0327 schema
template with rows in every widened table, twice, with both commits left in the WAL and
replayed on reopen (DuckDB 1.5.5 cannot replay ALTER on DEFAULT now() tables, hence the
swap). While 0328 is unregistered the template is the 0327 head.
"""

from __future__ import annotations

import datetime as dt
import shutil

import duckdb

from atx_db import ticker_history_incremental as p14
from atx_db.migration_admin import verify_schema
from atx_db.migrations import bodies_0328 as b0328

WIDENED = {
    "market_daily_metrics": b0328.MARKET_DAILY_COLUMNS,
    "publication_releases": b0328.RELEASE_COLUMNS,
    "publication_release_datasets": b0328.RELEASE_DATASET_COLUMNS,
    "entity_classification": b0328.CLASSIFICATION_COLUMNS,
    "fundamental_periods": b0328.FUNDAMENTAL_PERIOD_RDQ_COLUMNS,
}
SIC_SOURCE = "SEC submissions.zip SIC [current_sic_snapshot] archive:0123456789abcdef"


def _connect(path) -> duckdb.DuckDBPyConnection:
    con = duckdb.connect(str(path), config={"memory_limit": "384MB", "threads": 1})
    con.execute("SET TimeZone='UTC'")
    return con


def _literal(data_type: str, row: int):
    kind = data_type.upper()
    if kind.startswith(("VARCHAR", "TEXT")):
        return f"v{row}"
    if kind in {"BOOLEAN"}:
        return False
    if kind in {"DATE"}:
        return dt.date(2024, 1, 1 + row)
    if kind.startswith("TIMESTAMP"):
        return dt.datetime(2024, 1, 1 + row, 22)
    return row + 1


def _populate(con, table: str, rows: int, overrides=None) -> None:
    """Insert ``rows`` rows filling every NOT NULL column without a default."""
    required = con.execute(
        "SELECT column_name, data_type FROM duckdb_columns() "
        "WHERE table_name = ? AND NOT is_nullable AND column_default IS NULL ORDER BY column_index",
        [table],
    ).fetchall()
    for row in range(rows):
        values = {name: _literal(data_type, row) for name, data_type in required}
        values.update((overrides or {}).get(row, {}))
        columns = ", ".join(f'"{name}"' for name in values)
        placeholders = ", ".join("?" for _ in values)
        con.execute(f"INSERT INTO {table} ({columns}) VALUES ({placeholders})", list(values.values()))


def _shape(con, table):
    columns = con.execute(
        "SELECT column_name, data_type, is_nullable, column_default FROM duckdb_columns() "
        "WHERE table_name = ? ORDER BY column_index",
        [table],
    ).fetchall()
    constraints = sorted(
        (kind, tuple(names or ()), expression or "")
        for kind, names, expression in con.execute(
            "SELECT constraint_type, constraint_column_names, expression FROM duckdb_constraints() WHERE table_name = ?",
            [table],
        ).fetchall()
    )
    indexes = con.execute("SELECT sql FROM duckdb_indexes() WHERE table_name = ? ORDER BY index_name", [table]).fetchall()
    return columns, constraints, indexes


def _apply(con) -> None:
    con.execute("BEGIN TRANSACTION")
    try:
        b0328._post_b0_bundle(con)
    except Exception:
        con.execute("ROLLBACK")
        raise
    con.execute("COMMIT")


def test_0328_widens_only_its_tables_keeps_rows_backfills_and_replays_from_the_wal(_schema_template, tmp_path):
    path = tmp_path / "w.duckdb"
    shutil.copyfile(_schema_template, path)
    con = _connect(path)
    for table in WIDENED:
        _populate(con, table, 3, {
            0: {"source": SIC_SOURCE},
            1: {"source": f"{SIC_SOURCE}; french_siccodes12_v2"},
            2: {"source": "SEC submissions JSON"},
        } if table == "entity_classification" else None)
    others = {
        name for (name,) in con.execute("SELECT table_name FROM duckdb_tables() WHERE NOT internal").fetchall()
    } - set(WIDENED)
    before = {table: _shape(con, table) for table in (*WIDENED, *sorted(others))}
    rows_before = {table: con.execute(f"SELECT * FROM {table} ORDER BY ALL").fetchall() for table in WIDENED}
    con.execute("CHECKPOINT")
    con.execute("PRAGMA disable_checkpoint_on_shutdown")
    con.execute("SET checkpoint_threshold = '10GB'")  # keep both commits in the WAL
    _apply(con)
    _apply(con)  # a re-run changes nothing
    con.close()
    assert path.with_suffix(".duckdb.wal").exists()

    con = _connect(path)  # WAL replay
    try:
        for table, additions in WIDENED.items():
            columns, constraints, indexes = _shape(con, table)
            old_columns, old_constraints, old_indexes = before[table]
            # Exactly the declared nullable columns are appended; nothing else moves.
            assert columns == [*old_columns, *((name, kind, True, None) for name, kind in additions)], table
            assert (constraints, indexes) == (old_constraints, old_indexes), table
            names = ", ".join(f'"{column[0]}"' for column in old_columns)
            assert con.execute(f"SELECT {names} FROM {table} ORDER BY ALL").fetchall() == rows_before[table], table
            assert con.execute(
                f"SELECT count(*) FROM {table} WHERE " + " OR ".join(f'"{n}" IS NOT NULL' for n, _ in additions)
            ).fetchone() == ((2,) if table == "entity_classification" else (0,)), table
        tables = {name for (name,) in con.execute("SELECT table_name FROM duckdb_tables() WHERE NOT internal").fetchall()}
        new_tables = {p14.REBASES_TABLE, p14.REVISIONS_TABLE}
        assert tables == others | set(WIDENED) | new_tables  # no scratch table left
        # The P14 tables are exactly the loader's DDL (its CREATE IF NOT EXISTS would accept a drifted table).
        loader = duckdb.connect()
        loader.execute(p14.REBASES_TABLE_DDL)
        loader.execute(p14.REVISIONS_TABLE_DDL)
        for table in sorted(new_tables):
            assert _shape(con, table) == _shape(loader, table), table
        loader.close()
        assert {table: _shape(con, table) for table in sorted(others)} == {t: before[t] for t in sorted(others)}
        # Back-fill only from what source states.
        assert con.execute(
            "SELECT source, classification_basis, mapping_version FROM entity_classification ORDER BY source"
        ).fetchall() == [
            ("SEC submissions JSON", None, None),
            (SIC_SOURCE, "current_sic_snapshot", None),
            (f"{SIC_SOURCE}; french_siccodes12_v2", "current_sic_snapshot", "french_siccodes12_v2"),
        ]
        assert con.execute("SELECT count(*) FROM v_fundamental_periods_latest").fetchone() == (3,)
        assert verify_schema(con) == ()
    finally:
        con.close()
