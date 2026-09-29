"""Pure helpers and the per-quarter DuckDB transform of the alpha panel's insider stage."""

from __future__ import annotations

import datetime as dt
from pathlib import Path

import duckdb
import pytest

from atx_db.alpha_panel import insider as I


def test_quarter_key_and_links() -> None:
    assert I.quarter_key("2015q1") == (2015, 1)
    with pytest.raises(ValueError):
        I.quarter_key("2015q5")
    html = (
        '<a href="/files/structureddata/data/insider-transactions-data-sets/2014q4_form345.zip">x</a>'
        '<a href="/files/structureddata/data/insider-transactions-data-sets/2016q1_form345.zip">x</a>'
        '<a href="/files/structureddata/data/insider-transactions-data-sets/2015q1_form345.zip">x</a>'
        '<a href="/files/datastandardsinnovation/data/insider-transactions-data-sets/2026q2_form345.zip">x</a>'
    )
    links = I.dataset_links(html, "2015q1")
    assert list(links) == ["2015q1", "2016q1", "2026q2"]
    assert links["2026q2"] == ("https://www.sec.gov/files/datastandardsinnovation/data/"
                               "insider-transactions-data-sets/2026q2_form345.zip")


def test_relationship_flags_match_sql() -> None:
    samples = ["Director,TenPercentOwner", "Officer", "Director, Officer", "Other", "", None]
    con = duckdb.connect()
    for s in samples:
        py = I.relationship_flags(s)
        lit = "NULL" if s is None else "'" + s + "'"
        sql = con.execute(f"SELECT {I.rel_sql(lit, 'DIRECTOR')}, {I.rel_sql(lit, 'OFFICER')}, "
                          f"{I.rel_sql(lit, 'TENPERCENTOWNER')}, {I.rel_sql(lit, 'OTHER')}").fetchone()
        assert tuple(sql) == (py["is_director"], py["is_officer"], py["is_ten_percent_owner"], py["is_other"]), s


def test_sec_date_sql() -> None:
    con = duckdb.connect()
    assert con.execute(f"SELECT {I.sec_date_sql(chr(39) + '31-MAR-2015' + chr(39))}").fetchone()[0] == dt.date(2015, 3, 31)
    assert con.execute(f"SELECT {I.sec_date_sql(chr(39) + chr(39))}").fetchone()[0] is None


def _tsv(path: Path, header: list[str], rows: list[list[str]]) -> None:
    path.write_text("\n".join("\t".join(r) for r in [header] + rows) + "\n", encoding="utf-8")


def test_transform_quarter(tmp_path: Path) -> None:
    d = tmp_path / "tsv"
    d.mkdir()
    _tsv(d / "SUBMISSION.tsv",
         ["ACCESSION_NUMBER", "FILING_DATE", "PERIOD_OF_REPORT", "DATE_OF_ORIG_SUB", "NO_SECURITIES_OWNED",
          "NOT_SUBJECT_SEC16", "FORM3_HOLDINGS_REPORTED", "FORM4_TRANS_REPORTED", "DOCUMENT_TYPE", "ISSUERCIK",
          "ISSUERNAME", "ISSUERTRADINGSYMBOL", "REMARKS"],
         [["0001-15-1", "02-JAN-2015", "30-DEC-2014", "", "", "", "", "", "4", "0000320193", "APPLE INC", "AAPL",
           '"remark with ""quotes"""'],
          ["0001-15-2", "05-JAN-2015", "02-JAN-2015", "02-JAN-2015", "", "", "", "", "4/A", "0000000042", "X CO",
           "X", ""]])
    _tsv(d / "REPORTINGOWNER.tsv",
         ["ACCESSION_NUMBER", "RPTOWNERCIK", "RPTOWNERNAME", "RPTOWNER_RELATIONSHIP", "RPTOWNER_TITLE",
          "RPTOWNER_TXT", "RPTOWNER_STREET1", "RPTOWNER_STREET2", "RPTOWNER_CITY", "RPTOWNER_STATE",
          "RPTOWNER_ZIPCODE", "RPTOWNER_STATE_DESC", "FILE_NUMBER"],
         [["0001-15-1", "0001557203", "Riccio Daniel J.", "Officer", "Senior Vice President", "", "", "", "", "",
           "", "", ""],
          ["0001-15-2", "0000000900", "Fund B", "TenPercentOwner", "", "", "", "", "", "", "", "", ""],
          ["0001-15-2", "0000000500", "Fund A", "Director,TenPercentOwner", "", "", "", "", "", "", "", "", ""]])
    nd_h = ["ACCESSION_NUMBER", "NONDERIV_TRANS_SK", "SECURITY_TITLE", "SECURITY_TITLE_FN", "TRANS_DATE",
            "TRANS_DATE_FN", "DEEMED_EXECUTION_DATE", "DEEMED_EXECUTION_DATE_FN", "TRANS_FORM_TYPE", "TRANS_CODE",
            "EQUITY_SWAP_INVOLVED", "EQUITY_SWAP_TRANS_CD_FN", "TRANS_TIMELINESS", "TRANS_TIMELINESS_FN",
            "TRANS_SHARES", "TRANS_SHARES_FN", "TRANS_PRICEPERSHARE", "TRANS_PRICEPERSHARE_FN",
            "TRANS_ACQUIRED_DISP_CD", "TRANS_ACQUIRED_DISP_CD_FN", "SHRS_OWND_FOLWNG_TRANS",
            "SHRS_OWND_FOLWNG_TRANS_FN", "VALU_OWND_FOLWNG_TRANS", "VALU_OWND_FOLWNG_TRANS_FN",
            "DIRECT_INDIRECT_OWNERSHIP", "DIRECT_INDIRECT_OWNERSHIP_FN", "NATURE_OF_OWNERSHIP",
            "NATURE_OF_OWNERSHIP_FN"]
    _tsv(d / "NONDERIV_TRANS.tsv", nd_h,
         [["0001-15-1", "11", "Common Stock", "", "30-DEC-2014", "", "", "", "4", "S", "0", "", "", "", "2904.0",
           "", "112.73", "F1", "D", "", "4704.0", "", "", "", "D", "", "", ""],
          ["0001-15-2", "12", "Common Stock", "", "02-JAN-2015", "", "", "", "4", "P", "0", "", "", "", "100",
           "", "", "", "A", "", "1100", "", "", "", "I", "", "By fund", ""]])
    de_h = ["ACCESSION_NUMBER", "DERIV_TRANS_SK", "SECURITY_TITLE", "SECURITY_TITLE_FN", "CONV_EXERCISE_PRICE",
            "CONV_EXERCISE_PRICE_FN", "TRANS_DATE", "TRANS_DATE_FN", "DEEMED_EXECUTION_DATE",
            "DEEMED_EXECUTION_DATE_FN", "TRANS_FORM_TYPE", "TRANS_CODE", "EQUITY_SWAP_INVOLVED",
            "EQUITY_SWAP_TRANS_CD_FN", "TRANS_TIMELINESS", "TRANS_TIMELINESS_FN", "TRANS_SHARES", "TRANS_SHARES_FN",
            "TRANS_TOTAL_VALUE", "TRANS_TOTAL_VALUE_FN", "TRANS_PRICEPERSHARE", "TRANS_PRICEPERSHARE_FN",
            "TRANS_ACQUIRED_DISP_CD", "TRANS_ACQUIRED_DISP_CD_FN", "EXCERCISE_DATE", "EXCERCISE_DATE_FN",
            "EXPIRATION_DATE", "EXPIRATION_DATE_FN", "UNDLYNG_SEC_TITLE", "UNDLYNG_SEC_TITLE_FN",
            "UNDLYNG_SEC_SHARES", "UNDLYNG_SEC_SHARES_FN", "UNDLYNG_SEC_VALUE", "UNDLYNG_SEC_VALUE_FN",
            "SHRS_OWND_FOLWNG_TRANS", "SHRS_OWND_FOLWNG_TRANS_FN", "VALU_OWND_FOLWNG_TRANS",
            "VALU_OWND_FOLWNG_TRANS_FN", "DIRECT_INDIRECT_OWNERSHIP", "DIRECT_INDIRECT_OWNERSHIP_FN",
            "NATURE_OF_OWNERSHIP", "NATURE_OF_OWNERSHIP_FN"]
    _tsv(d / "DERIV_TRANS.tsv", de_h,
         [["0001-15-1", "21", "Option", "", "17.79", "", "30-DEC-2014", "", "", "", "4", "M", "0", "", "", "", "500",
           "", "", "", "0", "", "D", "", "12-JUL-2016", "", "12-JUL-2023", "", "Common Stock", "", "500", "", "",
           "", "0", "", "", "", "D", "", "", ""]])
    lookup = tmp_path / "lookup.parquet"
    con = duckdb.connect()
    con.execute("SET TimeZone='UTC'")
    con.execute(f"""COPY (SELECT '0001-15-1' AS accession, TIMESTAMP '2015-01-02 22:15:00' AS acceptance_utc,
                          'file_utc' AS acceptance_clock, DATE '2015-01-02' AS filing_date) TO '{lookup.as_posix()}' (FORMAT PARQUET)""")
    tx, own = tmp_path / "tx.parquet", tmp_path / "own.parquet"
    st = I.transform_quarter(con, d, "2015q1", lookup.as_posix(), tx, own)
    assert st["transactions"] == 3 and st["owners"] == 3 and st["submissions"] == 2
    assert st["available_basis"] == {"acceptance": 2, "filing_date_eod_et": 1}
    rows = con.execute(f"""SELECT accession, table_type, issuer_cik, owner_cik, n_reporting_owners, is_director,
                                  any_ten_percent_owner, transaction_code, shares, price, acquired_disposed,
                                  shares_owned_after, is_amendment, available_at, available_basis, exercise_date,
                                  conv_exercise_price, price_footnoted, officer_title
                           FROM read_parquet('{tx.as_posix()}') ORDER BY accession, table_type""").fetchall()
    d1, n1, n2 = rows
    assert d1[:4] == ("0001-15-1", "derivative", 320193, 1557203)
    assert d1[15] == dt.date(2016, 7, 12) and d1[16] == 17.79
    assert n1[7:12] == ("S", 2904.0, 112.73, "D", 4704.0) and n1[17] is True
    assert n1[13] == dt.datetime(2015, 1, 2, 22, 15) and n1[14] == "acceptance"
    assert n1[18] == "Senior Vice President"
    # joint filing: the smallest owner CIK is the primary owner; flags aggregate over owners
    assert n2[3] == 500 and n2[4] == 2 and n2[5] is True and n2[6] is True and n2[12] is True
    assert n2[9] is None and n2[7] == "P"
    assert n2[13] == dt.datetime(2015, 1, 6, 5, 0) and n2[14] == "filing_date_eod_et"  # 2015-01-06 00:00 EST
