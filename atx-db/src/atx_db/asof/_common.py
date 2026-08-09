from __future__ import annotations

import datetime as dt
from pathlib import Path

import pandas as pd

from ..connection import DEFAULT_DB_PATH, connect


def _month_end(value: dt.date) -> dt.date:
    if value.month == 12:
        return dt.date(value.year, 12, 31)
    return dt.date(value.year, value.month + 1, 1) - dt.timedelta(days=1)

def _month_end_asof_ts(value: dt.date) -> dt.datetime:
    return dt.datetime.combine(_month_end(value), dt.time(23, 59, 59, 999999))

def end_of_day_asof_ts(as_of_date: dt.date) -> dt.datetime:
    return dt.datetime.combine(as_of_date, dt.time(23, 59, 59))

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
