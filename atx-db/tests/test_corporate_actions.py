"""P8: corporate-action events from the vendor factor, classified by R1d's corroborated split epochs."""

from __future__ import annotations

import datetime as dt
import json

import duckdb
import pytest

from atx_db.corporate_actions import (
    TIE_OUT_TOLERANCE,
    build_corporate_action_events,
    corporate_actions_asof_sql,
    corporate_actions_current_sql,
)

EX, LATE_SHARES, DIVIDEND = dt.date(2021, 3, 15), dt.date(2021, 6, 23), dt.date(2021, 6, 15)
XBRL_AT = dt.datetime(2021, 8, 5, 16)
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
                f"'USD/shares', DATE '2021-04-01', DATE '2021-06-30', 0.10, TIMESTAMP '{XBRL_AT}')")
    summary = build_corporate_action_events(con, "ca", run_id="t")
    yield con, summary
    con.close()


def _visible(con, cutoff: dt.datetime):
    """(security, action type, reason or evidence) of each step's row visible at ``cutoff``."""
    visible = corporate_actions_asof_sql(f"TIMESTAMP '{cutoff}'", "ca")
    rows = con.execute(f"SELECT security_id, action_type, details_json FROM ({visible}) ORDER BY 1, 2")
    return [(sid, kind, json.loads(details)["reason"] or json.loads(details)["evidence_basis"])
            for sid, kind, details in rows.fetchall()]


def test_events_are_labelled_by_their_evidence_revisioned_at_its_clocks_and_tie_out(events):
    con, summary = events
    latest = {(r[0], r[1]): r for r in con.execute(
        "SELECT security_id, action_type, ex_date, cash_amount, split_from, split_to, adjustment_factor, "
        "details_json, available_at FROM ca WHERE is_latest_revision").fetchall()}
    assert set(latest) == {("SPLIT", "split"), ("SPLIT", "cash_dividend"), ("SPECIAL", "distribution_unclassified"),
                           ("LATE", "split")}
    # 2:1 with share corroboration: a split of 2.0, known at the ex-date bar (no earlier revision).
    _, _, ex, cash, split_from, split_to, factor, details, available = latest[("SPLIT", "split")]
    assert (ex, cash, split_from, split_to, factor, available) == (EX, None, 1.0, 2.0, pytest.approx(0.5), _at(EX))
    assert (json.loads(details)["corroboration"], json.loads(details)["revision"]) == ("share_count", 0)
    # A 25 % special with a flat count is a distribution, never a split.
    _, _, ex, cash, split_from, _, _, details, _ = latest[("SPECIAL", "distribution_unclassified")]
    assert (ex, cash, split_from) == (EX, pytest.approx(10.0 * SPECIAL_CUT), None)
    assert (json.loads(details)["reason"], json.loads(details)["evidence_basis"]) == ("special_or_spinoff",
                                                                                     "vendor_factor")
    # A 1 % cash residual, later corroborated by the quarter's XBRL dividends per share.
    assert latest[("SPLIT", "cash_dividend")][2:4] == (DIVIDEND, pytest.approx(0.10))
    # The events rebuild every line's vendor factor (FLAT: a line without events), one row per step.
    assert summary["lines"] == 4 and summary["tie_out_lines_over"] == 0 and summary["revisions"] == 3
    assert summary["tie_out_max_abs"] <= TIE_OUT_TOLERANCE
    # Point in time: each step's state is visible from the clock it was decidable, never earlier.
    pending = "split_candidate_pending"
    assert _visible(con, _at(EX, 21)) == []
    assert _visible(con, _at(EX)) == [("LATE", "adjustment_unclassified", pending),
                                      ("SPECIAL", "adjustment_unclassified", pending),
                                      ("SPLIT", "split", "vendor_factor+shares")]
    assert _visible(con, _at(DIVIDEND + dt.timedelta(days=1), 12)) == [
        ("LATE", "adjustment_unclassified", pending), ("SPECIAL", "distribution_unclassified", "special_or_spinoff"),
        ("SPLIT", "cash_dividend", "vendor_factor"), ("SPLIT", "split", "vendor_factor+shares")]
    assert _visible(con, XBRL_AT) == [
        ("LATE", "split", "vendor_factor+shares"), ("SPECIAL", "distribution_unclassified", "special_or_spinoff"),
        ("SPLIT", "cash_dividend", "vendor_factor+xbrl_dps"), ("SPLIT", "split", "vendor_factor+shares")]
    # Revisions chain: the pending row is superseded at the confirmation; the late split names it.
    chain = [(r[0], json.loads(r[1]), r[2]) for r in con.execute(
        "SELECT action_type, details_json, available_at FROM ca WHERE security_id = 'LATE' ORDER BY available_at"
    ).fetchall()]
    assert [(kind, at) for kind, _, at in chain] == [("adjustment_unclassified", _at(EX)), ("split", _at(LATE_SHARES))]
    assert chain[0][1]["superseded_at"] == _at(LATE_SHARES).isoformat(sep=" ")
    assert chain[1][1]["supersedes_event_id"] == chain[0][1]["event_id"]
    # The current relation counts the dividend once, at its ex-date clock, with its final evidence.
    current = con.execute(f"SELECT available_at, details_json FROM ({corporate_actions_current_sql('ca')}) "
                          "WHERE security_id = 'SPLIT' AND action_type = 'cash_dividend'").fetchall()
    assert [(at, json.loads(details)["evidence_basis"]) for at, details in current] == [
        (_at(DIVIDEND), "vendor_factor+xbrl_dps")]
