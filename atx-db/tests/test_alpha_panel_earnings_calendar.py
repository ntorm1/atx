"""Pure helpers of the alpha panel's earnings_calendar stage: NYSE calendar, ET timing, expected-date rule."""

from __future__ import annotations

import datetime as dt

from atx_db.alpha_panel import earnings_calendar as E


def _d(s: str) -> dt.date:
    return dt.date.fromisoformat(s)


def test_easter() -> None:
    assert E.easter(2024) == _d("2024-03-31")
    assert E.easter(2026) == _d("2026-04-05")
    assert E.easter(2019) == _d("2019-04-21")


def test_nyse_holidays_known_years() -> None:
    assert E.nyse_holidays(2024) == {_d(x) for x in (
        "2024-01-01", "2024-01-15", "2024-02-19", "2024-03-29", "2024-05-27", "2024-06-19", "2024-07-04",
        "2024-09-02", "2024-11-28", "2024-12-25")}
    # New Year on a Saturday is not observed; Juneteenth on a Sunday moves to Monday.
    assert E.nyse_holidays(2022) == {_d(x) for x in (
        "2022-01-17", "2022-02-21", "2022-04-15", "2022-05-30", "2022-06-20", "2022-07-04", "2022-09-05",
        "2022-11-24", "2022-12-26")}
    h21 = E.nyse_holidays(2021)
    assert _d("2021-07-05") in h21 and _d("2021-12-24") in h21 and not any(d.month == 6 for d in h21)
    assert {_d("2012-10-29"), _d("2012-10-30")} <= E.nyse_holidays(2012)
    assert _d("2026-07-03") in E.nyse_holidays(2026) and _d("2026-04-03") in E.nyse_holidays(2026)


def test_nyse_early_closes() -> None:
    assert E.nyse_early_closes(2024) == {_d("2024-07-03"), _d("2024-11-29"), _d("2024-12-24")}
    assert E.nyse_early_closes(2021) == {_d("2021-11-26")}  # Dec 24 is the observed Christmas holiday
    assert E.nyse_early_closes(2026) == {_d("2026-11-27"), _d("2026-12-24")}  # July 3 is a holiday


def _cal(start: str = "2023-12-01", end: str = "2025-03-31") -> E.SessionCalendar:
    return E.SessionCalendar(E.nyse_rule_sessions(_d(start), _d(end)))


def test_build_calendar_prefers_vendor_inside_range() -> None:
    vendor = [d for d in E.nyse_rule_sessions(_d("2024-01-02"), _d("2024-01-31")) if d != _d("2024-01-10")]
    cal, audit = E.build_calendar(vendor, _d("2023-12-01"), _d("2024-02-29"))
    assert not cal.is_session(_d("2024-01-10"))  # vendor wins inside its range
    assert cal.is_session(_d("2023-12-29")) and cal.basis[_d("2023-12-29")] == "nyse_rule"
    assert cal.basis[_d("2024-01-11")] == "vendor_calendar"
    assert audit["rule_not_vendor"] == ["2024-01-10"] and audit["vendor_not_rule"] == []


def test_classify_timing_and_sessions() -> None:
    cal = _cal()
    cases = [
        (dt.datetime(2024, 1, 25, 7, 0), "pre_market", "2024-01-25", "2024-01-25"),
        (dt.datetime(2024, 1, 25, 9, 29, 59), "pre_market", "2024-01-25", "2024-01-25"),
        (dt.datetime(2024, 1, 25, 9, 30), "intraday", "2024-01-25", "2024-01-25"),
        (dt.datetime(2024, 1, 25, 15, 59), "intraday", "2024-01-25", "2024-01-25"),
        (dt.datetime(2024, 1, 25, 16, 0), "post_market", "2024-01-25", "2024-01-26"),
        (dt.datetime(2024, 1, 26, 16, 30), "post_market", "2024-01-26", "2024-01-29"),  # Friday -> Monday
        (dt.datetime(2024, 3, 29, 10, 0), "closed_day", "2024-04-01", "2024-04-01"),     # Good Friday
        (dt.datetime(2024, 11, 29, 14, 0), "post_market", "2024-11-29", "2024-12-02"),   # early close 13:00
        (dt.datetime(2024, 11, 29, 12, 0), "intraday", "2024-11-29", "2024-11-29"),
    ]
    for et, timing, sd, rs in cases:
        assert E.classify_timing(et, cal) == timing, et
        assert E.sessions_for(et, timing, cal) == (_d(sd), _d(rs)), et


def test_match_year_ago() -> None:
    prior = [(_d("2023-03-31"), 0), (_d("2023-06-30"), 1), (_d("2023-04-01"), 2)]
    assert E.match_year_ago(_d("2024-03-30"), prior) == 0  # 365 days vs 364 for 2023-04-01
    assert E.match_year_ago(_d("2024-06-29"), prior) == 1
    assert E.match_year_ago(_d("2024-09-28"), prior) is None


def _ann(acc: str, utc: dt.datetime, pe: str | None) -> dict:
    return {"cik": 1, "accession": acc, "form": "8-K", "items": "2.02,9.01", "filing_date": utc.date(),
            "event_date": utc.date(), "announcement_utc": utc, "available_at": utc,
            "fiscal_period_end": _d(pe) if pe else None, "fiscal_period_form": "10-Q"}


def test_process_cik_expected_dates_and_flags() -> None:
    cal = _cal("2022-12-01", "2025-06-30")
    rows = [
        _ann("a1", dt.datetime(2023, 4, 27, 20, 30), "2023-03-31"),   # 16:30 ET post-market, Thursday
        _ann("a2", dt.datetime(2023, 7, 27, 20, 30), "2023-06-30"),
        _ann("a2b", dt.datetime(2023, 7, 27, 21, 0), "2023-06-30"),   # same reaction session: duplicate
        _ann("a3", dt.datetime(2024, 4, 25, 20, 30), "2024-03-31"),   # year-ago = a1
        _ann("a4", dt.datetime(2024, 5, 15, 12, 0), "2024-03-31"),    # second release for the same period
    ]
    out = {o["accession"]: o for o in E.process_cik(rows, cal)}
    a1, a2, a2b, a3, a4 = (out[k] for k in ("a1", "a2", "a2b", "a3", "a4"))
    assert a1["timing"] == "post_market" and a1["session_date"] == _d("2023-04-27")
    assert a1["reaction_session"] == _d("2023-04-28") and a1["is_primary"]
    assert a1["expected_date"] is None  # no prior announcement
    assert a1["next_expected_date"] == _d("2024-04-25")  # 2023-04-27 + 364 days is a Thursday session
    assert a2["expected_rule"] == "prev_primary_plus_91" and a2["expected_source_accession"] == "a1"
    assert a2["expected_date"] == _d("2023-07-27")  # 2023-04-27 + 91 days
    assert a2["expected_error_sessions"] == 0
    assert a2b["same_session_dup"] and not a2b["is_primary"]
    assert a3["expected_rule"] == "yoy_364" and a3["expected_source_accession"] == "a1"
    assert a3["expected_date"] == _d("2024-04-25") and a3["expected_error_sessions"] == 0
    assert a3["expected_available_at"] == dt.datetime(2023, 4, 27, 20, 30)  # known a year in advance
    assert not a4["is_primary"] and a4["expected_date"] is None
    assert a4["period_lag_days"] == 45


def test_process_cik_error_sign() -> None:
    cal = _cal("2022-12-01", "2025-06-30")
    rows = [_ann("a1", dt.datetime(2023, 4, 27, 11, 0), "2023-03-31"),   # 07:00 ET pre-market
            _ann("a3", dt.datetime(2024, 4, 30, 11, 0), "2024-03-31")]   # three sessions later than expected
    out = {o["accession"]: o for o in E.process_cik(rows, cal)}
    assert out["a1"]["timing"] == "pre_market" and out["a1"]["reaction_session"] == _d("2023-04-27")
    assert out["a3"]["expected_date"] == _d("2024-04-25")
    assert out["a3"]["expected_error_sessions"] == 3 and out["a3"]["expected_error_days"] == 5
