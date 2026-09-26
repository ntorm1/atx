from __future__ import annotations

import hashlib
import os

import duckdb
import pytest

from atx_db.connection import DuckDBStore, spill_root


@pytest.mark.parametrize("read_only", [False, True])
def test_reopen_budget_is_active_before_session_setup(tmp_path, monkeypatch, read_only):
    path = tmp_path / "budgeted.duckdb"
    with duckdb.connect(str(path), config={"memory_limit": "64MB", "threads": "1"}) as con:
        con.execute("CREATE TABLE committed_rows AS SELECT 42 AS value")
        expected_memory = con.execute("SELECT current_setting('memory_limit')").fetchone()[0]
    original_connect = duckdb.connect
    observations = []

    def observe_connect(*args, **kwargs):
        # Inspect before DuckDBStore can issue its first session SET statement.
        con = original_connect(*args, **kwargs)
        observations.append((kwargs, con.execute(
            "SELECT current_setting('memory_limit'), current_setting('threads'), "
            "current_setting('preserve_insertion_order')"
        ).fetchone()))
        return con

    monkeypatch.setattr(duckdb, "connect", observe_connect)
    store = DuckDBStore(path, read_only=read_only)
    store.analytical_memory_limit = "64MB"
    store.analytical_threads = 1
    try:
        store.reopen()
        assert store.con.execute("SELECT value FROM committed_rows").fetchone() == (42,)
        assert store.con.execute("SELECT current_setting('TimeZone')").fetchone() == ("UTC",)
        assert observations == [(
            {"read_only": read_only, "config": {
                "memory_limit": "64MB", "threads": 1, "preserve_insertion_order": False,
                "temp_directory": str(store.temp_directory), "max_temp_directory_size": "40GB",
            }},
            (expected_memory, 1, False),
        )]
        # A private spill directory for this (process, file) below the shared root (ruling C-56).
        assert store.temp_directory.parent == spill_root()
        assert store.temp_directory.name.startswith(f"conn-{os.getpid()}-")
    finally:
        store.close()


def test_readonly_close_and_reopen_preserve_file_and_committed_rows(tmp_path):
    path = tmp_path / "readonly.duckdb"
    with duckdb.connect(str(path), config={"memory_limit": "64MB", "threads": "1"}) as con:
        con.execute("CREATE TABLE committed_rows AS SELECT 42 AS value")
    before = hashlib.sha256(path.read_bytes()).hexdigest()
    store = DuckDBStore(path, read_only=True)
    store.analytical_memory_limit = "64MB"
    store.analytical_threads = 1
    try:
        store.reopen()
        store.close()  # CHECKPOINT would fail on this real read-only connection.
        assert store.connection is None
        store.reopen()
        assert store.con.execute("SELECT value FROM committed_rows").fetchone() == (42,)
    finally:
        store.close()
    assert hashlib.sha256(path.read_bytes()).hexdigest() == before
