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

# The full tbltickerhistory3 source contract (ticker_history_quality._validate_source_columns):
# dn is the vendor's per-security trading-day sequence, closeUnadjPr the prior unadjusted close.
_HEADER = (
    "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\tclosePr\tvolume\tshares"
    "\treturnFactor\tcumulReturnFactor\tdn\tcloseUnadjPr\ttotalReturn"
)
_MEMBER = "tbltickerhistory3_10y.txt"
_SYMBOLS = ("AAA", "BBB", "CCC")
_DATES = ("2024-01-02", "2024-01-03", "2024-01-04")


def _rows() -> list[str]:
    rows: list[str] = []
    for index, symbol in enumerate(_SYMBOLS, start=1):
        for dn, day in enumerate(_DATES, start=1):
            base = 10.0 * index
            # Flat close, no distributions: daily and cumulative factors 1, total return 0.
            rows.append(
                f"{day}\t{32950 + index}\t{symbol}\t{symbol}\t{base}\t{base + 0.5}\t"
                f"{base - 0.1}\t{base + 0.2}\t{base + 0.2}\t{1000 * index}\t{1_000_000 * index}\t1.0"
                f"\t1.0\t{dn}\t{base + 0.2}\t0.0"
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

    from atx_db.clock import utc_today

    # First acquisition happens now, so the cutoff must not be a past day (receipt guard).
    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "downloader": fake_downloader,
           "as_of_date": utc_today()}
    )
    result = stage_security_master(tmp_store, options)
    assert calls == ["https://www.sec.gov/files/company_tickers.json"]
    assert result.rows == 1
    row = tmp_store.con.execute("SELECT count(*) FROM sec_company_tickers").fetchone()
    assert row is not None and row[0] == 1
    sha_row = tmp_store.con.execute(
        "SELECT sha256 FROM raw_source_files WHERE dataset_id = 'sec_security_master'"
    ).fetchone()
    assert sha_row is not None and sha_row[0] and len(sha_row[0]) == 64


_COMPANY_TICKERS_JSON = '{"0":{"cik_str":320193,"ticker":"AAPL","title":"Apple Inc."}}'


def _retained_company_tickers(tmp_path: Path, received_utc: dt.datetime) -> Path:
    """A retained company_tickers.json with no cache receipt, so its mtime is its receipt."""
    import calendar
    import os

    path = tmp_path / "cache" / "company_tickers.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(_COMPANY_TICKERS_JSON, encoding="utf-8")
    stamp = calendar.timegm(received_utc.timetuple())
    os.utime(path, (stamp, stamp))
    return path


def _refusing_downloader(url: str, dest: Path, *, user_agent: str) -> int:
    raise AssertionError(f"a retained snapshot must load without a request: {url}")


def test_security_master_file_received_after_the_cutoff_day_fails_loudly(tmp_store, tmp_path, three_symbol_zip):
    """A retained company_tickers.json received after the cutoff never loads (available_at > cutoff)."""
    from atx_db.activation import stage_security_master
    from atx_db.symbol_directory import SnapshotAfterCutoffError

    _retained_company_tickers(tmp_path, dt.datetime(2024, 1, 5, 3, 0))  # cutoff day is 2024-01-04
    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "downloader": _refusing_downloader}
    )
    with pytest.raises(SnapshotAfterCutoffError, match=r"2024-01-05T03:00:00 .*file_mtime.*cutoff day 2024-01-04"):
        stage_security_master(tmp_store, options)
    written = tmp_store.con.execute(
        "SELECT (SELECT count(*) FROM sec_company_tickers), "
        "(SELECT count(*) FROM security_identifier_history WHERE source = 'SEC company_tickers'), "
        "(SELECT count(*) FROM exchange_listings WHERE source = 'SEC company_tickers')"
    ).fetchone()
    assert written == (0, 0, 0)


def test_security_master_rows_are_available_at_the_files_receipt_not_the_load(tmp_store, tmp_path, three_symbol_zip):
    from atx_db.activation import stage_security_master

    received = dt.datetime(2024, 1, 4, 1, 30)  # inside the 2024-01-04 cutoff day
    _retained_company_tickers(tmp_path, received)
    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "downloader": _refusing_downloader}
    )
    result = stage_security_master(tmp_store, options)
    assert result.rows == 1
    assert result.detail["acquisition"] == "retained" and result.detail["receipt_basis"] == "file_mtime"
    clocks = tmp_store.con.execute(
        "SELECT min(available_at), max(available_at), min(as_of_date), max(as_of_date) "
        "FROM security_identifier_history WHERE source = 'SEC company_tickers'"
    ).fetchone()
    assert clocks == (received, received, dt.date(2024, 1, 4), dt.date(2024, 1, 4))
    listing = tmp_store.con.execute(
        "SELECT max(available_at) FROM exchange_listings WHERE source = 'SEC company_tickers'"
    ).fetchone()
    assert listing == (received,)


_NASDAQ_LISTED_TXT = (
    "Symbol|Security Name|Market Category|Test Issue|Financial Status|Round Lot Size|ETF|NextShares\n"
    "AAA|AAA Corp|Q|N|N|100|N|N\n"
)
_OTHER_LISTED_TXT = (
    "ACT Symbol|Security Name|Exchange|CQS Symbol|ETF|Round Lot Size|Test Issue|NASDAQ Symbol\n"
    "BBB|BBB Corp|N|BBB|N|100|N|BBB\n"
)


def test_symbol_directory_stage_records_source_files_with_sha256(tmp_store, tmp_path, three_symbol_zip):
    from atx_db.activation import stage_symbol_directory

    payloads = {
        "https://www.nasdaqtrader.com/dynamic/SymDir/nasdaqlisted.txt": _NASDAQ_LISTED_TXT,
        "https://www.nasdaqtrader.com/dynamic/SymDir/otherlisted.txt": _OTHER_LISTED_TXT,
    }

    def fake_downloader(url: str, dest: Path, *, user_agent: str) -> int:
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_text(payloads[url], encoding="utf-8")
        return dest.stat().st_size

    from atx_db.clock import utc_today

    # First acquisition happens now, so the cutoff must not be a past day (A1 receipt guard).
    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "downloader": fake_downloader,
           "as_of_date": utc_today()}
    )
    result = stage_symbol_directory(tmp_store, options)
    assert result.rows == 2
    rows = tmp_store.con.execute(
        "SELECT source_url, sha256, byte_count FROM raw_source_files "
        "WHERE dataset_id = 'nasdaq_symbol_directory' ORDER BY source_url"
    ).fetchall()
    assert len(rows) == 2
    for source_url, sha256, byte_count in rows:
        assert sha256 is not None and len(sha256) == 64
        assert byte_count is not None and byte_count > 0
        assert source_url in payloads


def test_stages_fail_fast_without_a_sec_user_agent(tmp_store, tmp_path, three_symbol_zip):
    from atx_db.activation import stage_security_master

    options = ActivationOptions(
        **{**_options(tmp_path, three_symbol_zip).as_dict(), "sec_user_agent": None}
    )
    with pytest.raises(ValueError, match="ATX_SEC_USER_AGENT"):
        stage_security_master(tmp_store, options)
