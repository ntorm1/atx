"""Earnings calendar v2 (S6.5): 6-K results classification and the periodic-report fallback merge."""

from __future__ import annotations

import datetime as dt

from atx_db.alpha_panel import earnings_calendar as EC1
from atx_db.alpha_panel import earnings_calendar_v2 as V2

FILED = dt.date(2024, 11, 5)


def test_results_release_with_period_date() -> None:
    text = ("Exhibit 99.1\nACME Ltd. Reports Third Quarter 2024 Financial Results\nTEL AVIV, November 5, 2024 - "
            "ACME Ltd. today announced its unaudited results for the third quarter ended September 30, 2024.")
    r = V2.classify_release(text, FILED)
    assert r is not None and r["period_end"] == dt.date(2024, 9, 30) and r["period_basis"] == "text_date"


def test_results_release_label_only_uses_fiscal_year_end() -> None:
    text = "Foo plc announces interim results for H1 2024. Revenue grew 12%."
    r = V2.classify_release(text, dt.date(2024, 8, 1))
    assert r is not None and r["period_end"] == dt.date(2024, 6, 30) and r["period_basis"] == "text_label"
    # fiscal year ending March: Q2 of fiscal 2025 ends September 2024
    r2 = V2.classify_release("Bar Ltd reports second quarter fiscal 2025 results", FILED, fye_month=3)
    assert r2 is not None and r2["period_end"] == dt.date(2024, 9, 30)


def test_not_results_releases() -> None:
    for text in (
        "ACME Ltd. to Report Third Quarter 2024 Financial Results on November 12, 2024",
        "ACME will release its third quarter 2024 results before the market opens on November 12.",
        "Results of the Annual General Meeting held on June 3, 2024. All resolutions were passed.",
        "Foo Mining announces Q3 2024 production update and exploration results.",
        "Notice of board meeting.",
    ):
        assert V2.classify_release(text, FILED) is None, text


def _row(src: str, acc: str, at: dt.datetime, pe: dt.date | None) -> dict:
    return {"cik": 1, "accession": acc, "source": src, "announcement_utc": at, "available_at": at,
            "fiscal_period_end": pe, "form": "x"}


def test_periodic_fallback_only_when_not_announced_first() -> None:
    cal = EC1.SessionCalendar(d for d in (dt.date(2024, 1, 1) + dt.timedelta(days=i) for i in range(400))
                              if d.weekday() < 5)
    rows = [
        _row("8k_202", "a1", dt.datetime(2024, 4, 25, 20, 30), dt.date(2024, 3, 31)),
        _row("periodic_report", "q1", dt.datetime(2024, 5, 2, 20, 0), dt.date(2024, 3, 31)),
        # no 2.02 for Q2: the 10-Q is the first disclosure
        _row("periodic_report", "q2", dt.datetime(2024, 8, 1, 20, 0), dt.date(2024, 6, 30)),
        # 6-K for Q3 within 10 days of the report date, accepted before the report
        _row("6k_results", "k3", dt.datetime(2024, 10, 30, 11, 0), dt.date(2024, 9, 28)),
        _row("periodic_report", "q3", dt.datetime(2024, 11, 5, 20, 0), dt.date(2024, 9, 30)),
    ]
    out = V2._merge_cik(rows, cal)
    assert [r["accession"] for r in out] == ["a1", "q2", "k3"]
    assert [r["timing"] for r in out] == ["post_market", "post_market", "pre_market"]
    assert all(r["is_primary"] for r in out)
    assert out[2]["expected_rule"] == "prev_primary_plus_91"
