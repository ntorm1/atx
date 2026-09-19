from __future__ import annotations

from datetime import UTC, datetime, timedelta

from atx_db.connection import DuckDBStore, open_duckdb_connection


def test_duckdb_store_session_uses_utc(monkeypatch) -> None:
    monkeypatch.setattr(DuckDBStore, "initialize", lambda _store: None)
    with DuckDBStore(":memory:") as store:
        timezone_name, database_now = store.con.execute(
            "SELECT current_setting('TimeZone'), CAST(now() AS TIMESTAMP)"
        ).fetchone()
        timezone_aware_now = store.con.execute("SELECT now()").fetchone()[0]

    assert timezone_name == "UTC"
    python_now = datetime.now(UTC).replace(tzinfo=None)
    assert abs((python_now - database_now).total_seconds()) < 5
    assert timezone_aware_now.utcoffset() == timedelta(0)


def test_raw_duckdb_connection_uses_utc() -> None:
    with open_duckdb_connection(":memory:") as connection:
        assert connection.execute("SELECT current_setting('TimeZone')").fetchone() == ("UTC",)


def test_close_then_reopen_round_trips_a_usable_connection(monkeypatch, tmp_path) -> None:
    monkeypatch.setattr(DuckDBStore, "initialize", lambda _store: None)
    db_path = tmp_path / "close-reopen.duckdb"
    store = DuckDBStore(db_path).__enter__()
    try:
        store.con.execute("CREATE TABLE t (x INTEGER)")
        store.con.execute("INSERT INTO t VALUES (1)")
        store.close()
        assert store.connection is None
        store.reopen()
        assert store.connection is not None
        row = store.con.execute("SELECT x FROM t").fetchone()
        assert row == (1,)
    finally:
        store.__exit__(None, None, None)


def test_reopen_replays_recorded_analytical_settings(monkeypatch, tmp_path) -> None:
    monkeypatch.setattr(DuckDBStore, "initialize", lambda _store: None)
    db_path = tmp_path / "analytical-reopen.duckdb"
    store = DuckDBStore(db_path).__enter__()
    try:
        store.analytical_memory_limit = "512MB"
        store.analytical_threads = 2
        store.con.execute("SET memory_limit = ?", [store.analytical_memory_limit])
        store.con.execute("SET threads = ?", [store.analytical_threads])
        store.con.execute("SET preserve_insertion_order = false")

        store.close()
        store.reopen()

        threads, preserve_order = store.con.execute(
            "SELECT current_setting('threads'), current_setting('preserve_insertion_order')"
        ).fetchone()
        assert int(threads) == 2
        assert preserve_order in (False, "false")
    finally:
        store.__exit__(None, None, None)


def test_reopen_without_recorded_analytical_settings_does_not_set_them(monkeypatch, tmp_path) -> None:
    """A store that never had configure_analytical_session applied stays plain."""
    monkeypatch.setattr(DuckDBStore, "initialize", lambda _store: None)
    db_path = tmp_path / "plain-reopen.duckdb"
    store = DuckDBStore(db_path).__enter__()
    try:
        assert store.analytical_memory_limit is None
        assert store.analytical_threads is None
        store.close()
        store.reopen()
        # No error, and the store is usable -- absence of recorded settings must
        # not be treated as "replay defaults" or otherwise raise.
        assert store.con.execute("SELECT 1").fetchone() == (1,)
    finally:
        store.__exit__(None, None, None)
