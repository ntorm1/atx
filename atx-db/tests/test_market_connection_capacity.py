"""Small lifecycle fixtures; arithmetic/PIT matrices remain in their existing tests."""

from __future__ import annotations

import datetime as dt

import duckdb
import pytest

from atx_db import market_daily as engine
from atx_db.cli import _configure_analytical_session
from atx_db.connection import DuckDBStore
from atx_db.derived_registry import DERIVED_SOURCE_NAME
from tests.test_market_daily import _assert_rows_equal, _bar, _fact, _weekdays

SETTINGS_SQL = (
    "SELECT current_setting('memory_limit'), current_setting('threads'), "
    "current_setting('preserve_insertion_order'), current_setting('TimeZone'), "
    "current_setting('temp_directory')"
)
TEMP_SQL = """SELECT EXISTS (
    SELECT 1 FROM duckdb_tables() WHERE temporary AND NOT internal
) OR EXISTS (SELECT 1 FROM duckdb_views() WHERE temporary AND NOT internal)"""


@pytest.fixture
def store(tmp_store):
    # Apply real SQL caps even to the unconfigured baseline, without recording
    # replay metadata until the individual test opts into the production path.
    # conftest records its own test budget as replay metadata (7d0b7ee9); clear it
    # so the baseline really is an unconfigured caller.
    tmp_store.analytical_memory_limit = None
    tmp_store.analytical_threads = None
    tmp_store.con.execute("SET memory_limit='512MB'")
    tmp_store.con.execute("SET threads=1")
    tmp_store.con.execute("SET preserve_insertion_order=false")
    return tmp_store


def _seed(store, *, securities=("S1", "S2", "S3"), days=25):
    dates = _weekdays(dt.date(2025, 1, 2), days)
    for index, security in enumerate(securities):
        _fact(store, security, "preferred_stock", "instant", dt.date(2024, 12, 31),
              5.0 + index, dt.datetime(2025, 1, 1, 12))
        store.con.execute("""
            INSERT INTO derived_metric_values (
                derived_value_id, source, security_id, metric_code, metric_window,
                period_end, value, available_at, inputs_hash, as_of_date, value_status,
                value_origin, fiscal_period_end
            ) VALUES (?, ?, ?, 'common_equity_q', 'q', DATE '2024-12-31', ?,
                      TIMESTAMP '2025-01-01 12:00:00', ?, DATE '2025-01-01', 'valid',
                      'instant', DATE '2024-12-31')
        """, [f"{security}|book", DERIVED_SOURCE_NAME, security, 500.0 + index, f"{security}|book-input"])
        for offset, trade_date in enumerate(dates):
            _bar(store, security, trade_date, 20.0 + index + offset / 100, shares=100.0 + index)
    return dates


def _snapshot(store, *, physical=False):
    projection = "*" if physical else "* EXCLUDE (source_loaded_at)"
    cursor = store.con.execute(
        f"SELECT {projection} FROM market_daily_metrics ORDER BY source, security_id, trade_date"
    )
    columns = [column[0] for column in cursor.description]
    return columns, cursor.fetchall()


def _assert_snapshot(actual, expected):
    columns, actual_rows = actual
    expected_columns, expected_rows = expected
    assert columns == expected_columns
    assert len(actual_rows) == len(expected_rows)
    for actual_row, expected_row in zip(actual_rows, expected_rows, strict=True):
        _assert_rows_equal(actual_row, expected_row, columns, range(len(columns)))


def _observe_recycling(store, monkeypatch, expected_settings):
    counts = []
    close = store.close
    reopen = store.reopen

    def observed_close():
        assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings
        assert store.con.execute(TEMP_SQL).fetchone() == (False,)
        close()
        assert store.connection is None

    def observed_reopen():
        assert store.connection is None
        reopen()
        assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings
        assert store.con.execute(TEMP_SQL).fetchone() == (False,)
        counts.append(store.con.execute("SELECT count(*) FROM market_daily_metrics").fetchone()[0])

    monkeypatch.setattr(store, "close", observed_close)
    monkeypatch.setattr(store, "reopen", observed_reopen)
    return counts


@pytest.mark.parametrize("batch_size", [1, 2])
def test_recycles_each_batch_with_budget_replay_and_identical_output(store, monkeypatch, batch_size):
    assert store.path.is_file()
    # Capture expectations before ANY possible close/reopen, including baseline.
    expected_settings = store.con.execute(SETTINGS_SQL).fetchone()
    _seed(store)
    connection = store.con
    baseline_count = engine.refresh_market_daily_metrics(store, engine.MarketDailyOptions(run_id="stable"))
    assert baseline_count == 75
    assert store.con is connection
    assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings
    baseline = _snapshot(store)
    store.con.execute("DELETE FROM market_daily_metrics")

    _configure_analytical_session(store, memory_limit="512MB", threads=1)
    assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings
    counts = _observe_recycling(store, monkeypatch, expected_settings)
    count = engine.refresh_market_daily_metrics(
        store, engine.MarketDailyOptions(batch_size=batch_size, run_id="stable")
    )

    assert count == baseline_count
    assert counts == [min(offset + batch_size, 3) * 25 for offset in range(0, 3, batch_size)]
    assert store.con is not connection
    _assert_snapshot(_snapshot(store), baseline)
    # Non-null trailing/valuation values make the complete-row comparison useful.
    assert store.con.execute("""
        SELECT count(*) FROM market_daily_metrics
        WHERE total_return_1m IS NOT NULL AND dollar_volume_20d IS NOT NULL AND pb IS NOT NULL
    """).fetchone()[0] > 0


def test_scoped_recycle_preserves_dates_sources_foreign_rows_and_empty_batch(store, monkeypatch):
    dates = _seed(store, securities=("S1", "S2", "S3", "FOREIGN"))
    assert engine.refresh_market_daily_metrics(
        store, engine.MarketDailyOptions(bar_source="test", run_id="stable")
    ) == 100
    store.con.execute("""
        INSERT INTO market_daily_metrics
        SELECT * REPLACE ('foreign|' || market_daily_id AS market_daily_id, 'foreign-owner' AS source)
        FROM market_daily_metrics WHERE security_id='S1'
    """)
    baseline = _snapshot(store)
    physical_columns, physical_rows = _snapshot(store, physical=True)
    source_idx = physical_columns.index("source")
    security_idx = physical_columns.index("security_id")
    date_idx = physical_columns.index("trade_date")

    def outside_scope(row):
        return (row[source_idx] != engine.MARKET_DAILY_SOURCE_NAME
                or row[security_idx] not in ("S1", "S2", "S3")
                or not dates[20] <= row[date_idx] <= dates[24])

    untouched = [row for row in physical_rows if outside_scope(row)]
    store.con.execute("""
        INSERT INTO equity_daily_bars
        SELECT * REPLACE ('other-bars' AS source, 999.0 AS close, 999.0 AS adjusted_close,
                          available_at + INTERVAL 1 HOUR AS available_at)
        FROM equity_daily_bars WHERE source='test'
    """)
    store.con.execute("""
        INSERT INTO derived_metric_values
        SELECT * REPLACE ('foreign|' || derived_value_id AS derived_value_id, 'other-derived' AS source,
                          9999.0 AS value, available_at + INTERVAL 1 HOUR AS available_at)
        FROM derived_metric_values
    """)
    store.con.execute("""
        UPDATE market_daily_metrics SET close=-1, run_id='stale'
        WHERE source=? AND security_id IN ('S1', 'S2', 'S3') AND trade_date BETWEEN ? AND ?
    """, [engine.MARKET_DAILY_SOURCE_NAME, dates[20], dates[24]])
    _configure_analytical_session(store, memory_limit="512MB", threads=1)
    expected_settings = store.con.execute(SETTINGS_SQL).fetchone()
    counts = _observe_recycling(store, monkeypatch, expected_settings)
    count = engine.refresh_market_daily_metrics(store, engine.MarketDailyOptions(
        security_ids=("S3", "S1", "Z_EMPTY", "S2"), start_date=dates[20], end_date=dates[24],
        bar_source="test", batch_size=1, run_id="stable",
    ))

    assert count == 15
    assert counts == [125, 125, 125, 125]  # The final successful batch inserts zero.
    _assert_snapshot(_snapshot(store), baseline)
    assert [row for row in _snapshot(store, physical=True)[1] if outside_scope(row)] == untouched


def test_failing_batch_rolls_back_after_an_earlier_committed_reopen(store, monkeypatch):
    _seed(store, days=2)
    assert engine.refresh_market_daily_metrics(store, engine.MarketDailyOptions(run_id="old")) == 6
    columns, before = _snapshot(store, physical=True)
    security_idx = columns.index("security_id")
    close_idx = columns.index("close")
    run_idx = columns.index("run_id")
    store.con.execute("UPDATE equity_daily_bars SET close=close*2, adjusted_close=adjusted_close*2")
    _configure_analytical_session(store, memory_limit="512MB", threads=1)
    expected_settings = store.con.execute(SETTINGS_SQL).fetchone()
    counts = _observe_recycling(store, monkeypatch, expected_settings)
    build = engine.build_market_daily_sql
    calls = 0

    def failing_second_insert(**kwargs):
        nonlocal calls
        calls += 1
        sql = build(**kwargs)
        if calls == 2:
            identity = "sha256(? || '|' || f.security_id || '|' || CAST(f.trade_date AS VARCHAR))"
            assert identity in sql
            # Fail inside INSERT, after DELETE: a NULL id breaks market_daily_id NOT NULL (migration
            # 0329 dropped the sha256 PRIMARY KEY a duplicate id used to break, R-4).
            return sql.replace(identity, "CAST(CASE WHEN ? IS NULL THEN 'unreachable' END AS VARCHAR)", 1)
        return sql

    monkeypatch.setattr(engine, "build_market_daily_sql", failing_second_insert)
    with pytest.raises(duckdb.ConstraintException, match=r"(?i)not null"):
        engine.refresh_market_daily_metrics(store, engine.MarketDailyOptions(batch_size=1, run_id="new"))

    assert calls == 2
    assert counts == [6]
    after = _snapshot(store, physical=True)[1]
    assert [row for row in after if row[security_idx] != "S1"] == [
        row for row in before if row[security_idx] != "S1"
    ]
    first_before = [row for row in before if row[security_idx] == "S1"]
    first_after = [row for row in after if row[security_idx] == "S1"]
    assert [row[close_idx] for row in first_after] == [row[close_idx] * 2 for row in first_before]
    assert [row[run_idx] for row in first_after] == ["new", "new"]
    assert store.con.execute(TEMP_SQL).fetchone() == (False,)
    assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings


@pytest.mark.parametrize("kind", ["TABLE", "VIEW"])
@pytest.mark.parametrize("entrypoint", ["refresh", "dataset"])
def test_caller_temp_is_refused_before_initialization_or_mutation(store, monkeypatch, kind, entrypoint):
    _seed(store, securities=("S1",), days=1)
    assert engine.refresh_market_daily_metrics(store) == 1
    before = _snapshot(store, physical=True)
    runs_before = store.con.execute("SELECT * FROM dataset_runs ORDER BY run_id").fetchall()
    _configure_analytical_session(store, memory_limit="512MB", threads=1)
    store.con.execute(f"CREATE TEMP {kind} caller_state AS SELECT 42 AS value")
    connection = store.con

    def unexpected_action():
        pytest.fail("temp refusal must precede initialization and connection replacement")

    monkeypatch.setattr(store, "initialize", unexpected_action)
    monkeypatch.setattr(store, "close", unexpected_action)
    monkeypatch.setattr(store, "reopen", unexpected_action)
    with pytest.raises(RuntimeError, match="caller-owned temporary"):
        if entrypoint == "dataset":
            engine.MarketDailyDataset().run(store, engine.MarketDailyOptions())
        else:
            engine.refresh_market_daily_metrics(store)
    assert store.con is connection
    assert store.con.execute("SELECT value FROM caller_state").fetchone() == (42,)
    assert _snapshot(store, physical=True) == before
    assert store.con.execute("SELECT * FROM dataset_runs ORDER BY run_id").fetchall() == runs_before


@pytest.mark.parametrize("recorded", [(None, None), ("512MB", None), (None, 1)],
                         ids=["neither", "memory-only", "threads-only"])
def test_unconfigured_file_keeps_connection_settings_and_temp_state(store, monkeypatch, recorded):
    store.analytical_memory_limit, store.analytical_threads = recorded
    store.con.execute("SET preserve_insertion_order=true")
    store.con.execute("CREATE TEMP TABLE caller_state AS SELECT 42 AS value")
    store.con.execute("CREATE TEMP VIEW caller_view AS SELECT value FROM caller_state")
    expected_settings = store.con.execute(SETTINGS_SQL).fetchone()
    connection = store.con
    _seed(store, days=1)

    def unexpected_recycle():
        pytest.fail("unconfigured callers must retain their connection")

    monkeypatch.setattr(store, "close", unexpected_recycle)
    monkeypatch.setattr(store, "reopen", unexpected_recycle)
    assert engine.refresh_market_daily_metrics(store, engine.MarketDailyOptions(batch_size=1)) == 3
    assert store.con is connection
    assert store.con.execute(SETTINGS_SQL).fetchone() == expected_settings
    assert store.con.execute("SELECT value FROM caller_view").fetchone() == (42,)


def test_configured_memory_keeps_database_settings_and_temp_state(store, monkeypatch):
    # Copy only the existing DDL needed by the real daily SQL, avoiding another
    # full migration/bootstrap for a three-row in-memory compatibility fixture.
    definitions = store.con.execute("""
        SELECT sql FROM duckdb_tables() WHERE table_name IN (
            'equity_daily_bars', 'fundamental_standardized', 'derived_metric_values',
            'shares_outstanding_history', 'market_daily_metrics'
        ) ORDER BY table_name
    """).fetchall()
    assert len(definitions) == 5
    memory = DuckDBStore(":memory:")
    memory.connection = duckdb.connect(":memory:", config={"memory_limit": "512MB", "threads": "1"})
    try:
        for (sql,) in definitions:
            memory.con.execute(sql)
        memory._initialized = True
        _configure_analytical_session(memory, memory_limit="512MB", threads=1)
        memory.con.execute("CREATE TEMP TABLE caller_state AS SELECT 42 AS value")
        memory.con.execute("CREATE TEMP VIEW caller_view AS SELECT value FROM caller_state")
        expected_settings = memory.con.execute(SETTINGS_SQL).fetchone()
        connection = memory.con
        _seed(memory, days=1)

        def unexpected_recycle():
            pytest.fail("in-memory callers must retain their database")

        monkeypatch.setattr(memory, "close", unexpected_recycle)
        monkeypatch.setattr(memory, "reopen", unexpected_recycle)
        assert engine.refresh_market_daily_metrics(memory, engine.MarketDailyOptions(batch_size=1)) == 3
        assert memory.con is connection
        assert memory.con.execute(SETTINGS_SQL).fetchone() == expected_settings
        assert memory.con.execute("SELECT count(*) FROM market_daily_metrics").fetchone() == (3,)
        assert memory.con.execute("SELECT value FROM caller_view").fetchone() == (42,)
    finally:
        memory.__exit__(None, None, None)
