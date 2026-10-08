"""NYSE trading sessions generated from rules, independent of how far the vendor file extends.

Rule ``nyse-sessions-v1``: every weekday (Monday to Friday) that is not an NYSE full-day closure. The closures are
the installed ``holidays`` package's NYSE calendar (``holidays.financial_holidays("NYSE")``: the regular holidays
with their observed-day shifts, plus the unscheduled closures it records, e.g. Hurricane Sandy 2012-10-29 / 30 and
the national days of mourning 2018-12-05 and 2025-01-09) and ``EXTRA_CLOSURES`` for any closure the package lacks.
Early closes (13:00 ET) are sessions.

Measured 2026-10-05: the rule sessions equal the vendor ``calendar.parquet`` exactly on its whole extent (3,642
sessions, 2012-03-26 .. 2026-09-18), so ``EXTRA_CLOSURES`` is empty. Every build that uses this calendar asserts the
equality again (:func:`check_against_vendor`) and aborts on a mismatch.

Used by stage ``indexes_v2`` for the R2 ``effective_from`` (the first session after a reconstitution date, which can
lie past the vendor's last session) and the R3 exit stamps.
"""

from __future__ import annotations

import datetime as dt
from typing import Any

RULE = "nyse-sessions-v1"
# Full-day closures missing from the package's NYSE calendar: {date: reason}. None as of holidays 0.102.
EXTRA_CLOSURES: dict[dt.date, str] = {}


def closures(first_year: int, last_year: int) -> dict[dt.date, str]:
    import holidays

    out = dict(holidays.financial_holidays("NYSE", years=range(first_year, last_year + 1)).items())
    out.update(EXTRA_CLOSURES)
    return out


def sessions(first: dt.date, last: dt.date) -> list[dt.date]:
    """NYSE sessions in [first, last], ascending."""
    closed = closures(first.year, last.year)
    days = (first + dt.timedelta(days=i) for i in range((last - first).days + 1))
    return [d for d in days if d.weekday() < 5 and d not in closed]


def check_against_vendor(rule_sessions: list[dt.date], vendor: list[dt.date]) -> dict[str, Any]:
    """Assert the rule sessions equal the vendor sessions on their overlap; raise ValueError naming the differing
    dates otherwise (the build aborts)."""
    lo, hi = max(rule_sessions[0], vendor[0]), min(rule_sessions[-1], vendor[-1])
    if lo > hi:
        raise ValueError(f"exchange calendar: no overlap with the vendor calendar ({vendor[0]} .. {vendor[-1]})")
    rules = {d for d in rule_sessions if lo <= d <= hi}
    seen = {d for d in vendor if lo <= d <= hi}
    if rules != seen:
        raise ValueError(f"exchange calendar {RULE} differs from the vendor calendar on {lo} .. {hi}: "
                         f"vendor only {[str(d) for d in sorted(seen - rules)[:20]]}, "
                         f"rules only {[str(d) for d in sorted(rules - seen)[:20]]}")
    return {"rule": RULE, "overlap": [lo, hi], "sessions": len(seen), "match": True}
