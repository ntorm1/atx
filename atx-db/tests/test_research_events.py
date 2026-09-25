"""P3 earnings-event dataset: session attribution, announcement basis, EAR / run-up hand
calculation, SUE at its own clock, and no event feature before event-session close + 1."""

from __future__ import annotations

import datetime as dt
import math

import duckdb
import pandas as pd
import pytest

from atx_db.derived_registry import DERIVED_SOURCE_NAME
from atx_db.research.events import EarningsEventOptions, build_earnings_events, validate_event_version
from atx_db.research.store import ResearchStore

HOLIDAY = dt.date(2023, 7, 4)
SESSIONS = [d.date() for d in pd.bdate_range("2022-11-01", "2023-12-29") if d.date() != HOLIDAY]
INDEX = {day: i for i, day in enumerate(SESSIONS)}
LINES = {"LA": 1, "LB": 2, "LC": 3, "M1": None, "M2": None}  # line -> owner CIK (None: unlinked)
FORMATIONS = sorted({max(d for d in SESSIONS if (d.year, d.month) == ym)
                     for ym in {(d.year, d.month) for d in SESSIONS}})[:-1]
CLOSE = dt.time(22)


def _price(line, i):
    k = list(LINES).index(line)
    return (20.0 + 7 * k) * math.exp(0.0005 * (k + 1) * i + 0.03 * math.sin(i / 2.3 + k))


def _ret(line, day):
    i = INDEX[day]
    return _price(line, i) / _price(line, i - 1) - 1


def _car(line, event, lo, hi):
    """Hand calculation: sum of (line return - equal-weighted return of all five lines)."""
    days = [SESSIONS[INDEX[event] + k] for k in range(lo, hi + 1)]
    return sum(_ret(line, d) - sum(_ret(x, d) for x in LINES) / len(LINES) for d in days)


def _next_close(day):
    return dt.datetime.combine(SESSIONS[INDEX[day] + 1], CLOSE)


def _cik(owner):
    return f"{owner:010d}"


# (owner, period_end, type, form, accession, filing date, acceptance UTC or None)
PERIODS = [
    (1, "2023-03-31", "quarter", "10-Q", "a-q1", "2023-05-05", "2023-05-05 14:00"),
    (1, "2023-06-30", "quarter", "10-Q", "a-q2", "2023-08-01", "2023-08-01 14:00"),
    (1, "2022-06-30", "quarter", "10-Q", "a-q2", "2023-08-01", "2023-08-01 14:00"),  # comparative only
    (2, "2022-12-31", "annual", "10-K", "b-k22", "2023-02-20", "2023-02-20 15:00"),
    (2, "2023-03-31", "quarter", "10-Q", "b-q1", "2023-05-08", "2023-05-08 15:00"),
    (2, "2023-06-30", "quarter", "10-Q", "b-q2", "2023-08-03", "2023-08-03 12:00"),   # 08:00 EDT pre-open
    (3, "2023-03-31", "quarter", "10-Q", "c-q1", "2023-05-31", "2023-05-31 14:00"),   # 10:00 EDT intraday
]
# (owner, accession, report date, filing date, acceptance UTC, raw stamp)
EIGHT_KS = [
    (1, "a-8k-q1", "2023-04-25", "2023-04-25", "2023-04-25 20:05", "2023-04-25T20:05:00.000Z"),  # 16:05 EDT
    (1, "a-8k-q2", "2023-07-04", "2023-07-05", "2023-07-04 13:00", "2023-07-04T13:00:00.000Z"),  # holiday
    (2, "b-8k-q4", "2023-02-02", "2023-02-02", "2023-02-02 20:30", "2023-02-02T20:30:00.000Z"),  # 15:30 EST
    (2, "b-8k-q1", "2023-04-20", "2023-04-20", "2023-04-20 11:30", None),                        # 07:30 EDT
    (2, "b-8k-q2", "2023-08-09", "2023-08-09", "2023-08-09 20:10", "2023-08-09T20:10:00.000Z"),  # after 10-Q
]


@pytest.fixture
def store(tmp_path):
    warehouse = tmp_path / "warehouse.duckdb"
    con = duckdb.connect(str(warehouse), config={"threads": 1, "memory_limit": "256MB"})
    con.execute("""
        CREATE TABLE trading_calendar (calendar_id VARCHAR, trade_date DATE, is_open BOOLEAN, source VARCHAR);
        CREATE TABLE equity_daily_bars (source VARCHAR, security_id VARCHAR, symbol VARCHAR, trade_date DATE,
            close DOUBLE, adjusted_close DOUBLE, available_at TIMESTAMP, vendor_security_id VARCHAR,
            source_loaded_at TIMESTAMP);
        CREATE TABLE fundamental_periods (security_id VARCHAR, cik VARCHAR, period_end DATE,
            normalized_period_type VARCHAR, form VARCHAR, accession_number VARCHAR, fdate DATE, as_of_date DATE,
            available_at TIMESTAMP, rdq DATE, statement_point_count INTEGER);
        CREATE TABLE sec_submissions (security_id VARCHAR, cik VARCHAR, accession_number VARCHAR, filing_date DATE,
            report_date DATE, acceptance_datetime TIMESTAMP, acceptance_datetime_raw VARCHAR, form VARCHAR,
            items VARCHAR, source_url VARCHAR, source_loaded_at TIMESTAMP);
        CREATE TABLE derived_metric_values (derived_value_id VARCHAR, source VARCHAR, security_id VARCHAR,
            metric_code VARCHAR, metric_window VARCHAR, period_end DATE, value DOUBLE, available_at TIMESTAMP,
            source_loaded_at TIMESTAMP);
    """)
    con.executemany("INSERT INTO trading_calendar VALUES ('XNYS', ?, true, 'equity_daily_bars calendar')",
                    [(d,) for d in SESSIONS])
    con.executemany("INSERT INTO equity_daily_bars VALUES ('prices', ?, ?, ?, ?, ?, ?, 'v', '2024-01-01')",
                    [(line, line, d, 3 * _price(line, i), _price(line, i), dt.datetime.combine(d, CLOSE))
                     for line in LINES for i, d in enumerate(SESSIONS)])
    for owner, end, kind, form, accession, filed, accepted in PERIODS:
        sid = f"SEC-CIK-{_cik(owner)}"
        con.execute("INSERT INTO fundamental_periods VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, NULL, 40)",
                    [sid, str(owner), end, kind, form, accession, filed, filed,
                     dt.datetime.fromisoformat(filed) + dt.timedelta(hours=46)])
        con.execute("INSERT INTO sec_submissions VALUES (?, ?, ?, ?, ?, ?, ?, ?, '', 'u', '2024-01-01')",
                    [sid, _cik(owner), accession, filed, end, accepted, accepted.replace(" ", "T") + ":00.000Z",
                     form])
    for owner, accession, report, filed, accepted, raw in EIGHT_KS:
        con.execute("INSERT INTO sec_submissions VALUES (?, ?, ?, ?, ?, ?, ?, '8-K', '2.02,9.01', 'u', '2024-01-01')",
                    [f"SEC-CIK-{_cik(owner)}", _cik(owner), accession, filed, report, accepted, raw])
    for did, owner, end, value, available in (("sue-a1", 1, "2023-03-31", 1.5, "2023-05-07 22:00"),
                                              ("sue-a1r", 1, "2023-03-31", 1.7, "2023-09-01 00:00"),
                                              ("sue-b1", 2, "2023-03-31", -0.8, "2023-05-10 22:00")):
        con.execute("INSERT INTO derived_metric_values VALUES (?, ?, ?, 'sue_ni', 'q', ?, ?, ?, '2024-01-01')",
                    [did, DERIVED_SOURCE_NAME, f"SEC-CIK-{_cik(owner)}", end, value, available])
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
                '{}', 's', '2022-11-01', '2023-11-01', '2023-12-29', now(), 'w', '[]', 'panel-seal', now())
    """)
    for f in FORMATIONS:
        rc.execute("INSERT INTO research_panel_calendar (run_id, month_start, expected_session, formation_date, "
                   "cutoff, status) VALUES ('run1', ?, ?, ?, ?, 'formed')",
                   [f.replace(day=1), f, f, dt.datetime.combine(f, CLOSE)])
        for line, owner in LINES.items():
            rc.execute("INSERT INTO research_panel_cohort (run_id, formation_date, security_id, owner_cik, "
                       "identity_basis, cohort_reason, eligible, primary_line) VALUES ('run1', ?, ?, ?, 'r', ?, "
                       "true, ?)", [f, line, _cik(owner) if owner else None,
                                    "valid" if owner else "missing_owner_link", True if owner else None])
    try:
        yield research
    finally:
        research.close()


def test_event_dataset_sessions_basis_returns_and_pit_clocks(store):
    result = build_earnings_events(store, EarningsEventOptions(panel_run_id="run1"))
    assert result.status == "sealed" and result.events == 6 and not result.reused
    assert result.feature_rows == len(FORMATIONS) * 3 * 4                       # dense: lines x features
    periods = result.diagnostic["periods"]
    assert periods["excluded"] == {"comparative_or_late_period": 1, "duplicate_period_end_same_filing": 0}
    ann = result.diagnostic["announcements"]
    assert ann["basis"] == {"8k_202": 4, "periodic_filing": 2}
    assert ann["rdq_rejection"] == {"none": 4, "no_8k_202_in_window": 1, "rdq_after_filing_date": 1}
    assert "periodic_filing_basis_late_proxy:2/6" in result.blockers
    assert "acceptance_zone_normalized_unqualified:1/6" in result.blockers

    ev = {(r[0], str(r[1])): r[2:] for r in store.con.execute("""
        SELECT owner_cik, fiscal_period_end, announcement_basis, announcement_accession, session_timing,
               event_session, event_available_at, ear_m1p1, ear_available_at, runup_m21_m2, sue, sue_available_at
        FROM research_earnings_events WHERE event_version=?""", [result.event_version]).fetchall()}
    d = dt.date.fromisoformat
    expected = {  # AMC 16:05 EDT -> next; BMO 07:30 -> same; 15:30 EST (winter) -> same; holiday -> next
        (_cik(1), "2023-03-31"): ("8k_202", "a-8k-q1", "after_close", d("2023-04-26")),
        (_cik(1), "2023-06-30"): ("8k_202", "a-8k-q2", "non_session_day", d("2023-07-05")),
        (_cik(2), "2022-12-31"): ("8k_202", "b-8k-q4", "intraday", d("2023-02-02")),
        (_cik(2), "2023-03-31"): ("8k_202", "b-8k-q1", "pre_open", d("2023-04-20")),
        (_cik(2), "2023-06-30"): ("periodic_filing", "b-q2", "pre_open", d("2023-08-03")),   # 8-K after 10-Q
        (_cik(3), "2023-03-31"): ("periodic_filing", "c-q1", "intraday", d("2023-05-31")),   # no 8-K 2.02
    }
    assert {key: row[:4] for key, row in ev.items()} == expected
    for key, (_, _, _, session) in expected.items():
        assert ev[key][4] == _next_close(session)                     # event visible at close of E+1
    line = {_cik(1): "LA", _cik(2): "LB", _cik(3): "LC"}
    for (owner, end), row in ev.items():
        session = expected[(owner, end)][3]
        assert row[5] == pytest.approx(_car(line[owner], session, -1, 1), abs=1e-12)
        assert row[6] == _next_close(session)
        assert row[7] == pytest.approx(_car(line[owner], session, -21, -2), abs=1e-12)
    a_q1, b_q1 = ev[(_cik(1), "2023-03-31")], ev[(_cik(2), "2023-03-31")]
    assert (a_q1[8], a_q1[9]) == (1.5, dt.datetime(2023, 5, 7, 22))            # SUE at its own clock
    assert (b_q1[8], b_q1[9]) == (-0.8, dt.datetime(2023, 5, 10, 22))

    feats = {(r[0], r[1], r[2]): r[3:] for r in store.con.execute("""
        SELECT formation_date, security_id, feature_id, raw_value, reason, available_at, event_session
        FROM research_event_features WHERE event_version=?""", [result.event_version]).fetchall()}
    apr, may, jun, sep = d("2023-04-28"), d("2023-05-31"), d("2023-06-30"), d("2023-09-29")
    assert feats[(apr, "LA", "ear_m1p1")][0] == pytest.approx(a_q1[5], abs=1e-15)
    assert feats[(apr, "LA", "days_since_announcement")][0] == 2.0
    assert feats[(apr, "LA", "sue")][:2] == (None, "no_recent_visible_sue")    # SUE clock 05-07 not reached
    assert feats[(may, "LA", "sue")][0] == 1.5
    assert feats[(sep, "LA", "sue")][0] == 1.7                                 # newest visible revision
    assert feats[(may, "LC", "ear_m1p1")][:2] == (None, "no_recent_event")     # E = formation day: not yet
    assert feats[(jun, "LC", "ear_m1p1")][0] == pytest.approx(ev[(_cik(3), "2023-03-31")][5], abs=1e-15)
    for (formation, _, _), (_value, _, available, session) in feats.items():
        if available is not None:                                              # PIT: every visible value
            assert available <= dt.datetime.combine(formation, CLOSE)
            assert available >= _next_close(session) and session < formation

    assert validate_event_version(store, result.event_version)["event_grain"] == 0
    again = build_earnings_events(store, EarningsEventOptions(panel_run_id="run1"))
    assert again.reused and again.event_version == result.event_version
