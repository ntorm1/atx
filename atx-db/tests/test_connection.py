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
