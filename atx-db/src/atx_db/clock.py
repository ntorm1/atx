"""The warehouse as-of-date contract.

Determinism rule (see docs/PRODUCTION_RUNBOOK.md "Determinism"): no derived or
ingest path may read the wall clock. A row's ``as_of_date`` is either supplied
explicitly by the operator or derived from the source data itself, so rerunning
the same inputs produces byte-identical rows.

``now_utc_naive()`` in ``atx_db.warehouse`` remains the only sanctioned wall-clock
*timestamp* read, and only for lineage stamps (``source_loaded_at``) that never
feed a signal. ``utc_today()`` here is the only sanctioned wall-clock *date* read,
and only at a CLI or script edge, where it is passed down explicitly.
"""

from __future__ import annotations

import datetime as dt

__all__ = ["resolve_as_of_date", "utc_today"]


def utc_today() -> dt.date:
    """Return today's UTC calendar date.

    Call this ONLY from a CLI/script edge and pass the result down explicitly.
    Library code must take an ``as_of_date`` argument instead.
    """
    return dt.datetime.now(dt.UTC).date()


def resolve_as_of_date(
    explicit: dt.date | None,
    *,
    source_max_date: dt.date | dt.datetime | None = None,
) -> dt.date:
    """Resolve the ``as_of_date`` for a batch of rows without reading the clock.

    Precedence: an operator-supplied ``explicit`` date, then a date derived from
    the source data (``source_max_date`` — e.g. a vendor file's creation time or
    the maximum filing date in the batch). If neither is available the caller is
    asking for a non-reproducible run, which is an error.
    """
    if explicit is not None:
        if isinstance(explicit, dt.datetime):
            return explicit.date()
        return explicit
    if source_max_date is not None:
        if isinstance(source_max_date, dt.datetime):
            return source_max_date.date()
        return source_max_date
    raise ValueError(
        "as_of_date is required: pass an explicit date, or supply source_max_date "
        "derived from the source data. Ingest and derived paths must not read the "
        "wall clock (atx_db.clock.utc_today() is for CLI/script edges only)."
    )
