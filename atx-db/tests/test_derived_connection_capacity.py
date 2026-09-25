"""Tiny real-store lifecycle checks; production memory capacity is a separate gate."""

from __future__ import annotations

from contextlib import closing
from uuid import UUID

import duckdb
import pytest

from atx_db import _derived_pit as pit
from atx_db import derived_metrics as engine
from atx_db.cli import _configure_analytical_session
from atx_db.connection import DuckDBStore
from atx_db.derived_registry import default_derived_definitions
from tests.test_derived_annual import current, fact

SOURCE = "connection-capacity-test"
SETTINGS_SQL = """SELECT current_setting('memory_limit'), current_setting('threads'),
    current_setting('preserve_insertion_order'), current_setting('TimeZone'), current_setting('temp_directory')"""


class _ObservedConnection:
    def __init__(self, connection, statements):
        self.connection = connection
        self.statements = statements

    def execute(self, sql, *args, **kwargs):
        self.statements.append(sql)
        return self.connection.execute(sql, *args, **kwargs)

    def __getattr__(self, name):
        return getattr(self.connection, name)


@pytest.fixture
def small_catalog(monkeypatch):
    codes = {"revenue_ttm", "revenue_growth_yoy", "current_ratio"}
    definitions = tuple(d for d in default_derived_definitions() if d.metric_code in codes)
    assert {d.metric_code for d in definitions} == codes
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: definitions)


def _unconfigured(store):
    # conftest records its test budget as replay metadata (7d0b7ee9); an
    # unconfigured caller has none, so clear it for the unconfigured cases.
    store.analytical_memory_limit = None
    store.analytical_threads = None
    return store


@pytest.fixture
def configured_store(tmp_store, small_catalog):
    _configure_analytical_session(tmp_store, memory_limit="256MB", threads=1)
    return tmp_store


def _snapshot(store, *, security=None, source=SOURCE):
    predicate = "source = ?"
    params = [source]
    if security is not None:
        predicate += " AND security_id = ?"
        params.append(security)
    # Include every modeled state column, excluding only the load-time default.
    return store.con.execute(
        f"SELECT {', '.join(pit.STATE_COLUMNS)} FROM derived_metric_values "
        f"WHERE {predicate} ORDER BY derived_value_id", params,
    ).fetchall()


def _assert_no_temps(store):
    assert store.con.execute(
        "SELECT table_name FROM duckdb_tables() WHERE temporary AND NOT internal"
    ).fetchall() == []
    assert store.con.execute(
        "SELECT view_name FROM duckdb_views() WHERE temporary AND NOT internal"
    ).fetchall() == []


def _id_snapshots(store):
    return store.con.execute("""
        SELECT table_name, temporary FROM duckdb_tables()
        WHERE starts_with(table_name, '_derived_security_ids_') ORDER BY table_name
    """).fetchall()


def _copy_scope(store, *, security, source=SOURCE):
    store.con.execute("""
        INSERT INTO derived_metric_values
        SELECT * REPLACE (sha256(? || derived_value_id) AS derived_value_id,
                          ? AS security_id, ? AS source)
        FROM derived_metric_values WHERE source = ? AND security_id = 'S2'
    """, [security + source, security, source, SOURCE])


def _seed_mixed_history(store):
    # Keep P1's seven-quarter values/events, using the AF1 helper to supply
    # actual contiguous fiscal spans required for a comparable quarterly TTM.
    quarters = (
        ("2023-04-01", "2023-06-30", "2023-08-10 12:00:00"),
        ("2023-07-01", "2023-09-30", "2023-11-10 12:00:00"),
        ("2023-10-01", "2023-12-31", "2024-02-10 12:00:00"),
        ("2024-01-01", "2024-03-31", "2024-05-10 12:00:00"),
        ("2024-04-01", "2024-06-30", "2024-08-10 12:00:00"),
        ("2024-07-01", "2024-09-30", "2024-11-10 12:00:00"),
        ("2024-10-01", "2024-12-31", "2025-02-10 12:00:00"),
    )
    for index, (start, end, at) in enumerate(quarters):
        fact(store, "revenue", 70 + 10 * index, start=start, end=end, at=at, basis="quarterly")
        fact(store, "cost_of_revenue_cogs", 60, start=start, end=end, at=at, basis="quarterly")
    fact(store, "revenue", 160, start="2024-04-01", end="2024-06-30",
         at="2025-03-10 12:00:00", basis="quarterly", revision=2)
    # Match AF1's invalid-revision fixture: standardized values are NOT NULL,
    # while an admitted nonfinite value produces a canonical NULL state.
    fact(store, "revenue", float("nan"), start="2024-04-01", end="2024-06-30",
         at="2025-04-10 12:00:00", basis="quarterly", revision=3)
    fact(store, "revenue", 1000, security="S2")
    fact(store, "revenue", 1100, security="S2", at="2025-03-10", revision=2)
    fact(store, "revenue", float("nan"), security="S2", at="2025-04-10", revision=3)
    fact(store, "current_assets", 250, start=None, basis="instant", security="S3")
    fact(store, "current_liabilities", 125, start=None, basis="instant", security="S3")
    fact(store, "revenue", 700, security="S4")


@pytest.mark.parametrize("batch_size", [1, 3])
def test_keyset_reopens_preserve_full_history_stale_cleanup_and_budget(configured_store, monkeypatch, batch_size):
    store = configured_store
    expected_settings = store.con.execute(SETTINGS_SQL).fetchone()
    _seed_mixed_history(store)
    options = engine.DerivedMetricsOptions(source=SOURCE, batch_size=batch_size, run_id="same-build")
    monkeypatch.setattr(engine, "_DERIVED_REOPEN_SECURITIES", 100)
    expected_count = engine.refresh_derived_metrics(store, options)
    assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings
    assert _id_snapshots(store) == []
    expected = _snapshot(store)
    assert expected_count == len(expected) > 0
    assert current(store, "revenue_ttm", cutoff="2025-02-21")[:2] == (460, "quarterly")
    assert current(store, "revenue_ttm", cutoff="2025-03-21")[:2] == (510, "quarterly")
    assert current(store, "revenue_ttm")[0] is None
    assert current(store, "revenue_ttm", security="S2", cutoff="2025-02-21")[:2] == (1000, "annual_fallback")
    assert current(store, "revenue_ttm", security="S2", cutoff="2025-03-21")[:2] == (1100, "annual_fallback")
    assert current(store, "revenue_ttm", security="S2")[0] is None
    assert current(store, "current_ratio", security="S3")[0] == 2
    # Deleting early stale scopes must not move later IDs out of traversal;
    # another stale-only ID lies beyond all standardized IDs.
    for security in ("A0", "B0", "Z9"):
        _copy_scope(store, security=security)
    _copy_scope(store, security="FOREIGN", source="other-source")
    foreign = _snapshot(store, source="other-source")
    statements = []
    store.connection = _ObservedConnection(store.con, statements)
    visited = []
    closed_at = []
    reopened_at = []
    original_prepare = pit.prepare_security
    original_close = store.close
    original_reopen = store.reopen

    def prepare(con, security_id, limit):
        visited.append(security_id)
        original_prepare(con, security_id, limit)

    def close():
        _assert_no_temps(store)
        # A nested BEGIN would fail if publication left any transaction open.
        store.con.execute("BEGIN TRANSACTION")
        store.con.execute("ROLLBACK")
        closed_at.append(len(visited))
        original_close()

    def reopen():
        assert store.connection is None
        original_reopen()
        store.connection = _ObservedConnection(store.con, statements)
        assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings
        _assert_no_temps(store)
        reopened_at.append(len(visited))

    monkeypatch.setattr(engine, "_DERIVED_REOPEN_SECURITIES", 2)
    monkeypatch.setattr(pit, "prepare_security", prepare)
    monkeypatch.setattr(store, "close", close)
    monkeypatch.setattr(store, "reopen", reopen)
    assert engine.refresh_derived_metrics(store, options) == expected_count
    assert visited == ["A0", "B0", "S1", "S2", "S3", "S4", "Z9"]
    assert closed_at == reopened_at == [2, 4, 6, 7]
    assert _snapshot(store) == expected
    assert _snapshot(store, source="other-source") == foreign
    assert _snapshot(store, security="Z9") == []
    assert _id_snapshots(store) == []
    enumerations = [sql for sql in statements if "fundamental_standardized" in sql and "UNION" in sql]
    assert len(enumerations) == 1
    assert "derived_metric_values" in enumerations[0]
    assert "CREATE TABLE" in enumerations[0]
    pages = [sql for sql in statements if sql.startswith("SELECT security_id FROM")]
    assert len(pages) == (7 + batch_size - 1) // batch_size + 1
    assert all('_derived_security_ids_' in sql and 'LIMIT ?' in sql for sql in pages)
    assert all('fundamental_standardized' not in sql and 'derived_metric_values' not in sql for sql in pages)


def test_explicit_scopes_deduplicate_sort_and_keep_dependency_closure(configured_store, monkeypatch):
    store = configured_store
    for security in ("S1", "S2", "S3"):
        fact(store, "revenue", 1000, security=security)
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(source=SOURCE))
    _copy_scope(store, security="Z9")
    untouched = _snapshot(store, security="S2")
    for security in ("S1", "S2", "S3"):
        fact(store, "revenue", 1100, security=security, at="2025-03-10", revision=2)
    visited = []
    reopens = []
    original_prepare = pit.prepare_security
    original_reopen = store.reopen

    def prepare(con, security_id, limit):
        visited.append(security_id)
        original_prepare(con, security_id, limit)

    def reopen():
        original_reopen()
        reopens.append(len(visited))

    monkeypatch.setattr(engine, "_DERIVED_REOPEN_SECURITIES", 2)
    monkeypatch.setattr(pit, "prepare_security", prepare)
    monkeypatch.setattr(store, "reopen", reopen)
    options = engine.DerivedMetricsOptions(
        source=SOURCE, security_ids=("Z9", "S3", "S1", "S3"), metric_codes=("revenue_ttm",), batch_size=3,
    )
    assert list(engine.select_security_batches(store, options)) == [("S1", "S3", "Z9")]
    count = engine.refresh_derived_metrics(store, options)
    assert visited == ["S1", "S3", "Z9"]
    assert reopens == [2, 3]
    assert _snapshot(store, security="S2") == untouched
    assert store.con.execute("""
        SELECT DISTINCT metric_code FROM derived_metric_values
        WHERE source = ? AND security_id IN ('S1', 'S3') AND available_at = TIMESTAMP '2025-03-10'
        ORDER BY metric_code
    """, [SOURCE]).fetchall() == [("revenue_growth_yoy",), ("revenue_ttm",)]
    assert store.con.execute("""
        SELECT count(*) FROM derived_metric_values WHERE source = ? AND security_id IN ('S1', 'S3')
          AND metric_code IN ('revenue_ttm', 'revenue_growth_yoy')
    """, [SOURCE]).fetchone() == (count,)
    # Scoped metric cleanup preserves the independent family in the stale scope.
    assert {row[3] for row in _snapshot(store, security="Z9")} == {"current_ratio"}


@pytest.mark.parametrize("kind,name", [("TABLE", "caller_state"), ("VIEW", "caller_state"), ("TABLE", "_pit_stage")])
def test_caller_temporary_state_is_refused_before_any_mutation(configured_store, monkeypatch, kind, name):
    store = configured_store
    fact(store, "revenue", 1000)
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(source=SOURCE))
    before = _snapshot(store)
    fact(store, "revenue", 1100, at="2025-03-10", revision=2)
    store.con.execute(f"CREATE TEMP {kind} {name} AS SELECT 99 AS value")
    connection = store.con

    def unexpected_initialize():
        pytest.fail("caller temporary state must be refused before initialize")

    monkeypatch.setattr(store, "initialize", unexpected_initialize)
    with pytest.raises(RuntimeError, match="caller-owned temporary objects"):
        engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(source=SOURCE))
    assert store.con is connection
    assert store.con.execute(f"SELECT value FROM {name}").fetchone() == (99,)
    assert _snapshot(store) == before


def test_failure_after_reopen_keeps_earlier_commit_and_failing_scope(configured_store, monkeypatch):
    store = configured_store
    for security in ("S1", "S2"):
        fact(store, "revenue", 1000, security=security)
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(source=SOURCE))
    before_s2 = _snapshot(store, security="S2")
    for security in ("S1", "S2"):
        fact(store, "revenue", 1100, security=security, at="2025-03-10", revision=2)
    reopens = []
    original_reopen = store.reopen
    original_finish = pit.finish_metric

    def reopen():
        original_reopen()
        reopens.append(1)

    def duplicate(con):
        original_finish(con)
        if con.execute("SELECT count(*) FROM _pit_stage WHERE security_id = 'S2'").fetchone()[0]:
            con.execute("INSERT INTO _pit_stage SELECT * FROM _pit_stage LIMIT 1")

    monkeypatch.setattr(engine, "_DERIVED_REOPEN_SECURITIES", 1)
    monkeypatch.setattr(store, "reopen", reopen)
    monkeypatch.setattr(pit, "finish_metric", duplicate)
    with pytest.raises(Exception, match=r"[Dd]uplicate|[Pp]rimary|[Cc]onstraint"):
        engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(source=SOURCE, batch_size=2))
    assert reopens == [1]
    assert current(store, "revenue_ttm")[0] == 1100
    assert _snapshot(store, security="S2") == before_s2
    _assert_no_temps(store)
    assert _id_snapshots(store) == []


@pytest.mark.parametrize("configured", [True, False])
def test_early_iterator_close_removes_only_its_snapshot(tmp_store, small_catalog, configured):
    store = _unconfigured(tmp_store)
    if configured:
        _configure_analytical_session(store, memory_limit="256MB", threads=1)
    caller_table = "_derived_security_ids_caller_owned"
    store.con.execute(f"CREATE TABLE {caller_table} AS SELECT 99 AS value")
    fact(store, "revenue", 1000, security="S1")
    fact(store, "revenue", 1100, security="S2")
    with closing(engine.select_security_batches(store)) as batches:
        assert next(batches) == ("S1",)
        owned = [row for row in _id_snapshots(store) if row[0] != caller_table]
        assert len(owned) == 1
        assert owned[0][1] == (not configured)
        if configured:
            store.close()
            store.reopen()
            assert _id_snapshots(store) == sorted([(caller_table, False), *owned])
        assert next(batches) == ("S2",)
        # Exit without exhausting the generator; closing must release ownership.
    assert _id_snapshots(store) == [(caller_table, False)]
    assert store.con.execute(f"SELECT value FROM {caller_table}").fetchone() == (99,)


@pytest.mark.parametrize("configured", [True, False])
def test_snapshot_name_collision_preserves_caller_table(tmp_store, small_catalog, monkeypatch, configured):
    store = _unconfigured(tmp_store)
    if configured:
        _configure_analytical_session(store, memory_limit="256MB", threads=1)
    identity = UUID("12345678-1234-1234-1234-123456789abc")
    name = f"_derived_security_ids_{identity.hex}"
    kind = "" if configured else "TEMP "
    store.con.execute(f"CREATE {kind}TABLE {name} AS SELECT 99 AS value")
    monkeypatch.setattr(engine, "uuid4", lambda: identity)
    with (
        closing(engine.select_security_batches(store)) as batches,
        pytest.raises(duckdb.CatalogException, match="already exists"),
    ):
        next(batches)
    assert store.con.execute(f"SELECT value FROM {name}").fetchone() == (99,)


def test_unconfigured_persistent_store_keeps_its_session(tmp_store, small_catalog, monkeypatch):
    store = _unconfigured(tmp_store)
    store.con.execute("SET memory_limit='256MB'")
    store.con.execute("CREATE TEMP TABLE caller_state AS SELECT 99 AS value")
    expected_settings = store.con.execute(SETTINGS_SQL).fetchone()
    connection = store.con
    fact(store, "revenue", 1000)
    monkeypatch.setattr(engine, "_DERIVED_REOPEN_SECURITIES", 1)
    assert engine.refresh_derived_metrics(store) > 0
    assert store.con is connection
    assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings
    assert store.con.execute("SELECT value FROM caller_state").fetchone() == (99,)
    assert _id_snapshots(store) == []


def test_configured_memory_store_retains_database_and_caller_temp(tmp_store, small_catalog, monkeypatch):
    # Reuse the current table DDL; avoid a second full schema bootstrap for this
    # two-table in-memory compatibility check.
    definitions = tmp_store.con.execute("""
        SELECT sql FROM duckdb_tables()
        WHERE table_name IN ('fundamental_standardized', 'derived_metric_values')
        ORDER BY table_name
    """).fetchall()
    assert len(definitions) == 2
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:", config={"memory_limit": "256MB", "threads": "1"})
    try:
        for (sql,) in definitions:
            store.con.execute(sql)
        store._initialized = True
        _configure_analytical_session(store, memory_limit="256MB", threads=1)
        store.con.execute("CREATE TEMP TABLE caller_state AS SELECT 99 AS value")
        connection = store.con
        fact(store, "revenue", 1000)
        monkeypatch.setattr(engine, "_DERIVED_REOPEN_SECURITIES", 1)
        count = engine.refresh_derived_metrics(store)
        assert count > 0
        assert store.con is connection
        assert current(store, "revenue_ttm")[:2] == (1000, "annual_fallback")
        assert store.con.execute("SELECT count(*) FROM derived_metric_values").fetchone() == (count,)
        assert store.con.execute("SELECT value FROM caller_state").fetchone() == (99,)
        assert _id_snapshots(store) == []
    finally:
        store.__exit__(None, None, None)
