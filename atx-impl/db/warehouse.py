from __future__ import annotations

import datetime as dt
import hashlib
import json
import re
import uuid
from pathlib import Path
from typing import Any, Iterable

import pandas as pd

from .connection import DuckDBStore


OPEN_END_DATE = dt.date(9999, 12, 31)


def now_utc_naive() -> dt.datetime:
    return dt.datetime.now(dt.timezone.utc).replace(tzinfo=None)


def snake_case(value: str) -> str:
    value = re.sub(r"(?<!^)(?=[A-Z])", "_", value)
    value = re.sub(r"[^0-9a-zA-Z]+", "_", value)
    value = re.sub(r"_+", "_", value)
    return value.strip("_").lower()


def symbol_key(symbol: str | None) -> str:
    # NA-safe: pandas may pass pd.NA / NaN for blank ticker cells (the full
    # tbltickerhistory archive carries rows with no ticker in either column).
    # ``pd.NA or ""`` raises "boolean value of NA is ambiguous", so guard first.
    if symbol is None or symbol is pd.NA:
        return ""
    try:
        if pd.isna(symbol):
            return ""
    except (TypeError, ValueError):
        pass
    return str(symbol).strip().upper()


def cik_security_id(cik: str | int) -> str:
    return f"SEC-CIK-{int(cik):010d}"


def vendor_security_id(source: str, value: str | int) -> str:
    cleaned = str(value).strip()
    safe_source = re.sub(r"[^0-9A-Za-z]+", "-", source.strip().upper()).strip("-")
    return f"{safe_source}-{cleaned}"


def security_id_for_symbol(symbol: str) -> str:
    return f"US-TICKER-{symbol_key(symbol)}"


def security_id_for_cusip(cusip: str) -> str:
    cleaned = re.sub(r"[^0-9A-Za-z]+", "", str(cusip).strip()).upper()
    return f"US-CUSIP-{cleaned}"


def json_dumps(payload: Any) -> str:
    return json.dumps(payload, sort_keys=True, default=str)


def file_sha256(path: Path, *, chunk_size: int = 1024 * 1024) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while True:
            chunk = handle.read(chunk_size)
            if not chunk:
                break
            digest.update(chunk)
    return digest.hexdigest()


def record_source_file(
    store: DuckDBStore,
    *,
    dataset_id: str,
    source_url: str,
    cache_path: Path | None = None,
    status: str = "available",
    metadata: dict[str, Any] | None = None,
    sha256: str | None = None,
    compute_hash: bool = False,
) -> str:
    source_id = str(uuid.uuid5(uuid.NAMESPACE_URL, f"{dataset_id}:{source_url}:{cache_path or ''}"))
    byte_count = cache_path.stat().st_size if cache_path and cache_path.exists() else None
    if sha256 is None and compute_hash and cache_path and cache_path.exists():
        sha256 = file_sha256(cache_path)

    store.con.execute("DELETE FROM raw_source_files WHERE source_id = ?", [source_id])
    store.con.execute(
        """
        INSERT INTO raw_source_files (
            source_id,
            dataset_id,
            source_url,
            cache_path,
            sha256,
            byte_count,
            fetched_at,
            status,
            metadata_json
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            source_id,
            dataset_id,
            source_url,
            str(cache_path) if cache_path else None,
            sha256,
            byte_count,
            now_utc_naive(),
            status,
            json_dumps(metadata or {}),
        ],
    )
    return source_id


def register_frame(store: DuckDBStore, name: str, frame: pd.DataFrame) -> None:
    store.con.register(name, frame)


def insert_frame(store: DuckDBStore, frame: pd.DataFrame, table: str, relation_name: str) -> int:
    if frame.empty:
        return 0
    register_frame(store, relation_name, frame)
    try:
        columns = ", ".join(frame.columns)
        store.con.execute(f"INSERT INTO {table} ({columns}) SELECT {columns} FROM {relation_name}")
    finally:
        store.con.unregister(relation_name)
    return int(len(frame))


def replace_by_relation(
    store: DuckDBStore,
    *,
    table: str,
    relation: str,
    key_columns: Iterable[str],
) -> None:
    predicates = " AND ".join(
        f"coalesce(cast(dst.{column} AS VARCHAR), '') = coalesce(cast(src.{column} AS VARCHAR), '')"
        for column in key_columns
    )
    store.con.execute(
        f"""
        DELETE FROM {table} AS dst
        USING {relation} AS src
        WHERE {predicates}
        """
    )


def quality_check(
    store: DuckDBStore,
    *,
    dataset_id: str,
    table_name: str,
    check_name: str,
    status: str,
    observed_value: float | None = None,
    threshold_value: float | None = None,
    details: dict[str, Any] | None = None,
) -> None:
    store.con.execute(
        """
        INSERT INTO data_quality_checks (
            check_id,
            dataset_id,
            table_name,
            check_name,
            status,
            observed_value,
            threshold_value,
            details_json,
            checked_at
        )
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
        """,
        [
            str(uuid.uuid4()),
            dataset_id,
            table_name,
            check_name,
            status,
            observed_value,
            threshold_value,
            json_dumps(details or {}),
            now_utc_naive(),
        ],
    )


def compact_date(value: str | dt.date | None) -> dt.date | None:
    if value is None or value == "":
        return None
    if isinstance(value, dt.date):
        return value
    return dt.date.fromisoformat(str(value))
