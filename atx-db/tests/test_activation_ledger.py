"""Stage ledger for the resumable warehouse activation ladder."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.activation import (
    STAGE_ORDER,
    StageResult,
    begin_stage,
    completed_stages,
    finish_stage,
    select_stages,
)


def test_stage_order_is_the_documented_dependency_order():
    assert STAGE_ORDER == (
        "migrate",
        "security_master",
        "symbol_directory",
        "ticker_history_extract",
        "ticker_history_publish",
        "sec_bulk_download",
        "submissions_load",
        "earnings_release_facts",
        "companyfacts_load",
        "statement_points",
        "periods",
        "ttm",
        "calendarization",
        "standardized",
        "entity_classification",
        "industry_templates",
        "reconciliation",
        "derived_metrics",
        "market_daily",
        "equity_price_metrics",
        "listing_events",
        "listing_status",
        "legacy_liquid_universe",
        "factor_projections",
        "delisting_evidence",
        "universe_us_listed",
        "delisting_terminal_returns",
        "trading_calendar",
        "survivorship_forward_returns",
        "item_coverage",
        "provider_coverage",
        "quality",
    )


def test_activation_stage_runs_table_exists_after_migration(tmp_store):
    row = tmp_store.con.execute(
        "SELECT count(*) FROM duckdb_tables() WHERE schema_name='main' AND table_name='activation_stage_runs'"
    ).fetchone()
    assert row is not None and row[0] == 1


def test_activation_stage_runs_has_the_charter_columns(tmp_store):
    columns = [
        name
        for (name,) in tmp_store.con.execute(
            "SELECT column_name FROM duckdb_columns() "
            "WHERE schema_name='main' AND table_name='activation_stage_runs' "
            "ORDER BY column_index"
        ).fetchall()
    ]
    assert columns == [
        "stage",
        "run_id",
        "status",
        "started_at",
        "finished_at",
        "rows",
        "params_json",
        "error",
    ]


def test_begin_then_finish_records_a_completed_stage(tmp_store):
    started = begin_stage(tmp_store, stage="migrate", run_id="run-1", params={"threads": 4})
    assert isinstance(started, dt.datetime)
    assert completed_stages(tmp_store) == set()
    finish_stage(tmp_store, stage="migrate", run_id="run-1", started_at=started, status="completed", rows=7)
    assert completed_stages(tmp_store) == {"migrate"}
    row = tmp_store.con.execute(
        'SELECT status, "rows", params_json, error FROM activation_stage_runs '
        "WHERE stage='migrate' AND run_id='run-1'"
    ).fetchone()
    assert row == ("completed", 7, '{"threads": 4}', None)


def test_failed_stage_is_not_reported_completed_and_carries_the_error(tmp_store):
    started = begin_stage(tmp_store, stage="periods", run_id="run-2", params={})
    finish_stage(
        tmp_store,
        stage="periods",
        run_id="run-2",
        started_at=started,
        status="failed",
        rows=0,
        error="boom",
    )
    assert completed_stages(tmp_store) == set()
    row = tmp_store.con.execute("SELECT status, error FROM activation_stage_runs WHERE stage='periods'").fetchone()
    assert row == ("failed", "boom")


def test_a_later_completed_run_supersedes_an_earlier_failure(tmp_store):
    first = begin_stage(tmp_store, stage="periods", run_id="run-2", params={})
    finish_stage(tmp_store, stage="periods", run_id="run-2", started_at=first, status="failed", error="boom")
    second = begin_stage(tmp_store, stage="periods", run_id="run-3", params={})
    finish_stage(tmp_store, stage="periods", run_id="run-3", started_at=second, status="completed", rows=3)
    assert completed_stages(tmp_store) == {"periods"}


def test_select_stages_defaults_to_the_full_ladder():
    assert select_stages() == STAGE_ORDER


def test_select_stages_honours_start_and_stop():
    assert select_stages(start="periods", stop="standardized") == (
        "periods",
        "ttm",
        "calendarization",
        "standardized",
    )


def test_select_stages_only_is_order_normalized():
    assert select_stages(only=("standardized", "migrate")) == ("migrate", "standardized")


def test_select_stages_rejects_an_unknown_stage():
    with pytest.raises(ValueError, match="unknown activation stage"):
        select_stages(start="nope")


def test_stage_result_is_a_rows_plus_detail_pair():
    result = StageResult(rows=5, detail={"k": "v"})
    assert result.rows == 5
    assert result.detail == {"k": "v"}
