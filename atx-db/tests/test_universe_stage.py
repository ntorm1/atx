"""tier1-v4 Task 2 stage ``universe``: rule universe-all-common-v1, PIT as-of inputs, exchange repair, codes, gates."""

from __future__ import annotations

import datetime as dt
import json

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import universe as U

D = dt.date
T = dt.datetime
CAL = [D(2021, 6, 1), D(2021, 6, 2), D(2021, 6, 3), D(2021, 6, 4), D(2021, 6, 7), D(2021, 6, 8)]
UTC = dt.timezone.utc
FAR = D(2099, 12, 31)


def _w(root, rel: str, rows: list[dict], schema: pa.Schema | None = None) -> None:
    path = root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.Table.from_pylist(rows, schema=schema), path)


# line: (security_id, ticker, panel cik, panel exchange, raw_close, security_type, extra panel flags, shrout)
LINES = [
    (1, "AAA", 101, "XNYS", 20.0, "common", {}, 1e6),                  # US common NYSE -> all_common
    (2, "BBB", 102, None, 5.0, "common", {}, 2e6),                     # Nasdaq CM: exchange repaired from FINRA 'SC'
    (3, "AAA.PRA", 101, "XNYS", 25.0, "common_unverified", {}, 1e6),   # preferred ticker
    (4, "DDD", 104, "XNAS", 0.5, "common", {}, 1e6),                   # below $1
    (5, "EEE", 105, "XNAS", 9.0, "common", {}, None),                  # shares unknown
    (6, "FFF", 106, "XNYS", 30.0, "common", {}, 1e6),                  # regime fpi_20f
    (7, "GGG", 107, "XNYS", 30.0, "common", {}, 1e6),                  # Cayman cover visible from 06-04
    (8, "HHH", 108, "XNYS", 10.0, "common_unverified", {}, 1e6),       # SIC 6770 until a 7372 filing (06-04)
    (9, "III", 109, "XNYS", 15.0, "common", {}, 1e6),                  # BDC (814- 10-K)
    (10, "JJJ", 110, "XNAS", 12.0, "common", {}, 1e6),                 # BDC until N-54C (visible 06-03)
    (11, "KKK", None, "XNAS", 40.0, "common", {}, 1e6),                # link_table_v3 visible from 06-04
    (12, "LLL", 112, "XNYS", 50.0, "common", {}, 1e6),                 # REIT by SIC 6798
    (13, "ETF1", None, "ARCX", 100.0, "ETF", {"is_etf": True}, 1e6),   # ETF
    (14, "ADRX", 114, "XNYS", 8.0, "common_unverified", {"is_adr_likely": True}, 1e6),
]


def make_lake(root) -> None:
    _w(root, "calendar.parquet", [{"session_date": d} for d in CAL])
    panel, shares = [], []
    for d in CAL:
        for sid, tic, cik, ex, px, st, extra, sh in LINES:
            row = {c: None for c in U.PANEL_COLUMNS}
            row.update({"session_date": d, "security_id": sid, "cik": cik, "ticker": tic, "exchange": ex,
                        "raw_close": px, "security_type": st, "is_operating": st != "ETF", "is_index": False,
                        "is_adr": False, "is_adr_likely": False, "is_fpi": False, "is_reit": False, "is_lp": False,
                        "is_royalty_trust": False, "is_etf": False, "is_spac": False})
            row.update(extra)
            panel.append(row)
            if sh is not None:
                shares.append({"session_date": d, "security_id": sid, "shrout": sh})
    pschema = pa.schema([("session_date", pa.date32()), ("security_id", pa.int64()), ("cik", pa.int64()),
                         ("ticker", pa.string()), ("exchange", pa.string()), ("raw_close", pa.float64()),
                         ("security_type", pa.string())]
                        + [(c, pa.bool_()) for c in U.PANEL_COLUMNS[7:] if c != "sale_ttm"] + [("sale_ttm", pa.float64())])
    _w(root, "panel/year=2021/panel-01.parquet", panel, pschema)
    _w(root, "market/shares_daily/year=2021/shares_daily.parquet", shares)
    _w(root, "security_master/finra_names.parquet", [
        {"security_id": 2, "dissemination_date": D(2021, 5, 25), "market_class": "SC", "issue_name": "BBB Inc"},
        {"security_id": 1, "dissemination_date": D(2021, 5, 25), "market_class": "NYSE", "issue_name": "AAA Inc"}])
    ts = pa.timestamp("us")
    _w(root, "security_master/share_exchange_history.parquet", [
        {"security_id": 1, "valid_from": D(2021, 6, 1), "valid_to": D(2021, 6, 3), "shrcd": 11, "exchcd": 1,
         "available_at": T(2021, 5, 31, 21, 59, 59)},
        {"security_id": 1, "valid_from": D(2021, 6, 4), "valid_to": D(2021, 6, 8), "shrcd": 18, "exchcd": 1,
         "available_at": T(2021, 6, 3, 21, 59, 59)},                     # visible from its first session
        {"security_id": 2, "valid_from": D(2021, 6, 1), "valid_to": D(2021, 6, 4), "shrcd": 11, "exchcd": None,
         "available_at": T(2021, 5, 31, 21, 59, 59)},
        {"security_id": 2, "valid_from": D(2021, 6, 7), "valid_to": D(2021, 6, 8), "shrcd": 11, "exchcd": None,
         "available_at": T(2021, 6, 7, 22, 0, 0)},                       # stamped late: never visible here
        *[{"security_id": sid, "valid_from": CAL[0], "valid_to": CAL[-1], "shrcd": 11,
           "exchcd": {"XNYS": 1, "XNAS": 3, "ARCX": 4}[ex], "available_at": T(2021, 5, 31, 21, 59, 59)}
          for sid, _, _, ex, *_ in LINES if sid > 2],
    ], pa.schema([("security_id", pa.int64()), ("valid_from", pa.date32()), ("valid_to", pa.date32()),
                  ("shrcd", pa.int32()), ("exchcd", pa.int32()), ("available_at", ts)]))
    _w(root, "identity/link_table_v3.parquet", [
        {"security_id": 11, "cik": 111, "valid_from": D(2021, 1, 4), "valid_to": None, "available_at": T(2021, 6, 2, 23)},
        {"security_id": 1, "cik": 101, "valid_from": D(2020, 1, 2), "valid_to": None, "available_at": T(2020, 1, 1)},
        {"security_id": 1, "cik": 999, "valid_from": D(2021, 1, 4), "valid_to": None, "available_at": T(2021, 1, 1)},
    ], pa.schema([("security_id", pa.int64()), ("cik", pa.int64()), ("valid_from", pa.date32()),
                  ("valid_to", pa.date32()), ("available_at", ts)]))
    reg = [{"cik": c, "regime": "domestic", "valid_from": D(2010, 1, 4), "valid_to": FAR, "available_at": T(2010, 1, 4)}
           for c in (101, 102, 104, 105, 107, 108, 109, 110, 111, 112, 114)]
    reg.append({"cik": 106, "regime": "fpi_20f", "valid_from": D(2010, 1, 4), "valid_to": FAR,
                "available_at": T(2010, 1, 4)})
    _w(root, "sec_filings/filer_regime.parquet", reg)
    _w(root, "classification/issuer_industry.parquet", [
        {"cik": 108, "available_at": T(2021, 1, 10), "sic": 6770},
        {"cik": 108, "available_at": T(2021, 6, 3, 21), "sic": 7372},
        {"cik": 112, "available_at": T(2020, 3, 1), "sic": 6798},
        {"cik": 101, "available_at": T(2020, 3, 1), "sic": 3571}])
    tz = pa.timestamp("us", tz="UTC")
    _w(root, "identity_cover/cover_page.parquet", [
        {"cik": 101, "adsh": "a1", "available_at": T(2021, 5, 1, tzinfo=UTC), "inc_state_country": "DE"},
        {"cik": 107, "adsh": "g1", "available_at": T(2021, 6, 3, 15, tzinfo=UTC), "inc_state_country": "E9"},
    ], pa.schema([("cik", pa.int64()), ("adsh", pa.string()), ("available_at", tz), ("inc_state_country", pa.string())]))
    _w(root, "sec_filings/issuer_profile.parquet", [
        {"cik": 107, "state_of_incorporation": "DE"}, {"cik": 102, "state_of_incorporation": "E9"},
        {"cik": 111, "state_of_incorporation": "X1"}])
    _w(root, "sec_filings/filings.parquet", [
        {"cik": 109, "form": "10-K", "file_number": "814-00001", "available_at": T(2020, 3, 1)},
        {"cik": 110, "form": "10-Q", "file_number": "814-00002", "available_at": T(2019, 5, 1)},
        {"cik": 110, "form": "N-54C", "file_number": "814-00002", "available_at": T(2021, 6, 2, 12)},
        {"cik": 101, "form": "4", "file_number": "814-00009", "available_at": T(2020, 1, 1)},   # not a BDC form
        {"cik": 101, "form": "10-K", "file_number": "001-00001", "available_at": T(2020, 1, 1)}])
    _w(root, "indexes/membership/year=2021/membership.parquet", [
        *[{"session_date": d, "security_id": 1, "cik": 101, "r1000": True, "r2000": False, "r3000": True,
           "sp500": True, "available_at": T(2021, 5, 7, 22)} for d in CAL],
        *[{"session_date": d, "security_id": 2, "cik": 102, "r1000": False, "r2000": True, "r3000": True,
           "sp500": False, "available_at": T(2021, 6, 5, 22)} for d in CAL]])


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    root = tmp_path
    make_lake(root)
    con = C.connect(memory="200MB", threads=1)
    U.load_issuer_tables(con)
    U.build_year(con, 2021)
    return con, root


def _flag(con, col: str, sid: int, d: dt.date):
    return con.execute(f"SELECT {col} FROM t WHERE security_id = {sid} AND session_date = DATE '{d}'").fetchone()[0]


def test_all_common_rule_on_a_quiet_day(lake):
    con, _ = lake
    got = {r[0] for r in con.execute("SELECT security_id FROM t WHERE univ_all_common AND session_date = DATE "
                                     "'2021-06-07'").fetchall()}
    # 1 common NYSE; 2 Nasdaq CM (repaired exchange); 7 is foreign by then; 8 de-SPAC'd SIC visible; 10 BDC withdrawn;
    # 11 linked by link_table_v3 by then
    assert got == {1, 2, 8, 10, 11}


def test_price_screen_and_shares(lake):
    con, _ = lake
    d = D(2021, 6, 7)
    assert not _flag(con, "univ_all_common", 4, d) and _flag(con, "all_common_nop", 4, d)
    assert not _flag(con, "univ_all_common", 5, d) and not _flag(con, "all_common_nop", 5, d)


def test_preferred_ticker_and_regime_and_reit_and_etf_excluded(lake):
    con, _ = lake
    d = D(2021, 6, 7)
    assert _flag(con, "mi_repaired", 3, d) and not _flag(con, "univ_all_common", 3, d)     # AAA.PRA
    assert _flag(con, "is_foreign_issuer", 6, d) and not _flag(con, "univ_all_common", 6, d)
    assert _flag(con, "is_reit", 12, d) and not _flag(con, "univ_all_common", 12, d)
    assert _flag(con, "is_cef_etf", 13, d) and not _flag(con, "univ_all_common", 13, d)
    assert _flag(con, "is_adr", 14, d) and not _flag(con, "univ_all_common", 14, d)


def test_exchange_repair_for_nasdaq_capital_market(lake):
    con, _ = lake
    d = D(2021, 6, 1)
    assert _flag(con, "exchange_u", 2, d) == "XNAS" and _flag(con, "exchange_repaired", 2, d)
    assert not _flag(con, "mi_panel_exchange", 2, d) and _flag(con, "mi_repaired", 2, d)
    out = {r[1]: r for r in con.execute(f"SELECT * FROM ({U.output_sql()}) WHERE session_date = DATE '{d}'").fetchall()}
    cols = [c[0] for c in con.execute(f"SELECT * FROM ({U.output_sql()}) LIMIT 0").description]
    row2 = dict(zip(cols, out[2]))
    row1 = dict(zip(cols, out[1]))
    assert row2["exchcd_like"] == 3 and row2["exchcd_basis"] == "finra_class_repair"
    assert row1["exchcd_like"] == 1 and row1["exchcd_basis"] == "identity_v3"
    assert row1["available_at"] == T(2021, 6, 1, 22) and row1["me"] == pytest.approx(20e6)
    assert row2["me"] == pytest.approx(10e6)


def test_exchange_repair_respects_finra_staleness(lake):
    con, _ = lake
    # a FINRA row older than FINRA_STALE_DAYS repairs nothing
    con.execute("UPDATE finra SET dissemination_date = DATE '2021-04-01' WHERE security_id = 2")
    U.build_year(con, 2021)
    assert _flag(con, "exchange_u", 2, D(2021, 6, 1)) is None and not _flag(con, "univ_all_common", 2, D(2021, 6, 1))


def test_codes_follow_the_as_of_path(lake):
    con, _ = lake
    # the new run is visible from its first session (stamped before lake_cutoff(valid_from))
    assert _flag(con, "shrcd_like", 1, D(2021, 6, 3)) == 11 and _flag(con, "shrcd_like", 1, D(2021, 6, 4)) == 18
    # a run stamped at its own session's 22:00 is not visible (as-of < lake_cutoff(d)) -> NULL, never an older run
    assert _flag(con, "shrcd_like", 2, D(2021, 6, 7)) is None and _flag(con, "shrcd_like", 2, D(2021, 6, 8)) is None
    assert _flag(con, "shrcd_like", 2, D(2021, 6, 4)) == 11


def test_cover_page_incorporation_is_point_in_time(lake):
    con, _ = lake
    # cover clock 2021-06-03 15:00 UTC: before lake_cutoff(06-04) = 06-03 22:00, after lake_cutoff(06-03)
    assert not _flag(con, "is_foreign_issuer", 7, D(2021, 6, 3)) and _flag(con, "univ_all_common", 7, D(2021, 6, 3))
    assert _flag(con, "is_foreign_issuer", 7, D(2021, 6, 4)) and not _flag(con, "univ_all_common", 7, D(2021, 6, 4))
    assert _flag(con, "inc", 7, D(2021, 6, 3)) is None          # undated snapshot is never consumed


def test_spac_sic_history_is_point_in_time(lake):
    con, _ = lake
    assert _flag(con, "is_spac", 8, D(2021, 6, 3)) and not _flag(con, "univ_all_common", 8, D(2021, 6, 3))
    assert not _flag(con, "is_spac", 8, D(2021, 6, 4)) and _flag(con, "univ_all_common", 8, D(2021, 6, 4))


def test_bdc_from_814_filings_with_withdrawal(lake):
    con, _ = lake
    assert _flag(con, "is_cef_etf", 9, D(2021, 6, 8))
    assert _flag(con, "is_bdc", 10, D(2021, 6, 2)) and not _flag(con, "is_bdc", 10, D(2021, 6, 3))
    assert not _flag(con, "is_bdc", 1, D(2021, 6, 2))            # a Form 4 under an 814- number is not BDC evidence


def test_later_foreign_snapshot_never_backfills_missing_cover(lake):
    con, _ = lake
    assert _flag(con, "inc", 2, CAL[0]) is None
    assert not _flag(con, "is_foreign_issuer", 2, CAL[0])


def test_original_header_transition_and_future_append_preserve_history(lake):
    con, root = lake
    rows = [{"cik": 102, "accession": "prior", "inc_state_country": "DE", "available_at": T(2021,5,28,4,tzinfo=UTC)},
            {"cik": 102, "accession": "redomicile", "inc_state_country": "E9", "available_at": T(2021,6,3,4,tzinfo=UTC)}]
    rows=[{**r,'accepted_at':r['available_at']-dt.timedelta(hours=6)} for r in rows]
    rel = "issuer_history/incorporation.parquet"
    _w(root, rel, rows)
    U.load_issuer_tables(con); U.build_year(con,2021)
    assert _flag(con,"inc",2,CAL[0]) == "DE"
    assert _flag(con,"univ_all_common",2,CAL[0])
    assert _flag(con,"inc",2,D(2021,6,3)) == "DE"  # prior-session22 cutoff
    assert _flag(con,"inc",2,D(2021,6,4)) == "E9"
    assert not _flag(con,"univ_all_common",2,D(2021,6,4))
    before=con.execute("SELECT session_date,security_id,inc,univ_all_common FROM t ORDER BY 1,2").fetchall()
    _w(root,rel,rows+[{"cik":102,"accession":"future","inc_state_country":"NV","available_at":T(2022,1,1,tzinfo=UTC),'accepted_at':T(2021,12,31,20,tzinfo=UTC)}])
    U.load_issuer_tables(con);U.build_year(con,2021)
    assert before==con.execute("SELECT session_date,security_id,inc,univ_all_common FROM t ORDER BY 1,2").fetchall()


def test_dated_reit_election_termination_and_future_append(lake):
    con,root=lake
    rel='issuer_classes/reit_status.parquet'
    rows=[{'cik':102,'accession':'election','available_at':T(2021,6,2,4,tzinfo=UTC),'reit_status':True},
          {'cik':102,'accession':'termination','available_at':T(2021,6,7,4,tzinfo=UTC),'reit_status':False},
          {'cik':112,'accession':'stale_sic_termination','available_at':T(2021,6,3,4,tzinfo=UTC),'reit_status':False}]
    rows=[{**r,'accepted_at':r['available_at']-dt.timedelta(hours=6)} for r in rows]
    _w(root,rel,rows);U.load_issuer_tables(con);U.build_year(con,2021)
    assert not _flag(con,'is_reit',2,CAL[0])
    assert _flag(con,'is_reit',2,D(2021,6,3))
    assert not _flag(con,'univ_all_common',2,D(2021,6,3))
    assert not _flag(con,'is_reit',2,D(2021,6,8))
    assert _flag(con,'univ_all_common',2,D(2021,6,8))
    assert _flag(con,'is_reit',12,CAL[0])
    assert not _flag(con,'is_reit',12,D(2021,6,4))
    assert _flag(con,'univ_all_common',12,D(2021,6,4))
    before=con.execute('SELECT session_date,security_id,is_reit,univ_all_common FROM t ORDER BY 1,2').fetchall()
    _w(root,rel,rows+[{'cik':102,'accession':'future','available_at':T(2022,1,1,tzinfo=UTC),'accepted_at':T(2021,12,31,20,tzinfo=UTC),'reit_status':True}])
    U.load_issuer_tables(con);U.build_year(con,2021)
    assert before==con.execute('SELECT session_date,security_id,is_reit,univ_all_common FROM t ORDER BY 1,2').fetchall()


def test_source_termination_overrides_stale_panel_reit_flag(lake):
    con,root=lake;path=root/'panel/year=2021/panel-01.parquet'
    panel=pq.read_table(path).to_pylist()
    for row in panel:
        if row['security_id']==2:row['is_reit']=True
    pq.write_table(pa.Table.from_pylist(panel),path)
    _w(root,'issuer_classes/reit_status.parquet',[{'cik':102,'accession':'termination','accepted_at':T(2021,6,1,18,tzinfo=UTC),'available_at':T(2021,6,2,4,tzinfo=UTC),'reit_status':False}])
    U.load_issuer_tables(con);U.build_year(con,2021)
    assert _flag(con,'is_reit',2,CAL[0])
    assert not _flag(con,'univ_all_common',2,CAL[0])
    assert not _flag(con,'is_reit',2,D(2021,6,3))
    assert _flag(con,'univ_all_common',2,D(2021,6,3))


def test_retained_spac_sic_with_operating_revenue(lake):
    con, root = lake
    path = root / "panel/year=2021/panel-01.parquet"
    rows = pq.read_table(path).to_pylist()
    for row in rows:
        if row["security_id"] == 8 and row["session_date"] >= D(2021, 6, 3):
            row["sale_ttm"] = 1000000.0
    pq.write_table(pa.Table.from_pylist(rows), path)
    con.execute("DELETE FROM sic_hist WHERE cik=108 AND sic=7372")
    U.build_year(con, 2021)
    assert _flag(con, "is_spac", 8, D(2021, 6, 2))
    assert not _flag(con, "is_spac", 8, D(2021, 6, 3))
    assert _flag(con, "univ_all_common", 8, D(2021, 6, 3))


def test_bdc_re_election_and_future_withdrawal_do_not_revise_history(lake):
    con, root = lake
    path = root / "sec_filings/filings.parquet"
    rows = pq.read_table(path).to_pylist()
    rows += [{"cik": 110, "form": "N-54A", "file_number": "814-00002",
              "available_at": T(2021, 6, 4, 12)}]
    pq.write_table(pa.Table.from_pylist(rows), path)
    U.load_issuer_tables(con)
    U.build_year(con, 2021)
    before = [_flag(con, "is_bdc", 10, d) for d in CAL]
    assert before == [True, True, False, False, True, True]
    rows += [{"cik": 110, "form": "N-54C", "file_number": "814-00002",
              "available_at": T(2022, 6, 4, 12)}]
    pq.write_table(pa.Table.from_pylist(rows), path)
    U.load_issuer_tables(con)
    U.build_year(con, 2021)
    assert [_flag(con, "is_bdc", 10, d) for d in CAL] == before


def test_link_table_fallback_is_point_in_time_and_never_fans_out(lake):
    con, _ = lake
    assert _flag(con, "cik_u", 11, D(2021, 6, 3)) is None and not _flag(con, "univ_all_common", 11, D(2021, 6, 3))
    assert _flag(con, "cik_u", 11, D(2021, 6, 4)) == 111 and _flag(con, "univ_all_common", 11, D(2021, 6, 4))
    assert _flag(con, "cik_u", 1, D(2021, 6, 4)) == 101         # the panel CIK wins over the link table
    n, k = con.execute("SELECT count(*), count(DISTINCT (session_date, security_id)) FROM t").fetchone()
    assert n == k == len(CAL) * len(LINES)                      # overlapping link rows for line 1 do not duplicate


def test_index_membership_as_of(lake):
    con, _ = lake
    assert all(_flag(con, "r3000", 1, d) for d in CAL)
    assert not _flag(con, "r2000", 2, D(2021, 6, 7)) and _flag(con, "r2000", 2, D(2021, 6, 8))
    assert not _flag(con, "r3000", 4, D(2021, 6, 8))


def test_year_receipt_counts(lake):
    con, _ = lake
    rec = {r[0]: dict(zip(U.DAILY_COLS, r[1:])) for r in U.year_receipt(con)}
    r = rec[D(2021, 6, 7)]
    assert r["all_common"] == 5 and r["all_common_exchange_repaired"] == 1 and r["nyse_all_common"] == 2
    # line 2 has no visible code run on 06-07: all_common without shrcd
    assert r["all_common_with_codes"] == 4 and r["all_common_exchcd_from_repair"] == 1
    assert r["all_common_no_price_screen"] == 6                 # + line 4


def test_pure_maps_and_patterns():
    import re

    assert U.EXCHANGE_MAP["SC"] == "XNAS" and U.EXCHANGE_MAP["BZX"] == "BATS" and U.EXCHANGE_MAP["NNM"] == "XNAS"
    rx = re.compile(U.SUFFIX_RE)
    assert all(rx.search(t) for t in ("BAC.PRN", "AMBC.WS", "IPOC.U", "ACP.RT", "KSU.PR"))
    assert not any(rx.search(t) for t in ("BRK.B", "BF.A", "MOG.A", "AAPL"))
    assert {"DE", "PR", "X1", "US"} <= set(U.US_JURISDICTIONS) and "E9" not in U.US_JURISDICTIONS


def test_finra_staleness_matches_panel():
    from atx_db.alpha_panel import panel

    assert U.FINRA_STALE_DAYS == panel.SI_STALE_DAYS


def test_band_gate():
    daily = {f"{y}-01-0{i}": {"all_common": v} for y, vals in ((2020, (3300, 3500)), (2021, (3100, 3200)),
                                                               (2022, (4000,)), (2023, (4000,)), (2024, (4000,)),
                                                               (2025, (3250,))) for i, v in enumerate(vals, 1)}
    g = U.band_gate(daily)
    assert not g["pass"] and g["per_year"]["2021"]["mean"] == 3150 and g["per_year"]["2020"]["min"] == 3300
    daily["2021-01-03"] = {"all_common": 3500}
    assert U.band_gate(daily)["pass"]                                # 2021 mean 3,266.7


def test_french_gate():
    ours = {D(2021, 1, 31): (1050, 1000), D(2021, 2, 28): (1060, 1010), D(2023, 1, 31): (5000, 5000)}
    fr = {D(2021, 1, 31): 1000, D(2021, 2, 28): 1000, D(2023, 1, 31): 1000}
    g = U.french_gate(ours, fr)
    assert g["months"] == 2 and g["outside"] == {"2021-02": 0.06} and not g["pass"]   # 2023 is past the window
    assert U.french_gate({D(2021, 1, 31): (1049, 900)}, fr)["pass"]


def test_r2000_and_hand_check_gates(tmp_path):
    assert U.r2000_gate({"2021-06-28": 2000, "2022-06-27": 1950})["pass"]
    assert not U.r2000_gate({"2021-06-28": 2200})["pass"] and not U.r2000_gate({})["pass"]
    p = tmp_path / "hc.json"
    rows = [{"stratum": "is_adr", "verdict": "correct"}] * 96 + [{"stratum": "is_reit", "verdict": "wrong"}] * 4
    p.write_text(json.dumps({"seed": 1, "rows": rows}), encoding="utf-8")
    g = U.hand_check_gate(p)
    assert g["pass"] and g["precision"] == 0.96 and g["by_stratum"]["is_reit"] == {"correct": 0, "lines": 4}
    p.write_text(json.dumps({"seed": 1, "rows": rows[:50]}), encoding="utf-8")
    assert not U.hand_check_gate(p)["pass"]                          # fewer than 100 lines
    assert U.hand_check_gate(tmp_path / "none.json")["status"] == "missing"


def test_build_publishes_manifest_with_gate_status_and_resumes(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    make_lake(tmp_path)
    for name in U.INPUTS:
        stage, _, part = name.partition("/")
        f = tmp_path / stage / (part or "manifest.json")
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(json.dumps({"stage": name}), encoding="utf-8")
    _w(tmp_path, "indexes/recon_summary.parquet", [{"index_id": "R2000P", "effective_from": D(2021, 6, 8)}])
    fr = tmp_path / "french.parquet"
    pq.write_table(pa.Table.from_pylist([{"month_end": D(2021, 6, 30), "n_nyse_firms": 2}]), fr)
    monkeypatch.setattr(U, "FRENCH_BREAKPOINTS", fr)
    monkeypatch.setattr(U, "HAND_CHECK", tmp_path / "no_hand_check.json")
    monkeypatch.setattr(U, "coverage", lambda con, years=U.GATE_YEARS: {})
    monkeypatch.setattr(U, "hand_check_sample", lambda con, seed=U.HAND_SEED: [])
    rec = U.build(range(2021, 2022))
    man = json.loads((tmp_path / "universe" / "manifest.json").read_text(encoding="utf-8"))
    assert man["schema"] == U.SCHEMA and "year=2021/universe.parquet" in man["files"]
    assert set(man["input_manifests_sha256"]) == set(U.INPUTS)
    gs = man["gate_status"]
    assert gs["pass"] is False and gs["per_gate"]["hand_check"] is False      # verdict file missing
    val = json.loads((tmp_path / "validation" / "universe.json").read_text(encoding="utf-8"))
    fm = val["gates"]["french_nyse"]
    assert fm["monthly"] == [{"month": "2021-06", "french": 2, "lines": 2, "companies": 2, "dev_lines": 0.0,
                              "dev_companies": 0.0}]                       # lines 1 and 8 on 2021-06-08
    assert val["gates"]["r2000"]["companies"] == {"2021-06-08": 1}
    assert val["gates"]["codes_on_all_common"]["per_year"]["2021"] < 1     # line 2 lacks codes on 06-07 / 06-08
    assert rec["rows"]["2021"] == len(CAL) * len(LINES) and rec["resumed_years"] == []
    assert U.build(range(2021, 2022))["resumed_years"] == [2021]

def test_french_expected_months_cannot_pass_incomplete_overlap():
    ours={D(2021,1,31):(1000,1000)}
    french={D(2021,1,31):1000,D(2021,2,28):1000}
    g=U.french_gate(ours,french,expected=list(french))
    assert not g['pass'] and g['missing_months']==['2021-02']


def test_hand_verdicts_require_current_rule_and_sample_binding(tmp_path):
    p=tmp_path/'hand.json'
    p.write_text(json.dumps({'rule':'old','seed':U.HAND_SEED,'rows':[]}),encoding='utf-8')
    g=U.hand_check_gate(p,sample=[])
    assert not g['pass'] and g['status']=='stale_verdicts_require_recheck'


def test_source_plan_load_future_extension_preserves_prior_membership(tmp_path,monkeypatch):
    from types import SimpleNamespace
    from atx_db.alpha_panel import issuer_history as I
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path))
    make_lake(tmp_path)
    rawroot=tmp_path/'raw';rawroot.mkdir()
    (rawroot/I.H.FETCH_LEDGER_NAME).write_text('fixture ledger')
    monkeypatch.setattr(I,'RAW',rawroot)
    monkeypatch.setattr(C,'write_stage_manifest',lambda *args,**kwargs:None)
    stage=C.stage_dir(I.STAGE);stage.mkdir(parents=True,exist_ok=True)
    C.write_json_atomic(stage/'fetch_status.json',{})
    def row(accession,day,state):
        return {'cik':102,'accession':accession,'form':'10-Q','filing_date':day,'priority':1,'url':accession,'state':state}
    prior=row('0000000102-21-000000',D(2021,5,28),'DE')
    june=row('0000000102-21-000001',D(2021,6,2),'E9');november=row('0000000102-21-000002',D(2021,11,1),'DE')
    raws={}
    for r in (prior,june,november):
        stamp=r['filing_date'].strftime('%Y%m%d')
        raws[r['url']]=f'<SEC-HEADER><ACCEPTANCE-DATETIME>{stamp}100000\nACCESSION NUMBER: {r["accession"]}\nCONFORMED SUBMISSION TYPE: 10-Q\nFILED AS OF DATE: {stamp}\nCOMPANY DATA:\nCENTRAL INDEX KEY: 102\nSTATE OF INCORPORATION: {r["state"]}\nBUSINESS ADDRESS:\nSTATE: NY\n</SEC-HEADER>'.encode()
    store=SimpleNamespace(load=lambda:store,lookup=lambda url:SimpleNamespace(ok=True,sha256=url,fetched_at='fixture',bytes=len(raws[url]),url=url),read=lambda rec,maximum:raws[rec.url])
    monkeypatch.setattr(I.H,'FetchLedgerStore',lambda root:store)
    def publish(rows):
        C.write_json_atomic(stage/'plan.json',{'rows':I.retain_plan_observations(rows,stage/'plan.json')})
        I.load()
        con=C.connect(memory='200MB',threads=1)
        try:
            U.load_issuer_tables(con);U.build_year(con,2021)
            return con.execute('SELECT session_date,security_id,univ_all_common,is_reit,is_foreign_issuer FROM t ORDER BY session_date,security_id').fetchall()
        finally:con.close()
    before=publish([prior,june])
    after=publish(I.select_observations([prior,june,november],D(2021,6,1)))
    assert before==after
    line=[r for r in before if r[1]==2]
    assert [r[2] for r in line]==[True,True,True,False,False,False]
    assert [r[4] for r in line]==[False,False,False,True,True,True]
    assert {r['accession'] for r in C.read_json(stage/'plan.json')['rows']}=={prior['accession'],june['accession'],november['accession']}



@pytest.mark.parametrize('initial,pending',[
    ('We have elected to be taxed as a REIT. We will terminate our election next year.','We will terminate our election next year.'),
    ('We are a REIT.','The Company is no longer required to operate under REIT rules if its revocation is accepted.'),
    ('We are no longer a REIT.','Assuming the IRS approves the pending election and all other conditions are met, we are a REIT.'),
    ('We are no longer a REIT.','We are a REIT only if the IRS approves our election.'),
])
def test_pending_primary_assertion_loads_no_event_or_membership_transition(tmp_path,monkeypatch,initial,pending):
    from types import SimpleNamespace
    from atx_db.alpha_panel import issuer_history as I,issuer_classes as K
    monkeypatch.setenv('ATX_ALPHA_PANEL_ROOT',str(tmp_path));make_lake(tmp_path)
    rawroot=tmp_path/'raw';rawroot.mkdir();(rawroot/I.H.FETCH_LEDGER_NAME).write_text('fixture')
    monkeypatch.setattr(I,'RAW',rawroot);monkeypatch.setattr(C,'write_stage_manifest',lambda *a,**k:None)
    stage=C.stage_dir(K.STAGE);stage.mkdir(parents=True,exist_ok=True);C.write_json_atomic(stage/'fetch_status.json',{})
    rows=[];raws={}
    for i,(day,assertion) in enumerate(((D(2021,5,28),initial),(D(2021,6,2),pending)),1):
        accession=f'0000000112-21-{i:06d}';stamp=day.strftime('%Y%m%d');h=accession+'header';p=accession+'primary'
        rows.append({'cik':112,'accession':accession,'form':'10-Q','filing_date':day,'header_url':h,'primary_url':p})
        raws[h]=f'<SEC-HEADER><ACCEPTANCE-DATETIME>{stamp}100000\nACCESSION NUMBER: {accession}\nCONFORMED SUBMISSION TYPE: 10-Q\nFILED AS OF DATE: {stamp}\nCOMPANY DATA:\nCOMPANY CONFORMED NAME: Example Corp\nCENTRAL INDEX KEY: 112\nSTATE OF INCORPORATION: DE\nBUSINESS ADDRESS:\nSTATE: NY\n</SEC-HEADER>'.encode()
        raws[p]=('<ix:nonNumeric name="dei:EntityCentralIndexKey">112</ix:nonNumeric>'+assertion).encode()
    store=SimpleNamespace(load=lambda:store,lookup=lambda url:SimpleNamespace(ok=True,sha256=url,fetched_at='fixture',bytes=len(raws[url]),url=url),read=lambda rec,maximum:raws[rec.url])
    monkeypatch.setattr(I.H,'FetchLedgerStore',lambda root:store)
    def publish(selected):
        C.write_json_atomic(stage/'plan.json',{'rows':I.retain_plan_observations(selected,stage/'plan.json')});K.load()
        con=C.connect(memory='200MB',threads=1)
        try:
            U.load_issuer_tables(con);U.build_year(con,2021)
            return con.execute('SELECT session_date,security_id,is_reit,univ_all_common FROM t ORDER BY session_date,security_id').fetchall()
        finally:con.close()
    before=publish(rows[:1]);after=publish(rows)
    assert before==after
    events=pq.read_table(stage/'reit_status.parquet').to_pylist()
    assert len(events)==1 and events[0]['accession']==rows[0]['accession']
    assessment=C.read_json(stage/'assessment.json')['rows']
    assert assessment[1]['status'] is None
    if 'have elected' in initial:
        termination={**rows[1],'accession':'0000000112-21-000003','filing_date':D(2021,6,4),'header_url':'completed-header','primary_url':'completed-primary'}
        raws[termination['header_url']]=raws[rows[1]['header_url']].replace(b'20210602',b'20210604').replace(rows[1]['accession'].encode(),termination['accession'].encode())
        raws[termination['primary_url']]=b'<ix:nonNumeric name="dei:EntityCentralIndexKey">112</ix:nonNumeric>We are no longer a REIT.'
        final=publish(rows+[termination])
        line=[r for r in final if r[1]==12]
        assert [r[2] for r in line]==[True,True,True,True,True,False]
        assert [r[3] for r in line]==[False,False,False,False,False,True]
        events=pq.read_table(stage/'reit_status.parquet').to_pylist()
        assert [r['reit_status'] for r in events]==[True,False]
