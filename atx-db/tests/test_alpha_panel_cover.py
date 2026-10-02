"""Stage ``identity_cover`` (S2.1 cover-page identity evidence) on the notes fixture zip."""

from __future__ import annotations

import datetime as dt

import duckdb
import pandas as pd
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import cover_page as CP
from tests.test_alpha_panel_notes import C8, A, B, D, lake  # noqa: F401  (fixture)

UTC = dt.UTC

TITLES = [
    ("Class A Common Stock, $0.01 par value", None, True),
    ("Common Stock", None, True),
    ("Ordinary Shares, nominal value $0.0001", None, True),
    ("American Depositary Shares, each representing two ordinary shares", None, True),
    ("Common units representing limited partner interests", None, True),
    ("Units representing limited partner interests", None, True),
    ("Shares of Beneficial Interest", None, True),
    ("Depositary Shares, each representing 1/1000th of a share of 5.00% Series A Preferred Stock", None, False),
    ("Warrants, each exercisable for one share of Class A common stock", None, False),
    ("Units, each consisting of one share of Class A common stock and one-half of one redeemable warrant", None, False),
    ("Rights", None, False),
    ("2.950% Euro Notes due 2031", None, False),
    ("6.25% Senior Notes due 2028", None, False),
    (None, "CommonClassB", True),
    (None, "SeriesAPreferredStock", False),
    (None, "A2.950NotesDue2031", False),
    (None, "Warrant", False),
    (None, None, True),
]


def test_axis_member_and_symbol_placeholders() -> None:
    assert CP.axis_member("ClassOfStock=CommonClassA;EntityListingsExchange=NYSE;", "ClassOfStock") == "CommonClassA"
    assert CP.axis_member("ClassOfStock=CommonClassA;EntityListingsExchange=NYSE;", "EntityListingsExchange") == "NYSE"
    assert CP.axis_member("", "ClassOfStock") is None and CP.axis_member("LegalEntity=X;", "ClassOfStock") is None
    con = duckdb.connect()
    got = [con.execute(f"SELECT {CP._symbol_norm('?')}".replace("?", f"'{s}'")).fetchone()[0]
           for s in ["N/A", "none", " - ", "brk.b", "ABC PRA", "NA"]]
    assert got == [None, None, None, "BRK.B", "ABCPRA", None]
    ex = [con.execute(f"SELECT {CP._exchange_norm('?')}".replace("?", f"'{s}'")).fetchone()[0]
          for s in ["NONE", " NYSEArca ", "n/a", "NASDAQ"]]
    assert ex == [None, "NYSEArca", None, "NASDAQ"]
    names = ["aapl-20200926.htm", "brk.b-20201231.xml", "x.htm", "0001628280-20-012345.xml", "Tsla-20211231_htm.xml"]
    want = ["AAPL", "BRK.B", None, None, "TSLA"]
    assert [CP.instance_prefix(n) for n in names] == want
    assert [con.execute(f"SELECT {CP._instance_prefix(repr(n))}").fetchone()[0] for n in names] == want
    m = con.execute(f"SELECT {CP._member_sql(chr(39) + 'ClassOfStock=CommonClassA;EntityListingsExchange=NYSE;' + chr(39), 'EntityListingsExchange')}").fetchone()[0]
    assert m == "NYSE"


@pytest.mark.parametrize(("title", "member", "want"), TITLES)
def test_equity_like_python_and_sql_twins(title, member, want) -> None:
    assert CP.is_equity_like(title, member) is want
    lit = lambda v: "NULL" if v is None else "'" + v.replace("'", "''") + "'"  # noqa: E731
    got = duckdb.connect().execute(f"SELECT {CP.equity_like_sql(lit(title), lit(member))}").fetchone()[0]
    assert got is want


def test_cover_page_build_rows_clock_and_coverage(lake) -> None:  # noqa: F811
    lt = CP.link_table_path()
    lt.parent.mkdir(parents=True, exist_ok=True)
    pd.DataFrame({"security_id": [10, 40], "cik": [1, 4], "valid_from": [dt.date(2020, 1, 2), dt.date(2022, 1, 3)],
                  "valid_to": [dt.date(2021, 12, 31), dt.date(2022, 12, 30)], "ever_member": [True, False]}
                 ).to_parquet(lt)
    rec = CP.build()
    dest = (C.stage_dir(CP.STAGE) / "cover_page.parquet").as_posix()
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    rows = {(r["adsh"], r["security_key"], r["coreg"]): r for r in con.execute(
        f"SELECT * REPLACE (coalesce(coreg, '') AS coreg) FROM read_parquet('{dest}')").fetchdf().to_dict("records")}
    a = rows[(A, "ClassOfStock=CommonClassA;", "")]
    assert (a["trading_symbol"], a["security_exchange_name"], a["class_member"], a["is_equity_like"]) == ("ABC", "NYSE", "CommonClassA", True)
    assert a["shares_outstanding"] == 100_000_000 and a["shares_outstanding_date"].date() == dt.date(2021, 2, 19)
    assert a["available_at"].to_pydatetime() == dt.datetime(2021, 2, 26, 21, 5, tzinfo=UTC) and a["clock_basis"] == "accepted_utc"
    assert a["auditor_name"] == "Big Four LLP" and a["inc_state_country"] == "DE" and a["filer_category"] == "Large Accelerated Filer"
    assert a["public_float"] == 1.4e9 and a["public_float_basis"] == "dei_EntityPublicFloat"
    assert a["shares_outstanding_total"] == 105_000_000 and a["n_securities"] == 2 and a["cik"] == 1 and a["entity_cik"] == 1
    p = rows[(A, "ClassOfStock=SeriesAPreferredStock;", "")]
    assert p["symbol_norm"] == "ABCPRA" and not p["is_equity_like"] and pd.isna(p["shares_outstanding"])
    cb = rows[(A, "ClassOfStock=CommonClassB;", "")]
    assert pd.isna(cb["trading_symbol"]) and cb["shares_outstanding"] == 5_000_000 and cb["is_equity_like"]
    assert (A, "", "") not in rows
    b = rows[(B, "", "")]
    assert (b["trading_symbol"], b["security_exchange_name"], b["shares_outstanding"]) == ("XYZ", "NASDAQ", 2_500_000)
    assert b["available_at"].to_pydatetime() == dt.datetime(2021, 8, 5, 12, 0, tzinfo=UTC)
    c = rows[(C8, "", "")]
    assert c["trading_symbol"] == "N/A" and pd.isna(c["symbol_norm"]) and c["clock_basis"] == "filed_plus_46h"
    assert c["available_at"].to_pydatetime() == dt.datetime(2021, 3, 11, 22, 0, tzinfo=UTC)
    s = rows[(C8, "ClassOfStock=PreferredStock;LegalEntity=SubCo;", "SubCo")]
    assert (s["entity_cik"], s["symbol_norm"], s["is_equity_like"], s["cik"]) == (33, "SUBP", False, 3)
    d = rows[(D, "", "")]
    assert pd.isna(d["trading_symbol"]) and d["n_securities"] == 0
    ch = rec["checks"]
    assert ch["dup_keys"] == 0 and ch["available_at_null"] == 0 and ch["accessions"] == 5
    cov = rec["coverage"]
    assert cov["2021"]["periodic_filer_ciks"] == 2 and cov["2021"]["share_own_periodic"] == 1.0
    assert cov["2022"]["share_any_cover"] == 0.0
    assert (cov["2021"]["linked_periodic_ciks"], cov["2021"]["share_any_cover_linked"]) == (1, 1.0)
    assert (cov["2021"]["member_periodic_ciks"], cov["2022"]["linked_periodic_ciks"], cov["2022"]["member_periodic_ciks"]) == (1, 1, 0)
    assert cov["2022"]["share_any_cover_linked"] == 0.0 and cov["2022"]["share_any_cover_member"] is None
    man = (C.stage_dir(CP.STAGE) / "manifest.json").read_text()
    assert '"notes"' in man and "cover_page.parquet" in man
