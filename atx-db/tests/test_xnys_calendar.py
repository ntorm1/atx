"""PIT clock edge cases of the canonical XNYS calendar and the entry-clock rule (tier-1 v2 node 1.11).

Rule (docs/methodology/CLOCKS.md): a feature dated t is known at t 22:00 UTC; positions enter at
the close of the next session; FC1 fundamentals are usable at SEC filing date + 46 h.
"""

from __future__ import annotations

import datetime as dt
from zoneinfo import ZoneInfo

import pytest

from atx_db import calendar as xnys

D = dt.date


@pytest.mark.parametrize(("feature_date", "entry"), [
    (D(2024, 3, 4), D(2024, 3, 5)),      # a session never enters on itself (known after its close)
    (D(2024, 3, 1), D(2024, 3, 4)),      # Friday -> Monday
    (D(2024, 3, 2), D(2024, 3, 4)),      # a weekend feature date is not a session
    (D(2024, 3, 28), D(2024, 4, 1)),     # Good Friday
    (D(2012, 10, 26), D(2012, 10, 31)),  # Hurricane Sandy, Oct 29-30
    (D(2018, 12, 4), D(2018, 12, 6)),    # G.H.W. Bush funeral
    (D(2025, 1, 8), D(2025, 1, 10)),     # Carter mourning day
    (D(2021, 12, 30), D(2021, 12, 31)),  # New Year on Saturday is not observed on Friday
    (D(2021, 12, 31), D(2022, 1, 3)),
    (D(2021, 6, 17), D(2021, 6, 18)),    # Juneteenth is a closure only from 2022
    (D(2022, 6, 17), D(2022, 6, 21)),    # ... observed Monday 2022-06-20
    (D(2024, 7, 2), D(2024, 7, 3)),      # an early close is a session
    (D(2024, 7, 3), D(2024, 7, 5)),
])
def test_entry_is_the_close_of_the_next_rule_session(feature_date, entry):
    assert xnys.next_session(feature_date) == entry
    assert xnys.is_session(entry)
    assert xnys.xnys_sessions(feature_date + dt.timedelta(days=1), entry) == [entry]


def test_cutoff_is_22_utc_after_every_close_and_refuses_timestamps():
    cutoff = xnys.decision_cutoff_utc(D(2024, 1, 31))
    assert cutoff == dt.datetime(2024, 1, 31, 22, tzinfo=dt.UTC)
    assert cutoff.utcoffset() == dt.timedelta(0)
    # The cutoff follows the close in both DST regimes and on early closes, so no position can
    # enter at the close of the feature's own session.
    new_york = ZoneInfo("America/New_York")
    early = xnys.xnys_early_closes(D(2012, 1, 1), D(2026, 12, 31))
    for day in (D(2024, 1, 31), D(2024, 7, 31), D(2024, 3, 8), D(2024, 3, 11), D(2024, 11, 1), D(2024, 11, 4),
                *early):
        close = dt.datetime.combine(day, early.get(day, xnys.REGULAR_CLOSE_ET), tzinfo=new_york)
        assert close < xnys.decision_cutoff_utc(day), day
    # datetime is a date subclass: a timestamp would silently lose its clock.
    for clock in (xnys.decision_cutoff_utc, xnys.next_session, xnys.is_session):
        with pytest.raises(TypeError):
            clock(dt.datetime(2024, 1, 31, 23, 30))


def test_fc1_filing_clock_composes_with_the_entry_rule():
    # Filed on F (a date-only SEC stamp): usable at F 00:00 UTC + 46 h = (F + 1) 22:00 UTC, i.e. a
    # feature dated F + 1, entering at the close of the session after F + 1.
    for filed, entry in ((D(2024, 3, 4), D(2024, 3, 6)), (D(2024, 3, 1), D(2024, 3, 4)),
                         (D(2024, 3, 27), D(2024, 4, 1))):
        usable = dt.datetime.combine(filed, dt.time(0), tzinfo=dt.UTC) + dt.timedelta(hours=46)
        known_on = filed + dt.timedelta(days=1)
        assert usable == xnys.decision_cutoff_utc(known_on)
        assert xnys.next_session(known_on) == entry


def test_early_closes_follow_the_rules():
    closes = xnys.xnys_early_closes(D(2012, 1, 1), D(2025, 12, 31))
    assert all(time == dt.time(13) for time in closes.values())
    by_year: dict[int, list[dt.date]] = {}
    for day in closes:
        by_year.setdefault(day.year, []).append(day)
    assert by_year[2012] == [D(2012, 7, 3), D(2012, 11, 23), D(2012, 12, 24)]
    assert by_year[2013] == [D(2013, 7, 3), D(2013, 11, 29), D(2013, 12, 24)]   # Wednesday rule from 2013
    assert by_year[2015] == [D(2015, 11, 27), D(2015, 12, 24)]                  # July 3 is the observed holiday
    assert by_year[2021] == [D(2021, 11, 26)]                                   # Dec 24 is the observed holiday
    assert by_year[2025] == [D(2025, 7, 3), D(2025, 11, 28), D(2025, 12, 24)]
    assert xnys.xnys_early_closes(D(2002, 7, 1), D(2002, 7, 31)) == {D(2002, 7, 5): dt.time(13)}
    with pytest.raises(ValueError):
        xnys.xnys_early_closes(D(1999, 12, 1), D(2000, 1, 31))


def test_reconcile_lists_stray_bars_and_missing_sessions():
    bars = [D(2024, 3, 27), D(2024, 3, 29), D(2024, 3, 30), D(2024, 4, 2), D(2024, 5, 1)]
    assert xnys.reconcile_sessions_with_bars(bars, D(2024, 3, 27), D(2024, 4, 2)) == {
        "missing": [D(2024, 3, 28), D(2024, 4, 1)],
        "extra": [D(2024, 3, 29), D(2024, 3, 30)],   # Good Friday and a Saturday; 05-01 is out of range
    }
