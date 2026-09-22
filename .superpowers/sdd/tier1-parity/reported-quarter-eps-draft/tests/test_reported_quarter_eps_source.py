"""Focused, network-free contracts for SEC Item 2.02 reported EPS evidence."""

from __future__ import annotations

import datetime as dt

from atx_db.press_release import (
    SEC_FILING_DATE_CLOCK_POLICY,
    _daily_sec_clock,
    _ex99_documents,
    _explicit_sec_clock,
    extract_reported_gaap_diluted_eps,
)


def test_explicit_sec_offset_is_preserved_but_daily_clock_is_conservative() -> None:
    clock = _daily_sec_clock("2026-01-30T18:17:00-05:00", dt.date(2026, 1, 30))

    assert clock.raw_timestamp == "2026-01-30T18:17:00-05:00"
    assert clock.utc_offset == "-05:00"
    assert clock.available_at == dt.datetime(2026, 2, 1, 0, 0)
    assert clock.timezone_status == f"timestamp_offset_valid:{SEC_FILING_DATE_CLOCK_POLICY}"


def test_naive_source_timestamp_cannot_be_promoted_to_exact_utc() -> None:
    exact = _explicit_sec_clock("2026-01-30 18:17:00")
    daily = _daily_sec_clock("2026-01-30 18:17:00", dt.date(2026, 1, 30))

    assert exact.available_at is None
    assert exact.timezone_status == "timestamp_zone_unknown"
    assert daily.available_at == dt.datetime(2026, 2, 1, 0, 0)
    assert daily.timezone_status == f"timestamp_zone_unknown:{SEC_FILING_DATE_CLOCK_POLICY}"


def test_ex99_selection_rejects_multiple_documents_upstream() -> None:
    payload = {"directory": {"item": [
        {"name": "ex99.htm", "type": "EX-99.1"},
        {"name": "exhibit99-2.htm", "type": "EX-99.2"},
        {"name": "form8k.htm", "type": "8-K"},
    ]}}
    assert _ex99_documents(payload) == ("ex99.htm", "exhibit99-2.htm")


def test_extracts_one_gaap_diluted_eps_with_aligned_three_month_column() -> None:
    document = """
    <table><tr><th></th><th>Three Months Ended December 31,</th><th></th></tr>
    <tr><th></th><th>2025</th><th>2024</th></tr>
    <tr><td>Earnings per share - diluted</td><td>1.39</td><td>1.84</td></tr></table>
    """
    fact, reason = extract_reported_gaap_diluted_eps(document, period_end=dt.date(2025, 12, 31))

    assert reason is None
    assert fact is not None
    assert fact["value"] == 1.39
    assert fact["column_number"] == 1


def test_adjusted_or_continuing_operations_eps_is_not_reported_gaap_diluted_eps() -> None:
    document = """
    <table><tr><th></th><th>Three Months Ended December 31,</th></tr>
    <tr><th></th><th>2025</th></tr>
    <tr><td>Adjusted diluted earnings per share from continuing operations</td><td>4.20</td></tr></table>
    """
    fact, reason = extract_reported_gaap_diluted_eps(document, period_end=dt.date(2025, 12, 31))

    assert fact is None
    assert reason == "rejected_non_gaap_or_adjusted"
