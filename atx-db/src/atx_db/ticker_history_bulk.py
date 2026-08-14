"""Native DuckDB bulk publication for the broad ticker-history archive.

The pandas chunk loader remains useful for small symbol subsets.  Full-universe
publication is different: repeatedly probing and deleting from a multi-million
row indexed table turns linear ingestion into an effectively quadratic job.
This module scans an extracted TSV twice with DuckDB projection pushdown, builds
the complete replacement beside the live table, validates it, and swaps it in
atomically.
"""

from __future__ import annotations

import datetime as dt
import json
import logging
import time
import uuid
from dataclasses import asdict, dataclass
from pathlib import Path

from .connection import DuckDBStore
from .ticker_history import SOURCE_NAME, TBLTICKERHISTORY_ID_TYPE
from .warehouse import quality_check, record_source_file

LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class BulkTickerHistoryOptions:
    tsv_path: Path
    source: str = SOURCE_NAME
    memory_limit: str = "4GB"
    threads: int = 4
    minimum_rows: int = 30_000_000
    minimum_securities: int = 10_000
    minimum_latest_date_securities: int = 5_000
    run_id: str | None = None

    def __post_init__(self) -> None:
        if self.threads < 1:
            raise ValueError("threads must be positive")
        if min(
            self.minimum_rows,
            self.minimum_securities,
            self.minimum_latest_date_securities,
        ) < 1:
            raise ValueError("publication breadth floors must be positive")


@dataclass(frozen=True)
class BulkTickerHistoryResult:
    rows: int
    securities: int
    latest_date: dt.date
    latest_date_securities: int
    invalid_rows: int
    duplicate_keys: int
    elapsed_seconds: float
    run_id: str


_RAW_CTE = """
projected AS (
    SELECT
        try_cast(tradingDate AS DATE) AS trade_date,
        nullif(trim(securityID), '') AS vendor_security_id,
        upper(trim(coalesce(nullif(todayTicker, ''), nullif(ticker_tk, '')))) AS symbol,
        try_cast(open AS DOUBLE) AS open,
        try_cast(high AS DOUBLE) AS high,
        try_cast(low AS DOUBLE) AS low,
        try_cast(close AS DOUBLE) AS close,
        try_cast(closePr AS DOUBLE) AS adjusted_close,
        try_cast(volume AS BIGINT) AS volume,
        try_cast(shares AS BIGINT) AS shares_outstanding,
        try_cast(returnFactor AS DOUBLE) AS split_factor
    FROM read_csv(
        ?, delim = '\t', header = true, all_varchar = true,
        auto_detect = true, sample_size = 20480
    )
),
raw AS (
    SELECT *
    FROM projected
    WHERE trade_date IS NOT NULL AND symbol IS NOT NULL AND symbol <> ''
)
"""


def _configure(store: DuckDBStore, options: BulkTickerHistoryOptions) -> None:
    temp_dir = options.tsv_path.parent / "duckdb-tmp"
    temp_dir.mkdir(parents=True, exist_ok=True)
    store.con.execute("PRAGMA disable_progress_bar")
    store.con.execute("SET memory_limit = ?", [options.memory_limit])
    store.con.execute("SET threads = ?", [options.threads])
    store.con.execute("SET preserve_insertion_order = false")
    store.con.execute("SET temp_directory = ?", [str(temp_dir)])


def _create_symbol_map(store: DuckDBStore) -> None:
    store.con.execute(
        """
        CREATE OR REPLACE TEMP TABLE broad_symbol_map AS
        WITH candidates AS (
            SELECT upper(trim(ticker)) AS symbol, security_id, 1 AS priority,
                   source_loaded_at, security_id AS tie_breaker
            FROM sec_company_tickers
            WHERE ticker IS NOT NULL AND trim(ticker) <> ''
            UNION ALL
            SELECT upper(trim(id_value)), security_id, 2, source_loaded_at, security_id
            FROM security_identifier_history
            WHERE id_type = 'TICKER'
              AND (valid_to IS NULL OR valid_to >= current_date)
            UNION ALL
            SELECT upper(trim(ticker)), security_id, 3, source_loaded_at, security_id
            FROM exchange_listings
            WHERE valid_to IS NULL OR valid_to >= current_date
        )
        SELECT symbol, security_id
        FROM candidates
        WHERE symbol IS NOT NULL AND symbol <> '' AND security_id IS NOT NULL
        QUALIFY row_number() OVER (
            PARTITION BY symbol
            ORDER BY priority, source_loaded_at DESC NULLS LAST, tie_breaker
        ) = 1
        """
    )


def _create_line_map(store: DuckDBStore, options: BulkTickerHistoryOptions) -> None:
    store.con.execute(
        f"""
        CREATE OR REPLACE TEMP TABLE broad_line_map AS
        WITH {_RAW_CTE},
        resolved AS (
            SELECT
                r.vendor_security_id,
                r.symbol,
                r.trade_date,
                coalesce(
                    m.security_id,
                    CASE
                        WHEN r.vendor_security_id IS NULL OR r.vendor_security_id = '0'
                        THEN 'TBLTICKERHISTORY-SYMBOL-' || r.symbol || '-VENDOR-'
                             || coalesce(r.vendor_security_id, 'MISSING')
                        ELSE 'TBLTICKERHISTORY-' || r.vendor_security_id
                    END
                ) AS base_security_id
            FROM raw r
            LEFT JOIN broad_symbol_map m USING (symbol)
        ),
        line_stats AS (
            SELECT
                base_security_id,
                vendor_security_id,
                symbol,
                count(*) AS observations,
                min(trade_date) AS first_seen_date,
                max(trade_date) AS last_seen_date
            FROM resolved
            GROUP BY base_security_id, vendor_security_id, symbol
        ),
        ranked AS (
            SELECT *, row_number() OVER (
                PARTITION BY base_security_id
                ORDER BY observations DESC, vendor_security_id, symbol
            ) AS line_rank
            FROM line_stats
        )
        SELECT
            CASE
                WHEN line_rank = 1 THEN base_security_id
                ELSE 'TBLTICKERHISTORY-' || coalesce(vendor_security_id, '<NA>')
                     || '-' || symbol
            END AS security_id,
            vendor_security_id,
            symbol,
            CASE
                WHEN vendor_security_id IS NULL OR vendor_security_id = '0'
                THEN 'SYMBOL-' || symbol || '-VENDOR-'
                     || coalesce(vendor_security_id, 'MISSING')
                ELSE vendor_security_id
            END AS identifier_value,
            observations,
            first_seen_date,
            last_seen_date
        FROM ranked
        """,
        [str(options.tsv_path)],
    )


def _create_next_table(store: DuckDBStore, options: BulkTickerHistoryOptions) -> None:
    con = store.con
    con.execute("DROP TABLE IF EXISTS equity_daily_bars_bulk_next")
    con.execute(
        """
        CREATE TABLE equity_daily_bars_bulk_next (
            source VARCHAR NOT NULL,
            security_id VARCHAR NOT NULL,
            vendor_security_id VARCHAR,
            symbol VARCHAR NOT NULL,
            trade_date DATE NOT NULL,
            open DOUBLE,
            high DOUBLE,
            low DOUBLE,
            close DOUBLE,
            adjusted_close DOUBLE,
            volume BIGINT,
            vwap DOUBLE,
            dividend_amount DOUBLE,
            split_factor DOUBLE,
            is_adjusted BOOLEAN NOT NULL DEFAULT false,
            available_at TIMESTAMP,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now(),
            as_of_date DATE,
            is_latest_revision BOOLEAN,
            shares_outstanding BIGINT,
            market_cap_usd DOUBLE
        )
        """
    )
    con.execute(
        f"""
        INSERT INTO equity_daily_bars_bulk_next
        WITH {_RAW_CTE},
        valid AS (
            SELECT r.*, m.security_id
            FROM raw r
            JOIN broad_line_map m
              ON m.symbol = r.symbol
             AND m.vendor_security_id IS NOT DISTINCT FROM r.vendor_security_id
            WHERE NOT (
                coalesce(r.volume < 0, false)
                OR coalesce(r.open <= 0, false)
                OR coalesce(r.high <= 0, false)
                OR coalesce(r.low <= 0, false)
                OR coalesce(r.close <= 0, false)
                OR coalesce(r.high < greatest(r.open, r.low, r.close), false)
                OR coalesce(r.low > least(r.open, r.high, r.close), false)
            )
        )
        SELECT
            ?, security_id, vendor_security_id, symbol, trade_date,
            open, high, low, close, adjusted_close, volume,
            NULL::DOUBLE, NULL::DOUBLE, split_factor, false,
            trade_date::TIMESTAMP + INTERVAL 22 HOUR,
            ?, current_timestamp, NULL::DATE, NULL::BOOLEAN,
            shares_outstanding, shares_outstanding * close
        FROM valid
        QUALIFY row_number() OVER (
            PARTITION BY security_id, trade_date
            ORDER BY volume DESC NULLS LAST, vendor_security_id
        ) = 1
        """,
        [str(options.tsv_path), options.source, options.run_id],
    )


def _validate_next(
    store: DuckDBStore, options: BulkTickerHistoryOptions
) -> tuple[int, int, dt.date, int, int, int]:
    row = store.con.execute(
        """
        SELECT count(*), count(DISTINCT security_id), min(trade_date), max(trade_date)
        FROM equity_daily_bars_bulk_next
        WHERE source = ?
        """,
        [options.source],
    ).fetchone()
    if row is None or row[3] is None:
        raise RuntimeError("bulk ticker-history stage is empty")
    rows, securities, _minimum_date, latest_date = row
    latest_row = store.con.execute(
        """
        SELECT count(DISTINCT security_id)
        FROM equity_daily_bars_bulk_next
        WHERE source = ? AND trade_date = ?
        """,
        [options.source, latest_date],
    ).fetchone()
    if latest_row is None:
        raise RuntimeError("could not validate latest-date ticker-history breadth")
    latest_securities = latest_row[0]
    duplicate_row = store.con.execute(
        """
        SELECT count(*)
        FROM (
            SELECT security_id, trade_date
            FROM equity_daily_bars_bulk_next
            WHERE source = ?
            GROUP BY security_id, trade_date
            HAVING count(*) > 1
        )
        """,
        [options.source],
    ).fetchone()
    if duplicate_row is None:
        raise RuntimeError("could not validate ticker-history duplicate keys")
    duplicate_keys = duplicate_row[0]
    invalid_row = store.con.execute(
        """
        SELECT count(*)
        FROM equity_daily_bars_bulk_next
        WHERE source = ? AND (
            volume < 0 OR open <= 0 OR high <= 0 OR low <= 0 OR close <= 0
            OR high < greatest(open, low, close)
            OR low > least(open, high, close)
        )
        """,
        [options.source],
    ).fetchone()
    if invalid_row is None:
        raise RuntimeError("could not validate ticker-history OHLCV")
    invalid_rows = invalid_row[0]
    failures = []
    if rows < options.minimum_rows:
        failures.append(f"rows {rows:,} < {options.minimum_rows:,}")
    if securities < options.minimum_securities:
        failures.append(f"securities {securities:,} < {options.minimum_securities:,}")
    if latest_securities < options.minimum_latest_date_securities:
        failures.append(
            f"latest-date securities {latest_securities:,} < "
            f"{options.minimum_latest_date_securities:,}"
        )
    if duplicate_keys:
        failures.append(f"duplicate keys {duplicate_keys:,}")
    if invalid_rows:
        failures.append(f"invalid OHLCV rows {invalid_rows:,}")
    if failures:
        raise RuntimeError("bulk ticker-history publication gate failed: " + "; ".join(failures))
    return (
        int(rows),
        int(securities),
        latest_date,
        int(latest_securities),
        int(invalid_rows),
        int(duplicate_keys),
    )


def _publish(store: DuckDBStore, options: BulkTickerHistoryOptions) -> None:
    con = store.con
    with store.transaction():
        con.execute(
            """
            UPDATE securities AS s SET
                primary_symbol = m.symbol,
                name = coalesce(s.name, m.symbol),
                first_seen_date = least(s.first_seen_date, m.first_seen_date),
                last_seen_date = greatest(s.last_seen_date, m.last_seen_date),
                active = true
            FROM broad_line_map m
            WHERE s.security_id = m.security_id AND s.source = ?
            """,
            [options.source],
        )
        con.execute(
            """
            INSERT INTO securities (
                security_id, issuer_id, primary_symbol, name, asset_class,
                country, currency, active, first_seen_date, last_seen_date, source
            )
            SELECT security_id, NULL, symbol, symbol, 'EQUITY', 'US', 'USD', true,
                   first_seen_date, last_seen_date, ?
            FROM broad_line_map m
            WHERE NOT EXISTS (
                SELECT 1 FROM securities s WHERE s.security_id = m.security_id
            )
            """,
            [options.source],
        )
        con.execute(
            "DELETE FROM security_identifier_history WHERE source = ? AND id_type = ?",
            [options.source, TBLTICKERHISTORY_ID_TYPE],
        )
        con.execute(
            """
            INSERT INTO security_identifier_history (
                security_id, id_type, id_value, valid_from, valid_to, as_of_date,
                available_at, source, run_id
            )
            SELECT security_id, ?, identifier_value, first_seen_date, NULL,
                   first_seen_date, first_seen_date::TIMESTAMP + INTERVAL 22 HOUR,
                   ?, ?
            FROM broad_line_map
            """,
            [TBLTICKERHISTORY_ID_TYPE, options.source, options.run_id],
        )
        con.execute("DELETE FROM exchange_listings WHERE source = ?", [options.source])
        con.execute(
            """
            INSERT INTO exchange_listings (
                security_id, ticker, exchange_code, mic, currency, valid_from,
                valid_to, as_of_date, available_at, source, run_id
            )
            SELECT security_id, symbol, NULL, NULL, 'USD', first_seen_date, NULL,
                   first_seen_date, first_seen_date::TIMESTAMP + INTERVAL 22 HOUR,
                   ?, ?
            FROM broad_line_map
            """,
            [options.source, options.run_id],
        )
        con.execute("DELETE FROM equity_daily_bars WHERE source = ?", [options.source])
        con.execute("INSERT INTO equity_daily_bars SELECT * FROM equity_daily_bars_bulk_next")
        _ensure_indexes(store)
    con.execute("DROP TABLE equity_daily_bars_bulk_next")


def _ensure_indexes(store: DuckDBStore) -> None:
    store.con.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_equity_daily_bars_security_date
        ON equity_daily_bars(security_id, trade_date)
        """
    )
    store.con.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_equity_daily_bars_symbol_date
        ON equity_daily_bars(symbol, trade_date)
        """
    )


def publish_bulk_ticker_history(
    store: DuckDBStore, options: BulkTickerHistoryOptions
) -> BulkTickerHistoryResult:
    if not options.tsv_path.is_file():
        raise FileNotFoundError(options.tsv_path)
    started = time.perf_counter()
    run_id = options.run_id or f"broad-bars-bulk-{uuid.uuid4()}"
    options = BulkTickerHistoryOptions(**{**asdict(options), "run_id": run_id})
    _configure(store, options)
    store.con.execute(
        """
        INSERT OR REPLACE INTO dataset_runs (
            run_id, dataset_id, status, started_at, source, params_json
        )
        VALUES (?, 'tbltickerhistory_daily', 'running', current_timestamp, ?, ?)
        """,
        [run_id, options.source, json.dumps(asdict(options), default=str, sort_keys=True)],
    )
    try:
        record_source_file(
            store,
            dataset_id="tbltickerhistory_daily",
            source_url=str(options.tsv_path),
            cache_path=options.tsv_path,
            status="available",
            metadata={"mode": "native_bulk_projection", "run_id": run_id},
            compute_hash=False,
        )
        _create_symbol_map(store)
        LOGGER.info("built canonical symbol map")
        _create_line_map(store, options)
        LOGGER.info("built vendor-line collision map")
        _create_next_table(store, options)
        LOGGER.info("built canonical daily-bar staging table")
        metrics = _validate_next(store, options)
        LOGGER.info(
            "validated %,d rows across %,d securities; latest breadth %,d",
            metrics[0],
            metrics[1],
            metrics[3],
        )
        _publish(store, options)
        LOGGER.info("atomically published bars and canonical indexes")
    except Exception as exc:
        store.con.execute("DROP TABLE IF EXISTS equity_daily_bars_bulk_next")
        store.con.execute(
            """
            UPDATE dataset_runs SET status = 'failed', finished_at = current_timestamp,
                                    error_message = ?
            WHERE run_id = ?
            """,
            [str(exc), run_id],
        )
        raise
    rows, securities, latest_date, latest_securities, invalid_rows, duplicate_keys = metrics
    elapsed = time.perf_counter() - started
    store.con.execute(
        """
        UPDATE dataset_runs SET status = 'succeeded', finished_at = current_timestamp,
                                rows_loaded = ?
        WHERE run_id = ?
        """,
        [rows, run_id],
    )
    quality_check(
        store,
        dataset_id="tbltickerhistory_daily",
        table_name="equity_daily_bars",
        check_name="institutional_latest_date_breadth",
        status="passed",
        observed_value=float(latest_securities),
        threshold_value=float(options.minimum_latest_date_securities),
        details={
            "run_id": run_id,
            "rows": rows,
            "securities": securities,
            "latest_date": latest_date,
            "duplicate_keys": duplicate_keys,
            "invalid_rows": invalid_rows,
        },
    )
    store.con.execute("CHECKPOINT")
    return BulkTickerHistoryResult(
        rows=rows,
        securities=securities,
        latest_date=latest_date,
        latest_date_securities=latest_securities,
        invalid_rows=invalid_rows,
        duplicate_keys=duplicate_keys,
        elapsed_seconds=elapsed,
        run_id=run_id,
    )
