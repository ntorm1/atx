"""N-PORT: pure helpers, one-quarter parse on tiny TSVs, effective-report rule, flows, per-security aggregation."""

from __future__ import annotations

import datetime as dt
from pathlib import Path

import duckdb
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import nport as N


def test_quarter_and_links() -> None:
    assert N.quarter_key("2024Q3") == (2024, 3)
    with pytest.raises(ValueError):
        N.quarter_key("2024q5")
    html = ('<a href="/files/dera/data/form-n-port-data-sets/2019q3_nport.zip">a</a>'
            '<a href="/files/dera/data/form-n-port-data-sets/2020q1_nport.zip">b</a>'
            "<a href='/files/dera/data/form-n-port-data-sets/2019q4_nport.zip'>c</a>")
    links = N.dataset_links(html)
    assert list(links) == ["2019q4", "2020q1"]                     # before FIRST_QUARTER dropped, sorted
    assert links["2020q1"].startswith("https://www.sec.gov/files/dera/")


def test_cusip_python_sql_twin() -> None:
    cases = ["037833100", "03783-3100", "g35947202", "000000000", "NNNNNNNNN", "NA0000000", "12345", None, " 594918104 "]
    con = duckdb.connect()
    for c in cases:
        sql = con.execute(f"SELECT {N.CUSIP_SQL.format(x='v')} FROM (SELECT CAST(? AS VARCHAR) AS v)", [c]).fetchone()[0]
        assert sql == N.clean_cusip(c), c


def test_isin_to_cusip_check_digit() -> None:
    assert N.isin_cusip("US0378331005") == "037833100"
    assert N.isin_cusip("us0378331005") == "037833100"
    assert N.isin_cusip("US0378331006") is None                   # bad check digit
    assert N.isin_cusip("GB0002634946") is None                   # not US / CA
    assert N.isin_cusip("CA0679011084") == "067901108"
    assert N.isin_cusip(None) is None


def test_compound_quarter_and_deadline() -> None:
    assert N.compound([1.0, 2.0, -1.0]) == pytest.approx(1.01 * 1.02 * 0.99 - 1)
    assert N.compound([1.0, None, 2.0]) is None and N.compound([]) is None
    assert N.calendar_quarter_end(dt.date(2024, 11, 30)) == dt.date(2024, 12, 31)
    assert N.calendar_quarter_end(dt.date(2024, 1, 31)) == dt.date(2024, 3, 31)
    assert N.deadline(dt.date(2024, 3, 31)) == dt.date(2024, 5, 30)
    assert N.deadline(dt.date(2023, 12, 31)) == dt.date(2024, 2, 29)
    assert N.deadline(dt.date(2024, 6, 30)) == dt.date(2024, 8, 29)
    assert N.deadline(dt.date(2023, 9, 30)) == dt.date(2023, 11, 29)
    assert N.deadline(dt.date(2024, 5, 31)) == dt.date(2024, 7, 30)
    assert N.deadline(dt.date(2023, 11, 30)) == dt.date(2024, 1, 29)  # Jan 29 2024 is a Monday


def _tsv(path: Path, header: list[str], rows: list[list[str]]) -> None:
    path.write_text("\n".join("\t".join(r) for r in [header, *rows]) + "\n", encoding="utf-8")


def test_parse_quarter_tiny(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "lake"))
    d = tmp_path / "tsv"
    d.mkdir()
    a1, a2 = "0001-24-000001", "0001-24-000002"
    _tsv(d / "SUBMISSION.tsv", ["ACCESSION_NUMBER", "FILING_DATE", "SUB_TYPE", "REPORT_DATE", "REPORT_ENDING_PERIOD",
                                "IS_LAST_FILING"],
         [[a1, "28-MAY-2024", "NPORT-P", "31-MAR-2024", "31-DEC-2024", "N"],
          [a2, "29-MAY-2024", "NPORT-P/A", "31-MAR-2024", "31-DEC-2024", "N"]])
    _tsv(d / "REGISTRANT.tsv", ["ACCESSION_NUMBER", "CIK", "REGISTRANT_NAME", "FILE_NUM", "LEI"],
         [[a1, "0000036405", "VANGUARD INDEX FUNDS", "811-02652", "L1"], [a2, "36405", "VANGUARD INDEX FUNDS", "", ""]])
    _tsv(d / "FUND_REPORTED_INFO.tsv", ["ACCESSION_NUMBER", "SERIES_NAME", "SERIES_ID", "SERIES_LEI", "TOTAL_ASSETS",
                                        "TOTAL_LIABILITIES", "NET_ASSETS", "SALES_FLOW_MON1", "REDEMPTION_FLOW_MON1"],
         [[a1, "Total Stock", "S000002848", "", "1000", "10", "990", "50", "20"],
          [a2, "Total Stock", "S000002848", "", "1000", "10", "991", "", ""]])
    _tsv(d / "MONTHLY_TOTAL_RETURN.tsv", ["ACCESSION_NUMBER", "CLASS_ID", "MONTHLY_TOTAL_RETURN1", "MONTHLY_TOTAL_RETURN2",
                                          "MONTHLY_TOTAL_RETURN3"],
         [[a1, "C1", "1", "2", "3"], [a1, "C2", "1", "2", ""], [a2, "C1", "0", "0", "0"]])
    _tsv(d / "FUND_REPORTED_HOLDING.tsv", ["ACCESSION_NUMBER", "HOLDING_ID", "ISSUER_NAME", "ISSUER_LEI", "ISSUER_TITLE",
                                           "ISSUER_CUSIP", "BALANCE", "UNIT", "CURRENCY_CODE", "CURRENCY_VALUE",
                                           "EXCHANGE_RATE", "PERCENTAGE", "PAYOFF_PROFILE", "ASSET_CAT", "ISSUER_TYPE",
                                           "INVESTMENT_COUNTRY", "IS_RESTRICTED_SECURITY", "FAIR_VALUE_LEVEL"],
         [[a1, "11", "Apple Inc", "", "Apple", "037833100", "100", "NS", "USD", "17000", "", "1.7", "Long", "EC", "CORP",
           "US", "N", "1"],
          [a1, "12", "Shell plc", "", "Shell", "000000000", "10", "NS", "USD", "300", "", "0.03", "Long", "EC", "CORP",
           "GB", "N", "1"],
          [a1, "13", "UST", "", "Bond", "912828ZZ1", "1000", "PA", "USD", "990", "", "0.1", "Long", "DBT", "UST", "US",
           "N", "2"]])
    _tsv(d / "IDENTIFIERS.tsv", ["HOLDING_ID", "IDENTIFIERS_ID", "IDENTIFIER_ISIN", "IDENTIFIER_TICKER"],
         [["11", "1", "US0378331005", "AAPL"], ["12", "2", "gb00bp6mxd84", "SHEL"], ["13", "3", "US912828ZZ12", ""]])
    lookup = tmp_path / "acc.parquet"
    con = duckdb.connect()
    con.execute(f"""COPY (SELECT '{a1}' AS accession, TIMESTAMPTZ '2024-05-28 20:15:00+00' AS available_at,
                                 'acceptance_resolved' AS acceptance_clock) TO '{lookup.as_posix()}' (FORMAT parquet)""")
    res = N.parse_quarter(d, "2024q2", lookup)
    assert (res["filings"], res["equity_holdings"], res["holdings_with_cusip"]) == (2, 2, 1)
    assert res["available_basis"] == {"acceptance": 1, "filing_date_eod_et": 1}
    part = N.parts_dir() / "quarter=2024q2"
    f = {r["accession"]: r for r in pq.read_table(part / "filings.parquet").to_pylist()}
    assert f[a1]["cik"] == 36405 and f[a1]["report_date"] == dt.date(2024, 3, 31) and f[a1]["filing_date"] == dt.date(2024, 5, 28)
    assert f[a1]["sales_m1"] == 50 and f[a1]["redemption_m1"] == 20 and f[a1]["sales_m2"] is None
    assert f[a1]["ret_q"] == pytest.approx(1.01 * 1.02 * 1.03 - 1)     # median over classes with all 3 months
    assert (f[a1]["n_classes"], f[a1]["n_classes_ret"]) == (2, 1)
    assert f[a2]["is_amendment"] is True
    # no acceptance: 00:00 America/New_York the day after filing = 04:00 UTC in May
    assert f[a2]["available_at"].astimezone(dt.UTC) == dt.datetime(2024, 5, 30, 4, 0, tzinfo=dt.UTC)
    h = pq.read_table(part / "holdings.parquet").to_pylist()
    assert [(x["holding_id"], x["cusip"], x["isin"], x["ticker"]) for x in h] == \
        [(11, "037833100", "US0378331005", "AAPL"), (12, None, "GB00BP6MXD84", "SHEL")]
    assert h[0]["series_id"] == "S000002848" and h[0]["payoff_profile"] == "LONG" and h[0]["value_usd"] == 17000


def _fil(con: duckdb.DuckDBPyConnection, rows: list[tuple]) -> None:
    con.execute("""CREATE TABLE fil (fund_key VARCHAR, series_id VARCHAR, cik BIGINT, registrant_name VARCHAR,
                   series_name VARCHAR, accession VARCHAR, report_date DATE, filing_date DATE, available_at TIMESTAMPTZ,
                   net_assets DOUBLE, total_assets DOUBLE, ret_q DOUBLE, n_classes BIGINT, dataset_quarter VARCHAR,
                   sales_m1 DOUBLE, sales_m2 DOUBLE, sales_m3 DOUBLE, reinvestment_m1 DOUBLE, reinvestment_m2 DOUBLE,
                   reinvestment_m3 DOUBLE, redemption_m1 DOUBLE, redemption_m2 DOUBLE, redemption_m3 DOUBLE)""")
    con.executemany("INSERT INTO fil VALUES (?, ?, 1, 'R', 'S', ?, ?, ?, ?, ?, NULL, ?, 1, 'x', ?, 0, 0, 0, 0, 0, ?, 0, 0)",
                    rows)


def test_effective_reports_and_flows() -> None:
    con = duckdb.connect()
    t = lambda d: dt.datetime.combine(d, dt.time(20), tzinfo=dt.UTC)  # noqa: E731
    d1, d2, d3 = dt.date(2023, 12, 31), dt.date(2024, 3, 31), dt.date(2024, 2, 29)
    _fil(con, [
        ("S1", "S1", "a0", d1, dt.date(2024, 2, 20), t(dt.date(2024, 2, 20)), 100.0, 0.0, 0.0, 0.0),
        ("S1", "S1", "a1", d2, dt.date(2024, 5, 20), t(dt.date(2024, 5, 20)), 120.0, 0.10, 15.0, 5.0),
        ("S1", "S1", "a2", d2, dt.date(2024, 5, 29), t(dt.date(2024, 5, 29)), 121.0, 0.10, 15.0, 5.0),   # amendment in time
        ("S1", "S1", "a3", d2, dt.date(2024, 6, 15), t(dt.date(2024, 6, 15)), 999.0, 0.10, 0.0, 0.0),    # after deadline
        ("S2", "S2", "b1", d3, dt.date(2024, 4, 10), t(dt.date(2024, 4, 10)), 50.0, None, 1.0, 0.0),
        ("S2", "S2", "b0", dt.date(2024, 1, 31), dt.date(2024, 3, 1), t(dt.date(2024, 3, 1)), 40.0, None, 0.0, 0.0),
    ])
    N._effective_tables(con)
    eff = {(k, str(r)): a for k, r, a in con.execute("SELECT fund_key, report_date, accession FROM eff").fetchall()}
    assert eff[("S1", "2024-03-31")] == "a2"                                 # latest filed by deadline 2024-05-30
    effq = sorted(con.execute("SELECT fund_key, q, accession FROM effq").fetchall())
    assert effq == [("S1", dt.date(2023, 12, 31), "a0"), ("S1", dt.date(2024, 3, 31), "a2"),
                    ("S2", dt.date(2024, 3, 31), "b1")]                     # one report per fund and calendar quarter
    fl = {r[0]: r[1:] for r in con.execute(f"""SELECT accession, net_flow_reported, flow_pct_reported, flow_proxy, flow_proxy_pct
                                              FROM ({N.FLOWS_SQL})""").fetchall()}
    assert fl["a2"][0] == 10.0 and fl["a2"][1] == pytest.approx(0.10)
    assert fl["a2"][2] == pytest.approx(121.0 - 100.0 * 1.10) and fl["a2"][3] == pytest.approx(0.11)
    assert fl["b1"][1] is None                                              # prior report only 29 days earlier


def test_aggregate_and_changes(tmp_path: Path) -> None:
    con = duckdb.connect()
    av = dt.datetime(2024, 5, 20, tzinfo=dt.UTC)
    con.execute("""CREATE TABLE hq (fund_key VARCHAR, cik BIGINT, security_id BIGINT, balance DOUBLE, value_usd DOUBLE,
                   unit VARCHAR, payoff_profile VARCHAR, flow_pct_reported DOUBLE, available_at TIMESTAMPTZ)""")
    rows = [("F1", 1, 7, 100.0, 1000.0, "NS", "LONG", 0.1, av), ("F1", 1, 7, 50.0, 500.0, "NS", "LONG", 0.1, av),
            ("F2", 1, 7, 30.0, 300.0, "NS", "LONG", None, av + dt.timedelta(days=3)),
            ("F3", 2, 7, 20.0, 200.0, "NS", "SHORT", None, av), ("F3", 2, 7, 5.0, 50.0, "PA", "LONG", None, av),
            ("F4", 3, None, 9.0, 90.0, "NS", "LONG", None, av)]
    con.executemany("INSERT INTO hq VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)", rows)
    q = dt.date(2024, 3, 31)
    r = con.execute(N.aggregate_sql(q)).fetchall()
    cols = [c[0] for c in con.description]
    a = dict(zip(cols, r[0], strict=True))
    assert len(r) == 1 and a["security_id"] == 7 and a["period_q"] == q
    assert (a["fund_shares"], a["fund_shares_short"], a["fund_value_usd"]) == (180.0, 20.0, 1800.0)
    assert (a["n_fund_series"], a["n_fund_registrants"], a["n_series_with_flow"]) == (2, 1, 1)
    assert a["flow_induced_shares"] == pytest.approx(15.0) and a["top5_share"] == 1.0
    assert a["available_at"] == av + dt.timedelta(days=3)
    raw = tmp_path / "raw.parquet"
    con.execute(f"""COPY (SELECT * FROM ({N.aggregate_sql(q)}) UNION ALL
                          SELECT * REPLACE (DATE '2024-06-30' AS period_q, fund_shares * 1.5 AS fund_shares) FROM ({N.aggregate_sql(q)})
                          UNION ALL
                          SELECT * REPLACE (DATE '2024-12-31' AS period_q) FROM ({N.aggregate_sql(q)}))
                    TO '{raw.as_posix()}' (FORMAT parquet)""")
    ch = con.execute(f"SELECT period_q, d_fund_shares, pct_fund_shares FROM ({N.changes_sql(raw.as_posix())})").fetchall()
    assert ch[0][1] is None and ch[1][1] == pytest.approx(90.0) and ch[1][2] == pytest.approx(0.5)
    assert ch[2][1] is None                                                  # 2024q3 missing: no change against 2024q2
