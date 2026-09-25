from __future__ import annotations

import datetime as dt
import json

import duckdb
import pytest

from atx_db.ticker_history_bulk import BulkTickerHistoryOptions, publish_bulk_ticker_history

HEADER = (
    "tradingDate\tsecurityID\tticker_tk\ttodayTicker\topen\thigh\tlow\tclose\t"
    "closePr\tvolume\tshares\treturnFactor\tcumulReturnFactor\tdn\tcloseUnadjPr\ttotalReturn\n"
)


def test_bulk_publication_is_atomic_deduplicated_and_collision_safe(tmp_store, tmp_path):
    archive = tmp_path / "bars.tsv"
    archive.write_text(
        HEADER
        + "2025-01-02\t10\tAAA\tAAA\t10\t11\t9\t10\t10\t100\t1000\t1\t1\t1\t10\t0\n"
        + "2025-01-02\t10\tAAA\tAAA\t10\t11\t9\t10\t10\t200\t1000\t1\t1\t1\t10\t0\n"
        + "2025-01-03\t10\tAAA\tAAA\t11\t12\t10\t11\t11\t150\t1000\t1\t1\t1\t10\t0\n"
        + "2025-01-03\t20\tAAA\tAAA\t20\t21\t19\t20\t20\t50\t1000\t1\t1\t1\t10\t0\n"
        + "2025-01-03\t30\tBAD\tBAD\t20\t19\t18\t20\t20\t50\t1000\t1\t1\t1\t10\t0\n",
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
            minimum_rows=2,
            minimum_securities=2,
            minimum_latest_date_securities=2,
            run_id="bulk-test",
        ),
    )

    assert result.rows == 2
    assert result.securities == 2
    assert result.latest_date_securities == 2
    assert result.duplicate_keys == 0
    assert result.invalid_rows == 0
    assert result.source_diagnostics["quarantined_positive_key_rows"] == 2
    assert tmp_store.con.execute(
        "SELECT volume FROM equity_daily_bars WHERE vendor_security_id = '10' AND trade_date = DATE '2025-01-02'"
    ).fetchone() is None
    security_ids = tmp_store.con.execute(
        "SELECT DISTINCT security_id FROM equity_daily_bars WHERE source = 'tbltickerhistory3_10y' ORDER BY 1"
    ).fetchall()
    assert security_ids == [("SEC-CIK-0000000001",), ("TBLTICKERHISTORY-20",)]
    assert tmp_store.con.execute(
        "SELECT count(*) FROM equity_daily_bars WHERE source = 'other'"
    ).fetchone() == (1,)
    assert tmp_store.con.execute(
        "SELECT status, rows_loaded FROM dataset_runs WHERE run_id = 'bulk-test'"
    ).fetchone() == ("succeeded", 2)
    # A published run releases its session staging (the whole source projection); the
    # activation ladder's next stage refuses a connection holding caller temporaries.
    assert tmp_store.con.execute(
        "SELECT table_name FROM duckdb_tables() WHERE temporary AND NOT internal"
    ).fetchall() == []


def test_bulk_publication_gate_preserves_live_table(tmp_store, tmp_path):
    archive = tmp_path / "too-small.tsv"
    archive.write_text(
        HEADER + "2025-01-02\t10\tAAA\tAAA\t10\t11\t9\t10\t10\t100\t1000\t1\t1\t1\t10\t0\n",
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


_VENDOR_FIELDS = (
    "tradingDate DATE, securityID BIGINT, ticker_tk VARCHAR, todayTicker VARCHAR, dn BIGINT, "
    "open DOUBLE, high DOUBLE, low DOUBLE, close DOUBLE, closePr DOUBLE, closeUnadjPr DOUBLE, "
    "volume DOUBLE, shares BIGINT, returnFactor DOUBLE, totalReturn DOUBLE, cumulReturnFactor DOUBLE"
)
_AAPL = "SEC-CIK-0000320193"
_CELG = "SEC-CIK-0000816284"
# (date, vendor id, ticker, close, shares in THOUSANDS as TickerHistory3.parquet reports them).
# AAPL: the vendor starts the 15,204,137 run on the 10-Q cover date 2024-07-19 (DEI
# 15,204,137,000, filed 2024-08-02); CELG 2012: 438,810 thousand = ~439M shares.
_VENDOR_ROWS = [
    ("2024-07-18", 1001, "AAPL", 224.31, 15_334_082),
    ("2024-07-19", 1001, "AAPL", 224.31, 15_204_137),
    ("2024-07-22", 1001, "AAPL", 223.96, 15_204_137),
    ("2012-10-24", 1002, "CELG", 76.12, 438_810),
    ("2012-10-25", 1002, "CELG", 76.50, 438_810),
]


def _vendor_source(tmp_path, source_format, *, shares_scale=1):
    path = tmp_path / f"TickerHistory3.{source_format}"
    with duckdb.connect(config={"memory_limit": "128MB", "threads": 1}) as con:
        con.execute("CREATE TABLE source (" + _VENDOR_FIELDS + ")")
        con.executemany(
            "INSERT INTO source VALUES (?, ?, ?, ?, 1, ?, ?, ?, ?, ?, ?, 1000, ?, 1, 0, 1)",
            [
                (day, vendor, ticker, ticker, close, close, close, close, close, close, shares * shares_scale)
                for day, vendor, ticker, close, shares in _VENDOR_ROWS
            ],
        )
        if source_format == "parquet":
            con.sql("SELECT * FROM source").write_parquet(str(path))
        else:
            con.sql("SELECT * FROM source").write_csv(str(path), sep="\t", header=True)
    return path


def _vendor_identity(store, *, dei=True):
    store.con.executemany(
        "INSERT INTO sec_company_tickers (cik, ticker, title, security_id) VALUES (?, ?, ?, ?)",
        [("0000320193", "AAPL", "Apple Inc.", _AAPL), ("0000816284", "CELG", "Celgene Corp", _CELG)],
    )
    if dei:
        store.con.execute(
            """
            INSERT INTO shares_outstanding_history (
                share_history_id, source, security_id, cik, share_count_type, taxonomy,
                concept, unit, period_type, period_end, effective_date, as_of_date,
                available_at, accession_number, revision_sequence, revision_count,
                is_latest_revision, share_count, source_url
            ) VALUES ('aapl-q3-2024', 'test', ?, '0000320193', 'shares_outstanding', 'dei',
                      'EntityCommonStockSharesOutstanding', 'shares', 'instant',
                      DATE '2024-07-19', DATE '2024-07-19', DATE '2024-08-02',
                      TIMESTAMP '2024-08-02 20:30:00', '0000320193-24-000081', 1, 1, true,
                      15204137000, 'test')
            """,
            [_AAPL],
        )


def _vendor_options(path, run_id, **overrides):
    return BulkTickerHistoryOptions(
        source_path=path,
        memory_limit="128MB",
        threads=1,
        minimum_rows=5,
        minimum_securities=2,
        minimum_latest_date_securities=1,
        run_id=run_id,
        **overrides,
    )


def _unit_check(store, run_id):
    row = store.con.execute(
        """
        SELECT status, details_json FROM data_quality_checks
        WHERE check_name = 'vendor_shares_unit' AND json_extract_string(details_json, '$.run_id') = ?
        """,
        [run_id],
    ).fetchone()
    assert row is not None
    return row[0], json.loads(row[1])


@pytest.mark.parametrize(
    ("source_format", "shares_scale", "unit"),
    [("parquet", 1, "thousands"), ("tsv", 1000, "units")],
)
def test_vendor_shares_are_stored_in_shares_whatever_the_source_unit(
    tmp_store, tmp_path, source_format, shares_scale, unit
):
    """A8: the parquet reports thousands, the TSV units; both publish shares (AAPL/CELG shapes)."""
    path = _vendor_source(tmp_path, source_format, shares_scale=shares_scale)
    _vendor_identity(tmp_store)
    run_id = f"units-{source_format}"

    result = publish_bulk_ticker_history(tmp_store, _vendor_options(path, run_id, shares_unit_min_pairs=1))

    rows = tmp_store.con.execute(
        """
        SELECT security_id, trade_date, shares_outstanding, market_cap_usd
        FROM equity_daily_bars WHERE source = 'tbltickerhistory3_10y' ORDER BY security_id, trade_date
        """
    ).fetchall()
    assert [(sid, day, shares) for sid, day, shares, _ in rows] == [
        (_AAPL, dt.date(2024, 7, 18), 15_334_082_000),
        (_AAPL, dt.date(2024, 7, 19), 15_204_137_000),
        (_AAPL, dt.date(2024, 7, 22), 15_204_137_000),
        (_CELG, dt.date(2012, 10, 24), 438_810_000),
        (_CELG, dt.date(2012, 10, 25), 438_810_000),
    ]
    closes = {(day, vendor): close for day, vendor, _, close, _ in _VENDOR_ROWS}
    for sid, day, shares, cap in rows:
        vendor = 1001 if sid == _AAPL else 1002
        assert cap == pytest.approx(shares * closes[(day.isoformat(), vendor)], rel=1e-9)
    assert result.provenance["shares_unit"] == unit
    assert result.provenance["shares_outstanding_unit"] == "shares"
    params = tmp_store.con.execute("SELECT params_json FROM dataset_runs WHERE run_id = ?", [run_id]).fetchone()
    assert json.loads(params[0])["shares_unit"] == unit
    metadata = tmp_store.con.execute(
        "SELECT metadata_json FROM raw_source_files WHERE cache_path = ?", [str(path)]
    ).fetchone()
    assert json.loads(metadata[0])["shares_unit"] == unit
    status, details = _unit_check(tmp_store, run_id)
    assert status == "passed"
    assert details["shares_unit"] == unit
    assert details["dei_pairs"] == 1
    assert details["dei_median_ratio"] == pytest.approx(1.0)


@pytest.mark.parametrize(("min_pairs", "raises"), [(1, True), (2, False)])
def test_share_unit_disagreeing_with_dei_fails_or_is_flagged(tmp_store, tmp_path, min_pairs, raises):
    """Reading the thousands parquet as units puts vendor/DEI at 1/1000: fail with evidence, else flag."""
    path = _vendor_source(tmp_path, "parquet")
    _vendor_identity(tmp_store)
    tmp_store.con.execute(
        """
        INSERT INTO equity_daily_bars (source, security_id, symbol, trade_date, close, is_adjusted)
        VALUES ('tbltickerhistory3_10y', 'LIVE-1', 'LIVE', DATE '2024-01-02', 1, false)
        """
    )
    run_id = f"units-mistaken-{min_pairs}"
    options = _vendor_options(path, run_id, shares_unit="units", shares_unit_min_pairs=min_pairs)

    if raises:
        with pytest.raises(RuntimeError, match="vendor share unit 'units' check failed"):
            publish_bulk_ticker_history(tmp_store, options)
        assert tmp_store.con.execute(
            "SELECT security_id FROM equity_daily_bars WHERE source = 'tbltickerhistory3_10y'"
        ).fetchall() == [("LIVE-1",)]
        assert tmp_store.con.execute("SELECT status FROM dataset_runs WHERE run_id = ?", [run_id]).fetchone() == (
            "failed",
        )
    else:
        publish_bulk_ticker_history(tmp_store, options)
    status, details = _unit_check(tmp_store, run_id)
    assert status == ("failed" if raises else "warning")
    assert details["dei_pairs"] == 1
    assert details["dei_median_ratio"] == pytest.approx(1e-3)


def test_share_unit_without_dei_evidence_is_flagged_unverified(tmp_store, tmp_path):
    path = _vendor_source(tmp_path, "parquet")
    _vendor_identity(tmp_store, dei=False)

    publish_bulk_ticker_history(tmp_store, _vendor_options(path, "units-unverified"))

    status, details = _unit_check(tmp_store, "units-unverified")
    assert status == "warning"
    assert details["dei_pairs"] == 0
    assert details["failures"] == []
