"""Failure ledger recovery after a dataset invalidates its DuckDB connection."""

from __future__ import annotations

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.dataset import Dataset, DatasetLoadResult


class _InvalidatedConnection:
    def __init__(self, connection, events):
        self.connection = connection
        self.events = events

    def execute(self, *_args, **_kwargs):
        self.events.append("invalidated_execute")
        raise duckdb.FatalException("connection invalidated after COMMIT failure")

    def close(self):
        self.events.append("raw_close")
        self.connection.close()


class _FatalDataset(Dataset):
    dataset_id = "fatal_fixture"
    source_name = "fixture"

    def __init__(self, original, events):
        self.original = original
        self.events = events

    def ensure_schema(self, store):
        self.events.append("ensure_schema")

    def load(self, store, options) -> DatasetLoadResult:
        self.events.append("load")
        store.con.execute("INSERT INTO partial_rows VALUES (42)")
        store.connection = _InvalidatedConnection(store.con, self.events)
        raise self.original


def test_dataset_run_records_failure_after_invalidated_connection(tmp_path, monkeypatch):
    path = tmp_path / "dataset-failure.duckdb"
    events = []
    opens = []
    configs = []
    original = duckdb.FatalException("original COMMIT failure: query memory exhausted")
    real_connect = duckdb.connect

    def connect_spy(*args, **kwargs):
        # Every store connect is bounded now; the recovery reopen is the one after the raw close.
        if "raw_close" in events:
            events.append("bounded_reopen")
            configs.append(kwargs.get("config"))
        else:
            opens.append(kwargs.get("config"))
        return real_connect(*args, **kwargs)

    def initialize(store):
        events.append("initialize")
        store.con.execute("""
            CREATE TABLE dataset_runs (
                run_id VARCHAR PRIMARY KEY, dataset_id VARCHAR, status VARCHAR,
                started_at TIMESTAMP, finished_at TIMESTAMP, source VARCHAR,
                params_json VARCHAR, error_message VARCHAR, rows_loaded BIGINT
            );
            CREATE TABLE partial_rows (row_value INTEGER);
        """)

    monkeypatch.setattr(duckdb, "connect", connect_spy)
    monkeypatch.setattr(DuckDBStore, "initialize", initialize)
    with DuckDBStore(path) as store:
        store.analytical_memory_limit = "128MB"
        store.analytical_threads = 1
        with pytest.raises(duckdb.FatalException) as caught:
            _FatalDataset(original, events).run(store, {"fixture": True})

    assert caught.value is original
    assert events.count("load") == 1
    assert events.count("ensure_schema") == 1
    assert events.count("initialize") == 1
    assert events.count("invalidated_execute") == 1
    assert events.count("raw_close") == 1
    assert events.index("raw_close") < events.index("bounded_reopen")
    spill = str(store.temp_directory)
    # The first open carries the store's default budget; the recovery reopen the recorded analytical one.
    assert opens == [{"memory_limit": "384MB", "threads": 1, "preserve_insertion_order": False,
                      "temp_directory": spill, "max_temp_directory_size": "40GB"}]
    assert configs == [{"memory_limit": "128MB", "threads": 1, "preserve_insertion_order": False,
                        "temp_directory": spill, "max_temp_directory_size": "40GB"}]

    with real_connect(str(path), config={"memory_limit": "128MB", "threads": "1"}) as con:
        status, finished_at, error_message, rows_loaded = con.execute(
            "SELECT status, finished_at, error_message, rows_loaded FROM dataset_runs"
        ).fetchone()
        assert status == "failed" and finished_at is not None
        assert str(original) in error_message
        assert "raise self.original" in error_message
        assert rows_loaded is None
        assert con.execute("SELECT row_value FROM partial_rows").fetchall() == [(42,)]
