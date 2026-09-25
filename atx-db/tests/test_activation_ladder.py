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

_CIK = "0000320193"


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


def _companyfacts_rich_payload() -> bytes:
    """Two years of quarterly Assets/Revenues/NetIncomeLoss facts for one CIK.

    ``Assets`` (balance_sheet, instant), ``Revenues`` and ``NetIncomeLoss``
    (income_statement, duration) are all recognized concepts in
    ``src/atx_db/seeds/concept_map.csv``. Eight consecutive fiscal quarters give
    ``refresh_fundamental_ttm_points`` enough duration history to compute a
    trailing-twelve-month window, and every quarter has at least one mapped
    concept for calendarization/standardization -- so the derived-chain ladder
    stages exercise real row-producing logic instead of asserting on an empty
    schema.
    """
    quarters = (
        ("2022-01-01", "2022-03-31", 2022, "Q1", "10-Q"),
        ("2022-04-01", "2022-06-30", 2022, "Q2", "10-Q"),
        ("2022-07-01", "2022-09-30", 2022, "Q3", "10-Q"),
        ("2022-10-01", "2022-12-31", 2022, "FY", "10-K"),
        ("2023-01-01", "2023-03-31", 2023, "Q1", "10-Q"),
        ("2023-04-01", "2023-06-30", 2023, "Q2", "10-Q"),
        ("2023-07-01", "2023-09-30", 2023, "Q3", "10-Q"),
        ("2023-10-01", "2023-12-31", 2023, "FY", "10-K"),
    )

    def duration_facts(base: int, step: int) -> list[dict[str, object]]:
        return [
            {
                "start": start,
                "end": end,
                "val": base + step * i,
                "accn": f"0000320193-{fy}-{100000 + i:06d}",
                "fy": fy,
                "fp": fp,
                "form": form,
                "filed": end,
            }
            for i, (start, end, fy, fp, form) in enumerate(quarters)
        ]

    def instant_facts(base: int, step: int) -> list[dict[str, object]]:
        return [
            {
                "end": end,
                "val": base + step * i,
                "accn": f"0000320193-{fy}-{100000 + i:06d}",
                "fy": fy,
                "fp": fp,
                "form": form,
                "filed": end,
            }
            for i, (_start, end, fy, fp, form) in enumerate(quarters)
        ]

    return json.dumps(
        {
            "cik": 320193,
            "entityName": "Apple Inc.",
            "facts": {
                "us-gaap": {
                    "Assets": {
                        "label": "Assets",
                        "units": {"USD": instant_facts(300_000_000_000, 1_000_000_000)},
                    },
                    "Revenues": {
                        "label": "Revenues",
                        "units": {"USD": duration_facts(80_000_000_000, 1_000_000_000)},
                    },
                    "NetIncomeLoss": {
                        "label": "Net Income",
                        "units": {"USD": duration_facts(20_000_000_000, 500_000_000)},
                    },
                }
            },
        }
    ).encode("utf-8")


@pytest.fixture
def offline_options(built_warehouse, tmp_path, three_symbol_zip) -> ActivationOptions:
    db_path = built_warehouse("activation.duckdb")
    options = ActivationOptions(
        db_path=db_path,
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
    # Pre-place the (already "downloaded") companyfacts archive and the ticker
    # row it targets, so stage_companyfacts_load has real facts to load offline
    # -- no network stage runs in this slice.
    options.companyfacts_zip.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(options.companyfacts_zip, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(f"CIK{_CIK}.json", _companyfacts_rich_payload())
    with DuckDBStore(db_path) as store:
        store.con.execute(
            """
            INSERT INTO sec_company_tickers (cik, ticker, title, security_id, source_loaded_at)
            VALUES (?, 'AAPL', 'Apple Inc.', 'SEC-CIK-0000320193', TIMESTAMP '2024-01-01 00:00:00')
            """,
            [_CIK],
        )
    return options


_OFFLINE_SLICE = (
    "migrate",
    "ticker_history_extract",
    "ticker_history_publish",
    "companyfacts_load",
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


def test_price_metrics_runs_after_market_daily_and_before_terminal_returns():
    assert STAGE_ORDER[STAGE_ORDER.index("market_daily") + 1] == "equity_price_metrics"
    assert STAGE_ORDER.index("equity_price_metrics") < STAGE_ORDER.index("delisting_terminal_returns")
    assert STAGE_ORDER[-1] == "quality"


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


def test_derived_chain_stages_publish_nonzero_rows(offline_options):
    """The fixture seeds real facts, so every derived-chain stage does real work."""
    lines: list[dict[str, object]] = []
    run_activation(offline_options, stages=_OFFLINE_SLICE, emit=lines.append)
    rows_by_stage = {line["stage"]: line["rows"] for line in lines}
    for stage in ("companyfacts_load", "statement_points", "periods", "ttm", "calendarization", "standardized"):
        assert rows_by_stage[stage] > 0, f"{stage} published zero rows: {rows_by_stage}"


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


def test_dry_run_on_a_nonexistent_db_path_creates_no_file(tmp_path):
    """Critical regression: dry-run must never bootstrap/migrate the warehouse.

    A write-mode ``DuckDBStore(path).__enter__()`` creates the file and runs
    ``initialize()`` (schema bootstrap + migrations) as a side effect, which
    ``--dry-run`` must never trigger -- it promises to report a plan and touch
    nothing.
    """
    db_path = tmp_path / "never-created.duckdb"
    options = ActivationOptions(db_path=db_path, dry_run=True, run_id="dry-run-fresh")
    lines: list[dict[str, object]] = []
    run_activation(options, stages=("migrate", "provider_coverage"), emit=lines.append)
    assert [line["status"] for line in lines] == ["dry_run", "dry_run"]
    assert not db_path.exists(), "dry-run on a fresh path must not create the warehouse file"


def test_dry_run_on_an_existing_warehouse_does_not_change_its_migration_version(offline_options):
    with DuckDBStore(offline_options.db_path) as store:
        before = store.con.execute(
            "SELECT max(try_cast(version AS INTEGER)) FROM schema_migrations"
        ).fetchone()
    planned = ActivationOptions(**{**offline_options.as_dict(), "dry_run": True})
    run_activation(planned, stages=("migrate",), emit=lambda payload: None)
    with DuckDBStore(offline_options.db_path) as store:
        after = store.con.execute(
            "SELECT max(try_cast(version AS INTEGER)) FROM schema_migrations"
        ).fetchone()
    assert before == after


def test_analytical_session_is_configured_once_per_store_open_not_per_stage(offline_options, monkeypatch):
    observed: list[tuple[object, object]] = []

    def spy(store, options):
        row = store.con.execute(
            "SELECT current_setting('threads'), current_setting('preserve_insertion_order')"
        ).fetchone()
        observed.append(row)
        return StageResult(rows=0, detail={})

    monkeypatch.setitem(STAGES, "periods", spy)
    monkeypatch.setitem(STAGES, "ttm", spy)
    run_activation(
        offline_options,
        stages=("migrate", "companyfacts_load", "statement_points", "periods", "ttm"),
        emit=lambda payload: None,
    )
    assert len(observed) == 2
    for threads, preserve_order in observed:
        assert int(threads) == offline_options.threads
        assert preserve_order in (False, "false")


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
