from __future__ import annotations

import datetime as dt
from contextlib import contextmanager

import duckdb
import pytest

from atx_db import _forward_return_publication as publication
from atx_db._bulk_publication import _contract
from atx_db.connection import DuckDBStore
from atx_db.delisting import (
    FORWARD_RETURN_SS_COLUMNS,
    SurvivorshipSafeForwardReturnOptions,
    refresh_survivorship_safe_forward_returns,
)
from atx_db.migrations.bodies_0317 import _bounded_forward_return_publication
from atx_db.signal_eval import IC_HORIZONS


@pytest.fixture
def publication_store(tmp_path, request):
    """A real file and the production forward table contract, without full bootstrap."""
    path = ":memory:" if getattr(request, "param", "file") == "memory" else tmp_path / "forward.duckdb"
    store = DuckDBStore(path)
    store.connection = duckdb.connect(str(store.path), config={"threads": 1, "memory_limit": "128MB"})
    store._configure_session(store.con)
    store.analytical_memory_limit = "128MB"
    store.analytical_threads = 1
    store._initialized = True
    store.con.execute("""
        CREATE TABLE equity_daily_bars (
            source VARCHAR, security_id VARCHAR, symbol VARCHAR, trade_date DATE,
            close DOUBLE, adjusted_close DOUBLE, available_at TIMESTAMP,
            vendor_security_id VARCHAR, source_loaded_at TIMESTAMP
        );
        CREATE TABLE trading_calendar (
            calendar_id VARCHAR, trade_date DATE, is_open BOOLEAN, source VARCHAR
        );
        CREATE TABLE delisting_terminal_returns (
            terminal_return_id VARCHAR, security_id VARCHAR, delist_date DATE,
            terminal_return DOUBLE, terminal_return_source VARCHAR,
            return_observation_id VARCHAR, available_at TIMESTAMP, source_loaded_at TIMESTAMP
        );
        CREATE TABLE forward_returns_survivorship_safe (
            forward_return_id VARCHAR PRIMARY KEY,
            source VARCHAR NOT NULL, security_id VARCHAR NOT NULL, symbol VARCHAR,
            as_of_date DATE NOT NULL, horizon_days INTEGER NOT NULL, forward_end_date DATE,
            raw_forward_return DOUBLE, terminal_return DOUBLE, forward_return DOUBLE NOT NULL,
            is_delisted_in_horizon BOOLEAN NOT NULL, is_stitched BOOLEAN NOT NULL,
            delist_date DATE, terminal_return_source VARCHAR, return_observation_id VARCHAR,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true, available_at TIMESTAMP NOT NULL,
            run_id VARCHAR, price_basis VARCHAR, calculation_version VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            updated_at TIMESTAMP NOT NULL DEFAULT now()
        );
        CREATE VIEW v_forward_returns_survivorship_safe AS
            SELECT * FROM forward_returns_survivorship_safe WHERE is_latest_revision;
        CREATE VIEW consumer_forward_view AS
            SELECT * FROM v_forward_returns_survivorship_safe WHERE horizon_days = 1;
        CREATE TABLE _ss_forward_unrelated_stage (value VARCHAR);
        INSERT INTO _ss_forward_unrelated_stage VALUES ('caller-owned');
    """)
    days = [dt.date(2024, 1, 1) + dt.timedelta(days=offset) for offset in range(70)]
    store.con.executemany(
        "INSERT INTO trading_calendar VALUES ('XNYS', ?, true, 'equity_daily_bars calendar')",
        [(day,) for day in days],
    )
    store.con.executemany(
        "INSERT INTO equity_daily_bars VALUES ('prices', ?, ?, ?, ?, ?, ?, 'vendor', '2024-01-01')",
        [(security, security, day, 100.0 + offset, 200.0 + offset,
          dt.datetime.combine(day, dt.time(22)))
         for security, count in (("D", 4), ("S", 70))
         for offset, day in enumerate(days[:count])],
    )
    store.con.execute("""
        INSERT INTO delisting_terminal_returns VALUES
            ('terminal', 'D', '2024-01-05', -0.5, 'observed', 'obs',
             '2024-01-09', '2024-01-09');
    """)
    for identifier, source in (("old-source", "target"), ("", "other"), ("zz-foreign", "other")):
        store.con.execute("""
            INSERT INTO forward_returns_survivorship_safe
                (forward_return_id, source, security_id, as_of_date, horizon_days,
                 forward_return, is_delisted_in_horizon, is_stitched, available_at,
                 source_loaded_at, updated_at)
            VALUES (?, ?, 'OLD', '2020-01-01', 1, 0.1, false, false,
                    '2020-01-02', '2020-01-03', '2020-01-04')
        """, [identifier, source])
    try:
        yield store
    finally:
        if store.connection is not None:
            store.connection.close()


def _rows(store, relation="forward_returns_survivorship_safe"):
    return store.con.execute(f"SELECT * FROM {relation} ORDER BY forward_return_id").fetchall()


def _assert_cleanup(store):
    assert store.con.execute("""
        SELECT table_name FROM duckdb_tables()
        WHERE starts_with(table_name, '_ss_forward_') ORDER BY table_name
    """).fetchall() == [("_ss_forward_unrelated_stage",)]
    assert store.con.execute("SELECT * FROM _ss_forward_unrelated_stage").fetchall() == [("caller-owned",)]


def _force_boundaries(monkeypatch):
    monkeypatch.setattr(publication, "_FORMATION_BATCH_ROWS", 3)
    monkeypatch.setattr(publication, "_SHADOW_BATCH_ROWS", 2)
    monkeypatch.setattr(publication, "_CHECKPOINT_ROWS", 6)


@pytest.mark.parametrize("price_basis", ["adjusted_close", "close"])
def test_file_backed_prefix_reopen_matches_full_results_and_retains_foreign_source(
    publication_store, monkeypatch, price_basis,
):
    store = publication_store
    options = SurvivorshipSafeForwardReturnOptions(source="target", price_basis=price_basis, run_id="result")
    contract = _contract(store, "forward_returns_survivorship_safe")
    foreign = store.con.execute("SELECT * FROM forward_returns_survivorship_safe WHERE source='other' ORDER BY 1").fetchall()
    count = refresh_survivorship_safe_forward_returns(store, options)
    select = f"SELECT {', '.join(FORWARD_RETURN_SS_COLUMNS)} FROM forward_returns_survivorship_safe ORDER BY 1"
    expected = store.con.execute(select).fetchall()
    assert store.con.execute("SELECT DISTINCT horizon_days FROM forward_returns_survivorship_safe WHERE source='target' ORDER BY 1").fetchall() == [(h,) for h in IC_HORIZONS]
    prefix_groups = store.con.execute("SELECT count(*), max(n) FROM (SELECT left(forward_return_id, 2), count(*) AS n FROM forward_returns_survivorship_safe WHERE source='target' GROUP BY 1)").fetchone()
    assert prefix_groups[0] > 1 and prefix_groups[1] > 2
    expected_settings = store.con.execute("SELECT current_setting('memory_limit'), current_setting('threads'), current_setting('preserve_insertion_order')").fetchone()
    _force_boundaries(monkeypatch)
    original_reopen = store.reopen
    observed_reopens = []

    def reopen():
        original_reopen()
        observed_reopens.append(store.con.execute("SELECT current_setting('memory_limit'), current_setting('threads')").fetchone())
        # The public data and both pre-existing views stay readable during staging.
        assert store.con.execute(select).fetchall() == expected
        assert _rows(store, "v_forward_returns_survivorship_safe") == _rows(store)
        assert _rows(store, "consumer_forward_view")

    monkeypatch.setattr(store, "reopen", reopen)
    assert refresh_survivorship_safe_forward_returns(store, options) == count
    assert len(observed_reopens) > 2
    assert all(settings == expected_settings[:2] for settings in observed_reopens)
    assert store.con.execute("SELECT current_setting('memory_limit'), current_setting('threads'), current_setting('preserve_insertion_order')").fetchone() == expected_settings
    assert store.con.execute(select).fetchall() == expected
    assert store.con.execute("SELECT * FROM forward_returns_survivorship_safe WHERE source='other' ORDER BY 1").fetchall() == foreign
    assert _contract(store, "forward_returns_survivorship_safe") == contract
    assert store.con.execute("SELECT count(DISTINCT source_loaded_at), bool_and(source_loaded_at=updated_at) FROM forward_returns_survivorship_safe WHERE source='target'").fetchone() == (1, True)
    assert store.con.execute("""
        SELECT DISTINCT price_basis,calculation_version
        FROM forward_returns_survivorship_safe WHERE source='target'
    """).fetchall() == [(price_basis, publication.CALCULATION_VERSION)]
    assert store.con.execute("""
        SELECT count(*) FROM forward_returns_survivorship_safe
        WHERE source='other' AND price_basis IS NULL AND calculation_version IS NULL
    """).fetchone()[0] == 2
    _assert_cleanup(store)


@pytest.mark.parametrize("failure", ["shadow", "swap"])
def test_failure_preserves_previous_panel_views_and_cleans_only_owned_artifacts(
    publication_store, monkeypatch, failure,
):
    store = publication_store
    _force_boundaries(monkeypatch)
    previous = _rows(store)
    previous_view = _rows(store, "consumer_forward_view")
    if failure == "shadow":
        checkpoint = publication._Build.checkpoint

        def fail_during_shadow(self, rows):
            checkpoint(self, rows)
            if self.name("shadow") in self.owned and rows:
                raise RuntimeError("injected shadow failure")

        monkeypatch.setattr(publication._Build, "checkpoint", fail_during_shadow)
    else:
        transaction = store.transaction

        @contextmanager
        def fail_at_swap():
            with transaction() as con:
                yield con
                # Both renames and old-table DROP have executed but are uncommitted.
                raise RuntimeError("injected swap failure")

        monkeypatch.setattr(store, "transaction", fail_at_swap)
    with pytest.raises(RuntimeError, match=f"injected {failure} failure"):
        refresh_survivorship_safe_forward_returns(store, SurvivorshipSafeForwardReturnOptions(source="target"))
    assert _rows(store) == previous
    assert _rows(store, "v_forward_returns_survivorship_safe") == previous
    assert _rows(store, "consumer_forward_view") == previous_view
    _assert_cleanup(store)
    # A retry after the transient failure owns a new namespace and succeeds.
    monkeypatch.undo()
    assert refresh_survivorship_safe_forward_returns(store, SurvivorshipSafeForwardReturnOptions(source="target")) > 0
    _assert_cleanup(store)


def test_empty_refresh_keeps_foreign_rows_and_views(publication_store):
    store = publication_store
    foreign = store.con.execute("SELECT * FROM forward_returns_survivorship_safe WHERE source='other' ORDER BY 1").fetchall()
    store.con.execute("DELETE FROM equity_daily_bars")
    assert refresh_survivorship_safe_forward_returns(store, SurvivorshipSafeForwardReturnOptions(source="target")) == 0
    assert _rows(store) == foreign
    assert _rows(store, "v_forward_returns_survivorship_safe") == foreign
    _assert_cleanup(store)


@pytest.mark.parametrize("publication_store", ["memory"], indirect=True)
def test_in_memory_refresh_never_reopens_or_drops_caller_temporary_table(publication_store, monkeypatch):
    store = publication_store
    _force_boundaries(monkeypatch)
    store.con.execute("CREATE TEMP TABLE _ss_bars AS SELECT 'caller-owned' AS value")

    def no_reopen():
        pytest.fail("in-memory publication must not close or reopen its connection")

    monkeypatch.setattr(store, "close", no_reopen)
    monkeypatch.setattr(store, "reopen", no_reopen)
    assert refresh_survivorship_safe_forward_returns(store, SurvivorshipSafeForwardReturnOptions(source="target")) > 0
    assert store.con.execute("SELECT * FROM _ss_bars").fetchall() == [("caller-owned",)]
    _assert_cleanup(store)


@pytest.mark.parametrize("object_kind", ["table", "view", "macro"])
def test_file_backed_caller_temporary_objects_are_preserved(publication_store, object_kind):
    store = publication_store
    if object_kind == "macro":
        store.con.execute("CREATE TEMP MACRO caller_object() AS 42")
        read = "SELECT caller_object()"
    else:
        store.con.execute(f"CREATE TEMP {object_kind.upper()} caller_object AS SELECT 42 AS value")
        read = "SELECT * FROM caller_object"
    previous = _rows(store)
    with pytest.raises(RuntimeError, match="caller-owned temporary objects"):
        refresh_survivorship_safe_forward_returns(store)
    assert store.con.execute(read).fetchall() == [(42,)]
    assert _rows(store) == previous
    _assert_cleanup(store)


def test_unconfigured_file_caller_keeps_connection_and_manual_session_state(publication_store, monkeypatch):
    store = publication_store
    store.analytical_memory_limit = None
    store.analytical_threads = None
    original = store.con
    store.con.execute("SET max_expression_depth=777")
    store.con.execute("CREATE TEMP TABLE _ss_bars AS SELECT 'caller-owned' AS value")
    _force_boundaries(monkeypatch)

    def no_reopen():
        pytest.fail("unconfigured callers must retain their existing connection")

    monkeypatch.setattr(store, "close", no_reopen)
    monkeypatch.setattr(store, "reopen", no_reopen)
    assert refresh_survivorship_safe_forward_returns(store, SurvivorshipSafeForwardReturnOptions(source="target")) > 0
    assert store.con is original
    assert store.con.execute("SELECT current_setting('max_expression_depth')").fetchone() == (777,)
    assert store.con.execute("SELECT * FROM _ss_bars").fetchall() == [("caller-owned",)]
    assert store.analytical_memory_limit is None and store.analytical_threads is None
    _assert_cleanup(store)


def test_migration_0317_removes_only_optional_indexes_on_populated_table(publication_store):
    store = publication_store
    before = _rows(store)
    contract = _contract(store, "forward_returns_survivorship_safe")
    store.con.execute("""
        CREATE INDEX idx_forward_returns_ss_key ON forward_returns_survivorship_safe
            (source, security_id, as_of_date, horizon_days);
        CREATE INDEX idx_forward_returns_ss_delisted ON forward_returns_survivorship_safe
            (is_delisted_in_horizon, as_of_date);
        CREATE INDEX caller_index ON _ss_forward_unrelated_stage(value);
    """)
    _bounded_forward_return_publication(store.con)
    _bounded_forward_return_publication(store.con)
    assert store.con.execute("SELECT index_name FROM duckdb_indexes() ORDER BY index_name").fetchall() == [("caller_index",)]
    assert _rows(store) == before
    assert _rows(store, "v_forward_returns_survivorship_safe") == before
    assert _contract(store, "forward_returns_survivorship_safe") == contract
    with pytest.raises(duckdb.ConstraintException):
        store.con.execute("INSERT INTO forward_returns_survivorship_safe SELECT * FROM forward_returns_survivorship_safe LIMIT 1")
    with pytest.raises(duckdb.ConstraintException):
        store.con.execute("UPDATE forward_returns_survivorship_safe SET source=NULL")
    assert refresh_survivorship_safe_forward_returns(store, SurvivorshipSafeForwardReturnOptions(source="target")) > 0
    assert _contract(store, "forward_returns_survivorship_safe") == contract
