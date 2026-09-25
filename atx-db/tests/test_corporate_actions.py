"""P8: corporate-action events from the vendor factor, classified by R1d's corroborated split epochs."""

from __future__ import annotations

import datetime as dt
import json

import duckdb
import pytest

from atx_db.corporate_actions import TIE_OUT_TOLERANCE, build_corporate_action_events

EX, LATE_SHARES, DIVIDEND = dt.date(2021, 3, 15), dt.date(2021, 6, 23), dt.date(2021, 6, 15)
SPECIAL_CUT = 0.2513  # 25.13 % of the price: an inexact ratio (an exact 25 %, k = 4/3, is a 4:3 split candidate)


def _at(day: dt.date, hour: int = 22) -> dt.datetime:
    return dt.datetime.combine(day, dt.time(hour))


def _bars(con, security_id, *, split=False, special=False, dividend=False, shares_from=EX):
    """Weekday bars; the vendor factor (adjusted / raw close) back-adjusts every event, times 0.95 of later dividends."""
    day = dt.date(2021, 1, 1)
    rows = []
    while day <= dt.date(2022, 6, 30):
        if day.weekday() < 5:
            before = day < EX
            close = 10.0 * (2.0 if split and before else 1.0) * (1.0 - SPECIAL_CUT if special and not before else 1.0)
            factor = 0.95 * (0.5 if split and before else 1.0) * (1.0 - SPECIAL_CUT if special and before else 1.0)
            factor *= 0.99 if dividend and day < DIVIDEND else 1.0
            shares = 2000 if split and day >= shares_from else 1000
            rows.append(("vendor", security_id, day, close, close * factor, shares, _at(day), "r1"))
        day += dt.timedelta(days=1)
    con.executemany("INSERT INTO equity_daily_bars VALUES (?, ?, ?, ?, ?, ?, ?, ?)", rows)


@pytest.fixture
def events():
    con = duckdb.connect(":memory:")
    con.execute("SET memory_limit='256MB'")
    con.execute("SET threads=1")
    con.execute("""CREATE TABLE equity_daily_bars (source VARCHAR, security_id VARCHAR, trade_date DATE, close DOUBLE,
                   adjusted_close DOUBLE, shares_outstanding BIGINT, available_at TIMESTAMP, run_id VARCHAR)""")
    con.execute("""CREATE TABLE sec_company_facts (security_id VARCHAR, concept VARCHAR, unit VARCHAR,
                   period_start DATE, period_end DATE, value DOUBLE, available_at TIMESTAMP)""")
    _bars(con, "SPLIT", split=True, dividend=True)  # 2:1 on EX, count follows; a 1 % dividend on DIVIDEND
    _bars(con, "SPECIAL", special=True)  # a 25.13 % special distribution, count flat
    _bars(con, "FLAT")  # a flat vendor factor
    _bars(con, "LATE", split=True, shares_from=LATE_SHARES)  # 2:1 whose count follows 100 days late
    con.execute("INSERT INTO sec_company_facts VALUES ('SPLIT', 'CommonStockDividendsPerShareDeclared', "
                "'USD/shares', DATE '2021-04-01', DATE '2021-06-30', 0.10, TIMESTAMP '2021-08-05 16:00:00')")
    summary = build_corporate_action_events(con, "ca", run_id="t")
    rows = {(r[0], r[1]): r for r in con.execute(
        "SELECT security_id, action_type, ex_date, cash_amount, split_from, split_to, adjustment_factor, "
        "details_json, available_at FROM ca").fetchall()}
    yield con, summary, rows
    con.close()


def test_events_are_labelled_split_or_distribution_by_their_evidence_and_tie_out(events):
    con, summary, rows = events
    assert set(rows) == {("SPLIT", "split"), ("SPLIT", "cash_dividend"), ("SPECIAL", "distribution_unclassified"),
                         ("LATE", "split")}
    # 2:1 with share corroboration: a split of 2.0, known at the ex-date bar.
    _, _, ex, cash, split_from, split_to, factor, details, available = rows[("SPLIT", "split")]
    assert (ex, cash, split_from, split_to, factor, available) == (EX, None, 1.0, 2.0, pytest.approx(0.5), _at(EX))
    assert json.loads(details)["corroboration"] == "share_count"
    # A 25 % special with a flat count is a distribution, never a split.
    _, _, ex, cash, split_from, _, factor, details, _ = rows[("SPECIAL", "distribution_unclassified")]
    assert (ex, cash, split_from) == (EX, pytest.approx(10.0 * SPECIAL_CUT), None)
    assert (json.loads(details)["reason"], json.loads(details)["evidence_basis"]) == ("special_or_spinoff",
                                                                                     "vendor_factor")
    # A 1 % cash residual, corroborated by the quarter's XBRL dividends per share.
    _, _, ex, cash, _, _, factor, details, _ = rows[("SPLIT", "cash_dividend")]
    assert (ex, cash, factor) == (DIVIDEND, pytest.approx(0.10), pytest.approx(0.99))
    assert json.loads(details)["evidence_basis"] == "vendor_factor+xbrl_dps"
    # The events rebuild every line's vendor factor (FLAT: a line without events).
    assert summary["lines"] == 4 and summary["tie_out_lines_over"] == 0
    assert summary["tie_out_max_abs"] <= TIE_OUT_TOLERANCE
    # No event is visible before its clock: a late-confirmed split only from its share evidence, and a
    # split-sized step read as a distribution only once its share window closed with the count flat.
    visible = "SELECT security_id, action_type FROM ca WHERE available_at <= ? ORDER BY 1, 2"
    assert con.execute(visible, [_at(EX, 21)]).fetchall() == []
    assert con.execute(visible, [_at(EX)]).fetchall() == [("SPLIT", "split")]
    assert rows[("LATE", "split")][8] == _at(LATE_SHARES)
    assert _at(EX + dt.timedelta(days=80)) < rows[("SPECIAL", "distribution_unclassified")][8] < _at(LATE_SHARES)
