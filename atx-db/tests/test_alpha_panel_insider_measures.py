"""Insider measures: the CMP routine / opportunistic rule (Python twin vs SQL) and the monthly net-buying aggregate."""

from __future__ import annotations

import datetime as dt
import itertools
from pathlib import Path

import duckdb

from atx_db.alpha_panel import insider_measures as M


def test_cmp_label_python() -> None:
    routine = {(2016, 3), (2017, 3), (2018, 3), (2018, 7)}
    assert M.cmp_label(routine, 2019) == "routine"
    opp = {(2016, 3), (2017, 4), (2018, 3)}
    assert M.cmp_label(opp, 2019) == "opportunistic"
    assert M.cmp_label({(2017, 3), (2018, 3)}, 2019) == "unclassified"
    assert M.cmp_label(routine, 2020) == "unclassified"                   # needs 2017, 2018 and 2019


def _trades(con: duckdb.DuckDBPyConnection, rows: list[tuple]) -> None:
    con.execute("""CREATE TABLE t (owner_cik BIGINT, transaction_date DATE, available_at TIMESTAMPTZ)""")
    con.executemany("INSERT INTO t VALUES (?, ?, ?)", rows)


def test_label_sql_matches_python_twin() -> None:
    con = duckdb.connect()
    con.execute("SET TimeZone = 'UTC'")
    at = lambda d, lag=2: dt.datetime.combine(d + dt.timedelta(days=lag), dt.time(21), tzinfo=dt.UTC)  # noqa: E731
    rows = []
    # owner 1: March every year -> routine in 2019; owner 2: different months -> opportunistic
    for y in (2016, 2017, 2018, 2019):
        rows.append((1, dt.date(y, 3, 10), at(dt.date(y, 3, 10))))
        rows.append((2, dt.date(y, 1 + (y % 12), 5), at(dt.date(y, 1 + (y % 12), 5))))
    # owner 3: the 2018 trade was only public in January 2019 (late Form 5) -> not history for 2019
    for y in (2016, 2017):
        rows.append((3, dt.date(y, 6, 1), at(dt.date(y, 6, 1))))
    rows.append((3, dt.date(2018, 12, 28), at(dt.date(2018, 12, 28), lag=20)))
    rows.append((3, dt.date(2019, 6, 1), at(dt.date(2019, 6, 1))))
    # owner 4: gap year
    rows += [(4, dt.date(2016, 5, 1), at(dt.date(2016, 5, 1))), (4, dt.date(2018, 5, 1), at(dt.date(2018, 5, 1))),
             (4, dt.date(2019, 5, 1), at(dt.date(2019, 5, 1)))]
    _trades(con, rows)
    con.execute(M.HIST_SQL)
    con.execute(M.LABEL_SQL)
    sql = {(o, y): lab for o, y, lab in con.execute("SELECT owner_cik, y, cmp_label FROM lab").fetchall()}
    hist = {}
    for o, d, a in rows:
        if a < dt.datetime(d.year + 1, 1, 1, tzinfo=dt.UTC):
            hist.setdefault(o, set()).add((d.year, d.month))
    for (o, y), lab in sql.items():
        assert lab == M.cmp_label(hist.get(o, set()), y), (o, y)
    assert sql[(1, 2019)] == "routine" and sql[(2, 2019)] == "opportunistic"
    assert sql[(3, 2019)] == "unclassified" and sql[(4, 2019)] == "unclassified"


def test_monthly_net_buying(tmp_path: Path) -> None:
    con = duckdb.connect()
    con.execute("SET TimeZone = 'UTC'")
    p = tmp_path / "trades.parquet"
    base = dt.datetime(2024, 3, 5, 21, tzinfo=dt.UTC)
    rows = [  # issuer, owner, code, shares, value, label, officer, director, 10b5-1, available_at
        (10, 1, "P", 100.0, 1000.0, "opportunistic", True, False, None, base),
        (10, 2, "P", 50.0, None, "routine", False, True, False, base + dt.timedelta(days=1)),
        (10, 3, "S", 30.0, 600.0, "opportunistic", False, False, True, base + dt.timedelta(days=2)),
        (10, 1, "S", 10.0, 100.0, "opportunistic", True, False, False, dt.datetime(2024, 4, 1, 0, 30, tzinfo=dt.UTC)),
        (11, 5, "P", 1.0, 1.0, "unclassified", False, False, None, dt.datetime(2018, 12, 31, 23, tzinfo=dt.UTC)),
    ]
    con.execute("""CREATE TABLE x (issuer_cik BIGINT, owner_cik BIGINT, transaction_code VARCHAR, shares DOUBLE, value DOUBLE,
                   cmp_label VARCHAR, officer BOOLEAN, director BOOLEAN, aff10b5one BOOLEAN, available_at TIMESTAMPTZ)""")
    con.executemany("INSERT INTO x VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", rows)
    con.execute(f"COPY x TO '{p.as_posix()}' (FORMAT parquet)")
    res = con.execute(M.monthly_sql(p.as_posix())).fetchall()
    cols = [c[0] for c in con.description]
    out = [dict(zip(cols, r, strict=True)) for r in res]
    assert [(r["issuer_cik"], r["month"]) for r in out] == [(10, dt.date(2024, 3, 1)), (10, dt.date(2024, 4, 1))]  # 2018 dropped
    m = out[0]
    assert (m["n_buy"], m["n_sell"], m["net_shares"], m["net_value"]) == (2, 1, 120.0, 400.0)
    assert (m["n_buyers"], m["n_sellers"]) == (2, 1)
    assert (m["opp_n_buy"], m["opp_n_sell"], m["opp_net_value"], m["opp_net_buyers"]) == (1, 1, 400.0, 0)
    assert (m["rtn_n_buy"], m["rtn_net_value"]) == (1, 0.0)                 # value unknown (no price) -> 0 in sums
    assert (m["od_n_buy"], m["od_n_sell"], m["od_net_value"]) == (2, 0, 1000.0)
    assert m["share_10b5_1"] == 0.5 and m["available_at"] == base + dt.timedelta(days=2)
    assert out[1]["net_shares"] == -10.0
    assert list(itertools.chain(cols))[-1] == "cmp_rule"
