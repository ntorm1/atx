"""Shared imports and helpers for estimate dataset loaders."""
from __future__ import annotations

import datetime as dt
import hashlib
import json
import math
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable, Iterable

import pandas as pd

from ..connection import DuckDBStore
from ..dataset import Dataset, DatasetLoadResult
from ..warehouse import (
    file_sha256,
    insert_frame,
    json_dumps,
    now_utc_naive,
    quality_check,
    record_source_file,
    security_id_for_symbol,
    snake_case,
    symbol_key,
)
from ._columns import IBES_MEASURE_MAP

def _string_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="string")
    return frame[column].replace("", pd.NA).astype("string")


def _numeric_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="Float64")
    return pd.to_numeric(frame[column].replace("", pd.NA), errors="coerce")


def _integer_series(frame: pd.DataFrame, column: str) -> pd.Series:
    numeric = _numeric_series(frame, column)
    return numeric.round().astype("Int64")


def _date_series(
    frame: pd.DataFrame,
    column: str,
    fallback: pd.Series | dt.date | None = None,
) -> pd.Series:
    if column in frame.columns:
        parsed = pd.to_datetime(frame[column].replace("", pd.NA), errors="coerce").dt.date
    else:
        parsed = pd.Series([pd.NA] * len(frame), index=frame.index, dtype="object")
    if fallback is None:
        return parsed
    fallback_series = (
        fallback
        if isinstance(fallback, pd.Series)
        else pd.Series([fallback] * len(frame), index=frame.index, dtype="object")
    )
    return parsed.where(pd.notna(parsed), fallback_series)


def _time_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="object")
    values: list[dt.time | Any] = []
    for value in frame[column].replace("", pd.NA):
        if pd.isna(value):
            values.append(pd.NA)
            continue
        text = str(value).strip()
        try:
            values.append(dt.time.fromisoformat(text))
            continue
        except ValueError:
            pass
        try:
            values.append(dt.datetime.fromisoformat(text).time())
        except ValueError:
            values.append(pd.NA)
    return pd.Series(values, index=frame.index, dtype="object")


def _timestamp_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NaT] * len(frame), index=frame.index, dtype="datetime64[ns]")
    return pd.to_datetime(frame[column].replace("", pd.NA), errors="coerce")


def _timestamp_from_date_time(
    dates: pd.Series,
    times: pd.Series,
    fallback: dt.datetime,
) -> pd.Series:
    values: list[dt.datetime] = []
    for date_value, time_value in zip(dates, times):
        if pd.isna(date_value):
            values.append(fallback)
            continue
        parsed_time = dt.time()
        if not pd.isna(time_value):
            if isinstance(time_value, dt.time):
                parsed_time = time_value
            else:
                try:
                    parsed_time = dt.time.fromisoformat(str(time_value))
                except ValueError:
                    parsed_time = dt.time()
        values.append(dt.datetime.combine(date_value, parsed_time))
    return pd.Series(values, index=dates.index, dtype="datetime64[ns]")


def _timestamp_from_dates_end_of_day(
    dates: pd.Series,
    fallback: dt.datetime,
) -> pd.Series:
    values: list[dt.datetime] = []
    for date_value in dates:
        if pd.isna(date_value):
            values.append(fallback)
        else:
            values.append(dt.datetime.combine(date_value, dt.time(23, 59, 59)))
    return pd.Series(values, index=dates.index, dtype="datetime64[ns]")


def _bool_series(frame: pd.DataFrame, column: str) -> pd.Series:
    if column not in frame.columns:
        return pd.Series([pd.NA] * len(frame), index=frame.index, dtype="boolean")

    def _parse(value: Any) -> bool | pd._libs.missing.NAType:
        if pd.isna(value) or str(value).strip() == "":
            return pd.NA
        normalized = str(value).strip().lower()
        if normalized in {"1", "true", "t", "yes", "y", "gaap", "g"}:
            return True
        if normalized in {"0", "false", "f", "no", "n", "non-gaap", "nongaap", "adjusted"}:
            return False
        return pd.NA

    return frame[column].map(_parse).astype("boolean")


def _series_clean(value: Any) -> str:
    if pd.isna(value):
        return ""
    return str(value).strip()


def _hash_id(prefix: str, *parts: Any) -> str:
    payload = "|".join(_series_clean(part) for part in parts)
    digest = hashlib.sha256(payload.encode("utf-8")).hexdigest()[:16]
    return f"{prefix}-{digest}"


def _safe_provider(value: str) -> str:
    cleaned = re.sub(r"[^0-9A-Z]+", "-", str(value).strip().upper()).strip("-")
    return cleaned or "INJECTED"


def _period_type_from_fpi(value: Any) -> str | None:
    if pd.isna(value) or str(value).strip() == "":
        return None
    code = str(value).strip().upper()
    if code == "0":
        return "LTG"
    if code in {"1", "2", "3", "4", "5"}:
        return "FY"
    if code in {"6", "7", "8", "9"}:
        return "FQ"
    if code in {"A", "B"}:
        return "SEMI"
    if code == "Y":
        return "YTD"
    return None


def _quarter_label(value: Any) -> str | None:
    if pd.isna(value):
        return None
    month = int(value.month)
    if month <= 3:
        return "Q1"
    if month <= 6:
        return "Q2"
    if month <= 9:
        return "Q3"
    return "Q4"


def _canonical_measure(measure: Any, pdf: Any = None) -> str | None:
    if pd.isna(measure) or str(measure).strip() == "":
        return None
    code = str(measure).strip().upper()
    if code == "EPS":
        pdf_code = "" if pd.isna(pdf) else str(pdf).strip().upper()
        return "EPS_BASIC" if pdf_code == "P" else "EPS_DILUTED"
    return IBES_MEASURE_MAP.get(code, code)


def _derive_party_id(
    prefix: str,
    canonical: Any,
    provider: str,
    vendor_id: Any,
    mask_code: Any,
    name: Any,
    valid_from: Any,
) -> str | None:
    if not pd.isna(canonical) and str(canonical).strip():
        return str(canonical).strip()
    basis = vendor_id if not pd.isna(vendor_id) and str(vendor_id).strip() else mask_code
    if pd.isna(basis) or not str(basis).strip():
        basis = name
    if pd.isna(basis) or not str(basis).strip():
        return None
    return _hash_id(prefix, provider, basis, valid_from)


def _raw_payloads(frame: pd.DataFrame) -> pd.Series:
    return frame.apply(lambda row: json_dumps(row.dropna().to_dict()), axis=1)

def _insert_or_replace_frame(
    store: DuckDBStore,
    frame: pd.DataFrame,
    table: str,
    relation_name: str,
) -> int:
    if frame.empty:
        return 0
    store.con.register(relation_name, frame)
    try:
        columns = ", ".join(frame.columns)
        store.con.execute(f"INSERT OR REPLACE INTO {table} ({columns}) SELECT {columns} FROM {relation_name}")
    finally:
        store.con.unregister(relation_name)
    return int(len(frame))

__all__ = [
    "Any",
    "Callable",
    "Iterable",
    "Path",
    "dataclass",
    "dt",
    "hashlib",
    "json",
    "math",
    "pd",
    "re",
    "DuckDBStore",
    "Dataset",
    "DatasetLoadResult",
    "file_sha256",
    "insert_frame",
    "json_dumps",
    "now_utc_naive",
    "quality_check",
    "record_source_file",
    "security_id_for_symbol",
    "snake_case",
    "symbol_key",
    "_string_series",
    "_numeric_series",
    "_integer_series",
    "_date_series",
    "_time_series",
    "_timestamp_series",
    "_timestamp_from_date_time",
    "_timestamp_from_dates_end_of_day",
    "_bool_series",
    "_series_clean",
    "_hash_id",
    "_safe_provider",
    "_period_type_from_fpi",
    "_quarter_label",
    "_canonical_measure",
    "_derive_party_id",
    "_raw_payloads",
    "_insert_or_replace_frame",
]
