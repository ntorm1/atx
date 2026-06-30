"""Tests for the PIT investable-universe builder (``universe_memberships``).

The builder applies a trailing-liquidity screen over canonical daily bars and emits
one membership row per (security, qualifying day) with a point-in-time ``available_at``.
Membership is decided per-day, so a name that drops below the screen stops being a
member without being retroactively removed from the days it qualified (no survivorship
bias). No network: builds from in-memory fixture bars.
"""
from __future__ import annotations

import datetime as dt

import pandas as pd

from db.universes import UniverseBuildOptions, UniverseMembershipDataset
from db.warehouse import insert_frame


def _bars() -> pd.DataFrame:
    """Two securities over 4 days. LIQ is liquid throughout; THIN drops below the
    dollar-volume floor on the last day."""
    rows = []
    dates = [dt.date(2012, 4, 2), dt.date(2012, 4, 3), dt.date(2012, 4, 4), dt.date(2012, 4, 5)]
    for d in dates:
        rows.append(dict(source="s", security_id="LIQ", vendor_security_id="1", symbol="LIQ",
                         trade_date=d, open=50.0, high=51.0, low=49.0, close=50.0,
                         adjusted_close=50.0, volume=1_000_000, vwap=None, dividend_amount=None,
                         split_factor=1.0, is_adjusted=False,
                         available_at=pd.Timestamp(d) + pd.Timedelta(hours=22), run_id="t"))
    # THIN: liquid first 3 days, then volume collapses on day 4.
    for i, d in enumerate(dates):
        vol = 1_000_000 if i < 3 else 1
        rows.append(dict(source="s", security_id="THIN", vendor_security_id="2", symbol="THIN",
                         trade_date=d, open=50.0, high=51.0, low=49.0, close=50.0,
                         adjusted_close=50.0, volume=vol, vwap=None, dividend_amount=None,
                         split_factor=1.0, is_adjusted=False,
                         available_at=pd.Timestamp(d) + pd.Timedelta(hours=22), run_id="t"))
    return pd.DataFrame(rows)


def test_broad_universe_is_pit_and_survivorship_safe(tmp_store):
    insert_frame(tmp_store, _bars(), "equity_daily_bars", "uni_bars_load")
    options = UniverseBuildOptions(
        universe_id="test_uni",
        symbols=None,  # broad: all securities with bars
        lookback_days=1,
        min_history_days=1,
        min_price=5.0,
        min_dollar_volume=10_000_000.0,
    )
    UniverseMembershipDataset().run(tmp_store, options)
    con = tmp_store.con

    # available_at is the bar close (PIT), never null.
    assert con.execute(
        "SELECT COUNT(*) FROM universe_memberships WHERE universe_id='test_uni' AND available_at IS NULL"
    ).fetchone()[0] == 0

    # LIQ is liquid every day: 4 memberships.
    liq = con.execute(
        "SELECT COUNT(*) FROM universe_memberships WHERE universe_id='test_uni' AND security_id='LIQ'"
    ).fetchone()[0]
    assert liq == 4

    # THIN qualifies on the first three days; its dollar volume collapses on day 4.
    thin_days = [
        r[0]
        for r in con.execute(
            "SELECT as_of_date FROM universe_memberships WHERE universe_id='test_uni' "
            "AND security_id='THIN' ORDER BY as_of_date"
        ).fetchall()
    ]
    assert thin_days == [dt.date(2012, 4, 2), dt.date(2012, 4, 3), dt.date(2012, 4, 4)]
    # Survivorship: THIN stays a member on the days it qualified even though it drops on day 4.
    assert dt.date(2012, 4, 5) not in thin_days
