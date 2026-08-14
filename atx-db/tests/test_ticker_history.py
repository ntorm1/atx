"""Tests for the tbltickerhistory daily-bar loader transforms.

These exercise the pure DataFrame->DataFrame transforms (``_canonical_bars`` and
``_security_links``) plus the ``symbol_key`` helper for the broad-universe edge case
where a source row carries no usable ticker in either ``today_ticker`` or ``ticker_tk``.
The liquid40 sample never hit this (its symbols always have a ticker), but loading the
full ~7k-symbol archive surfaces rows with both ticker columns blank/NA.

No network: transforms run over in-memory fixtures.
"""
from __future__ import annotations

import datetime as dt

import pandas as pd
import pytest

from atx_db.ticker_history import (
    TickerHistoryDataset,
    TickerHistoryOptions,
    _apply_security_ids,
    _canonical_bars,
    _normalize_chunk,
    _security_links,
    disambiguate_vendor_collisions,
)
from atx_db.warehouse import insert_frame, symbol_key


def test_symbol_key_is_na_safe() -> None:
    # A key helper must never raise on missing input; it returns the empty key.
    assert symbol_key(None) == ""
    assert symbol_key(pd.NA) == ""
    assert symbol_key(float("nan")) == ""
    assert symbol_key("  aapl ") == "AAPL"


def _normalized_fixture() -> pd.DataFrame:
    """Mimic the post-``_normalize_chunk`` frame: empty tickers become NA strings."""
    frame = pd.DataFrame(
        {
            "security_id": ["SEC-A", "VENDOR-X"],
            "vendor_security_id": pd.array([101, 202], dtype="Int64"),
            "today_ticker": pd.array(["AAPL", pd.NA], dtype="string"),
            "ticker_tk": pd.array(["AAPL", pd.NA], dtype="string"),
            "trading_date": [dt.date(2012, 4, 2), dt.date(2012, 4, 2)],
            "open": [10.0, 5.0],
            "high": [11.0, 6.0],
            "low": [9.0, 4.0],
            "close": [10.5, 5.5],
            "close_pr": [10.5, 5.5],
            "volume": pd.array([1000, 2000], dtype="Int64"),
            "shares": pd.array([1000, 2000], dtype="Int64"),
            "return_factor": [1.0, 1.0],
        }
    )
    return frame


def test_canonical_bars_handles_blank_ticker_rows() -> None:
    options = TickerHistoryOptions(symbols=None, run_id="t")
    bars = _canonical_bars(_normalized_fixture(), options)
    # Both rows survive (the blank-ticker row keeps its security_id from the vendor id).
    assert len(bars) == 2
    symbols = set(bars["symbol"].tolist())
    assert "AAPL" in symbols
    # The blank-ticker row maps to an empty symbol, not a crash.
    assert "" in symbols
    aapl = bars.loc[bars["symbol"] == "AAPL"].iloc[0]
    assert aapl["shares_outstanding"] == 1000
    assert aapl["market_cap_usd"] == pytest.approx(10_500.0)


def test_price_projection_normalization_does_not_expand_vendor_width() -> None:
    raw = pd.DataFrame(
        {
            "tradingDate": ["2012-04-02"],
            "securityID": ["101"],
            "ticker_tk": ["AAPL"],
            "todayTicker": ["AAPL"],
            "open": ["10"],
            "high": ["11"],
            "low": ["9"],
            "close": ["10.5"],
            "closePr": ["10.5"],
            "volume": ["1000"],
            "shares": ["1000000"],
            "returnFactor": ["1"],
        }
    )

    normalized = _normalize_chunk(
        raw, TickerHistoryOptions(symbols=None, price_projection_only=True)
    )

    assert set(normalized.columns) == {
        "trading_date",
        "vendor_security_id",
        "ticker_tk",
        "today_ticker",
        "open",
        "high",
        "low",
        "close",
        "close_pr",
        "volume",
        "shares",
        "return_factor",
        "source",
        "run_id",
        "_symbol_for_mapping",
    }


def test_security_id_resolution_vectorizes_known_and_vendor_fallbacks(
    monkeypatch, tmp_store
) -> None:
    frame = pd.DataFrame(
        {
            "_symbol_for_mapping": ["AAPL", "NEW"],
            "vendor_security_id": pd.array([101, 202], dtype="Int64"),
        }
    )
    monkeypatch.setattr(
        "atx_db.ticker_history.security_ids_for_symbols",
        lambda _store, _symbols: {"AAPL": "SEC-AAPL"},
    )

    resolved = _apply_security_ids(
        tmp_store, frame, TickerHistoryOptions(symbols=None)
    )

    assert resolved["security_id"].tolist() == [
        "SEC-AAPL",
        "TBLTICKERHISTORY-202",
    ]


def test_price_projection_load_is_canonical_only(tmp_store) -> None:
    options = TickerHistoryOptions(
        symbols=None,
        price_projection_only=True,
        run_id="canonical-only-test",
    )
    TickerHistoryDataset()._load_chunk(
        tmp_store, _normalized_fixture().iloc[[0]].copy(), options
    )

    assert tmp_store.con.execute("SELECT count(*) FROM equity_daily_bars").fetchone()[0] == 1
    assert tmp_store.con.execute(
        """
        SELECT count(*) FROM information_schema.tables
        WHERE table_name='tbltickerhistory_daily'
        """
    ).fetchone()[0] == 0


def test_ticker_history_resume_bounds_are_validated() -> None:
    with pytest.raises(ValueError, match="max_chunks must exceed skip_chunks"):
        TickerHistoryOptions(skip_chunks=20, max_chunks=20)


def test_security_links_handles_blank_ticker_rows() -> None:
    options = TickerHistoryOptions(symbols=None, run_id="t")
    securities, identifiers, listings = _security_links(_normalized_fixture(), options)
    assert len(securities) == 2
    assert len(identifiers) == 2
    assert len(listings) == 2


def test_canonical_bars_drops_invalid_ohlcv() -> None:
    options = TickerHistoryOptions(symbols=None, run_id="t")
    frame = pd.DataFrame(
        {
            "security_id": ["A", "B", "C", "D"],
            "vendor_security_id": pd.array([1, 2, 3, 4], dtype="Int64"),
            "today_ticker": pd.array(["AAA", "BBB", "CCC", "DDD"], dtype="string"),
            "ticker_tk": pd.array(["AAA", "BBB", "CCC", "DDD"], dtype="string"),
            "trading_date": [dt.date(2012, 4, 2)] * 4,
            # A: valid. B: high < low (corrupt). C: non-positive low. D: negative volume.
            "open": [10.0, 10.0, 10.0, 10.0],
            "high": [11.0, 8.0, 11.0, 11.0],
            "low": [9.0, 9.0, 0.0, 9.0],
            "close": [10.5, 9.5, 5.0, 10.5],
            "close_pr": [10.5, 9.5, 5.0, 10.5],
            "volume": pd.array([1000, 1000, 1000, -5], dtype="Int64"),
            "return_factor": [1.0, 1.0, 1.0, 1.0],
        }
    )
    bars = _canonical_bars(frame, options)
    assert set(bars["security_id"]) == {"A"}


def _collision_bars() -> pd.DataFrame:
    """Two unrelated vendor securities (symbol 'ET') collapsed onto one canonical
    security_id, plus a clean single-vendor security."""
    rows = []
    src = "tbltickerhistory3_10y"
    # Primary issuer: vendor 111, 3 bars -> keeps the canonical id.
    for i, d in enumerate([dt.date(2012, 4, 2), dt.date(2012, 4, 3), dt.date(2012, 4, 4)]):
        rows.append(dict(source=src, security_id="SEC-CIK-X", vendor_security_id=111,
                         symbol="ET", trade_date=d, open=10.0, high=11.0, low=9.0,
                         close=10.0 + i, adjusted_close=10.0 + i, volume=1000,
                         vwap=None, dividend_amount=None, split_factor=1.0,
                         is_adjusted=False, available_at=None, run_id="t"))
    # Recycled-ticker ghost: vendor 222, 1 bar on an overlapping date -> re-keyed.
    rows.append(dict(source=src, security_id="SEC-CIK-X", vendor_security_id=222,
                     symbol="ET", trade_date=dt.date(2012, 4, 2), open=40.0, high=41.0,
                     low=39.0, close=40.0, adjusted_close=40.0, volume=2000,
                     vwap=None, dividend_amount=None, split_factor=1.0,
                     is_adjusted=False, available_at=None, run_id="t"))
    # Clean single-vendor security -> untouched.
    rows.append(dict(source=src, security_id="SEC-CIK-Y", vendor_security_id=999,
                     symbol="AAA", trade_date=dt.date(2012, 4, 2), open=5.0, high=6.0,
                     low=4.0, close=5.0, adjusted_close=5.0, volume=500,
                     vwap=None, dividend_amount=None, split_factor=1.0,
                     is_adjusted=False, available_at=None, run_id="t"))
    return pd.DataFrame(rows)


def test_disambiguate_vendor_collisions_splits_recycled_tickers(tmp_store) -> None:
    bars = _collision_bars()
    insert_frame(tmp_store, bars, "equity_daily_bars", "collision_bars_load")

    rekeyed = disambiguate_vendor_collisions(tmp_store)
    assert rekeyed == 1  # only vendor 222 is re-keyed

    con = tmp_store.con
    # No (security_id, trade_date) duplicates remain.
    dups = con.execute(
        "SELECT COUNT(*) FROM (SELECT security_id, trade_date FROM equity_daily_bars "
        "GROUP BY 1,2 HAVING COUNT(*) > 1)"
    ).fetchone()[0]
    assert dups == 0
    # Primary (vendor 111, most rows) keeps the canonical id.
    assert con.execute(
        "SELECT security_id FROM equity_daily_bars WHERE vendor_security_id = 111 LIMIT 1"
    ).fetchone()[0] == "SEC-CIK-X"
    # Ghost (vendor 222) re-keyed to its per-line synthetic id (vendor + symbol).
    assert con.execute(
        "SELECT security_id FROM equity_daily_bars WHERE vendor_security_id = 222 LIMIT 1"
    ).fetchone()[0] == "TBLTICKERHISTORY-222-ET"
    # Clean security untouched.
    assert con.execute(
        "SELECT security_id FROM equity_daily_bars WHERE vendor_security_id = 999 LIMIT 1"
    ).fetchone()[0] == "SEC-CIK-Y"
    # First-class security row materialized for the new id.
    assert con.execute(
        "SELECT COUNT(*) FROM securities WHERE security_id = 'TBLTICKERHISTORY-222-ET'"
    ).fetchone()[0] == 1
    # Idempotent: a second pass is a no-op.
    assert disambiguate_vendor_collisions(tmp_store) == 0


def _share_class_bars() -> pd.DataFrame:
    """One vendor id carrying two share-class symbols collapsed onto one id."""
    src = "tbltickerhistory3_10y"
    rows = []
    for sym, n in (("LMCA", 3), ("LMCAV", 1)):
        for i in range(n):
            rows.append(dict(source=src, security_id="TBLTICKERHISTORY-364255",
                             vendor_security_id=364255, symbol=sym,
                             trade_date=dt.date(2013, 1, 11 + i), open=100.0, high=101.0,
                             low=99.0, close=100.0 + i, adjusted_close=100.0 + i, volume=1000,
                             vwap=None, dividend_amount=None, split_factor=1.0,
                             is_adjusted=False, available_at=None, run_id="t"))
    return pd.DataFrame(rows)


def test_disambiguate_splits_share_classes(tmp_store) -> None:
    insert_frame(tmp_store, _share_class_bars(), "equity_daily_bars", "share_class_load")
    rekeyed = disambiguate_vendor_collisions(tmp_store)
    assert rekeyed == 1  # LMCAV (fewer bars) re-keyed; LMCA keeps the id
    con = tmp_store.con
    assert con.execute(
        "SELECT COUNT(*) FROM (SELECT security_id, trade_date FROM equity_daily_bars "
        "GROUP BY 1,2 HAVING COUNT(*) > 1)"
    ).fetchone()[0] == 0
    assert con.execute(
        "SELECT DISTINCT security_id FROM equity_daily_bars WHERE symbol = 'LMCAV'"
    ).fetchone()[0] == "TBLTICKERHISTORY-364255-LMCAV"
    assert con.execute(
        "SELECT DISTINCT security_id FROM equity_daily_bars WHERE symbol = 'LMCA'"
    ).fetchone()[0] == "TBLTICKERHISTORY-364255"
