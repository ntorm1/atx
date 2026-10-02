"""Pure helpers of the SHORTFLOW alpha-panel stages (ftd, regsho_threshold, short_volume_ext, thirteenf)."""

from __future__ import annotations

import datetime as dt

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from atx_db.alpha_panel import ftd, regsho, short_volume_ext, thirteenf
from atx_db.alpha_panel import shortflow_common as S

UTC = dt.UTC


# ---------------------------------------------------------------- FTD
FTD_TEXT = "\r\n".join([
    "SETTLEMENT DATE|CUSIP|SYMBOL|QUANTITY (FAILS)|DESCRIPTION|PRICE",
    "20210115|36467W109|GME|892653|GAMESTOP CORP (HLDG CO) CL A|39.91",
    "20210119|037833100|AAPL|1200|APPLE INC|.",
    "20210120|G1234X101||500|NO SYMBOL CO|",
    "20210121|12345P105|ABC|700|ALPHA | BETA CO|1.25",
    "2021012|BAD|ROW|1|X|1",
    "Trailer record count 4",
    "Trailer total quantity of shares 895053",
])


def test_ftd_parse_rows_trailer_and_prices() -> None:
    rows, st = ftd.parse_text(FTD_TEXT)
    assert st["header_ok"] and st["trailer_count"] == 4 and st["trailer_quantity"] == 895053
    assert st["bad"] == 1 and st["joined_description"] == 1 and len(rows) == 4
    assert rows[0] == (dt.date(2021, 1, 15), "36467W109", "GME", 892653.0, "GAMESTOP CORP (HLDG CO) CL A", 39.91)
    assert rows[1][5] is None                      # '.' = price unavailable or below a penny
    assert rows[2][2] is None and rows[2][5] is None
    assert rows[3][4] == "ALPHA | BETA CO" and rows[3][5] == 1.25


def test_ftd_half_month_schedule_and_clock() -> None:
    assert ftd.nominal_date("202101a") == dt.date(2021, 1, 31)
    assert ftd.nominal_date("202101b") == dt.date(2021, 2, 15)
    assert ftd.nominal_date("202112b") == dt.date(2022, 1, 15)
    assert ftd.half_bounds("202402b") == (dt.date(2024, 2, 15), dt.date(2024, 2, 29))
    assert ftd.half_bounds("202402a") == (dt.date(2024, 2, 1), dt.date(2024, 2, 14))
    rule = dt.datetime(2021, 2, 22, tzinfo=UTC)
    assert ftd.available_at("202101b") == (rule, "rule")
    early = dt.datetime(2021, 2, 16, 14, 51, tzinfo=UTC)          # observed Last-Modified before the rule
    assert ftd.available_at("202101b", early) == (rule, "rule")
    late = dt.datetime(2021, 3, 1, 9, 0, tzinfo=UTC)              # a late first posting inside 60 days wins
    assert ftd.available_at("202101b", late) == (late, "last_modified_after_rule")
    repost = dt.datetime(2023, 12, 19, 9, 0, tzinfo=UTC)          # a re-post years later is not evidence
    assert ftd.available_at("202101b", repost) == (rule, "rule")


def test_ftd_discover_keeps_suffixed_links() -> None:
    html = ('<a href="/files/data/fails-deliver-data/cnsfails202308b_0.zip">x</a>'
            '<a href="/files/data/fails-deliver-data/cnsfails202309a.zip">y</a>'
            '<a href="/files/data/x/cnsp_sec_fails_2008q1.zip">z</a>')
    got = ftd.discover(html)
    assert got == {"202308b": "https://www.sec.gov/files/data/fails-deliver-data/cnsfails202308b_0.zip",
                   "202309a": "https://www.sec.gov/files/data/fails-deliver-data/cnsfails202309a.zip"}


# ---------------------------------------------------------------- shared
def test_canonical_symbol_is_case_sensitive() -> None:
    assert S.canon("BRK.B") == S.canon("BRK/B") == S.canon("BRKB") == "BRKB"
    assert S.canon(" AIG-WS ") == "AIGWS"
    assert S.canon("TpC") != S.canon("TPC")


def test_sec_business_days_and_13f_deadline() -> None:
    assert dt.date(2021, 2, 15) in S.federal_holidays(2021)          # Presidents' Day
    assert dt.date(2022, 6, 20) in S.federal_holidays(2022)          # Juneteenth observed (Sun -> Mon)
    assert dt.date(2021, 12, 31) in S.federal_holidays(2021)         # New Year 2022 (Sat) observed
    assert thirteenf.deadline45(dt.date(2021, 3, 31)) == dt.date(2021, 5, 17)   # May 15 = Saturday
    assert thirteenf.deadline45(dt.date(2020, 12, 31)) == dt.date(2021, 2, 16)  # Feb 14 Sun, Feb 15 holiday
    assert thirteenf.deadline45(dt.date(2023, 6, 30)) == dt.date(2023, 8, 14)


def _vendor(tmp_path, rows):
    t = pa.table({"d": pa.array([r[0] for r in rows], pa.date32()), "security_id": pa.array([r[1] for r in rows], pa.int64()),
                  "tk": [r[2] for r in rows], "ck": [S.canon(r[2]) for r in rows], "sid_repaired": [False] * len(rows)})
    p = tmp_path / "vendor.parquet"
    pq.write_table(t, p)
    return p


def test_map_symbols_asof_lookback_fallback_and_ambiguity(tmp_path, monkeypatch) -> None:
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "root"))
    d = dt.date
    vendor = _vendor(tmp_path, [
        (d(2021, 1, 4), 1, "AAA"), (d(2021, 1, 5), 1, "AAA"),
        (d(2021, 1, 4), 2, "BRK.B"), (d(2021, 1, 5), 2, "BRK.B"),
        (d(2021, 1, 4), 3, "THIN"),                                 # absent on the last session
        (d(2021, 1, 5), 4, "DUP"), (d(2021, 1, 5), 5, "DUP"),        # two ids carry the form
        (d(2020, 12, 1), 6, "OLD"),                                  # beyond the 7-day look-back
        (d(2021, 1, 6), 7, "FUT"),                                   # after the key date: never read
    ])
    con = S.C.connect(memory="200MB", threads=1)
    keys = ("SELECT * FROM (VALUES (DATE '2021-01-05', 'AAA'), (DATE '2021-01-05', 'BRKB'), (DATE '2021-01-05', 'THIN'), "
            "(DATE '2021-01-05', 'DUP'), (DATE '2021-01-05', 'OLD'), (DATE '2021-01-05', 'FUT')) t(key_date, symbol)")
    dest = tmp_path / "m.parquet"
    S.map_symbols_asof(con, keys, vendor, dest)
    got = {r["symbol"]: r for r in pq.read_table(dest).to_pylist()}
    assert got["AAA"]["candidate_security_id"] == 1 and got["AAA"]["exact"] and not got["AAA"]["vendor_date_fallback"]
    assert got["BRKB"]["candidate_security_id"] == 2 and not got["BRKB"]["exact"]
    assert got["THIN"]["candidate_security_id"] == 3 and got["THIN"]["vendor_date_fallback"]
    assert got["DUP"]["outcome"] == "ambiguous" and got["DUP"]["candidate_security_id"] is None
    assert got["OLD"]["outcome"] == "unmapped"
    assert got["FUT"]["outcome"] == "unmapped"


# ---------------------------------------------------------------- Reg SHO threshold lists
def test_regsho_parse_pipe_with_trailer_stamp() -> None:
    text = ("Symbol|Security Name|Market Category|Reg SHO Threshold Flag|Filler|Filler\n"
            "GME|GameStop Corp. Class A|NYSE|Y||\n20210128210501\r\n")
    rows, stamp, _ = regsho.parse_pipe(text)
    assert stamp == "20210128210501" and len(rows) == 1
    n = regsho.normalise("nyse", rows[0])
    assert n == {"symbol": "GME", "security_name": "GameStop Corp. Class A", "market_category": "NYSE",
                 "on_list": True, "rule_flag": None}
    assert regsho.stamp_et_to_utc("20210128210501") == dt.datetime(2021, 1, 29, 2, 5, 1, tzinfo=UTC)
    assert regsho.stamp_et_to_utc("20240603220202") == dt.datetime(2024, 6, 4, 2, 2, 2, tzinfo=UTC)


def test_regsho_finra_rows_and_status() -> None:
    row = {"tradeDate": "2021-01-28", "issueSymbolIdentifier": "ALLIF", "issueName": "Alpha Lithium",
           "marketClassCode": "OTC", "thresholdListFlag": "NR", "marketCategoryDescription": "Other OTC",
           "regShoThresholdFlag": "N", "rule4320Flag": "Y"}
    n = regsho.normalise("finra_otc", row)
    assert n["on_list"] is False and n["rule_flag"] == "Y"
    old_style = dict(row, regShoThresholdFlag="", rule4320Flag="", thresholdListFlag="R")   # 2018 layout
    assert regsho.normalise("finra_otc", old_style)["on_list"] is True
    assert regsho.normalise("finra_otc", dict(old_style, thresholdListFlag="NR"))["rule_flag"] == "Y"
    assert regsho.classify("nyse", 200, 0, "20210130210500", True) == "empty_or_absent"
    assert regsho.classify("nyse", 200, 0, "20210129210501", True) == "empty_list"
    assert regsho.classify("nasdaq", 404, 0, None, False) == "absent"
    assert regsho.classify("cboe_bzx", 200, 3, "20210129030212", True) == "list"


def test_regsho_clock_rules() -> None:
    hol = {dt.date(2021, 2, 15)}
    fri = dt.date(2021, 2, 12)
    assert regsho.rule_available_at("nasdaq", fri, hol, None) == (dt.datetime(2021, 2, 13, 6, tzinfo=UTC), "rule")
    assert regsho.rule_available_at("cboe_bzx", fri, hol, None) == (dt.datetime(2021, 2, 16, 16, tzinfo=UTC), "rule")
    late = dt.datetime(2021, 2, 13, 9, tzinfo=UTC)
    assert regsho.rule_available_at("nasdaq", fri, hol, late) == (late, "last_modified_after_rule")
    reupload = dt.datetime(2026, 3, 23, 17, 29, tzinfo=UTC)
    assert regsho.rule_available_at("cboe_bzx", fri, hol, reupload)[1] == "rule"
    assert regsho.next_session(fri, hol) == dt.date(2021, 2, 16)


# ---------------------------------------------------------------- short volume
def test_short_volume_market_field_flags() -> None:
    f = short_volume_ext.parse_market("B,Q,N")
    assert (f["fac_b"], f["fac_q"], f["fac_n"], f["fac_d"], f["fac_other"], f["n_facilities"]) == (True, True, True, False, None, 3)
    f = short_volume_ext.parse_market("Q")
    assert (f["fac_q"], f["fac_n"], f["n_facilities"]) == (True, False, 1)
    assert short_volume_ext.parse_market("D,X")["fac_other"] == "X"
    assert short_volume_ext.parse_market(None)["n_facilities"] == 0


# ---------------------------------------------------------------- 13F
def test_13f_value_unit_rule_is_by_filing_date() -> None:
    assert thirteenf.value_usd(1234.0, dt.date(2022, 11, 14)) == 1_234_000.0
    assert thirteenf.value_usd(1234.0, dt.date(2023, 1, 2)) == 1_234_000.0
    assert thirteenf.value_usd(1234.0, dt.date(2023, 1, 3)) == 1234.0     # Q4-2022 reports filed in 2023: dollars
    assert thirteenf.value_usd(None, dt.date(2023, 5, 1)) is None


def test_13f_cusip_normalisation() -> None:
    assert thirteenf.normalise_cusip("037833100") == ("037833100", False)
    assert thirteenf.normalise_cusip("37833100") == ("037833100", True)      # leading zero dropped by a spreadsheet
    assert thirteenf.normalise_cusip(" g1151c-101 ") == ("G1151C101", False)
    assert thirteenf.normalise_cusip("") == (None, False)


def test_13f_archive_names_and_quarters() -> None:
    assert thirteenf.archive_period("https://x/2020q1_form13f.zip") == ("2020q1", dt.date(2020, 1, 1), dt.date(2020, 3, 31))
    assert thirteenf.archive_period("https://x/01jan2024-29feb2024_form13f.zip") == (
        "01jan2024-29feb2024", dt.date(2024, 1, 1), dt.date(2024, 2, 29))
    assert thirteenf.quarter_end(dt.date(2021, 3, 30)) == dt.date(2021, 3, 31)
    assert thirteenf.prior_quarter_end(dt.date(2021, 3, 31)) == dt.date(2020, 12, 31)
    assert thirteenf.prior_quarter_end(dt.date(2021, 9, 30)) == dt.date(2021, 6, 30)


@pytest.mark.parametrize("filings, expected", [
    ([("a1", dt.date(2021, 5, 10), "13F-HR", None, None)], ["a1"]),
    # restatement replaces the original
    ([("a1", dt.date(2021, 5, 10), "13F-HR", None, None), ("a2", dt.date(2021, 6, 1), "13F-HR/A", "Y", "RESTATEMENT")], ["a2"]),
    # new holdings add to the original
    ([("a1", dt.date(2021, 5, 10), "13F-HR", None, None), ("a2", dt.date(2021, 6, 1), "13F-HR/A", "Y", "NEW HOLDINGS")], ["a1", "a2"]),
    # a new-holdings amendment before a restatement is superseded by it
    ([("a1", dt.date(2021, 5, 10), "13F-HR", None, None), ("a2", dt.date(2021, 5, 20), "13F-HR/A", "Y", "NEW HOLDINGS"),
      ("a3", dt.date(2021, 7, 1), "13F-HR/A", "Y", "RESTATEMENT")], ["a3"]),
    # notices never count; an amendment without a base stands alone
    ([("n1", dt.date(2021, 5, 10), "13F-NT", None, None), ("a2", dt.date(2021, 6, 1), "13F-HR/A", "Y", "NEW HOLDINGS")], ["a2"]),
])
def test_13f_effective_accessions(filings, expected) -> None:
    assert thirteenf.effective_accessions(filings) == expected


def test_13f_effective_sql_matches_python(tmp_path, monkeypatch) -> None:
    monkeypatch.setenv("ATX_ALPHA_PANEL_ROOT", str(tmp_path / "root"))
    con = S.C.connect(memory="200MB", threads=1)
    rows = [("a1", "c1", dt.date(2021, 3, 31), dt.date(2021, 5, 10), "13F-HR", None, dt.date(2021, 5, 17)),
            ("a2", "c1", dt.date(2021, 3, 31), dt.date(2021, 5, 12), "13F-HR/A", "NEW HOLDINGS", dt.date(2021, 5, 17)),
            ("a3", "c1", dt.date(2021, 3, 31), dt.date(2021, 8, 1), "13F-HR/A", "RESTATEMENT", dt.date(2021, 5, 17)),
            ("b1", "c2", dt.date(2021, 3, 31), dt.date(2021, 9, 1), "13F-HR", None, dt.date(2021, 5, 17))]
    con.execute("CREATE TEMP TABLE fl (accession VARCHAR, filer_cik VARCHAR, period_q DATE, filing_date DATE, "
                "submission_type VARCHAR, amendment_type VARCHAR, deadline DATE)")
    con.executemany("INSERT INTO fl VALUES (?, ?, ?, ?, ?, ?, ?)", rows)
    final = sorted(r[0] for r in con.execute(thirteenf._effective_sql(None)).fetchall())
    asof = sorted(r[0] for r in con.execute(thirteenf._effective_sql("deadline")).fetchall())
    assert final == ["a3", "b1"]
    assert asof == ["a1", "a2"]
