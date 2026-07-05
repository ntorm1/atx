from __future__ import annotations

import datetime as dt
import zipfile

import pandas as pd

from db.backfill import run_backfill
from db.pricing_bulk import (
    BulkBarsBackfillDataset,
    BulkBarsDataset,
    BulkBarsOptions,
    _normalize_chunk,
)
from db.warehouse import insert_frame


def _seed_symbol(tmp_store, symbol: str, security_id: str) -> None:
    securities = pd.DataFrame(
        [
            {
                "security_id": security_id,
                "entity_id": f"ENTITY-{security_id}",
                "issuer_id": security_id,
                "primary_symbol": symbol,
                "name": symbol,
                "asset_class": "EQUITY",
                "country": "US",
                "currency": "USD",
                "active": True,
                "first_seen_date": dt.date(2010, 1, 1),
                "last_seen_date": pd.NaT,
                "source": "fixture",
            }
        ]
    )
    tickers = pd.DataFrame(
        [{"cik": "0000000001", "ticker": symbol, "title": symbol, "security_id": security_id}]
    )
    insert_frame(tmp_store, securities, "securities", "pricing_bulk_seed_securities")
    insert_frame(tmp_store, tickers, "sec_company_tickers", "pricing_bulk_seed_tickers")


def _write_csv(path, rows: list[dict[str, object]]) -> None:
    pd.DataFrame(rows).to_csv(path, index=False)


def test_bulk_bars_csv_loads_modern_bars_idempotently_and_records_pit(tmp_store, tmp_path) -> None:
    _seed_symbol(tmp_store, "AAPL", "SEC-AAPL")
    source = tmp_path / "bars.csv"
    _write_csv(
        source,
        [
            {
                "ticker": "AAPL",
                "date": "2014-12-31",
                "open": 1,
                "high": 2,
                "low": 1,
                "close": 2,
                "adjusted_close": 2,
                "volume": 100,
                "vendor_security_id": "AAPL-LINE",
            },
            {
                "ticker": "AAPL",
                "date": "2016-01-04",
                "open": 100,
                "high": 110,
                "low": 90,
                "close": 105,
                "adjusted_close": 104,
                "volume": 1000,
                "vendor_security_id": "AAPL-LINE",
            },
            {
                "ticker": "MSFT",
                "date": "2016-01-04",
                "open": 50,
                "high": 55,
                "low": 49,
                "close": 54,
                "volume": 2000,
                "available_at": "2016-01-05 08:30:00",
                "vendor_security_id": "MSFT-LINE",
            },
        ],
    )

    options = BulkBarsOptions(source_file=source, run_id="run-1")
    result = BulkBarsDataset().load(tmp_store, options)
    assert result.rows_loaded == 2

    rows = tmp_store.con.execute(
        """
        SELECT symbol, security_id, trade_date, close, adjusted_close, volume, available_at, run_id
        FROM equity_daily_bars
        WHERE source = 'bulk_bars_2015plus'
        ORDER BY symbol
        """
    ).fetchall()
    assert rows == [
        ("AAPL", "SEC-AAPL", dt.date(2016, 1, 4), 105.0, 104.0, 1000, dt.datetime(2016, 1, 4, 22), "run-1"),
        (
            "MSFT",
            "BULK-BARS-2015PLUS-MSFT-LINE",
            dt.date(2016, 1, 4),
            54.0,
            54.0,
            2000,
            dt.datetime(2016, 1, 5, 8, 30),
            "run-1",
        ),
    ]
    assert tmp_store.con.execute("SELECT count(*) FROM raw_source_files WHERE dataset_id = 'bulk_daily_bars'").fetchone()[
        0
    ] == 1
    assert tmp_store.con.execute("SELECT sha256 IS NOT NULL FROM raw_source_files WHERE dataset_id = 'bulk_daily_bars'").fetchone()[
        0
    ]

    BulkBarsDataset().load(tmp_store, options)
    assert tmp_store.con.execute(
        "SELECT count(*) FROM equity_daily_bars WHERE source = 'bulk_bars_2015plus'"
    ).fetchone()[0] == 2


def test_bulk_bars_zip_loads_csv_members(tmp_store, tmp_path) -> None:
    source = tmp_path / "bars.zip"
    rows = pd.DataFrame(
        [
            {
                "symbol": "IBM",
                "trade_date": "2017-02-03",
                "open": 10,
                "high": 12,
                "low": 9,
                "close": 11,
                "volume": 10,
            }
        ]
    ).to_csv(index=False)
    with zipfile.ZipFile(source, "w") as archive:
        archive.writestr("daily/bars.csv", rows)

    result = BulkBarsDataset().load(tmp_store, BulkBarsOptions(source_zip=source, run_id="zip-run"))

    assert result.rows_loaded == 1
    assert result.details["members"] == ["daily/bars.csv"]
    assert tmp_store.con.execute(
        "SELECT symbol, trade_date, available_at FROM equity_daily_bars WHERE source = 'bulk_bars_2015plus'"
    ).fetchall() == [("IBM", dt.date(2017, 2, 3), dt.datetime(2017, 2, 3, 22))]


def test_bulk_bars_namespaces_unresolved_input_security_id(tmp_store, tmp_path) -> None:
    source = tmp_path / "raw-security-id.csv"
    _write_csv(
        source,
        [
            {
                "symbol": "RAWID",
                "trade_date": "2019-04-05",
                "open": 10,
                "high": 11,
                "low": 9,
                "close": 10,
                "volume": 100,
                "security_id": "12345",
            }
        ],
    )

    BulkBarsDataset().load(tmp_store, BulkBarsOptions(source_file=source))

    assert tmp_store.con.execute(
        "SELECT security_id FROM equity_daily_bars WHERE symbol = 'RAWID'"
    ).fetchone()[0] == "BULK-BARS-2015PLUS-12345"


def test_bulk_bars_accepts_existing_input_security_id(tmp_store, tmp_path) -> None:
    source = tmp_path / "existing-security-id.csv"
    _seed_symbol(tmp_store, "OTHER", "SEC-EXISTING")
    _write_csv(
        source,
        [
            {
                "symbol": "UNKNOWN",
                "trade_date": "2019-04-05",
                "open": 10,
                "high": 11,
                "low": 9,
                "close": 10,
                "volume": 100,
                "security_id": "SEC-EXISTING",
            }
        ],
    )

    BulkBarsDataset().load(tmp_store, BulkBarsOptions(source_file=source))

    assert tmp_store.con.execute(
        "SELECT security_id FROM equity_daily_bars WHERE symbol = 'UNKNOWN'"
    ).fetchone()[0] == "SEC-EXISTING"


def test_bulk_bars_clamps_early_available_at_to_close_knowledge_time(tmp_store, tmp_path) -> None:
    source = tmp_path / "early-available.csv"
    _write_csv(
        source,
        [
            {
                "symbol": "PIT",
                "trade_date": "2021-03-04",
                "open": 10,
                "high": 11,
                "low": 9,
                "close": 10,
                "volume": 100,
                "available_at": "2021-03-04 09:30:00",
            }
        ],
    )

    BulkBarsDataset().load(tmp_store, BulkBarsOptions(source_file=source))

    assert tmp_store.con.execute(
        "SELECT available_at FROM equity_daily_bars WHERE symbol = 'PIT'"
    ).fetchone()[0] == dt.datetime(2021, 3, 4, 22)


def test_bulk_bars_reuses_vendor_collision_repair(tmp_store, tmp_path) -> None:
    _seed_symbol(tmp_store, "ET", "SEC-ET")
    source = tmp_path / "et.csv"
    _write_csv(
        source,
        [
            {"symbol": "ET", "date": "2018-01-02", "open": 10, "high": 11, "low": 9, "close": 10, "volume": 1000, "vendor_security_id": 111},
            {"symbol": "ET", "date": "2018-01-03", "open": 11, "high": 12, "low": 10, "close": 11, "volume": 1000, "vendor_security_id": 111},
            {"symbol": "ET", "date": "2018-01-02", "open": 40, "high": 41, "low": 39, "close": 40, "volume": 2000, "vendor_security_id": 222},
        ],
    )

    result = BulkBarsDataset().load(tmp_store, BulkBarsOptions(source_file=source))

    assert result.details["vendor_collisions_rekeyed"] == 1
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM (
            SELECT security_id, trade_date
            FROM equity_daily_bars
            WHERE source = 'bulk_bars_2015plus'
            GROUP BY 1, 2
            HAVING count(*) > 1
        )
        """
    ).fetchone()[0] == 0
    assert tmp_store.con.execute(
        "SELECT security_id FROM equity_daily_bars WHERE vendor_security_id = '222'"
    ).fetchone()[0] == "BULK-BARS-2015PLUS-222-ET"


def test_bulk_bars_links_only_surviving_ohlcv_rows(tmp_store, tmp_path) -> None:
    source = tmp_path / "invalid-links.csv"
    _write_csv(
        source,
        [
            {
                "symbol": "GOOD",
                "trade_date": "2020-01-02",
                "open": 10,
                "high": 11,
                "low": 9,
                "close": 10,
                "volume": 100,
            },
            {
                "symbol": "BAD",
                "trade_date": "2020-01-02",
                "open": 0,
                "high": 11,
                "low": 9,
                "close": 10,
                "volume": 100,
            },
        ],
    )

    result = BulkBarsDataset().load(tmp_store, BulkBarsOptions(source_file=source))

    assert result.rows_loaded == 1
    assert tmp_store.con.execute(
        "SELECT symbol FROM equity_daily_bars WHERE source = 'bulk_bars_2015plus'"
    ).fetchall() == [("GOOD",)]
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM securities
        WHERE source = 'bulk_bars_2015plus'
          AND (primary_symbol = 'BAD' OR security_id = 'BULK-BARS-2015PLUS-SYMBOL-BAD')
        """
    ).fetchone()[0] == 0
    assert tmp_store.con.execute(
        """
        SELECT count(*)
        FROM exchange_listings
        WHERE source = 'bulk_bars_2015plus'
          AND ticker = 'BAD'
        """
    ).fetchone()[0] == 0


def test_bulk_bars_supports_common_aliases_and_never_uses_network(tmp_store, tmp_path, monkeypatch) -> None:
    def fail_network(*args, **kwargs):
        raise AssertionError("network should not be used by BulkBarsDataset")

    monkeypatch.setattr("requests.sessions.Session.request", fail_network)
    source = tmp_path / "aliases.csv"
    _write_csv(
        source,
        [
            {
                "Ticker": "ZZZ",
                "Price Date": "2020-06-01",
                "Open Price": 7,
                "High Price": 8,
                "Low Price": 6,
                "Close Price": 7.5,
                "AdjClose": 7.4,
                "Vol": 123,
            }
        ],
    )

    BulkBarsDataset().load(tmp_store, BulkBarsOptions(source_file=source))

    assert tmp_store.con.execute(
        """
        SELECT symbol, close, adjusted_close, volume, security_id
        FROM equity_daily_bars
        WHERE source = 'bulk_bars_2015plus'
        """
    ).fetchall() == [("ZZZ", 7.5, 7.4, 123, "BULK-BARS-2015PLUS-SYMBOL-ZZZ")]


def test_normalize_chunk_requires_offline_ohlcv_columns() -> None:
    frame = pd.DataFrame([{"symbol": "AAPL", "date": "2016-01-04", "close": 1}])
    try:
        _normalize_chunk(frame, BulkBarsOptions(source_file="unused.csv"))
    except ValueError as exc:
        assert "bulk bars missing required columns" in str(exc)
    else:
        raise AssertionError("missing OHLCV columns should fail")


def test_bulk_bars_backfill_migration_catalogs_partition_surface(tmp_store) -> None:
    columns = {
        row[0]
        for row in tmp_store.con.execute(
            """
            SELECT column_name
            FROM information_schema.columns
            WHERE table_schema = 'main'
              AND table_name = 'price_backfill_partition'
            """
        ).fetchall()
    }
    assert {
        "dataset_id",
        "partition_key",
        "window_lo",
        "window_hi",
        "status",
        "rows_loaded",
        "watermark_after",
        "source_file_sha256",
        "details_json",
    }.issubset(columns)
    assert tmp_store.con.execute(
        "SELECT count(*) FROM dataset_catalog WHERE dataset_id = 'bulk_daily_bars_backfill'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM table_catalog WHERE table_name = 'price_backfill_partition'"
    ).fetchone()[0] == 1
    assert tmp_store.con.execute(
        "SELECT count(*) FROM schema_contract WHERE table_name = 'price_backfill_partition'"
    ).fetchone()[0] >= 10


def test_bulk_bars_backfill_runs_s1_partitions_and_skips_completed(tmp_store, tmp_path) -> None:
    _seed_symbol(tmp_store, "AAPL", "SEC-AAPL")
    source = tmp_path / "backfill-bars.csv"
    _write_csv(
        source,
        [
            {
                "symbol": "AAPL",
                "trade_date": "2015-01-02",
                "open": 10,
                "high": 11,
                "low": 9,
                "close": 10,
                "volume": 1000,
            },
            {
                "symbol": "AAPL",
                "trade_date": "2015-02-03",
                "open": 20,
                "high": 21,
                "low": 19,
                "close": 20,
                "volume": 2000,
            },
        ],
    )

    result = run_backfill(
        tmp_store,
        BulkBarsBackfillDataset.dataset_id,
        dt.date(2015, 1, 1),
        dt.date(2015, 3, 1),
        "1mo",
        params={"source_file": str(source), "chunk_size": 10},
        include_dependencies=False,
        backfill_run_id="price-bf-1",
    )

    assert result.status == "succeeded"
    assert result.partitions_succeeded == 2
    assert result.rows_written == 2
    assert tmp_store.con.execute(
        """
        SELECT partition_key, status, rows_loaded, watermark_after, min_trade_date, max_trade_date
        FROM price_backfill_partition
        ORDER BY partition_key
        """
    ).fetchall() == [
        (
            "bulk_daily_bars_backfill:2015-01-01:2015-02-01",
            "succeeded",
            1,
            "2015-02-01",
            dt.date(2015, 1, 2),
            dt.date(2015, 1, 2),
        ),
        (
            "bulk_daily_bars_backfill:2015-02-01:2015-03-01",
            "succeeded",
            1,
            "2015-03-01",
            dt.date(2015, 2, 3),
            dt.date(2015, 2, 3),
        ),
    ]

    rerun = run_backfill(
        tmp_store,
        BulkBarsBackfillDataset.dataset_id,
        dt.date(2015, 1, 1),
        dt.date(2015, 3, 1),
        "1mo",
        params={"source_file": str(source), "chunk_size": 10},
        include_dependencies=False,
        backfill_run_id="price-bf-2",
    )

    assert rerun.status == "succeeded"
    assert rerun.partitions_skipped == 2
    assert rerun.rows_written == 0
    assert tmp_store.con.execute(
        """
        SELECT count(*), count(DISTINCT security_id || ':' || trade_date)
        FROM equity_daily_bars
        WHERE source = 'bulk_bars_2015plus'
        """
    ).fetchone() == (2, 2)


def test_bulk_bars_backfill_failed_partition_rerun_replaces_existing_bars(tmp_store, tmp_path) -> None:
    _seed_symbol(tmp_store, "AAPL", "SEC-AAPL")
    source = tmp_path / "partial-replay.csv"
    _write_csv(
        source,
        [
            {
                "symbol": "AAPL",
                "trade_date": "2015-01-02",
                "open": 10,
                "high": 11,
                "low": 9,
                "close": 10,
                "volume": 1000,
            },
            {
                "symbol": "AAPL",
                "trade_date": "2015-01-05",
                "open": 12,
                "high": 13,
                "low": 11,
                "close": 12,
                "volume": 1500,
            },
        ],
    )

    first = run_backfill(
        tmp_store,
        BulkBarsBackfillDataset.dataset_id,
        dt.date(2015, 1, 1),
        dt.date(2015, 2, 1),
        "1mo",
        params={"source_file": str(source), "chunk_size": 1, "max_chunks": 1},
        include_dependencies=False,
        backfill_run_id="price-bf-partial",
    )
    assert first.rows_written == 1
    tmp_store.con.execute(
        """
        UPDATE backfill_watermark
        SET status = 'failed', rows_written = 0, watermark_after = NULL
        WHERE dataset_id = 'bulk_daily_bars_backfill'
          AND partition_key = 'bulk_daily_bars_backfill:2015-01-01:2015-02-01'
        """
    )

    replay = run_backfill(
        tmp_store,
        BulkBarsBackfillDataset.dataset_id,
        dt.date(2015, 1, 1),
        dt.date(2015, 2, 1),
        "1mo",
        params={"source_file": str(source), "chunk_size": 10},
        include_dependencies=False,
        backfill_run_id="price-bf-replay",
    )

    assert replay.partitions_succeeded == 1
    assert replay.rows_written == 2
    assert tmp_store.con.execute(
        """
        SELECT trade_date, close, run_id
        FROM equity_daily_bars
        WHERE source = 'bulk_bars_2015plus'
        ORDER BY trade_date
        """
    ).fetchall() == [
        (dt.date(2015, 1, 2), 10.0, "price-bf-replay"),
        (dt.date(2015, 1, 5), 12.0, "price-bf-replay"),
    ]
    assert tmp_store.con.execute(
        """
        SELECT count(*), count(DISTINCT security_id || ':' || trade_date)
        FROM equity_daily_bars
        WHERE source = 'bulk_bars_2015plus'
        """
    ).fetchone() == (2, 2)
