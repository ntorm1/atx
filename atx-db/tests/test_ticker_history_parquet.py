from __future__ import annotations

import datetime as dt
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

import duckdb
import pytest

from atx_db.ticker_history_bulk import (
    BulkTickerHistoryOptions,
    BulkTickerHistoryResult,
    publish_bulk_ticker_history,
)
from atx_db.ticker_history_quality import source_diagnostics, stage_source

_FIELDS = (
    "tradingDate DATE, securityID BIGINT, ticker_tk VARCHAR, todayTicker VARCHAR, dn BIGINT, "
    "open FLOAT, high FLOAT, low FLOAT, close FLOAT, closePr FLOAT, closeUnadjPr FLOAT, "
    "volume DOUBLE, shares BIGINT, returnFactor FLOAT, totalReturn FLOAT, cumulReturnFactor DOUBLE"
)
_ROWS = [
    ("2025-01-02", 101, "OLD", "NEW", 1, 100, 101, 99, 100, 90, 90, 1000, 10000, 1, 0, .5),
    ("2025-01-03", 101, "NEW", "NEW", 2, 55, 56, 54, 55, 50, 100, 2000, 20000, .5, .1, 1),
    ("2025-01-02", 202, "BAD", "BAD", 1, 10, 11, 9, 10, 9, 9, 100, 1000, 1, 0, 1),
    ("2025-01-02", 202, "BAD", "BAD", 1, 11, 12, 10, 11, 9, 9, 200, 1000, 1, 0, 1),
    ("2025-01-03", 202, "BAD", "BAD", 2, 12, 13, 11, 12, 11, 11, 100, 1000, 1, 0, None),
]


def _write_sources(tmp_path, *, select="SELECT * FROM source"):
    parquet = tmp_path / "vendor's source.parquet"
    tsv = tmp_path / "source.tsv"
    with duckdb.connect(config={"memory_limit": "128MB", "threads": 1}) as con:
        con.execute("CREATE TABLE source (" + _FIELDS + ")")
        con.executemany("INSERT INTO source VALUES (" + ",".join(["?"] * 16) + ")", _ROWS)
        con.sql(select).write_parquet(str(parquet))
        con.sql(select).write_csv(str(tsv), sep="\t", header=True)
    return parquet, tsv


def _script(name):
    path = Path(__file__).parents[1] / "scripts" / f"{name}.py"
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_typed_parquet_publishes_same_bars_diagnostics_and_hash_lineage_as_tsv(tmp_store, tmp_path):
    parquet, tsv = _write_sources(tmp_path)
    # Keep identity metadata equal across the two source replacements; otherwise
    # the first publication itself creates ticker links before the second run.
    tmp_store.con.executemany(
        "INSERT INTO sec_company_tickers (cik, ticker, title, security_id) VALUES (?, ?, ?, ?)",
        [("0000000101", "NEW", "New", "TBLTICKERHISTORY-101"),
         ("0000000202", "BAD", "Bad", "TBLTICKERHISTORY-202")],
    )
    published = []
    diagnostics = []
    for path, source_format in [(tsv, "tsv"), (parquet, "parquet")]:
        result = publish_bulk_ticker_history(tmp_store, BulkTickerHistoryOptions(
            source_path=path, memory_limit="128MB", threads=1, minimum_rows=3,
            minimum_securities=2, minimum_latest_date_securities=2,
            run_id="parquet-test-" + source_format,
        ))
        published.append(tmp_store.con.execute(
            "SELECT security_id, symbol, trade_date, close, adjusted_close, volume, "
            "shares_outstanding, split_factor FROM equity_daily_bars ORDER BY security_id, trade_date"
        ).fetchall())
        diagnostics.append(result.source_diagnostics)
        assert result.provenance["source_format"] == source_format
        assert "unverified" in result.provenance["economic_adjustment_status"]
        recorded = tmp_store.con.execute(
            "SELECT sha256, metadata_json FROM raw_source_files WHERE cache_path = ?", [str(path)]
        ).fetchone()
        assert recorded is not None
        assert recorded[0] == hashlib.sha256(path.read_bytes()).hexdigest()
        assert json.loads(recorded[1])["source_format"] == source_format
    assert published[0] == published[1]
    assert diagnostics[0] == diagnostics[1]
    assert published[1] == [
        ("TBLTICKERHISTORY-101", "OLD", dt.date(2025, 1, 2), 100, 50, 1000, 10000, None),
        ("TBLTICKERHISTORY-101", "NEW", dt.date(2025, 1, 3), 55, 55, 2000, 20000, None),
        ("TBLTICKERHISTORY-202", "BAD", dt.date(2025, 1, 3), 12, None, 100, 1000, None),
    ]
    assert diagnostics[1]["source_rows"] == 5
    assert diagnostics[1]["quarantined_positive_key_rows"] == 2
    assert diagnostics[1]["invalid_cumulative_factor_rows"] == 1
    assert diagnostics[1]["latest_date_source_rows"] == 2
    assert diagnostics[1]["latest_date_distinct_positive_vendor_ids"] == 2
    assert str(diagnostics[1]["first_date"]).startswith("2025-01-02")
    assert str(diagnostics[1]["last_date"]).startswith("2025-01-03")


@pytest.mark.parametrize(("select", "message"), [
    ("SELECT * EXCLUDE (cumulReturnFactor) FROM source", "missing required columns: cumulreturnfactor"),
    ("SELECT * REPLACE ([close] AS close) FROM source", "incompatible column types: close=FLOAT\\[\\]"),
    ("SELECT * REPLACE (101 AS ticker_tk) FROM source", "incompatible column types: ticker_tk=INTEGER"),
    ("SELECT * REPLACE (20250102 AS tradingDate) FROM source", "incompatible column types: tradingdate=INTEGER"),
])
def test_parquet_rejects_missing_or_incompatible_required_columns(tmp_path, select, message):
    parquet, _ = _write_sources(tmp_path, select=select)
    with duckdb.connect(config={"memory_limit": "128MB", "threads": 1}) as con:
        with pytest.raises(ValueError, match=message):
            stage_source(con, str(parquet))
        assert con.execute("SELECT count(*) FROM duckdb_tables() WHERE table_name = 'ticker_history_source_rows'").fetchone() == (0,)


def test_parquet_fractional_ids_are_not_rounded_into_positive_keys(tmp_path):
    parquet, _ = _write_sources(tmp_path, select="SELECT * REPLACE (101.5 AS securityID) FROM source")
    with duckdb.connect(config={"memory_limit": "128MB", "threads": 1}) as con:
        stage_source(con, str(parquet))
        diagnostics = source_diagnostics(con)
        assert diagnostics["missing_or_invalid_id_rows"] == 5
        assert diagnostics["repeated_positive_keys"] == 0


@pytest.mark.parametrize("flag", ["--source-path", "--tsv-path"])
def test_audit_cli_reports_exact_parquet_bytes_and_format(tmp_path, monkeypatch, capsys, flag):
    parquet, _ = _write_sources(tmp_path)
    output = tmp_path / "audit.json"
    monkeypatch.setattr(sys, "argv", ["audit", flag, str(parquet), "--output", str(output),
                                     "--memory-limit", "128MB"])
    _script("audit_ticker_history_source").main()
    report = json.loads(output.read_text(encoding="utf-8"))
    assert report["source_format"] == report["provenance"]["source_format"] == "parquet"
    assert report["source_sha256"] == hashlib.sha256(parquet.read_bytes()).hexdigest()
    assert report["diagnostics"]["source_rows"] == 5
    assert report["diagnostics"]["latest_date_distinct_positive_vendor_ids"] == 2
    assert "native typed Parquet" in report["provenance"]["source_numeric_representation"]
    assert "full source file" in report["hash_scope"]
    capsys.readouterr()


@pytest.mark.parametrize("flag", ["--source-path", "--tsv-path"])
def test_publish_cli_accepts_neutral_path_and_legacy_alias(tmp_path, monkeypatch, capsys, flag):
    script = _script("publish_broad_daily_bars")
    source = tmp_path / "prices.parquet"
    seen = []

    class Store:
        def __init__(self, path):
            pass

        def __enter__(self):
            return self

        def __exit__(self, *args):
            pass

    def publish(store, options):
        seen.append(options)
        return BulkTickerHistoryResult(1, 1, dt.date(2025, 1, 3), 1, 0, 0, 0, "test")

    monkeypatch.setattr(script, "DuckDBStore", Store)
    monkeypatch.setattr(script, "publish_bulk_ticker_history", publish)
    monkeypatch.setattr(sys, "argv", ["publish", flag, str(source)])
    assert script.main() == 0
    assert seen[0].source_path == source.resolve()
    assert seen[0].input_path == source.resolve()
    assert seen[0].threads == 1
    assert seen[0].memory_limit == "1GB"
    capsys.readouterr()


def test_bulk_options_reject_ambiguous_paths_and_preserve_positional_tsv(tmp_path):
    tsv = tmp_path / "old.tsv"
    assert BulkTickerHistoryOptions(tsv).input_path == tsv
    with pytest.raises(ValueError, match="exactly one"):
        BulkTickerHistoryOptions(tsv_path=tsv, source_path=tmp_path / "new.parquet")
    with pytest.raises(ValueError, match="exactly one"):
        BulkTickerHistoryOptions()
