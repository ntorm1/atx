"""S4.6 vintage access: per-event amendment / restatement flags (fund_items) and the as-of functions
(fund_asof) that reproduce a cutoff rebuild."""

from __future__ import annotations

import datetime as dt

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq

from atx_db.alpha_panel import fund_asof as fa
from atx_db.alpha_panel import fund_items as fi

D = dt.date
T = dt.datetime


def row(clock, accn, concept, start, end, value, form="10-K"):
    return (clock, accn, fi.CID[concept], start, end, value, "USD", form, clock.date(), None, None, "fsds_accepted_utc")


def _issuer():
    c1, c2, c3, c4 = T(2023, 2, 20, 21), T(2023, 5, 2, 21), T(2024, 2, 20, 21), T(2025, 2, 20, 21)
    s2, e2, s3, e3, s4, e4 = D(2022, 1, 1), D(2022, 12, 31), D(2023, 1, 1), D(2023, 12, 31), D(2024, 1, 1), D(2024, 12, 31)
    return [
        row(c1, "k22", "Assets", None, e2, 1000.0), row(c1, "k22", "NetIncomeLoss", s2, e2, 100.0),
        row(c1, "k22", "Revenues", s2, e2, 900.0),
        # 10-K/A: FY2022 net income restated 100 -> 80
        row(c2, "k22a", "Assets", None, e2, 1000.0, "10-K/A"), row(c2, "k22a", "NetIncomeLoss", s2, e2, 80.0, "10-K/A"),
        row(c2, "k22a", "Revenues", s2, e2, 900.0, "10-K/A"),
        # FY2023 10-K: comparatives equal the latest values, revenue off by 0.1% (rounding, not a restatement)
        row(c3, "k23", "Assets", None, e3, 1100.0), row(c3, "k23", "NetIncomeLoss", s3, e3, 120.0),
        row(c3, "k23", "NetIncomeLoss", s2, e2, 80.0), row(c3, "k23", "Revenues", s3, e3, 950.0),
        row(c3, "k23", "Revenues", s2, e2, 900.9),
        # FY2024 10-K: FY2023 revenue recast by 5%
        row(c4, "k24", "Assets", None, e4, 1200.0), row(c4, "k24", "NetIncomeLoss", s4, e4, 130.0),
        row(c4, "k24", "Revenues", s4, e4, 990.0), row(c4, "k24", "Revenues", s3, e3, 902.5),
    ]


def test_amendment_and_restatement_flags():
    ev, _ = fi.company_events(1, _issuer(), T(2020, 1, 1), {})
    by = {e["accession"]: e for e in ev}
    assert by["k22"]["is_amendment"] is False and by["k22"]["is_restated"] is False
    assert by["k22a"]["is_amendment"] is True and by["k22a"]["is_restated"] is True
    assert by["k22a"]["restated_items"] == "ni" and by["k22a"]["ni_ttm"] == 80.0
    assert by["k23"]["is_restated"] is False and by["k23"]["restated_items"] == ""
    assert by["k24"]["is_restated"] is True and by["k24"]["restated_items"] == "sale"


EV_SCHEMA = pa.schema([("cik", pa.int64()), ("accession", pa.string()), ("clock_utc", pa.timestamp("us")),
                       ("available_at", pa.timestamp("us")), ("period_end", pa.date32()), ("at", pa.float64())])
H_SCHEMA = pa.schema([("cik", pa.int64()), ("item", pa.string()), ("period_end", pa.date32()),
                      ("fiscal_period", pa.string()), ("accession", pa.string()), ("value", pa.float64()),
                      ("currency", pa.string()), ("available_at", pa.timestamp("us")), ("clock_basis", pa.string()),
                      ("zero_filled", pa.bool_())])


def _stage(tmp_path):
    ev = [{"cik": 1, "accession": "a", "clock_utc": T(2023, 2, 20, 21), "available_at": T(2023, 2, 20, 21),
           "period_end": D(2022, 12, 31), "at": 10.0},
          {"cik": 1, "accession": "b", "clock_utc": T(2023, 5, 2, 21), "available_at": T(2023, 5, 2, 21),
           "period_end": D(2023, 3, 31), "at": 11.0},
          {"cik": 2, "accession": "c", "clock_utc": T(2023, 3, 1, 12), "available_at": T(2023, 3, 9, 20),
           "period_end": D(2022, 12, 31), "at": 5.0}]
    h = [{"cik": 1, "item": "sale_q", "period_end": D(2022, 12, 31), "fiscal_period": "FY", "accession": "a",
          "value": 100.0, "currency": "USD", "available_at": T(2023, 2, 20, 21), "clock_basis": "x", "zero_filled": False},
         {"cik": 1, "item": "sale_q", "period_end": D(2022, 12, 31), "fiscal_period": "FY", "accession": "b",
          "value": 104.0, "currency": "USD", "available_at": T(2023, 5, 2, 21), "clock_basis": "x", "zero_filled": False}]
    pq.write_table(pa.Table.from_pylist(ev, schema=EV_SCHEMA), tmp_path / "events.parquet")
    pq.write_table(pa.Table.from_pylist(h, schema=H_SCHEMA), tmp_path / "quarterly_history.parquet")
    return tmp_path


def test_events_as_of_uses_available_at(tmp_path):
    st = _stage(tmp_path)
    con = duckdb.connect()
    got = con.execute(fa.events_as_of_sql(st, T(2023, 3, 5))).fetchall()
    assert [(r[0], r[1]) for r in got] == [(1, "a")]                        # cik 2 not visible before 03-09 20:00
    got = con.execute(fa.events_as_of_sql(st, T(2023, 12, 31), latest_only=True)).fetchall()
    assert sorted((r[0], r[1]) for r in got) == [(1, "b"), (2, "c")]


def test_history_as_of_and_first_vs_latest(tmp_path):
    st = _stage(tmp_path)
    con = duckdb.connect()
    got = con.execute(fa.history_as_of_sql(st, T(2023, 4, 1))).fetchall()
    assert [(r[1], r[5]) for r in got] == [("sale_q", 100.0)]
    got = con.execute(fa.history_as_of_sql(st, T(2023, 6, 1))).fetchall()
    assert [(r[1], r[5]) for r in got] == [("sale_q", 104.0)]
    v = con.execute(fa.vintages_sql(st)).fetchall()
    cols = [d[0] for d in con.description]
    r = dict(zip(cols, v[0]))
    assert r["first_value"] == 100.0 and r["latest_value"] == 104.0 and r["n_vintages"] == 2
    assert r["first_accession"] == "a" and r["latest_accession"] == "b" and r["restated"] is True


def test_asof_visibility_rule():
    assert fa.session_cutoff(D(2023, 3, 6)) == T(2023, 3, 3, 22)            # Monday: previous session is Friday
    assert fa.session_cutoff(D(2023, 3, 7)) == T(2023, 3, 6, 22)
