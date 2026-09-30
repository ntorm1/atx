"""S3.7 market/liquidity (rule liquidity-v1): Corwin-Schultz, Abdi-Ranaldo, Amihud, turnover, no-trade sessions."""

from __future__ import annotations

import datetime as dt
import importlib
import math
import random

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import liquidity as L

D = dt.date
T = dt.datetime
PRICE_SCHEMA = pa.schema([
    ("security_id", pa.int64()), ("session_date", pa.date32()), ("ticker", pa.string()), ("open", pa.float64()),
    ("high", pa.float64()), ("low", pa.float64()), ("close", pa.float64()), ("volume", pa.float64()),
    ("dollar_volume", pa.float64()), ("ret", pa.float64()), ("ret_guarded", pa.bool_()),
    ("return_factor", pa.float64()), ("fb_action", pa.string()), ("shares_vendor", pa.float64()),
    ("prev_raw_close", pa.float64()), ("gap_days", pa.int64())])


def sessions(start: dt.date, end: dt.date) -> list[dt.date]:
    return [start + dt.timedelta(days=i) for i in range((end - start).days + 1)
            if (start + dt.timedelta(days=i)).weekday() < 5]


def write(path, rows, schema=None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), path)


def test_cs_two_day_zero_for_flat_range_and_positive_for_wide_bounce():
    assert L.cs_two_day(10, 10, 10, 10, 10) == 0.0
    s = L.cs_two_day(10.2, 9.8, 10.0, 10.2, 9.8)
    assert 0 < s < 0.05
    # overnight gap: day t trades wholly above the prior close; its range is shifted down to the close
    assert L.cs_two_day(10.2, 9.8, 10.0, 12.2, 11.8) == pytest.approx(L.cs_two_day(10.2, 9.8, 10.0, 10.4, 10.0))
    assert L.cs_two_day(10, 0, 10, 10, 9) is None


def test_ar_term_sign():
    # close at the top of both ranges: positive bid-ask bounce term
    assert L.ar_term(10.2, 10.2, 9.8, 10.1, 9.9) > 0
    assert L.ar_term(10.0, 10.2, 9.8, 10.2, 9.8) == pytest.approx(0.0, abs=1e-6)


SPLIT = D(2018, 2, 1)
HOLE = D(2018, 2, 12)


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    cal = sessions(D(2018, 1, 2), D(2018, 2, 28))
    write(tmp_path / "calendar.parquet", [{"session_date": d, "vendor_rows": 5000, "vendor_ids": 5000} for d in cal])
    rng = random.Random(7)
    rows, prev = [], None
    for d in cal:
        if d == HOLE:
            continue
        basis = 0.5 if d >= SPLIT else 1.0
        mid = 40.0 * (1 + 0.02 * rng.uniform(-1, 1)) * basis
        hi, lo = mid * (1 + rng.uniform(0.002, 0.02)), mid * (1 - rng.uniform(0.002, 0.02))
        close = rng.uniform(lo, hi)
        vol = 0.0 if d == D(2018, 2, 20) else 1000.0 * rng.uniform(0.5, 1.5)
        f = 0.5 if d == SPLIT else 1.0
        ret = None if prev is None else close / (prev * f) - 1
        rows.append({"security_id": 16, "session_date": d, "ticker": "X", "open": mid, "high": hi, "low": lo,
                     "close": close, "volume": vol, "dollar_volume": close * vol, "ret": ret, "ret_guarded": False,
                     "return_factor": f, "fb_action": "none", "shares_vendor": None, "prev_raw_close": prev,
                     "gap_days": 1})
        prev = close
    write(tmp_path / "prices" / "year=2018" / "prices.parquet", rows, PRICE_SCHEMA)
    write(tmp_path / "market" / "shares_daily" / "year=2018" / "shares_daily.parquet",
          [{"security_id": 16, "session_date": d, "shrout": 1e6 if d < SPLIT else 2e6} for d in cal])
    return tmp_path, {r["session_date"]: r for r in rows}, cal


def test_liquidity_bucket_matches_reference_formulas(lake):
    root, bars, cal = lake
    from atx_db.alpha_panel import common, market_common
    importlib.reload(common)
    importlib.reload(market_common)
    importlib.reload(L)
    L.build(buckets=[0])
    out = {r["session_date"]: r for r in pq.read_table(root / "_tmp" / "mkt" / "liquidity" / "bucket=00.parquet").to_pylist()}
    assert len(out) == len(cal)                                   # the untraded session is a row
    hole = out[HOLE]
    assert hole["has_bar"] is False and hole["halt_proxy"] is True
    assert out[D(2018, 2, 20)]["halt_proxy"] is True               # a bar with zero volume
    d = D(2018, 2, 28)
    win = cal[cal.index(d) - 20: cal.index(d) + 1]
    cs, ar, il = [], [], []
    prev_win = cal[cal.index(d) - 21: cal.index(d) + 1]         # pairs whose second day is in the window
    for a, b in zip(prev_win, prev_win[1:]):
        if a not in bars or b not in bars:
            continue
        x, y = bars[a], bars[b]
        f = y["return_factor"]
        cs.append(L.cs_two_day(x["high"] * f, x["low"] * f, x["close"] * f, y["high"], y["low"]))
        ar.append(L.ar_term(x["close"] * f, x["high"] * f, x["low"] * f, y["high"], y["low"]))
    for t in win:
        r = bars.get(t)
        if r and r["dollar_volume"] > 0 and r["ret"] is not None:
            il.append(abs(r["ret"]) / r["dollar_volume"] * 1e6)
    got = out[d]
    assert got["spread_cs_21"] == pytest.approx(sum(cs) / len(cs), rel=1e-9) and got["n_pairs_cs_21"] == len(cs)
    assert got["spread_ar_21"] == pytest.approx(math.sqrt(max(sum(ar) / len(ar), 0)), rel=1e-9)
    assert got["amihud_21"] == pytest.approx(sum(il) / len(il), rel=1e-9)
    assert got["halt_days_21"] == 2 and got["zero_vol_21"] == pytest.approx(2 / 21)
    vols = [bars[t]["volume"] / (1e6 if t < SPLIT else 2e6) if t in bars else 0.0 for t in win]
    assert got["turnover_21"] == pytest.approx(sum(vols) / 21)
    adv = [bars[t]["dollar_volume"] if t in bars else 0.0 for t in win]
    assert got["adv_21"] == pytest.approx(sum(adv) / 21)
    assert got["available_at"] == T(2018, 2, 28, 22)
    assert out[D(2018, 1, 10)]["spread_cs_21"] is None            # fewer than 10 pairs
