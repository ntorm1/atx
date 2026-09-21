"""Bounded physical publication for the eight core fundamentals outputs."""

from __future__ import annotations

from collections.abc import Callable, Iterator, Sequence
from contextlib import contextmanager, suppress

from ._bulk_publication import _identifier, _validate_shadow_contract, publish_validated_shadows
from .connection import DuckDBStore

_KEYS = {
    "fundamental_fact_revisions": "fact_revision_id",
    "fundamental_statement_points": "statement_point_id",
    "fundamental_periods": "fundamental_period_id",
    "fundamental_ttm_points": "ttm_point_id",
    "fundamental_calendar_map": "calendar_map_id",
    "fundamental_calendar_ttm": "calendar_ttm_id",
    "fundamental_standardized": "standardized_id",
    "fundamental_standardization_exception": "exception_id",
}
_BATCH_ROWS = 50_000
_REOPEN_BATCHES = 4
_PREFIX_BOUNDARIES = tuple(f"{prefix:02x}" for prefix in range(1, 256))
_OWNER = "atx_db._fundamental_publication:v1"


def check_publication_session(
    store: DuckDBStore, *, owned_registrations: tuple[str, ...] = ()
) -> None:
    """Fail before mutation if a persistent caller would lose temporary state."""
    if (
        str(store.path).startswith(":memory:")
        or not store.path.is_file()
        or store.analytical_memory_limit is None
        or store.analytical_threads is None
    ):
        return
    temporary = store.con.execute("""
        SELECT table_name FROM duckdb_tables() WHERE temporary AND NOT internal
        UNION ALL
        SELECT view_name FROM duckdb_views() WHERE temporary AND NOT internal
    """).fetchall()
    unrelated = sorted({str(row[0]) for row in temporary} - set(owned_registrations))
    if unrelated:
        raise RuntimeError(
            "fundamentals publication requires a persistent session without caller "
            f"temporary relations; retained unchanged: {unrelated}"
        )


def _recycle(store: DuckDBStore) -> None:
    if (
        str(store.path).startswith(":memory:")
        or not store.path.is_file()
        or store.analytical_memory_limit is None
        or store.analytical_threads is None
    ):
        return
    check_publication_session(store)
    store.close()
    store.reopen()


def _assert_owned_or_absent(store: DuckDBStore, name: str) -> None:
    rows = store.con.execute("""
        SELECT comment, temporary FROM duckdb_tables()
        WHERE table_name = ? AND (
            (database_name = current_database() AND schema_name = current_schema())
            OR temporary
        )
    """, [name]).fetchall()
    if any(row[0] != _OWNER or row[1] for row in rows):
        raise RuntimeError(f"fundamentals publication reserved table is not owned: {name}")
    # A view collision must also refuse before any stale-stage cleanup.
    if store.con.execute("""
        SELECT 1 FROM duckdb_views()
        WHERE view_name = ? AND (
            (database_name = current_database() AND schema_name = current_schema())
            OR temporary
        )
    """, [name]).fetchone() is not None:
        raise RuntimeError(f"fundamentals publication reserved name is a view: {name}")


def _mark_owned(store: DuckDBStore, name: str) -> None:
    store.con.execute(f"COMMENT ON TABLE {_identifier(name)} IS '{_OWNER}'")


def _build_shadow(store: DuckDBStore, table: str) -> None:
    key = _KEYS[table]
    stage, ordered, shadow = (f"{table}_bulk_{suffix}" for suffix in ("stage", "ordered", "next"))
    # Validate before keyset pagination: duplicate boundary keys must neither
    # exceed a batch bound nor cause a later page to silently skip rows.
    if store.con.execute(f"""
        SELECT {key} FROM {stage} GROUP BY {key}
        HAVING {key} IS NULL OR count(*) <> 1 LIMIT 1
    """).fetchone() is not None:
        raise RuntimeError(f"fundamentals publication stage has invalid keys: {table}")
    expected_row = store.con.execute(f"SELECT count(*) FROM {stage}").fetchone()
    assert expected_row is not None
    expected = int(expected_row[0])
    # One spillable sort makes subsequent ordered ranges prune disk row groups.
    with store.transaction():
        store.con.execute(f"CREATE TABLE {ordered} AS SELECT * FROM {stage} ORDER BY {key}")
        _mark_owned(store, ordered)
        store.con.execute(f"DROP TABLE {stage}")
        store.con.execute(f"ALTER TABLE {ordered} RENAME TO {stage}")
    ddl_row = store.con.execute("""
        SELECT sql FROM duckdb_tables()
        WHERE database_name = current_database() AND schema_name = current_schema()
          AND table_name = ?
    """, [table]).fetchone()
    if ddl_row is None or not ddl_row[0]:
        raise RuntimeError(f"missing physical fundamentals table: {table}")
    ddl = str(ddl_row[0])
    # DuckDB's catalog DDL retains column order, defaults, NOT NULL, CHECK,
    # PRIMARY KEY and UNIQUE constraints, including migrated appended columns.
    with store.transaction():
        store.con.execute(f"CREATE TABLE {shadow} " + ddl[ddl.index("("):])
        _mark_owned(store, shadow)
    _validate_shadow_contract(store, live_table=table, shadow_table=shadow)
    _recycle(store)
    batches = 0
    copied = 0
    prefix_lower: str | None = None
    # Prefix upper bounds prevent every page from scanning the whole future
    # suffix. Open first/last intervals also retain IDs outside the SHA domain.
    for prefix_upper in (*_PREFIX_BOUNDARIES, None):
        lower: str | None = None
        while True:
            clauses: list[str] = []
            params: list[str] = []
            if lower is not None:
                clauses.append(f"{key} > ?")
                params.append(lower)
            elif prefix_lower is not None:
                clauses.append(f"{key} >= ?")
                params.append(prefix_lower)
            if prefix_upper is not None:
                clauses.append(f"{key} < ?")
                params.append(prefix_upper)
            predicate = " AND ".join(clauses) or "TRUE"
            boundary = store.con.execute(f"""
                SELECT max({key}), count(*) FROM (
                    SELECT {key} FROM {stage} WHERE {predicate}
                    ORDER BY {key} LIMIT ?
                ) batch
            """, [*params, _BATCH_ROWS]).fetchone()
            assert boundary is not None
            if boundary[0] is None:
                break
            upper = str(boundary[0])
            store.con.execute(f"""
                INSERT INTO {shadow}
                SELECT * FROM {stage} WHERE {predicate} AND {key} <= ? ORDER BY {key}
            """, [*params, upper])
            copied += int(boundary[1])
            lower = upper
            batches += 1
            if batches % _REOPEN_BATCHES == 0:
                _recycle(store)
        prefix_lower = prefix_upper
    actual_row = store.con.execute(f"SELECT count(*) FROM {shadow}").fetchone()
    if actual_row is None or int(actual_row[0]) != expected or copied != expected:
        raise RuntimeError(f"fundamentals shadow row count differs from complete stage: {table}")
    _validate_shadow_contract(store, live_table=table, shadow_table=shadow)
    _recycle(store)


@contextmanager
def fundamental_publication(
    store: DuckDBStore,
    tables: tuple[str, ...],
    *,
    replace_where: str = "TRUE",
    replace_params: Sequence[object] = (),
    owned_registrations: tuple[str, ...] = (),
    before_build: Callable[[], None] | None = None,
    before_swap: Callable[[], None] | None = None,
) -> Iterator[None]:
    """Fill unindexed persistent stages, then publish complete constrained tables.

    Callers insert into ``<table>_bulk_stage``. Retained rows are copied with
    their original physical values. Only this helper's marked artifacts are
    discarded on retry/failure; live tables change solely in the final swap.
    """
    if not tables or len(set(tables)) != len(tables) or any(table not in _KEYS for table in tables):
        raise ValueError("unsupported or duplicate fundamentals publication table")
    check_publication_session(store, owned_registrations=owned_registrations)
    artifacts = tuple(f"{table}_bulk_{suffix}" for table in tables for suffix in ("stage", "ordered", "next"))
    for name in artifacts:
        _assert_owned_or_absent(store, name)
    for table in tables:
        if store.con.execute("""
            SELECT 1 FROM information_schema.tables
            WHERE table_name = ?
        """, [f"{table}_bulk_previous"]).fetchone() is not None:
            raise RuntimeError(f"fundamentals publication previous-table name already exists: {table}")
        if store.con.execute("""
            SELECT index_name FROM duckdb_indexes()
            WHERE database_name = current_database() AND schema_name = current_schema()
              AND table_name = ?
        """, [table]).fetchone() is not None:
            raise RuntimeError(f"fundamentals publication requires governed index migration 0318: {table}")
    work_started = False
    try:
        with store.transaction():
            work_started = True
            for name in artifacts:
                store.con.execute(f"DROP TABLE IF EXISTS {name}")
            for table in tables:
                declarations = []
                for column in store.con.execute(f"DESCRIBE {table}").fetchall():
                    name, data_type, nullable, _, default, _ = column
                    declaration = f"{_identifier(str(name))} {data_type}"
                    if default is not None:
                        declaration += f" DEFAULT {default}"
                    if nullable == "NO":
                        declaration += " NOT NULL"
                    declarations.append(declaration)
                stage = f"{table}_bulk_stage"
                store.con.execute(f"CREATE TABLE {stage} ({', '.join(declarations)})")
                _mark_owned(store, stage)
                store.con.execute(
                    f"INSERT INTO {stage} SELECT * FROM {table} WHERE ({replace_where}) IS NOT TRUE",
                    list(replace_params),
                )
            yield
        if before_build is not None:
            before_build()
        for relation in owned_registrations:
            with suppress(Exception):
                store.con.unregister(relation)
        check_publication_session(store)
        for table in tables:
            _build_shadow(store, table)

        def complete_publication() -> None:
            # Ownership comments belong to private artifacts, not public tables.
            # Restore the public description within the same atomic transaction.
            for table in tables:
                row = store.con.execute("""
                    SELECT comment FROM duckdb_tables()
                    WHERE database_name = current_database() AND schema_name = current_schema()
                      AND table_name = ?
                """, [table]).fetchone()
                assert row is not None
                comment = "NULL" if row[0] is None else "'" + str(row[0]).replace("'", "''") + "'"
                store.con.execute(f"COMMENT ON TABLE {table}_bulk_next IS {comment}")
            if before_swap is not None:
                before_swap()

        publish_validated_shadows(
            store,
            tables=tuple((table, f"{table}_bulk_next") for table in tables),
            before_swap=complete_publication,
        )
    except BaseException:
        if work_started:
            with suppress(Exception):
                store.con.execute("ROLLBACK")
        raise
    finally:
        # An invalidated DB may reject cleanup. Preserve the original error;
        # marked leftovers are safe for a subsequent explicit retry to remove.
        for name in (artifacts if work_started else ()):
            with suppress(Exception):
                _assert_owned_or_absent(store, name)
                store.con.execute(f"DROP TABLE IF EXISTS {name}")
