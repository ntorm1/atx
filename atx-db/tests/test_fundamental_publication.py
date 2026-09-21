from __future__ import annotations

import datetime as dt
from contextlib import contextmanager

import duckdb
import pandas as pd
import pytest

from atx_db import _fundamental_publication as publication
from atx_db._bulk_publication import _contract, publish_validated_shadows
from atx_db.calendarization import (
    CalendarizationOptions,
    refresh_fundamental_calendar_map,
    refresh_fundamental_calendar_ttm,
)
from atx_db.connection import DuckDBStore, open_duckdb_connection
from atx_db.fundamental_statements import (
    refresh_fundamental_periods,
    refresh_fundamental_statement_points,
    refresh_fundamental_ttm_points,
)
from atx_db.fundamentals import refresh_fundamental_fact_revisions
from atx_db.migrations.bodies_0318 import _OPTIONAL_INDEXES, _bounded_fundamentals_publication
from atx_db.standardization import FundamentalStandardizationOptions, refresh_fundamental_standardized


def _configure(store):
    store.analytical_memory_limit = "256MB"
    store.analytical_threads = 1
    store.con.execute("SET memory_limit = '256MB'")
    store.con.execute("SET threads = 1")
    store.con.execute("SET preserve_insertion_order = false")


@pytest.fixture
def publication_store(tmp_path, monkeypatch):
    store = DuckDBStore(tmp_path / "publication.duckdb")
    store.connection = open_duckdb_connection(store.path)
    _configure(store)
    monkeypatch.setattr(publication, "_PREFIX_BOUNDARIES", ("40", "80"))
    try:
        yield store
    finally:
        store.close()


def _small_table(store, table="fundamental_fact_revisions"):
    key = publication._KEYS[table]
    store.con.execute(f"""
        CREATE TABLE {table} (
            {key} VARCHAR PRIMARY KEY, source VARCHAR, symbol VARCHAR,
            value DOUBLE NOT NULL CHECK (value >= 0),
            token VARCHAR NOT NULL UNIQUE, updated_at TIMESTAMP NOT NULL DEFAULT now()
        )
    """)
    store.con.execute(f"COMMENT ON TABLE {table} IS 'retained public description'")
    store.con.execute(f"""
        INSERT INTO {table} VALUES
          ('00-old', 'target', 'A', 1, 'old', TIMESTAMP '2020-01-01'),
          ('40-other', 'other', 'A', 2, 'other', TIMESTAMP '2020-01-01'),
          ('80-scope', 'target', 'B', 3, 'scope', TIMESTAMP '2020-01-01'),
          ('ff-null', NULL, NULL, 4, 'null', TIMESTAMP '2020-01-01')
    """)
    return key


def _rows(store, table):
    return store.con.execute(f"SELECT * FROM {table} ORDER BY 1").fetchall()


def _no_artifacts(store):
    assert store.con.execute("""
        SELECT table_name FROM duckdb_tables()
        WHERE table_name LIKE 'fundamental_%_bulk_%'
    """).fetchall() == []


@pytest.mark.parametrize("table", tuple(publication._KEYS))
def test_bounded_shadow_preserves_scope_keys_defaults_and_views(publication_store, monkeypatch, table):
    store = publication_store
    key = _small_table(store, table)
    original = _rows(store, table)
    contract = _contract(store, table)
    settings = store.con.execute("SELECT current_setting('memory_limit'), current_setting('threads')").fetchone()
    store.con.execute(f"CREATE VIEW publication_reader AS SELECT * FROM {table}")
    monkeypatch.setattr(publication, "_BATCH_ROWS", 2)
    monkeypatch.setattr(publication, "_REOPEN_BATCHES", 1)
    real_reopen = store.reopen
    reopens = []

    def reopen():
        real_reopen()
        assert store.con.execute("SELECT current_setting('memory_limit'), current_setting('threads')").fetchone() == settings
        assert store.con.execute("SELECT current_setting('preserve_insertion_order')").fetchone() == (False,)
        # The old full output and reader stay available until every batch finishes.
        assert _rows(store, table) == original
        assert _rows(store, "publication_reader") == original
        reopens.append(1)

    monkeypatch.setattr(store, "reopen", reopen)
    with publication.fundamental_publication(
        store, (table,), replace_where="source = ? AND symbol = ?", replace_params=("target", "A")
    ):
        store.con.execute(f"""
            INSERT INTO {table}_bulk_stage ({key}, source, symbol, value, token)
            SELECT printf('%02d-new', i), 'target', 'A', i, 'new-' || i FROM range(5) t(i)
        """)
    assert len(reopens) >= 5
    assert _contract(store, table) == contract
    rows = _rows(store, table)
    assert len(rows) == 8
    assert [row for row in rows if row[0] in {"40-other", "80-scope", "ff-null"}] == original[1:]
    assert all(row[-1] is not None for row in rows)
    assert _rows(store, "publication_reader") == rows
    assert store.con.execute("SELECT comment FROM duckdb_tables() WHERE table_name = ?", [table]).fetchone() == (
        "retained public description",
    )
    with pytest.raises(duckdb.ConstraintException):
        store.con.execute(f"INSERT INTO {table} SELECT * FROM {table} LIMIT 1")
    with pytest.raises(duckdb.ConstraintException):
        store.con.execute(f"INSERT INTO {table} ({key}, value, token) VALUES ('unique-violation', 1, 'other')")
    _no_artifacts(store)


@pytest.mark.parametrize("failure", ["calculation", "duplicate", "check", "reopen", "publication"])
def test_failure_preserves_old_output_and_allows_retry(publication_store, monkeypatch, failure):
    store = publication_store
    table = "fundamental_fact_revisions"
    key = _small_table(store, table)
    original = _rows(store, table)
    store.con.execute(f"CREATE VIEW failure_reader AS SELECT * FROM {table}")
    with monkeypatch.context() as patch:
        patch.setattr(publication, "_BATCH_ROWS", 1)
        patch.setattr(publication, "_REOPEN_BATCHES", 1)
        if failure == "reopen":
            real_reopen = store.reopen

            def fail_reopen():
                real_reopen()
                raise RuntimeError("injected reopen failure")

            patch.setattr(store, "reopen", fail_reopen)

        def fail_publication():
            raise RuntimeError("injected publication failure")

        with (
            pytest.raises((RuntimeError, duckdb.ConstraintException)),
            publication.fundamental_publication(
                store, (table,), before_swap=fail_publication if failure == "publication" else None
            ),
        ):
            store.con.execute(f"""
                INSERT INTO {table}_bulk_stage ({key}, source, symbol, value, token)
                VALUES ('01', 'target', 'A', ?, 'one'), ('02', 'target', 'A', 2, 'two')
            """, [-1 if failure == "check" else 1])
            if failure == "duplicate":
                store.con.execute(f"INSERT INTO {table}_bulk_stage SELECT * FROM {table}_bulk_stage LIMIT 1")
            if failure == "calculation":
                raise RuntimeError("injected calculation failure")
    assert _rows(store, table) == original
    assert _rows(store, "failure_reader") == original
    _no_artifacts(store)
    with publication.fundamental_publication(store, (table,)):
        pass
    assert _rows(store, table) == []


def test_reserved_name_collision_refuses_before_owned_cleanup(publication_store):
    store = publication_store
    table = "fundamental_fact_revisions"
    _small_table(store, table)
    store.con.execute(f"CREATE TABLE {table}_bulk_stage AS SELECT 'caller' AS value")
    store.con.execute(f"CREATE TABLE {table}_bulk_next AS SELECT 'owned' AS value")
    publication._mark_owned(store, f"{table}_bulk_next")
    with (
        pytest.raises(RuntimeError, match="not owned"),
        publication.fundamental_publication(store, (table,)),
    ):
        pytest.fail("collision must refuse before yielding")
    assert _rows(store, f"{table}_bulk_stage") == [("caller",)]
    assert _rows(store, f"{table}_bulk_next") == [("owned",)]


def test_configured_caller_temporary_state_is_retained_on_refusal(publication_store):
    store = publication_store
    _small_table(store)
    store.con.execute("CREATE TEMP TABLE caller_temp AS SELECT 7 AS value")
    store.con.register("caller_frame", pd.DataFrame({"value": [8]}))
    with (
        pytest.raises(RuntimeError, match="caller temporary relations"),
        publication.fundamental_publication(store, ("fundamental_fact_revisions",)),
    ):
        pytest.fail("caller state must refuse before yielding")
    assert _rows(store, "caller_temp") == [(7,)]
    assert _rows(store, "caller_frame") == [(8,)]


@pytest.mark.parametrize("memory", [False, True])
def test_unconfigured_and_memory_callers_keep_connection_and_temp_state(tmp_path, monkeypatch, memory):
    store = DuckDBStore(":memory:" if memory else tmp_path / "unconfigured.duckdb")
    store.connection = open_duckdb_connection(store.path)
    try:
        if memory:
            _configure(store)
        connection = store.con
        _small_table(store)
        store.con.execute("SET default_null_order = 'NULLS_FIRST'")
        store.con.execute("CREATE TEMP TABLE caller_temp AS SELECT 7 AS value")
        monkeypatch.setattr(publication, "_BATCH_ROWS", 1)
        monkeypatch.setattr(publication, "_PREFIX_BOUNDARIES", ("40", "80"))
        with publication.fundamental_publication(store, ("fundamental_fact_revisions",)):
            pass
        assert store.con is connection
        assert _rows(store, "caller_temp") == [(7,)]
        assert store.con.execute("SELECT current_setting('default_null_order')").fetchone() == ("NULLS_FIRST",)
    finally:
        store.close()


@pytest.mark.parametrize("failure", ["second_swap", "commit"])
def test_coupled_swap_rolls_back_both_tables_views_and_completion(publication_store, monkeypatch, failure):
    store = publication_store
    for table in ("one", "two"):
        store.con.execute(f"CREATE TABLE {table} (id VARCHAR PRIMARY KEY)")
        store.con.execute(f"CREATE TABLE {table}_next (id VARCHAR PRIMARY KEY)")
        store.con.execute(f"INSERT INTO {table} VALUES ('old')")
        store.con.execute(f"INSERT INTO {table}_next VALUES ('new')")
        store.con.execute(f"CREATE VIEW {table}_reader AS SELECT * FROM {table}")
    store.con.execute("CREATE TABLE build_ledger (status VARCHAR)")
    store.con.execute("INSERT INTO build_ledger VALUES ('running')")
    if failure == "second_swap":
        # DuckDB permits the first rename, then rejects the indexed second shadow.
        store.con.execute("CREATE INDEX refuse_rename ON two_next(id)")
    else:
        @contextmanager
        def fail_commit():
            store.con.execute("BEGIN TRANSACTION")
            yield store.con
            raise RuntimeError("injected COMMIT failure")

        monkeypatch.setattr(store, "transaction", fail_commit)
    with pytest.raises((duckdb.Error, RuntimeError)):
        publish_validated_shadows(
            store,
            tables=(("one", "one_next"), ("two", "two_next")),
            before_swap=lambda: store.con.execute("UPDATE build_ledger SET status='completed'"),
        )
    for table in ("one", "two"):
        assert _rows(store, table) == [("old",)]
        assert _rows(store, f"{table}_reader") == [("old",)]
        assert _rows(store, f"{table}_next") == [("new",)]
    assert _rows(store, "build_ledger") == [("running",)]


def test_0318_drops_only_optional_indexes_and_bootstrap_keeps_keys(tmp_store):
    from atx_db.schema import ensure_quant_schema

    before = {table: _contract(tmp_store, table) for table in publication._KEYS}
    for table, indexes in _OPTIONAL_INDEXES.items():
        for index in indexes:
            tmp_store.con.execute(f"CREATE INDEX {index} ON {table}({publication._KEYS[table]})")
    tmp_store.con.execute("CREATE TABLE unrelated_index_owner (id INTEGER PRIMARY KEY, value INTEGER)")
    tmp_store.con.execute("CREATE INDEX unrelated_index ON unrelated_index_owner(value)")
    _bounded_fundamentals_publication(tmp_store.con)
    ensure_quant_schema(tmp_store)
    for table in publication._KEYS:
        assert _contract(tmp_store, table) == before[table]
        assert tmp_store.con.execute("SELECT index_name FROM duckdb_indexes() WHERE table_name = ?", [table]).fetchall() == []
    assert tmp_store.con.execute("SELECT count(*) FROM duckdb_indexes() WHERE index_name='unrelated_index'").fetchone() == (1,)
    assert tmp_store.con.execute("SELECT count(*) FROM schema_migrations WHERE CAST(version AS INTEGER)=318").fetchone() == (1,)


def _seed_pipeline(store):
    store.con.execute("""
        INSERT INTO securities (security_id, primary_symbol, name, source)
        VALUES ('publication-issuer', 'PUB', 'Publication fixture', 'fixture')
    """)
    for quarter, start, end in (
        (1, "2024-01-01", "2024-03-31"), (2, "2024-04-01", "2024-06-30"),
        (3, "2024-07-01", "2024-09-30"), (4, "2024-10-01", "2024-12-31"),
    ):
        filed = dt.date.fromisoformat(end) + dt.timedelta(days=30)
        store.con.execute("""
            INSERT INTO sec_company_facts (
                source,security_id,cik,taxonomy,concept,unit,period_start,period_end,
                filed_date,fiscal_year,fiscal_period,form,accession_number,value,
                available_at,source_url,source_loaded_at
            ) VALUES (
                'SEC companyfacts','publication-issuer','0000000001','us-gaap','Revenues','USD',
                ?,?,?,2024,?,'10-Q',?,?,?,'fixture',TIMESTAMP '2025-02-01'
            )
        """, [start, end, filed, f"Q{quarter}", f"accession-{quarter}", quarter * 100,
              dt.datetime.combine(filed, dt.time(22))])
    store.con.execute("""
        INSERT INTO fundamental_xbrl_metric (
            metric_id,source,security_id,symbol,cik,canonical_metric,concept,taxonomy,unit,
            period_type,period_start,period_end,accession_number,value,is_latest_revision,
            as_of_date,available_at,source_loaded_at
        ) VALUES (
            'unknown','fixture','publication-issuer','PUB','0000000001','unknown',
            'UnknownTag','fixture','USD','duration',DATE '2024-01-01',DATE '2024-12-31',
            'unknown-accession',7,true,DATE '2025-01-30',TIMESTAMP '2025-01-30 22:00:00',
            TIMESTAMP '2025-02-01'
        )
    """)


def _refresh_pipeline(store):
    assert refresh_fundamental_fact_revisions(store) == 4
    assert refresh_fundamental_statement_points(store) == 4
    assert refresh_fundamental_periods(store) == 4
    assert refresh_fundamental_ttm_points(store) > 0
    assert refresh_fundamental_calendar_map(store, CalendarizationOptions(run_id="calendar")) == 4
    assert refresh_fundamental_calendar_ttm(store, CalendarizationOptions(run_id="calendar")) > 0
    return refresh_fundamental_standardized(
        store, FundamentalStandardizationOptions(run_id="standard", materialize_result_limit=0)
    )


def test_all_eight_sql_writers_cross_file_reopens_with_unchanged_payload(tmp_store, monkeypatch):
    _configure(tmp_store)
    _seed_pipeline(tmp_store)
    monkeypatch.setattr(publication, "_BATCH_ROWS", 2)
    monkeypatch.setattr(publication, "_REOPEN_BATCHES", 1)
    monkeypatch.setattr(publication, "_PREFIX_BOUNDARIES", ("40", "80"))
    original_reopen = tmp_store.reopen
    reopens = []

    def reopen():
        original_reopen()
        reopens.append(1)

    monkeypatch.setattr(tmp_store, "reopen", reopen)
    result = _refresh_pipeline(tmp_store)
    assert result.standardized.empty and result.exceptions.empty
    assert result.standardized_row_count > 0
    assert result.exception_row_count == 1
    assert tmp_store.con.execute("SELECT ttm_value FROM fundamental_ttm_points WHERE is_latest_revision").fetchall() == [(1000.0,)]
    # Calendar TTM retains incomplete windows; latest revision is per calendar
    # period, while only the fourth quarter has a complete four-quarter window.
    assert tmp_store.con.execute("""
        SELECT calendar_period, quarter_count, coverage_days, is_complete, ttm_value
        FROM fundamental_calendar_ttm WHERE is_latest_revision
        ORDER BY calendar_period
    """).fetchall() == [
        ("2024Q1", 1, 91, False, 100.0),
        ("2024Q2", 2, 182, False, 300.0),
        ("2024Q3", 3, 274, False, 600.0),
        ("2024Q4", 4, 366, True, 1000.0),
    ]
    # Exclude only refresh-generated storage clocks; input clocks remain part of
    # the parity comparison on the six upstream outputs.
    snapshots = {}
    for table in publication._KEYS:
        exclusions = "source_loaded_at, updated_at" if table.startswith("fundamental_standard") else "updated_at"
        snapshots[table] = tmp_store.con.execute(f"SELECT * EXCLUDE ({exclusions}) FROM {table} ORDER BY 1").fetchall()
        assert snapshots[table]
    result = _refresh_pipeline(tmp_store)
    for table, expected in snapshots.items():
        exclusions = "source_loaded_at, updated_at" if table.startswith("fundamental_standard") else "updated_at"
        assert tmp_store.con.execute(f"SELECT * EXCLUDE ({exclusions}) FROM {table} ORDER BY 1").fetchall() == expected
    assert len(reopens) >= 32
    assert tmp_store.con.execute("SELECT status FROM fundamental_standardization_builds WHERE build_id=?", [result.build_id]).fetchone() == ("completed",)
    _no_artifacts(tmp_store)


@pytest.mark.parametrize("failure", ["shadow", "publication"])
def test_standardized_coupled_failure_preserves_payload_and_failed_build(tmp_store, monkeypatch, failure):
    _configure(tmp_store)
    _seed_pipeline(tmp_store)
    _refresh_pipeline(tmp_store)
    tables = ("fundamental_standardized", "fundamental_standardization_exception")
    before = {table: _rows(tmp_store, table) for table in tables}
    for table in tables:
        tmp_store.con.execute(f"CREATE VIEW {table}_reader AS SELECT * FROM {table}")
    original_publish = publication.publish_validated_shadows

    def fail_after_completion(store, *, tables, before_swap):
        def fail():
            before_swap()
            assert store.con.execute("""
                SELECT status FROM fundamental_standardization_builds
                WHERE run_id='failing-build'
            """).fetchone() == ("completed",)
            raise RuntimeError("injected after completed ledger")

        original_publish(store, tables=tables, before_swap=fail)

    if failure == "publication":
        monkeypatch.setattr(publication, "publish_validated_shadows", fail_after_completion)
    else:
        original_build = publication._build_shadow

        def fail_second_shadow(store, table):
            if table == "fundamental_standardization_exception":
                raise RuntimeError("injected second shadow failure")
            original_build(store, table)

        monkeypatch.setattr(publication, "_build_shadow", fail_second_shadow)
    tmp_store.con.execute("UPDATE fundamental_statement_points SET value=value+1")
    with pytest.raises(RuntimeError, match="injected"):
        refresh_fundamental_standardized(
            tmp_store, FundamentalStandardizationOptions(run_id="failing-build", symbols=("PUB",))
        )
    for table in tables:
        assert _rows(tmp_store, table) == before[table]
        assert _rows(tmp_store, f"{table}_reader") == before[table]
    assert tmp_store.con.execute("SELECT status FROM fundamental_standardization_builds WHERE run_id='failing-build'").fetchone() == ("failed",)
    _no_artifacts(tmp_store)


def test_standardized_scoped_refresh_preserves_other_sources_and_symbols(tmp_store):
    _configure(tmp_store)
    _seed_pipeline(tmp_store)
    _refresh_pipeline(tmp_store)
    tmp_store.con.execute("CREATE TABLE _std_output AS SELECT 'caller-persistent' AS value")
    before = {}
    for table in ("fundamental_standardized", "fundamental_standardization_exception"):
        key = publication._KEYS[table]
        tmp_store.con.execute(f"""
            INSERT INTO {table} SELECT * REPLACE (
                'keep-symbol-' || {key} AS {key}, 'OTHER' AS symbol
            ) FROM {table}
        """)
        tmp_store.con.execute(f"""
            INSERT INTO {table} SELECT * REPLACE (
                'keep-source-' || {key} AS {key}, 'other-source' AS source
            ) FROM {table} WHERE symbol='PUB'
        """)
        before[table] = tmp_store.con.execute(f"""
            SELECT * FROM {table} WHERE symbol='OTHER' OR source='other-source' ORDER BY 1
        """).fetchall()
    tmp_store.con.execute("UPDATE fundamental_statement_points SET value=value+1")
    result = refresh_fundamental_standardized(
        tmp_store, FundamentalStandardizationOptions(symbols=("PUB",), run_id="scoped-refresh")
    )
    assert not result.standardized.empty
    assert not result.exceptions.empty
    assert set(result.standardized["symbol"]) == {"PUB"}
    assert set(result.standardized["run_id"]) == {"scoped-refresh"}
    assert _rows(tmp_store, "_std_output") == [("caller-persistent",)]
    for table, expected in before.items():
        assert tmp_store.con.execute(f"""
            SELECT * FROM {table} WHERE symbol='OTHER' OR source='other-source' ORDER BY 1
        """).fetchall() == expected


def test_0318_refuses_to_drop_a_required_unique_index(publication_store):
    store = publication_store
    _small_table(store)
    store.con.execute("""
        CREATE UNIQUE INDEX idx_fundamental_fact_revisions_group
        ON fundamental_fact_revisions(token)
    """)
    with pytest.raises(RuntimeError, match="non-optional index"):
        _bounded_fundamentals_publication(store.con)
    assert store.con.execute("""
        SELECT is_unique FROM duckdb_indexes()
        WHERE index_name='idx_fundamental_fact_revisions_group'
    """).fetchone() == (True,)
