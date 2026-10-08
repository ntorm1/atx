"""NYSE sessions from rules: equal to the vendor calendar on its whole extent, extend past it, mismatches abort."""

from __future__ import annotations

import datetime as dt
import hashlib

import pytest

from atx_db.alpha_panel import exchange_calendar as E

D = dt.date

# The vendor ``calendar.parquet`` (3,642 sessions, 2012-03-26 .. 2026-09-18) as data: its extent and the weekdays it
# has no session on. Measured 2026-10-05 under the memory guard (Task 6 report); VENDOR_SHA256 is the SHA-256 of the
# file's session dates, one ISO date per line joined by "\n", so the fixture is bound to the real file.
VENDOR_FIRST, VENDOR_LAST, VENDOR_SESSIONS = D(2012, 3, 26), D(2026, 9, 18), 3642
VENDOR_SHA256 = "a997fc32975dca8fff8ada817ecdd7b7029b18e637521523604ff3b8e7a2424d"
VENDOR_CLOSED_WEEKDAYS = """
    2012-04-06 2012-05-28 2012-07-04 2012-09-03 2012-10-29 2012-10-30 2012-11-22 2012-12-25 2013-01-01
    2013-01-21 2013-02-18 2013-03-29 2013-05-27 2013-07-04 2013-09-02 2013-11-28 2013-12-25 2014-01-01
    2014-01-20 2014-02-17 2014-04-18 2014-05-26 2014-07-04 2014-09-01 2014-11-27 2014-12-25 2015-01-01
    2015-01-19 2015-02-16 2015-04-03 2015-05-25 2015-07-03 2015-09-07 2015-11-26 2015-12-25 2016-01-01
    2016-01-18 2016-02-15 2016-03-25 2016-05-30 2016-07-04 2016-09-05 2016-11-24 2016-12-26 2017-01-02
    2017-01-16 2017-02-20 2017-04-14 2017-05-29 2017-07-04 2017-09-04 2017-11-23 2017-12-25 2018-01-01
    2018-01-15 2018-02-19 2018-03-30 2018-05-28 2018-07-04 2018-09-03 2018-11-22 2018-12-05 2018-12-25
    2019-01-01 2019-01-21 2019-02-18 2019-04-19 2019-05-27 2019-07-04 2019-09-02 2019-11-28 2019-12-25
    2020-01-01 2020-01-20 2020-02-17 2020-04-10 2020-05-25 2020-07-03 2020-09-07 2020-11-26 2020-12-25
    2021-01-01 2021-01-18 2021-02-15 2021-04-02 2021-05-31 2021-07-05 2021-09-06 2021-11-25 2021-12-24
    2022-01-17 2022-02-21 2022-04-15 2022-05-30 2022-06-20 2022-07-04 2022-09-05 2022-11-24 2022-12-26
    2023-01-02 2023-01-16 2023-02-20 2023-04-07 2023-05-29 2023-06-19 2023-07-04 2023-09-04 2023-11-23
    2023-12-25 2024-01-01 2024-01-15 2024-02-19 2024-03-29 2024-05-27 2024-06-19 2024-07-04 2024-09-02
    2024-11-28 2024-12-25 2025-01-01 2025-01-09 2025-01-20 2025-02-17 2025-04-18 2025-05-26 2025-06-19
    2025-07-04 2025-09-01 2025-11-27 2025-12-25 2026-01-01 2026-01-19 2026-02-16 2026-04-03 2026-05-25
    2026-06-19 2026-07-03 2026-09-07
""".split()  # noqa: SIM905 (a data block)


def vendor_sessions() -> list[dt.date]:
    closed = {D.fromisoformat(s) for s in VENDOR_CLOSED_WEEKDAYS}
    days = (VENDOR_FIRST + dt.timedelta(days=i) for i in range((VENDOR_LAST - VENDOR_FIRST).days + 1))
    return [d for d in days if d.weekday() < 5 and d not in closed]


def test_fixture_is_the_vendor_file():
    got = vendor_sessions()
    assert len(got) == VENDOR_SESSIONS
    assert hashlib.sha256("\n".join(d.isoformat() for d in got).encode()).hexdigest() == VENDOR_SHA256


def test_rule_sessions_equal_vendor_sessions_on_their_overlap():
    vendor = vendor_sessions()
    assert E.sessions(VENDOR_FIRST, VENDOR_LAST) == vendor
    # a rule calendar wider than the vendor file is compared on the overlap only
    wide = E.sessions(D(2010, 1, 4), D(2027, 12, 31))
    check = E.check_against_vendor(wide, vendor)
    assert check == {"rule": E.RULE, "overlap": [VENDOR_FIRST, VENDOR_LAST], "sessions": VENDOR_SESSIONS,
                     "match": True}


def test_unscheduled_closures_and_sessions_past_the_vendor_end():
    cal = E.sessions(D(2012, 10, 26), D(2027, 12, 31))
    # Hurricane Sandy and the two national days of mourning come from the package's NYSE calendar
    assert {D(2012, 10, 29), D(2012, 10, 30), D(2018, 12, 5), D(2025, 1, 9)}.isdisjoint(cal)
    # past the vendor's last session: Labor Day, Thanksgiving, Christmas; early closes are sessions
    assert cal[cal.index(VENDOR_LAST) + 1] == D(2026, 9, 21)
    assert {D(2026, 11, 26), D(2026, 12, 25), D(2027, 6, 18), D(2027, 12, 24)}.isdisjoint(cal)
    assert {D(2026, 11, 27), D(2026, 12, 24), D(2026, 12, 11), D(2026, 12, 18)} <= set(cal)
    assert all(d.weekday() < 5 for d in cal) and cal == sorted(set(cal))


def test_mismatch_with_the_vendor_aborts():
    vendor = vendor_sessions()
    rules = E.sessions(VENDOR_FIRST, VENDOR_LAST)
    missing = [d for d in vendor if d != D(2021, 6, 25)]                  # a vendor gap on a real session
    with pytest.raises(ValueError, match="2021-06-25"):
        E.check_against_vendor(rules, missing)
    extra = sorted([*vendor, D(2021, 4, 2)])                              # a vendor session on Good Friday
    with pytest.raises(ValueError, match="2021-04-02"):
        E.check_against_vendor(rules, extra)
    with pytest.raises(ValueError, match="overlap"):
        E.check_against_vendor(E.sessions(D(2027, 1, 4), D(2027, 2, 1)), vendor)
