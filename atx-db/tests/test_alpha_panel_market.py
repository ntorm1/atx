"""S3 market index returns (rule crsp-index-v1): lagged-ME value weights, monthly compounding, D6 holdout."""

from __future__ import annotations

import datetime as dt
import importlib

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

D = dt.date


def sessions(start: dt.date, end: dt.date) -> list[dt.date]:
    return [start + dt.timedelta(days=i) for i in range((end - start).days + 1)
            if (start + dt.timedelta(days=i)).weekday() < 5]


@pytest.fixture()
def mi(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    from atx_db.alpha_panel import common, market_common, market_index
    importlib.reload(common)
    importlib.reload(market_common)
    importlib.reload(market_index)
    cal = sessions(D(2019, 1, 2), D(2019, 3, 29))
    pq.write_table(pa.Table.from_pylist([{"session_date": d} for d in cal]), tmp_path / "calendar.parquet")
    con = common.connect(memory="100MB", threads=1)
    rows = []
    for i, d in enumerate(cal):
        # line 1: +1% a day, ME 100 -> grows; line 2: -1% a day, ME 300; line 3 is not a member in January
        for sid, r, me0 in ((1, 0.01, 100.0), (2, -0.01, 300.0), (3, 0.02, 50.0)):
            rows.append((d, sid, sid * 10, True, "XNYS", r if i else None, False,
                         sid != 3 or d >= D(2019, 2, 1), me0 * (1 + r) ** i))
    con.execute("""CREATE TABLE u (session_date DATE, security_id BIGINT, cik BIGINT, is_primary BOOLEAN,
                   exchange VARCHAR, ret DOUBLE, ret_guarded BOOLEAN, member_common BOOLEAN, me DOUBLE)""")
    con.executemany("INSERT INTO u VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", rows)
    return con, market_index, cal


def test_daily_vw_uses_previous_session_me(mi):
    con, m, cal = mi
    out = {(r[1], r[2]): r for r in con.execute(m.daily_sql()).fetchall()}
    d, prev = cal[5], cal[4]
    me1, me2, me3 = 100 * 1.01 ** 4, 300 * 0.99 ** 4, 50 * 1.02 ** 4
    vw = (me1 * 0.01 + me2 * -0.01) / (me1 + me2)
    r = out[(d, "member_common")]
    assert r[3] == pytest.approx(vw) and r[4] == pytest.approx(0.0) and r[5] == 2
    r = out[(d, "all_common")]
    assert r[3] == pytest.approx((me1 * 0.01 - me2 * 0.01 + me3 * 0.02) / (me1 + me2 + me3)) and r[5] == 3
    assert (cal[0], "all_common") not in out                     # no previous session in the universe


def test_monthly_weights_are_last_session_of_prior_month(mi):
    con, m, cal = mi
    out = {(r[1], r[2]): r for r in con.execute(m.monthly_sql()).fetchall()}
    feb = [d for d in cal if d.month == 2]
    jan_last = max(d for d in cal if d.month == 1)
    k = cal.index(jan_last)
    w1, w2 = 100 * 1.01 ** k, 300 * 0.99 ** k
    r1, r2 = 1.01 ** len(feb) - 1, 0.99 ** len(feb) - 1
    got = out[(D(2019, 2, 28), "member_common")]
    assert got[3] == pytest.approx((w1 * r1 + w2 * r2) / (w1 + w2)) and got[4] == pytest.approx((r1 + r2) / 2)
    assert got[5] == 2 and out[(D(2019, 3, 31), "member_common")][5] == 3   # line 3 member at end of February


def test_holdout_blocks_return_statistics_after_2022(mi):
    con, m, _ = mi
    with pytest.raises(ValueError, match="D6"):
        m.factor_checks(con, D(2023, 1, 31))


def test_pearson():
    from atx_db.alpha_panel.market_index import pearson
    assert pearson([1, 2, 3, 4], [2, 4, 6, 8]) == pytest.approx(1.0)
    assert pearson([1, 2, 3, 4], [4, 3, 2, 1]) == pytest.approx(-1.0)
    assert pearson([1, 2], [1, 2]) is None
