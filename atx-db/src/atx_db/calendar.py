"""Canonical XNYS session calendar and the house point-in-time clocks (tier-1 v2 node 1.11).

Sessions are rule-based, never read from price bars: a session is a weekday that is
not a full-day NYSE closure (the holiday rules of :func:`nyse_full_day_closures`
plus the unscheduled closures in :data:`NYSE_SPECIAL_CLOSURES`). Bars are
reconciled against the calendar (:func:`reconcile_sessions_with_bars`); a bar dated
on a closure or a weekend is a stray bar, never a session. Measured on the retained
``TickerHistory3.parquet`` (2012-03-26 -> 2026-09-18): see
``.superpowers/sdd/tier1-v2/task-1.11-report.md``.

Clocks (``docs/methodology/CLOCKS.md``):

* a feature dated ``t`` is known at ``t`` 22:00 UTC (:func:`decision_cutoff_utc`);
* positions enter at the close of the next session (:func:`next_session`);
* FC1 fundamentals are usable at SEC filing date + 46 h
  (``_fundamental_clock.FUNDAMENTAL_CLOCK_POLICY``).

Validity: the full-day rules and special closures are checked against observed
bars for 2012-03-26 -> 2026-09-18; special closures are listed back to 1994 only.
Early closes (13:00 ET) are the NYSE rules from 2000 on (:data:`EARLY_CLOSE_RULES_FROM`);
ad-hoc early closes are not modeled and daily bars cannot verify any early close.
Future unscheduled closures are unknown until they happen.

:class:`TradingCalendarDataset` writes these sessions into ``trading_calendar`` over
the ``equity_daily_bars`` date range. Its ``source`` value stays
``'equity_daily_bars calendar'``: it is the key every calendar reader (and the
frozen migration 0327 body) filters on; the rows are the rule sessions
(``calendar_basis`` :data:`CALENDAR_BASIS`), no longer the distinct bar dates.
"""

from __future__ import annotations

import datetime as dt
from collections.abc import Iterable
from dataclasses import dataclass

from .connection import DuckDBStore
from .dataset import Dataset, DatasetLoadResult
from .warehouse import quality_check

SOURCE_NAME = "equity_daily_bars calendar"
CALENDAR_ID = "XNYS"
#: ``trading_calendar`` rows are the rule sessions of this module (v1 rule set).
CALENDAR_BASIS = "xnys_rules_v1"

#: House decision cutoff: a feature dated t is known at t 22:00 UTC.
DECISION_HOUR_UTC = 22
#: Regular session hours and the early close, America/New_York wall clock.
REGULAR_OPEN_ET = dt.time(9, 30)
REGULAR_CLOSE_ET = dt.time(16)
EARLY_CLOSE_ET = dt.time(13)
#: Early-close rules are modeled from this date on; before it ``close_time`` is unknown.
EARLY_CLOSE_RULES_FROM = dt.date(2000, 1, 1)

#: Unscheduled full-day NYSE closures (the weekday rule holidays are computed):
#: Nixon funeral, 9/11, Reagan funeral, Ford mourning day, Hurricane Sandy,
#: G.H.W. Bush funeral, Carter mourning day.
NYSE_SPECIAL_CLOSURES = frozenset({
    dt.date(1994, 4, 27), dt.date(2001, 9, 11), dt.date(2001, 9, 12), dt.date(2001, 9, 13),
    dt.date(2001, 9, 14), dt.date(2004, 6, 11), dt.date(2007, 1, 2), dt.date(2012, 10, 29),
    dt.date(2012, 10, 30), dt.date(2018, 12, 5), dt.date(2025, 1, 9),
})

#: A bounded search: no run of consecutive non-sessions is anywhere near this long.
_MAX_CLOSED_RUN_DAYS = 14


# ---------------------------------------------------------------------------
# Holiday rules
# ---------------------------------------------------------------------------

def _easter(year: int) -> dt.date:
    """Gregorian Easter Sunday (anonymous algorithm)."""
    a, b, c = year % 19, year // 100, year % 100
    d, e = b // 4, b % 4
    f = (b + 8) // 25
    g = (b - f + 1) // 3
    h = (19 * a + b - d - g + 15) % 30
    i, k = c // 4, c % 4
    l_ = (32 + 2 * e + 2 * i - h - k) % 7
    m = (a + 11 * h + 22 * l_) // 451
    month = (h + l_ - 7 * m + 114) // 31
    day = (h + l_ - 7 * m + 114) % 31 + 1
    return dt.date(year, month, day)


def _month_last_day(year: int, month: int) -> dt.date:
    return dt.date(year + month // 12, month % 12 + 1, 1) - dt.timedelta(days=1)


def _nth_weekday(year: int, month: int, weekday: int, nth: int) -> dt.date:
    first = dt.date(year, month, 1)
    return first + dt.timedelta(days=(weekday - first.weekday()) % 7 + 7 * (nth - 1))


def _last_weekday(year: int, month: int, weekday: int) -> dt.date:
    last = _month_last_day(year, month)
    return last - dt.timedelta(days=(last.weekday() - weekday) % 7)


def _observed(day: dt.date) -> dt.date:
    if day.weekday() == 5:
        return day - dt.timedelta(days=1)
    if day.weekday() == 6:
        return day + dt.timedelta(days=1)
    return day


def nyse_full_day_closures(year: int) -> frozenset[dt.date]:
    """NYSE full-day holidays of ``year`` (rule-based) plus known special closures.

    New Year's Day on a Saturday is not observed on the prior Friday (NYSE rule).
    """
    days: set[dt.date] = set()
    new_year = dt.date(year, 1, 1)
    if new_year.weekday() == 6:
        days.add(new_year + dt.timedelta(days=1))
    elif new_year.weekday() < 5:
        days.add(new_year)
    if year >= 1998:
        days.add(_nth_weekday(year, 1, 0, 3))
    days.add(_nth_weekday(year, 2, 0, 3))
    days.add(_easter(year) - dt.timedelta(days=2))
    days.add(_last_weekday(year, 5, 0))
    if year >= 2022:
        days.add(_observed(dt.date(year, 6, 19)))
    days.add(_observed(dt.date(year, 7, 4)))
    days.add(_nth_weekday(year, 9, 0, 1))
    days.add(_nth_weekday(year, 11, 3, 4))
    days.add(_observed(dt.date(year, 12, 25)))
    days.update(day for day in NYSE_SPECIAL_CLOSURES if day.year == year)
    return frozenset(day for day in days if day.year == year)


def _early_close_rule_days(year: int) -> set[dt.date]:
    """13:00 ET closes of ``year`` by rule (candidates; the caller keeps sessions only).

    Day after Thanksgiving; Christmas Eve on Monday-Thursday; July 3 on Monday, Tuesday
    or Thursday, and on Wednesday from 2013; before 2013 the Friday July 5 after a
    Thursday Independence Day instead of the Wednesday.
    """
    days = {_nth_weekday(year, 11, 3, 4) + dt.timedelta(days=1)}
    christmas_eve = dt.date(year, 12, 24)
    if christmas_eve.weekday() <= 3:
        days.add(christmas_eve)
    july_3 = dt.date(year, 7, 3)
    if july_3.weekday() in (0, 1, 3) or (july_3.weekday() == 2 and year >= 2013):
        days.add(july_3)
    july_5 = dt.date(year, 7, 5)
    if july_5.weekday() == 4 and year <= 2012:
        days.add(july_5)
    return days


# ---------------------------------------------------------------------------
# Sessions
# ---------------------------------------------------------------------------

def _as_date(value: object, label: str) -> dt.date:
    # datetime is a date subclass: a timestamp here would silently drop its clock.
    if isinstance(value, dt.datetime) or not isinstance(value, dt.date):
        raise TypeError(f"{label} must be a datetime.date (not a datetime); got {type(value).__name__}")
    return value


def is_session(day: dt.date) -> bool:
    """True when ``day`` is an XNYS session under the rules."""
    day = _as_date(day, "day")
    return day.weekday() < 5 and day not in nyse_full_day_closures(day.year)


def xnys_sessions(start: dt.date, end: dt.date) -> list[dt.date]:
    """The XNYS rule sessions in ``[start, end]`` (weekdays that are not full-day closures).

    ``start`` and ``end`` must be dates: a ``datetime`` never equals a closure date, so it
    would return holidays as sessions (1.11 review m1); it is refused like every clock here.
    """
    start, end = _as_date(start, "start"), _as_date(end, "end")
    closures: dict[int, frozenset[dt.date]] = {}
    days, day = [], start
    while day <= end:
        if day.weekday() < 5 and day not in closures.setdefault(day.year, nyse_full_day_closures(day.year)):
            days.append(day)
        day += dt.timedelta(days=1)
    return days


def xnys_early_closes(start: dt.date, end: dt.date) -> dict[dt.date, dt.time]:
    """The 13:00 ET early-close sessions in ``[start, end]`` -> their close (ET wall clock).

    Rules from :data:`EARLY_CLOSE_RULES_FROM`; an earlier ``start`` is refused, because
    early closes before it are not modeled.
    """
    start, end = _as_date(start, "start"), _as_date(end, "end")
    if start < EARLY_CLOSE_RULES_FROM:
        raise ValueError(f"early closes are modeled from {EARLY_CLOSE_RULES_FROM} on; start={start}")
    closes: dict[dt.date, dt.time] = {}
    for year in range(start.year, end.year + 1):
        for day in sorted(_early_close_rule_days(year)):
            if start <= day <= end and is_session(day):
                closes[day] = EARLY_CLOSE_ET
    return dict(sorted(closes.items()))


def next_session(d: dt.date) -> dt.date:
    """The first XNYS session strictly after ``d`` (``d`` itself need not be a session).

    Entry clock: a feature dated ``d`` (known at ``d`` 22:00 UTC, after every XNYS close)
    enters at the close of ``next_session(d)``.
    """
    day = _as_date(d, "d")
    for _ in range(_MAX_CLOSED_RUN_DAYS):
        day += dt.timedelta(days=1)
        if is_session(day):
            return day
    raise RuntimeError(f"no XNYS session within {_MAX_CLOSED_RUN_DAYS} days after {d}")


def expected_month_end_session(year: int, month: int) -> dt.date:
    """The last XNYS session of the month under the holiday rules."""
    closures = nyse_full_day_closures(year)
    day = _month_last_day(year, month)
    while day.weekday() >= 5 or day in closures:
        day -= dt.timedelta(days=1)
    return day


def decision_cutoff_utc(d: dt.date) -> dt.datetime:
    """``d`` 22:00 UTC (timezone-aware): when a feature dated ``d`` is known (house rule)."""
    return dt.datetime.combine(_as_date(d, "d"), dt.time(DECISION_HOUR_UTC), tzinfo=dt.UTC)


def reconcile_sessions_with_bars(bar_dates: Iterable[dt.date], start: dt.date,
                                 end: dt.date) -> dict[str, list[dt.date]]:
    """{'missing': sessions without bars, 'extra': bar dates that are closures/weekends}

    Both lists are sorted and cover ``[start, end]`` only; bar dates outside it are
    out of scope.
    """
    start, end = _as_date(start, "start"), _as_date(end, "end")
    observed = {_as_date(day, "bar date") for day in bar_dates}
    observed = {day for day in observed if start <= day <= end}
    sessions = xnys_sessions(start, end)
    session_set = set(sessions)
    return {"missing": [day for day in sessions if day not in observed],
            "extra": sorted(observed - session_set)}


# ---------------------------------------------------------------------------
# trading_calendar dataset
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class TradingCalendarOptions:
    calendar_id: str = CALENDAR_ID
    source: str = SOURCE_NAME
    run_id: str | None = None


class TradingCalendarDataset(Dataset):
    """``trading_calendar`` = the XNYS rule sessions over the ``equity_daily_bars`` date range.

    Only open sessions are written (``is_open`` true); ``close_time`` is 13:00 on early
    closes, 16:00 otherwise, and NULL before :data:`EARLY_CLOSE_RULES_FROM`. The rows
    of other (calendar, source) keys are kept. The write is an INSERT-only row swap
    (``_table_swap``; the table has a ``DEFAULT now()`` column) followed by a CHECKPOINT.
    The bar reconciliation is recorded as the ``sessions_reconcile_with_bars`` check.
    """

    dataset_id = "trading_calendar"
    source_name = SOURCE_NAME

    def ensure_schema(self, store: DuckDBStore) -> None:
        store.initialize()

    def load(self, store: DuckDBStore, options: TradingCalendarOptions) -> DatasetLoadResult:
        from ._table_swap import replace_rows_by_swap, table_columns

        if options.calendar_id != CALENDAR_ID:
            raise ValueError(f"the rule calendar is {CALENDAR_ID}; got calendar_id={options.calendar_id!r}")
        bar_counts = dict(store.con.execute(
            "SELECT trade_date, count(*) FROM equity_daily_bars WHERE trade_date IS NOT NULL GROUP BY trade_date"
        ).fetchall())
        sessions: list[dt.date] = []
        early: dict[dt.date, dt.time] = {}
        recon: dict[str, list[dt.date]] = {"missing": [], "extra": []}
        if bar_counts:
            first, last = min(bar_counts), max(bar_counts)
            sessions = xnys_sessions(first, last)
            if last >= EARLY_CLOSE_RULES_FROM:
                early = xnys_early_closes(max(first, EARLY_CLOSE_RULES_FROM), last)
            recon = reconcile_sessions_with_bars(bar_counts, first, last)
        regular, early_text = REGULAR_CLOSE_ET.isoformat(), EARLY_CLOSE_ET.isoformat()
        close_times = [None if day < EARLY_CLOSE_RULES_FROM else early_text if day in early else regular
                       for day in sessions]
        loaded_at = dt.datetime.now(dt.UTC).replace(tzinfo=None)
        with store.transaction() as con:
            columns = table_columns(con, "trading_calendar")
            names = ", ".join(f'"{name}"' for name in columns)
            fresh = {"calendar_id": "?", "trade_date": "s.trade_date", "is_open": "true", "open_time": "?",
                     "close_time": "s.close_time", "source": "?", "source_loaded_at": "?"}
            unknown = set(columns) - set(fresh)
            if unknown:
                raise RuntimeError(f"trading_calendar has columns this writer does not fill: {sorted(unknown)}")
            select = f"""
                SELECT {names} FROM trading_calendar WHERE NOT (calendar_id = ? AND source = ?)
                UNION ALL
                SELECT {", ".join(fresh[name] for name in columns)}
                FROM (SELECT unnest(?::DATE[]) AS trade_date, unnest(?::VARCHAR[]) AS close_time) AS s
            """
            values = {"calendar_id": options.calendar_id, "open_time": REGULAR_OPEN_ET.isoformat(),
                      "source": options.source, "source_loaded_at": loaded_at}
            params = [options.calendar_id, options.source,
                      *(values[name] for name in columns if fresh[name] == "?"), sessions, close_times]
            replace_rows_by_swap(con, "trading_calendar", columns, select, params)
        store.con.execute("CHECKPOINT")
        found = store.con.execute(
            "SELECT count(*) FROM trading_calendar WHERE calendar_id = ? AND source = ?",
            [options.calendar_id, options.source],
        ).fetchone()
        rows = 0 if found is None else int(found[0])
        details = {
            "calendar_id": options.calendar_id,
            "calendar_basis": CALENDAR_BASIS,
            "first_session": sessions[0].isoformat() if sessions else None,
            "last_session": sessions[-1].isoformat() if sessions else None,
            "early_closes": len(early),
            "missing_sessions": len(recon["missing"]),
            "stray_bar_dates": len(recon["extra"]),
        }
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="trading_calendar",
            check_name="open_days_loaded",
            status="passed" if rows > 0 else "warning",
            observed_value=float(rows),
            threshold_value=1.0,
            details={"calendar_id": options.calendar_id},
        )
        quality_check(
            store,
            dataset_id=self.dataset_id,
            table_name="trading_calendar",
            check_name="sessions_reconcile_with_bars",
            status="passed" if not recon["missing"] and not recon["extra"] else "warning",
            observed_value=float(len(recon["missing"]) + len(recon["extra"])),
            threshold_value=0.0,
            details={
                "calendar_id": options.calendar_id,
                "missing_sessions": [day.isoformat() for day in recon["missing"]],
                "stray_bar_dates": {day.isoformat(): int(bar_counts[day]) for day in recon["extra"]},
            },
        )
        return DatasetLoadResult(
            dataset_id=self.dataset_id,
            rows_loaded=int(rows),
            source=options.source,
            details=details,
        )
