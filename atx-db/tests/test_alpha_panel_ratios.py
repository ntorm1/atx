"""Stage ratios on fixtures: TTM from four fundq quarters (missing quarter -> NULL, never 0), the fiscal-year path at
a 10-K, YTD cash-flow TTM, balance averages, EPS split basis, issuer market equity, GC1 cutoffs (monthly m 22:00,
daily d-1 22:00), staleness, restatement vintages, composite scores, the daily as-of macro and the gates."""

from __future__ import annotations

import datetime as dt
import math

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import ratios as R

T = dt.datetime
D = dt.date

Q_ITEMS = sorted(set(R.FLOW_Q.values()) | set(R.EPS_Q.values()) | set(R.YTD_Q.values()) | set(R.BAL_Q.values()))
A_ITEMS = sorted({c for v in R.ANN_LAGS.values() for c in v})
DESC = [("cik", pa.int64()), ("datadate", pa.date32()), ("fqtr", pa.int64()), ("fyr", pa.int64()),
        ("accession", pa.string()), ("form", pa.string()), ("filed", pa.date32()), ("available_at", pa.timestamp("us")),
        ("fin_template", pa.string()), ("sic", pa.int32()), ("curcd", pa.string()), ("fx_converted", pa.bool_()),
        ("first_reported", pa.bool_())]
FQ_SCHEMA = pa.schema(DESC + [(c, pa.float64()) for c in Q_ITEMS])
FA_SCHEMA = pa.schema(DESC + [(c, pa.float64()) for c in A_ITEMS])


def qdates(year0: int, n: int) -> list[D]:
    ends = [(3, 31), (6, 30), (9, 30), (12, 31)]
    return [D(year0 + i // 4, *ends[i % 4]) for i in range(n)]


class World:
    def __init__(self, root):
        self.root = root
        self.fq, self.fa, self.links, self.px, self.sh, self.splits = [], [], [], [], [], []

    def q(self, cik, dd, fqtr, accn, avail, form="10-Q", **items):
        r = {f.name: None for f in FQ_SCHEMA}
        r.update({"cik": cik, "datadate": dd, "fqtr": fqtr, "fyr": 12, "accession": accn, "form": form,
                  "filed": avail.date(), "available_at": avail, "fin_template": "other", "sic": 3570, "curcd": "USD",
                  "fx_converted": False, "first_reported": True})
        r.update(items)
        self.fq.append(r)
        return r

    def a(self, cik, dd, accn, avail, form="10-K", **items):
        r = {f.name: None for f in FA_SCHEMA}
        r.update({"cik": cik, "datadate": dd, "fqtr": 4, "fyr": 12, "accession": accn, "form": form,
                  "filed": avail.date(), "available_at": avail, "fin_template": "other", "sic": 3570, "curcd": "USD",
                  "fx_converted": False, "first_reported": True})
        r.update(items)
        self.fa.append(r)

    def link(self, sid, cik, prim="P", tier="strict", start=D(2018, 1, 1)):
        self.links.append({"security_id": sid, "cik": cik, "valid_from": start, "valid_to": None, "link_tier": tier,
                           "linkprim": prim, "available_at": T(2017, 12, 29, 22)})

    def write(self, sessions):
        root = self.root
        pq.write_table(pa.table({"session_date": pa.array(sessions, pa.date32())}), root / "calendar.parquet")
        st = root / "compustat_std"
        st.mkdir(parents=True, exist_ok=True)
        pq.write_table(pa.Table.from_pylist(self.fq, schema=FQ_SCHEMA), st / "fundq.parquet")
        pq.write_table(pa.Table.from_pylist(self.fa, schema=FA_SCHEMA), st / "funda.parquet")
        (st / "manifest.json").write_text("{}")
        (root / "identity").mkdir(exist_ok=True)
        pq.write_table(pa.Table.from_pylist(self.links, schema=pa.schema(
            [("security_id", pa.int64()), ("cik", pa.int64()), ("valid_from", pa.date32()), ("valid_to", pa.date32()),
             ("link_tier", pa.string()), ("linkprim", pa.string()), ("available_at", pa.timestamp("us"))])),
            root / "identity" / "link_table_v3.parquet")
        (root / "corporate_actions").mkdir(exist_ok=True)
        pq.write_table(pa.Table.from_pylist(self.splits, schema=pa.schema(
            [("security_id", pa.int64()), ("ex_date", pa.date32()), ("kind", pa.string()),
             ("split_ratio", pa.float64())])), root / "corporate_actions" / "vendor_events.parquet")
        (root / "security_master").mkdir(exist_ok=True)
        pq.write_table(pa.table({"security_id": pa.array([901, 901, 100], pa.int64()),
                                 "finra_type": ["warrant", "warrant", "common"]}),
                       root / "security_master" / "finra_names.parquet")
        years = sorted({s.year for s in sessions})
        for y in years:
            p = root / "prices" / f"year={y}"
            p.mkdir(parents=True, exist_ok=True)
            rows = [r for r in self.px if r["session_date"].year == y]
            pq.write_table(pa.Table.from_pylist(rows, schema=pa.schema(
                [("session_date", pa.date32()), ("security_id", pa.int64()), ("close", pa.float64())])),
                p / "prices.parquet")
            s = root / "market" / "shares_daily" / f"year={y}"
            s.mkdir(parents=True, exist_ok=True)
            rows = [r for r in self.sh if r["session_date"].year == y]
            pq.write_table(pa.Table.from_pylist(rows, schema=pa.schema(
                [("session_date", pa.date32()), ("security_id", pa.int64()), ("shrout", pa.float64())])),
                s / "shares_daily.parquet")


def bdays(start: D, end: D) -> list[D]:
    out, d = [], start
    while d <= end:
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


SESSIONS = bdays(D(2018, 1, 2), D(2020, 10, 5))


def sale(i):          # discrete quarterly sales of quarter index i (2018Q1 = 0)
    return 100.0 + 10 * i


def base_items(i):
    return {"saleq": sale(i), "cogsq": 40.0 + 2 * i, "xsgaq": 10.0, "oiadpq": 30.0 + i, "oibdpq": 35.0 + i,
            "dpq": 5.0, "xintq": 2.0, "txtq": 6.0, "niq": 20.0 + i, "ibq": 20.0 + i, "epspxq": 0.20 + 0.01 * i,
            "epsfxq": 0.19 + 0.01 * i, "atq": 1000.0 + 10 * i, "ltq": 600.0, "ceqq": 390.0 + 10 * i,
            "seqq": 400.0 + 10 * i, "cheq": 50.0, "actq": 300.0, "lctq": 150.0, "invtq": 60.0 + i, "rectq": 80.0,
            "apq": 40.0, "ppentq": 200.0, "dlcq": 20.0, "dlttq": 200.0, "pstkq": 10.0, "mibq": 0.0,
            "cshoq": 100.0}


def ytd(i, per_q):    # fiscal YTD (calendar fiscal year) of a per-quarter flow
    k = i % 4
    return per_q * (k + 1)


ANN_FIELDS = dict(sale=0.0, cogs=0.0, xsga=40.0, oiadp=0.0, oibdp=0.0, dp=20.0, xint=8.0, ni=0.0, ib=0.0, pi=0.0,
                  txt=24.0, xad=12.0, at=0.0, lt=600.0, act=300.0, lct=150.0, rect=80.0, ppent=200.0, dltt=200.0,
                  re=150.0, oancf=0.0, capx=0.0, dvc=0.0, xrd=0.0, sstk=0.0, prstkc=0.0, txditc=7.0, epsfx=0.0,
                  epspx=0.0, epsfi=0.0)


def annual(i_q4):     # funda row of the fiscal year ending at quarter index i_q4 (fqtr 4)
    qs = range(i_q4 - 3, i_q4 + 1)
    f = dict(ANN_FIELDS)
    f.update(sale=sum(sale(i) for i in qs) + 1.0,       # +1: proves the fiscal-year path at a 10-K
             cogs=sum(40.0 + 2 * i for i in qs), oiadp=sum(30.0 + i for i in qs), oibdp=sum(35.0 + i for i in qs),
             ni=sum(20.0 + i for i in qs), ib=sum(20.0 + i for i in qs), pi=sum(26.0 + i for i in qs),
             at=1000.0 + 10 * i_q4, oancf=4 * 25.0, capx=4 * 8.0, dvc=4 * 3.0, xrd=4 * 4.0, sstk=0.0,
             prstkc=4 * 1.0, epsfx=sum(0.19 + 0.01 * i for i in qs), epspx=sum(0.20 + 0.01 * i for i in qs),
             epsfi=sum(0.19 + 0.01 * i for i in qs))
    return f


@pytest.fixture()
def world(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    w = World(tmp_path)
    dates = qdates(2016, 19)           # 2016Q1 .. 2020Q3 (index 0 = 2016Q1)
    for i, dd in enumerate(dates):
        avail = T(dd.year, dd.month, dd.day, 21) + dt.timedelta(days=35)
        fq = 4 if dd.month == 12 else dd.month // 3
        it = base_items(i - 8)          # index relative to 2018Q1
        it.update(oancfy=ytd(i, 25.0), capxy=ytd(i, 8.0), dvy=ytd(i, 3.0), xrdy=ytd(i, 4.0), sstky=0.0,
                  prstkcy=ytd(i, 1.0))
        if fq == 4:
            avail = T(dd.year + 1, 2, 20, 21)
            w.q(1, dd, 4, f"a1-{i}", avail, form="10-K", **it)
            w.a(1, dd, f"a1-{i}", avail, **annual(i - 8))
        else:
            w.q(1, dd, fq, f"a1-{i}", avail, **it)
    # issuer 2: same quarters but 2019Q3 missing (TTM NULL until it rolls out); issuer 3: stops filing after 2019Q1
    for i, dd in enumerate(dates):
        avail = T(dd.year, dd.month, dd.day, 21) + dt.timedelta(days=36)
        fq = 4 if dd.month == 12 else dd.month // 3
        if dd == D(2019, 9, 30):
            continue
        it = base_items(i - 8)
        w.q(2, dd, fq, f"a2-{i}", avail if fq != 4 else T(dd.year + 1, 2, 21, 21), form="10-Q" if fq != 4 else "10-K",
            **it)
        if dd <= D(2019, 3, 31):
            w.q(3, dd, fq, f"a3-{i}", avail if fq != 4 else T(dd.year + 1, 2, 22, 21),
                form="10-Q" if fq != 4 else "10-K", **it)
    w.link(100, 1, "P")
    w.link(101, 1, "J")
    w.link(200, 2, "P")
    w.link(300, 3, "P")
    w.link(900, 1, "N")                 # a non-common line: not a cell, not in me
    w.link(901, 1, "J")                 # a warrant mislabelled J: excluded by its FINRA type
    for s in SESSIONS:
        w.px += [{"session_date": s, "security_id": 100, "close": 50.0}, {"session_date": s, "security_id": 101,
                                                                           "close": 49.0},
                 {"session_date": s, "security_id": 200, "close": 20.0}, {"session_date": s, "security_id": 300,
                                                                          "close": 10.0},
                 {"session_date": s, "security_id": 900, "close": 25.0},
                 {"session_date": s, "security_id": 901, "close": 3.0}]
        w.sh += [{"session_date": s, "security_id": 100, "shrout": 80.0}, {"session_date": s, "security_id": 101,
                                                                            "shrout": 20.0},
                 {"session_date": s, "security_id": 200, "shrout": 100.0},
                 {"session_date": s, "security_id": 300, "shrout": 100.0},
                 {"session_date": s, "security_id": 901, "shrout": 1000.0}]
    return w


def run(w, sessions=SESSIONS):
    w.write(sessions)
    R.build(buckets=2)
    return duckdb.connect()


def monthly(con, root, sid, day):
    rel = con.execute(f"SELECT * FROM read_parquet('{(root / 'ratios' / 'monthly' / '*' / '*.parquet').as_posix()}') "
                      f"WHERE security_id = {sid} AND session_date = DATE '{day}'")
    cols = [d[0] for d in rel.description]
    row = rel.fetchone()
    return None if row is None else dict(zip(cols, row))


def close(a, b, tol=1e-9):
    return a is not None and b is not None and abs(a - b) <= tol * max(1.0, abs(b))


def test_month_end_sessions_exclude_incomplete_month(world):
    world.write(SESSIONS)
    con = C.connect(memory="200MB", threads=1)
    ms = R.month_end_sessions(con, world.root)
    assert ms[0] == D(2018, 1, 31) and ms[-1] == D(2020, 9, 30)      # 2020-10 (ends 10-05) is incomplete
    assert D(2020, 6, 30) in ms


def test_ttm_quarters_annual_path_ytd_and_averages(world):
    con = run(world)
    # 2020-05-29: latest filing = 2020Q1 10-Q (index 8 of 2018 base: 2020Q1 = 8), filed 2020-05-05 21:00
    r = monthly(con, world.root, 100, D(2020, 5, 29))
    assert r["datadate"] == D(2020, 3, 31) and r["fqtr"] == 1 and r["ttm_src"] == "q4"
    s = sum(sale(i) for i in (5, 6, 7, 8))
    st = con.execute(f"SELECT * FROM read_parquet('{(world.root / 'ratios' / 'fund_state.parquet').as_posix()}') "
                     f"WHERE cik = 1 AND available_at = TIMESTAMP '{r['fund_available_at']}'").fetchdf().iloc[0]
    assert close(st["sale_ttm"], s)
    # YTD TTM: Q1'20 YTD + FY'19 YTD - Q1'19 YTD = 25 + 100 - 25
    assert close(st["oancf_ttm"], 100.0) and close(st["capx_ttm"], 32.0)
    # average assets: (at q0 + at q4) / 2 = (1000 + 80 + 1000 + 40) / 2
    assert close(r["at_turn"], s / ((1080.0 + 1040.0) / 2))
    assert close(r["roa"], sum(35.0 + i for i in (5, 6, 7, 8)) / 1060.0)
    # me: P and J common lines; the preferred line (N) is neither a cell nor in me
    assert close(r["me"], 80 * 50.0 + 20 * 49.0)
    assert monthly(con, world.root, 900, D(2020, 5, 29)) is None
    assert monthly(con, world.root, 901, D(2020, 5, 29)) is None
    assert close(r["ps"], r["me"] / s)
    be = 400.0 + 80 + 7.0 - 10.0            # seq + txditc (FY2019 annual) - pstk
    assert close(r["be_q0"], be) and close(r["bm"], be / r["me"])
    # 2020-03-31: lag-0 row = the FY2019 10-K (filed 2020-02-20) -> the fiscal-year value (quarter sum + 1)
    r2 = monthly(con, world.root, 100, D(2020, 3, 31))
    assert r2["ttm_src"] == "annual" and r2["fqtr"] == 4
    assert close(r2["ps"], r2["me"] / (sum(sale(i) for i in (4, 5, 6, 7)) + 1.0))


def test_missing_quarter_is_null_never_zero(world):
    con = run(world)
    # issuer 2 lacks 2019Q3: TTM at 2019Q3..2020Q2 states needs it -> NULL; 2020Q3 (10-Q filed 2020-11) not visible
    r = monthly(con, world.root, 200, D(2020, 5, 29))
    assert r["datadate"] == D(2020, 3, 31) and r["ttm_src"] is None
    assert r["ps"] is None and r["npm"] is None and r["at_turn"] is None
    assert r["curr_ratio"] is not None          # balance-only ratio unaffected
    # at the FY2019 10-K the annual path does not need the quarters, but issuer 2 has no funda row -> q4 sum -> NULL
    r2 = monthly(con, world.root, 200, D(2020, 3, 31))
    assert r2["ttm_src"] is None and r2["ps"] is None


def test_staleness_nulls_quarterly_ratios(world):
    con = run(world)
    ok = monthly(con, world.root, 300, D(2019, 12, 31))
    assert ok["datadate"] == D(2019, 3, 31) and ok["curr_ratio"] is not None
    stale = monthly(con, world.root, 300, D(2020, 6, 30))     # 457 days after datadate > 400
    assert stale["staleness_days"] > R.STALE_DAYS and stale["fresh_q"] is False
    assert stale["curr_ratio"] is None and stale["bm"] is None


def test_monthly_cutoff_is_month_end_2200_utc(world):
    # a filing accepted 2020-06-30 21:59 is visible at m = 2020-06-30, one at 22:00 is not
    world.q(1, D(2020, 6, 30), 2, "late-a", T(2020, 6, 30, 21, 59), **base_items(9))
    world.q(2, D(2020, 6, 30), 2, "late-b", T(2020, 6, 30, 22, 0), **base_items(9))
    con = run(world)
    assert monthly(con, world.root, 100, D(2020, 6, 30))["datadate"] == D(2020, 6, 30)
    assert monthly(con, world.root, 200, D(2020, 6, 30))["datadate"] == D(2020, 3, 31)
    assert monthly(con, world.root, 200, D(2020, 7, 31))["datadate"] == D(2020, 6, 30)


def test_restatement_vintage_changes_ttm_only_after_its_clock(world):
    # a restated 2019Q3 row (filed 2020-06-10) raises saleq by 50: TTM moves only for cutoffs after 2020-06-10
    it = base_items(6)
    it["saleq"] += 50.0
    world.q(1, D(2019, 9, 30), 3, "restate", T(2020, 6, 10, 21), form="10-Q", **it)
    con = run(world)
    may = monthly(con, world.root, 100, D(2020, 5, 29))
    jun = monthly(con, world.root, 100, D(2020, 6, 30))
    s = sum(sale(i) for i in (5, 6, 7, 8))
    assert close(may["me"] / may["ps"], s)
    assert close(jun["me"] / jun["ps"], s + 50.0)


def test_eps_split_basis(world):
    # 2:1 split ex 2020-03-15 on the issuer's primary line: quarters filed before it are halved
    world.splits.append({"security_id": 100, "ex_date": D(2020, 3, 15), "kind": "split", "split_ratio": 2.0})
    con = run(world)
    # 2020-03-31: state = FY2019 10-K filed 2020-02-20 (pre-split basis) -> eps_adj = 1/2 at the session
    # P/E = me / (EPS x cshoq) with EPS and cshoq on the state's basis (cshoq 100 in every fixture filing)
    r = monthly(con, world.root, 100, D(2020, 3, 31))
    eps = sum(0.19 + 0.01 * i for i in (4, 5, 6, 7))
    assert close(r["eps_adj"], 0.5) and close(r["pe_issuer_ib"], r["me"] / annual(7)["ib"])
    # 2020-05-29: the Q1'20 10-Q (filed after the split) is on the new basis; earlier quarters are halved
    r2 = monthly(con, world.root, 100, D(2020, 5, 29))
    eps2 = sum((0.19 + 0.01 * i) / 2 for i in (5, 6, 7)) + (0.19 + 0.01 * 8)
    assert close(r2["eps_adj"], 1.0) and close(r2["pe_issuer_ib"], r2["me"] / sum(20.0+i for i in (5,6,7,8)))
    # an amendment of an older quarter filed after the split moves the state's basis: cshoq of the pre-split lag-0
    # row is doubled, its EPS halved, the product (and P/E) unchanged
    # the split also reaches the J line of the same issuer (issuer-level split table)
    assert close(monthly(con, world.root, 101, D(2020, 3, 31))["eps_adj"], 0.5)


def test_me_null_when_a_common_line_lacks_shares(world):
    world.sh = [r for r in world.sh if not (r["security_id"] == 101 and r["session_date"] == D(2020, 5, 29))]
    con = run(world)
    r = monthly(con, world.root, 100, D(2020, 5, 29))
    assert r["me"] is None and r["bm"] is None and r["pe_issuer_ib"] is None and r["npm"] is not None


def test_composite_scores_match_formulas(world):
    con = run(world)
    r = monthly(con, world.root, 100, D(2020, 3, 31))      # FY0 = 2019 (q idx 7), FY-1 = 2018 (3), FY-2 = 2017 (-1)
    a0, a1, a2 = annual(7), annual(3), annual(-1)
    roa0, roa1 = a0["ib"] / a1["at"], a1["ib"] / a2["at"]
    cfo0 = a0["oancf"] / a1["at"]
    lev0, lev1 = a0["dltt"] / ((a0["at"] + a1["at"]) / 2), a1["dltt"] / ((a1["at"] + a2["at"]) / 2)
    gm0, gm1 = (a0["sale"] - a0["cogs"]) / a0["sale"], (a1["sale"] - a1["cogs"]) / a1["sale"]
    t0, t1 = a0["sale"] / a1["at"], a1["sale"] / a2["at"]
    f = [roa0 > 0, cfo0 > 0, roa0 - roa1 > 0, cfo0 > roa0, lev0 - lev1 < 0, False, a0["sstk"] <= 0, gm0 - gm1 > 0,
         t0 - t1 > 0]
    assert r["piotroski_f"] == sum(int(x) for x in f)
    z = (1.2 * (a0["act"] - a0["lct"]) / a0["at"] + 1.4 * a0["re"] / a0["at"] + 3.3 * a0["oiadp"] / a0["at"]
         + 0.6 * r["me"] / a0["lt"] + a0["sale"] / a0["at"])
    assert close(r["altman_z"], z)
    chin = (a0["ni"] - a1["ni"]) / (abs(a0["ni"]) + abs(a1["ni"]))
    o = (-1.32 - 0.407 * math.log(a0["at"] / 1e6) + 6.03 * a0["lt"] / a0["at"]
         - 1.43 * (a0["act"] - a0["lct"]) / a0["at"] + 0.0757 * a0["lct"] / a0["act"] - 1.72 * 0
         - 2.37 * a0["ni"] / a0["at"] - 1.83 * a0["oancf"] / a0["lt"] + 0.285 * 0 - 0.521 * chin)
    assert close(r["ohlson_o"], o)
    dsri = (a0["rect"] / a0["sale"]) / (a1["rect"] / a1["sale"])
    gmi = gm1 / gm0
    aqi = (1 - (a0["act"] + a0["ppent"]) / a0["at"]) / (1 - (a1["act"] + a1["ppent"]) / a1["at"])
    sgi = a0["sale"] / a1["sale"]
    depi = (a1["dp"] / (a1["dp"] + a1["ppent"])) / (a0["dp"] / (a0["dp"] + a0["ppent"]))
    sgai = (a0["xsga"] / a0["sale"]) / (a1["xsga"] / a1["sale"])
    lvgi = ((a0["lct"] + a0["dltt"]) / a0["at"]) / ((a1["lct"] + a1["dltt"]) / a1["at"])
    tata = (a0["ib"] - a0["oancf"]) / a0["at"]
    m = (-4.84 + 0.920 * dsri + 0.528 * gmi + 0.404 * aqi + 0.892 * sgi + 0.115 * depi - 0.172 * sgai
         + 4.679 * tata - 0.327 * lvgi)
    assert close(r["beneish_m"], m)
    # annual-frequency ratios use FY0
    assert close(r["ptpm"], a0["pi"] / a0["sale"]) and close(r["efftax"], a0["txt"] / a0["pi"])
    assert close(r["adv_sale"], a0["xad"] / a0["sale"])


def test_daily_macro_pit_and_consistency(world):
    con = run(world)
    con.execute(R.macros_sql(world.root))
    # 2020-05-06 (Wed): the Q1'20 10-Q accepted 2020-05-05 21:00 < 2020-05-05 22:00 -> visible
    d1 = con.execute("SELECT datadate, ps, me FROM ratios_asof(DATE '2020-05-06') WHERE security_id = 100").fetchone()
    assert d1[0] == D(2020, 3, 31)
    # 2020-05-05: cutoff = 2020-05-04 22:00 -> still the FY2019 10-K
    d0 = con.execute("SELECT datadate FROM ratios_asof(DATE '2020-05-05') WHERE security_id = 100").fetchone()
    assert d0[0] == D(2019, 12, 31)
    # price-free ratios on the session after a month end equal the monthly row (same fundamentals cutoff)
    m = monthly(con, world.root, 100, D(2020, 5, 29))
    nxt = con.execute("SELECT * FROM ratios_asof(DATE '2020-06-01') WHERE security_id = 100")
    cols = [c[0] for c in nxt.description]
    row = dict(zip(cols, nxt.fetchone()))
    for name in ("npm", "roa", "curr_ratio", "at_turn", "piotroski_f", "beneish_m"):
        assert close(row[name], m[name]), name
    assert row["available_at"] == T(2020, 6, 1, 22)


def test_coverage_gate_structural_and_undefined(world):
    con = C.connect(memory="200MB", threads=1)
    con.execute("""CREATE TEMP TABLE cells AS SELECT * FROM (VALUES
        (2020, 'bank', NULL, NULL, NULL, 1.0),
        (2020, 'other', 'invt', NULL, NULL, 2.0),
        (2020, 'other', NULL, NULL, 0.5, NULL),
        (2020, 'other', NULL, 'curr_ratio', NULL, NULL)) t(year, fin_template, absent_items, ratio_undefined,
                                                         curr_ratio, inv_turn)""")
    for r in list(R.RATIOS) + list(R.COMPOSITES):
        if r["name"] not in ("curr_ratio", "inv_turn"):
            con.execute(f"ALTER TABLE cells ADD COLUMN {r['name']} DOUBLE")
    res = R.coverage_gate(con, {"bank": ["act", "lct", "invt"], "other": []})
    cr = res["by_ratio"]["curr_ratio"]["by_year"]["2020"]
    assert cr["eligible"] == 2 and cr["share"] == 0.5          # bank masked, undefined (lct = 0) excluded
    it = res["by_ratio"]["inv_turn"]["by_year"]["2020"]
    assert it["eligible"] == 3 and it["share"] == 0.3333       # missing inventory is a gap, not structural


def test_ratio_spec_complete():
    names = R.ratio_names()
    assert len(names) == len(set(names)) == 73
    assert set(names) == set(R.LIT_RANGES)
    for r in R.RATIOS:
        assert r["inputs"] and r["family"] and r["definition"]
    sql = R.macros_sql(C.build_root())
    assert sql.startswith("CREATE OR REPLACE MACRO ratios_asof(d) AS TABLE")


def test_hand_values_and_items_rules():
    dd = D(2023, 12, 31)

    def fact(concept, value, start=None, end="2023-12-31", dim=False):
        return {"concept": concept, "start": start, "end": end, "instant": start is None, "dim": dim, "value": value}
    facts = [fact("Revenues", 1000.0, "2023-01-01"), fact("Revenues", 250.0, "2023-10-01"),      # 3-month ignored
             fact("Revenues", 9e9, "2023-01-01", dim=True),                                    # dimensional ignored
             fact("NetIncomeLoss", 90.0, "2023-01-01"), fact("IncomeLossFromDiscontinuedOperationsNetOfTax", -10.0,
                                                             "2023-01-01"),
             fact("EarningsPerShareDiluted", 0.9, "2023-01-01"), fact("CommonStockSharesOutstanding", 500.0),
             fact("NetCashProvidedByUsedInOperatingActivities", 150.0, "2023-01-01"),
             fact("Assets", 2000.0), fact("Assets", 1800.0, end="2022-12-31"),
             fact("StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest", 800.0),
             fact("StockholdersEquity", 750.0), fact("StockholdersEquity", 700.0, end="2022-12-31"),
             fact("DeferredTaxLiabilitiesNoncurrent", 30.0)]
    v = R._hand_values(facts, dd)
    assert v["sale"] == 1000.0 and v["at_p"] == 1800.0 and v["liab"] is None
    h = R.hand_items(v)
    assert h["ib"] == 100.0                     # ni - do (no continuing-operations concept)
    assert h["epsfx"] is None                   # DO present and no continuing-operations EPS: not total EPS
    assert h["lt"] is None                     # missing liabilities never become a balance plug
    assert h["be"] is None and h["be_p"] is None and h.get("pstk") is None
    m = {"me": 5000.0, "prc": 10.0, "eps_adj": 1.0}
    assert R.HAND_RATIOS["ps"](h, m) == 5.0
    assert R.HAND_RATIOS["roe"](h, m) is None
    assert R.HAND_RATIOS["at_turn"](h, m) == 1000.0 / 1900.0
    assert R.HAND_RATIOS["pe_issuer_ni"](h, m) == 5000.0 / 90.0 and R.HAND_RATIOS["pe_issuer_ib"](h, m) == 5000.0 / 100.0
    assert set(R.HAND_RATIOS) <= set(R.ratio_names())


def test_fy0_fallback_and_history_limited(world):
    # issuer 2 gets an FY2019 annual row: the missing 2019Q3 no longer blanks TTM; the FY0 value is used and listed
    a = annual(7)
    world.a(2, D(2019, 12, 31), "a2-15", T(2020, 2, 21, 21), **a)
    con = run(world)
    r = monthly(con, world.root, 200, D(2020, 5, 29))
    assert r["ttm_src"] is None and "sale" in r["ttm_fy0_items"].split(",")
    assert close(r["ps"], r["me"] / a["sale"])
    r2 = monthly(con, world.root, 200, D(2020, 3, 31))           # the 10-K itself: fiscal-year path, no fallback
    assert r2["ttm_src"] == "annual" and r2["ttm_fy0_items"] is None
    # capei needs five fiscal years: issuer 1's first statement (2016-03-31) is too recent at FY0 2019 -> undefined
    r1 = monthly(con, world.root, 100, D(2020, 3, 31))
    assert r1["capei"] is None and "capei" in r1["ratio_undefined"].split(",")
    assert r1["hist_days"] == (D(2019, 12, 31) - D(2016, 3, 31)).days
    assert r1["piotroski_f"] is not None and "piotroski_f" not in (r1["ratio_undefined"] or "")


def test_annual_comparative_restatement_and_successive_clocks(world):
    a = annual(7)
    a["sale"] = 900.0
    world.a(1, D(2019, 12, 31), "annual-comparative", T(2020, 6, 10, 21), form="10-Q", **a)
    a = dict(a, sale=950.0)
    world.a(1, D(2019, 12, 31), "annual-comparative", T(2020, 7, 10, 21), form="10-Q", **a)
    con = run(world)
    state = (world.root / "ratios" / "fund_state.parquet").as_posix()
    values = con.execute(f"SELECT available_at, sale_a0 FROM read_parquet('{state}') WHERE cik=1 AND available_at IN (TIMESTAMP '2020-06-10 21:00:00', TIMESTAMP '2020-07-10 21:00:00') ORDER BY available_at").fetchall()
    assert [v[1] for v in values] == [900.0, 950.0]
    assert monthly(con, world.root, 100, D(2020, 5, 29))["fund_available_at"] < T(2020, 6, 10, 21)


def test_missing_prior_average_stays_null():
    con = duckdb.connect()
    expr = R._avg_sql()["avg_at"]
    assert con.execute(f"SELECT {expr} FROM (SELECT 100.0 at_q0, NULL::DOUBLE at_q4)").fetchone()[0] is None
    assert con.execute(f"SELECT {expr} FROM (SELECT 100.0 at_q0, 80.0 at_q4)").fetchone()[0] == 90.0
    con.close()


def test_hand_share_count_uses_dated_cover_contract():
    facts=[{"concept":"CommonStockSharesOutstanding","start":None,"end":"2020-12-31","instant":True,"dim":False,"value":100.0},
           {"concept":"EntityCommonStockSharesOutstanding","start":None,"end":"2021-01-31","instant":True,"dim":False,"value":95.0}]
    assert R._hand_values(facts,D(2020,12,31))["csho"] == 95.0


def test_issuer_pe_ignores_cover_shares_eps_and_class_price(world):
    for row in world.fq:
        row["cshoq"] = 999999.0
        row["epsfxq"] = 17.0
    con=run(world)
    p=monthly(con,world.root,100,D(2020,5,29))
    j=monthly(con,world.root,101,D(2020,5,29))
    income=sum(20.0+i for i in (5,6,7,8))
    assert p["prc"] != j["prc"]
    assert close(p["pe_issuer_ni"],p["me"]/income) and p["pe_issuer_ni"] == j["pe_issuer_ni"]


def test_custom_stage_daily_macro_binds_that_stage():
    sql=R.macros_sql(C.build_root(),"ratios_v11")
    assert "/ratios_v11/fund_state.parquet" in sql
    assert "/ratios_v11/split_events.parquet" in sql
    assert "/ratios/fund_state.parquet" not in sql


def test_wrds_pe_not_fabricated_without_share_class_alignment(world):
    con=run(world)
    r=monthly(con,world.root,100,D(2020,5,29))
    assert r["pe_exi"] is None and r["pe_inc"] is None and r["peg_trailing"] is None
    assert 'missing_certified_security_class_diluted_EPS_alignment' in r['ratio_missing_primitives']
    assert next(x for x in R.RATIOS if x['name']=='debt_at')['num']=='lt_q0'
    assert next(x for x in R.RATIOS if x['name']=='roe')['num']=='ni_ttm'
