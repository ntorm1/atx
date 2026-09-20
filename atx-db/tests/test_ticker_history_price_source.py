from __future__ import annotations

import csv
import zipfile

import duckdb
import pandas as pd
import pytest

from atx_db.ticker_history import (
    SOURCE_COLUMNS,
    TickerHistoryDataset,
    TickerHistoryOptions,
    _apply_security_ids,
    _canonical_bars,
    _normalize_chunk,
)
from atx_db.ticker_history_bulk import BulkTickerHistoryOptions, publish_bulk_ticker_history
from atx_db.ticker_history_quality import source_diagnostics, stage_source


def _row(date, close, factor, *, prior, daily=1, total=0, dn=1, vendor=101, ticker="OLD"):
    row = dict.fromkeys(SOURCE_COLUMNS, "")
    row.update(tradingDate=date, securityID=str(vendor), ticker_tk=ticker, todayTicker="NEW",
               open=str(close), high=str(close + 1), low=str(close - 1), close=str(close),
               closePr=str(prior * daily), closeUnadjPr=str(prior), volume="1000", shares="10000",
               returnFactor=str(daily), totalReturn=str(total), cumulReturnFactor=str(factor), dn=str(dn))
    return row


def _write(path, rows):
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=SOURCE_COLUMNS, delimiter="\t")
        writer.writeheader()
        writer.writerows(rows)


@pytest.mark.parametrize(("before", "after", "factor_before", "factor_after", "daily", "expected"), [
    (100, 110, 1, 1, 1, 0.1),
    (700, 110, 1 / 7, 1, 1 / 7, 0.1),
    (100, 55, 0.5, 1, 0.5, 0.1),
    (100, 99, 0.98, 1, 0.98, 1 / 98),
])
def test_current_adjusted_price_and_plain_bulk_parity(
    tmp_store, tmp_path, before, after, factor_before, factor_after, daily, expected,
):
    rows = [_row("2025-01-02", before, factor_before, prior=90),
            _row("2025-01-03", after, factor_after, prior=before, daily=daily,
                 total=expected, dn=2, ticker="NEW")]
    options = TickerHistoryOptions(symbols=None)
    normalized = _apply_security_ids(tmp_store, _normalize_chunk(pd.DataFrame(rows), options), options)
    dataset = TickerHistoryDataset()
    dataset.ensure_schema(tmp_store)
    dataset._load_chunk(tmp_store, normalized, options)
    query = ("SELECT security_id, symbol, close, adjusted_close, volume, shares_outstanding, split_factor "
             "FROM equity_daily_bars ORDER BY trade_date")
    plain = tmp_store.con.execute(query).fetchall()
    assert plain[1][3] / plain[0][3] - 1 == pytest.approx(expected)
    assert [r[1] for r in plain] == ["OLD", "NEW"]
    assert {r[0] for r in plain} == {"TBLTICKERHISTORY-101"}
    assert all(r[4:] == (1000, 10000, None) for r in plain)
    factors = tmp_store.con.execute(
        "SELECT return_factor FROM tbltickerhistory_daily ORDER BY trading_date"
    ).fetchall()
    assert [r[0] for r in factors] == pytest.approx([1, daily])
    tsv = tmp_path / "source.tsv"
    _write(tsv, rows)
    result = publish_bulk_ticker_history(tmp_store, BulkTickerHistoryOptions(
        tsv_path=tsv, memory_limit="1GB", threads=1, minimum_rows=2,
        minimum_securities=1, minimum_latest_date_securities=1,
    ))
    bulk = tmp_store.con.execute(query).fetchall()
    for actual, reference in zip(bulk, plain, strict=True):
        assert actual[:2] == reference[:2]
        assert actual[2:4] == pytest.approx(reference[2:4])
        assert actual[4:] == reference[4:]
    assert result.source_diagnostics["adjusted_return_residual_gt_1e_8"] == 0
    assert result.provenance["historical_source_vintage"] == "unknown"
    assert "modeled" in result.provenance["availability_policy"]


@pytest.mark.parametrize("factor", ["", "NaN", "inf", "-inf", "0", "-1", "1e308"])
def test_invalid_factor_never_falls_back_to_prior_or_raw_close(tmp_store, tmp_path, factor):
    rows = [_row("2025-01-02", 100, factor, prior=90)]
    options = TickerHistoryOptions(symbols=None, price_projection_only=True)
    normalized = _apply_security_ids(tmp_store, _normalize_chunk(pd.DataFrame(rows), options), options)
    bars = _canonical_bars(normalized, options)
    assert bars["adjusted_close"].isna().all()
    assert bars["close"].tolist() == [100]
    tsv = tmp_path / "bad-factor.tsv"
    _write(tsv, rows)
    result = publish_bulk_ticker_history(tmp_store, BulkTickerHistoryOptions(
        tsv_path=tsv, memory_limit="1GB", threads=1, minimum_rows=1,
        minimum_securities=1, minimum_latest_date_securities=1,
    ))
    assert tmp_store.con.execute("SELECT close, adjusted_close FROM equity_daily_bars").fetchone() == (100, None)
    assert result.source_diagnostics["invalid_adjusted_product_rows"] == 1


def test_original_duplicate_or_missing_dn_prevents_false_adjacency(tmp_path):
    rows = [_row("2025-01-02", 100, 1, prior=90, dn=1),
            _row("2025-01-03", 110, 1, prior=100, dn=2),
            _row("2025-01-03", 120, 1, prior=100, dn=2),
            _row("2025-01-06", 121, 1, prior=120, dn=3),
            _row("2025-01-08", 122, 1, prior=121, dn=5),
            _row("2025-01-08", 122, 1, prior=121, dn=5, vendor=0)]
    tsv = tmp_path / "conflicts.tsv"
    _write(tsv, rows)
    with duckdb.connect() as con:
        stage_source(con, str(tsv))
        counts = source_diagnostics(con)
    assert counts["source_rows"] == 6
    assert counts["quarantined_positive_key_rows"] == 2
    assert counts["zero_id_rows"] == 1
    assert counts["adjacent_unique_pairs"] == 0
    assert counts["adjusted_return_comparable"] == 0


def test_plain_quarantine_covers_chunk_boundary_and_retains_raw(tmp_store, tmp_path):
    rows = [_row("2025-01-02", 100, 1, prior=90),
            _row("2025-01-02", 101, 1, prior=90),
            _row("2025-01-03", 110, 1, prior=100, dn=2)]
    tsv = tmp_path / "source.tsv"
    _write(tsv, rows)
    archive = tmp_path / "source.zip"
    with zipfile.ZipFile(archive, "w") as handle:
        handle.write(tsv, "source.tsv")
    dataset = TickerHistoryDataset()
    dataset.ensure_schema(tmp_store)
    result = dataset.load(tmp_store, TickerHistoryOptions(zip_path=archive, symbols=None, chunk_size=1))
    assert result.details["source_diagnostics"]["quarantined_positive_key_rows"] == 2
    assert tmp_store.con.execute("SELECT count(*) FROM tbltickerhistory_daily").fetchone() == (3,)
    assert tmp_store.con.execute("SELECT close FROM equity_daily_bars").fetchall() == [(110,)]
