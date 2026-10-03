"""Stage compustat_std on fixtures: fiscal calendar (fyr / fqtr / fyearq), datadate window, YTD sums (PIT vintages,
FX, zero rule), ibq / EPS / cshprq from Company Facts (PIT clocks, FY - 9M), rdq, first_reported, catalog renames,
the as-of macros, the cutoff rebuild and the validation gates."""

from __future__ import annotations

import datetime as dt
import json

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import compustat_std as cs
from atx_db.alpha_panel import fundamentals as fu

T = dt.datetime
D = dt.date
EV_SCHEMA = fu.EVENT_SCHEMA.append(pa.field("nonreliance_402_at", pa.timestamp("us")))
CF_SCHEMA = pa.schema([("cik", pa.int64()), ("taxonomy", pa.string()), ("concept", pa.string()), ("unit", pa.string()),
                       ("period_start", pa.date32()), ("period_end", pa.date32()), ("filed_date", pa.date32()),
                       ("fiscal_year", pa.int32()), ("fiscal_period", pa.string()), ("form", pa.string()),
                       ("accession_number", pa.string()), ("value", pa.float64())])
ANN_SCHEMA = pa.schema([("cik", pa.int64()), ("accession", pa.string()), ("announcement_utc", pa.timestamp("us")),
                        ("available_at", pa.timestamp("us")), ("fiscal_period_end", pa.date32()),
                        ("is_primary", pa.bool_())])
FX_SCHEMA = pa.schema([("obs_date", pa.date32()), ("currency", pa.string()), ("usd_per_ccy", pa.float64()),
                       ("series_id", pa.string()), ("available_at", pa.timestamp("us"))])


class World:
    def __init__(self, root):
        self.root = root
        self.events, self.catalog, self.history, self.facts, self.ann, self.fx = [], [], [], [], [], []

    def ev(self, cik, accn, clock, pe, fp, form="10-Q", cat=None, **kw):
        r = {f.name: None for f in EV_SCHEMA}
        r.update({"cik": cik, "accession": accn, "form": form, "filed": clock.date(), "clock_utc": clock,
                  "available_at": clock, "clock_basis": "fsds_accepted_utc", "period_end": pe, "fiscal_period": fp,
                  "fiscal_year": pe.year, "currency": "USD", "fin_template": "other", "fx_converted": False,
                  "is_amendment": form.endswith("/A"), "is_restated": False, "restated_items": "", "zero_filled": "",
                  "staleness_days": 90})
        r.update(kw)
        self.events.append(r)
        c = {f.name: r.get(f.name) for f in fu.CATALOG_SCHEMA}
        c.update(cat or {})
        self.catalog.append(c)
        return r

    def hist(self, cik, item, pe, value, avail, accn="h", currency="USD"):
        self.history.append({"cik": cik, "item": item, "period_end": pe, "fiscal_period": None, "accession": accn,
                             "value": value, "currency": currency, "available_at": avail,
                             "clock_basis": "fsds_accepted_utc", "zero_filled": False})

    def fact(self, cik, concept, start, end, value, accn, unit="USD", tax="us-gaap"):
        self.facts.append({"cik": cik, "taxonomy": tax, "concept": concept, "unit": unit, "period_start": start,
                           "period_end": end, "filed_date": end, "fiscal_year": end.year, "fiscal_period": None,
                           "form": "10-Q", "accession_number": accn, "value": value})

    def write(self, stage="fund_x"):
        st = self.root / stage
        (st / "_work" / "cf").mkdir(parents=True, exist_ok=True)
        pq.write_table(pa.Table.from_pylist(self.events, schema=EV_SCHEMA), st / "events.parquet")
        pq.write_table(pa.Table.from_pylist(self.catalog, schema=fu.CATALOG_SCHEMA), st / "catalog.parquet")
        pq.write_table(pa.Table.from_pylist(self.history, schema=fu.HISTORY_SCHEMA), st / "quarterly_history.parquet")
        pq.write_table(pa.Table.from_pylist(self.facts, schema=CF_SCHEMA), st / "_work" / "cf" / "batch-0000.parquet")
        (st / "manifest.json").write_text("{}")
        ec = self.root / "earn_x"
        ec.mkdir(exist_ok=True)
        pq.write_table(pa.Table.from_pylist(self.ann, schema=ANN_SCHEMA), ec / "announcements.parquet")
        (ec / "manifest.json").write_text("{}")
        ref = self.root / "reference"
        ref.mkdir(exist_ok=True)
        pq.write_table(pa.Table.from_pylist(self.fx, schema=FX_SCHEMA), ref / "fx_daily.parquet")


def _nan(x) -> bool:
    return x is None or x != x


def _build(monkeypatch, w, build_date=D(2026, 10, 2)):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(w.root))
    monkeypatch.delenv("ATX_FX_DAILY", raising=False)
    w.write()
    res = cs.build("fund_x", "earn_x", "cstd", buckets=2, build_date=build_date)
    out = w.root / "cstd"
    con = duckdb.connect()

    def rows(name):
        cur = con.execute(f"SELECT * FROM read_parquet('{(out / name).as_posix()}')")
        cols = [d[0] for d in cur.description]
        return {r[cols.index("accession")]: dict(zip(cols, r)) for r in cur.fetchall()}

    return res, rows("fundq.parquet"), rows("funda.parquet")


def _quarters(w, cik, fy_end, ends, labels, base=None, **kw):
    """One 10-Q/10-K event per period end (clock = end + 35 days)."""
    out = []
    for k, (pe, fp) in enumerate(zip(ends, labels)):
        form = "10-K" if fp == "FY" else "10-Q"
        out.append(w.ev(cik, f"{cik}-{pe.isoformat()}", T(pe.year, pe.month, pe.day, 21) + dt.timedelta(days=35),
                        pe, fp, form, **kw))
    return out


def test_fiscal_calendar_fyr_fqtr_fyearq(monkeypatch, tmp_path):
    w = World(tmp_path)
    # retailer, FY ends late January (fyr 1): Compustat fiscal year = calendar year of the FYE - 1
    _quarters(w, 1, None, [D(2023, 1, 28), D(2023, 4, 29), D(2023, 7, 29), D(2023, 10, 28), D(2024, 2, 3)],
              ["FY", "Q1", "Q2", "Q3", "FY"])
    # 52/53-week September year ending on 2022-10-01 (effective month September)
    _quarters(w, 2, None, [D(2022, 10, 1), D(2022, 12, 31)], ["FY", "Q1"])
    # new issuer without an FY row yet: fyr from the label (Q2 ending June -> fyr 12)
    _quarters(w, 3, None, [D(2021, 6, 30)], ["Q2"])
    # mislabelled quarter: fyr known from the FY row, fqtr follows the dates
    _quarters(w, 4, None, [D(2020, 12, 31), D(2021, 3, 31)], ["FY", "Q2"])
    _, q, a = _build(monkeypatch, w)
    g = lambda acc: (q[acc]["fyr"], q[acc]["fqtr"], q[acc]["fyearq"])  # noqa: E731
    assert g("1-2023-01-28") == (1, 4, 2022)
    assert g("1-2023-04-29") == (1, 1, 2023)
    assert g("1-2023-10-28") == (1, 3, 2023)
    assert g("1-2024-02-03") == (1, 4, 2023)             # Feb 3 -> effective January
    assert g("2-2022-10-01") == (9, 4, 2022)
    assert g("2-2022-12-31") == (9, 1, 2023)
    assert g("3-2021-06-30") == (12, 2, 2021)
    assert g("4-2021-03-31") == (12, 1, 2021)
    assert set(a) == {"1-2023-01-28", "1-2024-02-03", "2-2022-10-01", "4-2020-12-31"}
    assert a["1-2024-02-03"]["fyear"] == 2023 and "fqtr" not in a["1-2024-02-03"]


def test_datadate_window_excludes_and_counts(monkeypatch, tmp_path):
    w = World(tmp_path)
    w.ev(1, "ok", T(2021, 5, 1), D(2021, 3, 31), "Q1")
    w.ev(1, "old", T(2021, 5, 2), D(1989, 12, 31), "Q1")
    w.ev(1, "far", T(2021, 5, 3), D(2028, 3, 31), "Q1")
    res, q, _ = _build(monkeypatch, w)
    assert set(q) == {"ok"}
    man = json.loads((tmp_path / "cstd" / "manifest.json").read_text())
    assert man["excluded_outside_datadate_window"]["events"] == 2
    assert man["datadate_window"] == ["1990-01-01", "2027-10-02"]


def test_ytd_sums_pit_vintages_zero_rule_and_q4_ttm(monkeypatch, tmp_path):
    w = World(tmp_path)
    ends = [D(2020, 12, 31), D(2021, 3, 31), D(2021, 6, 30), D(2021, 9, 30), D(2021, 12, 31)]
    evs = _quarters(w, 7, None, ends, ["FY", "Q1", "Q2", "Q3", "FY"], dvc_ttm=0.0)
    evs[-1]["cfo_ttm"] = 400.0
    clocks = {e["period_end"]: e["available_at"] for e in evs}
    w.hist(7, "cfo_q", D(2021, 3, 31), 100.0, clocks[D(2021, 3, 31)])
    w.hist(7, "cfo_q", D(2021, 6, 30), 120.0, clocks[D(2021, 6, 30)])
    w.hist(7, "cfo_q", D(2021, 3, 31), 90.0, clocks[D(2021, 9, 30)] + dt.timedelta(days=1))  # later restatement
    w.hist(7, "capx_q", D(2021, 6, 30), 5.0, clocks[D(2021, 6, 30)])                          # Q1 capx missing
    _, q, _ = _build(monkeypatch, w)
    assert q["7-2021-03-31"]["oancfy"] == 100.0
    assert q["7-2021-06-30"]["oancfy"] == 220.0          # the restated Q1 is not yet visible
    assert _nan(q["7-2021-09-30"]["oancfy"])              # Q3 quarter missing
    assert q["7-2021-12-31"]["oancfy"] == 400.0          # fqtr 4 = the fiscal-year TTM
    assert _nan(q["7-2021-06-30"]["capxy"])               # a quarter missing
    assert q["7-2021-06-30"]["dvy"] == 0.0               # TTM 0: no flow in the trailing year


def test_ytd_fx_converted_rates(monkeypatch, tmp_path):
    w = World(tmp_path)
    d0 = D(2020, 12, 1)
    for k in range(300):
        d = d0 + dt.timedelta(days=k)
        if d.weekday() < 5:
            w.fx.append({"obs_date": d, "currency": "CAD", "usd_per_ccy": 0.5 if d <= D(2021, 3, 31) else 0.8,
                         "series_id": "x", "available_at": T(d.year, d.month, d.day) + dt.timedelta(days=7)})
    e1 = w.ev(8, "c1", T(2021, 5, 10), D(2021, 3, 31), "Q1", currency="CAD", fx_converted=True, fx_rate_avg_q=0.5)
    e2 = w.ev(8, "c2", T(2021, 8, 10), D(2021, 6, 30), "Q2", currency="CAD", fx_converted=True, fx_rate_avg_q=0.8)
    w.ev(8, "fy0", T(2021, 2, 10), D(2020, 12, 31), "FY", currency="CAD", fx_converted=True)
    w.hist(8, "cfo_q", D(2021, 3, 31), 100.0, e1["available_at"], currency="CAD")
    w.hist(8, "cfo_q", D(2021, 6, 30), 10.0, e2["available_at"], currency="CAD")
    w.hist(8, "cfo_q", D(2021, 6, 30), 999.0, e2["available_at"], currency="USD")   # other-currency knowledge ignored
    _, q, _ = _build(monkeypatch, w)
    assert q["c1"]["oancfy"] == pytest.approx(50.0)
    assert q["c2"]["oancfy"] == pytest.approx(100.0 * 0.5 + 10.0 * 0.8)


def test_ibq_eps_cshpr_from_facts(monkeypatch, tmp_path):
    w = World(tmp_path)
    fy0, q1, q2, q3, fy1 = D(2020, 12, 31), D(2021, 3, 31), D(2021, 6, 30), D(2021, 9, 30), D(2021, 12, 31)
    w.ev(9, "a-fy0", T(2021, 2, 20), fy0, "FY", "10-K")
    # Q1: no DO in the trailing year -> ibq = niq, epspxq = total EPS
    w.ev(9, "a-q1", T(2021, 5, 1), q1, "Q1", ni_q=10.0, cat={"do_ttm": 0.0})
    w.fact(9, "EarningsPerShareBasic", D(2021, 1, 1), q1, 0.5, "a-q1", "USD/shares")
    w.fact(9, "WeightedAverageNumberOfSharesOutstandingBasic", D(2021, 1, 1), q1, 20.0, "a-q1", "shares")
    # Q2: DO reported -> ibq from IncomeLossFromContinuingOperations, EPS from continuing operations
    w.ev(9, "a-q2", T(2021, 8, 1), q2, "Q2", ni_q=12.0, cat={"do_ttm": 3.0})
    w.fact(9, "IncomeLossFromContinuingOperations", D(2021, 4, 1), q2, 9.0, "a-q2")
    w.fact(9, "IncomeLossFromContinuingOperationsPerBasicShare", D(2021, 4, 1), q2, 0.45, "a-q2", "USD/shares")
    w.fact(9, "EarningsPerShareBasic", D(2021, 4, 1), q2, 0.6, "a-q2", "USD/shares")
    # Q3: DO fact only -> ibq = niq - doq; total EPS not used (doq != 0), no continuing EPS -> NULL
    w.ev(9, "a-q3", T(2021, 11, 1), q3, "Q3", ni_q=11.0, cat={"do_ttm": 4.0})
    w.fact(9, "IncomeLossFromDiscontinuedOperationsNetOfTax", D(2021, 7, 1), q3, 1.0, "a-q3")
    w.fact(9, "EarningsPerShareBasic", D(2021, 7, 1), q3, 0.55, "a-q3", "USD/shares")
    # a later amendment's restated Q3 fact is invisible to the Q3 row (clock after its available_at)
    w.ev(9, "a-q3a", T(2021, 12, 1), q3, "Q3", "10-Q/A", ni_q=11.0, cat={"do_ttm": 4.0})
    w.fact(9, "IncomeLossFromDiscontinuedOperationsNetOfTax", D(2021, 7, 1), q3, 2.0, "a-q3a")
    # Q4 (10-K): FY - 9M of one concept; cshprq = 4 FY - 3 x 9M
    w.ev(9, "a-fy1", T(2022, 2, 20), fy1, "FY", "10-K", ni_q=15.0, cat={"do_ttm": 0.0, "epspx_ttm": 2.2,
                                                                        "epspi_ttm": 2.0, "intano": 7.0},
         intan=3.0, gdwl=4.0)
    w.fact(9, "IncomeLossFromContinuingOperations", D(2021, 1, 1), fy1, 50.0, "a-fy1")
    w.fact(9, "IncomeLossFromContinuingOperations", D(2021, 1, 1), q3, 36.0, "a-fy1")
    w.fact(9, "WeightedAverageNumberOfSharesOutstandingBasic", D(2021, 1, 1), fy1, 21.0, "a-fy1", "shares")
    w.fact(9, "WeightedAverageNumberOfSharesOutstandingBasic", D(2021, 1, 1), q3, 20.0, "a-fy1", "shares")
    # issuer 10: catalog DO TTM NULL; an old DO fact (not ending at datadate) -> the filing shows no DO now: doq = 0
    w.ev(10, "b-q3", T(2020, 11, 1), D(2020, 9, 30), "Q3", ni_q=1.0)
    w.fact(10, "IncomeLossFromDiscontinuedOperationsNetOfTax", D(2020, 7, 1), D(2020, 9, 30), 0.5, "b-q3")
    w.ev(10, "b-q1", T(2021, 5, 1), q1, "Q1", ni_q=8.0)
    w.fact(10, "EarningsPerShareBasic", D(2021, 1, 1), q1, 0.4, "b-q1", "USD/shares")
    # issuer 12: a non-zero year-to-date DO fact ending at datadate without a 3-month fact -> doq unknown, ibq NULL
    w.ev(12, "c-q2", T(2021, 8, 1), q2, "Q2", ni_q=8.0)
    w.fact(12, "IncomeLossFromDiscontinuedOperationsNetOfTax", D(2021, 1, 1), q2, 0.7, "c-q2")
    _, q, a = _build(monkeypatch, w)
    assert (q["b-q1"]["ibq"], q["b-q1"]["ibq_src"], q["b-q1"]["epspxq"]) == (8.0, "niq_no_do", 0.4)
    assert q["b-q3"]["ibq"] == 0.5 and q["c-q2"]["ibq"] is None
    assert (q["a-q1"]["ibq"], q["a-q1"]["ibq_src"]) == (10.0, "niq_no_do")
    assert (q["a-q1"]["epspxq"], q["a-q1"]["epspxq_src"], q["a-q1"]["cshprq"]) == (0.5, "total_3m", 20.0)
    assert (q["a-q2"]["ibq"], q["a-q2"]["ibq_src"]) == (9.0, "ib_3m")
    assert (q["a-q2"]["epspxq"], q["a-q2"]["epspxq_src"]) == (0.45, "cont_3m")
    assert (q["a-q3"]["ibq"], q["a-q3"]["ibq_src"]) == (10.0, "niq_less_doq")
    assert _nan(q["a-q3"]["epspxq"])                                    # DO quarter without continuing EPS
    assert q["a-q3a"]["ibq"] == 9.0                                     # the amendment sees its own restated DO
    assert (q["a-fy1"]["ibq"], q["a-fy1"]["ibq_src"]) == (14.0, "ib_fy_less_9m")
    assert q["a-fy1"]["cshprq"] == pytest.approx(24.0)
    # catalog renames: intano (gdwl + intan) is Compustat INTAN; total EPS is EPSPI, continuing EPS is EPSPX
    assert q["a-fy1"]["intanq"] == 7.0
    assert (a["a-fy1"]["intan"], a["a-fy1"]["intano"]) == (7.0, 3.0)
    assert (a["a-fy1"]["epspx"], a["a-fy1"]["epspi"]) == (2.0, 2.2)


def test_rdq_8k_before_filing_or_first_filing_date(monkeypatch, tmp_path):
    w = World(tmp_path)
    pe = D(2022, 3, 31)
    w.ev(5, "x-q", T(2022, 5, 5, 20), pe, "Q1")
    w.ev(5, "x-qa", T(2022, 6, 20, 20), pe, "Q1", "10-Q/A")
    w.ann.append({"cik": 5, "accession": "8k", "announcement_utc": T(2022, 4, 28, 20, 5),
                  "available_at": T(2022, 4, 28, 20, 5), "fiscal_period_end": pe, "is_primary": True})
    pe2 = D(2022, 6, 30)
    w.ev(5, "y-q", T(2022, 8, 2, 1, 30), pe2, "Q2")          # accepted 21:30 ET on Aug 1
    w.ann.append({"cik": 5, "accession": "8k2", "announcement_utc": T(2022, 8, 3, 12),
                  "available_at": T(2022, 8, 3, 12), "fiscal_period_end": pe2, "is_primary": True})
    _, q, _ = _build(monkeypatch, w)
    assert (q["x-q"]["rdq"], q["x-q"]["rdq_src"]) == (D(2022, 4, 28), "8k_202")
    assert (q["x-qa"]["rdq"], q["x-qa"]["first_reported"], q["x-q"]["first_reported"]) == (D(2022, 4, 28), False, True)
    assert (q["y-q"]["rdq"], q["y-q"]["rdq_src"]) == (D(2022, 8, 1), "filing")   # 8-K after the 10-Q


def test_asof_macro_latest_vintage(monkeypatch, tmp_path):
    w = World(tmp_path)
    pe = D(2022, 3, 31)
    w.ev(6, "v1", T(2022, 5, 1), pe, "Q1", sale_q=10.0)
    w.ev(6, "v2", T(2022, 6, 1), pe, "Q1", "10-Q/A", sale_q=11.0)
    _build(monkeypatch, w)
    con = duckdb.connect()
    for sql in cs.macros_sql(tmp_path / "cstd"):
        con.execute(sql)
    assert con.execute("SELECT accession, saleq FROM fundq_asof(TIMESTAMP '2022-05-15')").fetchall() == [("v1", 10.0)]
    assert con.execute("SELECT accession, saleq FROM fundq_asof(TIMESTAMP '2022-07-01')").fetchall() == [("v2", 11.0)]
    assert con.execute("SELECT count(*) FROM fundq_asof(TIMESTAMP '2022-05-01')").fetchone()[0] == 0


def test_cutoff_rebuild_equals_published_asof_rows(monkeypatch, tmp_path):
    w = World(tmp_path)
    ends = [D(2020, 12, 31), D(2021, 3, 31), D(2021, 6, 30), D(2021, 9, 30)]
    evs = _quarters(w, 11, None, ends, ["FY", "Q1", "Q2", "Q3"], ni_q=5.0, cat={"do_ttm": 0.0})
    for e in evs[1:]:
        w.hist(11, "cfo_q", e["period_end"], 7.0, e["available_at"])
        w.fact(11, "EarningsPerShareBasic", e["period_end"] - dt.timedelta(days=89), e["period_end"], 0.1,
               e["accession"], "USD/shares")
    w.hist(11, "sale_q", evs[1]["period_end"], 50.0, evs[1]["available_at"], accn=evs[1]["accession"])
    w.hist(11, "sale_q", evs[1]["period_end"], 55.0, evs[2]["available_at"], accn=evs[2]["accession"])
    w.hist(11, "cfo_q", evs[1]["period_end"], 9.0, evs[2]["available_at"] + dt.timedelta(minutes=1),
           accn=evs[2]["accession"])
    evs[1]["sale_q"] = 50.0
    w.ann.append({"cik": 11, "accession": "8", "announcement_utc": T(2021, 7, 20, 12),
                  "available_at": T(2021, 7, 20, 12), "fiscal_period_end": D(2021, 6, 30), "is_primary": True})
    _build(monkeypatch, w)
    out = tmp_path / "cstd"
    man = json.loads((out / "manifest.json").read_text())
    inp = cs.Inputs(tmp_path / "fund_x", tmp_path / "earn_x", cs._work("cstd") / "facts.parquet", [],
                    dt.date.fromisoformat(man["datadate_window"][1]))
    con = C.connect(memory="200MB", threads=1)
    cut = T(2021, 8, 15)
    cs.derive_subset(con, inp, "cik = 11", cut)
    reb = con.execute(f"SELECT * FROM q_out {cs.ORDER}").fetchall()
    pub = con.execute(f"SELECT * FROM read_parquet('{(out / 'fundq.parquet').as_posix()}') "
                      f"WHERE available_at < TIMESTAMP '2021-08-15' {cs.ORDER}").fetchall()
    assert len(pub) == 5 and reb == pub
    v = [r for r in pub if r[0] == 11 and r[1] == D(2021, 3, 31)]
    assert len(v) == 3  # base + sale restatement + delayed same-accession CFO snapshot
    assert con.execute("SELECT saleq, oancfy FROM q_out WHERE datadate=DATE '2021-03-31' "
                       "ORDER BY available_at").fetchall() == [(50.0, 7.0), (55.0, 7.0), (55.0, 9.0)]
    con.close()


def test_member_gates_coverage_sum4_balance(monkeypatch, tmp_path):
    w = World(tmp_path)
    ends = [D(2021, 3, 31), D(2021, 6, 30), D(2021, 9, 30), D(2021, 12, 31)]
    for cik, bad in ((21, False), (22, True)):
        evs = _quarters(w, cik, None, ends, ["Q1", "Q2", "Q3", "FY"], ni_q=1.0, sale_q=10.0, cat={"do_ttm": 0.0},
                        at=100.0, lt=60.0, che=5.0, shrs_q=1e6)
        for e in evs:
            w.catalog[w.events.index(e)].update({"teq": 40.0 if not bad else 30.0, "ceq": 40.0,
                                                 "mibt": 10.0 if bad else None})   # temporary equity
        evs[-1].update({"sale_ttm": 40.0 if not bad else 50.0, "ni_ttm": 4.0, "cfo_ttm": 1.0, "capx_ttm": 1.0})
        for e in evs:                                   # quarters as the fundamentals stage knew them
            w.hist(cik, "sale_q", e["period_end"], 10.0, e["available_at"])
        w.catalog[w.events.index(evs[-1])]["ib_ttm"] = 4.0
    w.ann.append({"cik": 21, "accession": "8k", "announcement_utc": T(2021, 4, 20, 12),
                  "available_at": T(2021, 4, 20, 12), "fiscal_period_end": D(2021, 3, 31), "is_primary": True})
    # cik 22: the 8-K comes 90 minutes after the 10-Q (accepted 2021-05-05 21:00 UTC), same ET date
    w.ann.append({"cik": 22, "accession": "8k2", "announcement_utc": T(2021, 5, 5, 22, 30),
                  "available_at": T(2021, 5, 5, 22, 30), "fiscal_period_end": D(2021, 3, 31), "is_primary": True})
    _build(monkeypatch, w)
    (tmp_path / "_tmp" / "panel_member").mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table({"session_date": pa.array([D(2021, 6, 1)] * 2, pa.date32()),
                             "security_id": [1, 2], "member": [True, True]}),
                   tmp_path / "_tmp" / "panel_member" / "year=2021.parquet")
    (tmp_path / "identity").mkdir(exist_ok=True)
    pq.write_table(pa.table({"security_id": [1, 2], "cik": [21, 22],
                             "valid_from": pa.array([D(2020, 1, 1)] * 2, pa.date32()),
                             "valid_to": pa.array([None, None], pa.date32())}),
                   tmp_path / "identity" / "link_table_v3.parquet")
    con = C.connect(memory="200MB", threads=1)
    cs.member_ciks(con, (2021,))
    out = tmp_path / "cstd"
    cov = cs.coverage_gate(con, out)
    assert cov["by_year"]["2021"]["issuer_quarters"] == 8 and cov["by_year"]["2021"]["saleq"] == 1.0
    assert cov["by_year"]["2021"]["ibq"] == 1.0 and cov["by_year"]["2021"]["oancfy"] == 0.25
    rq = cs.rdq_gate(con, out, tmp_path / "earn_x")["by_year"]["2021"]
    assert (rq["periods"], rq["rdq_populated"], rq["rdq_from_8k_pit"]) == (6, 1.0, round(1 / 6, 4))
    assert rq["rdq_8k_same_date"] == round(2 / 6, 4)                 # the same-date 8-K counts (ruling T3-b)
    assert rq["info_issuer_files_no_202_near_period"] == 0.0
    s4 = cs.sum4_gate(con, out)
    assert s4["by_year"]["2021"]["saleq"] == {"fiscal_years": 2, "within_0_5pct": 0.5, "within_0_5pct_or_1m": 1.0,
                                              "info_within_0_5pct_quarters_as_known_at_annual": 0.5}
    assert s4["by_year"]["2021"]["niq"]["within_0_5pct"] == 1.0
    bal = cs.balance_gate(con, out)
    assert bal["by_year"]["2021"] == {"issuer_quarters": 8, "ok": 0.5, "ltq_or_teqq_missing": 0,
                                      "info_ok_with_temporary_equity": 1.0}
    con.close()


def test_parse_ixbrl_and_hand_value():
    html = """<xbrli:context id="c1"><xbrli:entity/><xbrli:period><xbrli:startDate>2024-04-01</xbrli:startDate>
      <xbrli:endDate>2024-06-30</xbrli:endDate></xbrli:period></xbrli:context>
      <xbrli:context id="c2"><xbrli:period><xbrli:startDate>2024-01-01</xbrli:startDate>
      <xbrli:endDate>2024-06-30</xbrli:endDate></xbrli:period></xbrli:context>
      <xbrli:context id="c3"><xbrli:period><xbrli:instant>2024-06-30</xbrli:instant></xbrli:period></xbrli:context>
      <xbrli:context id="c4"><xbrli:entity><xbrli:segment><xbrldi:explicitMember/></xbrli:segment></xbrli:entity>
      <xbrli:period><xbrli:instant>2024-06-30</xbrli:instant></xbrli:period></xbrli:context>
      <ix:nonFraction name="us-gaap:Revenues" contextRef="c1" unitRef="usd" scale="6" decimals="-6">1,234</ix:nonFraction>
      <ix:nonFraction name="us-gaap:NetCashProvidedByUsedInOperatingActivities" contextRef="c2" scale="3"
        sign="-">5</ix:nonFraction>
      <ix:nonFraction name="us-gaap:Assets" contextRef="c3" scale="0"><span>900</span></ix:nonFraction>
      <ix:nonFraction name="us-gaap:Assets" contextRef="c4" scale="0">1</ix:nonFraction>
      <ix:nonFraction name="us-gaap:NetIncomeLoss" contextRef="c1" scale="0"><ix:nonFraction
        name="us-gaap:ProfitLoss" contextRef="c1" scale="0">77</ix:nonFraction></ix:nonFraction>
      <ix:nonFraction name="dei:EntityCommonStockSharesOutstanding" contextRef="c4" scale="0">10</ix:nonFraction>
      <xbrli:context id="c5"><xbrli:entity><xbrli:segment><xbrldi:explicitMember>B</xbrldi:explicitMember>
      </xbrli:segment></xbrli:entity><xbrli:period><xbrli:instant>2024-06-30</xbrli:instant></xbrli:period></xbrli:context>
      <ix:nonFraction name="dei:EntityCommonStockSharesOutstanding" contextRef="c5" scale="0">5</ix:nonFraction>
      <ix:nonFraction name="us-gaap:StockholdersEquityIncludingPortionAttributableToNoncontrollingInterest"
        contextRef="c3" scale="0">400</ix:nonFraction>"""
    facts = cs.parse_ixbrl(html)
    assert len(facts) == 9 and sum(f["dim"] for f in facts) == 3
    dd = D(2024, 6, 30)
    assert cs.hand_value(facts, "q", ("SalesRevenueNet", "Revenues"), dd, None) == (1234e6, "Revenues")
    assert cs.hand_value(facts, "y", ("NetCashProvidedByUsedInOperatingActivities",), dd, D(2024, 1, 1))[0] == -5000.0
    assert cs.hand_value(facts, "i", ("Assets",), dd, None)[0] == 900.0          # the dimensional 1 is ignored
    assert cs.hand_value(facts, "q", ("ProfitLoss",), dd, None)[0] == 77.0        # nested tag
    assert cs.hand_value(facts, "q", ("NetIncomeLoss",), dd, None)[0] == 77.0
    assert cs.hand_value(facts, "c", ("EntityCommonStockSharesOutstanding",), dd, None) == (None, None)
    v, label = cs.cover_class_sum(facts)
    assert v == 15.0 and "2 class contexts" in label                             # multi-class cover sum (info)
    assert cs.hand_derived(facts, "ltq", dd) == (500.0, "derived:Assets-EquityInclNCI")
    assert cs.hand_derived(facts, "seqq", dd) == (400.0, "derived:EquityInclNCI-MinorityInterest")


def test_catalog_names_unique_and_renamed():
    names = {n: col for n, col, _, _ in cs.catalog_names()}
    assert len(names) == 81
    assert names["intan"] == "intano" and names["epspx"] == "epspi_ttm" and names["epspi"] == "epspx_ttm"
    assert names["tempeq"] == "mibt" and names["nicon"] == "nicon_ttm" and names["rouant"] == "rou"
    assert "mibt" not in names and "niadj" not in names        # not Compustat MIBT / NIADJ


def test_fsds_gate_own_filing_cells(monkeypatch, tmp_path):
    w = World(tmp_path)
    w.ev(31, "0001-21-000001", T(2021, 5, 1), D(2021, 3, 31), "Q1", sale_q=10.0, **{"at": 100.0})
    w.ev(31, "0001-21-000002", T(2021, 8, 1), D(2021, 6, 30), "Q2", sale_q=12.0, **{"at": 100.0})
    _build(monkeypatch, w)
    fsds = tmp_path / "fsds"
    for sub in ("num", "sub"):
        (fsds / sub).mkdir(parents=True)
    rows = [("0001-21-000001", "Revenues", D(2021, 3, 31), 1, "USD", 10.0),       # match
            ("0001-21-000001", "Assets", D(2021, 3, 31), 0, "USD", 101.0),        # 1% off: mismatch
            ("0001-21-000002", "Revenues", D(2021, 6, 30), 2, "USD", 22.0),       # 6-month YTD: not a saleq cell
            ("0001-21-000002", "Revenues", D(2021, 6, 30), 1, "USD", 12.0)]
    pq.write_table(pa.table({"adsh": [r[0] for r in rows], "tag": [r[1] for r in rows],
                             "ddate": pa.array([r[2] for r in rows], pa.date32()), "qtrs": [r[3] for r in rows],
                             "uom": [r[4] for r in rows], "value": [r[5] for r in rows],
                             "version": ["us-gaap/2021"] * 4, "segments": pa.array([None] * 4, pa.string()),
                             "coreg": pa.array([None] * 4, pa.string())}), fsds / "num" / "q.parquet")
    pq.write_table(pa.table({"adsh": ["x"]}), fsds / "sub" / "q.parquet")
    monkeypatch.setattr(cs.C, "FSDS_DIR", fsds)
    con = C.connect(memory="200MB", threads=1)
    r = cs.fsds_gate(con, tmp_path / "cstd", n_cells=100)
    con.close()
    assert (r["cells"], r["mapped"], r["matched"]) == (3, 3, 2)
    assert r["by_col"]["saleq"] == {"cells": 2, "mapped": 1.0, "match_of_mapped": 1.0}
    assert r["by_col"]["atq"]["match_of_mapped"] == 0.0


def test_restatement_vintages_from_quarterly_history(monkeypatch, tmp_path):
    w = World(tmp_path)
    fy0, q1, q2, fy1, fy2 = D(2020, 12, 31), D(2021, 3, 31), D(2021, 6, 30), D(2021, 12, 31), D(2022, 12, 31)
    w.ev(40, "fy0", T(2021, 2, 20), fy0, "FY", "10-K")
    e1 = w.ev(40, "q1", T(2021, 5, 1), q1, "Q1", sale_q=100.0, mib=50.0, pstk=0.0, sale_ttm=400.0,
              cat={"ceq": 400.0, "teq": 450.0}, **{"at": 1000.0, "seq": 400.0})
    for item, v in (("sale_q", 100.0), ("at", 1000.0), ("seq", 400.0), ("pstk", 0.0)):
        w.hist(40, item, q1, v, e1["available_at"], accn="q1")
    e2 = w.ev(40, "q2", T(2021, 8, 1), q2, "Q2")
    for item, v in (("sale_q", 90.0), ("at", 1010.0), ("seq", 410.0), ("mib", 60.0)):
        w.hist(40, item, q1, v, e2["available_at"], accn="q2")
    e3 = w.ev(40, "fy1", T(2022, 2, 20), fy1, "FY", "10-K", sale_q=100.0, sale_ttm=400.0, ni_ttm=40.0)
    w.hist(40, "sale_q", fy1, 100.0, e3["available_at"], accn="fy1")
    e4 = w.ev(40, "fy2", T(2023, 5, 20), D(2023, 3, 31), "Q1", "10-Q")
    w.hist(40, "sale_q", fy1, 110.0, e4["available_at"], accn="fy2")    # FY2021 comparative restated (Q4 + 10)
    w.hist(40, "ni_q", fy1, 5.0, e4["available_at"], accn="fy2")        # newly derivable: no base, ttm unchanged
    _, q, a = _build(monkeypatch, w)
    con = duckdb.connect()
    rows = con.execute(f"SELECT accession, datadate, vintage_kind, is_restated, first_reported, saleq, atq, seqq, ceqq, "
                       f"teqq, restated_items, available_at FROM read_parquet('{(tmp_path / 'cstd' / 'fundq.parquet').as_posix()}') "
                       "WHERE datadate = DATE '2021-03-31' ORDER BY available_at").fetchall()
    assert rows[0][:5] == ("q1", q1, "filing", False, True)
    v = rows[1]
    assert v[:5] == ("q2", q1, "restatement", True, False) and v[11] == e2["available_at"]
    assert v[5:10] == (90.0, 1010.0, 410.0, 410.0, 470.0)  # new history MIB moves actual base stock 50 -> 60
    assert set(v[10].split(",")) == {"sale_q", "sale_ttm", "at", "seq", "mib", "c_ceq", "c_teq"}
    for sql in cs.macros_sql(tmp_path / "cstd"):
        con.execute(sql)
    asof = lambda t: con.execute(f"SELECT accession, saleq FROM fundq_asof(TIMESTAMP '{t}') "  # noqa: E731
                                 "WHERE datadate = DATE '2021-03-31'").fetchall()
    assert asof("2021-07-01") == [("q1", 100.0)] and asof("2021-09-01") == [("q2", 90.0)]
    # A later 10-Q can restate an annual comparative; it must retain annual values and the 10-Q clock.
    fa = con.execute(f"SELECT accession, sale, ni, vintage_kind FROM read_parquet('{(tmp_path / 'cstd' / 'funda.parquet').as_posix()}') "
                     "WHERE datadate = DATE '2021-12-31' ORDER BY available_at").fetchall()
    assert fa == [("fy1", 400.0, 40.0, "filing"), ("fy2", 410.0, 40.0, "restatement")]
    assert con.execute(f"SELECT form, available_at FROM read_parquet('{(tmp_path / 'cstd' / 'funda.parquet').as_posix()}') "
                       "WHERE accession = 'fy2'").fetchall() == [("10-Q", e4["available_at"])]


def test_ten_q_labelled_fy_is_no_fiscal_year_source(monkeypatch, tmp_path):
    w = World(tmp_path)
    w.ev(41, "k20", T(2021, 2, 20), D(2020, 12, 31), "FY", "10-K")
    w.ev(41, "q21a", T(2021, 5, 1), D(2021, 3, 31), "FY", "10-Q", sale_ttm=10.0)   # DEI error: 10-Q labelled FY
    w.ev(41, "q21b", T(2021, 8, 1), D(2021, 6, 30), "Q2", "10-Q")
    w.ev(42, "only", T(2021, 5, 1), D(2021, 3, 31), "FY", "10-Q")                  # no annual row, FY label
    _, q, a = _build(monkeypatch, w)
    assert (q["q21a"]["fyr"], q["q21a"]["fqtr"]) == (12, 1)
    assert (q["q21b"]["fyr"], q["q21b"]["fqtr"]) == (12, 2)                       # no fyr reset to March
    assert q["only"]["fqtr"] is None and q["only"]["fyr"] is None
    assert set(a) == {"k20"}


def test_rdq_gate_uses_acceptance_et_date_of_filing(tmp_path):
    w = World(tmp_path)
    # rdq is an earlier release date; same-date acceptance must compare against filing date, not rdq.
    w.ann = [{"cik": 51, "accession": "a", "announcement_utc": T(2021, 4, 20),
              "available_at": T(2021, 5, 6, 1), "fiscal_period_end": D(2021, 3, 31), "is_primary": True},
             {"cik": 52, "accession": "b", "announcement_utc": T(2021, 4, 20),
              "available_at": T(2021, 5, 6, 5), "fiscal_period_end": D(2021, 3, 31), "is_primary": True}]
    w.write()
    con = duckdb.connect()
    con.execute("SET TimeZone = 'UTC'")
    con.execute("CREATE TABLE cov AS SELECT 2021 AS year, cik, 'q' AS accession, DATE '2021-03-31' AS datadate, "
                "DATE '2021-04-19' AS rdq, 'filing' AS rdq_src, TIMESTAMP '2021-05-05 20:00' AS clock_utc, "
                "TIMESTAMP '2021-05-07' AS available_at, DATE '2021-05-05' AS filed, '10-Q' AS form "
                "FROM (VALUES (51), (52)) v(cik)")
    r = cs.rdq_gate(con, tmp_path, tmp_path / 'earn_x')["by_year"]["2021"]
    assert r["rdq_8k_same_date"] == 0.5  # 01:00 UTC is May 5 ET; 05:00 UTC is May 6 ET
    assert r["rdq_from_8k_pit"] == 0.0
    con.close()


def test_rdq_gate_modeled_filing_clock_uses_filed_date(monkeypatch, tmp_path):
    w = World(tmp_path)
    w.ev(53, 'q', T(2021, 5, 7, 22), D(2021, 3, 31), 'Q1', clock_basis='fc1', filed=D(2021, 5, 5))
    w.ann.append({'cik': 53, 'accession': 'a', 'announcement_utc': T(2021, 5, 6, 16),
                  'available_at': T(2021, 5, 6, 16), 'fiscal_period_end': D(2021, 3, 31), 'is_primary': True})
    _build(monkeypatch, w)
    con = C.connect(memory='100MB', threads=1)
    con.execute('CREATE TABLE mem AS SELECT 2021 AS year, 53 AS cik')
    cs.coverage_gate(con, tmp_path / 'cstd')
    r = cs.rdq_gate(con, tmp_path / 'cstd', tmp_path / 'earn_x')['by_year']['2021']
    assert r['rdq_8k_same_date'] == 0.0  # conservative modeled clock is not an acceptance date
    con.close()


@pytest.mark.parametrize('annual', [False, True])
@pytest.mark.parametrize('same_accession', [False, True])
def test_delayed_component_clock_snapshots(monkeypatch, tmp_path, annual, same_accession):
    w = World(tmp_path)
    pe = D(2020, 12, 31) if annual else D(2021, 3, 31)
    t0, t1, t2 = T(2021, 5, 1), T(2021, 8, 1), T(2021, 8, 2)
    w.ev(61, 'q1', t0, pe, 'FY' if annual else 'Q1', '10-K' if annual else '10-Q',
         sale_q=100.0, sale_ttm=400.0, **{'at': 1000.0})
    w.ev(61, 'q2', t1, D(2021, 6, 30), 'Q2')
    for item, value in [('sale_q', 100.0), ('at', 1000.0)]:
        w.hist(61, item, pe, value, t0, accn='q1')
    acc = 'q1' if same_accession else 'q2'
    w.hist(61, 'sale_q', pe, 90.0, t1, accn=acc)
    w.hist(61, 'at', pe, 1010.0, t2, accn=acc)
    _build(monkeypatch, w)
    out = tmp_path / 'cstd'
    con = C.connect(memory='200MB', threads=1)
    for sql in cs.macros_sql(out):
        con.execute(sql)
    rows = con.execute(f"SELECT accession,available_at,saleq,atq FROM read_parquet('{(out/'fundq.parquet').as_posix()}') "
                       "WHERE datadate=? ORDER BY available_at", [pe]).fetchall()
    assert rows == [('q1', t0, 100.0, 1000.0), (acc, t1, 90.0, 1000.0), (acc, t2, 90.0, 1010.0)]
    for cut, expected in [(t1, (100.0, 1000.0)), (t1+dt.timedelta(seconds=1), (90.0, 1000.0)),
                          (t2, (90.0, 1000.0)), (t2+dt.timedelta(seconds=1), (90.0, 1010.0))]:
        assert con.execute('SELECT saleq,atq FROM fundq_asof(?) WHERE datadate=?', [cut,pe]).fetchall() == [expected]
        cs.derive_subset(con, cs.Inputs(tmp_path/'fund_x',tmp_path/'earn_x',None,[],D(2027,10,2)), 'cik=61', cut)
        for table, tmp in [('fundq','q_out'), ('funda','a_out')]:
            pub=con.execute(f"SELECT * FROM read_parquet('{(out/(table+'.parquet')).as_posix()}') "
                            f"WHERE available_at<? {cs.ORDER}", [cut]).fetchall()
            reb=con.execute(f'SELECT * FROM {tmp} {cs.ORDER}').fetchall()
            assert reb == pub
            assert con.execute(f'SELECT count(*) FROM (SELECT cik,datadate,accession,available_at FROM {tmp} '
                               'GROUP BY ALL HAVING count(*)>1)').fetchone()[0] == 0
        if annual:
            assert con.execute('SELECT sale,"at" FROM funda_asof(?) WHERE datadate=?', [cut,pe]).fetchall() == [
                (400.0 if cut<=t1 else 390.0, expected[1])]
    con.close()
