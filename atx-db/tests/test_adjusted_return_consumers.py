"""Small source-contract fixtures; no warehouse bootstrap or live data."""
from __future__ import annotations

import datetime as dt
import json
from types import SimpleNamespace

import duckdb
import numpy as np
import pandas as pd
import pytest

from atx_db.earnings_surprise import FACTOR_ID as SUE_ID
from atx_db.earnings_surprise import SOURCE_NAME as SUE_SOURCE
from atx_db.equity_price_metrics import (
    EquityPriceMetricsOptions,
    compute_equity_price_metrics,
    load_price_inputs,
)
from atx_db.filing_reaction import load_filing_reaction_inputs
from atx_db.fundamental_momentum import (
    FundamentalMomentumOptions,
    load_fundamental_momentum_inputs,
)
from atx_db.profitability_trend import FACTOR_ID as TREND_ID
from atx_db.profitability_trend import SOURCE_NAME as TREND_SOURCE
from atx_db.signal_eval import (
    _derive_forward_returns_from_prices,
    _derive_same_day_returns_from_prices,
)
from atx_db.twin_momentum import TwinMomentumOptions, load_twin_momentum_inputs


@pytest.fixture
def price_store():
    con = duckdb.connect(":memory:", config={"threads": 1, "memory_limit": "128MB"})
    con.execute("""
        CREATE TABLE equity_daily_bars (
            source VARCHAR, security_id VARCHAR, symbol VARCHAR, trade_date DATE,
            open DOUBLE, close DOUBLE, adjusted_close DOUBLE, volume DOUBLE,
            split_factor DOUBLE, available_at TIMESTAMP, source_loaded_at TIMESTAMP
        );
        CREATE TABLE fundamental_factor_values (
            factor_value_id VARCHAR, factor_id VARCHAR, security_id VARCHAR, symbol VARCHAR,
            as_of_date DATE, value DOUBLE, available_at TIMESTAMP, input_lineage_json VARCHAR,
            is_latest_revision BOOLEAN, source VARCHAR, source_loaded_at TIMESTAMP
        );
        CREATE TABLE fundamental_statement_points (
            statement_point_id VARCHAR, security_id VARCHAR, as_of_date DATE,
            available_at TIMESTAMP, accession_number VARCHAR, form VARCHAR
        )
    """)
    try:
        yield SimpleNamespace(con=con)
    finally:
        con.close()


def _bars(store, sid, closes, adjusted, *, late_clock=None):
    for i, (close, adj) in enumerate(zip(closes, adjusted, strict=True)):
        day = dt.date(2020, 1, 2) + dt.timedelta(days=i)
        clock = late_clock if i == 0 and late_clock else dt.datetime.combine(day, dt.time(22))
        store.con.execute(
            "INSERT INTO equity_daily_bars VALUES ('fixture', ?, ?, ?, ?, ?, ?, 1000, .5, ?, ?)",
            [sid, sid, day, close, close, adj, clock, clock],
        )


def _parent(store, sid, factor_id, source, *, date=dt.date(2020, 1, 6)):
    store.con.execute(
        "INSERT INTO fundamental_factor_values VALUES (?, ?, ?, ?, ?, 1, ?, ?, true, ?, ?)",
        [f"{sid}-{factor_id}", factor_id, sid, sid, date,
         dt.datetime.combine(date, dt.time(22)),
         json.dumps({"current": {"statement_point_id": sid}}), source,
         dt.datetime.combine(date, dt.time(22))],
    )


@pytest.mark.parametrize(
    ("closes", "adjusted", "expected"),
    [([100, 50], [50, 50], 0), ([100, 99], [98, 99], 1 / 98),
     ([100, 110], [100, 110], .1)],
)
def test_return_readers_use_adjusted_endpoints_and_raw_turnover(price_store, closes, adjusted, expected):
    _bars(price_store, "A", closes, adjusted)
    bars = load_price_inputs(price_store, EquityPriceMetricsOptions())
    metrics = compute_equity_price_metrics(bars).sort_values("trade_date")
    assert metrics.iloc[-1].daily_return == pytest.approx(expected)
    assert metrics.iloc[-1].gap_return == pytest.approx(expected)
    assert metrics.iloc[-1].dollar_volume == closes[-1] * 1000
    assert metrics.iloc[0].adjusted_close == adjusted[0]
    forward = _derive_forward_returns_from_prices(price_store, horizons=(1,), panel=None)
    assert forward.iloc[0].forward_return == pytest.approx(expected)
    same_day = _derive_same_day_returns_from_prices(price_store)
    assert same_day.iloc[-1].same_day_return == pytest.approx(expected)
    assert load_price_inputs(price_store, EquityPriceMetricsOptions(symbols=())).empty
    assert _derive_forward_returns_from_prices(price_store, horizons=(1,), panel=pd.DataFrame()).empty


@pytest.mark.parametrize("invalid", [None, 0, -1, float("inf"), float("nan")])
def test_missing_adjustments_preserve_observation_positions(price_store, invalid):
    _bars(price_store, "A", [100, 101, 102], [100, invalid, 102])
    metrics = compute_equity_price_metrics(load_price_inputs(price_store, EquityPriceMetricsOptions()))
    assert metrics.daily_return.isna().all()
    assert metrics.gap_return.isna().all()
    forward = _derive_forward_returns_from_prices(price_store, horizons=(1, 2))
    assert forward.horizon.tolist() == [2]
    assert forward.iloc[0].forward_return == pytest.approx(.02)
    assert _derive_same_day_returns_from_prices(price_store).same_day_return.isna().all()


def test_latest_invalid_revision_does_not_resurrect_valid_price(price_store):
    _bars(price_store, "A", [100, 110], [100, 110])
    price_store.con.execute("""
        INSERT INTO equity_daily_bars
        SELECT 'correction', security_id, symbol, trade_date, open, close, NULL,
               volume, NULL, available_at + INTERVAL 1 HOUR, source_loaded_at + INTERVAL 1 HOUR
        FROM equity_daily_bars WHERE trade_date = DATE '2020-01-03'
    """)
    assert _derive_forward_returns_from_prices(price_store, horizons=(1,)).empty


@pytest.mark.parametrize("loader, options, factor_id, source", [
    (load_fundamental_momentum_inputs, FundamentalMomentumOptions, SUE_ID, SUE_SOURCE),
    (load_twin_momentum_inputs, TwinMomentumOptions, TREND_ID, TREND_SOURCE),
])
def test_momentum_loaders_use_adjusted_close_without_shortening_history(
    price_store, loader, options, factor_id, source,
):
    _parent(price_store, "A", factor_id, source)
    _bars(price_store, "A", [100, 50, 55, 60, 66], [50, 50, 55, 60, 66])
    opts = options(skip_sessions=1, lookback_sessions=3, minimum_names_per_date=3)
    result = loader(price_store, opts)
    assert result.iloc[0].price_momentum_12_1 == pytest.approx(.2)
    # Latest reference date has an invalid end point. An earlier valid reference
    # date must not substitute for it, and NULL must not shorten the price grid.
    price_store.con.execute("UPDATE equity_daily_bars SET adjusted_close=NULL WHERE trade_date=DATE '2020-01-05'")
    assert loader(price_store, opts).empty
    price_store.con.execute("DELETE FROM fundamental_factor_values")
    assert loader(price_store, opts).empty


@pytest.mark.parametrize("invalid_first_reaction", [False, True])
def test_filing_reaction_adjusted_prices_first_session_and_input_clocks(price_store, invalid_first_reaction):
    for sid in ("A", "B", "C"):
        _parent(price_store, sid, SUE_ID, SUE_SOURCE, date=dt.date(2020, 1, 31))
        price_store.con.execute(
            "INSERT INTO fundamental_statement_points VALUES (?, ?, DATE '2020-01-02', TIMESTAMP '2020-01-02 22:00:00', ?, '10-Q')",
            [sid, sid, f"{sid}-filing"],
        )
    late = dt.datetime(2020, 1, 5, 22)
    _bars(price_store, "A", [100, 50, 51], [50, None if invalid_first_reaction else 50, 51])
    _bars(price_store, "B", [100, 100, 100], [100, 100, 100], late_clock=late)
    _bars(price_store, "C", [100, 99, 100], [98, 99, 100])
    result = load_filing_reaction_inputs(price_store).set_index("security_id")
    if invalid_first_reaction:
        assert "A" not in result.index
    else:
        assert result.loc["A", "daily_return"] == pytest.approx(0)
        assert result.loc["C", "abnormal_return"] == pytest.approx(1 / 98)
    assert result.loc["C", "daily_return"] == pytest.approx(1 / 98)
    assert result.loc["C", "reaction_available_at"] == late
    assert result.loc["C", "reaction_date"].date() == dt.date(2020, 1, 3)


def test_price_metrics_carries_prior_input_clock_and_no_adjustment_fallback():
    bars = pd.DataFrame({
        "security_id": ["A", "A"], "trade_date": ["2020-01-02", "2020-01-03"],
        "open": [100, 110], "close": [100, 110], "adjusted_close": [100, 110],
        "volume": [1000, 1000], "available_at": ["2020-01-07", "2020-01-03"],
    })
    result = compute_equity_price_metrics(bars)
    assert result.iloc[-1].available_at == pd.Timestamp("2020-01-07")
    missing = compute_equity_price_metrics(bars.drop(columns="adjusted_close"))
    assert missing.daily_return.isna().all()
    assert np.isfinite(missing.dollar_volume).all()
