from __future__ import annotations

import duckdb
import pytest

from atx_db.connection import DuckDBStore
from atx_db.openfigi_signals import ensure_openfigi_signal_schema
from atx_db.thirteenf_backtest import ensure_thirteenf_backtest_schema, refresh_thirteenf_signal_backtest
from atx_db.thirteenf_signals import ensure_thirteenf_signal_schema


def test_backtest_enters_after_signal_and_uses_trading_horizons() -> None:
    store = DuckDBStore(":memory:")
    store.connection = duckdb.connect(":memory:")
    try:
        ensure_thirteenf_signal_schema(store)
        ensure_openfigi_signal_schema(store)
        ensure_thirteenf_backtest_schema(store)
        store.con.execute(
            """
                CREATE TABLE equity_daily_bars (
                    source VARCHAR, security_id VARCHAR, symbol VARCHAR, trade_date DATE,
                    available_at TIMESTAMP, adjusted_close DOUBLE, close DOUBLE, volume BIGINT,
                    market_cap_usd DOUBLE
            )
            """
        )
        store.con.execute(
            """
            CREATE TABLE tbltickerhistory_daily (
                source VARCHAR, security_id VARCHAR, trading_date DATE,
                shares BIGINT, close_pr DOUBLE, close DOUBLE
            )
            """
        )
        store.con.execute(
            """
            INSERT INTO thirteenf_consensus_amendment_signals (
                signal_id, report_period, cusip, signal_available_at,
                distinct_filer_count, average_zscore, minimum_zscore,
                maximum_zscore, corrected_position_events, manager_ciks_json,
                signal_rank, is_stress_quarter
            ) VALUES (
                'signal', DATE '2020-03-31', '037833100', TIMESTAMP '2020-05-20 23:00:00',
                3, 4.0, 3.0, 5.0, 3, '[]', 1, false
            )
            """
        )
        store.con.execute(
            """
            INSERT INTO thirteenf_signal_instrument_candidates (
                mapping_id, cusip, figi, ticker, selected, mapping_status,
                available_at, source
            ) VALUES (
                'mapping', '037833100', 'BBG000B9XRY4', 'AAPL', true, 'mapped',
                TIMESTAMP '2020-05-20', 'OpenFIGI v3'
            )
            """
        )
        store.con.execute(
            """
            INSERT INTO equity_daily_bars
            SELECT
                'test_prices', 'price-security', 'AAPL',
                DATE '2020-05-21' + i::INTEGER,
                cast(DATE '2020-05-21' + i::INTEGER AS TIMESTAMP) + INTERVAL 22 HOUR,
                    100.0 - i, 100.0 - i, 1000000, 50_000_000 * (100.0 - i)
            FROM range(0, 48) values_(i)
            """
        )
        store.con.execute(
            """
            INSERT INTO tbltickerhistory_daily
            SELECT 'test_prices', 'price-security', DATE '2020-05-21' + i::INTEGER,
                   50_000_000, 100.0 - i, 100.0 - i
            FROM range(0, 48) values_(i)
            """
        )

        result = refresh_thirteenf_signal_backtest(
            store,
            horizons=(5, 47),
            price_source="test_prices",
            one_way_slippage_bps=5,
            run_id="test-backtest",
        )

        assert (result.signal_count, result.mapped_signal_count, result.priced_signal_count) == (1, 1, 1)
        rows = store.con.execute(
            """
            SELECT horizon_trading_days, entry_date, exit_date, gross_short_return,
                   net_short_return, entry_market_cap_usd
            FROM thirteenf_amendment_backtest_trades
            ORDER BY horizon_trading_days
            """
        ).fetchall()
        assert rows[0][0] == 5
        assert str(rows[0][1]) == "2020-05-21"
        assert str(rows[0][2]) == "2020-05-26"
        assert rows[0][3] == pytest.approx(0.05)
        assert rows[0][4] == pytest.approx(0.049)
        assert rows[0][5] == pytest.approx(5_000_000_000.0)
        assert rows[1][0] == 47
        assert rows[1][3] == pytest.approx(0.47)
    finally:
        store.connection.close()
        store.connection = None
