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


def test_build_index_writes_daily_and_monthly_rows(mi, tmp_path):
    con, m, cal = mi
    rec: dict = {}
    m.build_index(con, rec)
    out = pq.read_table(tmp_path / "market" / "index_returns.parquet").to_pylist()
    months = sorted({r["period_end"] for r in out if r["freq"] == "M"})
    assert months == [D(2019, 2, 28), D(2019, 3, 31)]           # January has no prior month-end weights
    assert {(r["universe"], r["weighting"]) for r in out} == {(u, w) for u in ("all_common", "member_common")
                                                             for w in ("VW", "EW")}
    daily = [r for r in out if r["freq"] == "D" and r["universe"] == "all_common" and r["weighting"] == "VW"]
    assert len(daily) == len(cal) - 1 and rec["index_rows"] == len(out)
    feb = next(r for r in out if r["freq"] == "M" and r["period_end"] == D(2019, 2, 28))
    assert feb["available_at"] == dt.datetime(2019, 2, 28, 22)


def test_holdout_blocks_return_statistics_after_2022(mi):
    con, m, _ = mi
    with pytest.raises(ValueError, match="D6"):
        m.factor_checks(con, D(2023, 1, 31))


def test_pearson():
    from atx_db.alpha_panel.market_index import pearson
    assert pearson([1, 2, 3, 4], [2, 4, 6, 8]) == pytest.approx(1.0)
    assert pearson([1, 2, 3, 4], [4, 3, 2, 1]) == pytest.approx(-1.0)
    assert pearson([1, 2], [1, 2]) is None


@pytest.fixture()
def hist(tmp_path, monkeypatch):
    """Pre-stage bars (rule crsp-index-history-v1): lines 1, 2 are in ``u`` on its first session, line 3 is not."""
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    from atx_db.alpha_panel import common, market_common, market_index
    importlib.reload(common)
    importlib.reload(market_common)
    importlib.reload(market_index)
    con = common.connect(memory="100MB", threads=1)
    con.execute("""CREATE TABLE u (session_date DATE, security_id BIGINT, cik BIGINT, is_primary BOOLEAN,
                   exchange VARCHAR, ret DOUBLE, ret_guarded BOOLEAN, member_common BOOLEAN, me DOUBLE)""")
    con.executemany("INSERT INTO u VALUES (?, 1, 1, true, 'XNYS', 0.0, false, true, 1.0)", [(D(2018, 3, 1),)])
    con.executemany("INSERT INTO u VALUES (?, 2, 2, true, 'XNYS', 0.0, false, true, 1.0)", [(D(2018, 3, 1),)])
    con.executemany("INSERT INTO u VALUES (?, 3, 3, true, 'XNYS', 0.0, false, true, 1.0)", [(D(2018, 3, 2),)])
    days = sessions(D(2017, 12, 1), D(2018, 2, 28))
    rows = []
    for i, d in enumerate(days):
        for sid, r, shares in ((1, 0.01, 10.0), (2, -0.01, 30.0), (3, 0.02, 1000.0)):
            guarded = sid == 2 and d == D(2018, 2, 14)
            rows.append((sid, d, 10 * (1 + r) ** i, shares, r if i else None, guarded))
    con.execute("""CREATE TABLE hb (security_id BIGINT, session_date DATE, close DOUBLE, shares_vendor DOUBLE,
                   ret DOUBLE, ret_guarded BOOLEAN)""")
    con.executemany("INSERT INTO hb VALUES (?, ?, ?, ?, ?, ?)", rows)
    return con, market_index, days


def test_history_months_weight_prior_month_end_vendor_me(hist):
    con, m, days = hist
    out = {r[0]: r for r in con.execute(m.history_monthly_sql("hb", D(2018, 1, 1), D(2018, 3, 1))).fetchall()}
    assert sorted(out) == [D(2018, 1, 1), D(2018, 2, 1)]                 # December is lookback only
    k = days.index(max(d for d in days if d.month == 12))
    jan = [d for d in days if d.month == 1]
    w1, w2 = 10 * 10 * 1.01 ** k, 30 * 10 * 0.99 ** k
    r1, r2 = 1.01 ** len(jan) - 1, 0.99 ** len(jan) - 1
    assert out[D(2018, 1, 1)][1] == pytest.approx((w1 * r1 + w2 * r2) / (w1 + w2))   # line 3 not in u at its start
    assert out[D(2018, 1, 1)][3] == 2
    feb = [d for d in days if d.month == 2]
    assert out[D(2018, 2, 1)][3] == 1                                    # line 2 has a guarded day in February
    assert out[D(2018, 2, 1)][1] == pytest.approx(1.01 ** len(feb) - 1)


def test_market_history_check_splices_stage_and_adds_rf_back(hist, monkeypatch):
    con, m, _ = hist
    monkeypatch.setattr(m, "HISTORY_START", D(2018, 1, 1))
    stage = {D(2018, 3, 1): 0.02, D(2018, 4, 1): -0.03, D(2018, 5, 1): 0.05}
    con.execute("CREATE TABLE idx (freq VARCHAR, period_end DATE, universe VARCHAR, vw DOUBLE)")
    for mo, v in stage.items():
        end = D(mo.year, mo.month + 1, 1) - dt.timedelta(days=1)
        con.execute("INSERT INTO idx VALUES ('M', ?, 'all_common', ?), ('M', ?, 'member_common', 0.0)", [end, v, end])
    hrows = {r[0]: r[1] for r in con.execute(m.history_monthly_sql("hb", D(2018, 1, 1), D(2018, 3, 1))).fetchall()}
    series = {**hrows, **stage}
    rf = {mo: 0.001 * (i + 1) for i, mo in enumerate(sorted(series))}
    ff3m = {mo: (v - rf[mo], 0.0, 0.0, rf[mo]) for mo, v in series.items()}   # French Mkt-RF = ours - RF
    res = m.market_history_check(con, D(2022, 12, 31), ff3m, bars_for=lambda splice: "hb")
    assert res["splice_month"] == "2018-03-01" and res["months"] == 5
    assert res["history_months"] == 2 and res["stage_months"] == 3
    assert res["monthly_rho"] == pytest.approx(1.0) and res["monthly_mean_diff"] == pytest.approx(0.0, abs=1e-12)
    assert res["missing_months"] == []
    none = m.market_history_check(con, D(2022, 12, 31), ff3m, bars_for=lambda splice: None)
    assert none["history_months"] == 0 and none["missing_months"] == ["2018-01-01", "2018-02-01"]
