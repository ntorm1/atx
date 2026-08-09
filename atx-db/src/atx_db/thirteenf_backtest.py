"""Availability-safe price evaluation for consensus 13F amendment signals."""

from __future__ import annotations

import datetime as dt
import time
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from .connection import DuckDBStore
from .dataset import DatasetLoadResult
from .thirteenf_signals import DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER
from .ticker_history import DEFAULT_TICKER_HISTORY_ZIP, TickerHistoryDataset, TickerHistoryOptions

DEFAULT_HORIZONS = (5, 10, 21, 47)
DEFAULT_MIDCAP_MIN_USD = 2_000_000_000.0
DEFAULT_MIDCAP_MAX_USD = 10_000_000_000.0


@dataclass(frozen=True)
class ThirteenFSignalPriceOptions:
    archive_path: Path = DEFAULT_TICKER_HISTORY_ZIP
    start_date: dt.date | None = None
    end_date: dt.date | None = None
    chunk_size: int = 200_000
    source: str = "tbltickerhistory3_10y"
    run_id: str | None = None


@dataclass(frozen=True)
class ThirteenFBacktestResult:
    signal_count: int
    mapped_signal_count: int
    priced_signal_count: int
    trade_rows: int
    elapsed_seconds: float


def load_signal_price_history(
    store: DuckDBStore,
    options: ThirteenFSignalPriceOptions,
) -> DatasetLoadResult:
    tickers = tuple(
        row[0]
        for row in store.con.execute(
            """
            SELECT DISTINCT upper(trim(ticker))
            FROM v_thirteenf_signal_instruments
            WHERE ticker IS NOT NULL AND trim(ticker) <> ''
            ORDER BY 1
            """
        ).fetchall()
    )
    if not tickers:
        raise RuntimeError("No selected signal instruments; run map-13f-signal-instruments first")
    return TickerHistoryDataset().run(
        store,
        TickerHistoryOptions(
            zip_path=options.archive_path,
            symbols=tickers,
            start_date=options.start_date,
            end_date=options.end_date,
            chunk_size=options.chunk_size,
            max_chunks=None,
            price_projection_only=True,
            source=options.source,
            compute_source_hash=False,
            run_id=options.run_id,
        ),
    )


def ensure_thirteenf_backtest_schema(store: DuckDBStore) -> None:
    store.con.execute(
        """
        CREATE TABLE IF NOT EXISTS thirteenf_amendment_backtest_trades (
            trade_id VARCHAR PRIMARY KEY,
            signal_id VARCHAR NOT NULL,
            report_period DATE NOT NULL,
            cusip VARCHAR NOT NULL,
            ticker VARCHAR NOT NULL,
            price_source VARCHAR NOT NULL,
            price_security_id VARCHAR NOT NULL,
            signal_available_at TIMESTAMP NOT NULL,
            entry_date DATE NOT NULL,
            entry_available_at TIMESTAMP NOT NULL,
            entry_adjusted_close DOUBLE NOT NULL,
            horizon_trading_days INTEGER NOT NULL,
            exit_date DATE,
            exit_available_at TIMESTAMP,
            exit_adjusted_close DOUBLE,
            gross_long_return DOUBLE,
            gross_short_return DOUBLE,
            one_way_slippage_bps DOUBLE NOT NULL,
            net_short_return DOUBLE,
            is_complete BOOLEAN NOT NULL,
            is_stress_quarter BOOLEAN NOT NULL,
            is_latest_revision BOOLEAN NOT NULL DEFAULT true,
            run_id VARCHAR,
            source_loaded_at TIMESTAMP NOT NULL DEFAULT now()
        )
        """
    )
    store.con.execute(
        """
        CREATE INDEX IF NOT EXISTS idx_13f_backtest_horizon_period
        ON thirteenf_amendment_backtest_trades(horizon_trading_days, report_period)
        """
    )
    store.con.execute(
        """
        CREATE OR REPLACE VIEW v_thirteenf_amendment_backtest_summary AS
        SELECT
            horizon_trading_days,
            is_stress_quarter,
            count(*) FILTER (WHERE is_complete) AS completed_trades,
            avg(net_short_return) FILTER (WHERE is_complete) AS average_net_short_return,
            median(net_short_return) FILTER (WHERE is_complete) AS median_net_short_return,
            avg((net_short_return > 0)::INTEGER) FILTER (WHERE is_complete) AS short_win_rate
        FROM thirteenf_amendment_backtest_trades
        WHERE is_latest_revision
        GROUP BY horizon_trading_days, is_stress_quarter
        """
    )


def ensure_thirteenf_backtest_market_cap_schema(store: DuckDBStore) -> None:
    ensure_thirteenf_backtest_schema(store)
    store.con.execute(
        """
        ALTER TABLE thirteenf_amendment_backtest_trades
        ADD COLUMN IF NOT EXISTS entry_market_cap_usd DOUBLE
        """
    )


def refresh_thirteenf_signal_backtest(
    store: DuckDBStore,
    *,
    horizons: Sequence[int] = DEFAULT_HORIZONS,
    price_source: str = "tbltickerhistory3_10y",
    one_way_slippage_bps: float = 5.0,
    maximum_rank_per_quarter: int = DEFAULT_MAX_SIGNAL_RANK_PER_QUARTER,
    include_stress_quarters: bool = False,
    minimum_market_cap_usd: float = DEFAULT_MIDCAP_MIN_USD,
    maximum_market_cap_usd: float = DEFAULT_MIDCAP_MAX_USD,
    run_id: str | None = None,
) -> ThirteenFBacktestResult:
    if not horizons or any(horizon < 1 for horizon in horizons):
        raise ValueError("horizons must contain positive trading-day counts")
    if one_way_slippage_bps < 0:
        raise ValueError("one_way_slippage_bps must be non-negative")
    if maximum_rank_per_quarter < 1:
        raise ValueError("maximum_rank_per_quarter must be positive")
    if minimum_market_cap_usd <= 0 or maximum_market_cap_usd <= minimum_market_cap_usd:
        raise ValueError("market-cap bounds must be positive and increasing")
    clean_horizons = tuple(sorted(set(int(horizon) for horizon in horizons)))
    started = time.perf_counter()
    ensure_thirteenf_backtest_market_cap_schema(store)
    con = store.con
    horizon_values = ", ".join(f"({horizon})" for horizon in clean_horizons)

    with store.transaction():
        con.execute("DELETE FROM thirteenf_amendment_backtest_trades WHERE price_source = ?", [price_source])
        con.execute(
            f"""
            INSERT INTO thirteenf_amendment_backtest_trades (
                trade_id, signal_id, report_period, cusip, ticker, price_source,
                price_security_id, signal_available_at, entry_date,
                entry_available_at, entry_adjusted_close, entry_market_cap_usd,
                horizon_trading_days,
                exit_date, exit_available_at, exit_adjusted_close, gross_long_return,
                gross_short_return, one_way_slippage_bps, net_short_return,
                is_complete, is_stress_quarter, run_id
            )
            WITH mapped_signals AS (
                SELECT
                    s.signal_id,
                    s.report_period,
                    s.cusip,
                    s.signal_available_at,
                    s.is_stress_quarter,
                    upper(trim(m.ticker)) AS ticker
                FROM thirteenf_consensus_amendment_signals s
                JOIN v_thirteenf_signal_instruments m USING (cusip)
                WHERE s.is_latest_revision
                  AND s.signal_rank <= {int(maximum_rank_per_quarter)}
                  AND {"true" if include_stress_quarters else "NOT s.is_stress_quarter"}
            ),
            entry_candidates AS (
                SELECT
                    s.*,
                    b.source AS price_source,
                    b.security_id AS price_security_id,
                    b.trade_date AS entry_date,
                    b.available_at AS entry_available_at,
                    coalesce(b.adjusted_close, b.close) AS entry_adjusted_close,
                    raw.shares * coalesce(raw.close_pr, raw.close) AS entry_market_cap_usd,
                    row_number() OVER (
                        PARTITION BY s.signal_id
                        ORDER BY b.trade_date, coalesce(b.volume, 0) DESC, b.security_id
                    ) AS candidate_rank
                FROM mapped_signals s
                JOIN equity_daily_bars b
                  ON upper(trim(b.symbol)) = s.ticker
                 AND b.source = ?
                 AND b.trade_date > cast(s.signal_available_at AS DATE)
                 AND b.available_at > s.signal_available_at
                 AND coalesce(b.adjusted_close, b.close) > 0
                JOIN tbltickerhistory_daily raw
                  ON raw.source = b.source
                 AND raw.security_id = b.security_id
                 AND raw.trading_date = b.trade_date
                 AND raw.shares > 0
                 AND raw.shares * coalesce(raw.close_pr, raw.close) BETWEEN ? AND ?
            ),
            entries AS (
                SELECT * FROM entry_candidates WHERE candidate_rank = 1
            ),
            forward_prices AS (
                SELECT
                    e.signal_id,
                    b.trade_date,
                    b.available_at,
                    coalesce(b.adjusted_close, b.close) AS adjusted_close,
                    row_number() OVER (
                        PARTITION BY e.signal_id ORDER BY b.trade_date
                    ) - 1 AS trading_days_after_entry
                FROM entries e
                JOIN equity_daily_bars b
                  ON b.source = e.price_source
                 AND b.security_id = e.price_security_id
                 AND b.trade_date >= e.entry_date
                 AND coalesce(b.adjusted_close, b.close) > 0
            ),
            horizons(horizon) AS (VALUES {horizon_values})
            SELECT
                md5(e.signal_id || '|' || e.price_source || '|' || cast(h.horizon AS VARCHAR)),
                e.signal_id, e.report_period, e.cusip, e.ticker, e.price_source,
                e.price_security_id, e.signal_available_at, e.entry_date,
                e.entry_available_at, e.entry_adjusted_close, e.entry_market_cap_usd,
                h.horizon,
                p.trade_date, p.available_at, p.adjusted_close,
                p.adjusted_close / e.entry_adjusted_close - 1,
                1 - p.adjusted_close / e.entry_adjusted_close,
                {float(one_way_slippage_bps)},
                1 - p.adjusted_close / e.entry_adjusted_close
                    - 2 * {float(one_way_slippage_bps)} / 10000.0,
                p.trade_date IS NOT NULL,
                e.is_stress_quarter,
                ?
            FROM entries e
            CROSS JOIN horizons h
            LEFT JOIN forward_prices p
              ON p.signal_id = e.signal_id
             AND p.trading_days_after_entry = h.horizon
            """,
            [price_source, minimum_market_cap_usd, maximum_market_cap_usd, run_id],
        )

    signal_count_row = con.execute(
        f"""
        SELECT
            count(*),
            count(*) FILTER (
                WHERE EXISTS (
                    SELECT 1 FROM v_thirteenf_signal_instruments m WHERE m.cusip = s.cusip
                )
            )
        FROM thirteenf_consensus_amendment_signals s
        WHERE s.is_latest_revision
          AND s.signal_rank <= {int(maximum_rank_per_quarter)}
          AND {"true" if include_stress_quarters else "NOT s.is_stress_quarter"}
        """
    ).fetchone()
    if signal_count_row is None:
        raise RuntimeError("Could not count 13F signals")
    signal_count, mapped_signal_count = signal_count_row
    trade_count_row = con.execute(
        """
        SELECT count(DISTINCT signal_id), count(*)
        FROM thirteenf_amendment_backtest_trades
        WHERE price_source = ?
        """,
        [price_source],
    ).fetchone()
    if trade_count_row is None:
        raise RuntimeError("Could not count 13F backtest trades")
    priced_signal_count, trade_rows = trade_count_row
    return ThirteenFBacktestResult(
        signal_count=int(signal_count),
        mapped_signal_count=int(mapped_signal_count),
        priced_signal_count=int(priced_signal_count),
        trade_rows=int(trade_rows),
        elapsed_seconds=time.perf_counter() - started,
    )
