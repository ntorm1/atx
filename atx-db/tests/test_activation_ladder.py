"""The activation ladder: ordering, ledgering, idempotence, dry-run, slicing."""

from __future__ import annotations

import datetime as dt
import json
import zipfile
from pathlib import Path

import pytest

from atx_db.activation import (
    STAGE_ORDER,
    STAGES,
    ActivationOptions,
    StageResult,
    completed_stages,
    run_activation,
)
from atx_db.connection import DuckDBStore

_HEADER = "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\tclosePr\tvolume\tshares\treturnFactor"
_MEMBER = "tbltickerhistory3_10y.txt"
_SYMBOLS = ("AAA", "BBB", "CCC")
_DATES = ("2024-01-02", "2024-01-03", "2024-01-04")


@pytest.fixture
def three_symbol_zip(tmp_path: Path) -> Path:
    rows = [
        f"{day}\t{32950 + i}\t{sym}\t{sym}\t{10.0 * i}\t{10.0 * i + 0.5}\t{10.0 * i - 0.1}\t"
        f"{10.0 * i + 0.2}\t{10.0 * i + 0.2}\t{1000 * i}\t{1_000_000 * i}\t1.0"
        for i, sym in enumerate(_SYMBOLS, start=1)
        for day in _DATES
    ]
    payload = ("\r\n".join([_HEADER, *rows]) + "\r\n").encode("utf-8")
    zip_path = tmp_path / "tbltickerhistory3_10y.zip"
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(_MEMBER, payload)
    return zip_path


@pytest.fixture
def offline_options(built_warehouse, tmp_path, three_symbol_zip) -> ActivationOptions:
    return ActivationOptions(
        db_path=built_warehouse("activation.duckdb"),
        as_of_date=dt.date(2024, 1, 4),
        ticker_history_zip=three_symbol_zip,
        ticker_history_expected_bytes=None,
        staging_dir=tmp_path / "staging",
        cache_dir=tmp_path / "cache",
        sec_user_agent="atx-db test agent test@example.com",
        minimum_rows=1,
        minimum_securities=1,
        minimum_latest_date_securities=1,
        reconciliation_shards=1,
        run_id="ladder-test",
    )


_OFFLINE_SLICE = (
    "migrate",
    "ticker_history_extract",
    "ticker_history_publish",
    "statement_points",
    "periods",
    "ttm",
    "calendarization",
    "standardized",
    "industry_templates",
    "provider_coverage",
)


def test_every_stage_in_stage_order_is_registered():
    assert tuple(sorted(STAGES)) == tuple(sorted(STAGE_ORDER))


def test_every_stage_function_has_the_uniform_signature():
    import inspect

    for name, func in STAGES.items():
        params = list(inspect.signature(func).parameters)
        assert params == ["store", "options"], f"{name} has {params}"
        assert inspect.signature(func).return_annotation in (StageResult, "StageResult")


def test_ladder_emits_one_json_line_per_stage(offline_options):
    lines: list[dict[str, object]] = []
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lines.append)
    assert [line["stage"] for line in lines] == list(_OFFLINE_SLICE)
    for line in lines:
        assert line["status"] == "completed"
        assert isinstance(line["rows"], int)
        assert isinstance(line["seconds"], float)
        json.dumps(line)  # every emitted payload must be JSON-serialisable


def test_ladder_records_every_stage_in_the_ledger(offline_options):
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    with DuckDBStore(offline_options.db_path) as store:
        assert completed_stages(store) == set(_OFFLINE_SLICE)


def test_rerunning_the_ladder_skips_completed_stages(offline_options):
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    second: list[dict[str, object]] = []
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=second.append)
    assert [line["status"] for line in second] == ["skipped"] * len(_OFFLINE_SLICE)
    with DuckDBStore(offline_options.db_path) as store:
        attempts = store.con.execute(
            "SELECT stage, count(*) FROM activation_stage_runs GROUP BY stage ORDER BY stage"
        ).fetchall()
    assert {stage for stage, _ in attempts} == set(_OFFLINE_SLICE)


def test_rerunning_the_ladder_leaves_published_row_counts_unchanged(offline_options):
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    with DuckDBStore(offline_options.db_path) as store:
        before = store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    with DuckDBStore(offline_options.db_path) as store:
        after = store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()
    assert before == after == (9,)


def test_force_reruns_completed_stages(offline_options):
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lambda payload: None)
    forced = ActivationOptions(**{**offline_options.as_dict(), "force": True})
    lines: list[dict[str, object]] = []
    run_activation(forced, stages=_OFFLINE_SLICE, emit=lines.append)
    assert [line["status"] for line in lines] == ["completed"] * len(_OFFLINE_SLICE)


def test_dry_run_reports_the_plan_and_writes_nothing(offline_options):
    planned = ActivationOptions(**{**offline_options.as_dict(), "dry_run": True})
    lines: list[dict[str, object]] = []
    run_activation(planned, stages=_OFFLINE_SLICE, emit=lines.append)
    assert [line["status"] for line in lines] == ["dry_run"] * len(_OFFLINE_SLICE)
    with DuckDBStore(planned.db_path) as store:
        rows = store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()
        assert rows is not None and rows[0] == 0
        assert completed_stages(store) == set()


def test_a_failing_stage_stops_the_ladder_and_is_recorded(offline_options, monkeypatch):
    def boom(store, options):
        raise RuntimeError("synthetic periods failure")

    monkeypatch.setitem(STAGES, "periods", boom)
    lines: list[dict[str, object]] = []
    with pytest.raises(RuntimeError, match="synthetic periods failure"):
        run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lines.append)
    assert [line["stage"] for line in lines][-1] == "periods"
    assert lines[-1]["status"] == "failed"
    with DuckDBStore(offline_options.db_path) as store:
        row = store.con.execute(
            "SELECT status, error FROM activation_stage_runs WHERE stage = 'periods'"
        ).fetchone()
    assert row is not None and row[0] == "failed" and "synthetic" in row[1]
    assert "ttm" not in [line["stage"] for line in lines]
