"""Focused filing-event correctness checks; root runs once after writer exit."""

from __future__ import annotations

import datetime as dt
import json

import pyarrow.parquet as pq
import pytest

from atx_db import _derived_pit as pit
from atx_db import derived_metrics as engine
from atx_db.api.catalog import DERIVED_METRICS_SCHEMA
from atx_db.api.models import RangeRequest
from atx_db.api.service import WarehouseReadService
from atx_db.derived_factor_projection import FactorProjection, FactorProjectionOptions, load_projection_inputs
from atx_db.derived_registry import DerivedMetricDefinition
from atx_db.market_daily import MarketDailyOptions, refresh_market_daily_metrics
from atx_db.panel_export import export_panel_quarterly
from atx_db.provider_coverage import _schema_stats
from atx_db.publication import RELEASE_DATASETS, release_query


def metric(code, expression, inputs, window="q"):
    return DerivedMetricDefinition(code, "rollup", expression, window, tuple(inputs), False, "PIT test", "1")


@pytest.fixture
def store(tmp_store, monkeypatch):
    tmp_store.con.execute("SET memory_limit='1GB'")
    tmp_store.con.execute("SET threads=1")
    definitions = (
        metric("revenue_ttm", "ttm(revenue)", ["item:revenue"], "ttm"),
        metric("gross_profit_q", "coalesce(gross_profit__1004, revenue - cost_of_revenue_cogs)",
               ["item:gross_profit__1004", "item:revenue", "item:cost_of_revenue_cogs"]),
        metric("gross_profit_ttm", "ttm(gross_profit_q)", ["metric:gross_profit_q"], "ttm"),
        metric("gross_margin", "safe_div(gross_profit_ttm, revenue_ttm)",
               ["metric:gross_profit_ttm", "metric:revenue_ttm"]),
        metric("revenue_growth_yoy", "yoy(revenue_ttm)", ["metric:revenue_ttm"]),
    )
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: definitions)
    return tmp_store


def fact(store, period, value, at, *, code="revenue", revision=1, security="S1", identity=None):
    period = dt.date.fromisoformat(period) if isinstance(period, str) else period
    at = dt.datetime.fromisoformat(at) if isinstance(at, str) else at
    identity = identity or f"{security}|{code}|{period}|{revision}"
    store.con.execute("""
        INSERT INTO fundamental_standardized (
            standardized_id, source, security_id, item_id, canonical_code, basis,
            period_end, value, as_of_date, available_at, input_codes_json,
            input_item_ids_json, rule_id, combination_rule, revision_sequence, is_latest_revision
        ) VALUES (?, 'test', ?, 1, ?, 'quarterly', ?, ?, ?, ?, '[]', '[]', 'r', 'direct', ?, true)
    """, [identity, security, code, period, value, at.date(), at, revision])
    store.con.execute("""
        UPDATE fundamental_standardized SET is_latest_revision=false
        WHERE security_id=? AND canonical_code=? AND period_end=? AND available_at < ?
    """, [security, code, period, at])


def bar(store, date, *, shares=100.0, close=46.0):
    date = dt.date.fromisoformat(date)
    store.con.execute("""
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, open, high, low, close, adjusted_close,
            volume, is_adjusted, available_at, as_of_date, is_latest_revision, shares_outstanding
        ) VALUES ('test', 'S1', 'AAA', ?, ?, ?, ?, ?, ?, 1000, false, ?, ?, true, ?)
    """, [date, close, close, close, close, close,
          dt.datetime.combine(date, dt.time(22)), date, shares])


def projection_membership(store):
    store.con.execute("""
        INSERT INTO universe_membership (universe_id, security_id, valid_from, as_of_date,
            available_at, is_member, is_latest_revision, source, reason, rules_json)
        VALUES ('pit-test', 'S1', DATE '2020-01-01', DATE '2020-01-01', TIMESTAMP '2020-01-01', true, true, 'test', 'test', '{}')
    """)


def state(store, code, period, cutoff="2099-01-01"):
    return store.con.execute("""
        SELECT value, available_at, value_status, derived_value_id, inputs_hash, arithmetic_available_at
        FROM derived_metric_values WHERE security_id='S1' AND metric_code=? AND period_end=?
          AND available_at <= CAST(? AS TIMESTAMP)
        ORDER BY available_at DESC, derived_value_id DESC LIMIT 1
    """, [code, period, cutoff]).fetchone()


def audit_history(store, *, amend=True):
    periods = ["2023-06-30", "2023-09-30", "2023-12-31", "2024-03-31", "2024-06-30", "2024-09-30", "2024-12-31"]
    events = ["2023-08-10", "2023-11-10", "2024-02-10", "2024-05-10", "2024-08-10", "2024-11-10", "2025-02-10"]
    for index, (period, event) in enumerate(zip(periods, events, strict=True)):
        fact(store, period, 70 + 10 * index, event + " 12:00:00")
        fact(store, period, 60, event + " 12:00:00", code="cost_of_revenue_cogs")
    if amend:
        fact(store, "2024-06-30", 160, "2025-03-10 12:00:00", revision=2)


def test_exact_amendment_dependency_export_daily_and_projection(store, tmp_path):
    audit_history(store)
    engine.refresh_derived_metrics(store)
    before = state(store, "revenue_ttm", "2024-12-31", "2025-02-20")
    after = state(store, "revenue_ttm", "2024-12-31")
    assert before[:3] == (460.0, dt.datetime(2025, 2, 10, 12), "valid")
    assert after[:3] == (510.0, dt.datetime(2025, 3, 10, 12), "valid")
    assert before[3:5] != after[3:5]
    for cutoff, numerator, denominator in (("2025-02-20", 220, 460), ("2025-03-20", 270, 510)):
        assert state(store, "gross_margin", "2024-12-31", cutoff)[0] == pytest.approx(numerator / denominator)
        result = export_panel_quarterly(store, dt.date.fromisoformat(cutoff), ["revenue"],
                                        ["revenue_ttm"], tmp_path / cutoff)
        rows = {str(row["period_end"]): row for row in pq.read_table(result.parquet_path).to_pylist()}
        assert rows["2024-12-31"]["revenue"] == 130
        assert rows["2024-12-31"]["revenue_ttm"] == denominator
        assert rows["2024-06-30"]["revenue"] == (110 if denominator == 460 else 160)
        bar(store, cutoff)
    refresh_market_daily_metrics(store, MarketDailyOptions(security_ids=("S1",)))
    assert store.con.execute("SELECT ps_ttm FROM market_daily_metrics ORDER BY trade_date").fetchall() == [
        (pytest.approx(10),), (pytest.approx(4600 / 510),),
    ]
    projection_membership(store)
    projection = FactorProjection("pit", "revenue_ttm", "quarter", 1, "PIT", "rollup", 0.0, 1, "test")
    values = load_projection_inputs(store, projection, FactorProjectionOptions(universe_id="pit-test"))
    assert values["metric_value"].tolist() == [460, 510]


def test_downstream_growth_rebuild_includes_changed_lagged_ttm(store):
    audit_history(store, amend=False)
    for period, value, event in (("2025-03-31", 140, "2025-05-10"), ("2025-06-30", 150, "2025-08-10"),
                                  ("2025-09-30", 160, "2025-11-10"), ("2025-12-31", 170, "2026-02-10")):
        fact(store, period, value, event)
    engine.refresh_derived_metrics(store)
    assert state(store, "revenue_growth_yoy", "2025-12-31")[0] == pytest.approx((620 - 460) / 460)
    fact(store, "2024-06-30", 160, "2026-03-10", revision=2)
    # The caller requests the changed parent only. Its descendant growth state
    # at target+4 must be rebuilt too, using both original and amended baselines.
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(metric_codes=("revenue_ttm",)))
    assert state(store, "revenue_growth_yoy", "2025-12-31", "2026-02-20")[0] == pytest.approx((620 - 460) / 460)
    assert state(store, "revenue_growth_yoy", "2025-12-31")[0] == pytest.approx((620 - 510) / 510)
    fact(store, "2024-06-30", 170, "2026-04-10", revision=3)
    # A leaf request recomputes its revenue_ttm prerequisite. Growth is a
    # sibling consumer of that prerequisite and must not retain the 510 base.
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(metric_codes=("gross_margin",)))
    assert state(store, "revenue_growth_yoy", "2025-12-31", "2026-03-20")[0] == pytest.approx((620 - 510) / 510)
    assert state(store, "revenue_growth_yoy", "2025-12-31")[0] == pytest.approx((620 - 520) / 520)


def test_invalid_recovery_fallback_api_coverage_and_release(store, monkeypatch, tmp_path):
    definitions = (
        metric("current_ratio", "safe_div(revenue, cost_of_revenue_cogs)",
               ["item:revenue", "item:cost_of_revenue_cogs"]),
        metric("gross_margin", "coalesce(current_ratio, total_assets)", ["metric:current_ratio", "item:total_assets"]),
    )
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: definitions)
    fact(store, "2024-12-31", 100, "2025-02-01")
    fact(store, "2024-12-31", 7, "2025-02-01", code="total_assets")
    for revision, event, value in ((1, "2025-02-01", 10), (2, "2025-03-01", 0), (3, "2025-04-01", 20)):
        fact(store, "2024-12-31", value, event, code="cost_of_revenue_cogs", revision=revision)
    engine.refresh_derived_metrics(store)
    rows = store.con.execute("""
        SELECT value, value_status, available_at, valid_to, revision_sequence, revision_count
        FROM derived_metric_values WHERE metric_code='current_ratio' ORDER BY available_at
    """).fetchall()
    assert [row[:2] for row in rows] == [(10, "valid"), (None, "zero_denominator"), (5, "valid")]
    assert [row[4:] for row in rows] == [(1, 3), (2, 3), (3, 3)]
    assert rows[0][3] == rows[1][2] and rows[1][3] == rows[2][2] and rows[2][3] is None
    fallback = state(store, "gross_margin", "2024-12-31", "2025-03-15")
    assert fallback[0:2] == (7, dt.datetime(2025, 3, 1))
    assert fallback[5] == dt.datetime(2025, 2, 1)  # arithmetic known earlier; transition is March
    result = export_panel_quarterly(store, dt.date(2025, 3, 15), [], ["current_ratio"], tmp_path)
    assert pq.read_table(result.parquet_path).to_pylist()[0]["current_ratio"] is None
    request = RangeRequest(dataset="ATX.US.FUNDAMENTALS", schema="derived-metrics", symbols=["S1"],
                           stype_in="security_id", start=dt.date(2024, 1, 1), end=dt.date(2025, 1, 1),
                           items=["current_ratio"])
    cursor = WarehouseReadService()._range_cursor(store.con, request, DERIVED_METRICS_SCHEMA,
                dt.datetime(2025, 3, 15), ["value", "value_status"], ["S1"])
    assert cursor.fetchall() == [(None, "zero_denominator")]
    first_reported = request.model_copy(update={"vintage": "first_reported"})
    cursor = WarehouseReadService()._range_cursor(store.con, first_reported, DERIVED_METRICS_SCHEMA,
                dt.datetime(2025, 4, 15), ["value", "value_status", "available_at"], ["S1"])
    assert cursor.fetchall() == [(10.0, "valid", dt.datetime(2025, 2, 1))]

    # Read the same quarterly ratio directly through the daily and factor
    # consumers. March must expose the invalid state, not February's quotient.
    from atx_db import market_daily

    monkeypatch.setattr(market_daily, "default_derived_definitions", lambda: (
        metric("ps_ttm", "current_ratio", ["metric:current_ratio"], "daily"),
    ))
    for date in ("2025-02-15", "2025-03-15", "2025-04-15"):
        bar(store, date)
    refresh_market_daily_metrics(store, MarketDailyOptions(security_ids=("S1",)))
    daily_rows = store.con.execute("""
        SELECT ps_ttm, fundamental_available_at FROM market_daily_metrics ORDER BY trade_date
    """).fetchall()
    assert daily_rows == [(10.0, dt.datetime(2025, 2, 1)),
                          (None, dt.datetime(2025, 3, 1)),
                          (5.0, dt.datetime(2025, 4, 1))]
    projection_membership(store)
    projection = FactorProjection("pit-invalid", "current_ratio", "quarter", 1, "PIT", "rollup", 0.0, 1, "test")
    projected = load_projection_inputs(store, projection, FactorProjectionOptions(universe_id="pit-test"))
    assert projected["metric_value"].isna().tolist() == [False, True, False]
    assert projected["metric_value"].iloc[[0, 2]].tolist() == [10.0, 5.0]
    assert projected["metric_available_at"].iloc[1] == dt.datetime(2025, 3, 1)
    assert json.loads(projected["metric_state_json"].iloc[1])["value_status"] == "zero_denominator"

    dataset = next(d for d in RELEASE_DATASETS if d.object_name == "derived_metric_values")
    published = store.con.execute(release_query(dataset, ["derived_value_id", "value_status"])).fetchall()
    assert sum(status == "zero_denominator" for _, status in published) == 1
    assert _schema_stats(store, DERIVED_METRICS_SCHEMA)[2] == 2
    # A latest invalid state removes that metric from valid breadth, while
    # its historical valid event remains published and API-addressable.
    fact(store, "2024-12-31", 0, "2025-05-01", code="cost_of_revenue_cogs", revision=4)
    engine.refresh_derived_metrics(store)
    assert _schema_stats(store, DERIVED_METRICS_SCHEMA)[2] == 1


def test_delayed_gap_arrival_and_same_value_lineage_are_clocked(store):
    for period, value, event in (("2024-03-31", 100, "2024-05-01"), ("2024-09-30", 120, "2024-11-01"),
                                  ("2024-12-31", 130, "2025-02-01")):
        fact(store, period, value, event)
    fact(store, "2024-06-30", 110, "2025-03-01")
    fact(store, "2024-06-30", 110, "2025-04-01", revision=2)
    engine.refresh_derived_metrics(store)
    missing = state(store, "revenue_ttm", "2024-12-31", "2025-02-20")
    first = state(store, "revenue_ttm", "2024-12-31", "2025-03-20")
    same = state(store, "revenue_ttm", "2024-12-31")
    assert missing[:3] == (None, dt.datetime(2025, 2, 1), "missing_input_or_domain")
    assert first[:2] == (460, dt.datetime(2025, 3, 1))
    assert same[:2] == (460, dt.datetime(2025, 4, 1))
    assert first[3:5] != same[3:5]


def test_chunk_equivalence_ties_and_event_aware_stub_period(store, monkeypatch):
    definitions = (metric("current_ratio", "revenue", ["item:revenue"]),)
    monkeypatch.setattr(engine, "default_derived_definitions", lambda: definitions)
    fact(store, "2024-12-28", 100, "2025-02-01", identity="a")
    fact(store, "2024-12-28", 120, "2025-02-01", identity="z")
    fact(store, "2024-12-31", 130, "2025-03-01", revision=2)
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(event_chunk_size=1))
    sql = "SELECT derived_value_id, period_end, value, available_at, inputs_hash, revision_sequence, valid_to FROM derived_metric_values ORDER BY derived_value_id"
    first = store.con.execute(sql).fetchall()
    assert state(store, "current_ratio", "2024-12-28", "2025-02-20")[0] == 120
    assert state(store, "current_ratio", "2024-12-31", "2025-02-20") is None
    assert state(store, "current_ratio", "2024-12-31")[0] == 130
    old_date_range = RangeRequest(dataset="ATX.US.FUNDAMENTALS", schema="derived-metrics", symbols=["S1"],
                                 stype_in="security_id", start=dt.date(2024, 12, 28), end=dt.date(2024, 12, 29),
                                 items=["current_ratio"])
    service = WarehouseReadService()
    fields = ["period_end", "value"]
    assert service._range_cursor(store.con, old_date_range, DERIVED_METRICS_SCHEMA,
                dt.datetime(2025, 2, 20), fields, ["S1"]).fetchall() == [(dt.date(2024, 12, 28), 120.0)]
    assert service._range_cursor(store.con, old_date_range, DERIVED_METRICS_SCHEMA,
                dt.datetime(2025, 3, 20), fields, ["S1"]).fetchall() == []
    all_dates = old_date_range.model_copy(update={"end": dt.date(2025, 1, 1)})
    assert service._range_cursor(store.con, all_dates, DERIVED_METRICS_SCHEMA,
                dt.datetime(2025, 3, 20), fields, ["S1"]).fetchall() == [(dt.date(2024, 12, 31), 130.0)]
    first_reported = old_date_range.model_copy(update={"vintage": "first_reported"})
    assert service._range_cursor(store.con, first_reported, DERIVED_METRICS_SCHEMA,
                dt.datetime(2025, 3, 20), fields, ["S1"]).fetchall() == [(dt.date(2024, 12, 28), 120.0)]
    # Reversing physical insertion order and changing run IDs must not alter
    # ties, revision groups, event identities, or selected input lineage.
    store.con.execute("CREATE TEMP TABLE reversed_raw AS SELECT * FROM fundamental_standardized ORDER BY standardized_id DESC")
    try:
        store.con.execute("DELETE FROM fundamental_standardized")
        store.con.execute("INSERT INTO fundamental_standardized SELECT * FROM reversed_raw")
    finally:
        # The refresh may recycle its connection; finish this caller-owned
        # staging table before entering the bounded production lifecycle.
        store.con.execute("DROP TABLE reversed_raw")
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(event_chunk_size=100, run_id="different"))
    assert store.con.execute(sql).fetchall() == first


def test_bounded_frames_scope_preservation_and_prepublication_failure(store, monkeypatch):
    audit_history(store)
    engine.refresh_derived_metrics(store)
    before = store.con.execute("SELECT derived_value_id, inputs_hash FROM derived_metric_values ORDER BY 1").fetchall()
    fact(store, "2024-12-31", 900, "2025-04-01", revision=2, security="S2")
    frames = []
    original = pit.frame_sql

    def counted_frame(definition, lowered, context, annual_plan):
        assert lowered.max_lag + 1 <= 8
        frames.append(lowered.max_lag + 1)
        return original(definition, lowered, context, annual_plan)

    monkeypatch.setattr(pit, "frame_sql", counted_frame)
    engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S2",), max_frame_rows=8, event_chunk_size=100))
    assert frames and max(frames) == 5
    assert store.con.execute("SELECT derived_value_id, inputs_hash FROM derived_metric_values WHERE security_id='S1' ORDER BY 1").fetchall() == before
    with pytest.raises(RuntimeError, match="input limit"):
        engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",), max_input_rows=1))
    with pytest.raises(RuntimeError, match="candidate upper bound"):
        engine.refresh_derived_metrics(store, engine.DerivedMetricsOptions(security_ids=("S1",), max_candidate_rows=1))
    assert store.con.execute("SELECT derived_value_id, inputs_hash FROM derived_metric_values WHERE security_id='S1' ORDER BY 1").fetchall() == before


def test_scoped_atomic_rollback_on_publish_error(store, monkeypatch):
    audit_history(store)
    engine.refresh_derived_metrics(store)
    before = store.con.execute("SELECT derived_value_id, inputs_hash FROM derived_metric_values ORDER BY 1").fetchall()
    original = pit.finish_metric

    def duplicate_after_staging(con):
        original(con)
        if con.execute("SELECT count(*) FROM _pit_stage WHERE metric_code='revenue_growth_yoy'").fetchone()[0]:
            # Violate the required canonical PK only at final publication,
            # after the scoped DELETE. The whole prior scope must survive.
            con.execute("INSERT INTO _pit_stage SELECT * FROM _pit_stage LIMIT 1")

    monkeypatch.setattr(pit, "finish_metric", duplicate_after_staging)
    with pytest.raises(Exception, match=r"[Dd]uplicate|[Pp]rimary|[Cc]onstraint"):
        engine.refresh_derived_metrics(store)
    assert store.con.execute("SELECT derived_value_id, inputs_hash FROM derived_metric_values ORDER BY 1").fetchall() == before


def test_daily_raw_and_dei_history_preserves_pre_amendment_state(store, monkeypatch):
    from atx_db import market_daily

    definitions = (
        metric("market_cap", "close * shares_outstanding", ["market:close", "market:shares_outstanding"], "daily"),
        metric("ps_ttm", "safe_div(market_cap, revenue)", ["metric:market_cap", "item:revenue"], "daily"),
    )
    monkeypatch.setattr(market_daily, "default_derived_definitions", lambda: definitions)
    fact(store, "2024-06-30", 110, "2024-08-10")
    fact(store, "2024-06-30", 160, "2025-03-10", revision=2)
    fact(store, "2024-12-31", 130, "2025-02-10")
    for revision, event, count in ((1, "2024-08-10", 100), (2, "2025-03-10", 200), (3, "2025-04-10", 0)):
        store.con.execute("""
            INSERT INTO shares_outstanding_history (share_history_id, source, security_id, cik, share_count_type,
                taxonomy, concept, unit, period_type, period_end, effective_date, as_of_date, available_at,
                accession_number, revision_sequence, revision_count, is_latest_revision, share_count, source_url)
            VALUES (?, 'test', 'S1', '1', 'shares_outstanding', 'dei', 'shares', 'shares', 'instant',
                    DATE '2024-06-30', DATE '2024-06-30', CAST(? AS DATE), CAST(? AS TIMESTAMP),
                    'test', ?, 3, ?, ?, '')
        """, [f"dei-{revision}", event, event, revision, revision == 3, count])
    for date in ("2024-09-20", "2025-03-20", "2025-04-20"):
        bar(store, date, shares=50)
    refresh_market_daily_metrics(store, MarketDailyOptions(security_ids=("S1",)))
    rows = store.con.execute("SELECT shares_outstanding, shares_source, ps_ttm FROM market_daily_metrics ORDER BY trade_date").fetchall()
    assert rows == [(100, "dei", pytest.approx(4600 / 110)),
                    (200, "dei", pytest.approx(9200 / 130)),
                    (50, "archive", pytest.approx(2300 / 130))]


def test_candidate_and_local_frame_growth_is_linear_in_input_events(store):
    definition = metric("revenue_ttm", "ttm(revenue)", ["item:revenue"], "ttm")
    sizes = []
    for quarters in (20, 40):
        security = f"N{quarters}"
        for index in range(quarters):
            year, quarter = 2000 + index // 4, index % 4
            month, day = ((3, 31), (6, 30), (9, 30), (12, 31))[quarter]
            period = dt.date(year, month, day)
            for revision in range(1, 4):
                event = dt.datetime.combine(period + dt.timedelta(days=40 + 365 * (revision - 1)), dt.time(12))
                fact(store, period, 100 + index + revision, event, revision=revision, security=security)
        try:
            pit.prepare_security(store.con, security, 1000)
            count = pit.prepare_metric(store.con, definition, 3, 2000)
            events = store.con.execute("SELECT count(*) FROM _pit_items").fetchone()[0]
            targets = store.con.execute("SELECT count(*) FROM _pit_targets").fetchone()[0]
            assert count <= events * 4 + targets
            frame_rows = store.con.execute("""
                SELECT count(*) FROM _pit_keys k,
                LATERAL range(k.target_bucket - 3, k.target_bucket + 1) f(bucket)
            """).fetchone()[0]
            assert frame_rows == count * 4
            assert frame_rows < events * quarters
            sizes.append(frame_rows)
        finally:
            pit.cleanup(store.con)
    assert sizes[1] < 3 * sizes[0]


def test_populated_0314_upgrade_preserves_legacy_contract_and_reentry(tmp_path, monkeypatch):
    """Upgrade two legacy rows from the real 0314 bootstrap through current HEAD."""
    import duckdb

    import atx_db.migrations as migrations
    from atx_db.connection import DuckDBStore
    from atx_db.schema_contract import assert_schema_contract_version

    db_path = tmp_path / "populated-derived-0314.duckdb"
    # Configure before initialization; this separate tiny warehouse must not
    # inherit a machine-sized DuckDB budget while replaying schema migrations.
    bounded_config = {"memory_limit": "256MB", "threads": "1", "preserve_insertion_order": "false"}
    upgrade_store = DuckDBStore(db_path)
    upgrade_store.analytical_memory_limit = bounded_config["memory_limit"]
    upgrade_store.analytical_threads = int(bounded_config["threads"])
    with duckdb.connect(str(db_path), config=bounded_config) as con:
        upgrade_store.connection = con
        upgrade_store._configure_session(con)
        with monkeypatch.context() as patch:
            patch.setattr(migrations, "MIGRATIONS", [m for m in migrations.MIGRATIONS if m.version <= 314])
            upgrade_store.initialize()
        assert con.execute("SELECT max(CAST(version AS INTEGER)) FROM schema_migrations").fetchone() == (314,)
        assert con.execute("""
            SELECT is_nullable FROM duckdb_columns()
            WHERE table_name='derived_metric_values' AND column_name='value'
        """).fetchone() == (False,)
        assert con.execute("""
            SELECT count(*) FROM duckdb_indexes() WHERE index_name='idx_derived_metric_values_lookup'
        """).fetchone() == (1,)
        con.execute("""
            INSERT INTO derived_metric_values (
                derived_value_id, source, security_id, metric_code, metric_window, period_end,
                value, available_at, inputs_hash, as_of_date, is_latest_revision, run_id, source_loaded_at
            ) VALUES
                ('legacy-a', 'legacy-test', 'S1', 'current_ratio', 'q', DATE '2024-09-30',
                 10.0, TIMESTAMP '2024-11-01', 'legacy-input-a', DATE '2024-11-01', true, 'old-run', TIMESTAMP '2025-02-02'),
                ('legacy-b', 'legacy-test', 'S1', 'current_ratio', 'q', DATE '2024-12-31',
                 20.0, TIMESTAMP '2025-02-01', 'legacy-input-b', DATE '2025-02-01', true, 'old-run', TIMESTAMP '2025-02-02')
        """)
        original_columns = (
            "derived_value_id, source, security_id, metric_code, metric_window, period_end, "
            "value, available_at, inputs_hash, as_of_date, is_latest_revision, run_id, source_loaded_at"
        )
        original_sql = f"SELECT {original_columns} FROM derived_metric_values ORDER BY derived_value_id"
        before = con.execute(original_sql).fetchall()
        # Commit the legacy fixture to disk and release the bootstrap session's
        # buffers before exercising the populated upgrade in a fresh session.
        con.execute("CHECKPOINT")

    with duckdb.connect(str(db_path), config=bounded_config) as con:
        upgrade_store.connection = con
        upgrade_store._configure_session(con)
        pending_versions = [m.version for m in migrations.MIGRATIONS if m.version > 314]
        assert pending_versions[:2] == [315, 316]
        assert migrations.apply_pending_migrations(con) == pending_versions
        assert con.execute("SELECT max(CAST(version AS INTEGER)) FROM schema_migrations").fetchone() == (pending_versions[-1],)
        assert con.execute(original_sql).fetchall() == before
        assert con.execute("""
            SELECT value_origin, fiscal_period_start, fiscal_period_end
            FROM derived_metric_values ORDER BY derived_value_id
        """).fetchall() == [("legacy_unspecified", None, None), ("legacy_unspecified", None, None)]
        assert con.execute("""
            SELECT count(*) FROM duckdb_tables()
            WHERE table_name='derived_metric_values' AND NOT temporary
        """).fetchone() == (1,)
        assert con.execute("""
            SELECT is_nullable FROM duckdb_columns()
            WHERE table_name='derived_metric_values' AND column_name='value'
        """).fetchone() == (True,)
        assert con.execute("""
            SELECT constraint_column_names FROM duckdb_constraints()
            WHERE table_name='derived_metric_values' AND constraint_type='PRIMARY KEY'
        """).fetchall() == [(["derived_value_id"],)]
        assert con.execute("""
            SELECT count(*) FROM duckdb_indexes() WHERE index_name='idx_derived_metric_values_lookup'
        """).fetchone() == (0,)
        assert con.execute("""
            SELECT derived_value_id, value_status, history_status, revision_group_id,
                   definition_hash, target_bucket, valid_to
            FROM derived_metric_values ORDER BY derived_value_id
        """).fetchall() == [
            (identity, "valid", "legacy_latest_only", None, None, None, None)
            for identity in ("legacy-a", "legacy-b")
        ]
        # Both rows share a NULL reconstructed group and the same security /
        # metric, so this specifically catches collapse without the ID fallback.
        request = RangeRequest(dataset="ATX.US.FUNDAMENTALS", schema="derived-metrics", symbols=["S1"],
                               stype_in="security_id", start=dt.date(2024, 1, 1), end=dt.date(2025, 1, 1),
                               items=["current_ratio"])
        for vintage in ("latest", "first_reported"):
            visible = WarehouseReadService()._range_cursor(
                con, request.model_copy(update={"vintage": vintage}), DERIVED_METRICS_SCHEMA,
                dt.datetime(2025, 3, 15), ["derived_value_id", "value", "history_status"], ["S1"],
            ).fetchall()
            assert visible == [("legacy-a", 10.0, "legacy_latest_only"), ("legacy-b", 20.0, "legacy_latest_only")]
        assert _schema_stats(upgrade_store, DERIVED_METRICS_SCHEMA)[:4] == (0, 0, 0, 0)
        assert con.execute("""
            SELECT nullable FROM field_catalog WHERE table_name='derived_metric_values' AND field_name='value'
        """).fetchone() == (True,)
        assert_schema_contract_version(con)

        # Prove the constraints work in addition to inspecting their metadata;
        # roll back the nullable probe so both legacy rows remain unchanged.
        con.execute("BEGIN TRANSACTION")
        con.execute("UPDATE derived_metric_values SET value=NULL WHERE derived_value_id='legacy-a'")
        assert con.execute("SELECT value FROM derived_metric_values WHERE derived_value_id='legacy-a'").fetchone() == (None,)
        con.execute("ROLLBACK")
        with pytest.raises(duckdb.ConstraintException):
            con.execute("INSERT INTO derived_metric_values SELECT * FROM derived_metric_values WHERE derived_value_id='legacy-a'")
        assert migrations.apply_pending_migrations(con) == []
        upgrade_store._initialized = False
        upgrade_store.initialize()
        assert con.execute("SELECT count(*) FROM schema_migrations WHERE CAST(version AS INTEGER)=315").fetchone() == (1,)
        assert con.execute(original_sql).fetchall() == before
        assert_schema_contract_version(con)
