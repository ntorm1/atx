"""P9 identity: a delisted issuer's 13F CUSIP maps only inside its dated name window and its
holding periods -- one stale filer in the latest quarter neither makes it current nor remaps its
history to today's same-named registrant; only eligible common-equity rows with valid CUSIPs name a
CUSIP-period (preferred, placeholder bucket, bad check digit and option rows are excluded and
counted); dated tickers only, ties ambiguous; unmapped rows counted by reason; FINRA symbol-date map."""

from __future__ import annotations

import datetime as dt
import json

import duckdb

from atx_db.ownership_identity import stage_13f_cusip_periods, stage_cusip_owner_map, stage_si_line_map
from atx_db.thirteenf import CUSIP_FALLBACK_SOURCE


def _cusip(base8):
    """Append the CUSIP mod-10 check digit."""
    total = 0
    for i, ch in enumerate(base8, 1):
        value = int(ch) if ch.isdigit() else ord(ch) - 55
        value *= 2 if i % 2 == 0 else 1
        total += value // 10 + value % 10
    return base8 + str((10 - total % 10) % 10)


ACME_OLD, ACME_NEW, BRK, GAMMA = _cusip("11111111"), _cusip("22222222"), _cusip("33333333"), _cusip("44444444")
OMEGA, ZETA, PREF, MIXED = _cusip("55555555"), _cusip("66666666"), _cusip("77777777"), _cusip("88888888")
BAD = _cusip("12345678")[:8] + str((int(_cusip("12345678")[8]) + 1) % 10)       # wrong check digit
Q2 = "2021-06-30"
# (manager, period, cusip, issuer name, title, put_call)
HOLDINGS = [
    ("9001", "2020-03-31", ACME_OLD, "ACME CORP /DE/", "COM", ""),
    ("9001", "2020-06-30", ACME_OLD, "ACME CORP", "COM", ""),
    ("9001", "2020-09-30", ACME_OLD, "ACME CORP /DE/", "COM", ""),
    ("9001", "2021-03-31", ACME_OLD, "ACME CORP", "COM", ""),              # after the name window
    ("9002", Q2, ACME_OLD, "ACME CORP", "COM", ""),                         # one stale filer (rv-p9 probe 4.1)
    *[(m, Q2, ACME_NEW, "ACME CORP", "COM", "") for m in ("9001", "9002", "9003")],
    *[(m, Q2, BRK, "BERKSHIRE HATHAWAY INC DEL", "CL B NEW", "") for m in ("9001", "9002", "9003")],
    ("9001", Q2, BRK, "BERKSHIRE HATHAWAY INC DEL", "CL B NEW", "PUT"),     # option: excluded
    *[(m, Q2, GAMMA, "GAMMA INC", "COM", "") for m in ("9001", "9002", "9003")],
    *[(m, Q2, OMEGA, "OMEGA INDUSTRIES", "COMMON STOCK", "") for m in ("9001", "9002", "9003")],
    *[(m, Q2, ZETA, "ZETA WIDGETS", "COM", "") for m in ("9001", "9002", "9003")],
    ("9001", Q2, PREF, "ACME CORP", "PFD SER A", ""),                       # same-name preferred (probe 4.2)
    ("9001", Q2, "000000000", "DELTA CO", "COM", ""),                       # placeholder bucket (probe 4.3)
    ("9002", Q2, "000000000", "DELTA CO", "COM", ""),
    ("9003", Q2, "000000000", "ZETA WIDGETS", "COM", ""),
    ("9001", Q2, BAD, "DELTA CO", "COM", ""),
    *[(m, Q2, MIXED, "DELTA CO", "COM", "") for m in ("9001", "9002", "9003")],
    ("9004", Q2, MIXED, "SOMETHING ELSE", "COM", ""),                       # 3/4 < 80%: name not dominant
]
# (cik, ticker, title)
SEC_TICKERS = [("200", "ACME", "Acme Corp"), ("300", "BRK-B", "Berkshire Hathaway Inc"),
               ("401", "GAM", "Gamma Inc"), ("402", "GAMC", "GAMMA CORP"),
               ("501", "DLT", "Omega Industries"), ("502", "DLTA", "Delta Company"),
               ("600", "ZETA", "Unrelated Name")]
# (security, id_type, value, source, valid_from)
IDENTIFIERS = [("S200", "CUSIP", ACME_NEW, "cusip master", "2021-01-01"),
               ("S200", "TICKER", "ACME", "tickers", "2021-01-01"),
               ("S300", "CUSIP", BRK, "cusip master", None),
               ("S300", "TICKER", "BRK.B", "tickers", "2000-01-01"),
               ("S300", "TICKER", "BRKZ", "snapshot", None),                  # undated: never a ticker hop
               ("S500", "CUSIP", OMEGA, "cusip master", None),
               ("S500", "TICKER", "DLT", "tickers", "2020-01-01"),           # two dated tickers valid at the
               ("S500", "TICKER", "DLTX", "tickers", "2021-01-01"),          # quarter end: ambiguous
               ("S600", "CUSIP", ZETA, CUSIP_FALLBACK_SOURCE, None),         # loader fallback: ignored
               ("S600", "TICKER", "ZETA", "tickers", "2000-01-01")]


def _warehouse():
    con = duckdb.connect(config={"threads": 1, "memory_limit": "256MB"})
    con.execute("""
        CREATE TABLE thirteenf_submissions (accession_number VARCHAR, filing_date DATE, submission_type VARCHAR,
            cik VARCHAR, period_of_report DATE, source_period VARCHAR);
        CREATE TABLE thirteenf_holdings (accession_number VARCHAR, cusip VARCHAR, name_of_issuer VARCHAR,
            share_quantity DOUBLE, share_quantity_type VARCHAR, put_call VARCHAR, title_of_class VARCHAR,
            source_period VARCHAR);
        CREATE TABLE sec_company_tickers (cik VARCHAR, ticker VARCHAR, title VARCHAR, security_id VARCHAR);
        CREATE TABLE security_identifier_history (security_id VARCHAR, id_type VARCHAR, id_value VARCHAR,
            source VARCHAR, valid_from DATE, valid_to DATE);
        CREATE TABLE security_identity_evidence (cik VARCHAR, fact_kind VARCHAR, native_key_namespace VARCHAR,
            native_key VARCHAR, value_json VARCHAR, valid_from DATE, valid_to DATE, evidence_status VARCHAR,
            is_latest_revision BOOLEAN);
        CREATE TABLE equity_daily_bars (security_id VARCHAR, symbol VARCHAR, trade_date DATE);
        CREATE TABLE finra_short_interest (symbol VARCHAR, settlement_date DATE);
    """)
    filings = sorted({(manager, period) for manager, period, *_ in HOLDINGS})
    con.executemany("INSERT INTO thirteenf_submissions VALUES (?, ?, '13F-HR', ?, ?, 'p')",
                    [(f"{m}-{p}", dt.date.fromisoformat(p) + dt.timedelta(days=40), m, p) for m, p in filings])
    con.executemany("INSERT INTO thirteenf_holdings VALUES (?, ?, ?, 100, 'SH', ?, ?, 'p')",
                    [(f"{m}-{p}", cusip, name, put, title) for m, p, cusip, name, title, put in HOLDINGS])
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


def test_cusip_owner_windows_stale_rows_common_equity_and_unmapped_counts():
    con = _warehouse()
    periods = stage_13f_cusip_periods(con)
    assert periods["cusip_periods"] == 11
    assert periods["excluded_rows"] == {"cusip_invalid_check_digit": 1, "cusip_placeholder": 3,
                                        "not_sh_or_option": 1, "title_not_common": 1}
    counts = stage_cusip_owner_map(con)
    rows = {(cusip, str(period)): (owner, basis, confidence, reason, conditioning)
            for cusip, period, owner, basis, confidence, reason, conditioning in con.execute(
                "SELECT cusip, report_period, owner_cik, identity_basis, confidence, reason, sample_conditioning "
                "FROM _oi_cusip_owner").fetchall()}
    unmapped = (None, None, None)
    old = (f"{100:010d}", "name_match", "high", "mapped", "dated_name_window")
    survivor = "cusip_survivor_conditioned"
    assert rows == {
        # delisted ACME: only inside the dated window; the stale 2021Q2 filer changes nothing (never CIK 200)
        (ACME_OLD, "2020-03-31"): old, (ACME_OLD, "2020-06-30"): old, (ACME_OLD, "2020-09-30"): old,
        (ACME_OLD, "2021-03-31"): (*unmapped, "no_identity_evidence", None),
        (ACME_OLD, Q2): (*unmapped, "no_identity_evidence", None),
        (ACME_NEW, Q2): (f"{200:010d}", "cusip_ticker_current", "high", "mapped", survivor),
        (BRK, Q2): (f"{300:010d}", "cusip_ticker_current", "high", "mapped", survivor),    # BRK.B~BRK-B, DEL
        (GAMMA, Q2): (*unmapped, "ambiguous_name", None),
        (OMEGA, Q2): (*unmapped, "ambiguous_ticker", None),                                # never max(ticker)
        (ZETA, Q2): (*unmapped, "no_identity_evidence", None),                             # fallback seed
        (MIXED, Q2): (*unmapped, "name_not_dominant", None),
    }
    assert counts["current_cusips"] == 4 and counts["latest_period"] == Q2
    assert counts["mapped"] == {"cusip_ticker_current/high": 2, "name_match/high": 3}
    assert counts["unmapped"] == {"ambiguous_name": 1, "ambiguous_ticker": 1, "name_not_dominant": 1,
                                  "no_identity_evidence": 3}
    assert counts["sample_conditioning"] == {survivor: 2, "dated_name_window": 3}

    assert stage_si_line_map(con) == {"ambiguous_symbol_line": 1, "mapped": 1, "no_price_line_for_symbol": 1}
    si = dict(con.execute("SELECT symbol, security_id FROM _oi_si_line").fetchall())
    assert si == {"BRK/B": "S300", "DUP": None, "NOPE": None}   # latest session in the lookback wins
