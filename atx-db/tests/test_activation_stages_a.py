"""Activation stages: migrate, identity load, ticker-history extract and publish."""

from __future__ import annotations

import datetime as dt
import zipfile
from pathlib import Path

import pytest

from atx_db.activation import (
    STAGES,
    ActivationOptions,
    stage_migrate,
    stage_ticker_history_extract,
    stage_ticker_history_publish,
)

_HEADER = "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\tclosePr\tvolume\tshares\treturnFactor"
_MEMBER = "tbltickerhistory3_10y.txt"
_SYMBOLS = ("AAA", "BBB", "CCC")
_DATES = ("2024-01-02", "2024-01-03", "2024-01-04")


def _rows() -> list[str]:
    rows: list[str] = []
    for index, symbol in enumerate(_SYMBOLS, start=1):
        for day in _DATES:
            base = 10.0 * index
            rows.append(
                f"{day}\t{32950 + index}\t{symbol}\t{symbol}\t{base}\t{base + 0.5}\t"
                f"{base - 0.1}\t{base + 0.2}\t{base + 0.2}\t{1000 * index}\t{1_000_000 * index}\t1.0"
            )
    return rows


@pytest.fixture
def three_symbol_zip(tmp_path: Path) -> Path:
    zip_path = tmp_path / "tbltickerhistory3_10y.zip"
    payload = ("\r\n".join([_HEADER, *_rows()]) + "\r\n").encode("utf-8")
    with zipfile.ZipFile(zip_path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr(_MEMBER, payload)
    return zip_path


def _options(tmp_path: Path, zip_path: Path) -> ActivationOptions:
    return ActivationOptions(
        db_path=tmp_path / "warehouse.duckdb",
        as_of_date=dt.date(2024, 1, 4),
        ticker_history_zip=zip_path,
        staging_dir=tmp_path / "staging",
        cache_dir=tmp_path / "cache",
        sec_user_agent="atx-db test agent test@example.com",
        ticker_history_expected_bytes=None,
        minimum_rows=1,
        minimum_securities=1,
        minimum_latest_date_securities=1,
        run_id="test-run",
    )


def test_every_stage_name_has_a_registered_function():
    from atx_db.activation import STAGE_ORDER

    assert set(STAGES) >= {
        "migrate",
        "security_master",
        "symbol_directory",
        "ticker_history_extract",
        "ticker_history_publish",
    }
    assert set(STAGES).issubset(set(STAGE_ORDER))


def test_migrate_stage_reports_head_schema_and_applies_nothing_on_a_current_db(tmp_store, tmp_path, three_symbol_zip):
    from atx_db.migrations import MIGRATIONS

    result = stage_migrate(tmp_store, _options(tmp_path, three_symbol_zip))
    assert result.rows == 0
    assert result.detail["schema_version"] == max(m.version for m in MIGRATIONS)
    assert result.detail["applied_versions"] == []


def test_extract_stage_writes_the_staging_tsv(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    result = stage_ticker_history_extract(tmp_store, options)
    tsv_path = Path(str(result.detail["tsv_path"]))
    assert tsv_path.is_file()
    assert result.detail["skipped"] is False
    assert len(str(result.detail["sha256"])) == 64
    assert result.rows == result.detail["written_bytes"]


def test_extract_stage_is_idempotent(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    stage_ticker_history_extract(tmp_store, options)
    second = stage_ticker_history_extract(tmp_store, options)
    assert second.detail["skipped"] is True


def test_publish_stage_loads_bars_for_every_symbol(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    stage_ticker_history_extract(tmp_store, options)
    result = stage_ticker_history_publish(tmp_store, options)
    assert result.rows == len(_SYMBOLS) * len(_DATES)
    assert result.detail["securities"] == len(_SYMBOLS)
    row = tmp_store.con.execute(
        "SELECT count(*), count(DISTINCT symbol), max(trade_date) FROM equity_daily_bars"
    ).fetchone()
    assert row == (9, 3, dt.date(2024, 1, 4))


def test_publish_stage_is_idempotent_on_row_count(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    stage_ticker_history_extract(tmp_store, options)
    stage_ticker_history_publish(tmp_store, options)
    stage_ticker_history_publish(tmp_store, options)
    row = tmp_store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()
    assert row is not None and row[0] == 9


def test_publish_stage_breadth_floors_come_from_activation_options(tmp_store, tmp_path, three_symbol_zip):
    options = _options(tmp_path, three_symbol_zip)
    stage_ticker_history_extract(tmp_store, options)
    strict = ActivationOptions(**{**options.as_dict(), "minimum_securities": 10_000})
    with pytest.raises(RuntimeError, match="publication gate failed"):
        stage_ticker_history_publish(tmp_store, strict)


def test_default_breadth_floors_match_the_bulk_loader_defaults():
    from atx_db.ticker_history_bulk import BulkTickerHistoryOptions

    defaults = BulkTickerHistoryOptions(tsv_path=Path("unused.tsv"))
    options = ActivationOptions()
    assert options.minimum_rows == defaults.minimum_rows
    assert options.minimum_securities == defaults.minimum_securities
    assert options.minimum_latest_date_securities == defaults.minimum_latest_date_securities


def test_security_master_stage_uses_the_injected_downloader(tmp_store, tmp_path, three_symbol_zip, monkeypatch):
    from atx_db.activation import stage_security_master

    calls: list[str] = []

    def fake_downloader(url: str, dest: Path, *, user_agent: str) -> int:
        calls.append(url)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(
            '{"0":{"cik_str":320193,"ticker":"AAPL","title":"Apple Inc."}}', encoding="utf-8"
        )
        return dest.stat().st_size

    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "downloader": fake_downloader}
    )
    result = stage_security_master(tmp_store, options)
    assert calls == ["https://www.sec.gov/files/company_tickers.json"]
    assert result.rows == 1
    row = tmp_store.con.execute("SELECT count(*) FROM sec_company_tickers").fetchone()
    assert row is not None and row[0] == 1


def test_stages_fail_fast_without_a_sec_user_agent(tmp_store, tmp_path, three_symbol_zip):
    from atx_db.activation import stage_security_master

    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "sec_user_agent": None}
    )
    with pytest.raises(ValueError, match="ATX_SEC_USER_AGENT"):
        stage_security_master(tmp_store, options)
