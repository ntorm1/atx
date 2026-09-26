"""P9 identity: a delisted issuer's 13F CUSIP maps only inside its dated name window and its
holding periods (never through today's same-named registrant), unmapped rows are counted by
reason, the loader's CUSIP fallback seed is ignored, and FINRA symbols map by symbol-date."""

from __future__ import annotations

import datetime as dt
import json

import duckdb

from atx_db.ownership_identity import stage_13f_cusip_periods, stage_cusip_owner_map, stage_si_line_map
from atx_db.thirteenf import CUSIP_FALLBACK_SOURCE

# (accession, report period) -> [(cusip, issuer name)]
HOLDINGS = {
    ("m-20q1", "2020-03-31"): [("11111111A", "ACME CORP /DE/")],
    ("m-20q2", "2020-06-30"): [("11111111A", "ACME CORP")],
    ("m-20q3", "2020-09-30"): [("11111111A", "ACME CORP /DE/")],
    ("m-21q1", "2021-03-31"): [("11111111A", "ACME CORP")],             # stale: after the name window
    ("m-21q2", "2021-06-30"): [("22222222B", "ACME CORP"),              # today's ACME (new CIK, new CUSIP)
                               ("33333333C", "BERKSHIRE HATHAWAY INC DEL"),
                               ("44444444D", "GAMMA INC"),
                               ("55555555E", "DELTA CO"),
                               ("66666666F", "ZETA WIDGETS")],
}
# (cik, ticker, title)
SEC_TICKERS = [("200", "ACME", "Acme Corp"), ("300", "BRK-B", "Berkshire Hathaway Inc"),
               ("401", "GAM", "Gamma Inc"), ("402", "GAMC", "GAMMA CORP"),
               ("501", "DLT", "Omega Industries"), ("502", "DLTA", "Delta Company"),
               ("600", "ZETA", "Unrelated Name")]
# (security, id_type, value, source, valid_from)
IDENTIFIERS = [("S200", "CUSIP", "22222222B", "cusip master", "2021-01-01"),
               ("S200", "TICKER", "ACME", "tickers", "2021-01-01"),
               ("S300", "CUSIP", "33333333C", "cusip master", None),
               ("S300", "TICKER", "BRK.B", "tickers", "2000-01-01"),
               ("S500", "CUSIP", "55555555E", "cusip master", None),
               ("S500", "TICKER", "DLT", "tickers", None),
               ("S600", "CUSIP", "66666666F", CUSIP_FALLBACK_SOURCE, None),    # loader fallback: ignored
               ("S600", "TICKER", "ZETA", "tickers", None)]


def _warehouse():
    con = duckdb.connect(config={"threads": 1, "memory_limit": "256MB"})
    con.execute("""
        CREATE TABLE thirteenf_submissions (accession_number VARCHAR, filing_date DATE, submission_type VARCHAR,
            cik VARCHAR, period_of_report DATE, source_period VARCHAR);
        CREATE TABLE thirteenf_holdings (accession_number VARCHAR, cusip VARCHAR, name_of_issuer VARCHAR,
            share_quantity DOUBLE, share_quantity_type VARCHAR, put_call VARCHAR, source_period VARCHAR);
        CREATE TABLE sec_company_tickers (cik VARCHAR, ticker VARCHAR, title VARCHAR, security_id VARCHAR);
        CREATE TABLE security_identifier_history (security_id VARCHAR, id_type VARCHAR, id_value VARCHAR,
            source VARCHAR, valid_from DATE, valid_to DATE);
        CREATE TABLE security_identity_evidence (cik VARCHAR, fact_kind VARCHAR, native_key_namespace VARCHAR,
            native_key VARCHAR, value_json VARCHAR, valid_from DATE, valid_to DATE, evidence_status VARCHAR,
            is_latest_revision BOOLEAN);
        CREATE TABLE equity_daily_bars (security_id VARCHAR, symbol VARCHAR, trade_date DATE);
        CREATE TABLE finra_short_interest (symbol VARCHAR, settlement_date DATE);
    """)
    for (accession, period), rows in HOLDINGS.items():
        filed = dt.date.fromisoformat(period) + dt.timedelta(days=40)
        con.execute("INSERT INTO thirteenf_submissions VALUES (?, ?, '13F-HR', '9001', ?, 'p')",
                    [accession, filed, period])
        con.executemany("INSERT INTO thirteenf_holdings VALUES (?, ?, ?, 100, 'SH', '', 'p')",
                        [(accession, cusip, name) for cusip, name in rows])
    con.executemany("INSERT INTO sec_company_tickers VALUES (?, ?, ?, NULL)", SEC_TICKERS)
    con.executemany("INSERT INTO security_identifier_history VALUES (?, ?, ?, ?, ?, NULL)", IDENTIFIERS)
    # C9 former-name window of the delisted ACME (CIK 100): [2010-01-01, 2021-01-01)
    con.execute("INSERT INTO security_identity_evidence VALUES ('100', 'issuer_link', 'sec.issuer_name', 'ACME Corp', "
                "?, '2010-01-01', '2021-01-01', 'verified_dated', true)", [json.dumps({"issuer_name": "ACME Corp"})])
    con.executemany("INSERT INTO equity_daily_bars VALUES (?, ?, ?)",
                    [("S300", "BRK.B", "2021-06-10"), ("D1", "DUP", "2021-06-11"), ("D2", "DUP", "2021-06-11"),
                     ("OLD", "BRK.B", "2021-06-09")])
    con.executemany("INSERT INTO finra_short_interest VALUES (?, ?)",
                    [("BRK/B", "2021-06-15"), ("DUP", "2021-06-15"), ("NOPE", "2021-06-15")])
    return con


def test_cusip_owner_windows_unmapped_counts_and_si_symbol_date():
    con = _warehouse()
    assert stage_13f_cusip_periods(con) == 9
    counts = stage_cusip_owner_map(con)
    rows = {(cusip, str(period)): (owner, basis, confidence, reason) for cusip, period, owner, basis, confidence, reason
            in con.execute("SELECT cusip, report_period, owner_cik, identity_basis, confidence, reason "
                           "FROM _oi_cusip_owner").fetchall()}
    acme_old = (f"{100:010d}", "name_match", "high", "mapped")
    assert rows == {
        # delisted ACME: only inside the dated window and the periods it is held; never CIK 200
        ("11111111A", "2020-03-31"): acme_old,
        ("11111111A", "2020-06-30"): acme_old,
        ("11111111A", "2020-09-30"): acme_old,
        ("11111111A", "2021-03-31"): (None, None, None, "no_identity_evidence"),
        ("22222222B", "2021-06-30"): (f"{200:010d}", "cusip_ticker_current", "high", "mapped"),
        ("33333333C", "2021-06-30"): (f"{300:010d}", "cusip_ticker_current", "medium", "mapped"),  # BRK.B~BRK-B
        ("44444444D", "2021-06-30"): (None, None, None, "ambiguous_name"),
        ("55555555E", "2021-06-30"): (None, None, None, "basis_conflict"),
        ("66666666F", "2021-06-30"): (None, None, None, "no_identity_evidence"),                  # fallback seed
    }
    assert counts["mapped"] == {"cusip_ticker_current/high": 1, "cusip_ticker_current/medium": 1,
                                "name_match/high": 3}
    assert counts["unmapped"] == {"ambiguous_name": 1, "basis_conflict": 1, "no_identity_evidence": 2}
    assert counts["latest_period"] == "2021-06-30" and counts["dated_name_windows"] == 1

    assert stage_si_line_map(con) == {"ambiguous_symbol_line": 1, "mapped": 1, "no_price_line_for_symbol": 1}
    si = dict(con.execute("SELECT symbol, security_id FROM _oi_si_line").fetchall())
    assert si == {"BRK/B": "S300", "DUP": None, "NOPE": None}   # latest session in the lookback wins
