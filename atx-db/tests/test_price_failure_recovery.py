"""Small persistent-DB regressions for fatal publication/activation failures."""

from __future__ import annotations

import datetime as dt
import logging

import duckdb
import pytest

from atx_db import activation
from atx_db import ticker_history_bulk as bulk
from atx_db.connection import DuckDBStore


class InvalidatedConnection:
    def __init__(self, con, events):
        self.con = con
        self.events = events

    def execute(self, *_args, **_kwargs):
        self.events.append("invalidated_execute")
        raise duckdb.FatalException("database invalidated after original COMMIT failure")

    def close(self):
        self.events.append("raw_close")
        self.con.close()


@pytest.mark.parametrize("entry", ["bulk", "activation_bulk", "activation_other", "reopen_failure"])
def test_original_fatal_error_survives_bounded_ledger_recovery(
    tmp_path, monkeypatch, caplog, entry
):
    path = tmp_path / "failure.duckdb"
    source = tmp_path / "source.tsv"
    source.write_text("fixture source", encoding="utf-8")
    events = []
    configs = []
    init_calls = []
    original = duckdb.FatalException("original COMMIT failure: query memory exhausted")
    real_connect = duckdb.connect

    def connect_spy(*args, **kwargs):
        config = kwargs.get("config")
        if config is not None:
            configs.append(config)
            events.append("bounded_reopen")
            if entry == "reopen_failure":
                raise duckdb.IOException("injected reopen unavailable")
        return real_connect(*args, **kwargs)

    def initialize(store):
        init_calls.append(True)
        store.con.execute("""
            CREATE TABLE dataset_runs (
                run_id VARCHAR PRIMARY KEY, dataset_id VARCHAR, status VARCHAR,
                started_at TIMESTAMP, finished_at TIMESTAMP, source VARCHAR,
                params_json VARCHAR, error_message VARCHAR
            );
            CREATE TABLE activation_stage_runs (
                stage VARCHAR, run_id VARCHAR, status VARCHAR, started_at TIMESTAMP,
                finished_at TIMESTAMP, rows BIGINT, params_json VARCHAR, error VARCHAR,
                PRIMARY KEY (stage, run_id)
            );
            CREATE TABLE equity_daily_bars AS SELECT 'original-live' AS marker;
        """)

    def symbols(store):
        store.con.execute("""
            CREATE TEMP TABLE broad_symbol_map (symbol VARCHAR, security_id VARCHAR);
            CREATE TEMP TABLE ticker_history_source_rows (current_symbol VARCHAR);
        """)

    def staging(store, _options):
        store.con.execute("CREATE TABLE equity_daily_bars_bulk_next AS SELECT 'staged' AS marker")

    def fatal(store, _options):
        events.append("source_failure")
        store.connection = InvalidatedConnection(store.con, events)
        raise original

    monkeypatch.setattr(duckdb, "connect", connect_spy)
    monkeypatch.setattr(DuckDBStore, "initialize", initialize)
    monkeypatch.setattr(bulk, "record_source_file", lambda *_a, **_kw: None)
    monkeypatch.setattr(bulk, "stage_source", lambda *_a: None)
    monkeypatch.setattr(bulk, "source_diagnostics", lambda *_a: {"quarantined_positive_key_rows": 0})
    monkeypatch.setattr(bulk, "quality_check", lambda *_a, **_kw: None)
    monkeypatch.setattr(bulk, "_create_symbol_map", symbols)
    monkeypatch.setattr(bulk, "_create_line_map", lambda *_a: None)
    monkeypatch.setattr(bulk, "_create_next_table", staging)
    monkeypatch.setattr(bulk, "_validate_next", lambda *_a: (1, 1, dt.date(2026, 9, 18), 1, 0, 0))
    monkeypatch.setattr(bulk, "_publish", fatal)
    caplog.set_level(logging.INFO, logger=bulk.__name__)
    lines = []
    if entry == "bulk":
        with DuckDBStore(path) as store, pytest.raises(duckdb.FatalException) as caught:
            bulk.publish_bulk_ticker_history(
                store,
                bulk.BulkTickerHistoryOptions(
                    source_path=source, memory_limit="128MB", threads=1, run_id="test-broad-bars"
                ),
            )
    else:
        stage = "ticker_history_publish"
        if entry == "activation_other":
            stage = "periods"
            monkeypatch.setitem(activation.STAGES, stage, fatal)
        with pytest.raises(duckdb.FatalException) as caught:
            activation.run_activation(
                activation.ActivationOptions(
                    db_path=path, ticker_history_source_path=source,
                    memory_limit="128MB", threads=1, run_id="test", force=True,
                ),
                stages=(stage,), emit=lines.append,
            )
        assert len(lines) == 1 and lines[0]["status"] == "failed"
        assert lines[0]["detail"]["error"] == str(original)

    assert caught.value is original
    assert init_calls == [True]  # Recovery must never initialize/migrate the warehouse.
    assert events.count("source_failure") == 1  # No destructive publication retry.
    assert events.count("raw_close") == 1
    assert events.index("raw_close") < events.index("bounded_reopen")
    assert configs
    for config in configs:
        assert config["memory_limit"] == "128MB"
        assert config["threads"] == "1"
        assert config["preserve_insertion_order"] == "false"
    if entry != "activation_other":
        assert "validated 1 rows across 1 securities; latest breadth 1" in caplog.text

    with real_connect(str(path), config={"memory_limit": "128MB", "threads": "1"}) as con:
        assert con.execute("SELECT marker FROM equity_daily_bars").fetchone() == ("original-live",)
        if entry != "activation_other":
            assert con.execute("SELECT marker FROM equity_daily_bars_bulk_next").fetchone() == ("staged",)
            dataset = con.execute("SELECT status, finished_at, error_message FROM dataset_runs").fetchone()
            if entry == "reopen_failure":
                assert dataset[0] == "running"
            else:
                assert dataset[0] == "failed" and dataset[1] is not None
                assert dataset[2] == str(original)
        if entry != "bulk":
            ledger = con.execute("SELECT status, finished_at, error FROM activation_stage_runs").fetchone()
            if entry == "reopen_failure":
                assert ledger[0] == "running"
                assert lines[0]["detail"]["failure_ledger"] == "operator_recovery_required"
                assert any("dataset_runs" in note for note in original.__notes__)
                assert any("activation_stage_runs" in note for note in original.__notes__)
                assert "Operator recovery required" in caplog.text
            else:
                assert ledger[0] == "failed" and ledger[1] is not None
                assert ledger[2] == str(original)
