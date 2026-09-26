"""P9 ownership features: 13F IO / dIO / breadth at the quarter's deadline clock (IO > 1 kept and
flagged, put/call rows excluded), the verified-shares gate, and FINRA short interest visible only
from its publication clock (never at settlement)."""

from __future__ import annotations

import datetime as dt

import duckdb
import pandas as pd
import pytest

from atx_db.market_daily import MARKET_DAILY_SOURCE_NAME
from atx_db.research.ownership_features import (
    OwnershipFeatureOptions,
    build_ownership_features,
    validate_ownership_version,
)
from atx_db.research.store import ResearchStore

SESSIONS = [d.date() for d in pd.bdate_range("2020-01-01", "2020-12-31")]
FORMATIONS = [max(d for d in SESSIONS if d.month == m) for m in range(5, 10)]      # May..Sep month ends
CLOSE = dt.time(22)
LINES = {"LA": ("1", "ALPHA INC", "dei", 100.0), "LB": ("2", "BETA INC", "xbrl", 200.0)}
# 13F: (accession, manager, period, filed, [(cusip, shares, put_call)])
FILINGS = [
    ("q1-m1", "9001", "2020-03-31", "2020-05-10", [("CUSIPLA01", 60, ""), ("CUSIPLB01", 5, "")]),
    ("q1-m2", "9002", "2020-03-31", "2020-05-10", [("CUSIPLA01", 30, "")]),
    ("q1-m3", "9003", "2020-03-31", "2020-05-10", [("CUSIPLB01", 10, ""), ("NOBODY001", 7, "")]),
    ("q2-m1", "9001", "2020-06-30", "2020-08-10", [("CUSIPLA01", 70, "")]),
    ("q2-m2", "9002", "2020-06-30", "2020-08-10", [("CUSIPLA01", 50, "")]),
    ("q2-m3", "9003", "2020-06-30", "2020-08-10", [("CUSIPLA01", 5, ""), ("CUSIPLA01", 1000, "PUT"),
                                                     ("CUSIPLB01", 20, ""), ("NOBODY001", 7, "")]),
]
# FINRA: (symbol, settlement, short interest, ADV)
SHORTS = [("LA", "2020-07-15", 10, 5), ("LA", "2020-07-31", 20, 4), ("LA", "2020-08-25", 30, 10),
          ("LB", "2020-07-15", 50, 0)]


def _store(tmp_path):
    warehouse = tmp_path / "warehouse.duckdb"
    con = duckdb.connect(str(warehouse), config={"threads": 1, "memory_limit": "256MB"})
    con.execute("""
        CREATE TABLE trading_calendar (calendar_id VARCHAR, trade_date DATE, is_open BOOLEAN, source VARCHAR);
        CREATE TABLE equity_daily_bars (security_id VARCHAR, symbol VARCHAR, trade_date DATE);
        CREATE TABLE market_daily_metrics (market_daily_id VARCHAR, security_id VARCHAR, trade_date DATE,
            as_of_date DATE, available_at TIMESTAMP, source VARCHAR, shares_outstanding DOUBLE, shares_source VARCHAR);
        CREATE TABLE sec_company_tickers (cik VARCHAR, ticker VARCHAR, title VARCHAR, security_id VARCHAR);
        CREATE TABLE security_identifier_history (security_id VARCHAR, id_type VARCHAR, id_value VARCHAR,
            source VARCHAR, valid_from DATE, valid_to DATE);
        CREATE TABLE thirteenf_submissions (accession_number VARCHAR, filing_date DATE, submission_type VARCHAR,
            cik VARCHAR, period_of_report DATE, source_period VARCHAR);
        CREATE TABLE thirteenf_holdings (accession_number VARCHAR, cusip VARCHAR, name_of_issuer VARCHAR,
            share_quantity DOUBLE, share_quantity_type VARCHAR, put_call VARCHAR, source_period VARCHAR);
        CREATE TABLE finra_short_interest (symbol VARCHAR, settlement_date DATE, market_class_code VARCHAR,
            current_short_position_quantity BIGINT, average_daily_volume_quantity BIGINT, available_at TIMESTAMP);
    """)
    con.executemany("INSERT INTO trading_calendar VALUES ('XNYS', ?, true, 'equity_daily_bars calendar')",
                    [(d,) for d in SESSIONS])
    for line, (cik, name, shares_source, shares) in LINES.items():
        con.executemany("INSERT INTO equity_daily_bars VALUES (?, ?, ?)", [(line, line, d) for d in SESSIONS])
        con.executemany("INSERT INTO market_daily_metrics VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                        [(f"{line}-{d}", line, d, d, dt.datetime.combine(d, CLOSE), MARKET_DAILY_SOURCE_NAME, shares,
                          shares_source) for d in SESSIONS])
        con.execute("INSERT INTO sec_company_tickers VALUES (?, ?, ?, NULL)", [cik, line, name.title()])
        con.executemany("INSERT INTO security_identifier_history VALUES (?, ?, ?, 'master', NULL, NULL)",
                        [(line, "CUSIP", f"CUSIP{line}01"), (line, "TICKER", line)])
    names = {"CUSIPLA01": "ALPHA INC", "CUSIPLB01": "BETA INC", "NOBODY001": "NOBODY CORP"}
    for accession, manager, period, filed, rows in FILINGS:
        con.execute("INSERT INTO thirteenf_submissions VALUES (?, ?, '13F-HR', ?, ?, 'p')",
                    [accession, filed, manager, period])
        con.executemany("INSERT INTO thirteenf_holdings VALUES (?, ?, ?, ?, 'SH', ?, 'p')",
                        [(accession, cusip, names[cusip], n, put) for cusip, n, put in rows])
    con.executemany("INSERT INTO finra_short_interest VALUES (?, ?, 'N', ?, ?, ?)",
                    [(symbol, settle, si, adv, dt.datetime.fromisoformat(settle) + dt.timedelta(days=10, hours=22))
                     for symbol, settle, si, adv in SHORTS])
    con.close()
    research = ResearchStore(tmp_path / "research.duckdb", warehouse_path=warehouse)
    research.open()
    rc = research.con
    rc.execute("""
        INSERT INTO research_panel_runs (run_id, status, basis, universe_id, identity_basis, universe_basis,
            fundamental_availability_basis, market_availability_basis, query_version, spec_json, spec_sha256,
            definitions_json, definitions_sha256, code_sha256, source_ids_json, calendar_sha256, start_month,
            end_month, as_of_date, run_at, warehouse_path, blockers_json, panel_sha256, created_at)
        VALUES ('run1', 'complete', 'reconstructed', 'u', 'i', 'u', 'f', 'm', 'q', '{}', 's', '{}', 's', 's',
                '{}', 's', '2020-05-01', '2020-09-01', '2020-12-31', now(), 'w', '[]', 'panel-seal', now())
    """)
    for f in FORMATIONS:
        rc.execute("INSERT INTO research_panel_calendar (run_id, month_start, expected_session, formation_date, "
                   "cutoff, status) VALUES ('run1', ?, ?, ?, ?, 'formed')",
                   [f.replace(day=1), f, f, dt.datetime.combine(f, CLOSE)])
        for line, (cik, *_rest) in LINES.items():
            rc.execute("INSERT INTO research_panel_cohort (run_id, formation_date, security_id, owner_cik, "
                       "identity_basis, cohort_reason, eligible, primary_line) VALUES ('run1', ?, ?, ?, 'r', "
                       "'valid', true, true)", [f, line, f"{int(cik):010d}"])
    return research


@pytest.fixture
def store(tmp_path):
    research = _store(tmp_path)
    try:
        yield research
    finally:
        research.close()


def test_ownership_features_clocks_io_flag_and_verified_shares(store):
    result = build_ownership_features(store, OwnershipFeatureOptions(panel_run_id="run1"))
    assert result.status == "sealed" and not result.reused
    assert result.feature_rows == len(FORMATIONS) * 2 * 5                          # dense: lines x features
    assert "thirteenf_unmapped_cusip_periods:2/6" in result.blockers                # NOBODY CORP, both quarters
    feats = {(str(r[0]), r[1], r[2]): r[3:] for r in store.con.execute("""
        SELECT formation_date, security_id, feature_id, raw_value, reason, available_at, source_period, value_flag
        FROM research_ownership_features WHERE ownership_version=?""", [result.ownership_version]).fetchall()}
    may, jun, jul, aug, sep = (str(f) for f in FORMATIONS)
    d, ts = dt.date.fromisoformat, dt.datetime.fromisoformat

    # 13F: nothing before Q1's deadline clock (05-15 + 46 h); Q2 only from 08-15 22:00 (08-14 + 46 h)
    assert feats[(may, "LA", "io_ratio_13f")][:5] == (0.9, "valid", ts("2020-05-16 22:00"), d("2020-03-31"), None)
    assert feats[(jul, "LA", "io_ratio_13f")][3] == d("2020-03-31")
    io = feats[(aug, "LA", "io_ratio_13f")]
    assert io[:5] == (pytest.approx(1.25), "valid", ts("2020-08-15 22:00"), d("2020-06-30"), "io_above_one")
    assert feats[(aug, "LA", "io_change_13f")][:2] == (pytest.approx(0.35), "valid")
    assert feats[(may, "LA", "io_change_13f")][:2] == (None, "previous_quarter_no_mapped_13f_holding")
    assert feats[(aug, "LA", "breadth_change_13f")][:2] == (pytest.approx(1 / 3), "valid")
    assert feats[(aug, "LB", "breadth_change_13f")][:2] == (pytest.approx(-1 / 3), "valid")
    # verified-shares gate: LB's share count is not DEI -> NULL with a reason, never a value
    assert feats[(aug, "LB", "io_ratio_13f")][:3] == (None, "unverified_shares", None)
    assert feats[(jul, "LB", "short_interest_ratio")][:2] == (None, "unverified_shares")

    # FINRA: 07-31 settlement publishes 08-12 22:00, so the 07-31 formation still sees 07-15 (published 07-27)
    assert feats[(jun, "LA", "short_interest_ratio")][:2] == (None, "no_recent_short_interest")
    assert feats[(jul, "LA", "short_interest_ratio")][:4] == (0.1, "valid", ts("2020-07-27 22:00"), d("2020-07-15"))
    assert feats[(aug, "LA", "short_interest_ratio")][:4] == (0.2, "valid", ts("2020-08-12 22:00"), d("2020-07-31"))
    assert feats[(aug, "LA", "days_to_cover_si")][:2] == (5.0, "valid")            # 08-25 publishes 09-04
    assert feats[(sep, "LA", "days_to_cover_si")][:2] == (3.0, "valid")
    assert feats[(jul, "LB", "days_to_cover_si")][:2] == (None, "missing_adv")
    for (formation, _, _), (_value, _, available, _, _) in feats.items():
        if available is not None:
            assert available <= dt.datetime.combine(d(formation), CLOSE)

    assert validate_ownership_version(store, result.ownership_version)["short_interest_before_publication"] == 0
    again = build_ownership_features(store, OwnershipFeatureOptions(panel_run_id="run1"))
    assert again.reused and again.ownership_version == result.ownership_version
