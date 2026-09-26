from __future__ import annotations

import datetime as dt
from collections.abc import Sequence
from pathlib import Path

import pandas as pd

from ..calendar import decision_cutoff_utc
from ..connection import DEFAULT_DB_PATH, connect


def _month_end(value: dt.date) -> dt.date:
    if value.month == 12:
        return dt.date(value.year, 12, 31)
    return dt.date(value.year, value.month + 1, 1) - dt.timedelta(days=1)


def decision_cutoff(d: dt.date) -> dt.datetime:
    """``d`` 22:00 UTC as a naive UTC timestamp: when a feature dated ``d`` is known.

    Delegates to :func:`atx_db.calendar.decision_cutoff_utc` (docs/methodology/CLOCKS.md).
    Warehouse clocks are naive UTC, so the zone is dropped here: an aware value bound
    to ``CAST(? AS TIMESTAMP)`` would be shifted by the session time zone.
    """
    if isinstance(d, dt.datetime):
        d = d.date()
    return decision_cutoff_utc(d).replace(tzinfo=None)


def _month_end_asof_ts(value: dt.date) -> dt.datetime:
    """The decision cutoff of ``value``'s calendar month end (22:00 UTC, not 23:59:59)."""
    return decision_cutoff(_month_end(value))


def end_of_day_asof_ts(as_of_date: dt.date) -> dt.datetime:
    """Default ``as_of_ts`` of every as-of reader: the 22:00 UTC decision cutoff.

    Kept under its historical name for callers; it no longer means 23:59:59, which
    admitted rows published after the decision (and after every XNYS close).
    """
    return decision_cutoff(as_of_date)


def latest_visible_sql(
    table: str,
    key_cols: Sequence[str],
    cutoff_param: str = "$cutoff",
    order_cols: Sequence[str] = ("available_at", "source_loaded_at", "id"),
    *,
    alias: str = "t",
    joins: str = "",
    predicates: Sequence[str] = (),
) -> str:
    """Newest revision per key visible at ``cutoff_param`` (one whole row per key).

    ``SELECT * EXCLUDE (rn) FROM (SELECT <alias>.*, row_number() OVER (PARTITION BY <key>
    ORDER BY <order> DESC) AS rn FROM <table> <alias> <joins> WHERE <alias>.available_at IS NOT
    NULL AND <alias>.available_at <= <cutoff> [AND <predicates>]) WHERE rn = 1``.

    Visibility is decided before the pick and never by a stored "latest" flag: a flag is
    today's knowledge, so filtering on it drops the original row at every cutoff between
    it and a later restatement. A row with no clock is never visible. The newest visible
    row wins whole, NULL values included (a NULL is never skipped in favour of an older
    value). ``order_cols`` must end in a unique column so ties resolve the same way on
    every run. ``joins`` (e.g. filter joins, ``CROSS JOIN params p``) and ``predicates``
    (extra visibility conditions such as ``t.as_of_date <= p.as_of_date``) are applied
    before ranking, so no filter can promote an older revision by removing a newer one.
    All arguments are package-owned SQL fragments, never user input.
    """
    if not key_cols or not order_cols:
        raise ValueError("latest_visible_sql needs key and order columns")
    key = ", ".join(f"{alias}.{column}" for column in key_cols)
    order = ", ".join(f"{alias}.{column} DESC NULLS LAST" for column in order_cols)
    where = " AND ".join((
        f"{alias}.available_at IS NOT NULL",
        f"{alias}.available_at <= {cutoff_param}",
        *predicates,
    ))
    return (
        "SELECT * EXCLUDE (rn) FROM (\n"
        f"    SELECT {alias}.*, row_number() OVER (PARTITION BY {key} ORDER BY {order}) AS rn\n"
        f"    FROM {table} {alias}\n"
        f"    {joins}\n"
        f"    WHERE {where}\n"
        ") WHERE rn = 1"
    )

def _normalize_symbols(symbols: tuple[str, ...] | list[str] | None) -> list[str]:
    if symbols is None:
        return []
    return sorted({str(symbol).strip().upper() for symbol in symbols if str(symbol).strip()})

def _normalize_strings(values: tuple[str, ...] | list[str] | None) -> list[str]:
    if values is None:
        return []
    return sorted({str(value).strip().upper() for value in values if str(value).strip()})

def _normalize_ids(values: tuple[str, ...] | list[str] | None) -> list[str]:
    """Normalize opaque identifiers (e.g. security_id) for filter joins.

    Unlike _normalize_strings, this does NOT upper-case: security_id is an opaque
    internal key (e.g. 'SEC-CIK-0000320193'), not a categorical code, so changing
    its case would break the join. Matches the convention in entity_classification_asof
    (which registers the raw security_id). Strips whitespace and de-dups only.
    """
    if values is None:
        return []
    return sorted({str(value).strip() for value in values if str(value).strip()})

def _register_filter(store, relation_name: str, column_name: str, values: list[str]) -> bool:
    if not values:
        return False
    store.con.register(relation_name, pd.DataFrame({column_name: values}))
    return True
