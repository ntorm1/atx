"""The single as-of-date resolution contract for deterministic ingest paths."""

from __future__ import annotations

import datetime as dt

import pytest

from atx_db.clock import resolve_as_of_date, utc_today


def test_explicit_as_of_date_wins_over_source_max_date():
    resolved = resolve_as_of_date(dt.date(2020, 1, 2), source_max_date=dt.date(2024, 6, 30))
    assert resolved == dt.date(2020, 1, 2)


def test_source_max_date_is_used_when_no_explicit_date():
    resolved = resolve_as_of_date(None, source_max_date=dt.date(2024, 6, 30))
    assert resolved == dt.date(2024, 6, 30)


def test_missing_both_raises_with_an_actionable_message():
    with pytest.raises(ValueError) as excinfo:
        resolve_as_of_date(None)
    message = str(excinfo.value)
    assert "as_of_date is required" in message
    assert "source_max_date" in message


def test_datetime_source_max_date_is_narrowed_to_a_date():
    resolved = resolve_as_of_date(None, source_max_date=dt.datetime(2024, 6, 30, 22, 0, 0))
    assert resolved == dt.date(2024, 6, 30)
    assert not isinstance(resolved, dt.datetime)


def test_utc_today_matches_the_utc_calendar_date():
    assert utc_today() == dt.datetime.now(dt.UTC).date()
