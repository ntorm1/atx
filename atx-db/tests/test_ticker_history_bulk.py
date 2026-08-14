from __future__ import annotations

import pytest

from atx_db.ticker_history_bulk import BulkTickerHistoryOptions, publish_bulk_ticker_history

HEADER = (
    "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\t"
    "closePr\tvolume\tshares\treturnFactor\n"
)


def test_bulk_publication_is_atomic_deduplicated_and_collision_safe(tmp_store, tmp_path):
    archive = tmp_path / "bars.tsv"
    archive.write_text(
        HEADER
        + "2025-01-02\t10\tAAA\tAAA\t10\t11\t9\t10\t10\t100\t1000\t1\n"
        + "2025-01-02\t10\tAAA\tAAA\t10\t11\t9\t10\t10\t200\t1000\t1\n"
        + "2025-01-03\t10\tAAA\tAAA\t11\t12\t10\t11\t11\t150\t1000\t1\n"
        + "2025-01-03\t20\tAAA\tAAA\t20\t21\t19\t20\t20\t50\t1000\t1\n"
        + "2025-01-03\t30\tBAD\tBAD\t20\t19\t18\t20\t20\t50\t1000\t1\n",
        encoding="utf-8",
    )
    tmp_store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, close, is_adjusted
        ) VALUES ('other', 'OTHER-1', 'OTH', DATE '2025-01-02', 1, false)
        """
    )
    tmp_store.con.execute(
        """
        INSERT INTO sec_company_tickers (cik, ticker, title, security_id)
        VALUES ('0000000001', 'AAA', 'AAA Inc', 'SEC-CIK-0000000001')
        """
    )
    result = publish_bulk_ticker_history(
        tmp_store,
        BulkTickerHistoryOptions(
            tsv_path=archive,
            memory_limit="1GB",
            threads=1,
            minimum_rows=3,
            minimum_securities=2,
            minimum_latest_date_securities=2,
            run_id="bulk-test",
        ),
    )

    assert result.rows == 3
    assert result.securities == 2
    assert result.latest_date_securities == 2
    assert result.duplicate_keys == 0
    assert result.invalid_rows == 0
    assert tmp_store.con.execute(
        "SELECT volume FROM equity_daily_bars WHERE vendor_security_id = '10' AND trade_date = DATE '2025-01-02'"
    ).fetchone() == (200,)
    security_ids = tmp_store.con.execute(
        "SELECT DISTINCT security_id FROM equity_daily_bars WHERE source = 'tbltickerhistory3_10y' ORDER BY 1"
    ).fetchall()
    assert security_ids == [("SEC-CIK-0000000001",), ("TBLTICKERHISTORY-20-AAA",)]
    assert tmp_store.con.execute(
        "SELECT count(*) FROM equity_daily_bars WHERE source = 'other'"
    ).fetchone() == (1,)
    assert tmp_store.con.execute(
        "SELECT status, rows_loaded FROM dataset_runs WHERE run_id = 'bulk-test'"
    ).fetchone() == ("succeeded", 3)


def test_bulk_publication_gate_preserves_live_table(tmp_store, tmp_path):
    archive = tmp_path / "too-small.tsv"
    archive.write_text(
        HEADER + "2025-01-02\t10\tAAA\tAAA\t10\t11\t9\t10\t10\t100\t1000\t1\n",
        encoding="utf-8",
    )
    tmp_store.con.execute(
        """
        INSERT INTO equity_daily_bars (
            source, security_id, symbol, trade_date, close, is_adjusted
        ) VALUES ('tbltickerhistory3_10y', 'LIVE-1', 'LIVE', DATE '2024-01-02', 1, false)
        """
    )
    with pytest.raises(RuntimeError, match="publication gate failed"):
        publish_bulk_ticker_history(
            tmp_store,
            BulkTickerHistoryOptions(
                tsv_path=archive,
                memory_limit="1GB",
                threads=1,
                minimum_rows=2,
                minimum_securities=1,
                minimum_latest_date_securities=1,
                run_id="bulk-failed-test",
            ),
        )
    assert tmp_store.con.execute(
        "SELECT security_id FROM equity_daily_bars WHERE source = 'tbltickerhistory3_10y'"
    ).fetchall() == [("LIVE-1",)]
    assert tmp_store.con.execute(
        "SELECT status FROM dataset_runs WHERE run_id = 'bulk-failed-test'"
    ).fetchone() == ("failed",)
