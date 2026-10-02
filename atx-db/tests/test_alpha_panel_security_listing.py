"""S2.2 rule ``line-listing-v1`` on a fixture lake: confirmed IPO, vendor-batch censoring, registration before
the vendor, successor re-key, unconfirmed first session, delisting form, former-name history."""

import datetime as dt

import duckdb
import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import common as C
from atx_db.alpha_panel import listing_events as LE

D = dt.date


def _write(path, cols: dict, schema=None) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    pq.write_table(pa.table(cols, schema=schema), path)


@pytest.fixture()
def lake(tmp_path, monkeypatch):
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path))
    monkeypatch.setattr(LE, "BATCH_MIN_LINES", 2)
    lines = [  # security_id, first, last
        (10, D(2020, 6, 1), D(2026, 9, 18)),     # IPO confirmed by 8-A12B + 424B4
        (11, D(2016, 1, 4), D(2026, 9, 18)),     # vendor batch, registered in 1998
        (12, D(2016, 1, 4), D(2026, 9, 18)),     # vendor batch, nothing known -> censored
        (14, D(2013, 5, 1), D(2015, 10, 2)),     # predecessor, ends before the re-key
        (13, D(2015, 10, 5), D(2026, 9, 18)),    # successor of 14 (same CIK)
        (15, D(2019, 3, 1), D(2021, 2, 1)),      # unlinked, unconfirmed
    ]
    _write(tmp_path / "security_master" / "lines.parquet",
           {"security_id": [r[0] for r in lines], "first_session": [r[1] for r in lines],
            "last_session": [r[2] for r in lines], "left_censored": [False] * len(lines),
            "last_ticker": ["AAA", "BBB", "CCC", "DDD", "DDD2", "EEE"], "is_index_line": [False] * len(lines)})
    th = tmp_path / "th.parquet"
    rows = [(r[1], r[0], t) for r, t in zip(lines, ["AAA", "BBB", "CCC", "DDD", "DDD2", "EEE"])]
    _write(th, {"tradingDate": [r[0] for r in rows], "securityID": [r[1] for r in rows], "ticker_tk": [r[2] for r in rows]})
    monkeypatch.setattr(C, "TICKERHISTORY", th)
    links = [(10, 1000, D(2020, 6, 2)), (11, 1100, D(2016, 1, 5)), (12, 1200, D(2016, 1, 5)), (14, 1300, D(2013, 5, 2)),
             (13, 1300, D(2015, 10, 6))]
    _write(tmp_path / "identity" / "links.parquet",
           {"security_id": [r[0] for r in links], "cik": [r[1] for r in links], "start": [r[2] for r in links]})
    _write(tmp_path / "identity" / "link_table.parquet",
           {"security_id": [10], "cik": [1000], "valid_from": [D(2020, 6, 2)], "link_tier": ["strict"]})
    fil = [  # cik, accession, form, filing_date
        (1000, "a1", "8-A12B", D(2020, 5, 28)), (1000, "a2", "424B4", D(2020, 6, 2)), (1000, "a3", "10-K", D(2021, 3, 1)),
        (1300, "a4", "25", D(2015, 9, 25)),
    ]
    ts = [dt.datetime.combine(r[3], dt.time(20)) for r in fil]
    _write(tmp_path / "sec_filings" / "filings.parquet",
           {"cik": [r[0] for r in fil], "accession": [r[1] for r in fil], "form": [r[2] for r in fil],
            "filing_date": [r[3] for r in fil], "available_at": ts, "available_basis": ["acceptance"] * len(fil),
            "file_number": [None] * len(fil), "is_amendment": [False] * len(fil)})
    sub = tmp_path / "_tmp" / "identity_v3" / "sub"
    _write(sub / "events_0000.parquet",
           {"cik": [1100, 1000], "source_file": [1, 0], "accession": ["z1", "a1"], "form": ["8-A12B", "8-A12B"],
            "filing_date": ["1998-06-19", "2020-05-28"], "acceptance_raw": [None, None], "file_number": [None, None],
            "report_date": [None, None]}, schema=LE.EVENT_SCHEMA)
    filers = [{"cik": 1300, "name": "NEWCO INC", "entity_type": "operating", "lei": None, "ein": None,
               "state_of_incorporation": "DE", "tickers": ["DDD2"], "exchanges": ["NYSE"],
               **{f"business_{k}": None for k in LE.ADDR_FIELDS}, **{f"mailing_{k}": None for k in LE.ADDR_FIELDS},
               "former_names": [{"name": "OLDCO INC", "from_date": D(2001, 1, 2), "to_date": D(2015, 10, 1)}]}]
    _write(sub / "filers_0000.parquet", {k: [r[k] for r in filers] for k in filers[0]}, schema=LE.FILER_SCHEMA)
    _write(tmp_path / "_tmp" / "panel_member" / "year=2020.parquet",
           {"session_date": [D(2020, 7, 1)], "security_id": [10], "member": [True]})
    return tmp_path


def test_line_listing_rule(lake) -> None:
    rec = LE.build()
    con = duckdb.connect()
    got = {r[0]: r[1:] for r in con.execute(f"""
        SELECT security_id, listing_basis, listing_date, listing_type, predecessor_security_id, delisting_form
        FROM read_parquet('{(lake / 'security_master' / 'line_listing.parquet').as_posix()}')""").fetchall()}
    assert got[10] == ("sec_confirmed", D(2020, 6, 1), "ipo", None, None)
    assert got[11] == ("sec_registration_before_vendor", D(1998, 6, 19), None, None, None)
    assert got[12] == ("censored", None, None, None, None)
    assert got[13] == ("successor_of_line", D(2013, 5, 1), None, 14, None)
    assert got[14][0] == "vendor_first_session" and got[14][4] == "25"
    assert got[15] == ("vendor_first_session", D(2019, 3, 1), None, None, None)
    av = con.execute(f"""SELECT listing_available_at FROM read_parquet('{(lake / 'security_master' / 'line_listing.parquet').as_posix()}')
                         WHERE security_id = 10""").fetchone()[0]
    assert av == dt.datetime(2020, 6, 1, 22)                    # never before the first session's mark
    names = con.execute(f"""SELECT name, valid_from, valid_to FROM read_parquet('{(lake / 'security_master' / 'name_history.parquet').as_posix()}')
                            WHERE cik = 1300 ORDER BY valid_from""").fetchall()
    assert names == [("OLDCO INC", D(2001, 1, 2), D(2015, 10, 1)), ("NEWCO INC", D(2015, 10, 2), None)]
    ev = con.execute(f"""SELECT count(*) FILTER (WHERE source = 'submissions_zip'), count(*)
                         FROM read_parquet('{(lake / 'security_master' / 'listing_events.parquet').as_posix()}')""").fetchone()
    assert ev == (1, 4)       # the archive adds 1998 only; a1 comes from sec_filings once
    assert rec["line_listing"]["done_measure"]["all_lines"]["lines_first_seen_2013_plus"] == 6
